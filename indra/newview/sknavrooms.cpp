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

#include "sknavlayout.h"

#include "llagent.h"
#include "llbase64.h"
#include "llthread.h"
#include "llviewercontrol.h"
#include "sknavexport.h"
#include "sknavmesh.h"
#include "sknavoverlay.h"
#include "sknavpalette.h"
#include "skworldcensus.h"
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
    std::shared_ptr<const RoomsInput> mRoomsInput;
    std::unique_ptr<SpacesPass> mSpaces;        ///< once the enclosure pass is done
    std::chrono::steady_clock::time_point mPosted;
    bool mCancelled = false;
};

struct SKNavRooms::WindJob
{
    LLUUID mId;
    U64 mGeneration = 0;
    std::shared_ptr<std::atomic<U64>> mLatest;
    std::unique_ptr<WindPass> mPass;
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

bool SKNavWorldView::spaceAt(const LLVector3d& pos, SpaceId& out) const
{
    // The gap enclosureAt would answer from, and its mesh.
    const Mesh* best = nullptr;
    Enclosure found;
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
        const bool known = e.mBucket != EnclosureBucket::UNKNOWN, had = best && found.mBucket != EnclosureBucket::UNKNOWN;
        if (!best || (known && !had) || (known == had && e.mFloor > found.mFloor))
        {
            found = e;
            best = &m;
        }
    }
    if (!best || !found.mSpaceId)
    {
        return false;
    }
    out.mMesh = best->mId;
    out.mId = found.mSpaceId;
    return true;
}

const Space* SKNavWorldView::space(const SpaceId& id, const Mesh** mesh_out) const
{
    for (const Mesh& m : mMeshes)
    {
        if (m.mId == id.mMesh && m.mRooms)
        {
            const Space* s = m.mRooms->byId(id.mId);
            if (s && mesh_out)
            {
                *mesh_out = &m;
            }
            return s;
        }
    }
    return nullptr;
}

void SKNavWorldView::spacesIn(const LLVector3d& min, const LLVector3d& max, const SpaceFn& fn) const
{
    for (const Mesh& m : mMeshes)
    {
        if (!m.mRooms)
        {
            continue;
        }
        for (const Space& s : m.mRooms->mSpaces)
        {
            const LLVector3d lo = m.mOrigin + LLVector3d(s.mMin), hi = m.mOrigin + LLVector3d(s.mMax);
            if (lo.mdV[VX] <= max.mdV[VX] && hi.mdV[VX] >= min.mdV[VX] && lo.mdV[VY] <= max.mdV[VY] && hi.mdV[VY] >= min.mdV[VY] &&
                lo.mdV[VZ] <= max.mdV[VZ] && hi.mdV[VZ] >= min.mdV[VZ])
            {
                fn(m, s);
            }
        }
    }
}

void SKNavWorldView::portalsIn(const LLVector3d& min, const LLVector3d& max, const PortalFn& fn) const
{
    for (const Mesh& m : mMeshes)
    {
        if (!m.mRooms)
        {
            continue;
        }
        for (const Portal& p : m.mRooms->mPortals)
        {
            const LLVector3d c = m.mOrigin + LLVector3d(p.mCentre);
            if (c.mdV[VX] >= min.mdV[VX] && c.mdV[VX] <= max.mdV[VX] && c.mdV[VY] >= min.mdV[VY] && c.mdV[VY] <= max.mdV[VY] &&
                c.mdV[VZ] >= min.mdV[VZ] && c.mdV[VZ] <= max.mdV[VZ])
            {
                fn(m, p);
            }
        }
    }
}

