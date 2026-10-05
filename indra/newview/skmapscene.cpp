/**
 * @file skmapscene.cpp
 * @brief The mini-map floorplan: navmesh demand, the scene worker, cell bakes and their draw.
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
#include "llgroupmgr.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "sknavmesh.h"
#include "sknavrooms.h"
#include "sknetmap.h"
#include "skworldcensus.h"
#include "workqueue.h"

#include <chrono>

namespace
{
    constexpr F64 DRAWN_RECENTLY_S = 2.0;
    constexpr F64 POST_EVERY_S = 0.25;
    constexpr F64 POST_BUILDING_S = 1.0;   ///< while the navmesh still has tiles queued
    const char* QUEUE_NAME = "SKNetMap";
    const char* DEMAND = "sknetmap";
}

SKMapScene::SKMapScene()
    : mAlive(std::make_shared<bool>(true))
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
    // Results still queued for the main loop see the token gone and drop.
    mAlive.reset();
    if (mPool)
    {
        mPool->close();
        mPool.reset();
    }
    updateDemand(false);
    mScene.reset();
}

void SKMapScene::noteDrawn(const SKNetMap& map)
{
    mLastDrawn = LLFrameTimer::getElapsedSeconds();
    mMapCentre = map.view().centre();
    mMapLong = map.view().longSide();
    mMapShort = map.view().shortSide();
}

bool SKMapScene::active() const
{
    return LLFrameTimer::getElapsedSeconds() - mLastDrawn < DRAWN_RECENTLY_S;
}

bool SKMapScene::built(S32 x, S32 y) const
{
    return mScene && mScene->mBuilt.count({ x, y }) > 0;
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
    if (!want)
    {
        return;
    }
    maybePost();
}

SKMapScene::Trigger SKMapScene::currentTrigger() const
{
    Trigger t;
    skmap::analysisBox(gAgent.getPositionGlobal(), mMapCentre, mMapLong, mMapShort, t.mMin, t.mMax);
    const SKNavMesh& navmesh = SKNavMesh::instance();
    t.mNavRevision = navmesh.revision();
    if (const std::shared_ptr<const SKNavWorldView> view = navmesh.view())
    {
        t.mViewRevision = view->revision();
    }
    if (SKWorldCensus::instanceExists())
    {
        if (const SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
        {
            t.mCensus = snap->mStamp;
        }
    }
    U64 regions = sknav::SEED;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        regions ^= sknav::mix(sknav::SEED, region->getHandle());
    }
    t.mRegions = regions;
    return t;
}

void SKMapScene::maybePost()
{
    if (mInFlight)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const SKNavMesh::Stats& nav = SKNavMesh::instance().stats();
    const F64 every = (nav.mQueued + nav.mInFlight) > 0 ? POST_BUILDING_S : POST_EVERY_S;
    if (now - mLastPost < every)
    {
        return;
    }
    const Trigger trigger = currentTrigger();
    if (mHasPosted && trigger == mPosted)
    {
        return;
    }

    if (!mPool)
    {
        mPool = std::make_unique<LL::ThreadPool>(QUEUE_NAME, 1);
        mPool->start();
    }
    const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
    if (!queue)
    {
        return;
    }
    auto input = std::make_shared<skmap::SceneInput>();
    input->mMin = trigger.mMin;
    input->mMax = trigger.mMax;
    input->mOrigin.set(trigger.mMin.mdV[VX], trigger.mMin.mdV[VY], 0.0);
    gather(*input);

    const U32 generation = ++mGeneration;
    std::weak_ptr<bool> alive = mAlive;
    std::shared_ptr<const skmap::SceneInput> job = input;
    const bool posted = queue->post([this, job, generation, alive]() {
        const auto start = std::chrono::steady_clock::now();
        skmap::ScenePtr scene = skmap::buildScene(*job);
        const F32 millis = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - start).count();
        LL::WorkQueue::postMaybe(LL::WorkQueue::getInstance("mainloop"), [this, scene, generation, millis, alive]() {
            if (!alive.expired())
            {
                land(scene, generation, millis);
            }
        });
    });
    if (posted)
    {
        mInFlight = true;
        mLastPost = now;
        mPosted = trigger;
        mHasPosted = true;
    }
}

void SKMapScene::gather(skmap::SceneInput& in) const
{
    LL_PROFILE_ZONE_SCOPED;
    const SKNavMesh& navmesh = SKNavMesh::instance();
    navmesh.polysIn(in.mMin, in.mMax, sknav::AgentClass::AVATAR, [&in](const SKNavMesh::PolyTile& tile) {
        skmap::TileInput t;
        t.mMesh = tile.mMesh;
        t.mTerrain = tile.mTerrain;
        t.mRegion = tile.mRegion;
        t.mOrigin = tile.mOrigin;
        t.mKey = tile.mKey;
        t.mContentKey = tile.mContentKey;
        t.mMin = tile.mMin;
        t.mMax = tile.mMax;
        t.mPolys = tile.mPolys;
        t.mSegments = tile.mSegments;
        t.mLinks = tile.mLinks;
        t.mFlights = tile.mFlights;
        if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(tile.mRegion))
        {
            t.mWater = region->getWaterHeight();
        }
        in.mTiles.push_back(t);
    });
    navmesh.linksIn(in.mMin, in.mMax, sknav::AgentClass::AVATAR,
                    [&in](const LLVector3d& from, const LLVector3d& to, F32, sknav::LinkKind kind) {
                        if (kind == sknav::LinkKind::LADDER || kind == sknav::LinkKind::ELEVATOR)
                        {
                            in.mFeatureLinks.push_back({ from, to, kind });
                        }
                    });
    navmesh.doorsIn(in.mMin, in.mMax, [&in](const sknav::DoorEntity& door) {
        skmap::DoorInput d;
        d.mMin = door.mMin;
        d.mMax = door.mMax;
        d.mPlanePoint = door.mPlanePoint;
        d.mPlaneKnown = door.mPlaneKnown;
        d.mNormal = door.mNormal;
        d.mWidth = door.mWidth;
        d.mSill = door.mSill;
        d.mDouble = door.mDouble;
        in.mDoors.push_back(d);
    });
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        in.mRegions.push_back({ region->getOriginGlobal(), static_cast<F64>(region->getWidth()) });
    }
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    in.mClimb = climb;
    // The enclosure view is immutable once published, so the worker may read it.
    const std::shared_ptr<const SKNavWorldView> view = navmesh.view();
    in.mCoverage = [view](const LLVector3d& pos) {
        sknav::Enclosure e;
        return view && view->enclosureAt(pos, e) ? e.mBucket : sknav::EnclosureBucket::UNKNOWN;
    };
}

void SKMapScene::land(const skmap::ScenePtr& scene, U32 generation, F32 millis)
{
    mInFlight = false;
    if (generation != mGeneration || !scene)
    {
        return;     // superseded
    }
    mScene = scene;
    mStats.mJobs++;
    mStats.mJobMs = millis;
    mStats.mPolys = static_cast<U32>(scene->mPolys.size());
    mStats.mLayers = static_cast<U32>(scene->mOrder.size());
}
