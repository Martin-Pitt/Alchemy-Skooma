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

#include "altestmatchers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "../skoutlinepolicy.h"

#include <cmath>

using namespace SKOutlinePolicy;

namespace
{
    // A 13 m vehicle gets 160 cells on its long side plus the padding, and the grid covers it.
    TEST_CASE("skoutlinepolicy: A 13 m vehicle gets 160 cells on its long side plus the padding and the grid covers it", "[skoutlinepolicy]")
    {
        const Grid g = gridFor(-6.5f, -2.f, 6.5f, 2.f, GridLimits());
        { INFO("valid"); CHECK((g.valid())); }
        { INFO("cell"); CHECK_THAT(g.cell, ll_test::WithinBits(13.f / 160.f, 8)); }
        { INFO("cols"); CHECK((g.cols >= 160 && g.cols <= 163)); }
        { INFO("covers"); CHECK((g.x0 <= -6.5f && g.x1 >= 6.5f && g.y0 <= -2.f && g.y1 >= 2.f)); }
        { INFO("padding"); CHECK((g.x0 < -6.5f && g.y1 > 2.f)); }
        { INFO("not coarsened"); CHECK((!g.coarsened)); }
        { INFO("width"); CHECK_THAT(g.x1 - g.x0, ll_test::WithinBits(g.cols * g.cell, 8)); }
    }

    // The ceiling gives a 50 m ship more than 160 cells, up to what the slot holds.
    TEST_CASE("skoutlinepolicy: The ceiling gives a 50 m ship more than 160 cells up to what the slot holds", "[skoutlinepolicy]")
    {
        const Grid g = gridFor(-25.f, -5.f, 25.f, 5.f, GridLimits());
        { INFO("ceiling cell"); CHECK_THAT(g.cell, ll_test::WithinBits(0.25f, 8)); }
        { INFO("more cells"); CHECK((g.cols > 160 && g.cols <= 254)); }
        { INFO("not coarsened"); CHECK((!g.coarsened)); }
    }

    // A 200 m ship cannot keep the ceiling: it is coarsened to fit the slot and says so.
    TEST_CASE("skoutlinepolicy: A 200 m ship cannot keep the ceiling: it is coarsened to fit the slot and says so", "[skoutlinepolicy]")
    {
        const GridLimits limits;
        const Grid g = gridFor(-100.f, -10.f, 100.f, 10.f, limits);
        { INFO("valid"); CHECK((g.valid())); }
        { INFO("coarsened"); CHECK((g.coarsened)); }
        { INFO("fits"); CHECK((g.cols <= limits.max_cells && g.rows <= limits.max_cells)); }
        { INFO("covers"); CHECK((g.x0 <= -100.f && g.x1 >= 100.f)); }
    }

    // A tiny part keeps the floor; an empty or inverted box makes no grid.
    TEST_CASE("skoutlinepolicy: A tiny part keeps the floor - an empty or inverted box makes no grid", "[skoutlinepolicy]")
    {
        const Grid g = gridFor(0.f, 0.f, 0.1f, 0.05f, GridLimits());
        { INFO("floor"); CHECK_THAT(g.cell, ll_test::WithinBits(0.002f, 8)); }
        { INFO("fewer cells"); CHECK((g.cols < 160)); }
        { INFO("inverted"); CHECK((!gridFor(1.f, 0.f, 0.f, 1.f, GridLimits()).valid())); }
        { INFO("nan"); CHECK((!gridFor(NAN, 0.f, 1.f, 1.f, GridLimits()).valid())); }
    }

    // Slots are handed out lowest first, reused after release, and run out.
    TEST_CASE("skoutlinepolicy: Slots are handed out lowest first reused after release and run out", "[skoutlinepolicy]")
    {
        SlotAtlas atlas(2048, 256);
        { INFO("capacity"); CHECK((atlas.capacity()) == (64)); }
        { INFO("first"); CHECK((atlas.acquire()) == (0)); }
        { INFO("second"); CHECK((atlas.acquire()) == (1)); }
        S32 x = 0, y = 0;
        atlas.origin(9, x, y);
        { INFO("origin"); CHECK((x == 256 && y == 256)); }
        atlas.release(0);
        { INFO("reuse"); CHECK((atlas.acquire()) == (0)); }
        for (S32 i = 2; i < 64; ++i)
        {
            atlas.acquire();
        }
        { INFO("full"); CHECK((atlas.acquire()) == (-1)); }
        { INFO("used"); CHECK((atlas.used()) == (64)); }
        atlas.release(70);
        atlas.release(-1);
        { INFO("bad release ignored"); CHECK((atlas.used()) == (64)); }
        atlas.clear();
        { INFO("cleared"); CHECK((atlas.used()) == (0)); }
    }

