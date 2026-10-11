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
#include "sknavlayout.h"
#include "sknavmesh.h"
#include "sknavrooms.h"
#include "sknetmap.h"
#include "skworldcensus.h"
#include "workqueue.h"

#include "llvoavatarself.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <chrono>
#include <cstring>

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
    mLastTiles.clear();
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
    // The people's floor relations read this.
    if (mScene)
    {
        updateAgentFloor();
    }
}

LL::WorkQueue::ptr_t SKMapScene::queue()
{
    if (!mPool)
    {
        // One thread: scenes and bakes run one at a time and never hold up anything else.
        mPool = std::make_unique<LL::ThreadPool>(QUEUE_NAME, 1);
        mPool->start();
    }
    return LL::WorkQueue::getInstance(QUEUE_NAME);
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
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
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

    const LL::WorkQueue::ptr_t work = queue();
    if (!work)
    {
        return;
    }
    auto input = std::make_shared<skmap::SceneInput>();
    input->mMin = trigger.mMin;
    input->mMax = trigger.mMax;
    input->mOrigin = skmap::analysisOrigin(trigger.mMin, mHasPosted ? &mPostedOrigin : nullptr);
    gather(*input);

    const U32 generation = ++mGeneration;
    std::weak_ptr<bool> alive = mAlive;
    std::shared_ptr<const skmap::SceneInput> job = input;
    const bool posted = work->post([this, job, generation, alive]() mutable {
        const auto start = std::chrono::steady_clock::now();
        skmap::ScenePtr scene = skmap::buildScene(*job);
        job.reset();
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
        mPostedOrigin = input->mOrigin;
    }
}

void SKMapScene::gather(skmap::SceneInput& in)
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
    // A tile being rebuilt is unpublished for a moment; carry its last copy so the floorplan
    // does not drop to object boxes for that moment.
    constexpr F64 CARRY_TILE_S = 10.0;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const size_t published = in.mTiles.size();
    size_t carried = 0;
    std::set<std::pair<LLUUID, sknav::TileKey>> seen;
    for (const skmap::TileInput& t : in.mTiles)
    {
        seen.insert({ t.mMesh, t.mKey });
        mLastTiles[{ t.mMesh, t.mKey }] = { t, now };
    }
    for (auto it = mLastTiles.begin(); it != mLastTiles.end();)
    {
        if (seen.count(it->first))
        {
            ++it;
            continue;
        }
        if (now - it->second.second > CARRY_TILE_S)
        {
            it = mLastTiles.erase(it);
            continue;
        }
        const skmap::TileInput& t = it->second.first;
        if (t.mMax.mdV[VX] > in.mMin.mdV[VX] && t.mMin.mdV[VX] < in.mMax.mdV[VX] && t.mMax.mdV[VY] > in.mMin.mdV[VY]
            && t.mMin.mdV[VY] < in.mMax.mdV[VY])
        {
            in.mTiles.push_back(t);
            ++carried;
        }
        ++it;
    }
    if (carried > 0)
    {
        LL_INFOS("SKMap") << "scene input: " << published << " tiles published, " << carried << " carried while unpublished" << LL_ENDL;
    }
    navmesh.linksIn(in.mMin, in.mMax, sknav::AgentClass::AVATAR,
                    [&in](const LLVector3d& from, const LLVector3d& to, F32, sknav::LinkKind kind) {
                        if (kind == sknav::LinkKind::LADDER || kind == sknav::LinkKind::ELEVATOR)
                        {
                            in.mFeatureLinks.push_back({ from, to, kind });
                        }
                    });
    auto addDoor = [&in](const sknav::DoorEntity& door) {
        skmap::DoorInput d;
        d.mMin = door.mMin;
        d.mMax = door.mMax;
        d.mPlanePoint = door.mPlanePoint;
        d.mPlaneKnown = door.mPlaneKnown;
        d.mNormal = door.mNormal;
        d.mWidth = door.mWidth;
        d.mSill = door.mSill;
        d.mDouble = door.mDouble;
        d.mLeaves = door.mLeaves;
        in.mDoors.push_back(d);
    };
    navmesh.doorsIn(in.mMin, in.mMax, addDoor);
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
    if (mScene && scene->mBuilt.size() < mScene->mBuilt.size())
    {
        LL_INFOS("SKMap") << "scene landed with " << scene->mBuilt.size() << " built cells, down from " << mScene->mBuilt.size() << LL_ENDL;
    }
    mScene = scene;
    mStats.mJobs++;
    mStats.mJobMs = millis;
    mStats.mPolys = static_cast<U32>(scene->mPolys.size());
    mStats.mLayers = static_cast<U32>(scene->mOrder.size());
}

void SKMapScene::updateAgentFloor()
{
    const skmap::Scene& scene = *mScene;
    const LLVector3d agent = gAgent.getPositionGlobal();
    skmap::AgentFloor floor;
    floor.mPos = agent;
    floor.mFloorPos = agent;
    // A jump lifts the agent clear of the usual reach: look further down unless flying.
    const F32 below = gAgent.getFlying() ? 2.5f : 8.f;
    skmap::FloorHit hit = skmap::floorAt(scene, agent, below, 0.5f);
    // The navmesh is eroded by the agent's radius, so against a wall the agent stands just off it:
    // look a little way around before calling the agent off every floor.
    for (F32 r : { 0.4f, 0.8f })
    {
        for (S32 k = 0; k < 8 && !hit.mFound; ++k)
        {
            const F32 a = F_TWO_PI * k / 8.f;
            const LLVector3d at = agent + LLVector3d(std::cos(a) * r, std::sin(a) * r, 0.0);
            hit = skmap::floorAt(scene, at, below, 0.5f);
            if (hit.mFound)
            {
                floor.mFloorPos = at;
            }
        }
    }
    if (hit.mFound)
    {
        floor.mFound = true;
        floor.mLayer = hit.mLayer;
        floor.mZ = hit.mZ;
    }
    else
    {
        // The one navmesh read per frame, for an agent off the scene's floors.
        F32 z = 0.f;
        if (SKNavMesh::instance().floorUnder(agent, sknav::AgentClass::AVATAR, z) && std::fabs(z - agent.mdV[VZ]) < 4.0)
        {
            floor.mFound = true;
            floor.mZ = z;
        }
    }
    mAgent = floor;
    mAgentFrame = LLFrameTimer::getFrameCount();
}
