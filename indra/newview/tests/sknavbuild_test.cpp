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
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
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
            p.maxTiles = 64;
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
}
