/**
 * @file sknavpolys_test.cpp
 * @brief A built tile's polygons copied out of Detour: counts, areas, neighbours, borders, detail.
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

#include "../sknavpolys.h"

#include "sknavbuild.h"

#include "DetourNavMesh.h"

#include <cmath>
#include <cstring>

namespace
{
    using namespace sknav;

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

    // Reaches past the tile's border cells so Recast leaves portals on every side.
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

    TileBuildInput tileInput()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 12.f;
        in.mCell = 0.25f;
        in.mEdges[0] = true;
        in.mTerrain = flat(0.f);
        // A platform too high to step onto, so its edge is a ledge with drop links.
        in.mRecords.push_back(box(12.f, 8.f, 0.f, 20.f, 24.f, 1.6f));
        return in;
    }

    struct Fixture
    {
        TileBuildResult mResult;
        dtNavMesh* mNav = nullptr;
        const dtMeshTile* mTile = nullptr;
        std::shared_ptr<const TilePolys> mPolys;

        Fixture()
        {
            mResult = buildTile(tileInput());
            mNav = dtAllocNavMesh();
            dtNavMeshParams p;
            p.orig[0] = p.orig[1] = p.orig[2] = 0.f;
            p.tileWidth = 32.f;
            p.tileHeight = 32.f;
            p.maxTiles = 1024;
            p.maxPolys = 4096;
            mNav->init(&p);
            const std::vector<U8>& data = mResult.mData[0];
            if (!data.empty())
            {
                unsigned char* copy = static_cast<unsigned char*>(dtAlloc(static_cast<int>(data.size()), DT_ALLOC_PERM));
                std::memcpy(copy, data.data(), data.size());
                mNav->addTile(copy, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, nullptr);
            }
            mTile = mNav->getTileAt(0, 0, 0);
            if (mTile)
            {
                mPolys = extractTilePolys(*mTile);
            }
        }
        ~Fixture()
        {
            dtFreeNavMesh(mNav);
        }
    };
}

namespace tut
{
    struct sknavpolys_data
    {
        Fixture mF;
    };
    typedef test_group<sknavpolys_data> sknavpolys_group;
    typedef sknavpolys_group::object sknavpolys_object;
    tut::sknavpolys_group g("sknavpolys");

    // The extracted count is Recast's: Detour's off-mesh polys are not in it.
    template<> template<>
    void sknavpolys_object::test<1>()
    {
        ensure("built: " + mF.mResult.mError, mF.mResult.mStatus == TileBuildResult::SUCCESS);
        ensure("tile in Detour", mF.mTile != nullptr);
        ensure("polys", mF.mPolys && !mF.mPolys->mPolys.empty());
        ensure_equals("poly count", static_cast<S32>(mF.mPolys->mPolys.size()), mF.mResult.mPolys[0]);
    }

    // Every area is one of the three walkable ones, and every vertex index is in range.
    template<> template<>
    void sknavpolys_object::test<2>()
    {
        const TilePolys& tp = *mF.mPolys;
        for (const TilePolys::Poly& p : tp.mPolys)
        {
            ensure("area", p.mArea == AREA_GROUND || p.mArea == AREA_OBJECT || p.mArea == AREA_SUBMERGED);
            ensure("vertex count", p.mVertCount >= 3 && p.mVertCount <= TilePolys::MAX_VERTS);
            for (S32 v = 0; v < p.mVertCount; ++v)
            {
                ensure("vertex index", p.mVerts[v] < tp.mVerts.size());
            }
        }
    }

    // Internal neighbours point back, through the same shared edge.
    template<> template<>
    void sknavpolys_object::test<3>()
    {
        const TilePolys& tp = *mF.mPolys;
        S32 internal = 0;
        for (size_t i = 0; i < tp.mPolys.size(); ++i)
        {
            const TilePolys::Poly& p = tp.mPolys[i];
            for (S32 v = 0; v < p.mVertCount; ++v)
            {
                const S16 n = p.mNeighbour[v];
                if (n < 0)
                {
                    continue;
                }
                ++internal;
                ensure("neighbour in range", static_cast<size_t>(n) < tp.mPolys.size());
                const TilePolys::Poly& q = tp.mPolys[n];
                bool back = false;
                for (S32 w = 0; w < q.mVertCount; ++w)
                {
                    back = back || q.mNeighbour[w] == static_cast<S16>(i);
                }
                ensure("symmetric", back);
            }
        }
        ensure("some internal edges", internal > 0);
    }

    // An edge flagged as border lies on the tile's own edge.
    template<> template<>
    void sknavpolys_object::test<4>()
    {
        const TilePolys& tp = *mF.mPolys;
        S32 borders = 0;
        for (const TilePolys::Poly& p : tp.mPolys)
        {
            for (S32 v = 0; v < p.mVertCount; ++v)
            {
                if (p.mNeighbour[v] != TilePolys::EDGE_BORDER)
                {
                    continue;
                }
                ++borders;
                const LLVector3& a = tp.mVerts[p.mVerts[v]];
                const LLVector3& b = tp.mVerts[p.mVerts[(v + 1) % p.mVertCount]];
                bool on_line = false;
                for (S32 axis = 0; axis < 2; ++axis)
                {
                    for (F32 line : { 0.f, 32.f })
                    {
                        on_line = on_line || (std::fabs(a.mV[axis] - line) < 0.02f && std::fabs(b.mV[axis] - line) < 0.02f);
                    }
                }
                ensure("border edge on the tile edge", on_line);
            }
        }
        ensure("some border edges", borders > 0);
    }

    // Detail triangles exist and lie inside their poly's z range.
    template<> template<>
    void sknavpolys_object::test<5>()
    {
        const TilePolys& tp = *mF.mPolys;
        S32 tris = 0;
        for (const TilePolys::Poly& p : tp.mPolys)
        {
            ensure("z range ordered", p.mZMin <= p.mZMax);
            ensure("triangle range", (p.mFirstTri + p.mTriCount) * 3 <= tp.mTris.size());
            for (U32 t = 0; t < p.mTriCount * 3u; ++t)
            {
                const F32 z = tp.mTris[(p.mFirstTri * 3) + t].mV[VZ];
                ensure("detail z within the poly's range", z >= p.mZMin - 1e-4f && z <= p.mZMax + 1e-4f);
            }
            tris += p.mTriCount;
        }
        ensure("detail triangles", tris > 0);
    }

    // Off-mesh connections are left out; none of their area shows up.
    template<> template<>
    void sknavpolys_object::test<6>()
    {
        ensure("tile", mF.mTile && mF.mTile->header);
        const dtMeshHeader& h = *mF.mTile->header;
        ensure_equals("ground polys only", static_cast<S32>(mF.mPolys->mPolys.size()), h.polyCount - h.offMeshConCount);
        for (const TilePolys::Poly& p : mF.mPolys->mPolys)
        {
            ensure("no drop area", p.mArea != AREA_DROP);
        }
    }
}
