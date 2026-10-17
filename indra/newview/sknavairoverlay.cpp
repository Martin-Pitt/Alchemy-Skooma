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
#include "sknavpalette.h"
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
            // Only a tile in reach asks for its spilled detail back.
            const F64 d2 = planDistance2(camera, tile.mMin, tile.mMax);
            if (d2 > reach2)
            {
                continue;
            }
            if (!tile.mAir)
            {
                // A settled tile's detail is on disk: it is drawn once it is back.
                if (tile.mAirSpilled)
                {
                    requestDetail(mesh.mId);
                }
                continue;
            }
            // Air built for the tile is its detail; air with no build signature is the coarse air a far tile reads.
            const bool coarse = tile.mAirBuilt == 0;
            tiles.push_back({ d2, &mesh, t.first, &tile, coarse });
        }
    }
    std::sort(tiles.begin(), tiles.end(), [](const Near& a, const Near& b) { return a.mDistance2 < b.mDistance2; });

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
            bool ok = true;
            for (size_t p = 0; ok && p < polys.mPolys.size() && p < spaces.size(); ++p)
            {
                const TilePolys::Poly& poly = polys.mPolys[p];
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
    std::vector<LLColor4> gap_colour;
    const LLColor4& outdoors = skNavSwatchColor(SKNavSwatch::ENCLOSURE_OUTDOORS);
    const LLColor4& sheltered = skNavSwatchColor(SKNavSwatch::ENCLOSURE_SHELTERED);
    const LLColor4& indoors = skNavSwatchColor(SKNavSwatch::ENCLOSURE_INDOORS);
    const LLColor4& unknown = skNavSwatchColor(SKNavSwatch::ENCLOSURE_UNKNOWN);
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
        else if (which == AirDebugMode::INDOOR)
        {
            // Blue outdoors through amber sheltered to red indoors by e, grey where nothing answered.
            gap_colour.assign(air.mGaps.size(), unknown);
            EnclosureProductPtr enc;
            RoomsProductPtr rooms;
            const TileEnclosure* te = mRooms->landed(n.mMesh->mId, enc, rooms) ? enc->tile(n.mKey) : nullptr;
            if (te && te->mAir == n.mTile->mAir && te->mGaps.size() == gap_colour.size())
            {
                for (size_t g = 0; g < gap_colour.size(); ++g)
                {
                    const GapEnclosure& ge = te->mGaps[g];
                    if (ge.mD != ENCLOSURE_D_UNKNOWN)
                    {
                        const F32 e = ge.mE / 255.f;
                        gap_colour[g] = e < 0.5f ? lerp(outdoors, sheltered, e * 2.f) : lerp(sheltered, indoors, e * 2.f - 1.f);
                    }
                }
            }
            in.mGapColour = gap_colour.data();
        }
        sink.mOffset = gAgent.getPosAgentFromGlobal(n.mMesh->mOrigin);
        bool ok = gatherAirDebug(air, in, sink);
        if (ok && n.mCoarse)
        {
            // Far tiles framed under the camera, so coarse air never passes for detail.
            const F32 z = static_cast<F32>(camera.mdV[VZ] - 2.0 - n.mMesh->mOrigin.mdV[VZ]);
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
