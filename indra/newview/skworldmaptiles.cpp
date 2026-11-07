/**
 * @file skworldmaptiles.cpp
 * @brief The 3D world map's tiles: the map server's images by level, fetched nearest first under a
 *        cap, and the per-region surface lookup a cached impostor can later answer instead.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skworldmaptiles.h"

#include "llviewertexture.h"
#include "llworldmipmap.h"

#include <algorithm>

using namespace skworldmap;

void SKWorldMapTiles::Entry::raise()
{
    if (!mBoosted)
    {
        mPriorBoost = mImage->getBoostLevel();
        mImage->setBoostLevel(llmax(mPriorBoost, (S32)LLGLTexture::BOOST_MAP_VISIBLE));
        mBoosted = true;
    }
}

void SKWorldMapTiles::Entry::lower()
{
    if (mBoosted)
    {
        mImage->setBoostLevel(mPriorBoost);
        mBoosted = false;
    }
}

bool SKWorldMapTiles::isReady(const Entry& e) const
{
    return e.mImage.notNull() && !e.mMissing && e.mImage->hasGLTexture();
}

SKWorldMapTiles::Entry* SKWorldMapTiles::find(const TileKey& key)
{
    auto it = mTiles.find(key);
    return it == mTiles.end() ? nullptr : &it->second;
}

void SKWorldMapTiles::use(Entry& e, const TileKey& cover, const F32 uv[4], F64 now, std::vector<Draw>& out)
{
    // A stand-in is kept loaded and fresh as long as it stands in, or it would leave before the tile it covers for.
    e.mLastUsed = now;
    e.raise();
    Draw& d = out.emplace_back();
    d.mTexture = e.mImage.get();
    d.mCover = cover;
    std::copy(uv, uv + 4, d.mUV);
}

void SKWorldMapTiles::addDescendants(const TileKey& key, S32 depth, F64 now, std::vector<Draw>& out)
{
    if (depth <= 0 || key.mLevel <= 1)
    {
        return;
    }
    static constexpr F32 FULL[4] = { 0.f, 0.f, 1.f, 1.f };
    const S32 half = key.regionsPerSide() / 2;
    for (S32 j = 0; j < 2; ++j)
    {
        for (S32 i = 0; i < 2; ++i)
        {
            TileKey child;
            child.mLevel = key.mLevel - 1;
            child.mGridX = key.mGridX + i * half;
            child.mGridY = key.mGridY + j * half;
            Entry* e = find(child);
            if (e && isReady(*e))
            {
                use(*e, child, FULL, now, out);
            }
            else if (!e || !e->mMissing)
            {
                addDescendants(child, depth - 1, now, out);
            }
        }
    }
}

bool SKWorldMapTiles::resolve(const SelectedTile& tile, F64 now, std::vector<Draw>& out)
{
    mWanted.push_back(tile);
    for (S32 level : UNDERLAY_LEVELS)
    {
        if (level > tile.mKey.mLevel)
        {
            F32 uv[4];
            const TileKey key = ancestor(tile.mKey, level - tile.mKey.mLevel, uv);
            auto [it, fresh] = mUnderlay.try_emplace(key, tile.mDistance);
            if (!fresh)
            {
                it->second = std::min(it->second, tile.mDistance);
            }
        }
    }
    const size_t start = out.size();
    if (Entry* e = find(tile.mKey))
    {
        if (e->mMissing)
        {
            // Nothing there: open water or no region. The background shows through.
            return false;
        }
        if (isReady(*e))
        {
            static constexpr F32 FULL[4] = { 0.f, 0.f, 1.f, 1.f };
            use(*e, tile.mKey, FULL, now, out);
            return true;
        }
    }
    // Zooming in: the coarser tile it shows a part of lies underneath.
    for (S32 up = 1; tile.mKey.mLevel + up <= TILE_LEVELS; ++up)
    {
        F32 uv[4];
        const TileKey key = ancestor(tile.mKey, up, uv);
        Entry* e = find(key);
        if (e && isReady(*e))
        {
            use(*e, tile.mKey, uv, now, out);
            break;
        }
    }
    // Zooming out: the finer tiles it is made of are drawn over that.
    addDescendants(tile.mKey, FALLBACK_LEVELS_DOWN, now, out);
    return out.size() > start;
}

void SKWorldMapTiles::update(F64 now)
{
    // Settle what is in flight: arrived, missing, or given up on.
    mInFlight = 0;
    for (auto it = mTiles.begin(); it != mTiles.end();)
    {
        Entry& e = it->second;
        if (e.mImage.notNull() && !e.mMissing && e.mImage->isMissingAsset())
        {
            e.mMissing = true;
        }
        if (e.mImage.notNull() && (e.mMissing || e.mImage->hasGLTexture()) && !mKnown.count(it->first))
        {
            mKnown[it->first] = !e.mMissing;
            mKnownChanged = true;
        }
        if (e.mImage.notNull() && !e.mMissing && !e.mImage->hasGLTexture())
        {
            if (now - e.mRequested >= FETCH_TIMEOUT)
            {
                // Given up on: forgotten, so a later frame that still wants it asks again under the cap.
                if (e.mBoosted)
                {
                    e.lower();
                }
                it = mTiles.erase(it);
                continue;
            }
            ++mInFlight;
        }
        ++it;
    }

    // Start the nearest wanted tiles first, up to the cap, the underlay ahead of them: coarsest level
    // first, so the whole view has something before any of it has detail.
    std::sort(mWanted.begin(), mWanted.end(), [](const SelectedTile& a, const SelectedTile& b) { return a.mDistance < b.mDistance; });
    std::vector<SelectedTile> underlay;
    underlay.reserve(mUnderlay.size());
    for (const auto& [key, distance] : mUnderlay)
    {
        SelectedTile& u = underlay.emplace_back();
        u.mKey = key;
        u.mDistance = distance;
    }
    std::sort(underlay.begin(), underlay.end(), [](const SelectedTile& a, const SelectedTile& b)
              { return a.mKey.mLevel != b.mKey.mLevel ? a.mKey.mLevel > b.mKey.mLevel : a.mDistance < b.mDistance; });
    mWanted.insert(mWanted.begin(), underlay.begin(), underlay.end());
    mUnderlay.clear();
    for (const SelectedTile& t : mWanted)
    {
        Entry* e = find(t.mKey);
        if (e)
        {
            e->mLastUsed = now;
            if (e->mImage.notNull() && !e->mMissing && !e->mBoosted && !e->mImage->hasGLTexture())
            {
                e->raise();
            }
            continue;
        }
        if (mInFlight >= MAX_IN_FLIGHT)
        {
            continue;
        }
        Entry& fresh = mTiles[t.mKey];
        fresh.mImage = LLWorldMipmap::loadObjectsTile(t.mKey.mGridX, t.mKey.mGridY, t.mKey.mLevel);
        fresh.raise();
        fresh.mRequested = now;
        fresh.mLastUsed = now;
        fresh.mMissing = false;
        ++mInFlight;
    }
    mWanted.clear();

    // Tiles that left the view fall back to the ordinary priority once; past the budget the oldest go.
    for (auto& [key, e] : mTiles)
    {
        if (e.mBoosted && e.mLastUsed < now)
        {
            e.lower();
        }
    }
    if (mTiles.size() > static_cast<size_t>(MAX_KEPT))
    {
        std::vector<std::pair<F64, TileKey>> by_age;
        by_age.reserve(mTiles.size());
        for (const auto& [key, e] : mTiles)
        {
            by_age.emplace_back(e.mLastUsed, key);
        }
        const size_t drop = mTiles.size() - MAX_KEPT;
        std::nth_element(by_age.begin(), by_age.begin() + drop, by_age.end());
        for (size_t i = 0; i < drop; ++i)
        {
            Entry* e = find(by_age[i].second);
            if (e && e->mBoosted)
            {
                e->lower();
            }
            mTiles.erase(by_age[i].second);
        }
    }
}

void SKWorldMapTiles::dropPriorities()
{
    for (auto& [key, e] : mTiles)
    {
        if (e.mBoosted)
        {
            e.lower();
        }
    }
}

void SKWorldMapTiles::cover(const TileKey& key, std::vector<std::pair<F64, F64>>& corners) const
{
    bool refined = false;
    if (key.mLevel > 1)
    {
        const S32 half = key.regionsPerSide() / 2;
        for (S32 i = 0; i < 4 && !refined; ++i)
        {
            const TileKey child{ key.mLevel - 1, key.mGridX + (i & 1) * half, key.mGridY + (i >> 1) * half };
            refined = mKnown.count(child) > 0;
        }
        if (refined)
        {
            for (S32 i = 0; i < 4; ++i)
            {
                const TileKey child{ key.mLevel - 1, key.mGridX + (i & 1) * half, key.mGridY + (i >> 1) * half };
                auto it = mKnown.find(child);
                if (it == mKnown.end() || it->second)
                {
                    cover(child, corners);
                }
            }
        }
    }
    if (!refined)
    {
        const F64 x0 = key.mGridX * REGION_METRES, y0 = key.mGridY * REGION_METRES, side = key.sideMetres();
        corners.emplace_back(x0, y0);
        corners.emplace_back(x0 + side, y0);
        corners.emplace_back(x0, y0 + side);
        corners.emplace_back(x0 + side, y0 + side);
    }
}

bool SKWorldMapTiles::occupiedDisc(WorldDisc& disc)
{
    if (mKnownChanged)
    {
        mKnownChanged = false;
        std::vector<std::pair<F64, F64>> corners;
        for (const auto& [key, holds] : mKnown)
        {
            if (key.mLevel == TILE_LEVELS && holds)
            {
                cover(key, corners);
            }
        }
        if (!corners.empty())
        {
            // The smallest disc round every occupied tile's corners holds every region in them.
            mDisc = enclosingDisc(std::move(corners));
            mDisc.mRadius += EDGE_MARGIN_REGIONS * REGION_METRES;
            mHasDisc = true;
        }
    }
    disc = mDisc;
    return mHasDisc;
}

void SKWorldMapTiles::clear()
{
    mTiles.clear();
    mWanted.clear();
    mUnderlay.clear();
    mInFlight = 0;
}
