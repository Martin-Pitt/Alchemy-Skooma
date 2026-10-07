/**
 * @file skmapview_test.cpp
 * @brief The mini-map's view transform, tier hysteresis, cells, analysis box and zoom clamp.
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

#include "../skmapview.h"

#include <algorithm>
#include <cmath>

namespace tut
{
    struct skmapview_data {};
    typedef test_group<skmapview_data> skmapview_group;
    typedef skmapview_group::object skmapview_object;
    tut::skmapview_group g("skmapview");

    // Global to widget and back is the identity, panned, zoomed and rotated.
    template<> template<>
    void skmapview_object::test<1>()
    {
        skmap::MapView view;
        const F32 rotations[] = { 0.f, 0.7f, -2.3f, F_PI };
        for (F32 rot : rotations)
        {
            view.set(LLVector3d(256123.5, 254321.25, 30.0), 87.f, rot, 240.f, 180.f);
            const LLVector3d p(256140.0, 254300.0, 30.0);
            const LLVector2 w = view.toWidget(p);
            const LLVector3d back = view.toGlobal(w.mV[VX], w.mV[VY]);
            ensure("x round trip", std::fabs(back.mdV[VX] - p.mdV[VX]) < 1e-3);
            ensure("y round trip", std::fabs(back.mdV[VY] - p.mdV[VY]) < 1e-3);
        }
        // North up: east is right, north is up, and the centre is the widget's middle.
        view.set(LLVector3d(100.0, 100.0, 0.0), 100.f, 0.f, 200.f, 100.f);
        const LLVector2 mid = view.toWidget(LLVector3d(100.0, 100.0, 0.0));
        ensure("centre x", std::fabs(mid.mV[VX] - 100.f) < 1e-3f);
        ensure("centre y", std::fabs(mid.mV[VY] - 50.f) < 1e-3f);
        ensure("span along the short side", std::fabs(view.mpp() - 1.f) < 1e-6f);
        const LLVector2 east = view.toWidget(LLVector3d(110.0, 100.0, 0.0));
        ensure("east is right", east.mV[VX] > mid.mV[VX] + 9.f);
        // A heading turned to the top.
        view.set(LLVector3d(0.0, 0.0, 0.0), 100.f, skmap::MapView::rotationForHeading(0.f), 100.f, 100.f);
        const LLVector2 ahead = view.toWidget(LLVector3d(10.0, 0.0, 0.0));
        ensure("heading points up", ahead.mV[VY] > 59.f && std::fabs(ahead.mV[VX] - 50.f) < 1e-3f);
    }

    // A 10% zoom across a tier boundary keeps the tier; 20% moves it.
    template<> template<>
    void skmapview_object::test<2>()
    {
        // Target texel = mpp / 2. Tier 5 (1 m texel) holds targets in [1, 2).
        const S32 start = skmap::chooseTier(2.f * 1.99f, -1);
        ensure_equals("start tier", start, 5);
        ensure_equals("10% out keeps", skmap::chooseTier(2.f * 1.99f * 1.1f, start), 5);
        ensure_equals("20% out moves", skmap::chooseTier(2.f * 1.99f * 1.2f, start), 6);
        const S32 low = skmap::chooseTier(2.f * 1.f, -1);
        ensure_equals("at the lower edge", low, 5);
        ensure_equals("10% in keeps", skmap::chooseTier(2.f * 0.9f, low), 5);
        ensure_equals("20% in moves", skmap::chooseTier(2.f * 0.8f, low), 4);
    }

    // The tier never leaves 0..6, however far the scale goes.
    template<> template<>
    void skmapview_object::test<3>()
    {
        for (F32 mpp = 1e-4f; mpp < 1e4f; mpp *= 1.37f)
        {
            const S32 t = skmap::chooseTier(mpp, -1);
            ensure("tier in range", t >= skmap::TIER_MIN && t <= skmap::TIER_MAX);
            ensure("kept tier in range", skmap::chooseTier(mpp, t) == t);
        }
        ensure_equals("finest", skmap::chooseTier(1e-5f, 3), 0);
        ensure_equals("coarsest", skmap::chooseTier(1e5f, 3), 6);
        ensure("cell 8 m at tier 0", std::fabs(skmap::tierCellSize(0) - 8.0) < 1e-9);
        ensure("cell 512 m at tier 6", std::fabs(skmap::tierCellSize(6) - 512.0) < 1e-9);
    }

    // cellsInBox covers the box: every point of it lies in a returned cell.
    template<> template<>
    void skmapview_object::test<4>()
    {
        const LLVector3d min(-37.5, 12.25, 0.0), max(91.0, 140.0, 0.0);
        for (S32 tier = 0; tier <= 6; tier += 2)
        {
            std::vector<skmap::CellId> cells;
            skmap::cellsInBox(tier, min, max, cells);
            ensure("some cells", !cells.empty());
            for (F64 x = min.mdV[VX]; x <= max.mdV[VX]; x += 3.3)
            {
                for (F64 y = min.mdV[VY]; y <= max.mdV[VY]; y += 3.3)
                {
                    const skmap::CellId c = skmap::cellAt(tier, x, y);
                    ensure("point covered", std::find(cells.begin(), cells.end(), c) != cells.end());
                }
            }
        }
        const skmap::CellId c = skmap::cellAt(0, -0.5, 9.0);
        ensure_equals("negative floors", c.mX, -1);
        ensure_equals("positive", c.mY, 1);
        ensure("ancestor holds the cell", c.ancestor(2) == skmap::cellAt(2, -0.5, 9.0));
    }

    // The analysis box holds a 256 m reach around a centre 128 m off the agent, and only moves in 64 m steps.
    template<> template<>
    void skmapview_object::test<5>()
    {
        const LLVector3d agent(1000.0, 2000.0, 20.0);
        const LLVector3d centre(1128.0, 2000.0, 20.0);
        LLVector3d min, max;
        skmap::analysisBox(agent, centre, 200.f, 200.f, min, max);
        ensure("holds the view west", min.mdV[VX] <= centre.mdV[VX] - 256.0);
        ensure("holds the view east", max.mdV[VX] >= centre.mdV[VX] + 256.0);
        ensure("holds the agent's view", min.mdV[VX] <= agent.mdV[VX] - 256.0 && min.mdV[VY] <= agent.mdV[VY] - 256.0);
        ensure("snapped", std::fmod(min.mdV[VX], 64.0) == 0.0 && std::fmod(max.mdV[VY], 64.0) == 0.0);
        // Small moves inside one snap step do not move the box.
        LLVector3d min2, max2;
        skmap::analysisBox(agent + LLVector3d(5.0, 3.0, 0.0), centre + LLVector3d(5.0, 3.0, 0.0), 200.f, 200.f, min2, max2);
        LLVector3d min3, max3;
        skmap::analysisBox(agent + LLVector3d(80.0, 0.0, 0.0), centre + LLVector3d(80.0, 0.0, 0.0), 200.f, 200.f, min3, max3);
        ensure("a crossing moves it", min3.mdV[VX] != min.mdV[VX]);
        ensure("moves by whole steps", std::fmod(min3.mdV[VX] - min.mdV[VX], 64.0) == 0.0);
        ensure("unchanged inside a step", min2 == min && max2 == max);
        // A wide widget reaches further along its long side, never past twice.
        LLVector3d wmin, wmax;
        skmap::analysisBox(agent, agent, 800.f, 200.f, wmin, wmax);
        ensure("aspect clamped at 2", wmax.mdV[VX] - wmin.mdV[VX] <= 2.0 * (512.0 + 32.0) + 128.0);
        ensure("aspect reaches", wmax.mdV[VX] - agent.mdV[VX] >= 512.0);
        // A camera across the grid (a teleport, a far cam) does not stretch the box to cover the agent too.
        LLVector3d fmin, fmax, cmin, cmax;
        skmap::analysisBox(agent + LLVector3d(21000.0, -9000.0, 0.0), centre, 200.f, 200.f, fmin, fmax);
        skmap::analysisBox(centre, centre, 200.f, 200.f, cmin, cmax);
        ensure("a far agent leaves the box to the centre", fmin == cmin && fmax == cmax);
        ensure("bounded", fmax.mdV[VX] - fmin.mdV[VX] <= 2.0 * (256.0 + 32.0) + 128.0);
    }

    // The zoom stays inside 4..2048 m and steps by 1.1.
    template<> template<>
    void skmapview_object::test<6>()
    {
        ensure_equals("floor", skmap::zoomByNotches(4.f, -5.f), 4.f);
        ensure_equals("ceiling", skmap::zoomByNotches(2048.f, 3.f), 2048.f);
        ensure("one notch out", std::fabs(skmap::zoomByNotches(100.f, 1.f) - 110.f) < 1e-3f);
        ensure("one notch in", std::fabs(skmap::zoomByNotches(110.f, -1.f) - 100.f) < 1e-3f);
        skmap::MapView view;
        view.set(LLVector3d(), 5000.f, 0.f, 100.f, 100.f);
        ensure_equals("view clamps", view.span(), 2048.f);
        const S32 b = skmap::zoomBucket(0.5f);
        ensure("bucket round trip", std::fabs(skmap::bucketMpp(b) - 0.5f) < 0.03f);
        ensure("bucket steps", skmap::zoomBucket(0.5f * 1.1f) == b + 1);
    }
}
