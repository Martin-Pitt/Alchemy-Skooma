/**
 * @file skmapscene.cpp
 * @brief The mini-map's hold on the navmesh: its demand while a map draws.
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
#include "skaircells.h"
#include "sknetmap.h"
#include "skworldcensus.h"

#include <cmath>

namespace
{
    constexpr F64 DRAWN_RECENTLY_S = 2.0;
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
    SKWorldCensus::instance();
    SKAirCells::instance();
    ALObjectPropertiesCache::instance();
    LLGroupMgr::instance();
    gIdleCallbacks.addFunction(&SKMapScene::onIdle, nullptr);
}

void SKMapScene::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SKMapScene::onIdle, nullptr);
    updateDemand(false);
}

void SKMapScene::noteDrawn(const SKNetMap&)
{
    mLastDrawn = LLFrameTimer::getUptimeSeconds().value();
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
    if (want == mDemanding || !SKAirCells::instanceExists() || !SKWorldCensus::instanceExists())
    {
        return;
    }
    // The air demand brings the rooms pass over it; the census is asked directly.
    static LLCachedControl<F32> range(gSavedSettings, "SKCensusRange", 512.f);
    if (want)
    {
        SKAirCells::instance().addDemand(DEMAND);
        SKWorldCensus::instance().addDemand(DEMAND, range);
    }
    else
    {
        SKAirCells::instance().removeDemand(DEMAND);
        SKWorldCensus::instance().removeDemand(DEMAND);
    }
    mDemanding = want;
}

void SKMapScene::idle()
{
    const bool want = active() && LLWorld::instanceExists() && gAgent.getRegion();
    updateDemand(want);
}
