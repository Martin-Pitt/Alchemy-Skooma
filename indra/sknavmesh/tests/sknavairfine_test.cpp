/**
 * @file sknavairfine_test.cpp
 * @brief Tests for the fine air over a tile's narrow openings.
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

#include "../sknavairdistil.h"
#include "../sknavairfine.h"

#include <algorithm>

namespace
{
    using namespace sknav;

    /// A grid of one gap per column from 0 to height, with solid columns where solid() says.
    template <typename Fn> AirGrid grid(S32 w, S32 h, U16 height, Fn&& solid)
    {
        AirGrid g;
        g.mWidth = w;
        g.mHeight = h;
        g.mStart.assign(static_cast<size_t>(w) * h + 1, 0);
        for (S32 y = 0; y < h; ++y)
        {
            for (S32 x = 0; x < w; ++x)
            {
                if (!solid(x, y))
                {
                    AirGap gap;
                    gap.mCeiling = height;
                    gap.mFlags = AirGap::TOP;
                    g.mGaps.push_back(gap);
                }
                g.mStart[x + y * w + 1] = static_cast<U32>(g.mGaps.size());
            }
        }
        return g;
    }

    /// A wall along x = 10 with a slot of the given width from y = 20.
    AirGrid slotted(S32 slot)
    {
        return grid(32, 32, 40, [slot](S32 x, S32 y) { return x == 10 && (y < 20 || y >= 20 + slot); });
    }
}

namespace tut
{
    struct sknavairfine_data
    {
    };
    typedef test_group<sknavairfine_data> sknavairfine_group;
    typedef sknavairfine_group::object sknavairfine_object;
    tut::sknavairfine_group sknavairfine_test("sknavairfine");

    template<> template<>
    void sknavairfine_object::test<1>()
    {
        set_test_name("a slot two cells wide makes its block a candidate, a wider one does not");
        const std::vector<U8> two = fineCandidates(slotted(2), 6, 4);
        ensure_equals("blocks", two.size(), 4u);
        ensure("the slot's block", two[0 + 1 * 2] == 1);
        ensure("no other block", two[0] == 0 && two[1] == 0 && two[3] == 0);
        const std::vector<U8> three = fineCandidates(slotted(3), 6, 4);
        ensure("a three-cell slot is no candidate", std::count(three.begin(), three.end(), 1) == 0);
    }

    template<> template<>
    void sknavairfine_object::test<2>()
    {
        set_test_name("touching candidate blocks share a window, apart ones do not");
        // 4 x 4 blocks over a 64-cell grid: (0,0) and (1,1) touch; (3,3) stands apart.
        std::vector<U8> blocks(16, 0);
        blocks[0] = blocks[1 + 4] = blocks[3 + 12] = 1;
        const std::vector<FineWindow> w = fineWindows(blocks, 64, 60);
        ensure_equals("windows", w.size(), 2u);
        ensure("the pair's bounds", w[0].mX0 == 0 && w[0].mY0 == 0 && w[0].mX1 == 32 && w[0].mY1 == 32);
        ensure("clipped to the grid", w[1].mX0 == 48 && w[1].mY0 == 48 && w[1].mX1 == 64 && w[1].mY1 == 60);
        // An L whose bounds hold a block of another group: one window.
        std::vector<U8> ell(16, 0);
        ell[0] = ell[1] = ell[2] = ell[2 + 4] = ell[2 + 8] = ell[0 + 8] = 1;
        const std::vector<FineWindow> merged = fineWindows(ell, 64, 64);
        ensure_equals("merged", merged.size(), 1u);
        ensure("the L's bounds", merged[0].mX1 == 48 && merged[0].mY1 == 48);
    }

    template<> template<>
    void sknavairfine_object::test<3>()
    {
        set_test_name("a fine wall the coarse air leaks through closes and separates the link");
        const AirGrid coarse = grid(2, 1, 40, [](S32, S32) { return false; });
        const std::vector<U8> blocks(1, 1);
        const FineWindow w{ 0, 0, 2, 1 };
        FineCompare open;
        compareFineAir(coarse, grid(4, 2, 80, [](S32, S32) { return false; }), w, blocks, 6, 4, 12, 8, open);
        ensure_equals("one coarse link", open.mCoarseLinks, 1u);
        ensure_equals("kept open", open.mClosed, 0u);
        FineCompare walled;
        compareFineAir(coarse, grid(4, 2, 80, [](S32 x, S32) { return x == 2; }), w, blocks, 6, 4, 12, 8, walled);
        ensure_equals("closed", walled.mClosed, 1u);
        ensure_equals("separating", walled.mClosedSeparating, 1u);
        ensure_equals("nothing opened", walled.mOpened, 0u);
        // Within coarse climb, but the fine floors step past the fine one.
        AirGrid stepped = grid(4, 2, 80, [](S32, S32) { return false; });
        for (S32 y = 0; y < 2; ++y)
        {
            for (S32 x = 2; x < 4; ++x)
            {
                stepped.mGaps[stepped.mStart[x + y * 4]].mFloor = 13;
            }
        }
        FineCompare step;
        compareFineAir(coarse, stepped, w, blocks, 6, 4, 12, 8, step);
        ensure_equals("a step closes", step.mClosed, 1u);
    }

    template<> template<>
    void sknavairfine_object::test<4>()
    {
        set_test_name("fine leaves sort into key order with their gaps");
        FineAir air;
        AirGrid fine = grid(4, 2, 80, [](S32 x, S32 y) { return x == 0 && y == 1; });
        for (size_t i = 0; i < fine.mGaps.size(); ++i)
        {
            fine.mGaps[i].mFloor = static_cast<U16>(i);
        }
        appendFineLeaves(fine, 0, 0, std::vector<U8>(4, 1), 32, air);
        sortFineLeaves(air);
        ensure_equals("leaves", air.mLeaves.size(), 7u);
        for (size_t i = 0; i < air.mLeaves.size(); ++i)
        {
            const FineLeaf& l = air.mLeaves[i];
            ensure("key order", i == 0 || airKey(air.mLeaves[i - 1].mX, air.mLeaves[i - 1].mY) < airKey(l.mX, l.mY));
            ensure("each column's gap went with it", air.mGaps[l.mFirstGap].mFloor == fine.mStart[l.mX + l.mY * 4]);
        }
    }
}