void SKNavWorldView::portalsOf(const SpaceId& id, const PortalFn& fn) const
{
    const Mesh* m = nullptr;
    const Space* s = space(id, &m);
    if (!s)
    {
        return;
    }
    for (U32 p : s->mPortals)
    {
        fn(*m, m->mRooms->mPortals[p]);
    }
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

// ---------------------------------------------------------------- wind

namespace
{
    F32 wrapHeading(F32 deg)
    {
        deg = std::fmod(deg, 360.f);
        return deg < 0.f ? deg + 360.f : deg;
    }
}

bool SKNavRooms::settlingIn(const LLVector3d& min, const LLVector3d& max) const
{
    for (const auto& m : mMeshes)
    {
        const MeshState& s = m.second;
        if ((s.mDirty || s.mInFlight) && overlapsXY(s.mMin, s.mMax, min, max) &&
            s.mMin.mdV[VZ] <= max.mdV[VZ] && min.mdV[VZ] <= s.mMax.mdV[VZ])
        {
            return true;
        }
    }
    return false;
}

SKNavWorldViewPtr SKNavRooms::view() const
{
    llassert(on_main_thread());
    return mView;
}

SKNavWindPtr SKNavRooms::wind() const
{
    llassert(on_main_thread());
    return mWind;
}

SKNavWind::SKNavWind(SKNavWorldViewPtr view)
    : mView(std::move(view))
{
}

U64 SKNavWind::revision() const
{
    return mView ? mView->revision() : 0;
}

const SKNavWorldView::Mesh* SKNavWind::meshAt(const LLVector3d& pos, S32* cell) const
{
    // The mesh whose ground is highest under the point without being above
    // it: a skybox's own floor rather than the terrain far below.
    const SKNavWorldView::Mesh* best = nullptr;
    F64 best_ground = 0.0;
    S32 best_cell = -1;
    if (!mView)
    {
        return nullptr;
    }
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        if (!m.mWind || m.mWind->mBands.empty())
        {
            continue;
        }
        const WindSlabPtr s = m.mWind->slab(0, 0);
        if (!s || !s->mZ)
        {
            continue;
        }
        const WindCell* c = s->at(static_cast<F32>(pos.mdV[VX] - m.mOrigin.mdV[VX]), static_cast<F32>(pos.mdV[VY] - m.mOrigin.mdV[VY]));
        if (!c || (c->mFlags & WindCell::UNSEEN))
        {
            continue;
        }
        const S32 i = static_cast<S32>(c - s->mCells.data());
        const F64 ground = (*s->mZ)[i] + m.mOrigin.mdV[VZ] - m.mWind->mBands[0].mOffsetM;
        const bool under = ground <= pos.mdV[VZ] + 0.5, best_under = best && best_ground <= pos.mdV[VZ] + 0.5;
        if (!best || (under && (!best_under || ground > best_ground)) || (!under && !best_under && ground < best_ground))
        {
            best = &m;
            best_ground = ground;
            best_cell = i;
        }
    }
    if (cell)
    {
        *cell = best_cell;
    }
    return best;
}

WindSlabPtr SKNavWind::slab(S32 heading, const LLVector3d& min, const LLVector3d& max, LLVector3d* origin) const
{
    const LLVector3d centre((min.mdV[VX] + max.mdV[VX]) * 0.5, (min.mdV[VY] + max.mdV[VY]) * 0.5, min.mdV[VZ]);
    S32 cell = -1;
    const SKNavWorldView::Mesh* m = meshAt(centre, &cell);
    if (!m)
    {
        return nullptr;
    }
    const F32 want = static_cast<F32>(min.mdV[VZ] - m->mOrigin.mdV[VZ]) + 1.5f;
    S32 band = 0;
    F32 best = std::numeric_limits<F32>::max();
    for (S32 b = 0; b < static_cast<S32>(m->mWind->mBands.size()); ++b)
    {
        const WindSlabPtr s = m->mWind->slab(heading, b);
        if (s && s->mZ && cell >= 0 && std::fabs((*s->mZ)[cell] - want) < best)
        {
            best = std::fabs((*s->mZ)[cell] - want);
            band = b;
        }
    }
    if (origin)
    {
        *origin = m->mOrigin;
    }
    return m->mWind->slab(heading, band);
}

