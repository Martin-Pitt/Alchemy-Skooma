/**
 * @file sknavannotate_test.cpp
 * @brief Annotation marks: ids, extents, the bundle's bounds and JSON escaping.
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

#include "../sknavannotate.h"

#include <cmath>

namespace
{
    using namespace sknav;

    Mark make(MarkKind kind, std::vector<LLVector3d> verts)
    {
        Mark m;
        m.mKind = kind;
        m.mVerts = std::move(verts);
        return m;
    }

    bool same(F64 a, F64 b) { return std::fabs(a - b) < 1e-9; }
}

namespace tut
{
    struct sknavannotate_data
    {
    };
    typedef test_group<sknavannotate_data> sknavannotate_group;
    typedef sknavannotate_group::object sknavannotate_object;
    tut::sknavannotate_group sknavannotate_tests("sknavannotate");

    template<> template<>
    void sknavannotate_object::test<1>()
    {
        set_test_name("ids are fresh and removal finds by id");
        MarkSet set;
        const U32 a = set.add(make(MarkKind::POINT, { LLVector3d(1, 2, 3) }));
        const U32 b = set.add(make(MarkKind::POINT, { LLVector3d(4, 5, 6) }));
        ensure("distinct", a != b);
        ensure("found", set.find(b) != nullptr);
        ensure("removed", set.remove(a));
        ensure("gone", set.find(a) == nullptr);
        ensure("not twice", !set.remove(a));
        const U32 c = set.add(make(MarkKind::POINT, { LLVector3d(0, 0, 0) }));
        ensure("never reused", c != a && c != b);
    }

    template<> template<>
    void sknavannotate_object::test<2>()
    {
        set_test_name("a box reaches its height up from its lower corner");
        Mark box = make(MarkKind::BOX, { LLVector3d(10, 20, 31), LLVector3d(12, 18, 30) });
        box.mHeight = 3.f;
        LLVector3d lo, hi;
        box.extent(lo, hi);
        ensure("x", same(lo.mdV[VX], 10) && same(hi.mdV[VX], 12));
        ensure("y", same(lo.mdV[VY], 18) && same(hi.mdV[VY], 20));
        ensure("z from the lower corner", same(lo.mdV[VZ], 30) && same(hi.mdV[VZ], 33));
        ensure("complete", box.complete());
        ensure("a one-vertex line is not", !make(MarkKind::LINE, { LLVector3d() }).complete());
    }

    template<> template<>
    void sknavannotate_object::test<3>()
    {
        set_test_name("bounds cover every mark plus the margin");
        MarkSet set;
        LLVector3d lo, hi;
        ensure("nothing without marks", !set.bounds(8.0, lo, hi));
        set.add(make(MarkKind::POINT, { LLVector3d(256100.5, 256200.0, 25.0) }));
        set.add(make(MarkKind::LINE, { LLVector3d(256090.0, 256210.0, 22.0), LLVector3d(256095.0, 256205.0, 28.0) }));
        ensure("bounds", set.bounds(8.0, lo, hi));
        ensure("min", same(lo.mdV[VX], 256082.0) && same(lo.mdV[VY], 256192.0) && same(lo.mdV[VZ], 14.0));
        ensure("max", same(hi.mdV[VX], 256108.5) && same(hi.mdV[VY], 256218.0) && same(hi.mdV[VZ], 36.0));
    }

    template<> template<>
    void sknavannotate_object::test<4>()
    {
        set_test_name("notes escape into a JSON string");
        ensure_equals(jsonEscape("a \"door\"\\\n\x01"), std::string("a \\\"door\\\"\\\\\\n\\u0001"));
        ensure_equals(std::string(markTypeName(MarkType::MISSED_OPENING)), std::string("missed_opening"));
        ensure_equals(std::string(markKindName(MarkKind::BOX)), std::string("box"));
    }
}
