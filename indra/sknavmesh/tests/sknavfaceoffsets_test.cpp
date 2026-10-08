/**
 * @file sknavfaceoffsets_test.cpp
 * @brief Face offsets: where a wall's true face sits inside its solid cell.
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

#include "linden_common.h"

#include "../test/lltut.h"

#include "../sknavair.h"
#include "../sknavairdistil.h"
#include "../sknavbuild.h"
#include "../sknavfaceoffsets.h"
#include "../sknavwalls.h"

#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using namespace sknav;

    constexpr F32 CELL = 0.125f;
    constexpr F32 UNIT = 0.1f;
    constexpr S32 CLIMB = 2;

    /// Three columns along x at 0.125 m: air, solid through the jamb band, air.
    AirGrid jambGrid()
    {
        AirGrid g;
        g.mCell = CELL;
        g.mUnit = UNIT;
        g.mWidth = 3;
        g.mHeight = 1;
        AirGap floor;
        floor.mFloor = 0;
        floor.mCeiling = 40;
        floor.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap above;
        above.mFloor = 25;
        above.mCeiling = 40;
        g.mGaps = { floor, above, floor };
        g.mStart = { 0, 1, 2, 3 };
        return g;
    }

    /// A soup in Recast space, filled with SL quads.
    struct Soup
    {
        std::vector<F32> mVerts;
        std::vector<S32> mTris;

        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
        {
            for (const LLVector3* t : { &a, &b, &c, &a, &c, &d })
            {
                F32 rc[3];
                toRecast(*t, rc);
                mTris.push_back(static_cast<S32>(mVerts.size() / 3));
                mVerts.insert(mVerts.end(), rc, rc + 3);
            }
        }
        /// A wall in the plane x, across the cell and up past the band.
        void wallX(F32 x) { quad(LLVector3(x, -1.f, 0.f), LLVector3(x, 2.f, 0.f), LLVector3(x, 2.f, 3.f), LLVector3(x, -1.f, 3.f)); }
        /// A slab at height z over x0 to x1.
        void slab(F32 x0, F32 x1, F32 z) { quad(LLVector3(x0, -1.f, z), LLVector3(x1, -1.f, z), LLVector3(x1, 2.f, z), LLVector3(x0, 2.f, z)); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

    /// A one-quad Detour tile, Recast x from bmin_x for dx cells and z from 0 for dz cells, at 0.125 m; its edges are
    /// walls unless a seam side is a portal. Edges: x0 side v0-v1, z1 side v1-v2 (SL +x), x1 side v2-v3, z0 side v3-v0.
    struct QuadTile
    {
        U8* mData = nullptr;
        S32 mSize = 0;

        QuadTile(F32 bmin_x, U16 dx, U16 dz, bool portal_x0, bool portal_x1, bool detail_mid = false, bool loose_x = false, U16 z_margin = 8)
        {
            const U16 z0 = z_margin, z1 = static_cast<U16>(z_margin + dz);     // clear of the tile's z bounds, a metre by default
            const U16 x0 = loose_x ? 8 : 0, x1 = static_cast<U16>(x0 + dx);
            const U16 verts[] = { x0, 0, z0, x0, 0, z1, x1, 0, z1, x1, 0, z0 };
            U16 polys[12] = { 0, 1, 2, 3, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX,
                              RC_MESH_NULL_IDX, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX };
            if (portal_x0)
            {
                polys[6] = 0x8000 | 0;
            }
            if (portal_x1)
            {
                polys[8] = 0x8000 | 2;
            }
            const U8 areas[] = { 1 };
            const U16 flags[] = { 1 };
            dtNavMeshCreateParams p;
            std::memset(&p, 0, sizeof(p));
            p.verts = verts;
            p.vertCount = 4;
            p.polys = polys;
            p.polyAreas = areas;
            p.polyFlags = flags;
            p.polyCount = 1;
            p.nvp = 6;
            p.walkableHeight = 2.f;
            p.walkableRadius = 0.3f;
            p.walkableClimb = 0.5f;
            p.bmin[0] = bmin_x - x0 * CELL;
            p.bmax[0] = bmin_x + (dx + 2 * x0) * CELL - x0 * CELL;
            p.bmax[1] = 1.f;
            p.bmin[2] = -1.f;
            p.bmax[2] = (dz + 2 * z_margin) * CELL - 1.f;
            p.cs = CELL;
            p.ch = UNIT;
            p.buildBvTree = true;
            // With detail_mid, a detail vertex halfway along the SL +x edge.
            const U32 dmeshes[] = { 0, 5, 0, 3 };
            F32 dverts[] = { 0.f, 0.f, -1.f + z0 * CELL, 0.f, 0.f, -1.f + z1 * CELL, dx * CELL, 0.f, -1.f + z1 * CELL,
                                   dx * CELL, 0.f, -1.f + z0 * CELL, 0.5f * dx * CELL, 0.f, -1.f + z1 * CELL };
            const U8 dtris[] = { 0, 1, 4, 0, 0, 4, 2, 0, 0, 2, 3, 0 };
            if (detail_mid)
            {
                for (S32 k = 0; k < 5; ++k)
                {
                    dverts[k * 3] += bmin_x;
                }
                p.detailMeshes = dmeshes;
                p.detailVerts = dverts;
                p.detailVertsCount = 5;
                p.detailTris = dtris;
                p.detailTriCount = 3;
            }
            dtCreateNavMeshData(&p, &mData, &mSize);
        }
        ~QuadTile() { dtFree(mData); }
        /// A vertex in SL, mesh-local.
        LLVector3 vert(S32 i) const
        {
            const F32* v = reinterpret_cast<const F32*>(mData + dtAlign4(sizeof(dtMeshHeader))) + i * 3;
            return LLVector3(v[2], v[0], v[1]);
        }
    };

    /// A wall whose true face stands at SL x = face, and one at SL y = face_y when that is given.
    FaceProbe wallProbe(F32 face, F32 face_y = -1.f)
    {
        return [face, face_y](const LLVector3& at, S32 dir, F32 reach) -> F32 {
            const F32 d = dir == 0 ? face - at.mV[VX] : dir == 2 && face_y >= 0.f ? face_y - at.mV[VY] : -1.f;
            return d >= 0.f && d <= reach ? d : -1.f;
        };
    }

}

namespace tut
{
    struct sknavfaceoffsets_data
    {
    };
    typedef test_group<sknavfaceoffsets_data> sknavfaceoffsets_group;
    typedef sknavfaceoffsets_group::object sknavfaceoffsets_object;
    tut::sknavfaceoffsets_group sknavfaceoffsets_tests("sknavfaceoffsets");

    template<> template<>
    void sknavfaceoffsets_object::test<1>()
    {
        set_test_name("a jamb 0.3 of a cell in reads 4 steps of 16, rounded towards the air, and its far face its own");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.wallX(2.f * CELL - 0.5f * CELL);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("both sides of the solid column face a floor", stats.mSides, 2u);
        ensure_equals("both are off the boundary", table.size(), static_cast<size_t>(2));
        ensure_equals("the -x face", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
        ensure_equals("the +x face", faceOffset(table, faceKey(1, 0, 3, 1)), 8.f / 16.f);
        ensure("the residual is under a step", stats.mResidualSum >= 0.0 && stats.mResidualSum < 2.0 * CELL / 16.0);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<2>()
    {
        set_test_name("a wall on the cell edge gives an offset of 0 and no entry");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL);
        s.wallX(2.f * CELL);
        s.slab(CELL, 2.f * CELL, 0.6f);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("measured", stats.mMeasured, 2u);
        ensure("no entry", table.empty());
        ensure_equals("offset 0", faceOffset(table, faceKey(1, 0, 3, 0)), 0.f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<3>()
    {
        set_test_name("a side measured over two floors keeps the nearer face, and one floor seeing the boundary keeps none");
        // Column 1 is solid through both floors' bands: the lower sees a jamb 0.3 in, the upper one 0.5 in.
        AirGrid g = jambGrid();
        AirGap upper;
        upper.mFloor = 30;
        upper.mCeiling = 60;
        upper.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap lower = g.mGaps[0];
        lower.mCeiling = 25;
        g.mGaps = { lower, upper, g.mGaps[1], lower, upper };
        g.mGaps[2].mFloor = 55;
        g.mGaps[2].mCeiling = 60;
        g.mStart = { 0, 2, 3, 5 };
        Soup s;
        s.quad(LLVector3(1.3f * CELL, -1.f, 0.f), LLVector3(1.3f * CELL, 2.f, 0.f), LLVector3(1.3f * CELL, 2.f, 2.f), LLVector3(1.3f * CELL, -1.f, 2.f));
        s.quad(LLVector3(1.5f * CELL, -1.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 5.f), LLVector3(1.5f * CELL, -1.f, 5.f));
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("each side measured for both floors", stats.mMeasured, 4u);
        ensure_equals("the nearer face, 4 steps", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
        // The upper floor's band meets a wall within a step of the boundary.
        s.quad(LLVector3(CELL + 0.002f, -1.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 5.f), LLVector3(CELL + 0.002f, -1.f, 5.f));
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table);
        ensure_equals("none", faceOffset(table, faceKey(1, 0, 3, 0)), 0.f);
        ensure_equals("another side keeps its own", faceOffset(table, faceKey(1, 0, 3, 1)) > 0.f, true);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<4>()
    {
        set_test_name("a wall face on its cell boundary moves back to the true face behind it");
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 0.f);     // the floor it stands on runs under it
        std::vector<WallFace> faces(1);
        faces[0].mMin.set(CELL, 0.f, 0.f);
        faces[0].mMax.set(CELL, CELL, 2.f);
        faces[0].mSide = 1;     // looks -x, solid at +x
        alignWallFaces(faces, s.mVerts.data(), s.mTris.data(), s.ntris(), CELL, UNIT);
        ensure("moved to the wall", std::fabs(faces[0].mMin.mV[VX] - 1.3f * CELL) < 1e-4f && std::fabs(faces[0].mMax.mV[VX] - 1.3f * CELL) < 1e-4f);
        ensure_equals("along it, unmoved", faces[0].mMax.mV[VY], CELL);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<5>()
    {
        set_test_name("a slab resting on the top of the jamb band only touches it, and leaves the jamb's offset alone");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 15 * UNIT);
        std::vector<FaceOffset> table;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table);
        ensure_equals("the -x face", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
    }
    template<> template<>
    void sknavfaceoffsets_object::test<9>()
    {
        set_test_name("a padded table crops to the tile's own columns");
        std::vector<FaceOffset> padded = { FaceOffset(faceKey(1, 1, 8, 2), 3), FaceOffset(faceKey(2, 3, 8, 0), 5), FaceOffset(faceKey(7, 7, 8, 1), 9) };
        std::vector<FaceOffset> out;
        cropFaceOffsets(padded, 8, 2, 4, 4, out);
        ensure_equals("one inside", out.size(), static_cast<size_t>(1));
        ensure_equals("at its own column", out[0].key(), faceKey(0, 1, 4, 0));
        ensure_equals("with its offset", out[0].offset(), 5u);
    }
    template<> template<>
    void sknavfaceoffsets_object::test<10>()
    {
        set_test_name("a wall edge moves out to stand the radius off the wall's true face, and the BV tree finds the floor it gained");
        QuadTile t(0.f, 8, 8, false, false, true, true);
        EdgeAlignStats stats;
        alignWallEdges(t.mData, t.mSize, 0.3f, CELL, wallProbe(1.5f), &stats);
        ensure("the +x edge's ends at 1.2", std::fabs(t.vert(1).mV[VX] - 1.2f) < 1e-4f && std::fabs(t.vert(2).mV[VX] - 1.2f) < 1e-4f);
        ensure("the others where they were", t.vert(0).mV[VX] == 0.f && t.vert(3).mV[VX] == 0.f);
        ensure_equals("one wall edge", stats.mWallEdges, 1u);
        ensure("its error 0.2 before and none after", std::fabs(stats.mErrBefore - 0.2) < 1e-4 && stats.mErrAfter < 1e-4);
        dtNavMesh nav;
        dtNavMeshParams np;
        np.orig[0] = np.orig[1] = np.orig[2] = 0.f;
        np.tileWidth = np.tileHeight = 2.f;
        np.maxTiles = 1;
        np.maxPolys = 4;
        ensure("navmesh", dtStatusSucceed(nav.init(&np)) && dtStatusSucceed(nav.addTile(t.mData, t.mSize, 0, 0, nullptr)));
        dtNavMeshQuery q;
        q.init(&nav, 16);
        dtQueryFilter filter;
        const F32 at[3] = { 0.5f, 0.f, 1.15f };     // SL x 1.15, past the old edge
        const F32 half[3] = { 0.01f, 0.5f, 0.01f };
        dtPolyRef ref = 0;
        F32 nearest[3];
        q.findNearestPoly(at, half, &filter, &ref, nearest);
        ensure("found by the refitted tree", ref != 0 && std::fabs(nearest[2] - 1.15f) < 1e-3f);
        const dtMeshTile* tile = nav.getTileAt(0, 0, 0);
        ensure("the BV tree's root grown to hold it", tile && tile->bvTree && tile->bvTree[0].bmax[2] >= 18);
        ensure("the detail vertex on the edge went with it", tile && tile->header->detailVertCount == 1 && std::fabs(tile->detailVerts[2] - 1.2f) < 1e-4f);
        nav.removeTile(nav.getTileRefAt(0, 0, 0), nullptr, nullptr);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<11>()
    {
        set_test_name("a seam vertex: where a wall crosses a seam both tiles keep their copies of it where they were");
        QuadTile a(0.f, 8, 8, false, true), b(1.f, 8, 8, true, false);
        alignWallEdges(a.mData, a.mSize, 0.3f, CELL, wallProbe(1.5f));
        alignWallEdges(b.mData, b.mSize, 0.3f, CELL, wallProbe(1.5f));
        // a's v2 and b's v1 are the seam's end of the wall edge: SL (x, y) = (1, 1) before.
        ensure("the same place, where it was", dist_vec(a.vert(2), b.vert(1)) < 1e-5f && std::fabs(a.vert(2).mV[VX] - 1.f) < 1e-5f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<12>()
    {
        set_test_name("a wall lying along the seam cannot drag its vertices off it");
        QuadTile a(0.f, 8, 8, false, false);
        alignWallEdges(a.mData, a.mSize, 0.3f, CELL, wallProbe(-1.f, 1.4f));
        ensure("on the seam still", std::fabs(a.vert(2).mV[VY] - 1.f) < 1e-5f && std::fabs(a.vert(3).mV[VY] - 1.f) < 1e-5f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<13>()
    {
        set_test_name("the air's probe finds the first solid in the jamb band and reads its true face");
        const AirGrid g = jambGrid();
        TileAir air;
        distilAir(g, CLIMB, air);
        air.mFaceOffsets = { FaceOffset(faceKey(1, 0, 3, 0), 4), FaceOffset(faceKey(1, 0, 3, 1), 8) };
        const FaceProbe probe = airFaceProbe(air, CLIMB);
        ensure("+x from the near column: to 0.125 + 4/16 of a cell", std::fabs(probe(LLVector3(0.05f, 0.06f, 0.f), 0, 1.f) - (1.25f * CELL - 0.05f)) < 1e-5f);
        ensure("-x from the far column: to 0.25 - 8/16 of a cell", std::fabs(probe(LLVector3(0.3f, 0.06f, 0.f), 1, 1.f) - (0.3f - 1.5f * CELL)) < 1e-5f);
        ensure("nothing along y", probe(LLVector3(0.05f, 0.06f, 0.f), 2, 1.f) < 0.f);
        ensure("nothing in reach", probe(LLVector3(0.05f, 0.06f, 0.f), 0, 0.05f) < 0.f);
        ensure("from the top of the solid, no wall", probe(LLVector3(0.05f, 0.06f, 2.5f), 0, 1.f) < 0.f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<14>()
    {
        set_test_name("where two walls meet, the corner moves to where both moved edges cross");
        QuadTile t(0.f, 8, 8, false, false, false, true);
        alignWallEdges(t.mData, t.mSize, 0.3f, CELL, wallProbe(1.5f, 1.4f));
        ensure("at (1.2, 1.1)", std::fabs(t.vert(2).mV[VX] - 1.2f) < 1e-4f && std::fabs(t.vert(2).mV[VY] - 1.1f) < 1e-4f);
        ensure("the +x wall's other end, along x only", std::fabs(t.vert(1).mV[VX] - 1.2f) < 1e-4f && std::fabs(t.vert(1).mV[VY]) < 1e-4f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<15>()
    {
        set_test_name("a wall edge's vertex stays put when moving it would bend a neighbouring polygon in");
        // Recast cells (x, z), z a metre in from the tile's bounds: a quad whose SL +x edge is a wall, and beside it a
        // quad sharing that edge's end whose next vertex is barely past the wall edge's line.
        const U16 verts[] = { 8, 0, 8, 8, 0, 16, 16, 0, 16, 16, 0, 8, 17, 0, 17, 24, 0, 18, 24, 0, 12 };
        const U16 N = RC_MESH_NULL_IDX;
        const U16 polys[] = { 0, 1, 2, 3, N, N, N, N, N, N, N, N, 2, 4, 5, 6, N, N, N, N, N, N, N, N };
        const U8 areas[] = { 1, 1 };
        const U16 flags[] = { 1, 1 };
        dtNavMeshCreateParams p;
        std::memset(&p, 0, sizeof(p));
        p.verts = verts;
        p.vertCount = 7;
        p.polys = polys;
        p.polyAreas = areas;
        p.polyFlags = flags;
        p.polyCount = 2;
        p.nvp = 6;
        p.walkableHeight = 2.f;
        p.walkableRadius = 0.3f;
        p.walkableClimb = 0.5f;
        p.bmin[0] = p.bmin[2] = -1.f;
        p.bmax[0] = p.bmax[2] = 3.f;
        p.bmax[1] = 1.f;
        p.cs = CELL;
        p.ch = UNIT;
        p.buildBvTree = true;
        U8* data = nullptr;
        S32 size = 0;
        ensure("built", dtCreateNavMeshData(&p, &data, &size));
        EdgeAlignStats stats;
        // The first quad's SL +x edge, Recast z = 16 cells, is SL x = 1; its wall's true face is at 1.5.
        alignWallEdges(data, size, 0.3f, CELL, wallProbe(1.5f), &stats);
        const F32* v = reinterpret_cast<const F32*>(data + dtAlign4(sizeof(dtMeshHeader)));
        ensure("the shared end stays", std::fabs(v[2 * 3 + 2] - 1.f) < 1e-5f);
        ensure("a move was dropped for it", stats.mDropped > 0);
        dtFree(data);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<16>()
    {
        set_test_name("a vertex a cell inside the tile's bounds is not pushed past them");
        QuadTile t(0.f, 8, 8, false, false, false, true, 1);     // SL x from -0.875 to 0.125, the bound at 0.25
        alignWallEdges(t.mData, t.mSize, 0.3f, CELL, wallProbe(0.625f));
        ensure("held at the bound", std::fabs(t.vert(1).mV[VX] - 0.25f) < 1e-5f);
    }
}
