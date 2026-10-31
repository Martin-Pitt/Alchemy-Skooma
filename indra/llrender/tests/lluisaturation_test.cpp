/**
 * @file lluisaturation_test.cpp
 * @brief The UI saturation ALUISaturation scopes set on LLRender.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Martin Pitt
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

#include "../llrender.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
    // No program is bound and nothing is batched, so none of this reaches GL.
    TEST_CASE("lluisaturation: with no scope open the UI draws unchanged", "[lluisaturation]")
    {
        CHECK(gGL.getUISaturation() == 1.f);
    }

    TEST_CASE("lluisaturation: a scope sets its saturation and puts the old one back", "[lluisaturation]")
    {
        const U32 before = gGL.getUIColorAdjustHash();
        {
            ALUISaturation grey(0.25f);
            CHECK(gGL.getUISaturation() == 0.25f);
            CHECK(gGL.getUIColorAdjustHash() != before);
        }
        CHECK(gGL.getUISaturation() == 1.f);
    }

    TEST_CASE("lluisaturation: nested scopes multiply and unwind in order", "[lluisaturation]")
    {
        {
            ALUISaturation outer(0.5f);
            {
                ALUISaturation inner(0.5f);
                CHECK(gGL.getUISaturation() == 0.25f);
            }
            CHECK(gGL.getUISaturation() == 0.5f);
        }
        CHECK(gGL.getUISaturation() == 1.f);
    }

    TEST_CASE("lluisaturation: a scope at 1 changes nothing, the hash included", "[lluisaturation]")
    {
        const U32 before = gGL.getUIColorAdjustHash();
        {
            ALUISaturation same(1.f);
            CHECK(gGL.getUIColorAdjustHash() == before);
        }
        CHECK(gGL.getUIColorAdjustHash() == before);
    }

    TEST_CASE("lluisaturation: values outside [0, 1] and NaN are held to it", "[lluisaturation]")
    {
        {
            ALUISaturation below(-3.f);
            CHECK(gGL.getUISaturation() == 0.f);
        }
        {
            ALUISaturation above(4.f);
            CHECK(gGL.getUISaturation() == 1.f);
        }
        {
            ALUISaturation nan(std::numeric_limits<F32>::quiet_NaN());
            CHECK(gGL.getUISaturation() == 1.f);
        }
        CHECK(gGL.getUISaturation() == 1.f);
    }

    TEST_CASE("lluisaturation: a nested scope out of range never brightens the outer one", "[lluisaturation]")
    {
        ALUISaturation outer(0.5f);
        {
            ALUISaturation above(4.f);
            CHECK(gGL.getUISaturation() == 0.5f);
        }
        {
            ALUISaturation nan(std::numeric_limits<F32>::quiet_NaN());
            CHECK(gGL.getUISaturation() == 0.5f);
        }
        CHECK(gGL.getUISaturation() == 0.5f);
    }
}
