/**
 * @file skterraincache.cpp
 * @brief The regions' ground kept on disk per grid, read and written off the main thread.
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

#include "skterraincache.h"

#include "llagent.h"
#include "llcontrol.h"
#include "lldir.h"
#include "lldate.h"
#include "lleventtimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "fsyspath.h"
#include "workqueue.h"

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace SKTerrain;

namespace
{
    constexpr size_t KEPT_REGIONS        = 48;
    constexpr F32    INDEX_WRITE_DELAY   = 5.f;
    constexpr char   INDEX_NAME[]        = "index";
    constexpr char   LIVE_SOURCE[]       = "live";
    constexpr char   SEED_SOURCE[]       = "seed";

    U64 nowSeconds()
    {
        return U64(LLDate::now().secondsSinceEpoch());
    }

    std::optional<std::vector<U8>> readBytes(const std::string& path)
    {
        std::ifstream in(std::filesystem::path(fsyspath(path)), std::ios::binary);
        if (!in)
            return std::nullopt;
        return std::vector<U8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // Written beside, then renamed over, so a reader never sees half a file.
    bool writeAtomic(const std::string& path, const void* data, size_t size)
    {
        const std::filesystem::path target = fsyspath(path);
        const std::filesystem::path temp   = fsyspath(path + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char*>(data), std::streamsize(size));
            if (!out)
                return false;
        }
        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

    void removeFile(const std::string& path)
    {
        std::error_code ec;
        std::filesystem::remove(fsyspath(path), ec);
    }

    std::shared_ptr<SKTerrainCache::Region> regionOf(SKTerrainCell cell, SKTerrainCache::ESource source,
                                                     const SKTerrainCache::Region* base, S32 rebuilt_layer)
    {
        auto region     = std::make_shared<SKTerrainCache::Region>();
        region->mCell   = std::move(cell);
        region->mSource = source;
        for (U32 l = 0; l < LAYER_COUNT; ++l)
        {
            if (base && S32(l) != rebuilt_layer && base->mCell.mWater == region->mCell.mWater)
                region->mPyramids[l] = base->mPyramids[l];
            else if (region->mCell.hasLayer(ELayer(l)))
                region->mPyramids[l] = std::make_shared<const SKTerrainPyramid>(
                    SKTerrainPyramid::build(region->mCell.mLayers[l], region->mCell.mWater));
        }
        return region;
    }

    bool sameGrid(U64 a, U64 b)
    {
        const S64 dx = S64(gridX(a)) - S64(gridX(b));
        const S64 dy = S64(gridY(a)) - S64(gridY(b));
        return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
    }
} // namespace

struct SKTerrainCache::Loaded
{
    SKTerrainManifest                 mManifest;
    bool                              mChanged = false;
    std::unordered_map<U64, SeedCell> mSeed;
};

struct SKTerrainCache::Put
{
    region_ptr                          mRegion;
    std::optional<SKTerrainManifestRow> mRow;
    bool                                mDropRow = false;
};

SKTerrainCache::SKTerrainCache() = default;

SKTerrainCache::~SKTerrainCache()
{
    flush();
}

std::string SKTerrainCache::regionPath(U64 handle) const
{
    return mDir + gDirUtilp->getDirDelimiter() + SKTerrainManifest::fileName(handle);
}

void SKTerrainCache::setGrid(const std::string& grid_id)
{
    if (grid_id == mGrid && mIndexLoaded)
        return;
    flush();

    ++mGeneration;
    mGrid        = grid_id;
    mIndexLoaded = false;
    mDirty       = false;
    mManifest    = SKTerrainManifest();
    mManifest.mGrid = grid_id;
    mRowOf.clear();
    mSeed.clear();
    mKept.clear();
    mReading.clear();
    mBusy.clear();
    mPendingPuts.clear();
    mMapWater.clear();

    const std::string root = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "terrain");
    mDir                   = root + gDirUtilp->getDirDelimiter() + LLDir::getScrubbedFileName(grid_id);
    static LLCachedControl<std::string> seed_dir(gSavedSettings, "SKTerrainCacheSeedDir", "");
    mSeedDir = seed_dir();

    const std::string dir      = mDir;
    const std::string seed     = mSeedDir;
    const U32         generation = mGeneration;
    auto load = [dir, seed, grid_id]() -> std::shared_ptr<Loaded>
    {
        LL_PROFILE_ZONE_NAMED("terrain cache index");
        auto            loaded = std::make_shared<Loaded>();
        std::error_code ec;
        std::filesystem::create_directories(fsyspath(dir), ec);

        const std::optional<std::vector<U8>> text = readBytes(dir + gDirUtilp->getDirDelimiter() + INDEX_NAME);
        std::optional<SKTerrainManifest>     read;
        if (text)
            read = SKTerrainManifest::parse(std::string(text->begin(), text->end()), grid_id);
        loaded->mChanged = !read;
        std::unordered_map<U64, SKTerrainManifestRow> known;
        if (read)
            for (SKTerrainManifestRow& row : read->mRows)
                known[row.mHandle] = std::move(row);

        // The files decide: a row with no file goes, a file with no row or a changed size is read again.
        loaded->mManifest.mGrid = grid_id;
        for (const auto& entry : std::filesystem::directory_iterator(fsyspath(dir), ec))
        {
            const std::string        name   = fsyspath(entry.path().filename()).string();
            const std::optional<U64> handle = SKTerrainManifest::handleFromFileName(name);
            if (!handle)
                continue;
            std::error_code size_ec;
            const U64       size = entry.file_size(size_ec);
            auto            it   = known.find(*handle);
            if (it != known.end() && !size_ec && it->second.mFileSize == size)
            {
                loaded->mManifest.mRows.push_back(std::move(it->second));
                known.erase(it);
                continue;
            }
            loaded->mChanged = true;
            const std::string                    path  = dir + gDirUtilp->getDirDelimiter() + name;
            const std::optional<std::vector<U8>> bytes = readBytes(path);
            SKTerrainCell                        header;
            const SKTerrainCell::EDecode         result = bytes ? SKTerrainCell::peek(bytes->data(), bytes->size(), header)
                                                                : SKTerrainCell::DECODE_CORRUPT;
            if (result != SKTerrainCell::DECODE_OK || header.mHandle != *handle)
            {
                // Another format version, or damaged: it is captured again rather than kept.
                removeFile(path);
                continue;
            }
            SKTerrainManifestRow row = SKTerrainManifestRow::fromHeader(header, bytes->size(),
                                                                        SKTerrainManifest::checksumOf(bytes->data(), bytes->size()));
            if (it != known.end())
            {
                row.mLastSeen = it->second.mLastSeen;
                row.mSource   = it->second.mSource;
                known.erase(it);
            }
            loaded->mManifest.mRows.push_back(std::move(row));
        }
        loaded->mChanged |= !known.empty();

        if (!seed.empty())
        {
            for (U32 l = 0; l < LAYER_COUNT; ++l)
            {
                const std::filesystem::path index = fsyspath(seed) / SKTerrainDataset::layerFolder(ELayer(l)) / "index.json";
                std::ifstream               in(index, std::ios::binary);
                if (!in)
                    continue;
                const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                for (SKTerrainDatasetRow& row : SKTerrainDataset::parseIndex(json))
                {
                    SeedCell& cell     = loaded->mSeed[row.handle()];
                    cell.mRow.mHandle  = row.handle();
                    cell.mRow.mSource  = SEED_SOURCE;
                    if (row.mWater)
                        cell.mRow.mWater = *row.mWater;
                    if (row.mValidSamples && !row.mHeight.empty())
                    {
                        cell.mRow.mLayers |= 1u << l;
                        cell.mLayers[l] = std::move(row);
                    }
                }
            }
        }
        return loaded;
    };

    LL::WorkQueue::ptr_t main_queue    = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue ||
        !main_queue->postTo(general_queue, std::move(load),
                            [generation](std::shared_ptr<Loaded> loaded)
                            {
                                if (SKTerrainCache::instanceExists())
                                    SKTerrainCache::instance().landIndex(generation, std::move(loaded));
                            }))
    {
        LL_WARNS("TerrainCache") << "No queue to read the terrain cache's index on" << LL_ENDL;
    }
}

void SKTerrainCache::landIndex(U32 generation, std::shared_ptr<Loaded> loaded)
{
    if (generation != mGeneration || !loaded)
        return;
    // Puts that landed while the index was read stay; their rows are the newer.
    std::vector<SKTerrainManifestRow> landed = std::move(mManifest.mRows);
    mManifest                                = std::move(loaded->mManifest);
    mRowOf.clear();
    for (size_t i = 0; i < mManifest.mRows.size(); ++i)
        mRowOf[mManifest.mRows[i].mHandle] = i;
    for (const SKTerrainManifestRow& row : landed)
        setRow(row);
    mSeed        = std::move(loaded->mSeed);
    mIndexLoaded = true;
    LL_INFOS("TerrainCache") << "Terrain cache for " << mGrid << ": " << mManifest.mRows.size() << " regions, " << mSeed.size()
                             << " in the seed dataset" << LL_ENDL;
    if (loaded->mChanged || !landed.empty())
        markDirty();
    evict();
}

const SKTerrainManifestRow* SKTerrainCache::lookup(U64 handle) const
{
    if (auto it = mRowOf.find(handle); it != mRowOf.end())
        return &mManifest.mRows[it->second];
    if (auto it = mSeed.find(handle); it != mSeed.end())
        return &it->second.mRow;
    return nullptr;
}

U32 SKTerrainCache::revision(U64 handle, ELayer layer) const
{
    const SKTerrainManifestRow* row = lookup(handle);
    return row ? row->mRevision[layer] : 0;
}

SKTerrainCache::region_ptr SKTerrainCache::get(U64 handle)
{
    for (Kept& kept : mKept)
    {
        if (kept.mHandle == handle)
        {
            kept.mUse = ++mUseClock;
            return kept.mRegion;
        }
    }
    if (!mIndexLoaded || mReading.count(handle) || mBusy.count(handle))
        return nullptr;

    const bool in_cache = mRowOf.count(handle) != 0;
    auto       seed_it  = mSeed.find(handle);
    if (!in_cache && seed_it == mSeed.end())
        return nullptr;

    mReading.insert(handle);
    const std::string path       = regionPath(handle);
    const std::string seed_root  = mSeedDir;
    std::optional<SeedCell> seed;
    if (seed_it != mSeed.end())
        seed = seed_it->second;
    const U32 generation = mGeneration;
    auto read = [path, seed_root, seed, handle, in_cache]() -> std::shared_ptr<Put>
    {
        LL_PROFILE_ZONE_NAMED("terrain cache read");
        auto put = std::make_shared<Put>();
        if (in_cache)
        {
            const std::optional<std::vector<U8>> bytes = readBytes(path);
            SKTerrainCell                        cell;
            const SKTerrainCell::EDecode result = bytes ? cell.decode(bytes->data(), bytes->size()) : SKTerrainCell::DECODE_CORRUPT;
            if (result == SKTerrainCell::DECODE_OK && cell.mHandle == handle)
            {
                put->mRegion = regionOf(std::move(cell), SOURCE_CACHE, nullptr, -1);
                return put;
            }
            removeFile(path);
            put->mDropRow = true;
        }
        if (seed)
        {
            SKTerrainCell cell;
            cell.mHandle = handle;
            cell.mWater  = seed->mRow.mWater;
            for (U32 l = 0; l < LAYER_COUNT; ++l)
            {
                if (!seed->mLayers[l])
                    continue;
                if (std::optional<SKTerrainLayerSamples> samples = SKTerrainDataset::readCell(fsyspath(seed_root), *seed->mLayers[l]))
                    cell.setLayer(ELayer(l), std::move(*samples), 0);
            }
            put->mRegion = regionOf(std::move(cell), SOURCE_SEED, nullptr, -1);
        }
        return put;
    };

    LL::WorkQueue::ptr_t main_queue    = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue ||
        !main_queue->postTo(general_queue, std::move(read),
                            [generation, handle](std::shared_ptr<Put> put)
                            {
                                if (SKTerrainCache::instanceExists())
                                    SKTerrainCache::instance().landRead(generation, handle, std::move(put));
                            }))
    {
        mReading.erase(handle);
    }
    return nullptr;
}

void SKTerrainCache::landRead(U32 generation, U64 handle, std::shared_ptr<Put> read)
{
    if (generation != mGeneration)
        return;
    mReading.erase(handle);
    if (read && read->mDropRow)
    {
        if (auto it = mRowOf.find(handle); it != mRowOf.end())
        {
            mManifest.mRows[it->second] = mManifest.mRows.back();
            mRowOf[mManifest.mRows[it->second].mHandle] = it->second;
            mManifest.mRows.pop_back();
            mRowOf.erase(handle);
            markDirty();
        }
    }
    // A put that started or landed meanwhile is the newer.
    const bool kept = std::any_of(mKept.begin(), mKept.end(), [handle](const Kept& k) { return k.mHandle == handle; });
    if (read && read->mRegion && !kept && !mBusy.count(handle))
        keep(handle, read->mRegion);
    mLoadedSignal(handle);
}

void SKTerrainCache::putLayer(U64 handle, ELayer layer, SKTerrainLayerSamples samples, U64 stamp, const std::string& producer)
{
    if (mGrid.empty() || layer >= LAYER_COUNT || (!samples.empty() && samples.mHeightCm.size() != CELL_SAMPLES))
        return;
    mPendingPuts[handle].push_back(PendingPut{ layer, std::move(samples), stamp, producer, {}, {} });
    if (!mBusy.count(handle))
        startPut(handle);
}

void SKTerrainCache::putCapture(U64 handle, ELayer layer, SKTerrainLayerSamples samples, F32 water, std::string sidecar,
                                U64 stamp, const std::string& producer)
{
    if (mGrid.empty() || layer >= LAYER_COUNT || !std::isfinite(water) || (!samples.empty() && samples.mHeightCm.size() != CELL_SAMPLES))
        return;
    mPendingPuts[handle].push_back(PendingPut{ layer, std::move(samples), stamp, producer, water, std::move(sidecar) });
    if (!mBusy.count(handle))
        startPut(handle);
}

void SKTerrainCache::setMapWater(U64 handle, F32 water)
{
    if (std::isfinite(water))
        mMapWater[handle] = water;
}

std::optional<F32> SKTerrainCache::mapWater(U64 handle) const
{
    if (auto it = mMapWater.find(handle); it != mMapWater.end())
        return it->second;
    return std::nullopt;
}

void SKTerrainCache::startPut(U64 handle)
{
    auto pending = mPendingPuts.find(handle);
    if (pending == mPendingPuts.end() || pending->second.empty())
        return;
    // Puts queued behind one another are applied together: the last of each layer wins.
    std::vector<PendingPut> puts = std::move(pending->second);
    mPendingPuts.erase(pending);
    mBusy.insert(handle);

    region_ptr base;
    for (const Kept& kept : mKept)
        if (kept.mHandle == handle && kept.mRegion->mSource == SOURCE_CACHE)
            base = kept.mRegion;
    const SKTerrainManifestRow* row      = mRowOf.count(handle) ? &mManifest.mRows[mRowOf[handle]] : nullptr;
    const bool                  on_disk  = row != nullptr;
    F32 water = 20.f;
    if (row)
        water = row->mWater;
    else if (mSeed.count(handle))
        water = mSeed[handle].mRow.mWater;
    else if (auto it = mMapWater.find(handle); it != mMapWater.end())
        water = it->second;
    const std::string           path     = regionPath(handle);
    const U32                   generation = mGeneration;

    auto apply = [path, handle, base, on_disk, water, puts = std::move(puts)]() mutable -> std::shared_ptr<Put>
    {
        LL_PROFILE_ZONE_NAMED("terrain cache put");
        auto          put = std::make_shared<Put>();
        SKTerrainCell cell;
        bool          have = false;
        if (base)
        {
            cell = base->mCell;
            have = true;
        }
        else if (on_disk)
        {
            const std::optional<std::vector<U8>> bytes = readBytes(path);
            have = bytes && cell.decode(bytes->data(), bytes->size()) == SKTerrainCell::DECODE_OK && cell.mHandle == handle;
        }
        if (!have)
        {
            cell        = SKTerrainCell();
            cell.mHandle = handle;
            cell.mWater  = water;
        }

        S32 rebuilt = -1;
        for (PendingPut& p : puts)
        {
            if (p.mWater)
                cell.mWater = *p.mWater;
            if (p.mSidecar)
                cell.mSidecar = std::move(*p.mSidecar);
            cell.setLayer(p.mLayer, std::move(p.mSamples), p.mStamp);
            if (p.mLayer == LAYER_TERRAIN)
                cell.mCaptureDate = std::max(cell.mCaptureDate, p.mStamp);
            rebuilt = (rebuilt == -1 || rebuilt == S32(p.mLayer)) ? S32(p.mLayer) : S32(LAYER_COUNT);
        }
        const std::vector<U8> bytes = cell.encode();
        if (bytes.empty() || !writeAtomic(path, bytes.data(), bytes.size()))
            return put;

        put->mRow = SKTerrainManifestRow::fromHeader(cell, bytes.size(), SKTerrainManifest::checksumOf(bytes.data(), bytes.size()));
        put->mRow->mSource = puts.back().mProducer;
        // Only the layers put are built again; the pyramids of the rest are the base's.
        put->mRegion = regionOf(std::move(cell), SOURCE_CACHE, rebuilt < S32(LAYER_COUNT) ? base.get() : nullptr, rebuilt);
        return put;
    };

    LL::WorkQueue::ptr_t main_queue    = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue ||
        !main_queue->postTo(general_queue, std::move(apply),
                            [generation, handle](std::shared_ptr<Put> put)
                            {
                                if (SKTerrainCache::instanceExists())
                                    SKTerrainCache::instance().landPut(generation, handle, std::move(put));
                            }))
    {
        mBusy.erase(handle);
    }
}

void SKTerrainCache::landPut(U32 generation, U64 handle, std::shared_ptr<Put> put)
{
    if (generation != mGeneration)
        return;
    mBusy.erase(handle);
    if (put && put->mRow && put->mRegion)
    {
        SKTerrainManifestRow row = *put->mRow;
        if (const SKTerrainManifestRow* old = mRowOf.count(handle) ? &mManifest.mRows[mRowOf[handle]] : nullptr)
            row.mLastSeen = old->mLastSeen;
        if (row.mSource == LIVE_SOURCE)
            row.mLastSeen = nowSeconds();
        setRow(row);
        keep(handle, put->mRegion);
        markDirty();
        mLoadedSignal(handle);
    }
    else
    {
        LL_WARNS("TerrainCache") << "Could not write the terrain of region " << gridX(handle) << "," << gridY(handle) << LL_ENDL;
    }
    startPut(handle);
    evict();
}

void SKTerrainCache::markSeen(U64 handle)
{
    if (auto it = mRowOf.find(handle); it != mRowOf.end())
    {
        mManifest.mRows[it->second].mLastSeen = nowSeconds();
        markDirty();
    }
}

void SKTerrainCache::setRow(const SKTerrainManifestRow& row)
{
    if (auto it = mRowOf.find(row.mHandle); it != mRowOf.end())
    {
        mManifest.mRows[it->second] = row;
        return;
    }
    mRowOf[row.mHandle] = mManifest.mRows.size();
    mManifest.mRows.push_back(row);
}

void SKTerrainCache::keep(U64 handle, const region_ptr& region)
{
    for (Kept& kept : mKept)
    {
        if (kept.mHandle == handle)
        {
            kept.mRegion = region;
            kept.mUse    = ++mUseClock;
            return;
        }
    }
    if (mKept.size() >= KEPT_REGIONS)
    {
        auto oldest = std::min_element(mKept.begin(), mKept.end(), [](const Kept& a, const Kept& b) { return a.mUse < b.mUse; });
        *oldest     = mKept.back();
        mKept.pop_back();
    }
    mKept.push_back(Kept{ handle, region, ++mUseClock });
}

void SKTerrainCache::forget(U64 handle)
{
    mKept.erase(std::remove_if(mKept.begin(), mKept.end(), [handle](const Kept& k) { return k.mHandle == handle; }), mKept.end());
}

void SKTerrainCache::markDirty()
{
    mDirty = true;
    writeIndex(false);
}

void SKTerrainCache::writeIndex(bool now)
{
    if (!mDirty || !mIndexLoaded || mDir.empty())
        return;
    if (!now)
    {
        // Debounced: a burst of puts writes the index once.
        if (!mWriteScheduled)
        {
            mWriteScheduled = true;
            LLEventTimer::run_after(INDEX_WRITE_DELAY,
                                    []()
                                    {
                                        if (SKTerrainCache::instanceExists())
                                        {
                                            SKTerrainCache::instance().mWriteScheduled = false;
                                            SKTerrainCache::instance().writeIndex(true);
                                        }
                                    });
        }
        return;
    }

    mDirty                  = false;
    const std::string text  = mManifest.format();
    const std::string path  = mDir + gDirUtilp->getDirDelimiter() + INDEX_NAME;
    auto              write = [text, path]()
    {
        if (!writeAtomic(path, text.data(), text.size()))
            LL_WARNS("TerrainCache") << "Could not write the terrain cache's index " << path << LL_ENDL;
    };
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!general_queue || !general_queue->post(std::move(write)))
        write();
}

void SKTerrainCache::flush()
{
    if (!mDirty || !mIndexLoaded || mDir.empty())
        return;
    mDirty = false;
    const std::string text = mManifest.format();
    writeAtomic(mDir + gDirUtilp->getDirDelimiter() + INDEX_NAME, text.data(), text.size());
}

void SKTerrainCache::evict()
{
    static LLCachedControl<U32> max_mb(gSavedSettings, "SKTerrainCacheMaxMB", 2048);
    const U64 budget = U64(max_mb()) << 20;
    U64       total  = 0;
    for (const SKTerrainManifestRow& row : mManifest.mRows)
        total += row.mFileSize;
    if (total <= budget)
        return;

    // Never the region the agent is in or those around it.
    const LLViewerRegion* agent_region = gAgent.getRegion();
    const U64             here         = agent_region ? agent_region->getHandle() : 0;
    std::vector<size_t>   order(mManifest.mRows.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [this](size_t a, size_t b) { return mManifest.mRows[a].mLastSeen < mManifest.mRows[b].mLastSeen; });

    std::vector<U64> evicted;
    for (size_t i : order)
    {
        if (total <= budget)
            break;
        const SKTerrainManifestRow& row = mManifest.mRows[i];
        if ((here && sameGrid(row.mHandle, here)) || mBusy.count(row.mHandle) || mPendingPuts.count(row.mHandle))
            continue;
        total -= row.mFileSize;
        evicted.push_back(row.mHandle);
    }
    if (evicted.empty())
        return;

    std::vector<std::string> paths;
    for (U64 handle : evicted)
    {
        paths.push_back(regionPath(handle));
        forget(handle);
        mRowOf.erase(handle);
    }
    mManifest.mRows.erase(std::remove_if(mManifest.mRows.begin(), mManifest.mRows.end(),
                                         [this](const SKTerrainManifestRow& row) { return !mRowOf.count(row.mHandle); }),
                          mManifest.mRows.end());
    mRowOf.clear();
    for (size_t i = 0; i < mManifest.mRows.size(); ++i)
        mRowOf[mManifest.mRows[i].mHandle] = i;

    auto remove = [paths]()
    {
        for (const std::string& path : paths)
            removeFile(path);
    };
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!general_queue || !general_queue->post(std::move(remove)))
        remove();
    markDirty();
}
