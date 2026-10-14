/**
 * @file skoutlinepolicy_test.cpp
 * @brief The outline capture's grid, atlas slots, render order and budget.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../test/lltut.h"

#include "../skoutlinepolicy.h"

#include <cmath>

using namespace SKOutlinePolicy;

namespace tut
{
    struct skoutlinepolicy_data
    {
    };
    typedef test_group<skoutlinepolicy_data> skoutlinepolicy_group;
    typedef skoutlinepolicy_group::object skoutlinepolicy_object;
    tut::skoutlinepolicy_group skoutlinepolicy("SKOutlinePolicy");

    // A 13 m vehicle gets 160 cells on its long side plus the padding, and the grid covers it.
    template<> template<>
    void skoutlinepolicy_object::test<1>()
    {
        const Grid g = gridFor(-6.5f, -2.f, 6.5f, 2.f, GridLimits());
        ensure("valid", g.valid());
        ensure_approximately_equals("cell", g.cell, 13.f / 160.f, 8);
        ensure("cols", g.cols >= 160 && g.cols <= 163);
        ensure("covers", g.x0 <= -6.5f && g.x1 >= 6.5f && g.y0 <= -2.f && g.y1 >= 2.f);
        ensure("padding", g.x0 < -6.5f && g.y1 > 2.f);
        ensure("not coarsened", !g.coarsened);
        ensure_approximately_equals("width", g.x1 - g.x0, g.cols * g.cell, 8);
    }

    // The ceiling gives a 50 m ship more than 160 cells, up to what the slot holds.
    template<> template<>
    void skoutlinepolicy_object::test<2>()
    {
        const Grid g = gridFor(-25.f, -5.f, 25.f, 5.f, GridLimits());
        ensure_approximately_equals("ceiling cell", g.cell, 0.25f, 8);
        ensure("more cells", g.cols > 160 && g.cols <= 254);
        ensure("not coarsened", !g.coarsened);
    }

    // A 200 m ship cannot keep the ceiling: it is coarsened to fit the slot and says so.
    template<> template<>
    void skoutlinepolicy_object::test<3>()
    {
        const GridLimits limits;
        const Grid g = gridFor(-100.f, -10.f, 100.f, 10.f, limits);
        ensure("valid", g.valid());
        ensure("coarsened", g.coarsened);
        ensure("fits", g.cols <= limits.max_cells && g.rows <= limits.max_cells);
        ensure("covers", g.x0 <= -100.f && g.x1 >= 100.f);
    }

    // A tiny part keeps the floor; an empty or inverted box makes no grid.
    template<> template<>
    void skoutlinepolicy_object::test<4>()
    {
        const Grid g = gridFor(0.f, 0.f, 0.1f, 0.05f, GridLimits());
        ensure_approximately_equals("floor", g.cell, 0.002f, 8);
        ensure("fewer cells", g.cols < 160);
        ensure("inverted", !gridFor(1.f, 0.f, 0.f, 1.f, GridLimits()).valid());
        ensure("nan", !gridFor(NAN, 0.f, 1.f, 1.f, GridLimits()).valid());
    }

    // Slots are handed out lowest first, reused after release, and run out.
    template<> template<>
    void skoutlinepolicy_object::test<5>()
    {
        SlotAtlas atlas(2048, 256);
        ensure_equals("capacity", atlas.capacity(), 64);
        ensure_equals("first", atlas.acquire(), 0);
        ensure_equals("second", atlas.acquire(), 1);
        S32 x = 0, y = 0;
        atlas.origin(9, x, y);
        ensure("origin", x == 256 && y == 256);
        atlas.release(0);
        ensure_equals("reuse", atlas.acquire(), 0);
        for (S32 i = 2; i < 64; ++i)
        {
            atlas.acquire();
        }
        ensure_equals("full", atlas.acquire(), -1);
        ensure_equals("used", atlas.used(), 64);
        atlas.release(70);
        atlas.release(-1);
        ensure_equals("bad release ignored", atlas.used(), 64);
        atlas.clear();
        ensure_equals("cleared", atlas.used(), 0);
    }

    // First sight, a lost context and changes always ask; motion asks once per interval.
    template<> template<>
    void skoutlinepolicy_object::test<6>()
    {
        ensure("first", reasonFor(false, false, false, false, 0.0, 0.5) == Reason::FIRST);
        ensure("lost", reasonFor(true, true, false, false, 0.0, 0.5) == Reason::CONTEXT);
        ensure("changed", reasonFor(true, false, true, true, 0.0, 0.5) == Reason::CHANGED);
        ensure("moving early", reasonFor(true, false, false, true, 0.2, 0.5) == Reason::NONE);
        ensure("moving due", reasonFor(true, false, false, true, 0.5, 0.5) == Reason::MOVING);
        ensure("still", reasonFor(true, false, false, false, 10.0, 0.5) == Reason::NONE);
    }

    // First sight before changes before motion; nearest first; then the longest waiting.
    template<> template<>
    void skoutlinepolicy_object::test<7>()
    {
        std::vector<Candidate> c = {
            { 0, Reason::MOVING, 5.f, 1.0 },  { 1, Reason::CHANGED, 50.f, 1.0 }, { 2, Reason::FIRST, 80.f, 0.1 },
            { 3, Reason::NONE, 1.f, 9.0 },    { 4, Reason::FIRST, 20.f, 0.1 },   { 5, Reason::CONTEXT, 20.f, 3.0 },
        };
        order(c);
        ensure_equals("dropped", c.size(), (size_t)5);
        ensure_equals("0", c[0].index, 5);
        ensure_equals("1", c[1].index, 4);
        ensure_equals("2", c[2].index, 2);
        ensure_equals("3", c[3].index, 1);
        ensure_equals("4", c[4].index, 0);
    }

    // One render always fits; then the estimate must fit, and the count caps it.
    template<> template<>
    void skoutlinepolicy_object::test<8>()
    {
        FrameBudget b;
        b.begin(0.1, 4);
        ensure("first fits", b.fits());
        b.spend(0.3);
        ensure("over", !b.fits());
        b.begin(1.0, 2);
        b.spend(0.01);
        ensure("second fits", b.fits());
        b.spend(0.01);
        ensure("count cap", !b.fits());
        b.begin(0.0, 4);
        ensure("off", !b.fits());
    }

    // Settled needs the top LOD and quiet time.
    template<> template<>
    void skoutlinepolicy_object::test<9>()
    {
        ensure("settled", settled(true, 3.0, 2.0));
        ensure("too soon", !settled(true, 1.0, 2.0));
        ensure("low LOD", !settled(false, 30.0, 2.0));
    }

    namespace
    {
        Box box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
        {
            Box b;
            b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
            b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
            return b;
        }

        CrewPiece piece(const Box& worn, const Box& root, F32 wx, F32 wy, F32 wz)
        {
            CrewPiece p;
            p.box = worn;
            p.root = root;
            p.wearer[0] = wx; p.wearer[1] = wy; p.wearer[2] = wz;
            return p;
        }
    }

    // The samples: a Stingray airframe, the Hydra's body, a 3 m gun on its driver, a helper cube and a rifle.
    template<> template<>
    void skoutlinepolicy_object::test<10>()
    {
        const Box stingray_root = box(-1.f, -1.f, -0.5f, 1.f, 1.f, 0.5f);
        CrewPiece airframe = piece(box(-6.5f, -5.f, -1.f, 6.5f, 5.f, 1.f), stingray_root, 0.f, 0.f, 0.f);
        airframe.avatar_center = true;
        airframe.temp = true;
        ensure("Stingray airframe", crewVerdict(airframe) == CrewVerdict::LAYER);

        const Box hydra_root = box(-4.f, -3.f, -1.f, 4.f, 3.f, 1.f);
        const CrewPiece body = piece(box(-4.2f, -3.f, -1.f, 4.3f, 3.f, 1.5f), hydra_root, 0.5f, 0.f, 0.5f);
        ensure("Hydra body is vehicle-sized", crewVerdict(body) == CrewVerdict::LAYER);

        CrewPiece gun = piece(box(-0.5f, -0.6f, 0.8f, 2.5f, 0.6f, 2.8f), hydra_root, 0.5f, 0.f, 0.5f);
        ensure("an anonymous gun", crewVerdict(gun) == CrewVerdict::NOT_LAYER);
        gun.temp = true;
        ensure("a temp gun", crewVerdict(gun) == CrewVerdict::LAYER);
        gun.temp = false;
        gun.name_affinity = true;
        ensure("a gun named after the vehicle", crewVerdict(gun) == CrewVerdict::LAYER);

        CrewPiece helper = piece(box(-0.01f, -0.01f, -0.01f, 0.01f, 0.01f, 0.01f), hydra_root, 0.f, 0.f, 0.f);
        helper.temp = true;
        helper.avatar_center = true;
        ensure("helper cube", crewVerdict(helper) == CrewVerdict::PERSONAL);

        CrewPiece rifle = piece(box(-0.2f, 0.2f, 0.f, 1.2f, 0.4f, 0.3f), hydra_root, 0.f, 0.f, 0.f);
        rifle.name_affinity = true;
        ensure("worn rifle", crewVerdict(rifle) == CrewVerdict::PERSONAL);

        CrewPiece hud = airframe;
        hud.hud = true;
        ensure("HUD", crewVerdict(hud) == CrewVerdict::NOT_LAYER);

        ensure_approximately_equals("reach", reach(box(0.f, 0.f, 0.f, 3.f, 4.f, 0.f), helper.wearer), 5.f, 16);
    }

    // The Stingray's 10.46 m outline plane goes; a thin opaque wing and a translucent canopy stay.
    template<> template<>
    void skoutlinepolicy_object::test<11>()
    {
        ensure("outline plane", isThinPlane(10.46f, 10.46f, 0.01f, true));
        ensure("opaque sheet", !isThinPlane(10.46f, 10.46f, 0.01f, false));
        ensure("canopy", !isThinPlane(2.f, 1.5f, 0.8f, true));
        ensure("small decal", !isThinPlane(0.3f, 0.3f, 0.01f, true));
        ensure("speck", isSpeck(2));
        ensure("not a speck", !isSpeck(3));
    }

    // A small layer inside a larger one is merged away; one that sticks out stays.
    template<> template<>
    void skoutlinepolicy_object::test<12>()
    {
        const Rect hull { -4.f, -3.f, 4.f, 3.f };
        const Rect seat { -1.f, -1.f, 1.f, 1.f };
        const Rect barrel { 2.f, -0.3f, 6.f, 0.3f };
        ensure_approximately_equals("inside", insideFraction(seat, hull), 1.f, 16);
        ensure_approximately_equals("half", insideFraction(barrel, hull), 0.5f, 16);
        ensure("merged", mergedInto(seat, 100, hull, 5000));
        ensure("sticks out", !mergedInto(barrel, 100, hull, 5000));
        ensure("larger is kept", !mergedInto(seat, 6000, hull, 5000));
        ensure("empty", insideFraction(Rect(), hull) == 0.f);
    }

    // Vendor tags and product words tie worn parts to the vehicle.
    template<> template<>
    void skoutlinepolicy_object::test<13>()
    {
        ensure("tag", nameAffinity("[Chaos] Hydra Base LBA", "[Chaos] Hydra Flak Turret v.3.02"));
        ensure("tag case", nameAffinity("[CHAOS] Something", "[chaos] other"));
        ensure("product word", nameAffinity("Stingray MK2 seat", "stingray airframe"));
        ensure("generic words", !nameAffinity("Tank Body Base", "Turret Body"));
        ensure("unrelated", !nameAffinity("[Chaos] Hydra Base LBA", "Cubitar Stack"));
        ensure("defaults", !nameAffinity("Object", "Object"));
        ensure("empty", !nameAffinity("", ""));
    }
}
