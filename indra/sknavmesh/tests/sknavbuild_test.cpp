/**
 * @file sknavbuild_test.cpp
 * @brief Tile builds from records and terrain: floors, storeys, a block to walk round, tile seams.
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

#include "../sknavbuild.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace sknav;

    // A box record in mesh-local metres (origin at zero), axis aligned.
    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, 1, 0);
        r->mAxes[2].set(0, 0, 1);
        r->mSolid = true;
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
    }

    // A record of triangles, as the census hands one over.
    RecordPtr soup(const std::vector<LLVector3>& tris, Provenance provenance)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::TRIANGLES;
        r->mProvenance = provenance;
        r->mSolid = provenance == Provenance::HULL || provenance == Provenance::BASE_HULL;
        r->mTriangles = std::make_shared<const std::vector<LLVector3>>(tris);
        r->mMin.set(1e9, 1e9, 1e9);
        r->mMax.set(-1e9, -1e9, -1e9);
        for (const LLVector3& p : tris)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                r->mMin.mdV[k] = std::min(r->mMin.mdV[k], static_cast<F64>(p.mV[k]));
                r->mMax.mdV[k] = std::max(r->mMax.mdV[k], static_cast<F64>(p.mV[k]));
            }
        }
        return r;
    }

    // A hull record: the triangle soup the sim collides with as a solid.
    RecordPtr hull(const std::vector<LLVector3>& tris)
    {
        return soup(tris, Provenance::HULL);
    }

    // A flat quad at z, as a floor arrives from a physics mesh.
    std::vector<LLVector3> slab(F32 x0, F32 y0, F32 x1, F32 y1, F32 z)
    {
        return {
            LLVector3(x0, y0, z), LLVector3(x1, y0, z), LLVector3(x1, y1, z),
            LLVector3(x0, y0, z), LLVector3(x1, y1, z), LLVector3(x0, y1, z),
        };
    }

    // A window that reaches past the tile's border cells on every side: Recast
    // makes a portal only where walkable spans continue into the border.
    TerrainWindow flat(F32 z, S32 n = 37, F32 ox = -2.f, F32 oy = -2.f)
    {
        TerrainWindow t;
        t.mOrigin.set(ox, oy, 0);
        t.mSpacing = 1.f;
        t.mWidth = n;
        t.mHeight = n;
        t.mZ.assign(n * n, z);
        t.mWater = -10.f;
        return t;
    }

    TileBuildInput tile(S32 tx, S32 ty)
    {
        TileBuildInput in;
        in.mTileX = tx;
        in.mTileY = ty;
        in.mZMin = -1.f;
        in.mZMax = 12.f;
        in.mCell = 0.25f; // coarser than the viewer, to keep the tests quick
        in.mClasses[0] = true;
        in.mClasses[1] = true;
        in.mParams[1].mHeight = 0.5f;
        in.mParams[1].mRadius = 0.f;
        return in;
    }

    struct Mesh
    {
        dtNavMesh* mNav = nullptr;
        dtNavMeshQuery* mQuery = nullptr;
        Mesh()
        {
            mNav = dtAllocNavMesh();
            dtNavMeshParams p;
            p.orig[0] = p.orig[1] = p.orig[2] = 0.f;
            p.tileWidth = 32.f;
            p.tileHeight = 32.f;
            p.maxTiles = 1024;      // the viewer's split: 10 + 12 bits leaves Detour its 10 salt bits
            p.maxPolys = 4096;
            tut::ensure("mesh init", dtStatusSucceed(mNav->init(&p)));
            mQuery = dtAllocNavMeshQuery();
            tut::ensure("query init", dtStatusSucceed(mQuery->init(mNav, 2048)));
        }
        ~Mesh()
        {
            dtFreeNavMeshQuery(mQuery);
            dtFreeNavMesh(mNav);
        }
        void add(const std::vector<U8>& data)
        {
            unsigned char* copy = static_cast<unsigned char*>(dtAlloc(static_cast<int>(data.size()), DT_ALLOC_PERM));
            std::memcpy(copy, data.data(), data.size());
            tut::ensure("addTile", dtStatusSucceed(mNav->addTile(copy, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, nullptr)));
        }
        // Nearest poly to an SL-space point; z snapped out.
        dtPolyRef nearest(const LLVector3& sl, F32* snapped_rc, F32 ext = 1.f)
        {
            F32 c[3];
            toRecast(sl, c);
            const F32 e[3] = { ext, 2.f, ext };
            dtQueryFilter f;
            dtPolyRef ref = 0;
            mQuery->findNearestPoly(c, e, &f, &ref, snapped_rc);
            return ref;
        }
        std::string mWhy;
        bool path(const LLVector3& a, const LLVector3& b, F32& length, bool& partial)
        {
            F32 s[3], e[3];
            const dtPolyRef sref = nearest(a, s), eref = nearest(b, e);
            if (!sref || !eref)
            {
                mWhy = llformat("nearest: start %d end %d, tiles %d", sref != 0, eref != 0, mNav->getMaxTiles());
                return false;
            }
            dtQueryFilter f;
            dtPolyRef polys[256];
            int n = 0;
            const dtStatus st = mQuery->findPath(sref, eref, s, e, &f, polys, &n, 256);
            if (!dtStatusSucceed(st))
            {
                mWhy = llformat("findPath status %x", st);
                return false;
            }
            partial = dtStatusDetail(st, DT_PARTIAL_RESULT);
            F32 straight[256 * 3];
            unsigned char flags[256];
            dtPolyRef refs[256];
            int ns = 0;
            mQuery->findStraightPath(s, e, polys, n, straight, flags, refs, &ns, 256);
            length = 0.f;
            for (int i = 1; i < ns; ++i)
            {
                const F32 dx = straight[i * 3] - straight[(i - 1) * 3], dz = straight[i * 3 + 2] - straight[(i - 1) * 3 + 2];
                length += std::sqrt(dx * dx + dz * dz);
            }
            return true;
        }
    };
}

namespace tut
{
    struct sknavbuild_data {};
    typedef test_group<sknavbuild_data> sknavbuild_group;
    typedef sknavbuild_group::object sknavbuild_object;
    tut::sknavbuild_group g("sknavbuild");

    // The coordinate map is a proper rotation and round-trips.
    template<> template<>
    void sknavbuild_object::test<1>()
    {
        const LLVector3 sl(1.f, 2.f, 3.f);
        F32 rc[3];
        toRecast(sl, rc);
        ensure_equals("x->z", rc[2], 1.f);
        ensure_equals("y->x", rc[0], 2.f);
        ensure_equals("z->y", rc[1], 3.f);
        ensure("round trip", fromRecast(rc) == sl);
    }

    // Terrain alone is one floor; a block on it is an obstacle to walk round
    // for the avatar class and a floor of its own for the weather class.
    template<> template<>
    void sknavbuild_object::test<2>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(box(14.f, 8.f, 0.f, 16.f, 24.f, 2.f));
        const TileBuildResult r = buildTile(in);
        ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));
        ensure("avatar polys", r.mPolys[0] > 0);
        ensure("weather polys", r.mPolys[1] > 0);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = true;
        ensure("path found", m.path(LLVector3(4.f, 16.f, 0.f), LLVector3(26.f, 16.f, 0.f), length, partial));
        ensure("path complete", !partial);
        ensure("path went round", length > 24.f);

        F32 snapped[3];
        ensure("block top is floor", m.nearest(LLVector3(15.f, 16.f, 2.f), snapped, 0.5f) != 0);
        ensure("top at 2 m", std::fabs(snapped[1] - 2.f) < 0.3f);
    }

    // Three storeys 3 m apart are three floors, none joined.
    template<> template<>
    void sknavbuild_object::test<3>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        for (S32 s = 1; s <= 3; ++s)
        {
            in.mRecords.push_back(box(8.f, 8.f, s * 3.f - 0.2f, 24.f, 24.f, s * 3.f));
        }
        const TileBuildResult r = buildTile(in);
        ensure("built", r.mStatus == TileBuildResult::SUCCESS);
        Mesh m;
        m.add(r.mData[0]);
        F32 snapped[3];
        for (S32 s = 1; s <= 3; ++s)
        {
            ensure("storey is floor", m.nearest(LLVector3(16.f, 16.f, s * 3.f), snapped, 0.5f) != 0);
            ensure("storey height", std::fabs(snapped[1] - s * 3.f) < 0.3f);
        }
        F32 length = 0.f;
        bool partial = false;
        ensure("path attempted", m.path(LLVector3(16.f, 16.f, 3.f), LLVector3(16.f, 16.f, 6.f), length, partial));
        ensure("storeys not joined", partial);
    }

    // Two tiles of one mesh stitch: a path crosses the seam.
    template<> template<>
    void sknavbuild_object::test<4>()
    {
        Mesh m;
        for (S32 tx = 0; tx < 2; ++tx)
        {
            TileBuildInput in = tile(tx, 0);
            in.mTerrain = flat(0.f, 37, tx * 32.f - 2.f);
            const TileBuildResult r = buildTile(in);
            ensure("tile built", r.mStatus == TileBuildResult::SUCCESS && r.mPolys[0] > 0);
            m.add(r.mData[0]);
        }
        F32 length = 0.f;
        bool partial = true;
        const bool ok = m.path(LLVector3(8.f, 16.f, 0.f), LLVector3(56.f, 16.f, 0.f), length, partial);
        ensure("path across seam: " + m.mWhy, ok);
        ensure("seam is linked", !partial);
        ensure("straight across", length < 50.f);
    }

    // A record from another region origin lands where it should, and an
    // empty tile says so rather than failing.
    template<> template<>
    void sknavbuild_object::test<5>()
    {
        Record r;
        r.mShape = Shape::BOX;
        r.mOrigin.set(256.0, 0.0, 0.0);
        r.mCenter.set(1.f, 1.f, 1.f);
        r.mHalf.set(1.f, 1.f, 1.f);
        r.mAxes[0].set(1, 0, 0);
        r.mAxes[1].set(0, 1, 0);
        r.mAxes[2].set(0, 0, 1);
        std::vector<LLVector3> out;
        emitRecord(r, LLVector3d(224.0, 0.0, 0.0), out);
        ensure_equals("12 triangles", out.size(), 36u);
        F32 minx = 1e9f, maxx = -1e9f;
        for (const LLVector3& p : out)
        {
            minx = std::min(minx, p.mV[VX]);
            maxx = std::max(maxx, p.mV[VX]);
        }
        ensure("shifted by the origin difference", std::fabs(minx - 32.f) < 1e-4f && std::fabs(maxx - 34.f) < 1e-4f);

        const TileBuildResult empty = buildTile(tile(3, 3));
        ensure("empty tile", empty.mStatus == TileBuildResult::EMPTY);
    }

    // A hull is what the sim collides with, and it collides with it from
    // either side: a surface is floor whichever way its triangles wind, and a
    // closed hull is solid, so there is no floor inside it.
    template<> template<>
    void sknavbuild_object::test<6>()
    {
        // A flat quad 2 m up, wound downward (clockwise seen from above).
        std::vector<LLVector3> quad = {
            LLVector3(4.f, 4.f, 2.f), LLVector3(4.f, 12.f, 2.f), LLVector3(12.f, 12.f, 2.f),
            LLVector3(4.f, 4.f, 2.f), LLVector3(12.f, 12.f, 2.f), LLVector3(12.f, 4.f, 2.f),
        };
        // A closed box 1 m up and 6 m tall, wound outward.
        const LLVector3 c[8] = {
            LLVector3(16.f, 16.f, 1.f), LLVector3(28.f, 16.f, 1.f), LLVector3(28.f, 28.f, 1.f), LLVector3(16.f, 28.f, 1.f),
            LLVector3(16.f, 16.f, 7.f), LLVector3(28.f, 16.f, 7.f), LLVector3(28.f, 28.f, 7.f), LLVector3(16.f, 28.f, 7.f),
        };
        std::vector<LLVector3> box_tris;
        auto tri = [&](int a, int b, int d) {
            box_tris.push_back(c[a]);
            box_tris.push_back(c[b]);
            box_tris.push_back(c[d]);
        };
        tri(0, 3, 2); tri(0, 2, 1);     // bottom
        tri(4, 5, 6); tri(4, 6, 7);     // top
        tri(0, 1, 5); tri(0, 5, 4);     // -y
        tri(1, 2, 6); tri(1, 6, 5);     // +x
        tri(2, 3, 7); tri(2, 7, 6);     // +y
        tri(3, 0, 4); tri(3, 4, 7);     // -x

        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(hull(quad));
        in.mRecords.push_back(hull(box_tris));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 snapped[3];
        ensure("down-facing hull surface is floor", m.nearest(LLVector3(8.f, 8.f, 2.f), snapped, 0.5f) != 0);
        ensure("at the surface height", std::fabs(snapped[1] - 2.f) < 0.3f);

        // Inside the box the fill leaves nothing walkable, and the terrain
        // below has a metre of headroom, which no avatar fits under.
        ensure("no floor inside the closed hull", m.nearest(LLVector3(22.f, 22.f, 1.f), snapped, 0.5f) == 0);
        ensure("the box roof is floor", m.nearest(LLVector3(22.f, 22.f, 7.f), snapped, 0.5f) != 0);
        ensure("roof at its height", std::fabs(snapped[1] - 7.f) < 0.3f);
    }

    // A steep ramp, 80 degrees: the weather class walks it, the avatar class
    // does not. The ledge filter must not cut it out of the weather mesh,
    // which its own rule would do at a climb of 0.75 m, where every slope
    // steeper than 72 degrees reads as a ledge.
    template<> template<>
    void sknavbuild_object::test<7>()
    {
        std::vector<LLVector3> ramp = {
            LLVector3(4.f, 4.f, 0.f), LLVector3(12.f, 4.f, 0.f), LLVector3(12.f, 7.5f, 20.f),
            LLVector3(4.f, 4.f, 0.f), LLVector3(12.f, 7.5f, 20.f), LLVector3(4.f, 7.5f, 20.f),
        };
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mParams[0].mSlope = 74.f;
        in.mParams[1].mSlope = 89.f;
        in.mRecords.push_back(soup(ramp, Provenance::EXACT));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        F32 snapped[3];
        Mesh weather;
        weather.add(r.mData[1]);
        ensure("weather walks the steep ramp", weather.nearest(LLVector3(8.f, 5.75f, 10.f), snapped, 0.5f) != 0);

        Mesh avatar;
        avatar.add(r.mData[0]);
        ensure("the avatar class leaves it", avatar.nearest(LLVector3(8.f, 5.75f, 10.f), snapped, 0.5f) == 0);
    }

    // A crevice one column wide between two floors is true air, and the sim
    // has it solid: the bridge joins it, so a path crosses.
    template<> template<>
    void sknavbuild_object::test<8>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.7f, 2.f), Provenance::EXACT));
        in.mRecords.push_back(soup(slab(4.f, 12.05f, 12.f, 20.f, 2.f), Provenance::EXACT));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = true;
        const bool ok = m.path(LLVector3(8.f, 8.f, 2.f), LLVector3(8.f, 16.f, 2.f), length, partial);
        ensure("path across the crevice: " + m.mWhy, ok);
        ensure("crevice joined", !partial);
    }

    // The wall's own matter in the crevice keeps the sides split: the bridge
    // must not tunnel from a floor through a face that stands on it.
    template<> template<>
    void sknavbuild_object::test<9>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.7f, 2.f), Provenance::EXACT));
        in.mRecords.push_back(soup(slab(4.f, 12.05f, 12.f, 20.f, 2.f), Provenance::EXACT));
        const std::vector<LLVector3> wall = {
            LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 4.f),
            LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 4.f), LLVector3(4.f, 11.875f, 4.f),
        };
        in.mRecords.push_back(soup(wall, Provenance::EXACT));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = false;
        const bool ok = m.path(LLVector3(8.f, 8.f, 2.f), LLVector3(8.f, 16.f, 2.f), length, partial);
        ensure("path attempted", ok);
        ensure("wall keeps the crevice split", partial);
    }

    // Two empty columns are not a crevice: nothing joins a gap that wide.
    template<> template<>
    void sknavbuild_object::test<10>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.7f, 2.f), Provenance::EXACT));
        in.mRecords.push_back(soup(slab(4.f, 13.2f, 12.f, 20.f, 2.f), Provenance::EXACT));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = false;
        const bool ok = m.path(LLVector3(8.f, 8.f, 2.f), LLVector3(8.f, 16.f, 2.f), length, partial);
        ensure("path attempted", ok);
        ensure("wide gap stays split", partial);
    }

    // A riser between two treads is matter at the seam's level, so the treads
    // stay split: they do not bridge straight up the step.
    template<> template<>
    void sknavbuild_object::test<11>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.7f, 2.f), Provenance::EXACT));
        in.mRecords.push_back(soup(slab(4.f, 12.05f, 12.f, 20.f, 2.5f), Provenance::EXACT));
        const std::vector<LLVector3> riser = {
            LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 2.5f),
            LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 2.5f), LLVector3(4.f, 11.875f, 2.5f),
        };
        in.mRecords.push_back(soup(riser, Provenance::EXACT));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = false;
        const bool ok = m.path(LLVector3(8.f, 8.f, 2.f), LLVector3(8.f, 16.f, 2.5f), length, partial);
        ensure("path attempted", ok);
        ensure("riser keeps the treads split", partial);
    }

    // Nothing below the land is worth a mesh: the sim keeps every agent on or
    // above it, so a box buried under the ground leaves no underground floor,
    // and one sunk into it stands only on the part that pokes out.
    template<> template<>
    void sknavbuild_object::test<12>()
    {
        TileBuildInput in = tile(0, 0);
        in.mZMin = -4.f;
        in.mTerrain = flat(0.f);
        // A buried roof 2.5 m down keeps 2 m of headroom to the land above it,
        // so the headroom filter alone would leave it walkable; only the cull
        // takes it out.
        in.mRecords.push_back(box(14.f, 14.f, -4.f, 18.f, 18.f, -2.5f));
        in.mRecords.push_back(box(20.f, 14.f, -3.5f, 24.f, 18.f, 2.5f));    // sunk, roof 2.5 m proud
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        ensure("terrain floor", r.mPolys[0] > 0);

        Mesh m;
        m.add(r.mData[0]);
        F32 snapped[3];
        // Each query sits deep enough that the land's own polys are out of
        // reach: only a span that survived underground could answer.
        ensure("nothing under the buried box", m.nearest(LLVector3(16.f, 16.f, -3.f), snapped, 0.5f) == 0);
        ensure("nothing under the proud box", m.nearest(LLVector3(22.f, 16.f, -3.f), snapped, 0.5f) == 0);
        ensure("the proud roof is floor", m.nearest(LLVector3(22.f, 16.f, 2.5f), snapped, 0.5f) != 0);
        ensure("roof at its height", std::fabs(snapped[1] - 2.5f) < 0.3f);
        // The land itself is still walkable floor: a path crosses the tile
        // round the proud box, where the buried one left no mark.
        F32 length = 0.f;
        bool partial = true;
        ensure("path on the land: " + m.mWhy, m.path(LLVector3(4.f, 16.f, 0.f), LLVector3(28.f, 16.f, 0.f), length, partial));
        ensure("path complete", !partial);
        ensure("path went round", length > 24.f);
    }

    // A slab whose edge ends one step above the land the sim lets an agent
    // walk up: the headroom filter takes the land out from under the slab's
    // lip, and the erosion used to read that strip as a wall to keep its
    // radius back from, carving the walkable ground either side of the seam
    // away and leaving the step a hole. Both classes walk the seam.
    template<> template<>
    void sknavbuild_object::test<13>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.25f);
        // The viewer's own numbers at its cell, coarse: height 1.25 m erodes
        // with a radius of 2 cells here, and the step is one cell.
        in.mParams[0] = { 1.25f, 0.3f, 0.75f, 74.f };
        in.mParams[1] = { 0.5f, 0.125f, 0.75f, 89.f };
        // The slab spans the tile so the seam can only be crossed, never
        // round: land to the north, its top a step up to the south.
        in.mRecords.push_back(box(0.f, -2.f, -0.5f, 36.f, 16.05f, 0.5f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        ensure("avatar polys", r.mPolys[0] > 0);
        ensure("weather polys", r.mPolys[1] > 0);

        Mesh m;
        m.add(r.mData[0]);
        F32 length = 0.f;
        bool partial = true;
        ensure("path found: " + m.mWhy, m.path(LLVector3(16.f, 20.f, 0.25f), LLVector3(16.f, 12.f, 0.5f), length, partial));
        ensure("the seam is walked", !partial);
        ensure("the path reaches the top", length > 7.f);

        Mesh w;
        w.add(r.mData[1]);
        F32 wlength = 0.f;
        bool wpartial = true;
        ensure("weather path found: " + w.mWhy, w.path(LLVector3(16.f, 20.f, 0.25f), LLVector3(16.f, 12.f, 0.5f), wlength, wpartial));
        ensure("the weather class walks the seam", !wpartial);
    }

    // The seam as the region had it: land rising over a wedge of ground whose
    // edge ends a step up, with a buried slab under the east side. Nothing
    // along the seam may lose its floor.
    template<> template<>
    void sknavbuild_object::test<14>()
    {
        TileBuildInput in = tile(0, 0);
        in.mCell = 0.125f;
        in.mZMin = 0.f;
        in.mZMax = 12.f;
        in.mParams[0] = { 1.25f, 0.3f, 0.75f, 74.f };
        in.mParams[1] = { 0.5f, 0.125f, 0.75f, 89.f };
        // The land: 5.375 m at the seam (y 6.2), rising a metre by y 8.2.
        TerrainWindow t;
        t.mOrigin.set(-2.f, -2.f, 0.f);
        t.mSpacing = 1.f;
        t.mWidth = 36;
        t.mHeight = 36;
        t.mZ.assign(36 * 36, 0.f);
        for (S32 j = 0; j < 36; ++j)
        {
            for (S32 i = 0; i < 36; ++i)
            {
                const F32 y = -2.f + j * 1.f;
                t.mZ[j * 36 + i] = y <= 6.2f ? 5.375f : llmin(5.375f + (y - 6.2f) * 0.5f, 6.375f);
            }
        }
        t.mWater = -10.f;
        in.mTerrain = t;
        // The box under the east side, its top under the land, edge at y 6.2.
        in.mRecords.push_back(box(5.f, 6.25f, 2.07f, 35.f, 50.f, 5.195f));
        // The ground: a wedge plate whose top runs 5.5 m at the seam down to
        // 5.25 m eight metres south, over a buried underside.
        std::vector<LLVector3> ground;
        const F32 x0 = -2.f, x1 = 22.f, y0 = -2.f, y1 = 6.3f;
        const F32 zN = 5.5f, zS = 5.25f, zB = 4.6f;
        const LLVector3 n00(x0, y1, zN), n10(x1, y1, zN), s00(x0, y0, zS), s10(x1, y0, zS);
        const LLVector3 b00(x0, y0, zB), b10(x1, y0, zB), bN0(x0, y1, zB), bN1(x1, y1, zB);
        const std::vector<LLVector3> quads = {
            n00, n10, s10, n00, s10, s00,     // top
            b00, b10, s10, b00, s10, s00,     // bottom
            s00, s10, b10, s00, b10, b00,     // south edge
            n00, n10, bN1, n00, bN1, bN0,     // north edge (the lip)
            n00, s00, b00, n00, b00, bN0,     // west edge
            n10, s10, b10, n10, b10, bN1,     // east edge
        };
        ground.insert(ground.end(), quads.begin(), quads.end());
        in.mRecords.push_back(soup(ground, Provenance::HULL));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        ensure("avatar polys", r.mPolys[0] > 0);
        ensure("weather polys", r.mPolys[1] > 0);

        Mesh m;
        m.add(r.mData[0]);
        S32 holes = 0;
        std::string profile;
        for (F32 y = 3.f; y <= 9.01f; y += 0.25f)
        {
            F32 snapped[3];
            const dtPolyRef ref = m.nearest(LLVector3(10.f, y, 5.4f), snapped, 0.02f);
            if (!ref)
            {
                holes++;
            }
            profile += (ref ? std::string(" ") : std::string(" HOLE ")) + llformat("%.2f", y);
        }
        ensure("no hole along the seam:" + profile, holes == 0);
    }

    // A kerb narrower than the avatar and lower than its climb is stepped
    // over, so the floor is flat across it and a metre either side; a dais
    // wide enough to stand on keeps its height. The detail vertices Recast
    // adds sit at the polygon corners' level, not a cell above them.
    template<> template<>
    void sknavbuild_object::test<15>()
    {
        TileBuildInput in = tile(0, 0);
        in.mCell = 0.125f; // the viewer's cell: the sampling this is about
        in.mClasses[1] = false;
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(box(10.f, 4.f, -0.5f, 10.3f, 28.f, 0.5f));
        in.mRecords.push_back(box(16.f, 4.f, -0.5f, 18.f, 28.f, 0.5f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        Mesh m;
        m.add(r.mData[0]);
        // The floor's level is its polygon's corners: on flat ground the
        // detail surface has no business leaving them.
        F32 ref[3];
        const dtPolyRef open = m.nearest(LLVector3(5.f, 16.f, 0.f), ref, 0.05f);
        ensure("open floor", open != 0);
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        m.mNav->getTileAndPolyByRefUnsafe(open, &tile, &poly);
        F32 floor = 1e9f;
        for (int v = 0; v < poly->vertCount; ++v)
        {
            floor = std::min(floor, tile->verts[poly->verts[v] * 3 + 1]);
        }
        F32 worst = 0.f;
        std::string where;
        for (F32 y = 6.f; y <= 26.01f; y += 0.25f)
        {
            for (F32 x = 7.f; x <= 13.31f; x += 0.125f)
            {
                F32 snapped[3];
                const dtPolyRef ref = m.nearest(LLVector3(x, y, 0.f), snapped, 0.05f);
                ensure("floor under " + llformat("%.2f %.2f", x, y), ref != 0);
                if (std::fabs(snapped[1] - floor) > worst)
                {
                    worst = std::fabs(snapped[1] - floor);
                    where = llformat("%.2f %.2f -> %.3f (floor %.3f)", x, y, snapped[1], floor);
                }
            }
        }
        ensure("floor is flat across the kerb: " + where, worst < 0.1f);
        // Up to the dais the floor stays floor: the detail vertices that
        // tessellate the edge shared with the dais polygon sit at the floor's
        // corner height, not a cell above it. The last sample spacing before
        // the dais is its ramp, which Recast owes to sharing corners.
        for (F32 x = 13.f; x <= 15.21f; x += 0.125f)
        {
            F32 snapped[3];
            ensure("floor under " + llformat("%.2f", x), m.nearest(LLVector3(x, 16.f, 0.f), snapped, 0.05f) != 0);
            ensure("floor level " + llformat("%.2f -> %.3f (floor %.3f)", x, snapped[1], floor), std::fabs(snapped[1] - floor) < 0.1f);
        }
        F32 top[3];
        ensure("dais is floor", m.nearest(LLVector3(17.f, 16.f, 0.5f), top, 0.05f) != 0);
        ensure("dais keeps its height", std::fabs(top[1] - floor - 0.5f) < 0.1f);
    }

    // A box sunk into the ground is solid down to its bottom: the terrain it
    // swallows is not a floor inside it. Its roof is, and so is the ground
    // around it.
    template<> template<>
    void sknavbuild_object::test<16>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(box(8.f, 8.f, -0.5f, 24.f, 24.f, 3.f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 snapped[3];
        ensure("no floor on the terrain inside the box", m.nearest(LLVector3(16.f, 16.f, 0.f), snapped, 0.5f) == 0);
        ensure("the box roof is floor", m.nearest(LLVector3(16.f, 16.f, 3.f), snapped, 0.5f) != 0);
        ensure("roof at its height", std::fabs(snapped[1] - 3.f) < 0.3f);
        ensure("ground beside the box is floor", m.nearest(LLVector3(4.f, 4.f, 0.f), snapped, 0.5f) != 0);
        ensure("ground at its height", std::fabs(snapped[1]) < 0.3f);
    }

    // A solid with a tunnel through it, as a hollow prim on its side or a
    // hull-less physics prim arrives: one closed surface. The fill takes the
    // column's crossings in pairs, so the tunnel floor is a floor under a
    // roof and the solid either side of it is filled to the top.
    template<> template<>
    void sknavbuild_object::test<17>()
    {
        // Outer 8..24 x 8..24, z -0.5..4; tunnel along y at x 14..18, z 0.5..3.
        std::vector<LLVector3> tris;
        auto q = [&](F32 ax, F32 ay, F32 az, F32 bx, F32 by, F32 bz, F32 cx, F32 cy, F32 cz, F32 dx, F32 dy, F32 dz) {
            const LLVector3 a(ax, ay, az), b(bx, by, bz), c(cx, cy, cz), d(dx, dy, dz);
            tris.insert(tris.end(), { a, b, c, a, c, d });
        };
        q(8, 8, 4, 24, 8, 4, 24, 24, 4, 8, 24, 4);                  // top
        q(8, 8, -0.5f, 8, 24, -0.5f, 24, 24, -0.5f, 24, 8, -0.5f);  // bottom
        q(8, 8, -0.5f, 8, 24, -0.5f, 8, 24, 4, 8, 8, 4);            // -x side
        q(24, 8, -0.5f, 24, 8, 4, 24, 24, 4, 24, 24, -0.5f);        // +x side
        for (F32 y : { 8.f, 24.f })                                 // the ends, minus the tunnel mouth
        {
            q(8, y, -0.5f, 14, y, -0.5f, 14, y, 4, 8, y, 4);        // left of the mouth
            q(18, y, -0.5f, 24, y, -0.5f, 24, y, 4, 18, y, 4);      // right of it
            q(14, y, -0.5f, 18, y, -0.5f, 18, y, 0.5f, 14, y, 0.5f); // below it
            q(14, y, 3, 18, y, 3, 18, y, 4, 14, y, 4);              // above it
        }
        q(14, 8, 0.5f, 18, 8, 0.5f, 18, 24, 0.5f, 14, 24, 0.5f);    // tunnel floor
        q(14, 8, 3, 14, 24, 3, 18, 24, 3, 18, 8, 3);                // tunnel ceiling
        q(14, 8, 0.5f, 14, 24, 0.5f, 14, 24, 3, 14, 8, 3);          // tunnel walls
        q(18, 8, 0.5f, 18, 8, 3, 18, 24, 3, 18, 24, 0.5f);

        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(hull(tris));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        F32 snapped[3];
        ensure("tunnel floor is floor", m.nearest(LLVector3(16.f, 16.f, 0.5f), snapped, 0.5f) != 0);
        ensure("at the tunnel floor's height", std::fabs(snapped[1] - 0.5f) < 0.3f);
        ensure("no floor in the solid beside the tunnel", m.nearest(LLVector3(11.f, 16.f, 0.f), snapped, 0.5f) == 0);
        ensure("the roof is floor", m.nearest(LLVector3(11.f, 16.f, 4.f), snapped, 0.5f) != 0);
        ensure("roof at its height", std::fabs(snapped[1] - 4.f) < 0.3f);
    }

    // The slope partition splits a flight from its landing: no polygon runs
    // from the steps onto the level top, so the line where the flight tops
    // out is a polygon edge rather than something a fan of slivers crosses.
    template<> template<>
    void sknavbuild_object::test<18>()
    {
        TileBuildInput in = tile(0, 0);
        in.mTerrain = flat(0.f);
        in.mPartition = Partition::SLOPE;
        // twelve 0.25 m steps on 0.4 m treads up the y axis, then a landing
        for (S32 k = 0; k < 12; ++k)
        {
            in.mRecords.push_back(box(12.f, 8.f + k * 0.4f, 0.f, 15.f, 20.f, (k + 1) * 0.25f));
        }
        const F32 top = 8.f + 12 * 0.4f;
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);

        Mesh m;
        m.add(r.mData[0]);
        const dtMeshTile* dt = static_cast<const dtNavMesh*>(m.mNav)->getTileAt(0, 0, 0);
        ensure("tile", dt && dt->header);
        S32 on_flight = 0, on_landing = 0;
        for (S32 p = 0; p < dt->header->polyCount; ++p)
        {
            const dtPoly& poly = dt->polys[p];
            F32 ymin = 1e9f, ymax = -1e9f, zmin = 1e9f, xmid = 0.f;
            for (S32 v = 0; v < poly.vertCount; ++v)
            {
                const LLVector3 sl = fromRecast(&dt->verts[poly.verts[v] * 3]);
                ymin = llmin(ymin, sl.mV[VY]);
                ymax = llmax(ymax, sl.mV[VY]);
                zmin = llmin(zmin, sl.mV[VZ]);
                xmid += sl.mV[VX] / poly.vertCount;
            }
            if (xmid < 12.f || xmid > 15.f || zmin < 0.2f)
            {
                continue; // the ground around the stair
            }
            ensure(llformat("poly %d spans the top of the flight: y %.2f..%.2f", p, ymin, ymax),
                   ymax <= top + 0.5f || ymin >= top - 0.5f);
            ++(ymax <= top + 0.5f ? on_flight : on_landing);
        }
        ensure("polys on the flight", on_flight > 0);
        ensure("polys on the landing", on_landing > 0);
    }

    // A single step within climb is a floor edge, not a slope: the slope
    // partition gives a curb no band of its own, so the floor either side of
    // it breaks into no more polygons than the plain watershed makes.
    template<> template<>
    void sknavbuild_object::test<19>()
    {
        S32 polys[2] = { 0, 0 };
        for (S32 k = 0; k < 2; ++k)
        {
            TileBuildInput in = tile(0, 0);
            in.mTerrain = flat(0.f);
            in.mPartition = k ? Partition::SLOPE : Partition::WATERSHED;
            in.mRecords.push_back(box(4.f, 4.f, 0.f, 28.f, 28.f, 0.5f));
            in.mRecords.push_back(box(4.f, 16.f, 0.5f, 28.f, 28.f, 0.86f)); // a 0.36 m curb across the floor
            const TileBuildResult r = buildTile(in);
            ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
            polys[k] = r.mPolys[0];
        }
        ensure(llformat("curb adds polygons: watershed %d, slope %d", polys[0], polys[1]), polys[1] <= polys[0]);
    }
}