    // First sight, a lost context and changes always ask; motion asks once per interval.
    TEST_CASE("skoutlinepolicy: First sight a lost context and changes always ask - motion asks once per interval", "[skoutlinepolicy]")
    {
        { INFO("first"); CHECK((reasonFor(false, false, false, false, 0.0, 0.5) == Reason::FIRST)); }
        { INFO("lost"); CHECK((reasonFor(true, true, false, false, 0.0, 0.5) == Reason::CONTEXT)); }
        { INFO("changed"); CHECK((reasonFor(true, false, true, true, 0.0, 0.5) == Reason::CHANGED)); }
        { INFO("moving early"); CHECK((reasonFor(true, false, false, true, 0.2, 0.5) == Reason::NONE)); }
        { INFO("moving due"); CHECK((reasonFor(true, false, false, true, 0.5, 0.5) == Reason::MOVING)); }
        { INFO("still"); CHECK((reasonFor(true, false, false, false, 10.0, 0.5) == Reason::NONE)); }
    }

    // First sight before changes before motion; nearest first; then the longest waiting.
    TEST_CASE("skoutlinepolicy: First sight before changes before motion - nearest first - then the longest waiting", "[skoutlinepolicy]")
    {
        std::vector<Candidate> c = {
            { 0, Reason::MOVING, 5.f, 1.0 },  { 1, Reason::CHANGED, 50.f, 1.0 }, { 2, Reason::FIRST, 80.f, 0.1 },
            { 3, Reason::NONE, 1.f, 9.0 },    { 4, Reason::FIRST, 20.f, 0.1 },   { 5, Reason::CONTEXT, 20.f, 3.0 },
        };
        order(c);
        { INFO("dropped"); CHECK((c.size()) == ((size_t)5)); }
        { INFO("0"); CHECK((c[0].index) == (5)); }
        { INFO("1"); CHECK((c[1].index) == (4)); }
        { INFO("2"); CHECK((c[2].index) == (2)); }
        { INFO("3"); CHECK((c[3].index) == (1)); }
        { INFO("4"); CHECK((c[4].index) == (0)); }
    }

    // One render always fits; then the estimate must fit, and the count caps it.
    TEST_CASE("skoutlinepolicy: One render always fits - then the estimate must fit and the count caps it", "[skoutlinepolicy]")
    {
        FrameBudget b;
        b.begin(0.1, 4);
        { INFO("first fits"); CHECK((b.fits())); }
        b.spend(0.3);
        { INFO("over"); CHECK((!b.fits())); }
        b.begin(1.0, 2);
        b.spend(0.01);
        { INFO("second fits"); CHECK((b.fits())); }
        b.spend(0.01);
        { INFO("count cap"); CHECK((!b.fits())); }
        b.begin(0.0, 4);
        { INFO("off"); CHECK((!b.fits())); }
    }

