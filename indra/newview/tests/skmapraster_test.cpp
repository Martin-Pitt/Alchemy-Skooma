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
        skmap::ScenePtr scene(const LLUUID& mesh, bool terrain, F32 water = -100.f, const std::vector<RecordPtr>* records = nullptr,
                              const std::vector<skmap::DoorInput>& doors = {}) const
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
            if (records)
            {
                in.mRecords = std::make_shared<const std::vector<RecordPtr>>(*records);
            }
            in.mDoors = doors;
            return skmap::buildScene(in);
        }
        skmap::ScenePtr section(const std::vector<RecordPtr>& records, const std::vector<skmap::DoorInput>& doors = {}) const
        {
            return scene(CLUSTER, false, -100.f, &records, doors);
        }
    };

    LLUUID partId(U64 signature)
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "00000000-0000-0000-0000-%012llx", static_cast<unsigned long long>(signature));
        return LLUUID(buf);
    }

    Record boxOf(const LLVector3& c, const LLVector3& h, U64 signature, const LLVector3& a0 = LLVector3(1.f, 0.f, 0.f),
                 const LLVector3& a1 = LLVector3(0.f, 1.f, 0.f), const LLVector3& a2 = LLVector3(0.f, 0.f, 1.f))
    {
        Record r;
        r.mRole = Role::NAV;
        r.mShape = Shape::BOX;
        r.mSolid = true;
        r.mCenter = c;
        r.mHalf = h;
        r.mAxes[0] = a0;
        r.mAxes[1] = a1;
        r.mAxes[2] = a2;
        r.mMin.set(DBL_MAX, DBL_MAX, DBL_MAX);
        r.mMax.set(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (S32 i = 0; i < 8; ++i)
        {
            LLVector3 p = c;
            for (S32 b = 0; b < 3; ++b)
            {
                p += r.mAxes[b] * (((i >> b) & 1) ? h.mV[b] : -h.mV[b]);
            }
            for (S32 k = 0; k < 3; ++k)
            {
                r.mMin.mdV[k] = llmin(r.mMin.mdV[k], static_cast<F64>(p.mV[k]));
                r.mMax.mdV[k] = llmax(r.mMax.mdV[k], static_cast<F64>(p.mV[k]));
            }
        }
        r.mSignature = signature;
        r.mPart = partId(signature);
        return r;
    }

    RecordPtr box(const LLVector3& c, const LLVector3& h, U64 signature)
    {
        return std::make_shared<const Record>(boxOf(c, h, signature));
    }

    RecordPtr soupOf(const std::vector<LLVector3>& tris, bool solid, U64 signature)
    {
        Record r;
        r.mRole = Role::NAV;
        r.mShape = Shape::TRIANGLES;
        r.mSolid = solid;
        r.mTriangles = std::make_shared<const std::vector<LLVector3>>(tris);
        r.mMin.set(DBL_MAX, DBL_MAX, DBL_MAX);
        r.mMax.set(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (const LLVector3& p : tris)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                r.mMin.mdV[k] = llmin(r.mMin.mdV[k], static_cast<F64>(p.mV[k]));
                r.mMax.mdV[k] = llmax(r.mMax.mdV[k], static_cast<F64>(p.mV[k]));
            }
        }
        r.mSignature = signature;
        r.mPart = partId(signature);
        return std::make_shared<const Record>(r);
    }

    /// The scene-local centre of a texel of a cell, for a scene whose origin is 0.
    LLVector2 texelAt(const skmap::CellId& cell, S32 x, S32 y)
    {
        const LLVector3d lo = cell.minCorner();
        const F32 t = skmap::tierTexel(cell.mTier);
        return LLVector2(static_cast<F32>(lo.mdV[VX]) + (x + 0.5f) * t, static_cast<F32>(lo.mdV[VY]) + (y + 0.5f) * t);
    }

    struct Box2
    {
        F32 x0, y0, x1, y1;
        bool holds(const LLVector2& p, F32 inset = 0.f) const
        {
            return p.mV[VX] > x0 + inset && p.mV[VX] < x1 - inset && p.mV[VY] > y0 + inset && p.mV[VY] < y1 - inset;
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

    // A ledge's halo lightens only off its floor; a wall has no halo.
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
        ensure("floor untouched by the wall", std::equal(r.at(125, 128), r.at(125, 128) + 4, r.at(32, 128)));
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

    // The section moves every bake key once.
    template<> template<>
    void skmapraster_object::test<9>()
    {
        ensure_equals("raster version", skmap::RASTER_VERSION, 2u);
    }

    // Two overlapping walls are one solid: full throughout, no outline on the faces buried in each other.
    template<> template<>
    void skmapraster_object::test<10>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 3.5f, 0.f, 0.f);
        skmap::ScenePtr s = t.section({ box(LLVector3(4.f, 4.f, 1.5f), LLVector3(3.f, 0.15f, 1.5f), 1),
                                        box(LLVector3(6.85f, 6.f, 1.5f), LLVector3(0.15f, 2.15f, 1.5f), 2) });
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        ensure("classes recorded", r.mStats.mSectionClass.size() == 256u * 256u && r.mStats.mFullTexels > 0);
        const Box2 a = { 1.f, 3.85f, 7.f, 4.15f }, b = { 6.7f, 3.85f, 7.f, 8.15f };
        // Inside by more than so many texels; the outline band itself reaches up to two texels in.
        auto deep = [&](const LLVector2& p, F32 texels) {
            const F32 t1 = skmap::tierTexel(0) * texels * 1.01f;
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const LLVector2 q = p + LLVector2(dx * t1, dy * t1);
                    if (!a.holds(q) && !b.holds(q))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        S32 checked = 0, buried = 0;
        for (S32 y = 0; y < 256; ++y)
        {
            for (S32 x = 0; x < 256; ++x)
            {
                const LLVector2 p = texelAt(TIER0, x, y);
                const U8 bits = r.mStats.mSectionClass[y * 256 + x];
                const bool on_x = std::fabs(p.mV[VX] - 6.7f) < 0.02f && p.mV[VY] > 3.9f && p.mV[VY] < 4.1f;
                const bool on_y = std::fabs(p.mV[VY] - 4.15f) < 0.02f && p.mV[VX] > 6.75f && p.mV[VX] < 6.95f;
                if (on_x || on_y)
                {
                    ensure("full on a buried face", (bits & skmap::SECTION_TEXEL_FULL) != 0);
                    ensure("no outline on a buried face", (bits & skmap::SECTION_TEXEL_FULL_EDGE) == 0);
                    ++buried;
                }
                if (deep(p, 1.f))
                {
                    ensure("full inside the union", (bits & skmap::SECTION_TEXEL_FULL) != 0);
                    ++checked;
                }
                if (deep(p, 2.f))
                {
                    ensure("no outline inside the union", (bits & skmap::SECTION_TEXEL_FULL_EDGE) == 0);
                }
            }
        }
        ensure("texels checked", checked > 1000);
        ensure("the buried faces checked", buried > 4);
    }

    // A 2 cm sheet still covers every row it crosses at 0.5 m texels.
    template<> template<>
    void skmapraster_object::test<11>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 16.f, 4.5f, 0.f, 0.f);
        std::vector<RecordPtr> recs = { std::make_shared<const Record>(boxOf(LLVector3(8.f, 8.f, 1.5f), LLVector3(2.f, 0.01f, 1.5f), 1,
                                                                             LLVector3(0.866025404f, 0.5f, 0.f), LLVector3(-0.5f, 0.866025404f, 0.f))) };
        skmap::ScenePtr s = t.section(recs);
        const skmap::CellId cell = { 4, 0, 0 };
        const Raster r = bake(*s, s->mPolys[0].mLayer, cell);
        const F32 reach = 2.f * 0.5f + 0.01f * 0.866025404f;
        for (S32 y = static_cast<S32>(std::floor((8.f - reach) / 0.5f)); y <= static_cast<S32>(std::floor((8.f + reach) / 0.5f)); ++y)
        {
            bool full = false;
            for (S32 x = 0; x < 256; ++x)
            {
                full = full || (r.mStats.mSectionClass[y * 256 + x] & skmap::SECTION_TEXEL_FULL);
            }
            ensure("a full texel in every row", full);
        }
    }

    // A 1.1 m wall is half cover, hatched; the cover toggle hides the whole section.
    template<> template<>
    void skmapraster_object::test<12>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 3.f, 0.f, 0.f);
        skmap::ScenePtr s = t.section({ box(LLVector3(4.f, 4.f, 0.55f), LLVector3(2.f, 0.1f, 0.55f), 1) });
        const S32 layer = s->mPolys[0].mLayer;
        const Raster r = bake(*s, layer, TIER0);
        ensure("half texels", r.mStats.mHalfTexels > 0);
        ensure_equals("no full texels", r.mStats.mFullTexels, 0);
        S32 lo = 255, hi = 0;
        for (S32 x = 70; x < 186; ++x)
        {
            ensure("the wall's middle row is half", (r.mStats.mSectionClass[128 * 256 + x] & skmap::SECTION_TEXEL_HALF) != 0);
            lo = llmin(lo, static_cast<S32>(r.at(x, 128)[3]));
            hi = llmax(hi, static_cast<S32>(r.at(x, 128)[3]));
        }
        ensure("hatched", hi > lo + 10);

        skmap::RasterParams off;
        off.mSection = false;
        const Raster hidden = bake(*s, layer, TIER0, off);
        ensure_equals("no section with cover off", hidden.mStats.mHalfTexels + hidden.mStats.mFullTexels, 0);
        for (U8 bits : hidden.mStats.mSectionClass)
        {
            ensure_equals("no bits", static_cast<S32>(bits), 0);
        }
    }

    // Nothing is cut over walkable floor, except an island on its own top.
    template<> template<>
    void skmapraster_object::test<13>()
    {
        auto clear = [](const Raster& r, const Box2& area, const char* what) {
            S32 n = 0;
            for (S32 y = 0; y < 256; ++y)
            {
                for (S32 x = 0; x < 256; ++x)
                {
                    if (area.holds(texelAt(TIER0, x, y), skmap::tierTexel(0)))
                    {
                        ensure(what, r.mStats.mSectionClass[y * 256 + x] == 0);
                        ++n;
                    }
                }
            }
            ensure("texels looked at", n > 0);
        };
        auto bits = [](const Raster& r, F32 x, F32 y) {
            return r.mStats.mSectionClass[static_cast<S32>(y * 32.f) * 256 + static_cast<S32>(x * 32.f)];
        };

        // (a) A wedge under its own ramp: the hill's body is floor all the way.
        {
            std::vector<LLVector3> tris;
            for (const LLVector3& v : { LLVector3(0.f, 0.f, 0.f), LLVector3(8.f, 0.f, 0.f), LLVector3(8.f, 0.f, 8.f),
                                        LLVector3(0.f, 4.f, 0.f), LLVector3(8.f, 4.f, 8.f), LLVector3(8.f, 4.f, 0.f) })
            {
                tris.push_back(v);
            }
            auto quad = [&](const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d) {
                for (const LLVector3& v : { a, b, c, a, c, d })
                {
                    tris.push_back(v);
                }
            };
            const LLVector3 p0a(0.f, 0.f, 0.f), p1a(8.f, 0.f, 0.f), p2a(8.f, 0.f, 8.f), p0b(0.f, 4.f, 0.f), p1b(8.f, 4.f, 0.f), p2b(8.f, 4.f, 8.f);
            quad(p0a, p0b, p1b, p1a);
            quad(p1a, p1b, p2b, p2a);
            quad(p2a, p2b, p0b, p0a);
            TileMaker t;
            t.quad(0.f, 0.f, 8.f, 4.f, 0.f, 8.f);
            skmap::ScenePtr s = t.section({ soupOf(tris, true, 1) });
            const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
            ensure("the wedge was cut", r.mStats.mPieces > 0);
            clear(r, { 0.f, 0.f, 8.f, 4.f }, "nothing over the ramp");
        }
        // (b) A 1 m stage: a hatched rim where its skirt is, nothing over its top.
        {
            TileMaker t;
            t.quad(0.f, 0.f, 8.f, 2.975f, 0.f, 0.f);
            t.quad(0.f, 7.025f, 8.f, 8.f, 0.f, 0.f);
            t.quad(0.f, 2.975f, 3.025f, 7.025f, 0.f, 0.f);
            t.quad(6.975f, 2.975f, 8.f, 7.025f, 0.f, 0.f);
            t.quad(3.775f, 3.725f, 6.225f, 6.275f, 1.f, 1.f);
            skmap::ScenePtr s = t.section({ box(LLVector3(5.f, 5.f, 0.5f), LLVector3(1.6f, 1.65f, 0.5f), 1) });
            std::set<S32> layers;
            for (const skmap::ScenePoly& p : s->mPolys)
            {
                layers.insert(p.mLayer);
            }
            ensure("one layer", layers.size() == 1);
            const Raster r = bake(*s, *layers.begin(), TIER0);
            ensure_equals("nothing at the stage centre", static_cast<S32>(bits(r, 5.f, 5.f)), 0);
            ensure("half inside its west face", (bits(r, 3.6f, 5.f) & skmap::SECTION_TEXEL_HALF) != 0);
        }
        // (c) The crate you can stand on: half over its top.
        {
            TileMaker t;
            const S32 south = t.quad(0.f, 0.f, 8.f, 3.5f, 0.f, 0.f);
            const S32 north = t.quad(0.f, 4.5f, 8.f, 8.f, 0.f, 0.f);
            const S32 west = t.quad(0.f, 3.5f, 3.5f, 4.5f, 0.f, 0.f);
            const S32 east = t.quad(4.5f, 3.5f, 8.f, 4.5f, 0.f, 0.f);
            const S32 top = t.quad(3.5f, 3.5f, 4.5f, 4.5f, 0.75f, 0.75f);
            t.join(top, 0, south, 2);
            t.join(top, 2, north, 0);
            t.join(top, 3, west, 1);
            t.join(top, 1, east, 3);
            skmap::ScenePtr s = t.section({ box(LLVector3(4.f, 4.f, 0.35f), LLVector3(0.5f, 0.5f, 0.35f), 1) });
            const Raster r = bake(*s, s->mPolys[top].mLayer, TIER0);
            const U8 b = bits(r, 4.f, 4.f);
            ensure("half over the crate", (b & skmap::SECTION_TEXEL_HALF) != 0 && !(b & skmap::SECTION_TEXEL_FULL));
        }
        // (d) Stair treads: never cover.
        {
            TileMaker t;
            const S32 floor = t.quad(1.f, 2.f, 4.f, 6.f, 0.f, 0.f);
            std::vector<RecordPtr> recs;
            S32 prev = floor;
            for (S32 i = 0; i < 5; ++i)
            {
                const F32 x0 = 4.f + 0.3f * i, top = 0.25f * (i + 1);
                const S32 tread = t.quad(x0, 3.f, x0 + 0.3f, 5.f, top, top);
                t.join(prev, 1, tread, 3);
                prev = tread;
                recs.push_back(box(LLVector3(x0 + 0.15f, 4.f, top * 0.5f), LLVector3(0.15f, 1.f, top * 0.5f), 10 + i));
            }
            const S32 landing = t.quad(5.5f, 2.f, 8.5f, 6.f, 1.25f, 1.25f);
            t.join(prev, 1, landing, 3);
            skmap::ScenePtr s = t.section(recs);
            const Raster r = bake(*s, s->mPolys[floor].mLayer, TIER0);
            for (S32 i = 0; i < 5; ++i)
            {
                clear(r, { 4.f + 0.3f * i, 3.f, 4.3f + 0.3f * i, 5.f }, "nothing over a tread");
            }
        }
    }

    // A placeholder box is a dashed outline, never a fill.
    template<> template<>
    void skmapraster_object::test<14>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 3.f, 0.f, 0.f);
        Record placeholder = boxOf(LLVector3(4.f, 5.f, 1.5f), LLVector3(2.f, 1.f, 1.5f), 1);
        placeholder.mProvenance = Provenance::BOX;
        skmap::ScenePtr s = t.section({ std::make_shared<const Record>(placeholder) });
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        S32 edges = 0;
        for (U8 bits : r.mStats.mSectionClass)
        {
            edges += (bits & skmap::SECTION_TEXEL_PROVISIONAL_EDGE) ? 1 : 0;
            ensure("no fill", !(bits & (skmap::SECTION_TEXEL_HALF | skmap::SECTION_TEXEL_FULL)));
        }
        ensure("a dashed outline", edges > 0);
    }

    // A wall across a cell border has no outline along the border on either side.
    template<> template<>
    void skmapraster_object::test<15>()
    {
        TileMaker t;
        t.quad(0.f, 4.f, 3.f, 12.f, 0.f, 0.f);
        skmap::ScenePtr s = t.section({ box(LLVector3(4.f, 8.f, 1.5f), LLVector3(0.15f, 3.f, 1.5f), 1) });
        const S32 layer = s->mPolys[0].mLayer;
        const skmap::CellId south = { 0, 0, 0 }, north = { 0, 0, 1 };
        const Raster a = bake(*s, layer, south);
        const Raster b = bake(*s, layer, north);
        S32 n = 0;
        for (S32 x = 0; x < 256; ++x)
        {
            const F32 cx = texelAt(south, x, 0).mV[VX];
            if (cx < 3.9f || cx > 4.1f)
            {
                continue;
            }
            for (const U8 bits : { a.mStats.mSectionClass[255 * 256 + x], b.mStats.mSectionClass[x] })
            {
                ensure("full at the seam", (bits & skmap::SECTION_TEXEL_FULL) != 0);
                ensure("no outline at the seam", (bits & skmap::SECTION_TEXEL_FULL_EDGE) == 0);
            }
            ++n;
        }
        ensure("seam texels checked", n > 4);
    }

    // A door's leaf never reaches the bake: the doorway is a gap.
    template<> template<>
    void skmapraster_object::test<16>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 3.625f, 8.f, 0.f, 0.f);
        t.quad(4.375f, 0.f, 8.f, 8.f, 0.f, 0.f);
        Record leaf = boxOf(LLVector3(4.f, 4.f, 1.1f), LLVector3(0.03f, 0.6f, 1.1f), 3);
        leaf.mFeature = FeatureKind::DOOR;
        skmap::DoorInput door;
        door.mPlanePoint.set(4.0, 4.0, 0.0);
        door.mNormal.set(1.f, 0.f, 0.f);
        door.mWidth = 1.2f;
        door.mMin.set(3.97, 3.4, 0.0);
        door.mMax.set(4.03, 4.6, 2.2);
        door.mLeaves = { leaf.mPart };
        skmap::ScenePtr s = t.section({ box(LLVector3(4.f, 1.7f, 1.5f), LLVector3(0.125f, 1.7f, 1.5f), 1),
                                        box(LLVector3(4.f, 6.3f, 1.5f), LLVector3(0.125f, 1.7f, 1.5f), 2), std::make_shared<const Record>(leaf) },
                                      { door });
        ensure("the leaf went to its door", s->mDoorLeaves.size() == 1 && s->mDoorLeaves[0].size() == 1);
        const Raster r = bake(*s, s->mPolys[0].mLayer, TIER0);
        ensure("the walls are cut", r.mStats.mFullTexels > 0);
        for (S32 y = 0; y < 256; ++y)
        {
            for (S32 x = 0; x < 256; ++x)
            {
                const LLVector2 p = texelAt(TIER0, x, y);
                if (p.mV[VX] >= 3.97f && p.mV[VX] <= 4.03f && p.mV[VY] >= 3.55f && p.mV[VY] <= 4.45f)
                {
                    ensure_equals("nothing in the doorway", static_cast<S32>(r.mStats.mSectionClass[y * 256 + x]), 0);
                }
            }
        }
    }
}
