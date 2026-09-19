/**
 * @file sknavrecast_test.cpp
 * @brief The recastnavigation port links and runs: a plane, a box, a path around it.
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

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    // Recast is y-up. The scene is a 16 x 16 m floor at y = 0 with a 2 m wide,
    // 2 m tall, 6 m long block across the middle: too tall to climb, too long
    // to be walked through, so a path from one side to the other must go round.
    constexpr float FLOOR = 16.f;
    constexpr float BOX_X0 = 7.f, BOX_X1 = 9.f;
    constexpr float BOX_Y1 = 2.f;
    constexpr float BOX_Z0 = 5.f, BOX_Z1 = 11.f;

    constexpr float AGENT_HEIGHT = 2.f;
    constexpr float AGENT_RADIUS = 0.3f;
    constexpr float AGENT_CLIMB = 0.75f;

    struct Soup
    {
        std::vector<float> verts;
        std::vector<int> tris;

        int add(float x, float y, float z)
        {
            verts.push_back(x);
            verts.push_back(y);
            verts.push_back(z);
            return static_cast<int>(verts.size() / 3) - 1;
        }

        // Two triangles over a quad; a, b, c, d counter-clockwise seen from
        // the side the normal should face.
        void quad(int a, int b, int c, int d)
        {
            tris.insert(tris.end(), { a, b, c, a, c, d });
        }

        void box(float x0, float y0, float z0, float x1, float y1, float z1)
        {
            const int v000 = add(x0, y0, z0), v001 = add(x0, y0, z1), v011 = add(x0, y1, z1), v010 = add(x0, y1, z0);
            const int v100 = add(x1, y0, z0), v101 = add(x1, y0, z1), v111 = add(x1, y1, z1), v110 = add(x1, y1, z0);
            quad(v010, v011, v111, v110); // top, normal +y
            quad(v000, v100, v101, v001); // bottom, normal -y
            quad(v000, v001, v011, v010); // -x
            quad(v100, v110, v111, v101); // +x
            quad(v000, v010, v110, v100); // -z
            quad(v001, v101, v111, v011); // +z
        }

        int nverts() const { return static_cast<int>(verts.size() / 3); }
        int ntris() const { return static_cast<int>(tris.size() / 3); }
    };

    Soup scene()
    {
        Soup s;
        const int a = s.add(0.f, 0.f, 0.f), b = s.add(0.f, 0.f, FLOOR), c = s.add(FLOOR, 0.f, FLOOR), d = s.add(FLOOR, 0.f, 0.f);
        s.quad(a, b, c, d); // floor, normal +y
        s.box(BOX_X0, 0.f, BOX_Z0, BOX_X1, BOX_Y1, BOX_Z1);
        return s;
    }

    // The whole pipeline, as RecastDemo's solo mesh sample runs it, into one
    // Detour tile. Every stage's failure is a test failure with the stage named.
    struct Built
    {
        dtNavMesh* nav = nullptr;
        dtNavMeshQuery* query = nullptr;
        int polys = 0;

        ~Built()
        {
            dtFreeNavMeshQuery(query);
            dtFreeNavMesh(nav);
        }
    };

    void build(const Soup& soup, Built& out)
    {
        rcContext ctx(false);

        rcConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.cs = 0.125f;
        cfg.ch = 0.125f;
        cfg.walkableSlopeAngle = 45.f;
        cfg.walkableHeight = static_cast<int>(std::ceil(AGENT_HEIGHT / cfg.ch));
        cfg.walkableClimb = static_cast<int>(std::floor(AGENT_CLIMB / cfg.ch));
        cfg.walkableRadius = static_cast<int>(std::ceil(AGENT_RADIUS / cfg.cs));
        cfg.maxEdgeLen = static_cast<int>(12.f / cfg.cs);
        cfg.maxSimplificationError = 1.3f;
        cfg.minRegionArea = 8 * 8;
        cfg.mergeRegionArea = 20 * 20;
        cfg.maxVertsPerPoly = 6;
        cfg.detailSampleDist = 6.f * cfg.cs;
        cfg.detailSampleMaxError = 2.f * cfg.ch;
        rcCalcBounds(soup.verts.data(), soup.nverts(), cfg.bmin, cfg.bmax);
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

        rcHeightfield* hf = rcAllocHeightfield();
        tut::ensure("heightfield", rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch));

        std::vector<unsigned char> areas(soup.ntris(), 0);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, soup.verts.data(), soup.nverts(), soup.tris.data(), soup.ntris(), areas.data());
        tut::ensure("rasterize", rcRasterizeTriangles(&ctx, soup.verts.data(), soup.nverts(), soup.tris.data(), areas.data(), soup.ntris(), *hf, cfg.walkableClimb));

        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        tut::ensure("compact", rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf));
        rcFreeHeightField(hf);
        tut::ensure("erode", rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf));
        tut::ensure("distance field", rcBuildDistanceField(&ctx, *chf));
        tut::ensure("regions", rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea));

        rcContourSet* cset = rcAllocContourSet();
        tut::ensure("contours", rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset));

        rcPolyMesh* pmesh = rcAllocPolyMesh();
        tut::ensure("polymesh", rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh));

        rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
        tut::ensure("detail mesh", rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh));
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);

        // Detour drops polys whose flags are zero from every query.
        for (int i = 0; i < pmesh->npolys; ++i)
        {
            pmesh->flags[i] = (pmesh->areas[i] == RC_WALKABLE_AREA) ? 1 : 0;
        }
        out.polys = pmesh->npolys;

        dtNavMeshCreateParams params;
        std::memset(&params, 0, sizeof(params));
        params.verts = pmesh->verts;
        params.vertCount = pmesh->nverts;
        params.polys = pmesh->polys;
        params.polyAreas = pmesh->areas;
        params.polyFlags = pmesh->flags;
        params.polyCount = pmesh->npolys;
        params.nvp = pmesh->nvp;
        params.detailMeshes = dmesh->meshes;
        params.detailVerts = dmesh->verts;
        params.detailVertsCount = dmesh->nverts;
        params.detailTris = dmesh->tris;
        params.detailTriCount = dmesh->ntris;
        params.walkableHeight = AGENT_HEIGHT;
        params.walkableRadius = AGENT_RADIUS;
        params.walkableClimb = AGENT_CLIMB;
        rcVcopy(params.bmin, pmesh->bmin);
        rcVcopy(params.bmax, pmesh->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;

        unsigned char* data = nullptr;
        int size = 0;
        tut::ensure("navmesh data", dtCreateNavMeshData(&params, &data, &size));
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);

        out.nav = dtAllocNavMesh();
        tut::ensure("navmesh init", dtStatusSucceed(out.nav->init(data, size, DT_TILE_FREE_DATA)));
        out.query = dtAllocNavMeshQuery();
        tut::ensure("query init", dtStatusSucceed(out.query->init(out.nav, 2048)));
    }

    dtPolyRef nearest(const Built& b, const float* pos, const float* extents, float* snapped)
    {
        dtQueryFilter filter;
        dtPolyRef ref = 0;
        tut::ensure("nearest poly", dtStatusSucceed(b.query->findNearestPoly(pos, extents, &filter, &ref, snapped)));
        return ref;
    }
}

namespace tut
{
    struct sknavrecast_data
    {
        Built built;
        sknavrecast_data() { build(scene(), built); }
    };

    typedef test_group<sknavrecast_data> sknavrecast_group;
    typedef sknavrecast_group::object sknavrecast_object;
    tut::sknavrecast_group g("sknavrecast");

    // The pipeline produces a mesh at all.
    template<> template<>
    void sknavrecast_object::test<1>()
    {
        ensure("polys built", built.polys > 0);
    }

    // A path from one side of the block to the other goes round it, not
    // through it, and is longer than the straight line by the detour.
    template<> template<>
    void sknavrecast_object::test<2>()
    {
        const float extents[3] = { 1.f, 2.f, 1.f };
        const float start[3] = { 2.f, 0.f, 8.f };
        const float end[3] = { 14.f, 0.f, 8.f };
        float s[3], e[3];
        const dtPolyRef sref = nearest(built, start, extents, s);
        const dtPolyRef eref = nearest(built, end, extents, e);
        ensure("start on mesh", sref != 0);
        ensure("end on mesh", eref != 0);

        dtQueryFilter filter;
        dtPolyRef path[256];
        int npath = 0;
        const dtStatus status = built.query->findPath(sref, eref, s, e, &filter, path, &npath, 256);
        ensure("findPath", dtStatusSucceed(status));
        ensure("path is complete", !dtStatusDetail(status, DT_PARTIAL_RESULT));
        ensure("path crosses polys", npath > 1);

        float straight[256 * 3];
        unsigned char flags[256];
        dtPolyRef refs[256];
        int nstraight = 0;
        ensure("findStraightPath", dtStatusSucceed(built.query->findStraightPath(s, e, path, npath, straight, flags, refs, &nstraight, 256)));
        ensure("straight path has corners", nstraight >= 3);

        float length = 0.f;
        for (int i = 1; i < nstraight; ++i)
        {
            const float* p = &straight[i * 3];
            ensure("corner clear of the block", !(p[0] > BOX_X0 && p[0] < BOX_X1 && p[2] > BOX_Z0 && p[2] < BOX_Z1));
            const float* q = &straight[(i - 1) * 3];
            length += std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]));
        }
        ensure("path detours", length > 13.f);
    }

    // The block's top is floor of its own: a 2 m step is above the climb
    // height, so the ground cannot reach it and the result is partial.
    template<> template<>
    void sknavrecast_object::test<3>()
    {
        const float extents[3] = { 0.5f, 0.5f, 0.5f };
        const float top[3] = { 8.f, BOX_Y1, 8.f };
        const float ground[3] = { 2.f, 0.f, 8.f };
        float t[3], g[3];
        const dtPolyRef tref = nearest(built, top, extents, t);
        const dtPolyRef gref = nearest(built, ground, extents, g);
        ensure("block top is floor", tref != 0);
        ensure("top is at block height", std::fabs(t[1] - BOX_Y1) < 0.2f);

        dtQueryFilter filter;
        dtPolyRef path[256];
        int npath = 0;
        const dtStatus status = built.query->findPath(gref, tref, g, t, &filter, path, &npath, 256);
        ensure("findPath to the top", dtStatusSucceed(status));
        ensure("top is unreachable from the ground", dtStatusDetail(status, DT_PARTIAL_RESULT));
    }
}
