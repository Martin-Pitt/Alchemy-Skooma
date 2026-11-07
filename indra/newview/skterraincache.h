/**
 * @file skterraincache.h
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

#ifndef SK_SKTERRAINCACHE_H
#define SK_SKTERRAINCACHE_H

#include "llsingleton.h"
#include "skterraincell.h"
#include "skterraindataset.h"
#include "skterrainmanifest.h"

#include <boost/signals2.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

// Each grid has a directory under the cache: a file per region and an index of them. Files are read,
// decoded, encoded and written on the General queue; what they hold reaches the main thread decoded,
// where the recently used regions are kept. Nothing here blocks the main thread on the disk.
// A region is looked up in the cache, then in the dataset SKTerrainCacheSeedDir names, which is only read.
class SKTerrainCache : public LLSingleton<SKTerrainCache>
{
    LLSINGLETON(SKTerrainCache);
    ~SKTerrainCache() override;

public:
    enum ESource : U8
    {
        SOURCE_CACHE,
        SOURCE_SEED,
    };

    // A region decoded, never changed once handed out: a put makes a new one.
    struct Region
    {
        SKTerrainCell mCell;
        std::array<std::shared_ptr<const SKTerrainPyramid>, SKTerrain::LAYER_COUNT> mPyramids;
        ESource mSource = SOURCE_CACHE;
    };
    using region_ptr = std::shared_ptr<const Region>;

    // Opens the grid's directory and loads its index, rebuilding it from the files where it is missing,
    // damaged or out of date. Regions of the grid before are let go.
    void setGrid(const std::string& grid_id);
    const std::string& grid() const { return mGrid; }

    // The index: what the cache or the seed dataset knows of a region, without opening it.
    const SKTerrainManifestRow* lookup(U64 handle) const;
    U32 revision(U64 handle, SKTerrain::ELayer layer) const;

    // The region decoded, or nothing yet: a read is started, and the loaded signal names the handle
    // once it lands, or once it is known there is nothing to read.
    region_ptr get(U64 handle);

    // Replaces one layer of a region. Only that layer's revision moves and only its pyramid is built
    // again; the samples carry their own validity. Puts to one region are applied in order.
    void putLayer(U64 handle, SKTerrain::ELayer layer, SKTerrainLayerSamples samples, U64 stamp, const std::string& producer);

    // The region was seen live now; the last seen are the last to be evicted.
    void markSeen(U64 handle);

    using loaded_signal_t = boost::signals2::signal<void(U64 handle)>;
    boost::signals2::connection setLoadedCallback(const loaded_signal_t::slot_type& cb) { return mLoadedSignal.connect(cb); }

    // Writes the index now if it has changed, as the viewer quits.
    void flush();

private:
    struct PendingPut
    {
        SKTerrain::ELayer     mLayer;
        SKTerrainLayerSamples mSamples;
        U64                   mStamp;
        std::string           mProducer;
    };
    struct Kept
    {
        U64        mHandle;
        region_ptr mRegion;
        U64        mUse;
    };
    struct Loaded;
    struct Put;

    std::string regionPath(U64 handle) const;
    void keep(U64 handle, const region_ptr& region);
    void forget(U64 handle);
    void startPut(U64 handle);
    void landIndex(U32 generation, std::shared_ptr<Loaded> loaded);
    void landRead(U32 generation, U64 handle, std::shared_ptr<Put> read);
    void landPut(U32 generation, U64 handle, std::shared_ptr<Put> put);
    void setRow(const SKTerrainManifestRow& row);
    void markDirty();
    void writeIndex(bool now);
    void evict();

    std::string mGrid;
    std::string mDir;
    U32         mGeneration = 0;
    bool        mIndexLoaded = false;
    bool        mDirty       = false;

    SKTerrainManifest                       mManifest;
    std::unordered_map<U64, size_t>         mRowOf; // handle -> index into mManifest.mRows
    struct SeedCell
    {
        SKTerrainManifestRow                                                  mRow;
        std::array<std::optional<SKTerrainDatasetRow>, SKTerrain::LAYER_COUNT> mLayers;
    };
    std::unordered_map<U64, SeedCell>       mSeed;
    std::string                             mSeedDir;

    std::vector<Kept>                                     mKept; // the decoded, least recently used first out
    U64                                                   mUseClock = 0;
    std::unordered_set<U64>                               mReading;
    std::unordered_set<U64>                               mBusy; // a put is being applied
    std::unordered_map<U64, std::vector<PendingPut>>      mPendingPuts;

    bool            mWriteScheduled = false;
    loaded_signal_t mLoadedSignal;
};

#endif // SK_SKTERRAINCACHE_H
