/**
 * @file skworldmaptiles.cpp
 * @brief The 3D world map's tiles: the map server's images by level, fetched nearest first under a
 *        cap, and the per-region surface lookup a cached impostor can later answer instead.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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

bool SKWorldMapTiles::isReady(const Entry& e) const
{
    return e.mImage.notNull() && !e.mMissing && e.mImage->hasGLTexture();
}

SKWorldMapTiles::Entry* SKWorldMapTiles::find(const TileKey& key)
{
    auto it = mTiles.find(key);
    return it == mTiles.end() ? nullptr : &it->second;
}

bool SKWorldMapTiles::resolve(const SelectedTile& tile, F64 now, Draw& out)
{
    mWanted.push_back(tile);
    for (S32 up = 0; tile.mKey.mLevel + up <= TILE_LEVELS; ++up)
    {
        F32 uv[4] = { 0.f, 0.f, 1.f, 1.f };
        const TileKey key = up == 0 ? tile.mKey : ancestor(tile.mKey, up, uv);
        Entry* e = find(key);
        if (!e)
        {
            continue;
        }
        if (up == 0 && e->mMissing)
        {
            // Nothing there: open water or no region. The background shows through.
            return false;
        }
        if (isReady(*e))
        {
            e->mLastUsed = now;
            e->mImage->setBoostLevel(LLGLTexture::BOOST_MAP_VISIBLE);
            e->mBoosted = true;
            out.mTexture = e->mImage.get();
            std::copy(uv, uv + 4, out.mUV);
            return true;
        }
    }
    return false;
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
        if (e.mImage.notNull() && !e.mMissing && !e.mImage->hasGLTexture())
        {
            if (now - e.mRequested >= FETCH_TIMEOUT)
            {
                // Given up on: forgotten, so a later frame that still wants it asks again under the cap.
                if (e.mBoosted)
                {
                    e.mImage->setBoostLevel(LLGLTexture::BOOST_NONE);
                }
                it = mTiles.erase(it);
                continue;
            }
            ++mInFlight;
        }
        ++it;
    }

    // Start the nearest wanted tiles first, up to the cap.
    std::sort(mWanted.begin(), mWanted.end(), [](const SelectedTile& a, const SelectedTile& b) { return a.mDistance < b.mDistance; });
    for (const SelectedTile& t : mWanted)
    {
        Entry* e = find(t.mKey);
        if (e)
        {
            e->mLastUsed = now;
            if (e->mImage.notNull() && !e->mMissing && !e->mBoosted && !e->mImage->hasGLTexture())
            {
                e->mImage->setBoostLevel(LLGLTexture::BOOST_MAP_VISIBLE);
                e->mBoosted = true;
            }
            continue;
        }
        if (mInFlight >= MAX_IN_FLIGHT)
        {
            continue;
        }
        Entry& fresh = mTiles[t.mKey];
        fresh.mImage = LLWorldMipmap::loadObjectsTile(t.mKey.mGridX, t.mKey.mGridY, t.mKey.mLevel);
        fresh.mImage->setBoostLevel(LLGLTexture::BOOST_MAP_VISIBLE);
        fresh.mRequested = now;
        fresh.mLastUsed = now;
        fresh.mMissing = false;
        fresh.mBoosted = true;
        ++mInFlight;
    }
    mWanted.clear();

    // Tiles that left the view fall back to the ordinary priority once; past the budget the oldest go.
    for (auto& [key, e] : mTiles)
    {
        if (e.mBoosted && e.mLastUsed < now)
        {
            e.mImage->setBoostLevel(LLGLTexture::BOOST_NONE);
            e.mBoosted = false;
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
                e->mImage->setBoostLevel(LLGLTexture::BOOST_NONE);
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
            e.mImage->setBoostLevel(LLGLTexture::BOOST_NONE);
            e.mBoosted = false;
        }
    }
}

void SKWorldMapTiles::clear()
{
    mTiles.clear();
    mWanted.clear();
    mInFlight = 0;
}
