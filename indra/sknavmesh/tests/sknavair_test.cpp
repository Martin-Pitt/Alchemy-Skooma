/**
 * @file sknavair_test.cpp
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

#include "../test/lltut.h"

#include "../sknavair.h"
#include "../sknavbuild.h"

#include "Recast.h"

#include <cmath>

namespace
{
    using namespace sknav;

    const S32 WEATHER = static_cast<S32>(AgentClass::WEATHER);

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

namespace tut
{
    struct sknavair_data
    {
    };
    typedef test_group<sknavair_data> sknavair_group;
    typedef sknavair_group::object sknavair_object;
    tut::sknavair_group sknavair_tests("sknavair");

    template<> template<>
    void sknavair_object::test<1>()
    {
        set_test_name("a floor under a roof gives a walkable gap and an open one above");
        Field f;
        // Recast column x = 2, z = 1 is SL (ix, iy) = (z - 1, x - 1) = (0, 1).
        f.span(2, 1, 0, 5, RC_WALKABLE_AREA);
        f.span(2, 1, 20, 22, RC_NULL_AREA);
        AirGrid air;
        extractAir(*f.mHf, 1, air);
        ensure_equals("width", air.mWidth, 4);
        ensure_equals("height", air.mHeight, 4);
        ensure_equals("gaps in the column", air.count(0, 1), 2u);
        ensure_equals("nothing in the swapped column", air.count(1, 0), 0u);
        const AirGap* g = air.gaps(0, 1);
        ensure_equals("floor", g[0].mFloor, 5);
        ensure_equals("ceiling", g[0].mCeiling, 20);
        ensure_equals("walkable, closed", g[0].mFlags, static_cast<U8>(AirGap::WALKABLE));
        ensure_equals("roof top", g[1].mFloor, 22);
        ensure_equals("open to the tile's top", g[1].mCeiling, 50);
        ensure_equals("open, own sky, not walkable", g[1].mFlags, static_cast<U8>(AirGap::OPEN | AirGap::OWN_SKY));
        ensure("ceiling in metres", std::fabs(air.height(g[0].mCeiling) - 2.f) < 1e-4f);
        ensure("origin x is Recast z past the border", std::fabs(air.mOrigin.mV[VX] - 0.5f) < 1e-4f);
        ensure("origin y is Recast x past the border", std::fabs(air.mOrigin.mV[VY] - 0.5f) < 1e-4f);
    }

    template<> template<>
    void sknavair_object::test<2>()
    {
        set_test_name("the border is left out");
        Field f;
        f.span(0, 0, 0, 5, RC_WALKABLE_AREA);
        f.span(5, 3, 0, 5, RC_WALKABLE_AREA);
        AirGrid air;
        extractAir(*f.mHf, 1, air);
        ensure("no gaps from border columns", air.empty());
    }

    template<> template<>
    void sknavair_object::test<3>()
    {
        set_test_name("a tile build keeps the weather class's air, door leaf and all");
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;
        in.mClasses[WEATHER] = true;
        in.mParams[WEATHER].mHeight = 0.5f;
        in.mParams[WEATHER].mRadius = 0.f;
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
        TileBuildResult out = buildTile(in);
        ensure("built", out.mStatus == TileBuildResult::SUCCESS);
        ensure("air came back", !out.mAir.empty());
        const S32 ix = static_cast<S32>((16.f - out.mAir.mOrigin.mV[VX]) / out.mAir.mCell);
        const S32 iy = static_cast<S32>((16.f - out.mAir.mOrigin.mV[VY]) / out.mAir.mCell);
        std::string dump;
        for (U32 i = 0; i < out.mAir.count(ix, iy); ++i)
        {
            const AirGap& d = out.mAir.gaps(ix, iy)[i];
            dump += llformat(" [%.2f %.2f %d]", out.mAir.height(d.mFloor), out.mAir.height(d.mCeiling), d.mFlags);
        }
        ensure_equals("floor gap and roof gap:" + dump, out.mAir.count(ix, iy), 2u);
        const AirGap* g = out.mAir.gaps(ix, iy);
        // A top rasterises up to a unit high: Recast rounds span tops up.
        const F32 floor = out.mAir.height(g[0].mFloor);
        ensure("floor at the slab top:" + dump, floor > 0.5f - 0.01f && floor < 0.5f + out.mAir.mUnit + 0.01f);
        ensure("ceiling at the roof:" + dump, std::fabs(out.mAir.height(g[0].mCeiling) - 3.f) < 0.01f);
        ensure("under the roof is closed", !(g[0].mFlags & AirGap::OPEN));
        ensure("over the roof is open", (g[1].mFlags & AirGap::OPEN) != 0);

        in.mAir = false;
        ensure("no air unless asked", buildTile(in).mAir.empty());
    }
    template<> template<>
    void sknavair_object::test<4>()
    {
        set_test_name("clearance is the 2/3 chamfer to a step no one can climb, and the grid's edge is no wall");
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
            ensure_equals(llformat("clear at (%d, %d)", i % 5, i / 5), static_cast<S32>(g.gaps(i % 5, i / 5)[0].mClear), static_cast<S32>(want[i]));
        }
    }

    template<> template<>
    void sknavair_object::test<5>()
    {
        set_test_name("clearance caps at 2 m, and a low ceiling between columns is a wall");
        std::vector<U16> floors(40, 0);
        floors[0] = 0xffff;
        AirGrid g = flatGrid(40, 1, floors);
        computeClearance(g, 6, 4);
        ensure_equals("half a cell off the solid", static_cast<S32>(g.gaps(1, 0)[0].mClear), 0);
        ensure_equals("four and a half cells off", static_cast<S32>(g.gaps(5, 0)[0].mClear), 4);
        ensure_equals("capped", static_cast<S32>(g.gaps(39, 0)[0].mClear), static_cast<S32>(CLEAR_MAX));

        // Two columns whose air overlaps by less than overlap units.
        AirGrid low = flatGrid(3, 1);
        low.mGaps[2].mFloor = 4;
        low.mGaps[2].mCeiling = 44;
        low.mGaps[1].mCeiling = 6;
        computeClearance(low, 6, 4);
        ensure_equals("a crawlspace is walled from the room beside it", static_cast<S32>(low.gaps(2, 0)[0].mClear), 0);
    }

    template<> template<>
    void sknavair_object::test<6>()
    {
        set_test_name("cropping keeps the inner columns and moves the origin");
        AirGrid g = flatGrid(6, 4);
        g.mGaps[2 + 1 * 6].mFloor = 7;
        AirGrid c;
        cropAir(g, 1, c);
        ensure_equals("width", c.mWidth, 4);
        ensure_equals("height", c.mHeight, 2);
        ensure_equals("column moved with the crop", static_cast<S32>(c.gaps(1, 0)[0].mFloor), 7);
        ensure("origin moved a cell", std::fabs(c.mOrigin.mV[VX] - 0.125f) < 1e-5f && std::fabs(c.mOrigin.mV[VY] - 0.125f) < 1e-5f);
    }
}
