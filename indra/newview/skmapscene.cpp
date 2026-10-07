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

#include "llimage.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llvoavatarself.h"
#include "llviewertexture.h"
#include "skmappalette.h"

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
    constexpr F32 BAKE_MB = 0.25f;         ///< one 256 x 256 RGBA8 cell
    constexpr S32 FINER_FALLBACK_TIERS = 2; ///< how many finer tiers stand in for a cell not yet baked
    constexpr F32 LAST_SHOWN_SLOT_M = 2.f;   ///< floors this close in height share a cell's last-shown bake

    F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / llmax(e1 - e0, 1e-6f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
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
    mBakes.clear();
    mLastShown.clear();
    mUploads.clear();
    mWanted.clear();
    mRibbons.clear();
    mRibbonLatest.clear();
    mShownRibbons.clear();
    mCarrySources.clear();
    mCarried.clear();
    mHatch = nullptr;
    mBoundary = RibbonBuffer();
    mRibbonUploads.clear();
    mRibbonWanted.clear();
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

    const LL::WorkQueue::ptr_t work = queue();
    if (!work)
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
    const bool posted = work->post([this, job, generation, alive]() {
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
    const F64 now = LLFrameTimer::getElapsedSeconds();
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
    // The layers that drew lines hand them on: layer indices do not survive a new scene, footprints do.
    mCarrySources.clear();
    if (mScene && !mShownRibbons.empty())
    {
        for (const auto& shown : mShownRibbons)
        {
            const skmap::SceneLayer& layer = mScene->mLayers[shown.first];
            CarrySource source;
            source.mGround = layer.mGround;
            source.mBaseZ = layer.mBaseZ;
            source.mKey = shown.second.first;
            source.mCut = shown.second.second;
            constexpr size_t SAMPLE = 64;
            const size_t stride = llmax(static_cast<size_t>(1), layer.mFootprint.size() / SAMPLE);
            size_t i = 0;
            for (const auto& cell : layer.mFootprint)
            {
                if (i++ % stride == 0 && source.mSample.size() < SAMPLE)
                {
                    source.mSample.push_back(cell);
                }
            }
            mCarrySources.push_back(std::move(source));
        }
    }
    mShownRibbons.clear();
    mCarried.clear();
    if (mScene && scene->mBuilt.size() < mScene->mBuilt.size())
    {
        LL_INFOS("SKMap") << "scene landed with " << scene->mBuilt.size() << " built cells, down from " << mScene->mBuilt.size() << LL_ENDL;
    }
    mScene = scene;
    // Global, so a box step that keeps the edges keeps their buffer.
    U64 boundaries = sknav::SEED;
    auto cm = [](F64 metres) { return static_cast<U64>(std::llround(metres * 100.0)); };
    for (const skmap::LineMark& mark : scene->mBoundaries)
    {
        boundaries = sknav::mix(boundaries, static_cast<U64>(mark.mKind));
        for (const LLVector2& p : { mark.mA, mark.mB })
        {
            boundaries = sknav::mix(boundaries, cm(scene->mOrigin.mdV[VX] + p.mV[VX]));
            boundaries = sknav::mix(boundaries, cm(scene->mOrigin.mdV[VY] + p.mV[VY]));
        }
    }
    mBoundaryHash = boundaries;
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

void SKMapScene::updateSelection()
{
    const skmap::Scene& scene = *mScene;
    const skmap::AgentFloor& floor = mAgent;
    const LLVector3d agent = floor.mPos;
    const std::pair<S32, S32> cell = skmap::indexCell(agent.mdV[VX], agent.mdV[VY]);
    // Inside or out, from the enclosure a metre over the agent.
    sknav::EnclosureBucket bucket = sknav::EnclosureBucket::UNKNOWN;
    if (const std::shared_ptr<const SKNavWorldView> view = SKNavMesh::instance().view())
    {
        sknav::Enclosure e;
        if (view->enclosureAt(agent + LLVector3d(0.0, 0.0, 1.0), e))
        {
            bucket = e.mBucket;
        }
    }
    // The selection also reads the agent's height and the fade: flying straight up keeps the cell and finds no floor.
    static LLCachedControl<F32> fade(gSavedSettings, "SKMapVerticalFade", 30.f);
    const F32 za = floor.mFound ? floor.mZ : static_cast<F32>(agent.mdV[VZ]);
    const S32 z_step = ll_round(za * 2.f);
    if (mSelectedScene.lock() == mScene && mSelectedCell == cell && mSelectedLayer == floor.mLayer && mSelectedBucket == bucket
        && mSelectedZ == z_step && mSelectedFade == fade)
    {
        return;
    }
    mSelection = skmap::selectLayers(scene, floor, bucket, fade);
    mSelectedZ = z_step;
    mSelectedFade = fade;
    mSelectedScene = mScene;
    mSelectedCell = cell;
    mSelectedLayer = floor.mLayer;
    mSelectedBucket = bucket;
    mCarried.clear();       // carried lines match the cut, which may have moved
    // The cut's bounding box per layer, in global metres, so most cells skip the cell test.
    mCutBoxes.clear();
    mCutHashes.clear();
    for (const auto& entry : mSelection.mCut)
    {
        if (entry.second.empty())
        {
            continue;
        }
        U64 hash = sknav::SEED;
        for (const auto& c : entry.second)
        {
            hash = sknav::mix(sknav::mix(hash, static_cast<U32>(c.first)), static_cast<U32>(c.second));
        }
        mCutHashes[entry.first] = hash;
        LLVector3d lo(DBL_MAX, DBL_MAX, 0.0), hi(-DBL_MAX, -DBL_MAX, 0.0);
        for (const auto& c : entry.second)
        {
            lo.mdV[VX] = llmin(lo.mdV[VX], c.first * (F64)skmap::INDEX_CELL_M);
            lo.mdV[VY] = llmin(lo.mdV[VY], c.second * (F64)skmap::INDEX_CELL_M);
            hi.mdV[VX] = llmax(hi.mdV[VX], (c.first + 1) * (F64)skmap::INDEX_CELL_M);
            hi.mdV[VY] = llmax(hi.mdV[VY], (c.second + 1) * (F64)skmap::INDEX_CELL_M);
        }
        mCutBoxes[entry.first] = { lo, hi };
    }
}

U64 SKMapScene::bakeKey(S32 layer, const skmap::CellId& cell) const
{
    const U64 content = skmap::cellKey(*mScene, layer, cell);
    if (!content)
    {
        return 0;
    }
    U64 key = sknav::mix(content, static_cast<U64>(cell.mTier));
    key = sknav::mix(key, (mRasterParams.mStairs ? 1u : 0u) | (mRasterParams.mContours ? 2u : 0u) | (mRasterParams.mHeatmap ? 4u : 0u));
    key = sknav::mix(key, skmap::RASTER_VERSION);
    return key ? key : 1;
}

void SKMapScene::requestBake(S32 layer, const skmap::CellId& cell, U64 key, F32 priority)
{
    mWanted.push_back({ layer, cell, key, priority });
}

void SKMapScene::postBake()
{
    mStats.mQueued = static_cast<U32>(mWanted.size());
    if (mBakeInFlight || mWanted.empty() || !mScene)
    {
        mWanted.clear();
        return;
    }
    // Nearest the map centre first.
    const BakeRequest* best = nullptr;
    for (const BakeRequest& r : mWanted)
    {
        bool uploading = false;
        for (const BakeResult& u : mUploads)
        {
            uploading = uploading || u.mKey == r.mKey;
        }
        if (!uploading && (!best || r.mPriority < best->mPriority))
        {
            best = &r;
        }
    }
    const LL::WorkQueue::ptr_t work = best ? queue() : nullptr;
    if (work)
    {
        const skmap::ScenePtr scene = mScene;
        const BakeRequest request = *best;
        const skmap::RasterParams params = mRasterParams;
        std::weak_ptr<bool> alive = mAlive;
        const bool posted = work->post([this, scene, request, params, alive]() {
            auto result = std::make_shared<BakeResult>();
            result->mKey = request.mKey;
            skmap::rasterCell(*scene, request.mLayer, request.mCell, params, result->mPixels);
            LL::WorkQueue::postMaybe(LL::WorkQueue::getInstance("mainloop"), [this, result, alive]() {
                if (!alive.expired())
                {
                    mBakeInFlight = false;
                    mUploads.push_back(std::move(*result));
                }
            });
        });
        if (posted)
        {
            mBakeInFlight = true;
            mBakeKeyInFlight = request.mKey;
        }
    }
    mWanted.clear();
}

void SKMapScene::uploadBakes()
{
    static LLCachedControl<U32> per_frame(gSavedSettings, "SKMapBakeUploadsPerFrame", 2);
    mStats.mUploads = 0;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    while (!mUploads.empty() && mStats.mUploads < per_frame)
    {
        BakeResult& r = mUploads.front();
        if (r.mPixels.size() == static_cast<size_t>(skmap::CELL_TEXELS) * skmap::CELL_TEXELS * 4)
        {
            LLPointer<LLImageRaw> raw = new LLImageRaw(skmap::CELL_TEXELS, skmap::CELL_TEXELS, 4);
            std::memcpy(raw->getData(), r.mPixels.data(), r.mPixels.size());
            Bake& bake = mBakes[r.mKey];
            bake.mTexture = LLViewerTextureManager::getLocalTexture(raw.get(), false);
            bake.mLastDrawn = now;
            mStats.mUploads++;
            mStats.mBakes++;
        }
        mUploads.pop_front();
    }
}

void SKMapScene::evictBakes()
{
    static LLCachedControl<U32> budget_mb(gSavedSettings, "SKMapBakeBudgetMB", 96);
    const size_t cap = llmax(static_cast<size_t>(16), static_cast<size_t>(budget_mb / BAKE_MB));
    while (mBakes.size() > cap)
    {
        // The least recently drawn goes first.
        auto oldest = mBakes.begin();
        for (auto it = mBakes.begin(); it != mBakes.end(); ++it)
        {
            if (it->second.mLastDrawn < oldest->second.mLastDrawn)
            {
                oldest = it;
            }
        }
        mBakes.erase(oldest);
    }
    for (auto it = mLastShown.begin(); it != mLastShown.end();)
    {
        it = mBakes.count(it->second) ? std::next(it) : mLastShown.erase(it);
    }
    mStats.mCacheMB = mBakes.size() * BAKE_MB;
}

void SKMapScene::drawCell(const skmap::MapView& view, LLViewerTexture* texture, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0,
                          F32 u1, F32 v1, const LLColor4& tint) const
{
    gGL.getTextureSlot(0)->bindSampled(texture, ALSamplers::BilinearClamp);
    drawCellQuads(view, min, max, u0, v0, u1, v1, tint);
}

void SKMapScene::drawCellQuads(const skmap::MapView& view, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0, F32 u1, F32 v1,
                               const LLColor4& tint) const
{
    // 2 x 2 sub-quads, so the distance dim varies across a cell. It fades from the map centre, the camera.
    const LLVector3d agent = view.centre();
    const F32 span = view.span();
    LLVector2 px[3][3];
    LLColor4 col[3][3];
    F32 u[3], v[3];
    for (S32 j = 0; j < 3; ++j)
    {
        for (S32 i = 0; i < 3; ++i)
        {
            const LLVector3d p(min.mdV[VX] + (max.mdV[VX] - min.mdV[VX]) * i * 0.5, min.mdV[VY] + (max.mdV[VY] - min.mdV[VY]) * j * 0.5, 0.0);
            px[j][i] = view.toWidget(p);
            const F32 d = static_cast<F32>(std::hypot(p.mdV[VX] - agent.mdV[VX], p.mdV[VY] - agent.mdV[VY]));
            const F32 dim = 1.f - skmap::palette::DIM_AMOUNT * smoothstep(skmap::palette::DIM_NEAR * span, skmap::palette::DIM_FAR * span, d);
            col[j][i] = LLColor4(tint.mV[0], tint.mV[1], tint.mV[2], tint.mV[3] * dim);
        }
        u[j] = u0 + (u1 - u0) * j * 0.5f;
        v[j] = v0 + (v1 - v0) * j * 0.5f;
    }
    gGL.begin(LLRender::TRIANGLES);
    auto vert = [&](S32 j, S32 i) {
        gGL.color4fv(col[j][i].mV);
        gGL.texCoord2f(u[i], v[j]);
        gGL.vertex2f(px[j][i].mV[VX], px[j][i].mV[VY]);
    };
    for (S32 j = 0; j < 2; ++j)
    {
        for (S32 i = 0; i < 2; ++i)
        {
            vert(j, i);
            vert(j, i + 1);
            vert(j + 1, i + 1);
            vert(j, i);
            vert(j + 1, i + 1);
            vert(j + 1, i);
        }
    }
    gGL.end();
}

void SKMapScene::drawCellCut(const skmap::MapView& view, LLViewerTexture* texture, const LLVector3d& min, const LLVector3d& max, F32 u0,
                             F32 v0, F32 u1, F32 v1, const LLColor4& tint, S32 layer) const
{
    const auto box = mCutBoxes.find(layer);
    if (box == mCutBoxes.end() || box->second.second.mdV[VX] <= min.mdV[VX] || box->second.first.mdV[VX] >= max.mdV[VX]
        || box->second.second.mdV[VY] <= min.mdV[VY] || box->second.first.mdV[VY] >= max.mdV[VY])
    {
        drawCell(view, texture, min, max, u0, v0, u1, v1, tint);
        return;
    }
    // A part of the cell, its UVs mapped from the cell's; one bind for all of them, as a bind flushes.
    gGL.getTextureSlot(0)->bindSampled(texture, ALSamplers::BilinearClamp);
    auto part = [&](F64 x0, F64 y0, F64 x1, F64 y1) {
        if (x1 <= x0 || y1 <= y0)
        {
            return;
        }
        auto u = [&](F64 x) { return u0 + (u1 - u0) * static_cast<F32>((x - min.mdV[VX]) / (max.mdV[VX] - min.mdV[VX])); };
        auto v = [&](F64 y) { return v0 + (v1 - v0) * static_cast<F32>((y - min.mdV[VY]) / (max.mdV[VY] - min.mdV[VY])); };
        drawCellQuads(view, LLVector3d(x0, y0, 0.0), LLVector3d(x1, y1, 0.0), u(x0), v(y0), u(x1), v(y1), tint);
    };
    // Up to four rectangles round the cut's box, then 4 m pieces inside it that are not cut.
    const F64 bx0 = llmax(box->second.first.mdV[VX], min.mdV[VX]), by0 = llmax(box->second.first.mdV[VY], min.mdV[VY]);
    const F64 bx1 = llmin(box->second.second.mdV[VX], max.mdV[VX]), by1 = llmin(box->second.second.mdV[VY], max.mdV[VY]);
    part(min.mdV[VX], min.mdV[VY], max.mdV[VX], by0);
    part(min.mdV[VX], by1, max.mdV[VX], max.mdV[VY]);
    part(min.mdV[VX], by0, bx0, by1);
    part(bx1, by0, max.mdV[VX], by1);
    const auto& cut = mSelection.mCut.at(layer);
    const F64 step = skmap::INDEX_CELL_M;
    for (F64 y = by0; y < by1 - 1e-6; y += step)
    {
        for (F64 x = bx0; x < bx1 - 1e-6; x += step)
        {
            const auto c = skmap::indexCell(x + 0.5 * step, y + 0.5 * step);
            if (!cut.count(c))
            {
                part(x, y, llmin(x + step, bx1), llmin(y + step, by1));
            }
        }
    }
}

void SKMapScene::drawFinerBakes(const skmap::MapView& view, S32 layer, const skmap::CellId& cell, const LLColor4& tint, F64 now)
{
    const S32 tier = cell.mTier - 1;
    if (tier < skmap::TIER_MIN || tier < mTier - FINER_FALLBACK_TIERS)
    {
        return;
    }
    for (S32 k = 0; k < 4; ++k)
    {
        const skmap::CellId child = { tier, cell.mX * 2 + (k & 1), cell.mY * 2 + (k >> 1) };
        const U64 key = bakeKey(layer, child);
        if (!key)
        {
            continue;
        }
        const auto it = mBakes.find(key);
        if (it == mBakes.end())
        {
            drawFinerBakes(view, layer, child, tint, now);
            continue;
        }
        it->second.mLastDrawn = now;
        drawCellCut(view, it->second.mTexture, child.minCorner(), child.maxCorner(), 0.f, 0.f, 1.f, 1.f, tint, layer);
    }
}

void SKMapScene::drawFloorplan(const skmap::MapView& view)
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerFloorplan", true);
    static LLCachedControl<F32> fade_m(gSavedSettings, "SKMapVerticalFade", 30.f);
    uploadBakes();
    evictBakes();
    mStats.mCells = 0;
    // The people's floor relations read this, so it stays current with the layer off.
    if (mScene)
    {
        updateAgentFloor();
    }
    if (!show || !mScene)
    {
        mWanted.clear();
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    // The baked settings: part of every bake key, so a toggle re-bakes lazily in view.
    static LLCachedControl<bool> stairs(gSavedSettings, "SKMapLayerStairs", true);
    static LLCachedControl<bool> contours(gSavedSettings, "SKMapLayerContours", true);
    static LLCachedControl<bool> heatmap(gSavedSettings, "SKMapHeatmap", false);
    mRasterParams.mStairs = stairs;
    mRasterParams.mContours = contours;
    mRasterParams.mHeatmap = heatmap;
    updateSelection();
    const skmap::Scene& scene = *mScene;
    const F32 za = mAgent.mFound ? mAgent.mZ : static_cast<F32>(mAgent.mPos.mdV[VZ]);
    mTier = skmap::chooseTier(view.mpp(), mTier);
    LLVector3d lo, hi;
    view.visibleBox(lo, hi);
    std::vector<skmap::CellId> cells;
    skmap::cellsInBox(mTier, lo, hi, cells);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLVector3d centre = view.centre();
    uploadRibbons();
    const skmap::RibbonParams ribbon_params = ribbonParams(view);

    for (S32 l : mSelection.mDrawn)
    {
        const skmap::SceneLayer& layer = scene.mLayers[l];
        // Relative height: the ground layer by the ground under the agent.
        F32 base = layer.mBaseZ;
        if (layer.mGround)
        {
            const LLVector2 p = scene.toLocal(mAgent.mFloorPos);
            const auto it = scene.mIndex.find(skmap::indexCell(mAgent.mFloorPos.mdV[VX], mAgent.mFloorPos.mdV[VY]));
            if (it != scene.mIndex.end())
            {
                F32 best = -FLT_MAX;
                for (U32 id : it->second)
                {
                    if (scene.mPolys[id].mLayer == l && skmap::polyContains(scene, scene.mPolys[id], p))
                    {
                        best = llmax(best, skmap::polyHeightAt(scene, scene.mPolys[id], p));
                    }
                }
                base = best > -FLT_MAX ? best : base;
            }
        }
        const F32 delta = base - za;
        const F32 m = llclamp(0.75f + 0.2f * std::tanh(delta / 5.f), 0.45f, 1.f);
        F32 fade = 1.f;
        if (!layer.mGround)
        {
            const F32 half = fade_m * 0.5f;
            fade = std::fabs(delta) <= half ? 1.f : 1.f - 0.5f * llclamp((std::fabs(delta) - half) / llmax(half, 1e-3f), 0.f, 1.f);
        }
        LLColor4 tint(m, m, m, fade);
        if (heatmap)
        {
            // Below the agent's floor towards blue, above towards red, from the neutral bake.
            const skmap::PaletteColour& heat = delta < 0.f ? skmap::palette::HEAT_BELOW : skmap::palette::HEAT_ABOVE;
            const skmap::PaletteColour& neutral = skmap::palette::HEAT_NEUTRAL;
            const F32 k = std::fabs(std::tanh(delta / 8.f));
            const F32 t[3] = { lerp(1.f, heat.mR / neutral.mR, k), lerp(1.f, heat.mG / neutral.mG, k), lerp(1.f, heat.mB / neutral.mB, k) };
            // The factors reach 1.67, which the 8-bit vertex colour would clip: dim the whole tint instead, keeping the hue.
            const F32 peak = m * llmax(t[0], llmax(t[1], t[2]));
            const F32 scale = peak > 1.f ? m / peak : m;
            tint.mV[0] = scale * t[0];
            tint.mV[1] = scale * t[1];
            tint.mV[2] = scale * t[2];
        }

        // A floor keeps its slot across scenes, where its layer index does not.
        const S32 slot = layer.mGround ? INT_MIN : static_cast<S32>(std::floor(layer.mBaseZ / LAST_SHOWN_SLOT_M));
        for (const skmap::CellId& cell : cells)
        {
            const U64 key = bakeKey(l, cell);
            if (!key)
            {
                continue;
            }
            const LLVector3d cmin = cell.minCorner(), cmax = cell.maxCorner();
            const auto shown = std::make_tuple(cell.mTier, cell.mX, cell.mY, slot);
            auto it = mBakes.find(key);
            if (it != mBakes.end())
            {
                it->second.mLastDrawn = now;
                drawCellCut(view, it->second.mTexture, cmin, cmax, 0.f, 0.f, 1.f, 1.f, tint, l);
                mLastShown[shown] = key;
                mStats.mCells++;
                continue;
            }
            const LLVector3d mid = (cmin + cmax) * 0.5;
            requestBake(l, cell, key, static_cast<F32>(std::hypot(mid.mdV[VX] - centre.mdV[VX], mid.mdV[VY] - centre.mdV[VY])));
            // Meanwhile what this cell showed before, replaced in place when the new bake lands.
            const auto last = mLastShown.find(shown);
            if (last != mLastShown.end())
            {
                const auto lit = mBakes.find(last->second);
                if (lit != mBakes.end())
                {
                    lit->second.mLastDrawn = now;
                    drawCellCut(view, lit->second.mTexture, cmin, cmax, 0.f, 0.f, 1.f, 1.f, tint, l);
                    mStats.mCells++;
                    continue;
                }
            }
            // Meanwhile the best coarser bake, through the part of it this cell covers.
            bool covered = false;
            for (S32 t = mTier + 1; t <= skmap::TIER_MAX && !covered; ++t)
            {
                const skmap::CellId up = cell.ancestor(t);
                const auto ait = mBakes.find(bakeKey(l, up));
                if (ait == mBakes.end())
                {
                    continue;
                }
                const LLVector3d amin = up.minCorner();
                const F64 size = skmap::tierCellSize(t);
                ait->second.mLastDrawn = now;
                drawCellCut(view, ait->second.mTexture, cmin, cmax, static_cast<F32>((cmin.mdV[VX] - amin.mdV[VX]) / size),
                            static_cast<F32>((cmin.mdV[VY] - amin.mdV[VY]) / size), static_cast<F32>((cmax.mdV[VX] - amin.mdV[VX]) / size),
                            static_cast<F32>((cmax.mdV[VY] - amin.mdV[VY]) / size), tint, l);
                covered = true;
            }
            if (!covered)
            {
                // Zooming out asks for cells no tier has baked yet: draw the finer ones already cached.
                drawFinerBakes(view, l, cell, tint, now);
            }
        }
        gGL.getTextureSlot(0)->unbind();
        drawRibbons(view, l, ribbon_params);
    }
    gGL.getTextureSlot(0)->unbind();
    postBake();
    postRibbons();
}

skmap::RibbonParams SKMapScene::ribbonParams(const skmap::MapView& view) const
{
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> peek(gSavedSettings, "SKNavCoverPeek", 0.15f);
    static LLCachedControl<bool> cover(gSavedSettings, "SKMapLayerCover", true);
    static LLCachedControl<bool> doors(gSavedSettings, "SKMapLayerDoors", true);
    skmap::RibbonParams p;
    // Rebuilt per x1.1 zoom bucket, never per frame of a zoom.
    p.mMpp = skmap::bucketMpp(skmap::zoomBucket(view.mpp()));
    p.mEye = isAgentAvatarValid() ? gAgentAvatarp->mBodySize.mV[VZ] * 0.94f : 1.8f;
    p.mClimb = climb;
    p.mPeek = peek;
    p.mCover = cover;
    p.mDoors = doors;
    return p;
}

U64 SKMapScene::ribbonKey(U64 mark_hash, const skmap::RibbonParams& params)
{
    U64 key = sknav::mix(mark_hash, static_cast<U64>(skmap::zoomBucket(params.mMpp)));
    key = sknav::mix(key, static_cast<U64>(ll_round(params.mEye * 100.f)));
    key = sknav::mix(key, static_cast<U64>(ll_round(params.mClimb * 100.f)));
    key = sknav::mix(key, static_cast<U64>(ll_round(params.mPeek * 100.f)));
    return sknav::mix(key, (params.mCover ? 1u : 0u) | (params.mDoors ? 2u : 0u));
}

namespace
{
    /// A ribbon buffer for these triangles, positions written at the scale given.
    LLPointer<LLVertexBuffer> makeRibbonBuffer(const skmap::Ribbons& ribbons, F32 mpp)
    {
        if (ribbons.mVerts.empty())
        {
            return nullptr;
        }
        // The overlay's attribute set: what gUIProgram reads.
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR);
        LLStrider<LLVector3> positions;
        LLStrider<LLVector2> texcoords;
        LLStrider<LLColor4U> colours;
        if (!vb->allocateBuffer(static_cast<U32>(ribbons.mVerts.size()), 0) || !vb->getVertexStrider(positions)
            || !vb->getTexCoord0Strider(texcoords) || !vb->getColorStrider(colours))
        {
            return nullptr;
        }
        for (U32 i = 0; i < static_cast<U32>(ribbons.mVerts.size()); ++i)
        {
            positions[i] = ribbons.at(i, mpp);
            texcoords[i] = LLVector2(0.f, 0.f);
            colours[i] = ribbons.mColours[i];
        }
        vb->unmapBuffer();
        // The upload bound the buffer without its attributes: make the next setBuffer() set them up.
        LLVertexBuffer::unbind();
        return vb;
    }
}

void SKMapScene::drawBuffer(const skmap::MapView& view, RibbonBuffer& buffer) const
{
    LLVertexBuffer* vb = buffer.mBuffer;
    if (!vb || !vb->getNumVerts())
    {
        return;
    }
    // Widths are screen pixels: when the zoom has moved since the positions were written, move them out or in.
    const F32 mpp = view.mpp();
    if (buffer.mSource && std::fabs(mpp - buffer.mMpp) > buffer.mMpp * 0.002f)
    {
        LLStrider<LLVector3> positions;
        if (vb->getVertexStrider(positions))
        {
            for (U32 i = 0; i < static_cast<U32>(buffer.mSource->mVerts.size()); ++i)
            {
                positions[i] = buffer.mSource->at(i, mpp);
            }
            vb->unmapBuffer();
            // As in makeRibbonBuffer: otherwise the draw below reads through the last buffer's attributes.
            LLVertexBuffer::unbind();
            buffer.mMpp = mpp;
        }
    }
    const LLVector3d& origin = buffer.mOrigin;
    // Scene-local metres to widget pixels, from the scene the buffer was built in: a cached buffer outlives a box re-snap.
    const LLVector3d offset = origin - view.centre();
    gGL.flush();
    gGL.pushMatrix();
    gGL.translatef(view.width() * 0.5f, view.height() * 0.5f, 0.f);
    gGL.rotatef(view.rotation() * RAD_TO_DEG, 0.f, 0.f, 1.f);
    gGL.scalef(view.pixelsPerMetre(), view.pixelsPerMetre(), 1.f);
    gGL.translatef(static_cast<F32>(offset.mdV[VX]), static_cast<F32>(offset.mdV[VY]), 0.f);
    gGL.getTextureSlot(0)->bindSampled(LLViewerFetchedTexture::sWhiteImagep, ALSamplers::BilinearClamp);
    gGL.syncMatrices();
    vb->setBuffer();
    vb->drawArrays(LLRender::TRIANGLES, 0, vb->getNumVerts());
    gGL.popMatrix();
    gGL.getTextureSlot(0)->unbind();
}

U64 SKMapScene::ribbonIdentity(S32 layer) const
{
    const U64 marks = mScene->mMarkHashes[layer];
    // A cut layer has lines of its own, never stood in for by its uncut ones.
    const auto cut = mCutHashes.find(layer);
    return cut != mCutHashes.end() ? sknav::mix(marks, cut->second) : marks;
}

U64 SKMapScene::carriedRibbon(S32 layer)
{
    const auto cached = mCarried.find(layer);
    if (cached != mCarried.end())
    {
        return cached->second;
    }
    // The previous layer on the same kind of ground, at the same height and with the same cut, that shares
    // the most footprint: overlap alone cannot tell stacked storeys apart.
    const skmap::SceneLayer& here = mScene->mLayers[layer];
    const auto cut = mCutHashes.find(layer);
    const U64 cut_hash = cut != mCutHashes.end() ? cut->second : 0;
    U64 best = 0;
    size_t best_hits = 0;
    for (const CarrySource& source : mCarrySources)
    {
        // A layer that split hands its lines to one successor, not to each.
        const bool claimed = std::any_of(mCarried.begin(), mCarried.end(), [&](const auto& c) { return c.second == source.mKey; });
        if (claimed || source.mGround != here.mGround || source.mCut != cut_hash
            || (!here.mGround && std::fabs(source.mBaseZ - here.mBaseZ) > skmap::LAYER_GAP_M))
        {
            continue;
        }
        size_t hits = 0;
        for (const auto& cell : source.mSample)
        {
            hits += here.mFootprint.count(cell);
        }
        if (hits > best_hits)
        {
            best_hits = hits;
            best = source.mKey;
        }
    }
    mCarried[layer] = best;
    return best;
}

void SKMapScene::drawRibbons(const skmap::MapView& view, S32 layer, const skmap::RibbonParams& params)
{
    if (layer < 0 || layer >= static_cast<S32>(mScene->mMarkHashes.size()))
    {
        return;
    }
    const U64 marks = ribbonIdentity(layer);
    const U64 key = ribbonKey(marks, params);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    auto it = mRibbons.find(key);
    if (it == mRibbons.end())
    {
        mRibbonWanted.push_back({ layer, key, marks, params });
        // The old buffer keeps drawing until the new one lands.
        const auto latest = mRibbonLatest.find(marks);
        it = latest != mRibbonLatest.end() ? mRibbons.find(latest->second) : mRibbons.end();
        // A layer whose marks changed with the scene draws what the layer it replaces drew.
        if (it == mRibbons.end())
        {
            if (const U64 carried = carriedRibbon(layer))
            {
                it = mRibbons.find(carried);
            }
        }
        // A cut that moved keeps the lines of the layer's previous cut; an uncut layer never stands in for a cut one.
        if (it == mRibbons.end())
        {
            const auto shown = mShownRibbons.find(layer);
            if (shown != mShownRibbons.end() && shown->second.second != 0)
            {
                it = mRibbons.find(shown->second.first);
            }
        }
    }
    if (it != mRibbons.end())
    {
        it->second.mLastDrawn = now;
        drawBuffer(view, it->second);
        const auto cut = mCutHashes.find(layer);
        mShownRibbons[layer] = { it->first, cut != mCutHashes.end() ? cut->second : 0 };
    }
}

void SKMapScene::postRibbons()
{
    if (mRibbonInFlight || mRibbonWanted.empty() || !mScene)
    {
        mRibbonWanted.clear();
        return;
    }
    const LL::WorkQueue::ptr_t work = queue();
    if (work)
    {
        const skmap::ScenePtr scene = mScene;
        const RibbonRequest request = mRibbonWanted.front();
        // The cut as selected this frame, which is what the request's identity hashed.
        std::shared_ptr<const skmap::CutCells> cut;
        if (mCutHashes.count(request.mLayer))
        {
            cut = std::make_shared<const skmap::CutCells>(mSelection.mCut.at(request.mLayer));
        }
        std::weak_ptr<bool> alive = mAlive;
        const bool posted = work->post([this, scene, request, cut, alive]() {
            auto result = std::make_shared<RibbonResult>();
            result->mKey = request.mKey;
            result->mMarkHash = request.mMarkHash;
            result->mOrigin = scene->mOrigin;
            skmap::buildLayerRibbons(*scene, request.mLayer, request.mParams, result->mRibbons, cut.get());
            LL::WorkQueue::postMaybe(LL::WorkQueue::getInstance("mainloop"), [this, result, alive]() {
                if (!alive.expired())
                {
                    mRibbonInFlight = false;
                    mRibbonUploads.push_back(std::move(*result));
                }
            });
        });
        mRibbonInFlight = posted;
    }
    mRibbonWanted.clear();
}

void SKMapScene::uploadRibbons()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    while (!mRibbonUploads.empty())
    {
        RibbonResult& r = mRibbonUploads.front();
        auto source = std::make_shared<const skmap::Ribbons>(std::move(r.mRibbons));
        RibbonBuffer& buffer = mRibbons[r.mKey];
        buffer.mBuffer = makeRibbonBuffer(*source, source->mMpp);
        buffer.mMpp = source->mMpp;
        buffer.mSource = std::move(source);
        buffer.mOrigin = r.mOrigin;
        buffer.mLastDrawn = now;
        mRibbonLatest[r.mMarkHash] = r.mKey;
        mRibbonUploads.pop_front();
    }
    // Buffers not drawn for a while go.
    for (auto it = mRibbons.begin(); it != mRibbons.end();)
    {
        if (now - it->second.mLastDrawn > 30.0)
        {
            it = mRibbons.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SKMapScene::drawVoid(const skmap::MapView& view)
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerVoid", true);
    if (!show || !LLWorld::instanceExists())
    {
        return;
    }
    if (mHatch.isNull())
    {
        // One diagonal stripe in an 8 x 8 tile, repeated in screen pixels.
        const S32 n = skmap::palette::HATCH_PITCH_PX;
        LLPointer<LLImageRaw> raw = new LLImageRaw(n, n, 4);
        U8* data = raw->getData();
        for (S32 y = 0; y < n; ++y)
        {
            for (S32 x = 0; x < n; ++x)
            {
                U8* p = &data[(y * n + x) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = (x + y) % n == 0 ? 255 : 0;
            }
        }
        mHatch = LLViewerTextureManager::getLocalTexture(raw.get(), false);
    }
    const std::vector<LLViewerRegion*> regions(LLWorld::getInstance()->getRegionList().begin(), LLWorld::getInstance()->getRegionList().end());
    auto inRegion = [&regions](F64 x, F64 y) {
        for (LLViewerRegion* region : regions)
        {
            const LLVector3d o = region->getOriginGlobal();
            const F64 w = region->getWidth();
            if (x >= o.mdV[VX] && x < o.mdV[VX] + w && y >= o.mdV[VY] && y < o.mdV[VY] + w)
            {
                return true;
            }
        }
        return false;
    };
    // World-aligned 16 m cells outside every region; the pattern runs in screen pixels.
    constexpr F64 CELL = 16.0;
    LLVector3d lo, hi;
    view.visibleBox(lo, hi);
    const S32 x0 = static_cast<S32>(std::floor(lo.mdV[VX] / CELL)), x1 = static_cast<S32>(std::floor(hi.mdV[VX] / CELL));
    const S32 y0 = static_cast<S32>(std::floor(lo.mdV[VY] / CELL)), y1 = static_cast<S32>(std::floor(hi.mdV[VY] / CELL));
    const F32 pitch = static_cast<F32>(skmap::palette::HATCH_PITCH_PX);
    gGL.getTextureSlot(0)->bindSampled(mHatch, ALSamplers::PointWrap);
    skmapdraw::colour(skmap::palette::HATCH);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 cy = y0; cy <= y1; ++cy)
    {
        for (S32 cx = x0; cx <= x1; ++cx)
        {
            if (inRegion((cx + 0.5) * CELL, (cy + 0.5) * CELL))
            {
                continue;
            }
            const LLVector2 a = view.toWidget(LLVector3d(cx * CELL, cy * CELL, 0.0));
            const LLVector2 b = view.toWidget(LLVector3d((cx + 1) * CELL, cy * CELL, 0.0));
            const LLVector2 c = view.toWidget(LLVector3d((cx + 1) * CELL, (cy + 1) * CELL, 0.0));
            const LLVector2 d = view.toWidget(LLVector3d(cx * CELL, (cy + 1) * CELL, 0.0));
            for (const LLVector2* v : { &a, &b, &c, &a, &c, &d })
            {
                gGL.texCoord2f(v->mV[VX] / pitch, v->mV[VY] / pitch);
                gGL.vertex2f(v->mV[VX], v->mV[VY]);
            }
        }
    }
    gGL.end();
    gGL.getTextureSlot(0)->unbind();

    // The region and void edges, rebuilt when they or the zoom bucket move, not on every landing.
    if (!mScene)
    {
        return;
    }
    skmap::RibbonParams params = ribbonParams(view);
    const U64 key = sknav::mix(mBoundaryHash, static_cast<U64>(skmap::zoomBucket(params.mMpp)));
    if (key != mBoundaryKey)
    {
        mBoundaryKey = key;
        auto ribbons = std::make_shared<skmap::Ribbons>();
        skmap::buildBoundaryRibbons(*mScene, params, *ribbons);
        mBoundary.mOrigin = mScene->mOrigin;
        mBoundary.mBuffer = makeRibbonBuffer(*ribbons, ribbons->mMpp);
        mBoundary.mMpp = ribbons->mMpp;
        mBoundary.mSource = std::move(ribbons);
    }
    drawBuffer(view, mBoundary);
}