bool SKNavWind::sampleAt(const LLVector3d& pos, F32 heading_deg, Sample& out) const
{
    S32 cell = -1;
    const SKNavWorldView::Mesh* m = meshAt(pos, &cell);
    if (!m || cell < 0)
    {
        return false;
    }
    const WindProduct& w = *m->mWind;
    const F32 at = wrapHeading(heading_deg) / (360.f / WIND_HEADINGS);
    const S32 h0 = static_cast<S32>(at) % WIND_HEADINGS, h1 = (h0 + 1) % WIND_HEADINGS;
    const F32 th = at - std::floor(at);
    // The bands around the point, by their heights at its cell.
    const F32 z = static_cast<F32>(pos.mdV[VZ] - m->mOrigin.mdV[VZ]);
    const S32 bands = static_cast<S32>(w.mBands.size());
    S32 b0 = 0, b1 = 0;
    F32 tb = 0.f;
    for (S32 b = 0; b < bands; ++b)
    {
        const WindSlabPtr s = w.slab(h0, b);
        if (!s || !s->mZ || (*s->mZ)[cell] > z)
        {
            continue;
        }
        b0 = b1 = b;
        tb = 0.f;
        const WindSlabPtr n = b + 1 < bands ? w.slab(h0, b + 1) : nullptr;
        if (n && n->mZ && (*n->mZ)[cell] > z)
        {
            const F32 lo = (*s->mZ)[cell], hi = (*n->mZ)[cell];
            b1 = b + 1;
            tb = (z - lo) / (hi - lo);
        }
    }
    F32 f[4];
    LLVector2 d[4];
    U8 fl[4];
    const S32 hs[4] = { h0, h1, h0, h1 }, bs[4] = { b0, b0, b1, b1 };
    for (S32 k = 0; k < 4; ++k)
    {
        const WindCell& c = w.slab(hs[k], bs[k])->mCells[cell];
        f[k] = (c.mFlags & WindCell::BLOCKED) ? 0.f : c.factor();
        d[k] = c.deflection();
        fl[k] = c.mFlags;
    }
    out.mFactor = lerp(lerp(f[0], f[1], th), lerp(f[2], f[3], th), tb);
    out.mDeflect = lerp(lerp(d[0], d[1], th), lerp(d[2], d[3], th), tb);
    out.mFlags = fl[(th < 0.5f ? 0 : 1) + (tb < 0.5f ? 0 : 2)];
    return true;
}

void SKNavRooms::gatherWind(SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach, F32 heading_deg, bool opaque) const
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rooms - gather wind");
    if (!mView)
    {
        return;
    }
    const S32 heading = static_cast<S32>(std::lround(wrapHeading(heading_deg) / (360.f / WIND_HEADINGS))) % WIND_HEADINGS;
    const F32 rad = windHeadingDeg(heading) * DEG_TO_RAD;
    const LLVector2 along(std::cos(rad), std::sin(rad));
    const LLColor4 calm(0.15f, 0.2f, 0.9f, 0.9f), stream(0.1f, 0.9f, 0.9f, 0.9f), gust(1.f, 0.9f, 0.1f, 0.9f);
    const F64 reach2 = reach * reach;
    constexpr S32 STRIDE = 4;       // an arrow every 2 m
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        const WindSlabPtr s = m.mWind ? m.mWind->slab(heading, 0) : nullptr;
        if (!s || !s->mZ)
        {
            continue;
        }
        for (S32 y = STRIDE / 2; y < s->mHeight; y += STRIDE)
        {
            for (S32 x = STRIDE / 2; x < s->mWidth; x += STRIDE)
            {
                const S32 i = x + y * s->mWidth;
                const WindCell& c = s->mCells[i];
                if (c.mFlags & (WindCell::BLOCKED | WindCell::UNSEEN))
                {
                    continue;
                }
                const LLVector3d at = m.mOrigin + LLVector3d(s->mOrigin.mV[VX] + (x + 0.5) * s->mCell, s->mOrigin.mV[VY] + (y + 0.5) * s->mCell,
                                                             (*s->mZ)[i]);
                if (dist_vec_squared2D(at, camera) > reach2)
                {
                    continue;
                }
                const F32 f = c.factor();
                LLColor4 color = f < 1.f ? lerp(calm, stream, f) : lerp(stream, gust, llmin(f - 1.f, 1.f));
                if (opaque)
                {
                    color.mV[VALPHA] = 1.f;
                }
                LLVector2 dir = along + c.deflection();
                dir.normalize();
                const F32 len = 0.3f + 0.6f * llmin(f, 2.f);
                const LLVector3 p = gAgent.getPosAgentFromGlobal(at);
                const LLVector3 d(dir.mV[VX], dir.mV[VY], 0.f), side(-dir.mV[VY], dir.mV[VX], 0.f);
                const LLVector3 tail = p - d * (len * 0.5f), tip = p + d * (len * 0.5f);
                geom.line(tail, tip, color);
                geom.line(tip, tip - d * 0.25f + side * 0.15f, color);
                geom.line(tip, tip - d * 0.25f - side * 0.15f, color);
            }
        }
    }
}

