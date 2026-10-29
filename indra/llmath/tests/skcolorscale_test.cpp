/**
 * @file skcolorscale_test.cpp
 * @brief The colour scales against the Python oracle's tables.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "../skcolorscale.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string>

namespace
{
    struct SKScaleCase
    {
        const char* scale;
        F64         value;
        U8          r, g, b, a;
    };
    struct SKBakeCase
    {
        const char* scale;
        S32         n, i;
        U8          r, g, b, a;
    };
    struct SKUnitCase
    {
        const char* scale;
        F64         value, t;
    };

#include "skcolorscale_oracle.inc"

    LLColor4 rgb(F32 r, F32 g, F32 b) { return LLColor4(r, g, b, 1.f); }

    // The oracle's custom scales, built as a consumer would.
    SKColorScale custom(const std::string& name)
    {
        const SKColorScale& series = SKColorScales::series();
        std::vector<SKColorStop> four;
        for (S32 i = 0; i < 4; ++i)
        {
            four.push_back({ (F32)i / 3.f, series.classColour(i) });
        }
        if (name == "log")
        {
            return SKColorScale(SKColorScales::heat()).setTransform(SKColorScale::Transform::Log).setDomain(1.0, 1000.0);
        }
        if (name == "sqrt")
        {
            return SKColorScale(SKColorScales::ink()).setTransform(SKColorScale::Transform::Sqrt).setDomain(0.0, 100.0);
        }
        if (name == "div")
        {
            return SKColorScale(SKColorScales::signedScale()).setDomain(-1.0, 3.0).setMid(0.0);
        }
        if (name == "binned" || name == "binclamp")
        {
            SKColorScale s("bins", SKColorScale::Kind::Binned, four);
            s.setEdges({ 0.0, 10.0, 20.0, 50.0, 100.0 });
            if (name == "binned")
            {
                s.setUnder(rgb(.5f, .5f, .5f)).setOver(rgb(1.f, 1.f, 1.f));
            }
            return s;
        }
        if (name == "logzero")
        {
            return SKColorScale(SKColorScales::heat()).setDomain(0.0, 100.0).setTransform(SKColorScale::Transform::Log);
        }
        if (name == "divend")
        {
            return SKColorScale(SKColorScales::signedScale()).setMid(-1.0);
        }
        if (name == "flat")
        {
            return SKColorScale(SKColorScales::heat()).setDomain(5.0, 5.0);
        }
        return SKColorScale(SKColorScales::ink()).setUnder(rgb(1.f, 0.f, 0.f)).setOver(rgb(0.f, 0.f, 1.f));
    }

    bool withinByte(U8 a, U8 b) { return std::abs((S32)a - (S32)b) <= 1; }

    template <typename C> void checkColour(const LLColor4& c, const C& want)
    {
        CAPTURE(want.scale, c.mV[0], c.mV[1], c.mV[2]);
        CHECK(withinByte(SKColorScale::toByte(c.mV[0]), want.r));
        CHECK(withinByte(SKColorScale::toByte(c.mV[1]), want.g));
        CHECK(withinByte(SKColorScale::toByte(c.mV[2]), want.b));
        CHECK(withinByte(SKColorScale::toByte(c.mV[3]), want.a));
    }

    TEST_CASE("skcolorscale: every built-in scale matches the oracle to one in 255", "[skcolorscale]")
    {
        for (const SKScaleCase& want : BUILTIN_CASES)
        {
            const SKColorScale* s = SKColorScales::byName(want.scale);
            REQUIRE(s != nullptr);
            CAPTURE(want.value);
            checkColour(s->colour(want.value), want);
        }
    }

    TEST_CASE("skcolorscale: transforms bins and clip colours match the oracle", "[skcolorscale]")
    {
        for (const SKScaleCase& want : CUSTOM_CASES)
        {
            CAPTURE(want.value);
            checkColour(custom(want.scale).colour(want.value), want);
        }
        for (const SKUnitCase& want : UNIT_CASES)
        {
            CAPTURE(want.scale, want.value);
            CHECK(std::fabs(custom(want.scale).unitOf(want.value) - want.t) < 1e-9);
            const std::string name = want.scale;
            if (want.t >= 0.0 && want.t <= 1.0 && (name == "log" || name == "sqrt" || name == "div"))
            {
                CHECK(std::fabs(custom(want.scale).valueOf(want.t) - want.value) < 1e-9 * (1.0 + std::fabs(want.value)));
            }
        }
    }

    TEST_CASE("skcolorscale: bake samples texel centres as the oracle does", "[skcolorscale]")
    {
        for (const SKBakeCase& want : BAKE_CASES)
        {
            const std::vector<U8> bytes = SKColorScales::byName(want.scale)->bake(want.n);
            REQUIRE(bytes.size() == (size_t)want.n * 4);
            CAPTURE(want.scale, want.n, want.i);
            const U8* p = &bytes[(size_t)want.i * 4];
            CHECK(withinByte(p[0], want.r));
            CHECK(withinByte(p[1], want.g));
            CHECK(withinByte(p[2], want.b));
            CHECK(withinByte(p[3], want.a));
        }
    }

    TEST_CASE("skcolorscale: bake at a texel centre equals at of that position", "[skcolorscale]")
    {
        const SKColorScale& heat  = SKColorScales::heat();
        const S32           n     = 256;
        const auto          bytes = heat.bake(n);
        for (S32 i = 0; i < n; ++i)
        {
            const LLColor4 c = heat.at(((F32)i + 0.5f) / (F32)n);
            for (S32 k = 0; k < 4; ++k)
            {
                CHECK(bytes[(size_t)i * 4 + k] == SKColorScale::toByte(c.mV[k]));
            }
        }
    }

    TEST_CASE("skcolorscale: a degenerate domain gives finite uniforms and positions", "[skcolorscale]")
    {
        for (const std::string& name : { "logzero", "divend", "flat" })
        {
            CAPTURE(name);
            const SKColorScale           s = custom(name);
            const SKColorScale::Uniforms u = s.uniforms();
            CHECK(std::isfinite(u.lo));
            CHECK(std::isfinite(u.hi));
            CHECK(std::isfinite(u.mid));
            CHECK(std::isfinite(s.valueOf(0.5)));
            CHECK(std::isfinite(s.unitOf(3.0)));
        }
    }

    TEST_CASE("skcolorscale: NaN maps to the low end and to no class", "[skcolorscale]")
    {
        const F64 nan = std::nan("");
        CHECK(SKColorScales::heat().unitOf(nan) == 0.0);
        CHECK(SKColorScales::series().classOf(nan) == -1);
        CHECK(SKColorScales::series().colour(nan) == LLColor4(0.5f, 0.5f, 0.5f, 1.f));
    }

    TEST_CASE("skcolorscale: setters bump the revision and uniforms carry the stops", "[skcolorscale]")
    {
        SKColorScale s(SKColorScales::heat());
        const U32    r0 = s.revision();
        s.setDomain(0.0, 10.0);
        CHECK(s.revision() != r0);
        const SKColorScale::Uniforms u = s.uniforms();
        CHECK(u.count == 3);
        CHECK(u.hi == 10.f);
        CHECK(u.mid == 5.f);
        CHECK(u.c[1] == s.stops()[1].c);
        CHECK(u.under.mV[3] == 0.f);
    }

    TEST_CASE("skcolorscale: a sampled scale reproduces its function at the samples", "[skcolorscale]")
    {
        const SKColorScale s = SKColorScale::sampled("grey", [](F32 t) { return LLColor4(t * t, t, 1.f - t, 1.f); }, 8);
        CHECK(s.stops().size() == 9);
        CHECK(s.colour(0.5) == LLColor4(0.25f, 0.5f, 0.5f, 1.f));
        CHECK(SKColorScales::byName("nothing") == nullptr);
        CHECK(SKColorScales::names().size() == 8);
    }
}
