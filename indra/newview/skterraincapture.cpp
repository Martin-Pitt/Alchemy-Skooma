/**
 * @file skterraincapture.cpp
 * @brief Copies the live regions' ground into the terrain cache as it arrives.
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

#include "skterraincapture.h"

#include "skterraincache.h"

#include "llcontrol.h"
#include "lleventtimer.h"
#include "llgltfmaterial.h"
#include "llsdserialize.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvlcomposition.h"
#include "llworld.h"

#include <ctime>

namespace
{
    constexpr char LIVE_PRODUCER[] = "live";

    // The region's terrain look, as far as the composition holds it without fetching anything.
    std::string sidecarOf(LLViewerRegion* regionp)
    {
        LLVLComposition* comp = regionp->getComposition();
        if (!comp)
            return {};
        LLSD look;
        look["type"]       = comp->getMaterialType() == LLTerrainMaterials::Type::PBR ? "pbr" : "texture";
        look["paint_type"] = LLSD::Integer(comp->getPaintType());
        for (S32 i = 0; i < LLVLComposition::ASSET_COUNT; ++i)
        {
            look["assets"][i]   = comp->getDetailAssetID(i);
            look["overrides"][i] = comp->getMaterialOverride(i) != nullptr;
        }
        for (S32 c = 0; c < LLVLComposition::CORNER_COUNT; ++c)
        {
            look["start"][c] = comp->getStartHeight(c);
            look["range"][c] = comp->getHeightRange(c);
        }
        std::ostringstream out;
        LLSDSerialize::toNotation(look, out);
        return out.str();
    }

    U64 updatesOf(const LLSurface& land)
    {
        U64       sum   = 0;
        const S32 edge  = land.getPatchesPerEdge();
        for (S32 y = 0; y < edge; ++y)
            for (S32 x = 0; x < edge; ++x)
                if (const LLSurfacePatch* patch = land.getPatch(x, y))
                    sum += patch->getUpdateCount();
        return sum;
    }
} // namespace

SKTerrainCapture::SKTerrainCapture() = default;

SKTerrainCapture::~SKTerrainCapture()
{
    delete mTimer;
}

void SKTerrainCapture::start()
{
    if (mTimer)
        return;
    mTimer = LLEventTimer::run_every(1.f, [this]() { tick(); });
    mRemovedConnection = LLWorld::getInstance()->setRegionRemovedCallback([this](LLViewerRegion* regionp) { onRegionRemoved(regionp); });
}

SKTerrainCapture::Watched& SKTerrainCapture::watch(U64 handle)
{
    for (Watched& w : mWatched)
        if (w.mHandle == handle)
            return w;
    Watched& w = mWatched.emplace_back();
    w.mHandle  = handle;
    return w;
}

void SKTerrainCapture::tick()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKTerrainCacheCapture", true);
    static LLCachedControl<F32>  settle(gSavedSettings, "SKTerrainCacheCaptureSettle", 2.f);
    if (!enabled || SKTerrainCache::instance().grid().empty())
        return;
    LL_PROFILE_ZONE_SCOPED;
    const F64 now = LLTimer::getTotalSeconds();
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionp->getLand().hasZData())
            continue;
        Watched&  w       = watch(regionp->getHandle());
        const U64 updates = updatesOf(regionp->getLand());
        if (updates != w.mUpdates)
        {
            w.mUpdates   = updates;
            w.mChangedAt = now;
        }
        else if (w.mUpdates != w.mCaptured && now - w.mChangedAt >= F64(settle))
        {
            capture(regionp, w);
        }
    }
}

void SKTerrainCapture::capture(LLViewerRegion* regionp, Watched& watched)
{
    using namespace SKTerrain;
    watched.mCaptured = watched.mUpdates;
    const LLSurface& land = regionp->getLand();
    // The cell is 256 m at a metre a sample; a larger region is not captured.
    if (land.getGridsPerEdge() - 1 != S32(CELL_SIZE) || land.getMetersPerGrid() != 1.f)
        return;

    LL_PROFILE_ZONE_SCOPED;
    const S32             patch_edge   = land.getGridsPerPatchEdge();
    const S32             patches      = land.getPatchesPerEdge();
    std::vector<F32>      heights(CELL_SAMPLES);
    std::vector<U8>       valid(CELL_SAMPLES, 0);
    for (S32 py = 0; py < patches; ++py)
    {
        for (S32 px = 0; px < patches; ++px)
        {
            const LLSurfacePatch* patch = land.getPatch(px, py);
            if (!patch || !patch->getHasReceivedData())
                continue;
            for (S32 j = py * patch_edge; j < (py + 1) * patch_edge && j < S32(CELL_SIZE); ++j)
                for (S32 i = px * patch_edge; i < (px + 1) * patch_edge && i < S32(CELL_SIZE); ++i)
                {
                    heights[j * CELL_SIZE + i] = land.getZ(i, j);
                    valid[j * CELL_SIZE + i]   = 1;
                }
        }
    }
    SKTerrainCache::instance().putCapture(regionp->getHandle(), LAYER_TERRAIN,
                                          SKTerrainLayerSamples::fromMetres(heights.data(), valid.data()),
                                          land.getWaterHeight(), sidecarOf(regionp), U64(std::time(nullptr)), LIVE_PRODUCER);
    SKTerrainCache::instance().markSeen(regionp->getHandle());
}

void SKTerrainCapture::onRegionRemoved(LLViewerRegion* regionp)
{
    if (!regionp)
        return;
    const U64 handle = regionp->getHandle();
    for (auto it = mWatched.begin(); it != mWatched.end(); ++it)
    {
        if (it->mHandle != handle)
            continue;
        if (!SKTerrainCache::instance().grid().empty() && regionp->getLand().hasZData())
        {
            it->mUpdates = updatesOf(regionp->getLand());
            if (it->mUpdates != it->mCaptured)
                capture(regionp, *it);
        }
        mWatched.erase(it);
        return;
    }
}
