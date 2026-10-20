/**
 * @file skmapmarkers_test.cpp
 * @brief Mini-map marker codes: the sequence, lowest free, holds, stability, and the radius.
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

#include "../skmapmarkers.h"

#include <cmath>

namespace
{
    LLUUID idOf(S32 i)
    {
        LLUUID id;
        id.mData[0] = static_cast<U8>(i + 1);
        id.mData[1] = static_cast<U8>((i + 1) >> 8);
        return id;
    }
}

namespace tut
{
    struct skmapmarkers_data {};
    typedef test_group<skmapmarkers_data> skmapmarkers_group;
    typedef skmapmarkers_group::object skmapmarkers_object;
    tut::skmapmarkers_group g("skmapmarkers");

    // 1-9, A-Z, then AA.
    template<> template<>
    void skmapmarkers_object::test<1>()
    {
        ensure_equals("0", skmap::codeString(0), std::string("1"));
        ensure_equals("8", skmap::codeString(8), std::string("9"));
        ensure_equals("9", skmap::codeString(9), std::string("A"));
        ensure_equals("34", skmap::codeString(34), std::string("Z"));
        ensure_equals("35", skmap::codeString(35), std::string("AA"));
        ensure_equals("36", skmap::codeString(36), std::string("AB"));
        ensure_equals("60", skmap::codeString(60), std::string("AZ"));
        ensure_equals("61", skmap::codeString(61), std::string("BA"));
    }

    // The lowest free index; a code holds for the whole hold, then is reused lowest first.
    template<> template<>
    void skmapmarkers_object::test<2>()
    {
        skmap::MarkerCodes codes;
        codes.setHold(120.f);
        ensure_equals("first", codes.codeFor(idOf(0), 0.0), 0u);
        ensure_equals("second", codes.codeFor(idOf(1), 0.0), 1u);
        ensure_equals("third", codes.codeFor(idOf(2), 0.0), 2u);
        // The first two go out of sight; the third stays.
        for (F64 t = 1.0; t <= 119.0; t += 1.0)
        {
            codes.codeFor(idOf(2), t);
        }
        ensure_equals("still held at 119 s", codes.codeFor(idOf(3), 119.5), 3u);
        ensure("held", codes.has(idOf(0)) && codes.has(idOf(1)));
        codes.codeFor(idOf(2), 121.0);
        codes.codeFor(idOf(3), 121.0);
        ensure_equals("released, lowest first", codes.codeFor(idOf(4), 121.0), 0u);
        ensure_equals("then the next", codes.codeFor(idOf(5), 121.0), 1u);
        ensure("the old holders lost theirs", !codes.has(idOf(0)) && !codes.has(idOf(1)));
    }

    // A code is stable while others come and go.
    template<> template<>
    void skmapmarkers_object::test<3>()
    {
        skmap::MarkerCodes codes;
        codes.setHold(5.f);
        const U32 mine = codes.codeFor(idOf(7), 0.0);
        for (S32 i = 0; i < 40; ++i)
        {
            codes.codeFor(idOf(100 + i), i * 2.0);
            ensure_equals("stable", codes.codeFor(idOf(7), i * 2.0), mine);
        }
    }

    // The marker radius, all four cases.
    template<> template<>
    void skmapmarkers_object::test<4>()
    {
        ensure_equals("chevron", skmap::markerRadius(true, true, 2, 20.f, 8.f, 0.5f), 4.5f);
        ensure_equals("uncoded", skmap::markerRadius(false, false, 0, 0.f, 8.f, 0.5f), 3.f);
        ensure_equals("one character", skmap::markerRadius(false, true, 1, 5.f, 8.f, 0.5f), 4.5f);
        const F32 wide = skmap::markerRadius(false, true, 2, 12.f, 8.f, 0.5f);
        ensure("two characters grow", std::fabs(wide - (std::hypot(6.f, 8.f * 0.36f) + 0.5f)) < 1e-5f);
        ensure("never under the single", skmap::markerRadius(false, true, 2, 1.f, 8.f, 0.5f) == 4.5f);
    }
}
