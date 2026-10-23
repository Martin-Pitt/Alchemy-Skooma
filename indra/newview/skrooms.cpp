/**
 * @file skrooms.cpp
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

#include "skrooms.h"

#include "skairdebug.h"
#include "skroomscensus.h"
#include "skroomsexport.h"
#include "skroomsfloors.h"

#include "llagent.h"
#include "llapp.h"
#include "llthread.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "pipeline.h"
#include "skaircells.h"
#include "skworldcensus.h"
#include "skworldpalette.h"
#include "skworldtrace.h"
#include "workqueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <ostream>

using namespace skworld;

namespace
{
    constexpr F64 CHUNK_MS = 20.0;          ///< a pass yields its thread this often
    constexpr U32 CHUNK_STEP = 4096;        ///< work between clock reads
    constexpr U32 MAX_RERUNS = 3;           ///< neighbour-driven re-runs per wave
    const char* const QUEUE_NAME = "SKRooms";
}

struct SKRooms::Job
{
    LLUUID mId;
    U64 mGeneration = 0;
    std::shared_ptr<std::atomic<U64>> mLatest;
    std::unique_ptr<EnclosurePass> mPass;
    std::shared_ptr<const RoomsInput> mRoomsInput;
    std::unique_ptr<SpacesPass> mSpaces;        ///< once the enclosure pass is done
    EnclosureProductPtr mPreviousProduct;       ///< the answers the landed spaces were cut from
    RoomsProductPtr mPreviousRooms;             ///< those spaces, while the doors they were cut with stand
    RoomGraphPtr mRoomGraph;                    ///< the rooms of the spaces it lands
    RoomGraphPtr mPreviousRoomGraph;            ///< the rooms of mPreviousRooms
    F32 mRoomGraphMs = 0.f;
    U64 mDoorSignature = 0;
    bool mSpacesKept = false;                   ///< the layout stood, and so did the last spaces
    std::chrono::steady_clock::time_point mPosted;
    bool mCancelled = false;
};

struct SKRooms::WindJob
{
    LLUUID mId;
    U64 mGeneration = 0;
    std::shared_ptr<std::atomic<U64>> mLatest;
    std::unique_ptr<WindPass> mPass;
    bool mCancelled = false;
};

// ---------------------------------------------------------------- the view

void SKWorldView::columnAt(const LLVector3d& xy, AirColumn& out) const
{
    LL_PROFILE_ZONE_SCOPED;
    out.mXY = xy;
    size_t n = 0;   // the vectors are reused, since the point reads call this per point
    for (const Mesh& m : mMeshes)
    {
        if (!m.mProduct || xy.mdV[VX] < m.mMin.mdV[VX] || xy.mdV[VY] < m.mMin.mdV[VY] || xy.mdV[VX] >= m.mMax.mdV[VX] ||
            xy.mdV[VY] >= m.mMax.mdV[VY])
        {
            continue;
        }
        if (n == out.mMeshes.size())
        {
            out.mMeshes.emplace_back();
        }
        AirColumn::MeshColumn& c = out.mMeshes[n];
        c.mId = m.mId;
        c.mOrigin = m.mOrigin;
        const LLVector3 local(xy - m.mOrigin);
        m.mProduct->columnAt(local.mV[VX], local.mV[VY], c.mGaps);
        n += c.mGaps.empty() ? 0 : 1;
    }
    out.mMeshes.resize(n);
}

namespace
{
    // Across meshes an answer beats UNKNOWN, then the higher floor; each read is global already.
    template <typename READ>
    const SKWorldView::AirColumn::MeshColumn* pickMesh(const SKWorldView::AirColumn& column, F64 z, Enclosure& out, READ read)
    {
        const SKWorldView::AirColumn::MeshColumn* best = nullptr;
        for (const SKWorldView::AirColumn::MeshColumn& m : column.mMeshes)
        {
            Enclosure e;
            if (!read(m.mGaps, static_cast<F32>(z - m.mOrigin.mdV[VZ]), e))
            {
                continue;
            }
            e.mFloor += static_cast<F32>(m.mOrigin.mdV[VZ]);
            e.mCeiling += e.mCeiling > 1e29f ? 0.f : static_cast<F32>(m.mOrigin.mdV[VZ]);
            const bool known = e.mBucket != EnclosureBucket::UNKNOWN, had = best && out.mBucket != EnclosureBucket::UNKNOWN;
            if (!best || (known && !had) || (known == had && e.mFloor > out.mFloor))
            {
                out = e;
                best = &m;
            }
        }
        return best;
    }
}

bool SKWorldView::AirColumn::enclosureAt(F64 z, Enclosure& out, LLUUID* mesh_out) const
{
    const MeshColumn* m = pickMesh(*this, z, out, &skworld::columnEnclosureAt);
    if (m && mesh_out)
    {
        *mesh_out = m->mId;
    }
    return m != nullptr;
}

bool SKWorldView::AirColumn::airGapAt(F64 z, SpaceId& space, Enclosure& gap) const
{
    Enclosure found;
    const MeshColumn* m = pickMesh(*this, z, found, &skworld::columnAirGapAt);
    if (!m)
    {
        return false;
    }
    gap = found;
    space.mMesh = m->mId;
    space.mId = found.mSpaceId;
    return true;
}

bool SKWorldView::enclosureAt(const LLVector3d& pos, Enclosure& out) const
{
    thread_local AirColumn column;
    columnAt(pos, column);
    return column.enclosureAt(pos.mdV[VZ], out);
}

bool SKWorldView::spaceAt(const LLVector3d& pos, SpaceId& out) const
{
    // The gap enclosureAt would answer from, and its mesh.
    thread_local AirColumn column;
    columnAt(pos, column);
    Enclosure found;
    LLUUID mesh;
    if (!column.enclosureAt(pos.mdV[VZ], found, &mesh) || !found.mSpaceId)
    {
        return false;
    }
    out.mMesh = mesh;
    out.mId = found.mSpaceId;
    return true;
}

bool SKWorldView::airGapAt(const LLVector3d& pos, SpaceId& space, Enclosure& gap) const
{
    thread_local AirColumn column;
    columnAt(pos, column);
    return column.airGapAt(pos.mdV[VZ], space, gap);
}

const Space* SKWorldView::space(const SpaceId& id, const Mesh** mesh_out) const
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

void SKWorldView::spacesIn(const LLVector3d& min, const LLVector3d& max, const SpaceFn& fn) const
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

void SKWorldView::portalsIn(const LLVector3d& min, const LLVector3d& max, const PortalFn& fn) const
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

void SKWorldView::flightsIn(const LLVector3d& min, const LLVector3d& max, const FlightFn& fn) const
{
    for (const Mesh& m : mMeshes)
    {
        if (!m.mRooms)
        {
            continue;
        }
        for (const AirFlight& f : m.mRooms->mFlights)
        {
            F64 x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const LLVector2& p : f.mFootprint)
            {
                x0 = llmin(x0, static_cast<F64>(p.mV[VX]));
                y0 = llmin(y0, static_cast<F64>(p.mV[VY]));
                x1 = llmax(x1, static_cast<F64>(p.mV[VX]));
                y1 = llmax(y1, static_cast<F64>(p.mV[VY]));
            }
            const LLVector3d& o = m.mOrigin;
            if (x0 + o.mdV[VX] <= max.mdV[VX] && x1 + o.mdV[VX] >= min.mdV[VX] && y0 + o.mdV[VY] <= max.mdV[VY] && y1 + o.mdV[VY] >= min.mdV[VY] &&
                f.mZMin + o.mdV[VZ] <= max.mdV[VZ] && f.mZMax + o.mdV[VZ] >= min.mdV[VZ])
            {
                fn(m, f);
            }
        }
    }
}

const AirFlight* SKWorldView::flight(const LLUUID& mesh, U32 id, const Mesh** mesh_out) const
{
    for (const Mesh& m : mMeshes)
    {
        if (m.mId == mesh && m.mRooms)
        {
            const AirFlight* f = m.mRooms->flight(id);
            if (f && mesh_out)
            {
                *mesh_out = &m;
            }
            return f;
        }
    }
    return nullptr;
}

void SKWorldView::portalsOf(const SpaceId& id, const PortalFn& fn) const
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

const Room* SKWorldView::roomOf(const SpaceId& space, const Mesh** mesh_out) const
{
    for (const Mesh& m : mMeshes)
    {
        if (m.mId == space.mMesh)
        {
            const Room* r = m.mRoomGraph ? m.mRoomGraph->roomOfId(space.mId) : nullptr;
            if (r && mesh_out)
            {
                *mesh_out = &m;
            }
            return r;
        }
    }
    return nullptr;
}

const Room* SKWorldView::roomAt(const LLVector3d& pos, RoomId* id_out, const Mesh** mesh_out) const
{
    SpaceId space;
    const Mesh* m = nullptr;
    const Room* r = spaceAt(pos, space) ? roomOf(space, &m) : nullptr;
    if (r)
    {
        if (id_out)
        {
            id_out->mMesh = m->mId;
            id_out->mId = r->mId;
        }
        if (mesh_out)
        {
            *mesh_out = m;
        }
    }
    return r;
}

const Room* SKWorldView::room(const RoomId& id, const Mesh** mesh_out) const
{
    for (const Mesh& m : mMeshes)
    {
        if (m.mId == id.mMesh)
        {
            const Room* r = m.mRoomGraph ? m.mRoomGraph->byId(id.mId) : nullptr;
            if (r && mesh_out)
            {
                *mesh_out = &m;
            }
            return r;
        }
    }
    return nullptr;
}

LLVector3d SKWorldView::roomCellOrigin(const Mesh& mesh, const Room& room) const
{
    return LLVector3d(mesh.mOrigin.mdV[VX] + room.mCellX0 * static_cast<F64>(RASTER_XY_M),
                      mesh.mOrigin.mdV[VY] + room.mCellY0 * static_cast<F64>(RASTER_XY_M), mesh.mOrigin.mdV[VZ]);
}

bool SKWorldView::topOfWorld(const LLVector3d& xy, F64& top) const
{
    return mSky && mSky->topAt(xy.mdV[VX], xy.mdV[VY], top);
}

void SKWorldView::gapsAt(const LLVector3d& xy, const std::function<void(const Enclosure&)>& fn) const
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

void SKWorldView::raster(const LLVector3d& min, const LLVector3d& max, F32 res, Channel channel, std::vector<F32>& out) const
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
                    case Channel::D: v = known && e.depthKnown() ? e.mD : std::numeric_limits<F32>::quiet_NaN(); break;
                    default: v = e.mFloor; break;
                }
                best = e.mFloor;
            });
        }
    }
}

void SKWorldView::sheet(const LLVector3d& min, const LLVector3d& max, F32 res, std::vector<SpaceId>& out) const
{
    res = llmax(res, 0.05f);
    const S32 nx = llmax(1, static_cast<S32>(std::ceil((max.mdV[VX] - min.mdV[VX]) / res)));
    const S32 ny = llmax(1, static_cast<S32>(std::ceil((max.mdV[VY] - min.mdV[VY]) / res)));
    out.assign(static_cast<size_t>(nx) * ny, SpaceId());
    std::vector<F64> best(out.size(), -std::numeric_limits<F64>::max());
    for (const Mesh& m : mMeshes)
    {
        if (!m.mProduct || !overlapsXY(m.mMin, m.mMax, min, max))
        {
            continue;
        }
        // The mesh's own reading, on the same cells in its frame.
        std::vector<SheetCell> cells;
        const LLVector3d lo = min - m.mOrigin, hi = lo + LLVector3d(nx * res, ny * res, max.mdV[VZ] - min.mdV[VZ]);
        sheetSpaces(*m.mProduct, LLVector3(lo), LLVector3(hi), res, cells);
        for (size_t i = 0; i < out.size() && i < cells.size(); ++i)
        {
            const F64 floor = cells[i].mFloor + m.mOrigin.mdV[VZ];
            if (cells[i].mHit && floor > best[i])
            {
                best[i] = floor;
                out[i].mMesh = m.mId;
                out[i].mId = cells[i].mSpace;
            }
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

void SKRooms::SettleBox::add(const LLVector3d& min, const LLVector3d& max)
{
    for (S32 k = 0; k < 2; ++k)
    {
        mMin.mdV[k] = llmin(mMin.mdV[k], min.mdV[k]);
        mMax.mdV[k] = llmax(mMax.mdV[k], max.mdV[k]);
    }
}

void SKRooms::SettleBox::add(const SettleBox& other)
{
    mAll = mAll || other.mAll;
    add(other.mMin, other.mMax);
}

bool SKRooms::SettleBox::meets(const LLVector3d& min, const LLVector3d& max) const
{
    // Past this a moved gap's sky reads zero, so its floods mostly stop moving answers; depth can carry further, but rarely does.
    constexpr F64 REACH_M = 16.0;
    return mMin.mdV[VX] - REACH_M <= max.mdV[VX] && min.mdV[VX] <= mMax.mdV[VX] + REACH_M && mMin.mdV[VY] - REACH_M <= max.mdV[VY] &&
           min.mdV[VY] <= mMax.mdV[VY] + REACH_M;
}

bool SKRooms::settlingIn(const LLVector3d& min, const LLVector3d& max) const
{
    for (const auto& m : mMeshes)
    {
        const MeshState& s = m.second;
        if (!(s.mDirty || s.mInFlight) || !overlapsXY(s.mMin, s.mMax, min, max) || s.mMin.mdV[VZ] > max.mdV[VZ] || min.mdV[VZ] > s.mMax.mdV[VZ])
        {
            continue;
        }
        // A first publication settles the whole mesh; a repair only around what moved.
        if (!s.mProduct || (s.mDirty && (s.mPending.mAll || s.mPending.meets(min, max))) ||
            (s.mInFlight && (s.mPosted.mAll || s.mPosted.meets(min, max))))
        {
            return true;
        }
    }
    return false;
}

SKWorldViewPtr SKRooms::view() const
{
    llassert(on_main_thread());
    return mView;
}

SKWindPtr SKRooms::wind() const
{
    llassert(on_main_thread());
    return mWind;
}

SKWind::SKWind(SKWorldViewPtr view)
    : mView(std::move(view))
{
}

U64 SKWind::revision() const
{
    return mView ? mView->windRevision() : 0;
}

const SKWorldView::Mesh* SKWind::meshAt(const LLVector3d& pos, S32* cell) const
{
    // The mesh whose ground is highest under the point without being above
    // it: a skybox's own floor rather than the terrain far below.
    const SKWorldView::Mesh* best = nullptr;
    F64 best_ground = 0.0;
    S32 best_cell = -1;
    if (!mView)
    {
        return nullptr;
    }
    for (const SKWorldView::Mesh& m : mView->meshes())
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

WindSlabPtr SKWind::slab(S32 heading, const LLVector3d& min, const LLVector3d& max, LLVector3d* origin) const
{
    const LLVector3d centre((min.mdV[VX] + max.mdV[VX]) * 0.5, (min.mdV[VY] + max.mdV[VY]) * 0.5, min.mdV[VZ]);
    S32 cell = -1;
    const SKWorldView::Mesh* m = meshAt(centre, &cell);
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

bool SKWind::sampleAt(const LLVector3d& pos, F32 heading_deg, Sample& out) const
{
    S32 cell = -1;
    const SKWorldView::Mesh* m = meshAt(pos, &cell);
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

void SKRooms::gatherWind(SKWorldOverlayGeom& geom, const LLVector3d& camera, F64 reach, F32 heading_deg, bool opaque) const
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - gather wind");
    if (!mView)
    {
        return;
    }
    const S32 heading = static_cast<S32>(std::lround(wrapHeading(heading_deg) / (360.f / WIND_HEADINGS))) % WIND_HEADINGS;
    const F32 rad = windHeadingDeg(heading) * DEG_TO_RAD;
    const LLVector2 along(std::cos(rad), std::sin(rad));
    const LLColor4 calm(0.15f, 0.2f, 0.9f, 0.9f), stream(0.1f, 0.9f, 0.9f, 0.9f), gust(1.f, 0.9f, 0.1f, 0.9f);
    const LLColor4 unknown(0.5f, 0.5f, 0.5f, 0.9f);   ///< calm because nothing is known of the air there
    const F64 reach2 = reach * reach;
    constexpr S32 STRIDE = 4;       // an arrow every 2 m
    for (const SKWorldView::Mesh& m : mView->meshes())
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
                LLColor4 color = (c.mFlags & WindCell::UNKNOWN) ? unknown : f < 1.f ? lerp(calm, stream, f) : lerp(stream, gust, llmin(f - 1.f, 1.f));
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

std::vector<std::pair<LLVector3d, LLVector3d>> SKRooms::lifts(const LLVector3d& min, const LLVector3d& max)
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

SKRooms::SKRooms()
    : mAlive(std::make_shared<bool>(true))
{
    // The census's vehicle clearance reads the floor under a foot from the rooms' gaps, and its file carries their parameters.
    SKWorldCensus& census = SKWorldCensus::instance();
    census.setFloorProbe([](const LLVector3d& foot, F32 reach, F32& floor) {
        const SKWorldViewPtr view = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
        if (!view)
        {
            return false;
        }
        std::vector<Enclosure> gaps;
        view->gapsAt(foot, [&gaps](const Enclosure& e) { gaps.push_back(e); });
        return pickFloor(gaps, -1e9f, reach, floor);
    });
    census.addCensusSection(enclosureParamsSection(mCensusEnclosure));
    // The air builds while anything reads it, whatever SKAirAlways says.
    SKAirCells::instance().addDemand("rooms");
}

SKRooms::~SKRooms()
{
}

void SKRooms::cleanupSingleton()
{
    mAlive.reset();
    if (mThread)
    {
        mThread->close();
        mThread.reset();
    }
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus& census = SKWorldCensus::instance();
        census.setFloorProbe({});
        census.removeCensusSection("rooms.enclosure");
    }
    if (SKAirCells::instanceExists())
    {
        SKAirCells::instance().removeDemand("rooms");
    }
}

bool SKRooms::enabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKRoomsEnabled", true);
    return enabled && SKAirCells::enabled();
}

bool SKRooms::overlayActive()
{
    if (!enabled() || !gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WORLD))
    {
        return false;
    }
    static LLCachedControl<bool> spaces(gSavedSettings, "SKRoomsOverlaySpaces", false);
    static LLCachedControl<bool> wind(gSavedSettings, "SKRoomsOverlayWind", false);
    static LLCachedControl<U32> air(gSavedSettings, "SKAirOverlay", 0);
    return spaces || wind || air == static_cast<U32>(AirDebugMode::SPACE) || air == static_cast<U32>(AirDebugMode::INDOOR);
}

bool SKRooms::touches(const MeshState& a, const MeshState& b) const
{
    // A metre of slack: meshes whose tiles only meet at an edge still hand air across it.
    return a.mMin.mdV[VX] <= b.mMax.mdV[VX] + 1.0 && b.mMin.mdV[VX] <= a.mMax.mdV[VX] + 1.0 && a.mMin.mdV[VY] <= b.mMax.mdV[VY] + 1.0 &&
           b.mMin.mdV[VY] <= a.mMax.mdV[VY] + 1.0;
}

void SKRooms::update()
{
    if (!mAlive || !enabled() || !SKAirCells::instanceExists())
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - update");
    const SKAirCells& air = SKAirCells::instance();
    const SKWorldCensus& census = SKWorldCensus::instance();
    static LLCachedControl<U32> settle_ms(gSavedSettings, "SKRoomsSettleMs", 100);
    const F64 now = LLTimer::getTotalSeconds();

    // Only the meshes whose air the air cells say moved are walked; doors and lifts, and a backstop, walk them all.
    constexpr F64 FULL_WALK_S = 30.0;
    std::set<LLUUID> drained;
    bool walk_all = false;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - drain changes");
        std::vector<SKAirCells::AirChange> changes;
        walk_all = !air.drainChanges(changes);
        for (const SKAirCells::AirChange& change : changes)
        {
            drained.insert(change.mMesh);
        }
        if (!changes.empty())
        {
            LL_DEBUGS("SKRooms") << "rooms drained " << changes.size() << " air changes over " << drained.size() << " meshes" << LL_ENDL;
        }
    }
    const bool doors_moved = census.featureStamp() != mSeenFeatures;
    const bool backstop = !walk_all && !doors_moved && mLastFullWalk >= 0.0 && now - mLastFullWalk >= FULL_WALK_S;
    walk_all = walk_all || doors_moved || backstop || mLastFullWalk < 0.0;
    std::vector<SKAirCells::MeshInfo> meshes;
    air.snapshotMeshes(meshes);
    std::vector<LLUUID> walked;
    std::map<LLUUID, bool> unwalked;
    std::map<LLUUID, std::pair<U64, U32>> before;
    if (walk_all)
    {
        mLastFullWalk = now;
        for (const SKAirCells::MeshInfo& info : meshes)
        {
            walked.push_back(info.mId);
        }
        if (backstop)
        {
            for (const auto& entry : mMeshes)
            {
                before[entry.first] = { entry.second.mGeneration, entry.second.mEvicted };
            }
        }
    }
    else
    {
        for (const SKAirCells::MeshInfo& info : meshes)
        {
            if (drained.count(info.mId))
            {
                walked.push_back(info.mId);
            }
        }
        // A mesh not walked keeps its state; a drained one the air no longer has goes as an unseen one does.
        for (const auto& entry : mMeshes)
        {
            if (!drained.count(entry.first))
            {
                unwalked[entry.first] = true;
            }
        }
    }

    // A publish moves the air's revision; only then can a mesh's air have moved.
    if (walk_all || !drained.empty())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - revision walk");
        mSeenRevision = air.revision();
        mSeenFeatures = census.featureStamp();
        std::map<LLUUID, bool> seen;
        seen.insert(unwalked.begin(), unwalked.end());
        for (const LLUUID& id : walked)
        {
            const SKAirCells::AirSnapshot mesh = air.airSnapshot(id);
            if (!mesh.mFound)
            {
                continue;
            }
            U64 sig = SEED;
            U32 evicted = 0;
            LLVector3d lo(1e300, 1e300, 0.0), hi(-1e300, -1e300, 0.0);
            std::map<TileKey, std::pair<U64, std::pair<LLVector3d, LLVector3d>>> tile_air;
            for (const SKAirCells::AirSnapshotTile& t : mesh.mTiles)
            {
                if (!t.mAir)
                {
                    evicted += t.mCoarse ? 1 : 0;
                    continue;
                }
                tile_air.emplace(t.mKey, std::make_pair(t.mAirId, std::make_pair(t.mMin, t.mMax)));
                sig = mix(mix(sig, (static_cast<U64>(static_cast<U32>(t.mKey.mX)) << 32) | static_cast<U32>(t.mKey.mY)), t.mAirId);
                lo.mdV[VX] = llmin(lo.mdV[VX], t.mMin.mdV[VX]);
                lo.mdV[VY] = llmin(lo.mdV[VY], t.mMin.mdV[VY]);
                hi.mdV[VX] = llmax(hi.mdV[VX], t.mMax.mdV[VX]);
                hi.mdV[VY] = llmax(hi.mdV[VY], t.mMax.mdV[VY]);
            }
            if (sig == SEED && !mMeshes.count(id))
            {
                continue;       // no air, and never had any
            }
            seen[id] = true;
            MeshState& state = mMeshes[id];
            state.mId = id;
            state.mOrigin = mesh.mOrigin;
            state.mPriority = mesh.mPriority;
            // The doors and lifts over it cut its spaces too.
            U64 doors = SEED;
            if (sig != SEED)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - doors and lifts");
                const LLVector3d dlo(lo.mdV[VX], lo.mdV[VY], -1e9), dhi(hi.mdV[VX], hi.mdV[VY], 1e9);
                // Every field the spaces pass reads of a door (doorSpans): kept spaces stand only while all of them do.
                census.doorsIn(dlo, dhi, [&doors](const DoorEntity& d) {
                    doors = mix(mix(doors, d.mId.getCRC32()), d.mPlaneKnown ? 1 : 0);
                    for (S32 k = 0; k < 3; ++k)
                    {
                        doors = mixF64(mixF64(mixF64(mixF64(doors, d.mMin.mdV[k]), d.mMax.mdV[k]), d.mPlanePoint.mdV[k]), d.mNormal.mV[k]);
                    }
                    doors = mixF64(mixF64(mixF64(doors, d.mWidth), d.mHeight), d.mSill);
                });
                for (const auto& lift : lifts(dlo, dhi))
                {
                    for (S32 k = 0; k < 3; ++k)
                    {
                        doors = mixF64(mixF64(doors, lift.first.mdV[k]), lift.second.mdV[k]);
                    }
                }
            }
            // Slabs are built over detailed air: when the cache evicts some, they go with it.
            if (evicted > state.mEvicted && state.mWind)
            {
                if (SKWorldTrace::tracing())
                {
                    SKWorldTrace::Line("wind.drop").mesh(state.mId).box(state.mMin, state.mMax).num("evicted", evicted);
                }
                state.mWind.reset();
                state.windStale(now);
                mViewWindDirty = true;
            }
            state.mEvicted = evicted;
            if (state.mAirSignature != sig || state.mDoorSignature != doors)
            {
                // Only the tiles whose air came, went or changed are settling; doors alone may move anything.
                bool moved = false;
                for (const auto& t : tile_air)
                {
                    const auto it = state.mTileAir.find(t.first);
                    if (it == state.mTileAir.end() || it->second.first != t.second.first)
                    {
                        state.mPending.add(t.second.second.first, t.second.second.second);
                        moved = true;
                    }
                }
                for (const auto& t : state.mTileAir)
                {
                    if (!tile_air.count(t.first))
                    {
                        state.mPending.add(t.second.second.first, t.second.second.second);
                        moved = true;
                    }
                }
                state.mPending.mAll = state.mPending.mAll || !moved;
                if (SKWorldTrace::tracing())
                {
                    const bool air = state.mAirSignature != sig, door = state.mDoorSignature != doors;
                    SKWorldTrace::Line("rooms.dirty").mesh(id).box(lo, hi).str("why", air && door ? "air+doors" : air ? "air" : "doors")
                        .flag("published", state.mProduct != nullptr).flag("in_flight", state.mInFlight).flag("was_dirty", state.mDirty)
                        .num("evicted", evicted);
                }
                state.mAirSignature = sig;
                state.mDoorSignature = doors;
                state.mMin = lo;
                state.mMax = hi;
                if (!state.mDirty)
                {
                    state.mDirtySince = now;
                }
                state.mDirty = true;
                state.mChanged = now;
                state.mReruns = 0;
                if (!state.mLatest)
                {
                    state.mLatest = std::make_shared<std::atomic<U64>>(0);
                }
                // A pass in flight is not cancelled: it lands on the air it read, and the next one mends what it landed.
                ++state.mGeneration;
            }
            state.mTileAir.swap(tile_air);
        }
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            if (!seen.count(it->first))
            {
                if (SKWorldTrace::tracing())
                {
                    SKWorldTrace::Line("rooms.drop").mesh(it->first).box(it->second.mMin, it->second.mMax);
                }
                if (it->second.mLatest)
                {
                    it->second.mLatest->store(~0ull);   // cancel anything in flight
                }
                if (it->second.mWindLatest)
                {
                    it->second.mWindLatest->store(~0ull);
                }
                it = mMeshes.erase(it);
                mViewAirDirty = true;
                mViewWindDirty = true;
            }
            else
            {
                ++it;
            }
        }
    }
    if (backstop)
    {
        // Every mesh whose air moved was drained when it moved, so the backstop should find nothing.
        U32 missed = 0;
        for (const auto& entry : mMeshes)
        {
            const auto was = before.find(entry.first);
            const bool moved = was == before.end() || was->second.first != entry.second.mGeneration
                || was->second.second != entry.second.mEvicted;
            missed += moved && !drained.count(entry.first) ? 1 : 0;
        }
        for (const auto& entry : before)
        {
            missed += !mMeshes.count(entry.first) && !drained.count(entry.first) ? 1 : 0;
        }
        if (missed)
        {
            LL_WARNS("SKRooms") << "rooms full walk found " << missed << " meshes the change list missed" << LL_ENDL;
        }
    }

    // The one rooms thread serves the agent's region first: collect what is
    // ready, then post in the air's order rather than the map's.
    std::vector<MeshState*> ready;
    for (auto& entry : mMeshes)
    {
        MeshState& state = entry.second;
        // A mesh never published runs at once; churn may only delay a refresh, and only by a bounded wait.
        if (roomsReady(state.schedule(), now, settle_ms / 1000.0))
        {
            // The tier is read fresh: the agent's region may have moved since
            // the last revision walk, and this is the frame the order matters.
            state.mPriority = air.meshPriority(state.mId);
            ready.push_back(&state);
        }
    }
    // First publications before refreshes, then the air's order, then whoever waited longest.
    std::stable_sort(ready.begin(), ready.end(), [](const MeshState* a, const MeshState* b) { return roomsBefore(a->schedule(), b->schedule()); });
    for (MeshState* statep : ready)
    {
        MeshState& state = *statep;
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - post");
            if (state.mAirSignature == SEED)
            {
                // Its air is gone: nothing to answer with.
                state.mDirty = false;
                if (state.mProduct)
                {
                    if (SKWorldTrace::tracing())
                    {
                        SKWorldTrace::Line("rooms.clear").mesh(state.mId).box(state.mMin, state.mMax);
                    }
                    state.mProduct.reset();
                    state.mRooms.reset();
                    state.mRoomGraph.reset();
                    state.mWind.reset();
                    state.mWindDirty = false;
                    mViewAirDirty = true;
                    mViewWindDirty = true;
                }
                continue;
            }
            post(state);
        }
    }

    // A better mesh that just took the thread preempts a worse one's pass
    // mid-flight: the pass's next chunk comes back cancelled, and the mesh
    // runs again once the better one has had the thread. Without this, a long
    // pass that posted first keeps its 20 ms chunks ahead of a tier the agent
    // is standing in.
    if (!ready.empty())
    {
        U8 best = 3;
        skworld::RoomsSchedule first;     // the best tier's first publication that just posted
        first.mPriority = 4;
        for (const MeshState* statep : ready)
        {
            if (statep->mInFlight)
            {
                best = llmin(best, statep->mPriority);
                if (!statep->mProduct)
                {
                    first.mPriority = llmin(first.mPriority, statep->mPriority);
                }
            }
        }
        for (auto& entry : mMeshes)
        {
            MeshState& state = entry.second;
            if (!(state.mInFlight || state.mWindInFlight) || state.mPriority <= best)
            {
                continue;
            }
            // The tier is read fresh here too: the region may have moved while
            // this mesh's pass was running.
            state.mPriority = air.meshPriority(state.mId);
            // Only a refresh gives way, and only to a better tier's first publication.
            if (roomsPreemptible(state.schedule(), first))
            {
                if (SKWorldTrace::tracing())
                {
                    SKWorldTrace::Line("rooms.preempt").mesh(state.mId).box(state.mMin, state.mMax).num("prio", state.mPriority)
                        .num("by_prio", first.mPriority);
                }
                state.mLatest->store(++state.mGeneration);
                state.mPending.add(state.mPosted);
                if (!state.mDirty)
                {
                    state.mDirtySince = now;
                }
                state.mDirty = true;
            }
            // So do slabs: only a refresh, to a better tier's first rooms; slabs a mesh has none of are never held back.
            if (state.mWindInFlight && state.mWind && first.mPriority < state.mPriority && state.mWindLatest)
            {
                if (SKWorldTrace::tracing())
                {
                    SKWorldTrace::Line("wind.preempt").mesh(state.mId).box(state.mMin, state.mMax);
                }
                state.mWindLatest->store(++state.mWindGeneration);
                state.windStale(now);
            }
        }
    }

    // Wind follows the rooms: a mesh whose spaces landed builds its slabs next, on the rooms it has.
    // Churn delays only a refresh, and only boundedly; a mesh with no slabs builds them at once.
    static LLCachedControl<U32> wind_settle_ms(gSavedSettings, "SKRoomsWindSettleMs", 1000);
    for (auto& entry : mMeshes)
    {
        MeshState& state = entry.second;
        if (state.mRooms && state.mProduct && roomsReady(state.windSchedule(), now, wind_settle_ms / 1000.0))
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - post wind");
            postWind(state);
        }
    }

    // The view's sky map is the copy the last pass was handed, not one per publish.
    if (mViewAirDirty || mViewWindDirty)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - rebuild view");
        rebuildView();
    }
}

void SKRooms::post(MeshState& state)
{
    const SKAirCells& air = SKAirCells::instance();
    const SKAirCells::AirSnapshot mesh = air.airSnapshot(state.mId);
    if (!mesh.mFound)
    {
        return;
    }
    if (!mThread)
    {
        // One thread: passes run one at a time, and never behind the tile builds.
        mThread = std::make_unique<LL::ThreadPool>(QUEUE_NAME, 1);
        mThread->start();
    }
    if (air.skyMap().version() != mSkyVersion)
    {
        mSky = std::make_shared<const SkyMap>(air.skyMap());
        mSkyVersion = air.skyMap().version();
    }

    // The pass runs on this snapshot; only a preemption cancels it.
    state.mLatest->store(state.mGeneration);
    state.mLastPosted = LLTimer::getTotalSeconds();
    auto input = std::make_shared<EnclosureInput>();
    input->mGeneration = state.mGeneration;
    input->mMeshOrigin = mesh.mOrigin;
    input->mSky = mSky;
    input->mPrevious = state.mProduct;
    // Feet need an avatar's headroom: a slab void between storeys is not a floor.
    const TileInput build = air.params();
    input->mParams.mWalkHeadM = build.mAvatar.mHeight;
    static LLCachedControl<bool> sealed(gSavedSettings, "SKRoomsSealedIndoors", true);
    input->mParams.mSealedIndoors = sealed;
    mCensusEnclosure = input->mParams;
    for (const SKAirCells::AirSnapshotTile& t : mesh.mTiles)
    {
        if (t.mAir && !t.mAir->mLeaves.empty())
        {
            input->mTiles.push_back({ t.mKey, t.mAir, t.mContributor });
        }
        else if (!t.mAir && t.mCoarse && state.mProduct)
        {
            // Evicted: answer from the coarse product, which lets the detail go.
            if (const TileEnclosure* kept = state.mProduct->tile(t.mKey))
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
    rooms->mParams.mAgentRadiusM = build.mAvatar.mRadius;
    const LLVector3d dlo(state.mMin.mdV[VX], state.mMin.mdV[VY], -1e9), dhi(state.mMax.mdV[VX], state.mMax.mdV[VY], 1e9);
    SKWorldCensus::instance().doorsIn(dlo, dhi, [&rooms](const DoorEntity& d) { rooms->mDoors.push_back(d); });
    rooms->mLifts = lifts(dlo, dhi);
    job->mRoomsInput = rooms;
    job->mDoorSignature = state.mDoorSignature;
    if (state.mRooms && state.mRoomsDoorSignature == state.mDoorSignature)
    {
        job->mPreviousProduct = state.mProduct;
        job->mPreviousRooms = state.mRooms;
        job->mPreviousRoomGraph = state.mRoomGraph;
    }
    job->mPosted = std::chrono::steady_clock::now();
    std::weak_ptr<bool> alive = mAlive;
    const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
    const bool posted = queue && queue->post([this, job, alive]() {
        if (!alive.expired() && !LLApp::isExiting())
        {
            runChunk(job, alive);
        }
    });
    if (posted)
    {
        if (SKWorldTrace::tracing())
        {
            // A first publication, a settled refresh, or a refresh the bounded wait forced through churn.
            static LLCachedControl<U32> settle_ms(gSavedSettings, "SKRoomsSettleMs", 100);
            const F64 now = state.mLastPosted;
            const char* why = !state.mProduct ? "first" : (now - state.mChanged < settle_ms / 1000.0 ? "churn" : "refresh");
            SKWorldTrace::Line("rooms.start").mesh(state.mId).box(state.mMin, state.mMax).str("why", why).num("prio", state.mPriority)
                .real("waited", now - state.mDirtySince).real("quiet", now - state.mChanged).num("gen", static_cast<S64>(state.mGeneration))
                .num("tiles", static_cast<S64>(input->mTiles.size())).num("kept", static_cast<S64>(input->mKept.size()))
                .num("reruns", state.mReruns);
        }
        state.mDirty = false;
        state.mInFlight = true;
        state.mPosted = state.mPending;
        state.mPending = SettleBox();
    }
}

void SKRooms::runChunk(const std::shared_ptr<Job>& job, const std::weak_ptr<bool>& alive)
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
        if (job->mSpacesKept)
        {
            return true;
        }
        if (!job->mSpaces)
        {
            // The air, doors and every answer the last spaces were cut from stand: so do they.
            if (job->mPreviousRooms && job->mPass->repaired() && !job->mPass->layoutMoved())
            {
                carrySpaces(*job->mPass->takeProduct(), *job->mPreviousProduct);
                job->mSpacesKept = true;
                job->mRoomGraph = job->mPreviousRoomGraph ? job->mPreviousRoomGraph : buildRoomGraph(*job->mPreviousRooms);
                return true;
            }
            job->mSpaces = std::make_unique<SpacesPass>(job->mRoomsInput, job->mPass->takeProduct());
        }
        const bool done = job->mSpaces->step(CHUNK_STEP);
        if (done)
        {
            // Rooms are a few milliseconds over the spaces, so they land in the same chunk.
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - room graph");
            const auto graph_start = std::chrono::steady_clock::now();
            job->mRoomGraph = buildRoomGraph(*job->mSpaces->product());
            job->mRoomGraphMs =
                static_cast<F32>(std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - graph_start).count());
        }
        return done;
    };
    while (!step())
    {
        if (std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count() >= CHUNK_MS)
        {
            // Yield, so a newer generation can cancel this one between chunks.
            const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
            const bool posted = queue && queue->post([this, job, alive]() {
                if (!alive.expired() && !LLApp::isExiting())
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

void SKRooms::finish(const std::shared_ptr<Job>& job)
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
        if (SKWorldTrace::tracing())
        {
            SKWorldTrace::Line("rooms.cancel").mesh(state.mId).box(state.mMin, state.mMax).str("why", "readded");
        }
        return;
    }
    state.mInFlight = false;
    if (job->mCancelled || !(job->mSpacesKept || (job->mSpaces && job->mSpaces->done())))
    {
        ++mCancelled;
        if (SKWorldTrace::tracing())
        {
            SKWorldTrace::Line("rooms.cancel").mesh(state.mId).box(state.mMin, state.mMax).str("why", "preempted");
        }
        // Preempted: the state is dirty again and posts anew, over what this one would have settled.
        state.mPending.add(state.mPosted);
        state.mPosted = SettleBox();
        return;
    }
    state.mPosted = SettleBox();
    // Air that moved while it ran left the state dirty: this lands now, and the refresh follows.

    const EnclosureProductPtr product = job->mPass->product();
    ++mPasses;
    mLastMs = product->mMillis;
    mLastWallMs = static_cast<F32>(std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - job->mPosted).count());
    for (S32 s = 0; s < EnclosurePass::NUM_STAGES; ++s)
    {
        mLastStageMs[s] = job->mPass->stageMillis(s);
    }
    mRepairs += job->mPass->repaired() ? 1 : 0;
    mSpacesKept += job->mSpacesKept ? 1 : 0;
    mLastAnswered = job->mPass->tilesAnswered();
    mLastRoomGraphMs = job->mSpacesKept ? 0.f : job->mRoomGraphMs;
    mLastSpacesMs = job->mSpacesKept ? 0.f : job->mSpaces->product()->mMillis;
    if (!job->mSpacesKept)
    {
        const FlightStats& fs = job->mSpaces->product()->mFlightStats;
        mLastFlightMs = fs.mFitMs + fs.mGrowMs + fs.mRegionsMs;
    }
    for (S32 s = 0; s < SpacesPass::NUM_STAGES; ++s)
    {
        mLastSpaceStageMs[s] = job->mSpacesKept ? 0.f : job->mSpaces->stageMillis(s);
    }
    const bool moved = !state.mProduct || state.mProduct->mSummary != product->mSummary;
    if (SKWorldTrace::tracing())
    {
        SKWorldTrace::Line("rooms.publish").mesh(state.mId).box(state.mMin, state.mMax).flag("first", !state.mProduct).flag("moved", moved)
            .flag("still_dirty", state.mDirty).num("gen", static_cast<S64>(job->mGeneration)).real("ms", mLastMs + mLastSpacesMs)
            .real("wall_ms", mLastWallMs).num("rooms", static_cast<S64>(job->mRoomGraph ? job->mRoomGraph->mRooms.size() : 0));
    }
    state.mProduct = product;
    state.mRooms = job->mSpacesKept ? job->mPreviousRooms : job->mSpaces->product();
    state.mRoomGraph = job->mRoomGraph;
    state.mRoomsDoorSignature = job->mDoorSignature;
    // Only answers that moved, or none yet, are worth a new set of slabs.
    if (moved || !state.mWind)
    {
        state.windStale(LLTimer::getTotalSeconds());
    }
    mViewAirDirty = true;
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
            if (SKWorldTrace::tracing())
            {
                SKWorldTrace::Line("rooms.rerun").mesh(o.mId).box(o.mMin, o.mMax).mesh("by", state.mId).num("reruns", o.mReruns);
            }
            // It settles where the two meet, which is where this one's answers reach it.
            LLVector3d lo, hi;
            for (S32 k = 0; k < 2; ++k)
            {
                lo.mdV[k] = llmax(state.mMin.mdV[k], o.mMin.mdV[k]) - 1.0;
                hi.mdV[k] = llmin(state.mMax.mdV[k], o.mMax.mdV[k]) + 1.0;
            }
            o.mPending.add(lo, hi);
        }
    }
}

void SKRooms::postWind(MeshState& state)
{
    if (!mThread)
    {
        return;
    }
    if (!state.mWindLatest)
    {
        state.mWindLatest = std::make_shared<std::atomic<U64>>(0);
    }
    state.mWindLatest->store(++state.mWindGeneration);
    auto input = std::make_shared<WindInput>();
    input->mGeneration = state.mGeneration;
    input->mEnclosure = state.mProduct;
    input->mRooms = state.mRooms;
    input->mBands = defaultWindBands();
    auto job = std::make_shared<WindJob>();
    job->mId = state.mId;
    job->mGeneration = state.mWindGeneration;
    job->mLatest = state.mWindLatest;
    job->mPass = std::make_unique<WindPass>(input);
    std::weak_ptr<bool> alive = mAlive;
    const LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance(QUEUE_NAME);
    const bool posted = queue && queue->post([this, job, alive]() {
        if (!alive.expired() && !LLApp::isExiting())
        {
            runWind(job, alive);
        }
    });
    if (posted)
    {
        if (SKWorldTrace::tracing())
        {
            SKWorldTrace::Line("wind.start").mesh(state.mId).box(state.mMin, state.mMax).flag("first", state.mWind == nullptr);
        }
        state.mWindDirty = false;
        state.mWindInFlight = true;
    }
}

void SKRooms::runWind(const std::shared_ptr<WindJob>& job, const std::weak_ptr<bool>& alive)
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
                if (!alive.expired() && !LLApp::isExiting())
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

void SKRooms::finishWind(const std::shared_ptr<WindJob>& job)
{
    const auto it = mMeshes.find(job->mId);
    if (it == mMeshes.end() || it->second.mWindLatest != job->mLatest)
    {
        return;
    }
    MeshState& state = it->second;
    state.mWindInFlight = false;
    if (job->mCancelled || state.mWindGeneration != job->mGeneration || !job->mPass->done())
    {
        if (SKWorldTrace::tracing())
        {
            SKWorldTrace::Line("wind.cancel").mesh(state.mId).box(state.mMin, state.mMax);
        }
        return;     // preempted: it runs again once the better tier has had the thread
    }
    // Slabs over the rooms it read land even if newer rooms have landed since: those set it stale again.
    const WindProductPtr product = job->mPass->product();
    ++mWindPasses;
    mLastWindMs = product->mMillis;
    mLastWindBytes = product->bytes();
    state.mWind = product;
    mViewWindDirty = true;
    if (SKWorldTrace::tracing())
    {
        SKWorldTrace::Line("wind.publish").mesh(state.mId).box(state.mMin, state.mMax).real("ms", product->mMillis);
    }
}


void SKRooms::rebuildView()
{
    // Each revision moves only for what changed: a wind publish leaves every air consumer's cache standing.
    auto view = std::make_shared<SKWorldView>();
    view->mAirRevision = mViewAirDirty ? ++mViewAirRevision : mViewAirRevision;
    view->mWindRevision = mViewWindDirty ? ++mViewWindRevision : mViewWindRevision;
    mViewAirDirty = false;
    mViewWindDirty = false;
    if (SKWorldTrace::tracing())
    {
        SKWorldTrace::Line("view.rebuild").num("air", static_cast<S64>(view->mAirRevision)).num("wind", static_cast<S64>(view->mWindRevision));
    }
    view->mSky = mSky;
    for (const auto& entry : mMeshes)
    {
        const MeshState& s = entry.second;
        if (s.mProduct)
        {
            view->mMeshes.push_back({ s.mId, s.mOrigin, s.mMin, s.mMax, s.mProduct, s.mRooms, s.mRoomGraph, s.mWind });
        }
    }
    mView = view;
    mWind = std::make_shared<SKWind>(view);
}

std::string SKRooms::statsLine() const
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
    U32 rooms = 0, truncated = 0;
    size_t room_bytes = 0;
    for (const auto& entry : mMeshes)
    {
        if (entry.second.mRoomGraph)
        {
            rooms += static_cast<U32>(entry.second.mRoomGraph->mRooms.size());
            room_bytes += entry.second.mRoomGraph->bytes();
            for (const Room& r : entry.second.mRoomGraph->mRooms)
            {
                truncated += r.mTruncated ? 1 : 0;
            }
        }
    }
    U32 flights[3] = {};
    F32 flight_ms = 0.f;
    for (const auto& entry : mMeshes)
    {
        if (entry.second.mRooms)
        {
            for (const AirFlight& f : entry.second.mRooms->mFlights)
            {
                ++flights[static_cast<S32>(f.mKind)];
            }
        }
    }
    flight_ms = mLastFlightMs;
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
    return llformat("enclosure: %u meshes, %u passes (%u repairs, %u keeping their spaces, %u dropped, %u running), last %.0f ms on the thread (%s, %u tiles answered), %.0f ms posted to landed, unknown %.1f%% and sealed %.1f%% of %llu gaps; %u spaces and %u portals, last %.0f ms (%s); "
                    "flights: %u stairs, %u ramps, %u climbs, last %.1f ms; rooms %u (truncated %u), %.1f KB, graph %.1f ms view air %llu wind %llu; "
                    "wind slabs over %u meshes, %.1f MB, %u builds, last %.0f ms for %.1f MB",
                    meshes, mPasses, mRepairs, mSpacesKept, mCancelled, in_flight, mLastMs, stages.c_str(), mLastAnswered, mLastWallMs,
                    gaps ? 100.0 * unknown / gaps : 0.0, gaps ? 100.0 * sealed / gaps : 0.0, static_cast<unsigned long long>(gaps), spaces,
                    portals, mLastSpacesMs, space_stages.c_str(), flights[0], flights[1], flights[2], flight_ms, rooms, truncated, room_bytes / 1024.0, mLastRoomGraphMs,
                    static_cast<unsigned long long>(mViewAirRevision), static_cast<unsigned long long>(mViewWindRevision), winds,
                    wind_bytes / 1048576.0, mWindPasses, mLastWindMs, mLastWindBytes / 1048576.0);
}

void SKRooms::gatherSpaces(SKWorldOverlayGeom& geom, const LLVector3d& camera, F64 reach, bool opaque) const
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - gather spaces");
    if (!mView)
    {
        return;
    }
    const F64 reach2 = reach * reach;
    for (const SKWorldView::Mesh& m : mView->meshes())
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
            const SKWorldSwatch swatch = p.mKind == PortalKind::DOOR
                                           ? SKWorldSwatch::PORTAL_DOOR
                                           : (p.mKind == PortalKind::STAIR ? SKWorldSwatch::PORTAL_STAIR : SKWorldSwatch::PORTAL_OPENING);
            LLColor4 color = skWorldSwatchColor(swatch);
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
            LLColor4 color = skWorldSwatchColor(SKWorldSwatch::PORTAL_LINK);
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
        // Each flight the air's cells showed: its footprint on its plane, and a line up its run from foot to head.
        for (const AirFlight& f : m.mRooms->mFlights)
        {
            const LLVector3d mid = m.mOrigin + LLVector3d((f.mBottom + f.mTop) * 0.5f);
            if (f.mFootprint.size() < 2 || dist_vec_squared2D(mid, camera) > reach2)
            {
                continue;
            }
            const SKWorldSwatch swatch = f.mKind == FlightKind::STAIR  ? SKWorldSwatch::PORTAL_STAIR
                                         : f.mKind == FlightKind::RAMP ? SKWorldSwatch::PORTAL_LINK
                                                                       : SKWorldSwatch::PORTAL_DANGLING;
            LLColor4 color = skWorldSwatchColor(swatch);
            color.mV[VALPHA] = opaque ? 1.f : 0.8f;
            const auto on_plane = [&](const LLVector2& p) {
                const F32 z = llclamp(f.mPlane[0] * p.mV[VX] + f.mPlane[1] * p.mV[VY] + f.mPlane[2], f.mZMin, f.mZMax) + 0.1f;
                return gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(p.mV[VX], p.mV[VY], z));
            };
            for (size_t k = 0; k < f.mFootprint.size(); ++k)
            {
                geom.line(on_plane(f.mFootprint[k]), on_plane(f.mFootprint[(k + 1) % f.mFootprint.size()]), color);
            }
            geom.edge(gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(f.mBottom) + LLVector3d(0.0, 0.0, 0.1)),
                      gAgent.getPosAgentFromGlobal(m.mOrigin + LLVector3d(f.mTop) + LLVector3d(0.0, 0.0, 0.1)), color);
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
            LLColor4 color = skWorldSwatchColor(SKWorldSwatch::PORTAL_DANGLING);
            if (opaque)
            {
                color.mV[VALPHA] = 1.f;
            }
            const LLVector3 base = gAgent.getPosAgentFromGlobal(c + LLVector3d(0.0, 0.0, 0.3));
            geom.edge(base, base + LLVector3(0.f, 0.f, 1.2f), color);
        }
    }
}

void SKRooms::writeSpaces(std::ostream& file, const LLUUID& mesh) const
{
    if (!mView)
    {
        return;
    }
    for (const SKWorldView::Mesh& m : mView->meshes())
    {
        if (m.mId == mesh && m.mRooms)
        {
            writeExportSpaces(file, m.mOrigin, *m.mRooms, m.mWind.get());
            return;
        }
    }
}

bool SKRooms::landed(const LLUUID& mesh, EnclosureProductPtr& enclosure, RoomsProductPtr& rooms) const
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

bool SKRooms::passPending(const LLUUID& mesh) const
{
    const auto it = mMeshes.find(mesh);
    return it != mMeshes.end() && (it->second.mDirty || it->second.mInFlight || (!it->second.mProduct && it->second.mAirSignature != SEED));
}

bool SKRooms::windPending(const LLUUID& mesh) const
{
    const auto it = mMeshes.find(mesh);
    return it != mMeshes.end() && it->second.mProduct && (it->second.mWindDirty || it->second.mWindInFlight);
}

const TileEnclosure* SKRooms::enclosureTile(const LLUUID& mesh, const TileKey& key, const TileAirPtr& air, U64& generation) const
{
    if (!mView || !air)
    {
        return nullptr;
    }
    for (const SKWorldView::Mesh& m : mView->meshes())
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

// ---------------------------------------------------------------- overlays
namespace
{
    /// Mesh-local geometry into the overlay in agent coordinates, counting vertices against a budget.
    struct RoomsAirSink : AirDebugSink
    {
        SKWorldOverlayGeom& mGeom;
        LLVector3 mOffset;
        U32 mBudget;
        U32 mUsed = 0;

        RoomsAirSink(SKWorldOverlayGeom& geom, U32 budget) : mGeom(geom), mBudget(budget) {}

        bool spend(U32 verts)
        {
            if (mUsed + verts > mBudget)
            {
                return false;
            }
            mUsed += verts;
            return true;
        }
        bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d, const LLColor4& color) override
        {
            if (!spend(6))
            {
                return false;
            }
            mGeom.tri(a + mOffset, b + mOffset, c + mOffset, color);
            mGeom.tri(a + mOffset, c + mOffset, d + mOffset, color);
            return true;
        }
        bool line(const LLVector3& a, const LLVector3& b, const LLColor4& color) override
        {
            if (!spend(2))
            {
                return false;
            }
            mGeom.line(a + mOffset, b + mOffset, color);
            return true;
        }
    };
}

void SKRooms::gatherAirModes(const LLVector3d& camera, F64 reach, U32 mode)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skrooms - gather air modes");
    static LLCachedControl<U32> budget(gSavedSettings, "SKAirOverlayBudget", 1000000);
    static LLCachedControl<F32> climb(gSavedSettings, "SKAirClimb", 0.75f);
    if (!mView)
    {
        return;
    }
    // The air overlay's space and indoor modes, over the air each answer was given for, nearest tile first.
    const AirDebugMode which = static_cast<AirDebugMode>(mode);
    struct Near
    {
        F64 mDistance2;
        const SKWorldView::Mesh* mMesh;
        const TileEnclosure* mTile;
    };
    std::vector<Near> tiles;
    const F64 reach2 = reach * reach;
    for (const SKWorldView::Mesh& m : mView->meshes())
    {
        for (const TileEnclosure& te : m.mProduct->mTiles)
        {
            if (!te.mAir || te.mGaps.size() != te.mAir->mGaps.size())
            {
                continue;
            }
            const TileAir& air = *te.mAir;
            const LLVector3d lo = m.mOrigin + LLVector3d(air.mOrigin);
            const LLVector3d hi = lo + LLVector3d(air.mWidth * air.mCell, air.mHeight * air.mCell, 0.0);
            const F64 dx = llmax(lo.mdV[VX] - camera.mdV[VX], 0.0, camera.mdV[VX] - hi.mdV[VX]);
            const F64 dy = llmax(lo.mdV[VY] - camera.mdV[VY], 0.0, camera.mdV[VY] - hi.mdV[VY]);
            if (dx * dx + dy * dy <= reach2)
            {
                tiles.push_back({ dx * dx + dy * dy, &m, &te });
            }
        }
    }
    std::sort(tiles.begin(), tiles.end(), [](const Near& a, const Near& b) { return a.mDistance2 < b.mDistance2; });

    RoomsAirSink sink(mOverlay, budget);
    std::vector<U32> gap_space;
    std::vector<LLColor4> gap_colour;
    const LLColor4& outdoors = skWorldSwatchColor(SKWorldSwatch::ENCLOSURE_OUTDOORS);
    const LLColor4& sheltered = skWorldSwatchColor(SKWorldSwatch::ENCLOSURE_SHELTERED);
    const LLColor4& indoors = skWorldSwatchColor(SKWorldSwatch::ENCLOSURE_INDOORS);
    const LLColor4& unknown = skWorldSwatchColor(SKWorldSwatch::ENCLOSURE_UNKNOWN);
    for (const Near& n : tiles)
    {
        const TileAir& air = *n.mTile->mAir;
        AirDebugInput in;
        in.mMode = which;
        in.mClimb = air.mUnit > 0.f ? static_cast<S32>(climb / air.mUnit) : 0;
        in.mCentre = LLVector3(camera - n.mMesh->mOrigin);
        in.mReach = static_cast<F32>(reach);
        if (which == AirDebugMode::SPACE)
        {
            gap_space.assign(air.mGaps.size(), 0);
            const std::vector<U32>& ids = n.mMesh->mProduct->mSpaceIds;
            for (size_t g = 0; g < gap_space.size(); ++g)
            {
                const U16 space = n.mTile->mGaps[g].mSpace;
                gap_space[g] = space && space <= ids.size() ? ids[space - 1] : 0;
            }
            in.mGapSpace = gap_space.data();
        }
        else
        {
            // Blue outdoors through amber sheltered to red indoors by e, grey where nothing answered.
            gap_colour.assign(air.mGaps.size(), unknown);
            for (size_t g = 0; g < gap_colour.size(); ++g)
            {
                const GapEnclosure& ge = n.mTile->mGaps[g];
                if (ge.mD != ENCLOSURE_D_UNKNOWN)
                {
                    const F32 e = ge.mE / 255.f;
                    gap_colour[g] = e < 0.5f ? lerp(outdoors, sheltered, e * 2.f) : lerp(sheltered, indoors, e * 2.f - 1.f);
                }
            }
            in.mGapColour = gap_colour.data();
        }
        sink.mOffset = gAgent.getPosAgentFromGlobal(n.mMesh->mOrigin);
        if (!gatherAirDebug(air, in, sink))
        {
            break;
        }
    }
}

void SKRooms::refreshOverlay()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKWorldOverlayRange", 96.f);
    static LLCachedControl<bool> show_spaces(gSavedSettings, "SKRoomsOverlaySpaces", false);
    static LLCachedControl<bool> show_wind(gSavedSettings, "SKRoomsOverlayWind", false);
    static LLCachedControl<F32> wind_heading(gSavedSettings, "SKRoomsOverlayWindHeading", -1.f);
    static LLCachedControl<U32> show_air(gSavedSettings, "SKAirOverlay", 0);
    const U32 air_mode = show_air == static_cast<U32>(AirDebugMode::SPACE) || show_air == static_cast<U32>(AirDebugMode::INDOOR) ? (U32)show_air : 0;

    // Drawn around the camera alone, its last fifth falling away so the edge reads as distance.
    LLViewerCamera* view = LLViewerCamera::getInstance();
    const LLVector3 camera_agent = view->getOrigin();
    const LLVector3 camera_forward = view->getAtAxis();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(camera_agent);
    const F64 reach = range;
    const F32 fov_y = view->getView();
    const S32 viewport_height = gViewerWindow ? gViewerWindow->getWorldViewHeightRaw() : 0;

    // The wind layer shows one heading: the setting's, or the region's wind where the camera is.
    F32 heading = wind_heading;
    if (show_wind && heading < 0.f)
    {
        heading = 0.f;
        if (LLViewerRegion* region = gAgent.getRegion())
        {
            const LLVector3 v = region->mWind.getVelocity(region->getPosRegionFromGlobal(camera));
            heading = v.lengthSquared() > 1e-6f ? std::atan2(v.mV[VY], v.mV[VX]) * RAD_TO_DEG : 0.f;
        }
    }
    const S32 wind_bucket = static_cast<S32>(std::lround(heading / (360.f / WIND_HEADINGS)));

    U64 key = SEED;
    key = mix(key, (show_spaces ? 1ull : 0ull) | (show_wind ? 2ull : 0ull));
    key = mix(key, static_cast<U64>(air_mode));
    key = mix(key, show_spaces || air_mode ? airRevision() : 0ull);
    key = mix(key, show_wind ? windRevision() : 0ull);
    key = mix(key, show_wind ? static_cast<U64>(wind_bucket + 1000) : 0ull);
    key = mixF32(key, static_cast<F32>(reach));
    key = mixF32(key, fov_y);
    key = mix(key, static_cast<U64>(viewport_height));

    // The camera may move a metre and a half or turn ten degrees before the overlay is gathered again.
    static const F32 MOVE_M = 1.5f;
    static const F32 TURN_DOT = 0.985f;
    const bool rebuild = !mOverlayFresh || key != mOverlayKey || dist_vec_squared(camera_agent, mOverlayCamera) > MOVE_M * MOVE_M
        || camera_forward * mOverlayForward < TURN_DOT;
    if (!rebuild)
    {
        return;
    }
    mOverlay.clear();
    mOverlay.setEdgeWidth(3.f, camera_agent, fov_y, viewport_height);
    mOverlay.setFade(camera_agent, static_cast<F32>(reach), static_cast<F32>(reach * 0.8));
    if (show_spaces)
    {
        gatherSpaces(mOverlay, camera, reach, false);
    }
    if (air_mode)
    {
        gatherAirModes(camera, reach, air_mode);
    }
    if (show_wind)
    {
        gatherWind(mOverlay, camera, reach, wind_bucket * (360.f / WIND_HEADINGS), false);
    }
    mOverlay.upload();
    mOverlayFresh = true;
    mOverlayKey = key;
    mOverlayCamera = camera_agent;
    mOverlayForward = camera_forward;
}

void SKRooms::renderDebug()
{
    if (!overlayActive())
    {
        if (mOverlayFresh)
        {
            mOverlay.clear();
            mOverlayFresh = false;
        }
        return;
    }
    refreshOverlay();
    if (mOverlay.empty())
    {
        return;
    }
    SKWorldOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        mOverlay.draw();
    }
    SKWorldOverlayGeom::unbindShader();
}
