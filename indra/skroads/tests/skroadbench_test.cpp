/**
 * @file skroadbench_test.cpp
 * @brief Spike: the road model reads a road laid from plain data, as the road bench feeds it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "skroadbenchcase.h"

#include "llsdutil.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    // Two 40 m by 10 m LDPW slabs, unnamed, end to end along +x: a two-lane Linden road.
    LLSD straightRoad()
    {
        const std::string governor = "3d6181b0-6a4b-97ef-18d8-722652995cf1";
        const std::string ldpw = "4a18e287-db5b-0551-2500-087da6e89efb";
        auto slab = [](const std::string& id, F64 cx)
        {
            LLSD r;
            r["part"] = id;
            r["root"] = id;
            r["shape"] = "box";
            r["collides"] = true;
            r["origin"] = llsd::array(256000.0, 256000.0, 0.0);
            r["min"] = llsd::array(256000.0 + cx - 20, 255995.0, 19.75);
            r["max"] = llsd::array(256000.0 + cx + 20, 256005.0, 20.25);
            r["center"] = llsd::array(cx, 0.0, 20.0);
            r["axes"] = llsd::array(llsd::array(1.0, 0.0, 0.0), llsd::array(0.0, 1.0, 0.0), llsd::array(0.0, 0.0, 1.0));
            r["half"] = llsd::array(20.0, 5.0, 0.25);
            return r;
        };
        const std::string a = "11111111-0000-0000-0000-000000000001", b = "11111111-0000-0000-0000-000000000002";
        LLSD sd;
        sd["name"] = "straight";
        sd["records"] = llsd::array(slab(a, 20), slab(b, 60));
        for (const std::string& id : { a, b })
        {
            sd["props"][id]["name"] = "Object";
            sd["props"][id]["owner"] = governor;
            sd["props"][id]["group"] = ldpw;
        }
        sd["linden_owners"] = llsd::array(governor);
        sd["poses"] = llsd::array(llsd::array(0.0, 256005.0, 255998.0, 20.5, 0.0), llsd::array(3.0, 256035.0, 255998.0, 20.5, 0.0));
        return sd;
    }

    TEST_CASE("skroadbench: a two-lane Linden road's lane line lies the car's half width and half a metre right of its middle", "[skroads]")
    {
        skroadbench::Case c;
        std::string why;
        REQUIRE(skroadbench::readCase(straightRoad(), c, why));
        std::vector<skroadbench::Step> steps;
        skroadbench::runCase(c, [&](const skroadbench::Step& s) { steps.push_back(s); });
        REQUIRE(!steps.empty());
        for (const skroadbench::Step& s : steps)
        {
            INFO("t " << s.t << " source " << s.source << " walk end " << s.walk_end);
            REQUIRE(s.path.size() >= 2);
            F64 length = 0;
            for (size_t i = 0; i < s.path.size(); ++i)
            {
                CHECK(std::fabs(s.path[i].y - (256000.0 - c.car_half_w - 0.5)) < 0.05);
                if (i)
                {
                    length += std::hypot(s.path[i].x - s.path[i - 1].x, s.path[i].y - s.path[i - 1].y);
                }
            }
            // to the road's far end
            CHECK(std::fabs(s.path.back().x - 256080.0) < 2.5);
            CHECK(length > 40.0);
        }
    }
    TEST_CASE("skroadbench: a terminal square ends at the forward boundary", "[skroads]")
    {
        LLSD data = straightRoad();
        LLSD slab = data["records"][0];
        slab["half"][1] = 20.0;
        slab["min"][1] = 255980.0;
        slab["max"][1] = 256020.0;
        data["records"] = llsd::array(slab);
        data["poses"] = llsd::array(llsd::array(0.0, 256020.0, 256000.0, 20.5, 20.0));
        skroadbench::Case c;
        std::string why;
        REQUIRE(skroadbench::readCase(data, c, why));
        skroadbench::runCase(c, [&](const skroadbench::Step& step)
        {
            REQUIRE(step.path.size() >= 2);
            CHECK(step.path.back().x <= 256041.0);
            CHECK(step.path.back().x >= 256037.0);
        });
    }

    TEST_CASE("a prim recorded by its volume params is rebuilt where it stood", "[skroadbench]")
    {
        // a 10 x 4 x 0.5 box, turned 90 degrees about z, at (100, 50, 20): the survey's "vp" of a default box
        LLSD r;
        r["pos"] = llsd::array(100.0, 50.0, 20.0);
        r["rot"] = llsd::array(0.0, 0.0, 0.70710678, 0.70710678);
        r["scale"] = llsd::array(10.0, 4.0, 0.5);
        r["vp"] = llsd::array(1, 0.0, 1.0, 0.0, 16, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0);
        skroadbench::Rebuilt out;
        REQUIRE(skroadbench::rebuildPart(r, "", out));
        CHECK(out.faces.size() == 6);
        REQUIRE(!out.physics.empty());
        CHECK(out.physics.size() % 3 == 0);
        LLVector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        for (const LLVector3& v : out.physics)
        {
            lo.set(llmin(lo.mV[0], v.mV[0]), llmin(lo.mV[1], v.mV[1]), llmin(lo.mV[2], v.mV[2]));
            hi.set(llmax(hi.mV[0], v.mV[0]), llmax(hi.mV[1], v.mV[1]), llmax(hi.mV[2], v.mV[2]));
        }
        // turned: its 10 m side now runs along y
        CHECK(std::fabs(lo.mV[0] - 98.f) < 1e-3f);
        CHECK(std::fabs(hi.mV[0] - 102.f) < 1e-3f);
        CHECK(std::fabs(lo.mV[1] - 45.f) < 1e-3f);
        CHECK(std::fabs(hi.mV[1] - 55.f) < 1e-3f);
        CHECK(std::fabs(hi.mV[2] - 20.25f) < 1e-3f);
        for (const skroadbench::RebuiltFace& face : out.faces)
        {
            CHECK(face.uvs.size() == face.positions.size());
        }
    }

    TEST_CASE("a mesh missing from the cache says so", "[skroadbench]")
    {
        LLSD r;
        r["pos"] = llsd::array(0.0, 0.0, 0.0);
        r["rot"] = llsd::array(0.0, 0.0, 0.0, 1.0);
        r["scale"] = llsd::array(1.0, 1.0, 1.0);
        r["mesh"] = "22222222-0000-0000-0000-000000000001";
        r["phys_type"] = 0;
        skroadbench::Rebuilt out;
        CHECK(!skroadbench::rebuildPart(r, "no-such-cache", out));
        CHECK(out.why.find("not in the cache") != std::string::npos);
    }

    TEST_CASE("a cube's hull is twelve outward triangles", "[skroadbench]")
    {
        std::vector<LLVector3> points;
        for (S32 i = 0; i < 8; ++i)
        {
            points.emplace_back(i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : -1.f);
        }
        points.emplace_back(0.f, 0.f, 0.f);         // inside: no part of the hull
        points.emplace_back(0.5f, 0.2f, -0.3f);
        std::vector<LLVector3> tris;
        skroadbench::convexHull(points, tris);
        REQUIRE(tris.size() == 36);
        F32 volume = 0.f;
        for (size_t i = 0; i < tris.size(); i += 3)
        {
            const LLVector3 n = (tris[i + 1] - tris[i]) % (tris[i + 2] - tris[i]);
            CHECK(n * tris[i] > 0.f);           // faces away from the centre
            volume += tris[i] * (tris[i + 1] % tris[i + 2]) / 6.f;
        }
        CHECK(std::fabs(volume - 8.f) < 1e-3f);
    }

}
