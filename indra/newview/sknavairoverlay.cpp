/**
 * @file sknavairoverlay.cpp
 * @brief The navmesh's air overlay: leaves, gaps, spaces, stamp, clearance and face offsets as floors and edges.
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

#include "sknavmesh.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "sknavairdebug.h"
#include "sknavairdistil.h"
#include "sknavenclosure.h"
#include "sknavpolys.h"
#include "sknavrooms.h"

#include <algorithm>

using namespace sknav;

namespace
{
    /// Mesh-local geometry into the overlay in agent coordinates, counting vertices against a budget.
    struct AirSink : AirDebugSink
    {
        SKNavOverlayGeom& mGeom;
        LLVector3 mOffset;
        U32 mBudget;
        U32 mUsed = 0;
        bool mOpaque;

        AirSink(SKNavOverlayGeom& geom, U32 budget, bool opaque) : mGeom(geom), mBudget(budget), mOpaque(opaque) {}

        LLColor4 shade(LLColor4 c) const
        {
            if (mOpaque)
            {
                c.mV[VALPHA] = 1.f;
            }
            return c;
        }
        bool spend(U32 verts)
        {
            if (mUsed + verts > mBudget)
            {
                return false;
            }
            mUsed += verts;
            return true;
        }
        bool tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color)
        {
            if (!spend(3))
            {
                return false;
            }
            mGeom.tri(a + mOffset, b + mOffset, c + mOffset, shade(color));
            return true;
        }
        bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d, const LLColor4& color) override
        {
            if (!spend(6))
            {
                return false;
            }
            const LLColor4 s = shade(color);
            mGeom.tri(a + mOffset, b + mOffset, c + mOffset, s);
            mGeom.tri(a + mOffset, c + mOffset, d + mOffset, s);
            return true;
        }
        bool line(const LLVector3& a, const LLVector3& b, const LLColor4& color) override
        {
            if (!spend(2))
            {
                return false;
            }
            mGeom.line(a + mOffset, b + mOffset, shade(color));
            return true;
        }
    };

    F64 planDistance2(const LLVector3d& p, const LLVector3d& min, const LLVector3d& max)
    {
        const F64 dx = llmax(min.mdV[VX] - p.mdV[VX], 0.0, p.mdV[VX] - max.mdV[VX]);
        const F64 dy = llmax(min.mdV[VY] - p.mdV[VY], 0.0, p.mdV[VY] - max.mdV[VY]);
        return dx * dx + dy * dy;
    }
}

void SKNavMesh::gatherAirOverlay(const LLVector3d& camera, F64 reach, U32 mode, bool opaque)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - gather air overlay");
    static LLCachedControl<U32> source(gSavedSettings, "SKNavOverlayAirSource", 0);
    static LLCachedControl<F32> slice(gSavedSettings, "SKNavOverlayAirSlice", 0.f);
    static LLCachedControl<bool> slice_camera(gSavedSettings, "SKNavOverlayAirSliceCamera", false);
    static LLCachedControl<U32> budget(gSavedSettings, "SKNavOverlayAirBudget", 1000000);

    mStats.mAirOverlayVerts = 0;
    mStats.mAirOverlayTiles = 0;
    mStats.mAirOverlayCut = 0;
    const AirDebugMode which = static_cast<AirDebugMode>(llmin(mode, static_cast<U32>(AirDebugMode::COUNT) - 1));

    struct Near
    {
        F64 mDistance2;
        const Mesh* mMesh;
        TileKey mKey;
        const Tile* mTile;
        bool mCoarse;
    };
    std::vector<Near> tiles;
    const F64 reach2 = reach * reach;
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        for (const auto& t : mesh.mTiles)
        {
            const Tile& tile = t.second;
            if (!tile.mAir)
            {
                continue;
            }
            // Air built for the tile is its detail; air with no build signature is the coarse air a far tile reads.
            const bool coarse = tile.mAirBuilt == 0;
            if ((source == 1 && coarse) || (source == 2 && !coarse))
            {
                continue;
            }
            const F64 d2 = planDistance2(camera, tile.mMin, tile.mMax);
            if (d2 <= reach2)
            {
                tiles.push_back({ d2, &mesh, t.first, &tile, coarse });
            }
        }
    }
    std::sort(tiles.begin(), tiles.end(), [](const Near& a, const Near& b) { return a.mDistance2 < b.mDistance2; });

    // The floor the slice is centred on: under the camera, or under the agent's feet.
    F64 slice_z = 0.0;
    const bool slicing = slice > 0.f;
    if (slicing)
    {
        LLVector3d at = camera;
        if (!slice_camera)
        {
            at = gAgent.getPositionGlobal();
            at.mdV[VZ] -= isAgentAvatarValid() ? gAgentAvatarp->getPelvisToFoot() : 1.0;
        }
        slice_z = at.mdV[VZ];
        bool found = false;
        for (const Near& n : tiles)
        {
            if (planDistance2(at, n.mTile->mMin, n.mTile->mMax) > 0.0)
            {
                continue;
            }
            F32 floor = 0.f;
            if (airFloorUnder(*n.mTile->mAir, LLVector3(at - n.mMesh->mOrigin), floor))
            {
                const F64 z = n.mMesh->mOrigin.mdV[VZ] + floor;
                slice_z = found ? llmax(slice_z, z) : z;
                found = true;
            }
        }
    }

    AirSink sink(mOverlay, budget, opaque);

    if (which == AirDebugMode::STAMP)
    {
        // The stamped polygons by the space they carry, to set against the gap floors' spaces.
        struct Polys
        {
            F64 mDistance2;
            PolyTile mTile;
        };
        std::vector<Polys> found;
        const LLVector3d span(reach, reach, 0.0);
        LLVector3d lo = camera - span, hi = camera + span;
        lo.mdV[VZ] = -1e9;
        hi.mdV[VZ] = 1e9;
        polysIn(lo, hi, AgentClass::AVATAR, [&](const PolyTile& pt) {
            const F64 d2 = planDistance2(camera, pt.mMin, pt.mMax);
            if (d2 <= reach2 && pt.mPolys && pt.mPolySpace)
            {
                found.push_back({ d2, pt });
            }
        });
        std::sort(found.begin(), found.end(), [](const Polys& a, const Polys& b) { return a.mDistance2 < b.mDistance2; });
        for (size_t i = 0; i < found.size(); ++i)
        {
            const PolyTile& pt = found[i].mTile;
            const TilePolys& polys = *pt.mPolys;
            const std::vector<U32>& spaces = *pt.mPolySpace;
            sink.mOffset = gAgent.getPosAgentFromGlobal(pt.mOrigin);
            const F32 lo_z = static_cast<F32>(slice_z - pt.mOrigin.mdV[VZ] - slice);
            const F32 hi_z = static_cast<F32>(slice_z - pt.mOrigin.mdV[VZ] + slice);
            bool ok = true;
            for (size_t p = 0; ok && p < polys.mPolys.size() && p < spaces.size(); ++p)
            {
                const TilePolys::Poly& poly = polys.mPolys[p];
                if (slicing && (poly.mZMax < lo_z || poly.mZMin > hi_z))
                {
                    continue;
                }
                const U32 id = spaces[p];
                LLColor4 fill = spaceColour(id);
                fill.mV[VALPHA] = id ? 0.55f : 0.25f;
                const LLVector3 lift(0.f, 0.f, 0.04f);
                for (U32 t = 0; ok && t < poly.mTriCount; ++t)
                {
                    const LLVector3* c = &polys.mTris[(poly.mFirstTri + t) * 3];
                    ok = sink.tri(c[0] + lift, c[1] + lift, c[2] + lift, fill);
                }
                // Edges only where the space changes, so each stamped space reads as one shape.
                for (U32 v = 0; ok && v < poly.mVertCount; ++v)
                {
                    const S16 nei = poly.mNeighbour[v];
                    if (nei >= 0 && static_cast<size_t>(nei) < spaces.size() && spaces[nei] == id)
                    {
                        continue;
                    }
                    if (nei == TilePolys::EDGE_BORDER)
                    {
                        continue;
                    }
                    LLColor4 edge = spaceColour(id);
                    edge.mV[VALPHA] = 0.95f;
                    const LLVector3 a = polys.mVerts[poly.mVerts[v]] + lift, b = polys.mVerts[poly.mVerts[(v + 1) % poly.mVertCount]] + lift;
                    ok = sink.line(a, b, edge);
                }
            }
            ++mStats.mAirOverlayTiles;
            if (!ok)
            {
                mStats.mAirOverlayCut = static_cast<U32>(found.size() - i);
                break;
            }
        }
        mStats.mAirOverlayVerts = sink.mUsed;
        return;
    }

    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    const LLColor4 far_frame(0.65f, 0.45f, 1.f, opaque ? 1.f : 0.6f);
    std::vector<U32> gap_space;
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const Near& n = tiles[i];
        const TileAir& air = *n.mTile->mAir;
        AirDebugInput in;
        in.mMode = which;
        in.mCoarse = n.mCoarse;
        in.mClimb = air.mUnit > 0.f ? static_cast<S32>(climb / air.mUnit) : 0;
        in.mCentre = LLVector3(camera - n.mMesh->mOrigin);
        in.mReach = static_cast<F32>(reach);
        if (slicing)
        {
            in.mSliceLo = static_cast<F32>(slice_z - n.mMesh->mOrigin.mdV[VZ] - slice);
            in.mSliceHi = static_cast<F32>(slice_z - n.mMesh->mOrigin.mdV[VZ] + slice);
        }
        if (which == AirDebugMode::SPACE)
        {
            // Only answers for exactly this air: the gaps index both.
            gap_space.assign(air.mGaps.size(), 0);
            EnclosureProductPtr enc;
            RoomsProductPtr rooms;
            const TileEnclosure* te = mRooms->landed(n.mMesh->mId, enc, rooms) ? enc->tile(n.mKey) : nullptr;
            if (te && te->mAir == n.mTile->mAir && te->mGaps.size() == gap_space.size())
            {
                const std::vector<U32>& ids = enc->mSpaceIds;
                for (size_t g = 0; g < gap_space.size(); ++g)
                {
                    const U16 space = te->mGaps[g].mSpace;
                    gap_space[g] = space && space <= ids.size() ? ids[space - 1] : 0;
                }
            }
            in.mGapSpace = gap_space.data();
        }
        sink.mOffset = gAgent.getPosAgentFromGlobal(n.mMesh->mOrigin);
        bool ok = gatherAirDebug(air, in, sink);
        if (ok && n.mCoarse)
        {
            // Far tiles framed at the slice's floor, or the camera's height, so coarse air never passes for detail.
            const F32 z = static_cast<F32>((slicing ? slice_z : camera.mdV[VZ] - 2.0) - n.mMesh->mOrigin.mdV[VZ]);
            const LLVector3 lo(LLVector3(n.mTile->mMin - n.mMesh->mOrigin)), hi(LLVector3(n.mTile->mMax - n.mMesh->mOrigin));
            const LLVector3 c[4] = { LLVector3(lo.mV[VX], lo.mV[VY], z), LLVector3(hi.mV[VX], lo.mV[VY], z), LLVector3(hi.mV[VX], hi.mV[VY], z),
                                     LLVector3(lo.mV[VX], hi.mV[VY], z) };
            for (S32 k = 0; ok && k < 4; ++k)
            {
                ok = sink.line(c[k], c[(k + 1) % 4], far_frame);
            }
        }
        ++mStats.mAirOverlayTiles;
        if (!ok)
        {
            mStats.mAirOverlayCut = static_cast<U32>(tiles.size() - i);
            break;
        }
    }
    mStats.mAirOverlayVerts = sink.mUsed;
}
