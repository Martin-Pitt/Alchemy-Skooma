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
#include "llviewertexture.h"
#include "skmappalette.h"

#include <cfloat>
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
    mBakes.clear();
    mUploads.clear();
    mWanted.clear();
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

void SKMapScene::updateAgentFloor()
{
    const skmap::Scene& scene = *mScene;
    const LLVector3d agent = gAgent.getPositionGlobal();
    skmap::AgentFloor floor;
    floor.mPos = agent;
    const skmap::FloorHit hit = skmap::floorAt(scene, agent, 2.5f, 0.5f);
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
    if (mSelectedScene == &scene && mSelectedCell == cell && mSelectedLayer == floor.mLayer)
    {
        return;
    }
    static LLCachedControl<F32> fade(gSavedSettings, "SKMapVerticalFade", 30.f);
    mSelection = skmap::selectLayers(scene, floor, sknav::EnclosureBucket::OUTDOORS, fade);
    mSelectedScene = &scene;
    mSelectedCell = cell;
    mSelectedLayer = floor.mLayer;
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
    mStats.mCacheMB = mBakes.size() * BAKE_MB;
}

void SKMapScene::drawCell(const skmap::MapView& view, LLViewerTexture* texture, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0,
                          F32 u1, F32 v1, const LLColor4& tint) const
{
    // 2 x 2 sub-quads, so the distance dim varies across a cell.
    const LLVector3d agent = gAgent.getPositionGlobal();
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
    gGL.getTextureSlot(0)->bindSampled(texture, ALSamplers::BilinearClamp);
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
        drawCell(view, it->second.mTexture, child.minCorner(), child.maxCorner(), 0.f, 0.f, 1.f, 1.f, tint);
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

    for (S32 l : mSelection.mDrawn)
    {
        const skmap::SceneLayer& layer = scene.mLayers[l];
        // Relative height: the ground layer by the ground under the agent.
        F32 base = layer.mBaseZ;
        if (layer.mGround)
        {
            const LLVector2 p = scene.toLocal(mAgent.mPos);
            const auto it = scene.mIndex.find(skmap::indexCell(mAgent.mPos.mdV[VX], mAgent.mPos.mdV[VY]));
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
        const LLColor4 tint(m, m, m, fade);

        for (const skmap::CellId& cell : cells)
        {
            const U64 key = bakeKey(l, cell);
            if (!key)
            {
                continue;
            }
            const LLVector3d cmin = cell.minCorner(), cmax = cell.maxCorner();
            auto it = mBakes.find(key);
            if (it != mBakes.end())
            {
                it->second.mLastDrawn = now;
                drawCell(view, it->second.mTexture, cmin, cmax, 0.f, 0.f, 1.f, 1.f, tint);
                mStats.mCells++;
                continue;
            }
            const LLVector3d mid = (cmin + cmax) * 0.5;
            requestBake(l, cell, key, static_cast<F32>(std::hypot(mid.mdV[VX] - centre.mdV[VX], mid.mdV[VY] - centre.mdV[VY])));
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
                drawCell(view, ait->second.mTexture, cmin, cmax, static_cast<F32>((cmin.mdV[VX] - amin.mdV[VX]) / size),
                         static_cast<F32>((cmin.mdV[VY] - amin.mdV[VY]) / size), static_cast<F32>((cmax.mdV[VX] - amin.mdV[VX]) / size),
                         static_cast<F32>((cmax.mdV[VY] - amin.mdV[VY]) / size), tint);
                covered = true;
            }
            if (!covered)
            {
                // Zooming out asks for cells no tier has baked yet: draw the finer ones already cached.
                drawFinerBakes(view, l, cell, tint, now);
            }
        }
    }
    gGL.getTextureSlot(0)->unbind();
    postBake();
}
