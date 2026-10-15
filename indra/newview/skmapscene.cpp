/**
 * @file skmapscene.cpp
 * @brief The mini-map's hold on the navmesh: its demand while a map draws, and which tiles it has built.
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

#include "skmapscene.h"

#include "alobjectproperties.h"
#include "llagent.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llgroupmgr.h"
#include "llviewercontrol.h"
#include "llworld.h"
#include "skmapview.h"
#include "sknavmesh.h"
#include "sknetmap.h"
#include "skworldcensus.h"

#include <cmath>

namespace
{
    constexpr F64 DRAWN_RECENTLY_S = 2.0;
    constexpr F64 BUILT_EVERY_S = 0.25;
    constexpr F64 CARRY_TILE_S = 10.0;     ///< a tile being rebuilt is unpublished for a moment
    constexpr F64 LATTICE_M = 32.0;        ///< the navmesh's tile size
    const char* DEMAND = "sknetmap";
}

SKMapScene::SKMapScene()
{
}

SKMapScene::~SKMapScene()
{
}

void SKMapScene::initSingleton()
{
    // Taking these here makes them outlive us at teardown.
    SKNavMesh::instance();
    ALObjectPropertiesCache::instance();
    LLGroupMgr::instance();
    gIdleCallbacks.addFunction(&SKMapScene::onIdle, nullptr);
}

void SKMapScene::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SKMapScene::onIdle, nullptr);
    updateDemand(false);
    mBuilt.clear();
}

void SKMapScene::noteDrawn(const SKNetMap& map)
{
    mLastDrawn = LLFrameTimer::getUptimeSeconds().value();
    mMapCentre = map.view().centre();
    mMapLong = map.view().longSide();
    mMapShort = map.view().shortSide();
}

bool SKMapScene::active() const
{
    return LLFrameTimer::getUptimeSeconds().value() - mLastDrawn < DRAWN_RECENTLY_S;
}

// static
void SKMapScene::onIdle(void*)
{
    if (instanceExists())
    {
        getInstance()->idle();
    }
}

void SKMapScene::updateDemand(bool want)
{
    if (want == mDemanding || !SKNavMesh::instanceExists())
    {
        return;
    }
    // The demand also brings the census and the rooms pass, out to SKNavRange.
    if (want)
    {
        SKNavMesh::instance().addDemand(DEMAND);
    }
    else
    {
        SKNavMesh::instance().removeDemand(DEMAND);
    }
    mDemanding = want;
}

void SKMapScene::idle()
{
    const bool want = active() && LLWorld::instanceExists() && gAgent.getRegion();
    updateDemand(want);
    if (want)
    {
        updateBuilt();
    }
}

void SKMapScene::updateBuilt()
{
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    LLVector3d min, max;
    skmap::analysisBox(gAgent.getPositionGlobal(), mMapCentre, mMapLong, mMapShort, min, max);
    const SKNavMesh& navmesh = SKNavMesh::instance();
    const bool moved = navmesh.revision() != mBuiltRevision || min != mBuiltMin || max != mBuiltMax;
    if (!moved && now - mBuiltAt < CARRY_TILE_S)
    {
        return;
    }
    if (moved && now - mBuiltAt < BUILT_EVERY_S)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    mBuiltRevision = navmesh.revision();
    mBuiltMin = min;
    mBuiltMax = max;
    mBuiltAt = now;
    // Tiles sit on the lattice: a published tile's footprint is exactly its cell.
    navmesh.polysIn(min, max, sknav::AgentClass::AVATAR, [this, now](const SKNavMesh::PolyTile& tile) {
        mBuilt[{ static_cast<S32>(std::floor((tile.mMin.mdV[VX] + 1.0) / LATTICE_M)),
                 static_cast<S32>(std::floor((tile.mMin.mdV[VY] + 1.0) / LATTICE_M)) }] = now;
    });
    for (auto it = mBuilt.begin(); it != mBuilt.end();)
    {
        it = now - it->second > CARRY_TILE_S ? mBuilt.erase(it) : std::next(it);
    }
}
