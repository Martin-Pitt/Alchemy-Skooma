/**
 * @file skroadkits_test.cpp
 * @brief Spike: road kits measured from their own prims, against the offline kit library they replace.
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

#include "skroadkits.h"

#include "llsdjson.h"
#include "llsdutil.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
    // One analysed copy of each kit of the offline library (lane-library.json, 2026-11-07), its members in its root's
    // frame, and what the library says of it; where the library was wrong ("not_library" says why), what the prims say.
    LLSD fixture()
    {
        std::ifstream in(std::string(SK_ROADKITS_TEST_DIR) + "/skroadkits_fixture.json", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        LLSD sd;
        std::string err;
        LlsdFromJsonString(text.str(), sd, &err);
        return sd;
    }

    LLVector3 vec3(const LLSD& a)
    {
        return LLVector3(F32(a[0].asReal()), F32(a[1].asReal()), F32(a[2].asReal()));
    }

    LLQuaternion quat(const LLSD& a)
    {
        return LLQuaternion(F32(a[0].asReal()), F32(a[1].asReal()), F32(a[2].asReal()), F32(a[3].asReal()));
    }

    LLVolumeParams params(const LLSD& vp)
    {
        LLSD sd;
        sd["profile"]["curve"] = vp[0];
        sd["profile"]["begin"] = vp[1];
        sd["profile"]["end"] = vp[2];
        sd["profile"]["hollow"] = vp[3];
        const char* keys[] = { "curve", "begin", "end", "scale_x", "scale_y", "shear_x", "shear_y", "twist_begin", "twist",
                               "radius_offset", "taper_x", "taper_y", "revolutions", "skew" };
        for (S32 i = 0; i < 14; ++i)
        {
            sd["path"][keys[i]] = vp[4 + i];
        }
        sd["sculpt"]["type"] = 0;
        LLVolumeParams p;
        p.fromLLSD(sd);
        return p;
    }

    skroad::KitShape shapeOf(const LLSD& kit)
    {
        skroad::KitShape s;
        s.root_rot = quat(kit["root_rot"]);
        for (const LLSD& m : llsd::inArray(kit["members"]))
        {
            skroad::KitShapePrim p;
            p.params = params(m["vp"]);
            p.scale = vec3(m["scale"]);
            p.pos = vec3(m["pos"]);
            p.rot = quat(m["rot"]);
            s.prims.push_back(std::move(p));
        }
        return s;
    }

    TEST_CASE("skroadkits: a kit measured from its prims has the offline library's frame, deck, kerbs and lanes", "[skroads]")
    {
        const LLSD sd = fixture();
        REQUIRE(sd["kits"].size() > 0);
        for (const LLSD& k : llsd::inArray(sd["kits"]))
        {
            const LLSD& e = k["expect"];
            if (e.has("refused"))
            {
                continue;
            }
            CAPTURE(k["rank"].asInteger(), k["name"].asString());
            skroad::Kit kit;
            std::string why;
            REQUIRE(skroad::measureKit(shapeOf(k), kit, &why));
            CAPTURE(kit.deck_min, kit.deck_max, kit.kerb_min, kit.kerb_max, kit.across, kit.up, kit.up_sign);
            CHECK(kit.up == e["up"][0].asInteger());
            CHECK(kit.up_sign == e["up"][1].asInteger());
            CHECK(kit.across == e["across"].asInteger());
            CHECK(std::fabs(kit.deck_min - e["deck"][0].asReal()) <= 0.15);
            CHECK(std::fabs(kit.deck_max - e["deck"][1].asReal()) <= 0.15);
            CHECK(std::fabs(kit.kerb_min - e["kerb"][0].asReal()) <= 0.3);
            CHECK(std::fabs(kit.kerb_max - e["kerb"][1].asReal()) <= 0.3);
            REQUIRE(kit.lanes.size() == e["lanes"].size());
            for (size_t i = 0; i < kit.lanes.size(); ++i)
            {
                CHECK(std::fabs(kit.lanes[i] - e["lanes"][i].asReal()) <= 0.15);
            }
        }
    }

    TEST_CASE("skroadkits: a piece whose deck changes width along it, or a strip, is not a kit", "[skroads]")
    {
        const LLSD sd = fixture();
        for (const LLSD& k : llsd::inArray(sd["kits"]))
        {
            if (!k["expect"].has("refused"))
            {
                continue;
            }
            CAPTURE(k["rank"].asInteger(), k["expect"]["refused"].asString());
            skroad::Kit kit;
            std::string why;
            CHECK_FALSE(skroad::measureKit(shapeOf(k), kit, &why));
            CAPTURE(why, kit.deck_max - kit.deck_min);
        }
    }

    TEST_CASE("skroadkits: a kit lying another way up is another kit; its measurement is kept by key", "[skroads]")
    {
        skroad::forgetKits();
        const LLQuaternion flat, on_side(F_PI_BY_TWO, LLVector3::x_axis);
        const U64 a = skroad::kitKey(42, flat), b = skroad::kitKey(42, on_side);
        CHECK(a != b);
        CHECK_FALSE(skroad::kitAsked(a));
        skroad::kitAsk(a);
        CHECK(skroad::kitAsked(a));
        CHECK_FALSE(skroad::kitDone(a));
        CHECK(skroad::kitFor(a) == nullptr);
        skroad::Kit k;
        k.deck_min = -4;
        k.deck_max = 4;
        skroad::kitMeasured(a, true, k);
        REQUIRE(skroad::kitFor(a) != nullptr);
        CHECK(skroad::kitFor(a)->deck_max == 4);
        skroad::kitMeasured(b, false, skroad::Kit());
        CHECK(skroad::kitDone(b));
        CHECK(skroad::kitFor(b) == nullptr);
        skroad::forgetKits();
    }
}
