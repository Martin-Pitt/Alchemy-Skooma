/**
 * @file skair_test.cpp
 * @brief The air grid: gaps per column, their flags, and the Recast to SL axis swap.
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

#include <catch2/catch_test_macros.hpp>

#include "../skair.h"
#include "../skairdistil.h"
#include "../skairraster.h"

#include "Recast.h"

#include <cmath>

namespace
{
    using namespace skworld;


    /// A 6 x 6 heightfield with a one-cell border, 0.5 m cells, 0.1 m units.
    struct Field
    {
        rcContext mCtx { false };
        rcHeightfield* mHf = rcAllocHeightfield();
        Field()
        {
            const F32 bmin[3] = { 0.f, 0.f, 0.f };
            const F32 bmax[3] = { 3.f, 5.f, 3.f };
            rcCreateHeightfield(&mCtx, *mHf, 6, 6, bmin, bmax, 0.5f, 0.1f);
        }
        ~Field() { rcFreeHeightField(mHf); }
        void span(S32 x, S32 z, U16 smin, U16 smax, U8 area) { rcAddSpan(&mCtx, *mHf, x, z, smin, smax, area, 1); }
    };

    /// A grid of one gap per column, floor 0 and ceiling 40, at 0.125 m;
    /// floors[] overrides a column's floor, and 0xffff leaves it solid.
    AirGrid flatGrid(S32 w, S32 h, const std::vector<U16>& floors = {})
    {
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = w;
        g.mHeight = h;
        g.mStart.assign(static_cast<size_t>(w) * h + 1, 0);
        for (S32 i = 0; i < w * h; ++i)
        {
            const U16 f = floors.empty() ? 0 : floors[i];
            if (f != 0xffff)
            {
                AirGap gap;
                gap.mFloor = f;
                gap.mCeiling = f + 40;
                g.mGaps.push_back(gap);
            }
            g.mStart[i + 1] = static_cast<U32>(g.mGaps.size());
        }
        return g;
    }
}

namespace
{

    TEST_CASE("skair: a floor under a roof gives a walkable gap and an open one above", "[skair]")
    {
        Field f;
        // Recast column x = 2, z = 1 is SL (ix, iy) = (z - 1, x - 1) = (0, 1).
        f.span(2, 1, 0, 5, RC_WALKABLE_AREA);
        f.span(2, 1, 20, 22, RC_NULL_AREA);
        AirGrid air;
        extractAir(*f.mHf, 1, air);
        { INFO("width"); CHECK(air.mWidth == 4); }
        { INFO("height"); CHECK(air.mHeight == 4); }
        { INFO("gaps in the column"); CHECK(air.count(0, 1) == 2u); }
        { INFO("nothing in the swapped column"); CHECK(air.count(1, 0) == 0u); }
        const AirGap* g = air.gaps(0, 1);
        { INFO("floor"); CHECK(g[0].mFloor == 5); }
        { INFO("ceiling"); CHECK(g[0].mCeiling == 20); }
        { INFO("walkable, closed"); CHECK(g[0].mFlags == static_cast<U8>(AirGap::TOP)); }
        { INFO("roof top"); CHECK(g[1].mFloor == 22); }
        { INFO("open to the tile's top"); CHECK(g[1].mCeiling == 50); }
        { INFO("open, own sky, not walkable"); CHECK(g[1].mFlags == static_cast<U8>(AirGap::OPEN | AirGap::OWN_SKY)); }
        { INFO("ceiling in metres"); CHECK(std::fabs(air.height(g[0].mCeiling) - 2.f) < 1e-4f); }
        { INFO("origin x is Recast z past the border"); CHECK(std::fabs(air.mOrigin.mV[VX] - 0.5f) < 1e-4f); }
        { INFO("origin y is Recast x past the border"); CHECK(std::fabs(air.mOrigin.mV[VY] - 0.5f) < 1e-4f); }
    }

    TEST_CASE("skair: the border is left out", "[skair]")
    {
        Field f;
        f.span(0, 0, 0, 5, RC_WALKABLE_AREA);
        f.span(5, 3, 0, 5, RC_WALKABLE_AREA);
        AirGrid air;
        extractAir(*f.mHf, 1, air);
        { INFO("no gaps from border columns"); CHECK(air.empty()); }
    }

    TEST_CASE("skair: a tile build keeps the raster's air; door leaf and all", "[skair]")
    {
        TileInput in;
        in.mAirPad = airPadCells(0.25f);
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;
        in.mAir = true;
        // A 4 m slab at z 0..0.5 under a roof at 3..3.25.
        auto slab = [](F32 z0, F32 z1) {
            auto r = std::make_shared<Record>();
            r->mShape = Shape::BOX;
            r->mCenter.set(16.f, 16.f, (z0 + z1) / 2);
            r->mHalf.set(2.f, 2.f, (z1 - z0) / 2);
            r->mAxes[0].set(1, 0, 0);
            r->mAxes[1].set(0, 1, 0);
            r->mAxes[2].set(0, 0, 1);
            r->mMin.set(14.f, 14.f, z0);
            r->mMax.set(18.f, 18.f, z1);
            r->mSolid = true;
            return r;
        };
        in.mRecords.push_back(slab(0.f, 0.5f));
        in.mRecords.push_back(slab(3.f, 3.25f));
        TileAirResult out;
        rasteriseTile(in, out);
        { INFO("built"); CHECK(out.mStatus == TileAirResult::SUCCESS); }
        { INFO("air came back"); CHECK((out.mAir && out.mAirBuilt)); }
        const TileAir& air = *out.mAir;
        const S32 ix = static_cast<S32>((16.f - air.mOrigin.mV[VX]) / air.mCell);
        const S32 iy = static_cast<S32>((16.f - air.mOrigin.mV[VY]) / air.mCell);
        const AirLeaf* leaf = nullptr;
        for (const AirLeaf& l : air.mLeaves)
        {
            const S32 s = 1 << l.mLevel;
            if (ix >= l.mX && ix < l.mX + s && iy >= l.mY && iy < l.mY + s)
            {
                leaf = &l;
            }
        }
        { INFO("a leaf covers the middle"); CHECK(leaf != nullptr); }
        std::string dump;
        const auto height = [&air](U16 u) { return air.mOrigin.mV[VZ] + u * air.mUnit; };
        for (U32 i = 0; i < leaf->mGapCount; ++i)
        {
            const AirGap& d = air.mGaps[leaf->mFirstGap + i];
            dump += llformat(" [%.2f %.2f %d]", height(d.mFloor), height(d.mCeiling), d.mFlags);
        }
        { INFO("floor gap and roof gap:" + dump); CHECK(static_cast<U32>(leaf->mGapCount) == 2u); }
        const AirGap* g = &air.mGaps[leaf->mFirstGap];
        // A top rasterises up to a unit high: Recast rounds span tops up.
        const F32 floor = height(g[0].mFloor);
        { INFO("floor at the slab top:" + dump); CHECK((floor > 0.5f - 0.01f && floor < 0.5f + air.mUnit + 0.01f)); }
        { INFO("ceiling at the roof:" + dump); CHECK(std::fabs(height(g[0].mCeiling) - 3.f) < 0.01f); }
        { INFO("under the roof is closed"); CHECK(!(g[0].mFlags & AirGap::OPEN)); }
        { INFO("over the roof is open"); CHECK(((g[1].mFlags & AirGap::OPEN) != 0 && g[1].mCeiling == AIR_CEILING_OPEN)); }
        { INFO("over the roof is the column's own sky"); CHECK(((g[1].mFlags & AirGap::OWN_SKY) != 0 && !(g[0].mFlags & AirGap::OWN_SKY))); }
        { INFO("standing on an object"); CHECK(static_cast<S32>(g[0].area()) == static_cast<S32>(AREA_OBJECT)); }

        in.mAir = false;
        { INFO("no air unless asked"); TileAirResult none; rasteriseTile(in, none); CHECK((!none.mAir && !none.mAirBuilt)); }
    }

    TEST_CASE("skair: clearance is the 2/3 chamfer to a step no one can climb and the grid's edge is no wall", "[skair]")
    {
        // A 5 x 5 floor whose middle column stands 2.5 m up, past the 0.75 m climb.
        std::vector<U16> floors(25, 0);
        floors[2 + 2 * 5] = 20;
        AirGrid g = flatGrid(5, 5, floors);
        computeClearance(g, 6, 4);
        // Half cells by hand: 1 beside the step, +2 along an axis, +3 across;
        // clearance is that times 0.0625 m in whole 0.125 m.
        const U8 want[25] = { 3, 2, 1, 2, 3,
                              2, 1, 0, 1, 2,
                              1, 0, 0, 0, 1,
                              2, 1, 0, 1, 2,
                              3, 2, 1, 2, 3 };
        for (S32 i = 0; i < 25; ++i)
        {
            { INFO(llformat("clear at (%d, %d)", i % 5, i / 5)); CHECK(static_cast<S32>(g.gaps(i % 5, i / 5)[0].mClear) == static_cast<S32>(want[i])); }
        }
    }

    TEST_CASE("skair: clearance caps at 2 m and a low ceiling between columns is a wall", "[skair]")
    {
        std::vector<U16> floors(40, 0);
        floors[0] = 0xffff;
        AirGrid g = flatGrid(40, 1, floors);
        computeClearance(g, 6, 4);
        { INFO("half a cell off the solid"); CHECK(static_cast<S32>(g.gaps(1, 0)[0].mClear) == 0); }
        { INFO("four and a half cells off"); CHECK(static_cast<S32>(g.gaps(5, 0)[0].mClear) == 4); }
        { INFO("capped"); CHECK(static_cast<S32>(g.gaps(39, 0)[0].mClear) == static_cast<S32>(CLEAR_MAX)); }

        // Two columns whose air overlaps by less than overlap units.
        AirGrid low = flatGrid(3, 1);
        low.mGaps[2].mFloor = 4;
        low.mGaps[2].mCeiling = 44;
        low.mGaps[1].mCeiling = 6;
        computeClearance(low, 6, 4);
        { INFO("a crawlspace is walled from the room beside it"); CHECK(static_cast<S32>(low.gaps(2, 0)[0].mClear) == 0); }
    }

    TEST_CASE("skair: cropping keeps the inner columns and moves the origin", "[skair]")
    {
        AirGrid g = flatGrid(6, 4);
        g.mGaps[2 + 1 * 6].mFloor = 7;
        AirGrid c;
        cropAir(g, 1, c);
        { INFO("width"); CHECK(c.mWidth == 4); }
        { INFO("height"); CHECK(c.mHeight == 2); }
        { INFO("column moved with the crop"); CHECK(static_cast<S32>(c.gaps(1, 0)[0].mFloor) == 7); }
        { INFO("origin moved a cell"); CHECK((std::fabs(c.mOrigin.mV[VX] - 0.125f) < 1e-5f && std::fabs(c.mOrigin.mV[VY] - 0.125f) < 1e-5f)); }
    }

    TEST_CASE("skair: slivers fold into the solid and stop reading as walls", "[skair]")
    {
        AirGrid g = flatGrid(3, 1);
        // Column 1 gets a one-unit sliver under its gap, between two stacked prims.
        AirGap sliver;
        sliver.mFloor = 0;
        sliver.mCeiling = 1;
        g.mGaps.insert(g.mGaps.begin() + 1, sliver);
        g.mGaps[2].mFloor = 2;
        g.mStart = { 0, 1, 3, 4 };
        foldSlivers(g, 4);
        { INFO("the sliver is gone"); CHECK(g.count(1, 0) == 1u); }
        { INFO("the gap above it stays"); CHECK(static_cast<S32>(g.gaps(1, 0)[0].mFloor) == 2); }
        { INFO("the others are untouched"); CHECK(g.count(2, 0) == 1u); }
        computeClearance(g, 6, 4);
        { INFO("no wall left beside it"); CHECK((g.gaps(0, 0)[0].mClear > 0 || g.gaps(2, 0)[0].mClear > 0)); }
    }
}
