/**
 * @file skmapraster_test.cpp
 * @brief Mini-map cell bakes: fills, painter order, water, halos, contours, rungs, chevrons.
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

#include "../skmapraster.h"

#include "sknavbuild.h"

#include <cmath>

namespace
{
    using namespace sknav;
    using skmap::TileInput;

    const LLUUID LAND("11111111-1111-1111-1111-111111111111");
    const LLUUID CLUSTER("22222222-2222-2222-2222-222222222222");

    struct TileMaker
    {
        std::shared_ptr<TilePolys> mPolys = std::make_shared<TilePolys>();
        std::vector<Flight> mFlights;
        std::vector<WallSegment> mSegments;

        S32 quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 z_west, F32 z_east, U8 area = AREA_OBJECT)
        {
            TilePolys::Poly p;
            p.mArea = area;
            p.mVertCount = 4;
            const U16 base = static_cast<U16>(mPolys->mVerts.size());
            const LLVector3 v[4] = { LLVector3(x0, y0, z_west), LLVector3(x1, y0, z_east), LLVector3(x1, y1, z_east), LLVector3(x0, y1, z_west) };
            for (S32 k = 0; k < 4; ++k)
            {
                mPolys->mVerts.push_back(v[k]);
                p.mVerts[k] = base + k;
                p.mNeighbour[k] = TilePolys::EDGE_NONE;
            }
            p.mFirstTri = static_cast<U32>(mPolys->mTris.size() / 3);
            p.mTriCount = 2;
            const S32 tris[6] = { 0, 1, 2, 0, 2, 3 };
            for (S32 t : tris)
            {
                mPolys->mTris.push_back(v[t]);
            }
            p.mZMin = llmin(z_west, z_east);
            p.mZMax = llmax(z_west, z_east);
            mPolys->mPolys.push_back(p);
            return static_cast<S32>(mPolys->mPolys.size()) - 1;
        }
        void join(S32 a, S32 ea, S32 b, S32 eb)
        {
            mPolys->mPolys[a].mNeighbour[ea] = static_cast<S16>(b);
            mPolys->mPolys[b].mNeighbour[eb] = static_cast<S16>(a);
        }
        void wall(F32 x0, F32 y0, F32 x1, F32 y1, EdgeKind kind, F32 value)
        {
            WallSegment w;
            w.mA.set(x0, y0, 0.f);
            w.mB.set(x1, y1, 0.f);
            w.mKind = kind;
            w.mValue = value;
            mSegments.push_back(w);
        }
        skmap::ScenePtr scene(const LLUUID& mesh, bool terrain, F32 water = -100.f) const
        {
            TileInput t;
            t.mMesh = mesh;
            t.mTerrain = terrain;
            t.mContentKey = 42;
            t.mMin.set(0.0, 0.0, -10.0);
            t.mMax.set(32.0, 32.0, 100.0);
            t.mWater = water;
            t.mPolys = mPolys;
            t.mSegments = std::make_shared<const std::vector<WallSegment>>(mSegments);
            t.mLinks = std::make_shared<const std::vector<DropLink>>();
            t.mFlights = mFlights.empty() ? nullptr : std::make_shared<const std::vector<Flight>>(mFlights);
            skmap::SceneInput in;
            in.mMin.set(-64.0, -64.0, -1.0e4);
            in.mMax.set(128.0, 128.0, 1.0e5);
            in.mTiles.push_back(t);
            in.mRegions.push_back({ LLVector3d(0.0, 0.0, 0.0), 256.0 });
            return skmap::buildScene(in);
        }
    };

    struct Raster
    {
        std::vector<U8> mPx;
        skmap::RasterStats mStats;
        const U8* at(S32 x, S32 y) const { return &mPx[(static_cast<size_t>(y) * skmap::CELL_TEXELS + x) * 4]; }
    };

    Raster bake(const skmap::Scene& s, S32 layer, const skmap::CellId& cell, const skmap::RasterParams& params = skmap::RasterParams())
    {
        Raster r;
        skmap::rasterCell(s, layer, cell, params, r.mPx, &r.mStats);
        return r;
    }

    S32 brightness(const U8* p)
    {
        return p[0] + p[1] + p[2];
    }

    /// The texel as it shows over a light world behind the map.
    F32 overWhite(const U8* p)
    {
        const F32 a = p[3] / 255.f;
        return (p[0] + p[1] + p[2]) / 255.f * a + 3.f * (1.f - a);
    }

    /// The straight colour's luma, as it shows over a dark world.
    F32 luma(const U8* p)
    {
        return (0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]) / 255.f;
    }

    bool sameColour(const U8* a, const U8* b)
    {
        return std::abs(a[0] - b[0]) <= 1 && std::abs(a[1] - b[1]) <= 1 && std::abs(a[2] - b[2]) <= 1;
    }

    const skmap::CellId TIER0 = { 0, 0, 0 };
}

namespace tut
{
    struct skmapraster_data {};
    typedef test_group<skmapraster_data> skmapraster_group;
    typedef skmapraster_group::object skmapraster_object;
    tut::skmapraster_group g("skmapraster");

    // A square fills its own texels, within a row.
    template<> template<>
    void skmapraster_object::test<1>()
    {
        TileMaker t;
        t.quad(2.f, 2.f, 6.f, 6.f, 0.f, 0.f);
        skmap::ScenePtr s = t.scene(CLUSTER, false);
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        ensure_equals("bytes", r.mPx.size(), static_cast<size_t>(256 * 256 * 4));
        for (S32 y = 0; y < 256; y += 3)
        {
            for (S32 x = 0; x < 256; x += 3)
            {
                const bool inside = x >= 65 && x <= 190 && y >= 65 && y <= 190;
                const bool outside = x < 63 || x > 192 || y < 63 || y > 192;
                if (inside)
                {
                    ensure("filled inside", r.at(x, y)[3] > 0);
                }
                else if (outside)
                {
                    ensure_equals("clear outside", static_cast<S32>(r.at(x, y)[3]), 0);
                }
            }
        }
        // The wash over the backing: a = 0.776.
        ensure("backing alpha", std::abs(r.at(128, 128)[3] - 198) <= 1);
    }

    // The higher polygon wins where two overlap.
    template<> template<>
    void skmapraster_object::test<2>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 8.f, 0.f, 0.f, AREA_OBJECT);
        t.quad(2.f, 2.f, 6.f, 6.f, 0.5f, 0.5f, AREA_GROUND);
        skmap::ScenePtr s = t.scene(CLUSTER, false);
        ensure("one layer", s->mPolys[0].mLayer == s->mPolys[1].mLayer);
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        const U8* top = r.at(128, 128);
        const U8* low = r.at(32, 32);
        ensure("the green floor wins on top", top[1] > top[0] + 5);
        ensure("grey below", std::abs(static_cast<S32>(low[1]) - static_cast<S32>(low[0])) < 10);
    }

    // Water darkens and grows more opaque with depth.
    template<> template<>
    void skmapraster_object::test<3>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 8.f, -0.5f, -3.9f, AREA_SUBMERGED);
        skmap::ScenePtr s = t.scene(CLUSTER, false, 0.f);
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        const U8* shallow = r.at(8, 128);
        const U8* deep = r.at(248, 128);
        ensure("deeper is more opaque", deep[3] > shallow[3]);
        ensure("deeper is darker over the world", overWhite(deep) < overWhite(shallow));
        ensure("blue", shallow[2] > shallow[0]);

        // At full depth the straight colour is darker too, so deep water reads darker over a dark world.
        TileMaker two;
        two.quad(0.f, 0.f, 8.f, 4.f, -0.05f, -0.05f, AREA_SUBMERGED);
        two.quad(0.f, 4.f, 8.f, 8.f, -7.f, -7.f, AREA_SUBMERGED);
        skmap::ScenePtr s2 = two.scene(CLUSTER, false, 0.f);
        ensure("two layers", s2->mPolys[0].mLayer != s2->mPolys[1].mLayer);
        const Raster at_surface = bake(*s2, s2->mPolys[0].mLayer, TIER0);
        const Raster at_depth = bake(*s2, s2->mPolys[1].mLayer, TIER0);
        ensure("deep is darker in straight luma", luma(at_depth.at(128, 192)) < luma(at_surface.at(128, 64)));
    }

    // A ledge's halo lightens only off its floor; a wall's darkens only on it.
    template<> template<>
    void skmapraster_object::test<4>()
    {
        TileMaker ledge;
        ledge.quad(0.f, 0.f, 4.f, 8.f, 0.f, 0.f);
        ledge.wall(4.f, 0.f, 4.f, 8.f, EdgeKind::LEDGE, 3.f);
        skmap::ScenePtr s = ledge.scene(CLUSTER, false);
        ensure("ledge mark", s->mMarks.size() == 1 && s->mMarks[0].mKind == skmap::MarkKind::LEDGE);
        Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        ensure("light past the ledge", r.at(134, 128)[3] > 0 && r.at(134, 128)[0] > 240);
        ensure("floor untouched by it", std::equal(r.at(121, 128), r.at(121, 128) + 4, r.at(32, 128)));

        TileMaker wall;
        wall.quad(0.f, 0.f, 4.f, 8.f, 0.f, 0.f);
        wall.wall(4.f, 0.f, 4.f, 8.f, EdgeKind::WALL, 3.f);
        s = wall.scene(CLUSTER, false);
        r = bake(*s, s->mPolys[0].mLayer, TIER0);
        ensure("dark on the floor", brightness(r.at(125, 128)) < brightness(r.at(32, 128)));
        ensure_equals("nothing past the wall", static_cast<S32>(r.at(134, 128)[3]), 0);
    }

    // Contours draw only at multiples of the tier's step.
    template<> template<>
    void skmapraster_object::test<5>()
    {
        ensure_equals("fine", skmap::contourStep(1.f / 32.f), 0.5f);
        ensure_equals("1 m", skmap::contourStep(1.f / 16.f), 1.f);
        ensure_equals("2 m", skmap::contourStep(0.125f), 2.f);
        ensure_equals("5 m", skmap::contourStep(0.25f), 5.f);
        ensure_equals("20 m", skmap::contourStep(1.f), 20.f);

        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 8.f, 0.f, 3.9f, AREA_GROUND);
        skmap::ScenePtr s = t.scene(LAND, true);
        const S32 layer = s->mPolys[0].mLayer;
        Raster fine = bake(*s, layer, TIER0);
        ensure("every 0.5 m", fine.mStats.mContourLevels == std::vector<S32>({ 1, 2, 3, 4, 5, 6, 7 }));
        Raster coarse = bake(*s, layer, skmap::CellId{ 2, 0, 0 });
        ensure("every 2 m", coarse.mStats.mContourLevels == std::vector<S32>({ 4 }));
        skmap::RasterParams off;
        off.mContours = false;
        ensure("switched off", bake(*s, layer, TIER0, off).mStats.mContourLevels.empty());
        ensure("terrain gets no chevrons", fine.mStats.mChevrons == 0);
    }

    // A stair with six treads draws six rungs at a fine tier and fewer coarser; no treads is a ramp.
    template<> template<>
    void skmapraster_object::test<6>()
    {
        auto run = [](ConnectorKind kind, U8 treads) {
            TileMaker t;
            const S32 lower = t.quad(0.f, 0.f, 10.f, 8.f, 0.f, 0.f);
            const S32 flight = t.quad(10.f, 0.f, 14.f, 8.f, 0.f, 3.f);
            const S32 upper = t.quad(14.f, 0.f, 24.f, 8.f, 3.f, 3.f);
            t.join(lower, 1, flight, 3);
            t.join(flight, 1, upper, 3);
            Flight f;
            f.mKind = kind;
            f.mTreads = treads;
            f.mBottomA.set(10.f, 0.f, 0.f);
            f.mBottomB.set(10.f, 8.f, 0.f);
            f.mTopA.set(14.f, 0.f, 3.f);
            f.mTopB.set(14.f, 8.f, 3.f);
            f.mRun = 4.f;
            f.mRise = 3.f;
            f.mFootprint = { LLVector2(10.f, 0.f), LLVector2(14.f, 0.f), LLVector2(14.f, 8.f), LLVector2(10.f, 8.f) };
            f.mPolys = { static_cast<U16>(flight) };
            f.mKey = 7;
            t.mFlights.push_back(f);
            return t.scene(CLUSTER, false);
        };
        skmap::ScenePtr stair = run(ConnectorKind::STAIR, 6);
        const S32 layer = stair->mRuns[0].mLayer;
        const Raster fine = bake(*stair, layer, skmap::CellId{ 0, 1, 0 });
        ensure_equals("six rungs", fine.mStats.mRungs, 6);
        const Raster coarse = bake(*stair, layer, skmap::CellId{ 3, 0, 0 });
        ensure("fewer when coarse", coarse.mStats.mRungs > 0 && coarse.mStats.mRungs < 6);
        skmap::RasterParams off;
        off.mStairs = false;
        ensure_equals("stairs off", bake(*stair, layer, skmap::CellId{ 0, 1, 0 }, off).mStats.mRungs, 0);
        ensure("no chevrons on a stair", fine.mStats.mChevrons == 0);

        skmap::ScenePtr ramp = run(ConnectorKind::STAIR, 0);
        const Raster chevrons = bake(*ramp, ramp->mRuns[0].mLayer, skmap::CellId{ 0, 1, 0 });
        ensure("no treads is a ramp", chevrons.mStats.mChevrons > 0 && chevrons.mStats.mRungs == 0);
    }

    // The same input bakes the same bytes.
    template<> template<>
    void skmapraster_object::test<7>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 8.f, 0.f, 3.f, AREA_GROUND);
        t.quad(1.f, 1.f, 3.f, 3.f, 1.f, 1.f);
        t.wall(8.f, 0.f, 8.f, 8.f, EdgeKind::WALL, 2.f);
        skmap::ScenePtr a = t.scene(LAND, true);
        skmap::ScenePtr b = t.scene(LAND, true);
        for (S32 l = 0; l < static_cast<S32>(a->mLayers.size()); ++l)
        {
            ensure("identical", bake(*a, l, TIER0).mPx == bake(*b, l, TIER0).mPx);
        }
    }

    // Only ramps are shaded by height: sloped terrain and a floor above its layer's base are not.
    template<> template<>
    void skmapraster_object::test<8>()
    {
        skmap::RasterParams no_contours;
        no_contours.mContours = false;

        // Land at 0, a terrain slope up 3 m and land at 3, one surface whose base is 0.
        TileMaker land;
        const S32 low = land.quad(0.f, 0.f, 6.f, 8.f, 0.f, 0.f, AREA_GROUND);
        const S32 slope = land.quad(6.f, 0.f, 7.f, 8.f, 0.f, 3.f, AREA_GROUND);
        const S32 high = land.quad(7.f, 0.f, 8.f, 8.f, 3.f, 3.f, AREA_GROUND);
        land.join(low, 1, slope, 3);
        land.join(slope, 1, high, 3);
        skmap::ScenePtr s = land.scene(LAND, true);
        const S32 ground = s->mPolys[0].mLayer;
        ensure("one ground layer", s->mPolys[1].mLayer == ground && s->mPolys[2].mLayer == ground && s->mLayers[ground].mBaseZ == 0.f);
        const Raster r = bake(*s, ground, TIER0, no_contours);
        ensure("the slope's ends match", sameColour(r.at(194, 128), r.at(222, 128)));
        ensure("the floor 3 m up matches the base", sameColour(r.at(240, 128), r.at(64, 128)));

        // A ramp from 0 to 3 m still brightens uphill.
        TileMaker t;
        const S32 lower = t.quad(0.f, 0.f, 10.f, 8.f, 0.f, 0.f);
        const S32 flight = t.quad(10.f, 0.f, 14.f, 8.f, 0.f, 3.f);
        const S32 upper = t.quad(14.f, 0.f, 24.f, 8.f, 3.f, 3.f);
        t.join(lower, 1, flight, 3);
        t.join(flight, 1, upper, 3);
        Flight f;
        f.mKind = ConnectorKind::RAMP;
        f.mBottomA.set(10.f, 0.f, 0.f);
        f.mBottomB.set(10.f, 8.f, 0.f);
        f.mTopA.set(14.f, 0.f, 3.f);
        f.mTopB.set(14.f, 8.f, 3.f);
        f.mRun = 4.f;
        f.mRise = 3.f;
        f.mFootprint = { LLVector2(10.f, 0.f), LLVector2(14.f, 0.f), LLVector2(14.f, 8.f), LLVector2(10.f, 8.f) };
        f.mPolys = { static_cast<U16>(flight) };
        f.mKey = 7;
        t.mFlights.push_back(f);
        skmap::ScenePtr ramp = t.scene(CLUSTER, false);
        ensure("a ramp", ramp->mRuns.size() == 1 && !ramp->mRuns[0].mStair);
        // Row 16 is y = 0.5 m, clear of the chevrons.
        const Raster up = bake(*ramp, ramp->mRuns[0].mLayer, skmap::CellId{ 0, 1, 0 }, no_contours);
        ensure("the ramp brightens uphill", brightness(up.at(189, 16)) > brightness(up.at(67, 16)) + 6);
    }
}
