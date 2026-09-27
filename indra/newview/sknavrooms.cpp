/**
 * @file sknavrooms.cpp
 * @brief The per-mesh enclosure pass, scheduled off the main thread, and the view its answers are read through.
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

#include "sknavrooms.h"

#include "llagent.h"
#include "llbase64.h"
#include "llviewercontrol.h"
#include "sknavmesh.h"
#include "sknavoverlay.h"
#include "sknavpalette.h"
#include "workqueue.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <ostream>

using namespace sknav;

namespace
{
    constexpr F64 CHUNK_MS = 20.0;          ///< a pass yields its thread this often
    constexpr U32 CHUNK_STEP = 4096;        ///< work between clock reads
    constexpr U32 MAX_RERUNS = 3;           ///< neighbour-driven re-runs per wave
    const char* const QUEUE_NAME = "SKNavRooms";
}

struct SKNavRooms::Job
{
    LLUUID mId;
    U64 mGeneration = 0;
    std::shared_ptr<std::atomic<U64>> mLatest;
    std::unique_ptr<EnclosurePass> mPass;
    std::chrono::steady_clock::time_point mPosted;
    bool mCancelled = false;
};

// ---------------------------------------------------------------- the view

bool SKNavWorldView::enclosureAt(const LLVector3d& pos, Enclosure& out) const
{
    bool found = false;
    for (const Mesh& m : mMeshes)
    {
        if (!m.mProduct || pos.mdV[VX] < m.mMin.mdV[VX] || pos.mdV[VY] < m.mMin.mdV[VY] || pos.mdV[VX] >= m.mMax.mdV[VX] ||
            pos.mdV[VY] >= m.mMax.mdV[VY])
        {
            continue;
        }
        Enclosure e;
        if (!m.mProduct->enclosureAt(LLVector3(pos - m.mOrigin), e))
        {
            continue;
        }
        e.mFloor += static_cast<F32>(m.mOrigin.mdV[VZ]);
        e.mCeiling += e.mCeiling > 1e29f ? 0.f : static_cast<F32>(m.mOrigin.mdV[VZ]);
        const bool known = e.mBucket != EnclosureBucket::UNKNOWN, had = found && out.mBucket != EnclosureBucket::UNKNOWN;
        if (!found || (known && !had) || (known == had && e.mFloor > out.mFloor))
        {
            out = e;
            found = true;
        }
    }
    return found;
}

bool SKNavWorldView::topOfWorld(const LLVector3d& xy, F64& top) const
{
    return mSky && mSky->topAt(xy.mdV[VX], xy.mdV[VY], top);
}

void SKNavWorldView::gapsAt(const LLVector3d& xy, const std::function<void(const Enclosure&)>& fn) const
{
    for (const Mesh& m : mMeshes)
    {
        if (!m.mProduct || xy.mdV[VX] < m.mMin.mdV[VX] || xy.mdV[VY] < m.mMin.mdV[VY] || xy.mdV[VX] >= m.mMax.mdV[VX] ||
            xy.mdV[VY] >= m.mMax.mdV[VY])
        {
            continue;
        }
        const F32 z = static_cast<F32>(m.mOrigin.mdV[VZ]);
        m.mProduct->gapsAt(static_cast<F32>(xy.mdV[VX] - m.mOrigin.mdV[VX]), static_cast<F32>(xy.mdV[VY] - m.mOrigin.mdV[VY]),
                           [&](const Enclosure& e) {
                               Enclosure g = e;
                               g.mFloor += z;
                               g.mCeiling += g.mCeiling > 1e29f ? 0.f : z;
                               fn(g);
                           });
    }
}

void SKNavWorldView::raster(const LLVector3d& min, const LLVector3d& max, F32 res, Channel channel, std::vector<F32>& out) const
{
    res = llmax(res, 0.05f);
    const S32 nx = llmax(1, static_cast<S32>(std::ceil((max.mdV[VX] - min.mdV[VX]) / res)));
    const S32 ny = llmax(1, static_cast<S32>(std::ceil((max.mdV[VY] - min.mdV[VY]) / res)));
    out.assign(static_cast<size_t>(nx) * ny, std::numeric_limits<F32>::quiet_NaN());
    for (S32 iy = 0; iy < ny; ++iy)
    {
        for (S32 ix = 0; ix < nx; ++ix)
        {
            const LLVector3d at(min.mdV[VX] + (ix + 0.5) * res, min.mdV[VY] + (iy + 0.5) * res, 0.0);
            F32& v = out[ix + iy * nx];
            if (channel == Channel::TOP)
            {
                F64 top;
                if (topOfWorld(at, top))
                {
                    v = static_cast<F32>(top);
                }
                continue;
            }
            F32 best = -std::numeric_limits<F32>::max();
            gapsAt(at, [&](const Enclosure& e) {
                if (e.mFloor < min.mdV[VZ] || e.mFloor > max.mdV[VZ] || e.mFloor <= best)
                {
                    return;
                }
                const bool known = e.mBucket != EnclosureBucket::UNKNOWN;
                switch (channel)
                {
                    case Channel::S: v = e.mS; break;
                    case Channel::E: v = known ? e.mE : std::numeric_limits<F32>::quiet_NaN(); break;
                    case Channel::D: v = known ? e.mD : std::numeric_limits<F32>::quiet_NaN(); break;
                    default: v = e.mFloor; break;
                }
                best = e.mFloor;
            });
        }
    }
}

// ---------------------------------------------------------------- the scheduler

SKNavRooms::SKNavRooms()
    : mAlive(std::make_shared<bool>(true))
{
}

SKNavRooms::~SKNavRooms()
{
    close();
}

void SKNavRooms::close()
{
    mAlive.reset();
    if (mThread)
    {
        mThread->close();
        mThread.reset();
    }
}

bool SKNavRooms::touches(const MeshState& a, const MeshState& b) const
{
    // A metre of slack: meshes whose tiles only meet at an edge still hand air across it.
    return a.mMin.mdV[VX] <= b.mMax.mdV[VX] + 1.0 && b.mMin.mdV[VX] <= a.mMax.mdV[VX] + 1.0 && a.mMin.mdV[VY] <= b.mMax.mdV[VY] + 1.0 &&
           b.mMin.mdV[VY] <= a.mMax.mdV[VY] + 1.0;
}

void SKNavRooms::update(const SKNavMesh& navmesh)
{
    if (!mAlive)
    {
        return;
    }
    static LLCachedControl<U32> settle_ms(gSavedSettings, "SKNavRoomsSettleMs", 500);
    const F64 now = LLTimer::getTotalSeconds();

    // A publish moves the navmesh's revision; only then can a mesh's air have moved.
    if (navmesh.mRevision != mSeenRevision)
    {
        mSeenRevision = navmesh.mRevision;
        std::map<LLUUID, bool> seen;
        for (const auto& entry : navmesh.mMeshes)
        {
            const SKNavMesh::Mesh& mesh = *entry.second;
            U64 sig = SEED;
            LLVector3d lo(1e300, 1e300, 0.0), hi(-1e300, -1e300, 0.0);
            for (const auto& t : mesh.mTiles)
            {
                if (!t.second.mAir)
                {
                    continue;
                }
                sig = mix(mix(sig, (static_cast<U64>(static_cast<U32>(t.first.mX)) << 32) | static_cast<U32>(t.first.mY)),
                          reinterpret_cast<uintptr_t>(t.second.mAir.get()));
                lo.mdV[VX] = llmin(lo.mdV[VX], t.second.mMin.mdV[VX]);
                lo.mdV[VY] = llmin(lo.mdV[VY], t.second.mMin.mdV[VY]);
                hi.mdV[VX] = llmax(hi.mdV[VX], t.second.mMax.mdV[VX]);
                hi.mdV[VY] = llmax(hi.mdV[VY], t.second.mMax.mdV[VY]);
            }
            if (sig == SEED && !mMeshes.count(mesh.mId))
            {
                continue;       // no air, and never had any
            }
            seen[mesh.mId] = true;
            MeshState& state = mMeshes[mesh.mId];
            state.mId = mesh.mId;
            state.mOrigin = mesh.mOrigin;
            if (state.mAirSignature != sig)
            {
                state.mAirSignature = sig;
                state.mMin = lo;
                state.mMax = hi;
                state.mDirty = true;
                state.mChanged = now;
                state.mReruns = 0;
                if (!state.mLatest)
                {
                    state.mLatest = std::make_shared<std::atomic<U64>>(0);
                }
                state.mLatest->store(++state.mGeneration);
            }
        }
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            if (!seen.count(it->first))
            {
                if (it->second.mLatest)
                {
                    it->second.mLatest->store(~0ull);   // cancel anything in flight
                }
                it = mMeshes.erase(it);
                mViewDirty = true;
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& entry : mMeshes)
    {
        MeshState& state = entry.second;
        if (state.mDirty && !state.mInFlight && (now - state.mChanged) * 1000.0 >= settle_ms)
        {
            if (state.mAirSignature == SEED)
            {
                // Its air is gone: nothing to answer with.
                state.mDirty = false;
                if (state.mProduct)
                {
                    state.mProduct.reset();
                    mViewDirty = true;
                }
                continue;
            }
            post(state, navmesh);
        }
    }

    // The view's sky map is the copy the last pass was handed, not one per publish.
    if (mViewDirty)
    {
        rebuildView();
    }
}

void SKNavRooms::post(MeshState& state, const SKNavMesh& navmesh)
{
    const auto mit = navmesh.mMeshes.find(state.mId);
    if (mit == navmesh.mMeshes.end())
    {
        return;
    }
    if (!mThread)
    {
        // One thread: passes run one at a time, and never behind the tile builds.
        mThread = std::make_unique<LL::ThreadPool>(QUEUE_NAME, 1);
        mThread->start();
    }
    if (navmesh.mSkyMap.version() != mSkyVersion)
    {
        mSky = std::make_shared<const SkyMap>(navmesh.mSkyMap);
        mSkyVersion = navmesh.mSkyMap.version();
    }

    const SKNavMesh::Mesh& mesh = *mit->second;
    auto input = std::make_shared<EnclosureInput>();
    input->mGeneration = state.mGeneration;
    input->mMeshOrigin = mesh.mOrigin;
    input->mSky = mSky;
    input->mPrevious = state.mProduct;
    // Feet need an avatar's headroom: a slab void between storeys is not a floor.
    sknav::TileBuildInput build;
    navmesh.fillParams(build);
    input->mParams.mWalkHeadM = build.mParams[static_cast<S32>(sknav::AgentClass::AVATAR)].mHeight;
    static LLCachedControl<bool> sealed(gSavedSettings, "SKNavSealedIndoors", true);
    input->mParams.mSealedIndoors = sealed;
    for (const auto& t : mesh.mTiles)
    {
        if (t.second.mAir && !t.second.mAir->mLeaves.empty())
        {
            input->mTiles.push_back({ t.first, t.second.mAir, SKNavMesh::skyContributor(mesh.mId, t.first) });
        }
    }
    for (const auto& other : mMeshes)
    {
        if (other.first != state.mId && other.second.mProduct && touches(state, other.second))
        {
            input->mNeighbours.push_back({ other.second.mOrigin, other.second.mProduct });
        }
    }

    auto job = std::make_shared<Job>();
    job->mId = state.mId;
    job->mGeneration = state.mGeneration;
    job->mLatest = state.mLatest;
    job->mPass = std::make_unique<EnclosurePass>(input);
    job->mPosted = std::chrono::steady_clock::now();
    std::weak_ptr<bool> alive = mAlive;
    const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
    const bool posted = queue && queue->post([this, job, alive]() {
        if (!alive.expired())
        {
            runChunk(job, alive);
        }
    });
    if (posted)
    {
        state.mDirty = false;
        state.mInFlight = true;
    }
}

void SKNavRooms::runChunk(const std::shared_ptr<Job>& job, const std::weak_ptr<bool>& alive)
{
    // On the rooms thread, which must not touch mAlive: main resets it on close.
    auto land = [this, job, alive]() {
        LL::WorkQueue::postMaybe(LL::WorkQueue::getInstance("mainloop"), [this, job, alive]() {
            if (!alive.expired())
            {
                finish(job);
            }
        });
    };
    if (job->mLatest->load() != job->mGeneration)
    {
        job->mCancelled = true;
        land();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    while (!job->mPass->step(CHUNK_STEP))
    {
        if (std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count() >= CHUNK_MS)
        {
            // Yield, so a newer generation can cancel this one between chunks.
            const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
            const bool posted = queue && queue->post([this, job, alive]() {
                if (!alive.expired())
                {
                    runChunk(job, alive);
                }
            });
            if (!posted)
            {
                job->mCancelled = true;
                land();
            }
            return;
        }
    }
    land();
}

void SKNavRooms::finish(const std::shared_ptr<Job>& job)
{
    const auto it = mMeshes.find(job->mId);
    if (it == mMeshes.end())
    {
        return;
    }
    MeshState& state = it->second;
    // A mesh removed and re-added restarts its generations: the counter must be the same one too.
    if (state.mLatest != job->mLatest)
    {
        ++mCancelled;
        return;
    }
    state.mInFlight = false;
    if (job->mCancelled || state.mGeneration != job->mGeneration || !job->mPass->done())
    {
        ++mCancelled;
        return;     // superseded: the state is dirty again and posts anew once settled
    }
    const EnclosureProductPtr product = job->mPass->product();
    ++mPasses;
    mLastMs = product->mMillis;
    mLastWallMs = static_cast<F32>(std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - job->mPosted).count());
    for (S32 s = 0; s < EnclosurePass::NUM_STAGES; ++s)
    {
        mLastStageMs[s] = job->mPass->stageMillis(s);
    }
    const bool moved = !state.mProduct || state.mProduct->mSummary != product->mSummary;
    state.mProduct = product;
    mViewDirty = true;
    if (!moved)
    {
        return;
    }
    // Neighbours read this mesh one generation late: run them again, a few times at most.
    for (auto& other : mMeshes)
    {
        MeshState& o = other.second;
        if (other.first != state.mId && touches(state, o) && o.mReruns < MAX_RERUNS && o.mAirSignature != SEED)
        {
            ++o.mReruns;
            o.mDirty = true;
        }
    }
}

void SKNavRooms::rebuildView()
{
    mViewDirty = false;
    auto view = std::make_shared<SKNavWorldView>();
    view->mRevision = ++mViewRevision;
    view->mSky = mSky;
    for (const auto& entry : mMeshes)
    {
        const MeshState& s = entry.second;
        if (s.mProduct)
        {
            view->mMeshes.push_back({ s.mId, s.mOrigin, s.mMin, s.mMax, s.mProduct });
        }
    }
    mView = view;
}

std::string SKNavRooms::statsLine() const
{
    U64 gaps = 0, unknown = 0, sealed = 0;
    U32 meshes = 0, in_flight = 0;
    for (const auto& entry : mMeshes)
    {
        in_flight += entry.second.mInFlight ? 1 : 0;
        if (entry.second.mProduct)
        {
            ++meshes;
            gaps += entry.second.mProduct->mGapCount;
            unknown += entry.second.mProduct->mUnknownGaps;
            sealed += entry.second.mProduct->mSealedGaps;
        }
    }
    std::string stages;
    for (S32 s = 0; s < EnclosurePass::NUM_STAGES; ++s)
    {
        stages += llformat("%s%s %.0f", s ? ", " : "", EnclosurePass::stageName(s), mLastStageMs[s]);
    }
    return llformat("enclosure: %u meshes, %u passes (%u dropped, %u running), last %.0f ms on the thread (%s), %.0f ms posted to landed, unknown %.1f%% and sealed %.1f%% of %llu gaps",
                    meshes, mPasses, mCancelled, in_flight, mLastMs, stages.c_str(), mLastWallMs,
                    gaps ? 100.0 * unknown / gaps : 0.0, gaps ? 100.0 * sealed / gaps : 0.0, static_cast<unsigned long long>(gaps));
}

void SKNavRooms::gatherOverlay(SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach, U32 mode, bool opaque) const
{
    if (!mView || mode == 0)
    {
        return;
    }
    const LLColor4& outdoors = skNavSwatchColor(SKNavSwatch::ENCLOSURE_OUTDOORS);
    const LLColor4& sheltered = skNavSwatchColor(SKNavSwatch::ENCLOSURE_SHELTERED);
    const LLColor4& indoors = skNavSwatchColor(SKNavSwatch::ENCLOSURE_INDOORS);
    const LLColor4& unknown = skNavSwatchColor(SKNavSwatch::ENCLOSURE_UNKNOWN);
    const F64 reach2 = reach * reach;
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        for (const TileEnclosure& t : m.mProduct->mTiles)
        {
            const CoarseAir& c = t.mCoarse;
            const F64 side = c.mCells * t.mCell;
            const LLVector3d tile_min = m.mOrigin + LLVector3d(t.mOrigin);
            const LLVector3d tile_centre = tile_min + LLVector3d(t.mWidth * t.mCell * 0.5, t.mHeight * t.mCell * 0.5, 0.0);
            const F64 half = t.mWidth * t.mCell * 0.75;
            if (dist_vec_squared2D(tile_centre, camera) > (reach + half) * (reach + half))
            {
                continue;
            }
            for (S32 cy = 0; cy < c.mHeight; ++cy)
            {
                for (S32 cx = 0; cx < c.mWidth; ++cx)
                {
                    const LLVector3d corner = tile_min + LLVector3d(cx * side, cy * side, 0.0);
                    if (dist_vec_squared2D(corner + LLVector3d(side * 0.5, side * 0.5, 0.0), camera) > reach2)
                    {
                        continue;
                    }
                    const S32 cell = cx + cy * c.mWidth;
                    for (U32 i = c.mStart[cell]; i < c.mStart[cell + 1]; ++i)
                    {
                        const CoarseGap& g = c.mGaps[i];
                        LLColor4 color;
                        if (g.mD == ENCLOSURE_D_UNKNOWN)
                        {
                            color = unknown;
                        }
                        else if (mode == 1)
                        {
                            const F32 e = g.mE / 255.f;
                            color = e < 0.5f ? lerp(outdoors, sheltered, e * 2.f) : lerp(sheltered, indoors, e * 2.f - 1.f);
                        }
                        else
                        {
                            const EnclosureBucket b = enclosureBucket(g.mE / 255.f, EnclosureBucket::UNKNOWN, 0.f);
                            color = b == EnclosureBucket::OUTDOORS ? outdoors : (b == EnclosureBucket::INDOORS ? indoors : sheltered);
                        }
                        if (opaque)
                        {
                            color.mV[VALPHA] = 1.f;
                        }
                        const F64 z = corner.mdV[VZ] + g.mFloor * t.mUnit + 0.05;
                        const F32 inset = static_cast<F32>(side * 0.04);
                        const LLVector3 a = gAgent.getPosAgentFromGlobal(LLVector3d(corner.mdV[VX], corner.mdV[VY], z)) + LLVector3(inset, inset, 0.f);
                        const F32 s = static_cast<F32>(side) - 2.f * inset;
                        geom.tri(a, a + LLVector3(s, 0.f, 0.f), a + LLVector3(s, s, 0.f), color);
                        geom.tri(a, a + LLVector3(s, s, 0.f), a + LLVector3(0.f, s, 0.f), color);
                    }
                }
            }
        }
    }
}

void SKNavRooms::writeTile(std::ostream& file, const LLUUID& mesh, const TileKey& key, const TileAirPtr& air) const
{
    if (!mView || !air)
    {
        return;
    }
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        if (m.mId != mesh)
        {
            continue;
        }
        const TileEnclosure* t = m.mProduct->tile(key);
        if (!t || t->mAir != air || t->mGaps.size() != air->mGaps.size())
        {
            return;
        }
        std::vector<U8> blob;
        blob.reserve(t->mGaps.size() * 5);
        for (const GapEnclosure& g : t->mGaps)
        {
            blob.push_back(g.mS);
            blob.push_back(g.mE);
            blob.push_back(g.mD);
            blob.push_back(g.mBucket);
            blob.push_back(g.mFlags);
        }
        file << ",\"enclosure\":{\"generation\":" << m.mProduct->mGeneration
             << ",\"layout\":{\"record\":[\"S\",\"e\",\"D\",\"bucket\",\"flags\"],\"order\":\"the air's gaps\",\"S\":\"u8 / 255\",\"e\":\"u8 / 255\","
                "\"D\":\"u8 * 0.25 m, 255 unknown\",\"bucket\":[\"outdoors\",\"sheltered\",\"indoors\",\"unknown\"],\"flags\":{\"sealed\":1}},\"blob\":\""
             << LLBase64::encode(blob.data(), blob.size()) << "\"}";
        return;
    }
}