// ---------------------------------------------------------------- the scheduler

std::vector<std::pair<LLVector3d, LLVector3d>> SKNavRooms::lifts(const LLVector3d& min, const LLVector3d& max)
{
    // Elevator shafts from the census: each is a space of its own.
    std::vector<std::pair<LLVector3d, LLVector3d>> out;
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        for (const Feature& f : snap->mFeatures)
        {
            if (f.mKind == FeatureKind::ELEVATOR && f.mMax.mdV[VX] >= min.mdV[VX] && f.mMin.mdV[VX] <= max.mdV[VX] &&
                f.mMax.mdV[VY] >= min.mdV[VY] && f.mMin.mdV[VY] <= max.mdV[VY])
            {
                out.emplace_back(f.mMin, f.mMax);
            }
        }
    }
    return out;
}

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
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - update");
    static LLCachedControl<U32> settle_ms(gSavedSettings, "SKNavRoomsSettleMs", 500);
    const F64 now = LLTimer::getTotalSeconds();

    // A publish moves the navmesh's revision; only then can a mesh's air have moved.
    if (navmesh.mRevision != mSeenRevision || navmesh.mFeatureStamp != mSeenFeatures)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - revision walk");
        mSeenRevision = navmesh.mRevision;
        mSeenFeatures = navmesh.mFeatureStamp;
        std::map<LLUUID, bool> seen;
        for (const auto& entry : navmesh.mMeshes)
        {
            const SKNavMesh::Mesh& mesh = *entry.second;
            U64 sig = SEED;
            U32 evicted = 0;
            LLVector3d lo(1e300, 1e300, 0.0), hi(-1e300, -1e300, 0.0);
            for (const auto& t : mesh.mTiles)
            {
                if (!t.second.mAir)
                {
                    evicted += t.second.mAirCoarse ? 1 : 0;
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
            // The doors and lifts over it cut its spaces too.
            U64 doors = SEED;
            if (sig != SEED)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - doors and lifts");
                const LLVector3d dlo(lo.mdV[VX], lo.mdV[VY], -1e9), dhi(hi.mdV[VX], hi.mdV[VY], 1e9);
                navmesh.doorsIn(dlo, dhi, [&doors](const DoorEntity& d) {
                    doors = mix(mix(mixF64(mixF64(mix(doors, d.mId.getCRC32()), d.mPlanePoint.mdV[VX]), d.mPlanePoint.mdV[VY]), d.mPlaneKnown),
                                static_cast<U64>(d.mWidth * 100.f));
                });
                for (const auto& lift : lifts(dlo, dhi))
                {
                    doors = mixF64(mixF64(doors, lift.first.mdV[VX]), lift.second.mdV[VY]);
                }
            }
            // Slabs are built over detailed air: when the cache evicts some, they go with it.
            if (evicted > state.mEvicted && state.mWind)
            {
                state.mWind.reset();
                state.mWindDirty = true;
                mViewDirty = true;
            }
            state.mEvicted = evicted;
            if (state.mAirSignature != sig || state.mDoorSignature != doors)
            {
                state.mAirSignature = sig;
                state.mDoorSignature = doors;
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
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - post");
            if (state.mAirSignature == SEED)
            {
                // Its air is gone: nothing to answer with.
                state.mDirty = false;
                if (state.mProduct)
                {
                    state.mProduct.reset();
                    state.mRooms.reset();
                    state.mWind.reset();
                    state.mWindDirty = false;
                    mViewDirty = true;
                }
                continue;
            }
            post(state, navmesh);
        }
    }

    // Wind follows the rooms: a mesh whose spaces landed builds its slabs next.
    static LLCachedControl<U32> wind_settle_ms(gSavedSettings, "SKNavWindSettleMs", 3000);
    for (auto& entry : mMeshes)
    {
        MeshState& state = entry.second;
        // A mesh still streaming in lands its rooms again and again: wait for it to be quiet.
        const bool quiet = !state.mDirty && !state.mInFlight && (now - state.mWindChanged) * 1000.0 >= wind_settle_ms;
        if (quiet && state.mWindDirty && !state.mWindInFlight && state.mRooms && state.mProduct)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - post wind");
            postWind(state);
        }
    }

    // The view's sky map is the copy the last pass was handed, not one per publish.
    if (mViewDirty)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknr - rebuild view");
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
        else if (!t.second.mAir && t.second.mAirCoarse && state.mProduct)
        {
            // Evicted: answer from the coarse product, which lets the detail go.
            if (const TileEnclosure* kept = state.mProduct->tile(t.first))
            {
                input->mKept.push_back(*kept);
                input->mKept.back().mAir.reset();
                input->mKept.back().mGaps.clear();
            }
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
    // Doors and lifts copied by value: the worker never reads the live tables.
    auto rooms = std::make_shared<RoomsInput>();
    rooms->mGeneration = state.mGeneration;
    rooms->mMeshKey = mix(SEED, state.mId.getCRC32());
    rooms->mMeshOrigin = mesh.mOrigin;
    rooms->mPrevious = state.mRooms;
    rooms->mParams.mWalkHeadM = input->mParams.mWalkHeadM;
    rooms->mParams.mAgentRadiusM = build.mParams[static_cast<S32>(sknav::AgentClass::AVATAR)].mRadius;
    const LLVector3d dlo(state.mMin.mdV[VX], state.mMin.mdV[VY], -1e9), dhi(state.mMax.mdV[VX], state.mMax.mdV[VY], 1e9);
    navmesh.doorsIn(dlo, dhi, [&rooms](const DoorEntity& d) { rooms->mDoors.push_back(d); });
    rooms->mLifts = lifts(dlo, dhi);
    job->mRoomsInput = rooms;
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
    auto step = [&job]() {
        if (!job->mPass->done())
        {
            job->mPass->step(CHUNK_STEP);
            return false;
        }
        if (!job->mSpaces)
        {
            job->mSpaces = std::make_unique<SpacesPass>(job->mRoomsInput, job->mPass->takeProduct());
        }
        return job->mSpaces->step(CHUNK_STEP);
    };
    while (!step())
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
    if (job->mCancelled || state.mGeneration != job->mGeneration || !job->mSpaces || !job->mSpaces->done())
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
    mLastSpacesMs = job->mSpaces->product()->mMillis;
    for (S32 s = 0; s < SpacesPass::NUM_STAGES; ++s)
    {
        mLastSpaceStageMs[s] = job->mSpaces->stageMillis(s);
    }
    const bool moved = !state.mProduct || state.mProduct->mSummary != product->mSummary;
    state.mProduct = product;
    state.mRooms = job->mSpaces->product();
    // Only answers that moved, or none yet, are worth a new set of slabs.
    if (moved || !state.mWind)
    {
        state.mWindDirty = true;
        state.mWindChanged = LLTimer::getTotalSeconds();
    }
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

void SKNavRooms::postWind(MeshState& state)
{
    if (!mThread)
    {
        return;
    }
    auto input = std::make_shared<WindInput>();
    input->mGeneration = state.mGeneration;
    input->mEnclosure = state.mProduct;
    input->mRooms = state.mRooms;
    input->mBands = defaultWindBands();
    auto job = std::make_shared<WindJob>();
    job->mId = state.mId;
    job->mGeneration = state.mGeneration;
    job->mLatest = state.mLatest;
    job->mPass = std::make_unique<WindPass>(input);
    std::weak_ptr<bool> alive = mAlive;
    const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
    const bool posted = queue && queue->post([this, job, alive]() {
        if (!alive.expired())
        {
            runWind(job, alive);
        }
    });
    if (posted)
    {
        state.mWindDirty = false;
        state.mWindInFlight = true;
    }
}

void SKNavRooms::runWind(const std::shared_ptr<WindJob>& job, const std::weak_ptr<bool>& alive)
{
    // On the rooms thread: a band's field or one slab per step, yielding between chunks like the passes.
    auto land = [this, job, alive]() {
        LL::WorkQueue::postMaybe(LL::WorkQueue::getInstance("mainloop"), [this, job, alive]() {
            if (!alive.expired())
            {
                finishWind(job);
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
    while (!job->mPass->step())
    {
        if (std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count() >= CHUNK_MS)
        {
            const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
            const bool posted = queue && queue->post([this, job, alive]() {
                if (!alive.expired())
                {
                    runWind(job, alive);
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

void SKNavRooms::finishWind(const std::shared_ptr<WindJob>& job)
{
    const auto it = mMeshes.find(job->mId);
    if (it == mMeshes.end() || it->second.mLatest != job->mLatest)
    {
        return;
    }
    MeshState& state = it->second;
    state.mWindInFlight = false;
    if (job->mCancelled || state.mGeneration != job->mGeneration || !job->mPass->done())
    {
        return;     // its air moved: the rooms land again, and the wind follows them
    }
    const WindProductPtr product = job->mPass->product();
    ++mWindPasses;
    mLastWindMs = product->mMillis;
    mLastWindBytes = product->bytes();
    state.mWind = product;
    mViewDirty = true;
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
            view->mMeshes.push_back({ s.mId, s.mOrigin, s.mMin, s.mMax, s.mProduct, s.mRooms, s.mWind });
        }
    }
    mView = view;
    mWind = std::make_shared<SKNavWind>(view);
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
    U32 spaces = 0, portals = 0;
    for (const auto& entry : mMeshes)
    {
        if (entry.second.mRooms)
        {
            spaces += static_cast<U32>(entry.second.mRooms->mSpaces.size());
            portals += static_cast<U32>(entry.second.mRooms->mPortals.size());
        }
    }
    std::string space_stages;
    for (S32 s = 0; s < SpacesPass::NUM_STAGES; ++s)
    {
        space_stages += llformat("%s%s %.0f", s ? ", " : "", SpacesPass::stageName(s), mLastSpaceStageMs[s]);
    }
    U32 winds = 0;
    size_t wind_bytes = 0;
    for (const auto& entry : mMeshes)
    {
        if (entry.second.mWind)
        {
            ++winds;
            wind_bytes += entry.second.mWind->bytes();
        }
    }
    return llformat("enclosure: %u meshes, %u passes (%u dropped, %u running), last %.0f ms on the thread (%s), %.0f ms posted to landed, unknown %.1f%% and sealed %.1f%% of %llu gaps; %u spaces and %u portals, last %.0f ms (%s); wind slabs over %u meshes, %.1f MB, %u builds, last %.0f ms for %.1f MB",
                    meshes, mPasses, mCancelled, in_flight, mLastMs, stages.c_str(), mLastWallMs,
                    gaps ? 100.0 * unknown / gaps : 0.0, gaps ? 100.0 * sealed / gaps : 0.0, static_cast<unsigned long long>(gaps), spaces,
                    portals, mLastSpacesMs, space_stages.c_str(), winds, wind_bytes / 1048576.0, mWindPasses, mLastWindMs,
                    mLastWindBytes / 1048576.0);
}

void SKNavRooms::gatherOverlay(SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach, U32 mode, bool opaque) const
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rooms - gather overlay");
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
                            const EnclosureBucket b = static_cast<EnclosureBucket>(g.mBucket);
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

void SKNavRooms::gatherSpaces(SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach, bool opaque,
                              const PortalCrossedFn& crossed) const
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rooms - gather spaces");
    if (!mView)
    {
        return;
    }
    const F64 reach2 = reach * reach;
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        if (!m.mRooms || !m.mProduct || !m.mRooms->mDraw)
        {
            continue;
        }
        // The draw was traced once with the spaces pass; culling it to the
        // camera and emitting what it sees is all a gather does.
        const SpacesDraw& draw = *m.mRooms->mDraw;
        for (const SpacesDraw::Fill& f : draw.mFills)
        {
            const LLVector3d centre = m.mOrigin + LLVector3d(f.mX + f.mSide * 0.5, f.mY + f.mSide * 0.5, 0.0);
            if (dist_vec_squared2D(centre, camera) > reach2)
            {
                continue;
            }
            LLColor4 color = draw.mSpaces[f.mSpace].mColor;
            color.mV[VALPHA] = opaque ? 1.f : 0.45f;
            const LLVector3 a = gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(f.mX, f.mY, f.mZ + 0.06));
            geom.tri(a, a + LLVector3(f.mSide, 0.f, 0.f), a + LLVector3(f.mSide, f.mSide, 0.f), color);
            geom.tri(a, a + LLVector3(f.mSide, f.mSide, 0.f), a + LLVector3(0.f, f.mSide, 0.f), color);
        }
        // The outline rides over its fill a little, so the space reads as one
        // polygon; the lift's phase differs from a neighbour's, so a border
        // two spaces share does not flicker between them.
        for (const SpaceDraw& s : draw.mSpaces)
        {
            LLColor4 color = s.mColor;
            color.mV[VALPHA] = opaque ? 1.f : 0.9f;
            for (const PlanEdge& e : s.mOutline)
            {
                const LLVector3d mid = m.mOrigin + LLVector3d((e.mX0 + e.mX1) * 0.5, (e.mY0 + e.mY1) * 0.5, 0.0);
                if (dist_vec_squared2D(mid, camera) > reach2)
                {
                    continue;
                }
                const LLVector3 a = gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(e.mX0, e.mY0, e.mZ + s.mLift));
                const LLVector3 b = gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(e.mX1, e.mY1, e.mZ + s.mLift));
                geom.edge(a, b, color);
            }
        }
        for (const Portal& p : m.mRooms->mPortals)
        {
            const LLVector3d a = m.mOrigin + LLVector3d(p.mA) + LLVector3d(0.0, 0.0, 0.15);
            const LLVector3d b = m.mOrigin + LLVector3d(p.mB) + LLVector3d(0.0, 0.0, 0.15);
            if (dist_vec_squared2D((a + b) * 0.5, camera) > reach2)
            {
                continue;
            }
            LLColor4 color;
            if (crossed)
            {
                // A portal crossed out by an annotation: greyed, with the
                // strike the annotator draws over it on top.
                const std::vector<U32>& ids = m.mProduct->mSpaceIds;
                if (!ids.empty() && p.mSpaces[0] < ids.size() && p.mSpaces[1] < ids.size()
                    && crossed(m.mId, ids[p.mSpaces[0]], ids[p.mSpaces[1]], (a + b) * 0.5 - LLVector3d(0.0, 0.0, 0.15)))
                {
                    color = LLColor4(0.35f, 0.35f, 0.35f, opaque ? 1.f : 0.3f);
                    const LLVector3 pa = gAgent.getPosAgentFromGlobal(a), pb = gAgent.getPosAgentFromGlobal(b);
                    geom.edge(pa, pb, color);
                    continue;
                }
            }
            const SKNavSwatch swatch = p.mKind == PortalKind::DOOR
                                           ? SKNavSwatch::PORTAL_DOOR
                                           : (p.mKind == PortalKind::STAIR ? SKNavSwatch::PORTAL_STAIR : SKNavSwatch::PORTAL_OPENING);
            color = skNavSwatchColor(swatch);
            if (opaque)
            {
                color.mV[VALPHA] = 1.f;
            }
            const LLVector3 pa = gAgent.getPosAgentFromGlobal(a), pb = gAgent.getPosAgentFromGlobal(b);
            if (p.mKind != PortalKind::OPENING || p.lintel())
            {
                geom.edge(pa, pb, color);
                continue;
            }
            // Dashed: an opening with nothing over it.
            const F32 len = dist_vec(pa, pb);
            const LLVector3 dir = len > 0.f ? (pb - pa) / len : LLVector3::zero;
            for (F32 d = 0.f; d < len; d += 0.5f)
            {
                geom.edge(pa + dir * d, pa + dir * llmin(d + 0.3f, len), color);
            }
        }
        // A flight drawn as one staircase: rails either side of the run between
        // its ends with treads between them, the portals standing as bars.
        // A lone end with nothing at the other draws a stub over itself.
        for (const StairLink& l : stairLinks(*m.mRooms))
        {
            const LLVector3d ca = m.mOrigin + LLVector3d(m.mRooms->mPortals[l.mFrom].mCentre) + LLVector3d(0.0, 0.0, 0.3);
            const LLVector3d cb = m.mOrigin + LLVector3d(m.mRooms->mPortals[l.mTo].mCentre) + LLVector3d(0.0, 0.0, 0.3);
            if (dist_vec_squared2D((ca + cb) * 0.5, camera) > reach2)
            {
                continue;
            }
            LLColor4 color = skNavSwatchColor(SKNavSwatch::PORTAL_LINK);
            if (opaque)
            {
                color.mV[VALPHA] = 1.f;
            }
            const F32 width = llclamp(llmin(m.mRooms->mPortals[l.mFrom].mWidth, m.mRooms->mPortals[l.mTo].mWidth), 0.8f, 2.4f);
            std::vector<LLVector3> path;
            path.push_back(gAgent.getPosAgentFromGlobal(ca));
            path.push_back(gAgent.getPosAgentFromGlobal(cb));
            skDrawStairGlyph(geom, path, width, color, 0.f);
        }
        for (const Space& sp : m.mRooms->mSpaces)
        {
            if (!sp.mFlight)
            {
                continue;
            }
            U32 lone = 0;
            U32 count = 0;
            for (const U32 pi : sp.mPortals)
            {
                if (m.mRooms->mPortals[pi].mKind == PortalKind::STAIR)
                {
                    lone = pi;
                    ++count;
                }
            }
            if (count != 1)
            {
                continue;
            }
            const LLVector3d c = m.mOrigin + LLVector3d(m.mRooms->mPortals[lone].mCentre);
            if (dist_vec_squared2D(c, camera) > reach2)
            {
                continue;
            }
            LLColor4 color = skNavSwatchColor(SKNavSwatch::PORTAL_DANGLING);
            if (opaque)
            {
                color.mV[VALPHA] = 1.f;
            }
            const LLVector3 base = gAgent.getPosAgentFromGlobal(c + LLVector3d(0.0, 0.0, 0.3));
            geom.edge(base, base + LLVector3(0.f, 0.f, 1.2f), color);
        }
    }
}

void SKNavRooms::writeSpaces(std::ostream& file, const LLUUID& mesh) const
{
    if (!mView)
    {
        return;
    }
    for (const SKNavWorldView::Mesh& m : mView->meshes())
    {
        if (m.mId == mesh && m.mRooms)
        {
            writeExportSpaces(file, m.mOrigin, *m.mRooms, m.mWind.get());
            return;
        }
    }
}

bool SKNavRooms::landed(const LLUUID& mesh, EnclosureProductPtr& enclosure, RoomsProductPtr& rooms) const
{
    const auto it = mMeshes.find(mesh);
    if (it == mMeshes.end() || !it->second.mProduct || !it->second.mRooms)
    {
        return false;
    }
    enclosure = it->second.mProduct;
    rooms = it->second.mRooms;
    return true;
}

const TileEnclosure* SKNavRooms::enclosureTile(const LLUUID& mesh, const TileKey& key, const TileAirPtr& air, U64& generation) const
{
    if (!mView || !air)
    {
        return nullptr;
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
            return nullptr;
        }
        generation = m.mProduct->mGeneration;
        return t;
    }
    return nullptr;
}