    // Settled needs the top LOD and quiet time.
    TEST_CASE("skoutlinepolicy: Settled needs the top LOD and quiet time", "[skoutlinepolicy]")
    {
        { INFO("settled"); CHECK((settled(true, 3.0, 2.0))); }
        { INFO("too soon"); CHECK((!settled(true, 1.0, 2.0))); }
        { INFO("low LOD"); CHECK((!settled(false, 30.0, 2.0))); }
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
    TEST_CASE("skoutlinepolicy: The samples: a Stingray airframe the Hydra's body a 3 m gun on its driver a helper cube and a rifle", "[skoutlinepolicy]")
    {
        const Box stingray_root = box(-1.f, -1.f, -0.5f, 1.f, 1.f, 0.5f);
        CrewPiece airframe = piece(box(-6.5f, -5.f, -1.f, 6.5f, 5.f, 1.f), stingray_root, 0.f, 0.f, 0.f);
        airframe.avatar_center = true;
        airframe.temp = true;
        { INFO("Stingray airframe"); CHECK((crewVerdict(airframe) == CrewVerdict::LAYER)); }

        const Box hydra_root = box(-4.f, -3.f, -1.f, 4.f, 3.f, 1.f);
        const CrewPiece body = piece(box(-4.2f, -3.f, -1.f, 4.3f, 3.f, 1.5f), hydra_root, 0.5f, 0.f, 0.5f);
        { INFO("Hydra body is vehicle-sized"); CHECK((crewVerdict(body) == CrewVerdict::LAYER)); }

        CrewPiece gun = piece(box(-0.5f, -0.6f, 0.8f, 2.5f, 0.6f, 2.8f), hydra_root, 0.5f, 0.f, 0.5f);
        { INFO("an anonymous gun"); CHECK((crewVerdict(gun) == CrewVerdict::NOT_LAYER)); }
        gun.temp = true;
        { INFO("a temp gun"); CHECK((crewVerdict(gun) == CrewVerdict::LAYER)); }
        gun.temp = false;
        gun.name_affinity = true;
        { INFO("a gun named after the vehicle"); CHECK((crewVerdict(gun) == CrewVerdict::LAYER)); }

        CrewPiece helper = piece(box(-0.01f, -0.01f, -0.01f, 0.01f, 0.01f, 0.01f), hydra_root, 0.f, 0.f, 0.f);
        helper.temp = true;
        helper.avatar_center = true;
        { INFO("helper cube"); CHECK((crewVerdict(helper) == CrewVerdict::PERSONAL)); }

        CrewPiece rifle = piece(box(-0.2f, 0.2f, 0.f, 1.2f, 0.4f, 0.3f), hydra_root, 0.f, 0.f, 0.f);
        rifle.name_affinity = true;
        { INFO("worn rifle"); CHECK((crewVerdict(rifle) == CrewVerdict::PERSONAL)); }

        CrewPiece hud = airframe;
        hud.hud = true;
        { INFO("HUD"); CHECK((crewVerdict(hud) == CrewVerdict::NOT_LAYER)); }

        { INFO("reach"); CHECK_THAT(reach(box(0.f, 0.f, 0.f, 3.f, 4.f, 0.f), helper.wearer), ll_test::WithinBits(5.f, 16)); }
    }

    // The Stingray's 10.46 m outline plane goes; a thin opaque wing and a translucent canopy stay.
    TEST_CASE("skoutlinepolicy: The Stingray's 10.46 m outline plane goes - a thin opaque wing and a translucent canopy stay", "[skoutlinepolicy]")
    {
        { INFO("outline plane"); CHECK((isThinPlane(10.46f, 10.46f, 0.01f, true))); }
        { INFO("opaque sheet"); CHECK((!isThinPlane(10.46f, 10.46f, 0.01f, false))); }
        { INFO("canopy"); CHECK((!isThinPlane(2.f, 1.5f, 0.8f, true))); }
        { INFO("small decal"); CHECK((!isThinPlane(0.3f, 0.3f, 0.01f, true))); }
        { INFO("speck"); CHECK((isSpeck(2))); }
        { INFO("not a speck"); CHECK((!isSpeck(3))); }
    }

    // A small layer inside a larger one is merged away; one that sticks out stays.
    TEST_CASE("skoutlinepolicy: A small layer inside a larger one is merged away - one that sticks out stays", "[skoutlinepolicy]")
    {
        const Rect hull { -4.f, -3.f, 4.f, 3.f };
        const Rect seat { -1.f, -1.f, 1.f, 1.f };
        const Rect barrel { 2.f, -0.3f, 6.f, 0.3f };
        { INFO("inside"); CHECK_THAT(insideFraction(seat, hull), ll_test::WithinBits(1.f, 16)); }
        { INFO("half"); CHECK_THAT(insideFraction(barrel, hull), ll_test::WithinBits(0.5f, 16)); }
        { INFO("merged"); CHECK((mergedInto(seat, 100, hull, 5000))); }
        { INFO("sticks out"); CHECK((!mergedInto(barrel, 100, hull, 5000))); }
        { INFO("larger is kept"); CHECK((!mergedInto(seat, 6000, hull, 5000))); }
        { INFO("empty"); CHECK((insideFraction(Rect(), hull) == 0.f)); }
    }

    // Vendor tags and product words tie worn parts to the vehicle.
    TEST_CASE("skoutlinepolicy: Vendor tags and product words tie worn parts to the vehicle", "[skoutlinepolicy]")
    {
        { INFO("tag"); CHECK((nameAffinity("[Chaos] Hydra Base LBA", "[Chaos] Hydra Flak Turret v.3.02"))); }
        { INFO("tag case"); CHECK((nameAffinity("[CHAOS] Something", "[chaos] other"))); }
        { INFO("product word"); CHECK((nameAffinity("Stingray MK2 seat", "stingray airframe"))); }
        { INFO("generic words"); CHECK((!nameAffinity("Tank Body Base", "Turret Body"))); }
        { INFO("unrelated"); CHECK((!nameAffinity("[Chaos] Hydra Base LBA", "Cubitar Stack"))); }
        { INFO("defaults"); CHECK((!nameAffinity("Object", "Object"))); }
        { INFO("empty"); CHECK((!nameAffinity("", ""))); }
    }
}
