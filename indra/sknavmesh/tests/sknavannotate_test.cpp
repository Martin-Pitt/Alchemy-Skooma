/**
 * @file sknavannotate_test.cpp
 * @brief Annotation marks: ids, extents, types, keys and shapes, and how they are written.
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
#include <sstream>

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
        set_test_name("a line's extent covers its vertices");
        const Mark line = make(MarkKind::LINE, { LLVector3d(10, 20, 31), LLVector3d(12, 18, 30), LLVector3d(11, 19, 33) });
        LLVector3d lo, hi;
        line.extent(lo, hi);
        ensure("x", same(lo.mdV[VX], 10) && same(hi.mdV[VX], 12));
        ensure("y", same(lo.mdV[VY], 18) && same(hi.mdV[VY], 20));
        ensure("z", same(lo.mdV[VZ], 30) && same(hi.mdV[VZ], 33));
        ensure("complete", line.complete());
        ensure("a one-vertex line is not", !make(MarkKind::LINE, { LLVector3d() }).complete());
        ensure("a point is one vertex", make(MarkKind::POINT, { LLVector3d() }).complete());
        Mark portal = make(MarkKind::PORTAL, { LLVector3d(5, 5, 5) });
        ensure("a portal without its spaces is not", !portal.complete());
        portal.mSpaceIds[0] = 7;
        portal.mSpaceIds[1] = 3;
        ensure("a portal with them is", portal.complete());
        ensure("the pair is unordered", portal.portalPair() == std::make_pair(U32(3), U32(7)));
    }

    template<> template<>
    void sknavannotate_object::test<3>()
    {
        set_test_name("marks write with their kind, type, note and located vertices");
        std::vector<Mark> marks;
        Mark door = make(MarkKind::LINE, { LLVector3d(256010.5, 255754.0, 22.0), LLVector3d(256012.0, 255756.0, 22.0) });
        door.mId = 3;
        door.mType = MarkType::DOOR;
        door.mText = "a \"front\" door";
        marks.push_back(door);
        Mark far_point = make(MarkKind::POINT, { LLVector3d(1.0, 2.0, 3.0) });
        far_point.mId = 4;
        far_point.mType = MarkType::MERGE;
        marks.push_back(far_point);
        Mark crossed = make(MarkKind::PORTAL, { LLVector3d(256004.0, 255750.0, 21.5) });
        crossed.mId = 5;
        crossed.mType = MarkType::CROSS;
        crossed.mSpaceMesh.generate();
        const std::string mesh_id = crossed.mSpaceMesh.asString();
        crossed.mSpaceIds[0] = 9;
        crossed.mSpaceIds[1] = 2;
        crossed.mPortalCentre.set(256004.5, 255750.5, 21.0);
        marks.push_back(crossed);
        std::ostringstream out;
        writeMarks(out, marks, [](const LLVector3d& v, MarkPlace& place) {
            if (v.mdV[VX] < 256000.0)
            {
                return false;
            }
            place.mRegion = "Sim";
            place.mGridX = 1000;
            place.mGridY = 999;
            place.mLocal.set(static_cast<F32>(v.mdV[VX] - 256000.0), static_cast<F32>(v.mdV[VY] - 255744.0), static_cast<F32>(v.mdV[VZ]));
            return true;
        });
        ensure_equals(out.str(),
                      std::string("\"marks\":[{\"id\":3,\"kind\":\"line\",\"type\":\"door\",\"text\":\"a \\\"front\\\" door\",\"created\":0,"
                                  "\"vertices\":[{\"global\":[256010.5,255754,22],\"region\":\"Sim\",\"regionGrid\":[1000,999],"
                                  "\"local\":[10.5,10,22]},{\"global\":[256012,255756,22],\"region\":\"Sim\",\"regionGrid\":[1000,999],"
                                  "\"local\":[12,12,22]}]},{\"id\":4,\"kind\":\"point\",\"type\":\"merge\",\"text\":\"\",\"created\":0,"
                                  "\"vertices\":[{\"global\":[1,2,3]}]},{\"id\":5,\"kind\":\"portal\",\"type\":\"crossed\",\"text\":\"\",\"created\":0,"
                                  "\"mesh\":\"" + mesh_id + "\",\"spaces\":[9,2],\"centre\":[256004.5,255750.5,21],"
                                  "\"vertices\":[{\"global\":[256004,255750,21.5],\"region\":\"Sim\",\"regionGrid\":[1000,999],"
                                  "\"local\":[4,6,21.5]}]}]"));
    }

    template<> template<>
    void sknavannotate_object::test<4>()
    {
        set_test_name("notes escape into a JSON string, and every type has a stable name");
        ensure_equals(jsonEscape("a \"door\"\\\n\x01"), std::string("a \\\"door\\\"\\\\\\n\\u0001"));
        const char* names[] = { "missed_opening", "wrong", "door", "window", "stair", "ramp", "ladder", "elevator",
                                "ledge", "indoor", "sheltered", "outdoor", "street", "bridge", "balcony", "courtyard",
                                "water", "walkable", "not_walkable", "merge", "split", "crossed", "note" };
        ensure_equals("every type named", sizeof(names) / sizeof(names[0]), static_cast<size_t>(MarkType::COUNT));
        for (S32 t = 0; t < static_cast<S32>(MarkType::COUNT); ++t)
        {
            ensure_equals(names[t], std::string(markTypeName(static_cast<MarkType>(t))), std::string(names[t]));
        }
        ensure_equals(std::string(markKindName(MarkKind::LINE)), std::string("line"));
        ensure_equals(std::string(markKindName(MarkKind::PORTAL)), std::string("portal"));
    }

    template<> template<>
    void sknavannotate_object::test<5>()
    {
        set_test_name("every type has its own key, clear of movement, and groups run in order");
        std::string seen;
        MarkGroup last = MarkGroup::OPENING;
        for (S32 t = 0; t < static_cast<S32>(MarkType::COUNT); ++t)
        {
            const MarkType type = static_cast<MarkType>(t);
            const char key = markTypeKey(type);
            ensure(markTypeName(type), (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'));
            ensure("unique", seen.find(key) == std::string::npos);
            ensure("not a movement key", std::string("WASDECFQZ").find(key) == std::string::npos);
            seen += key;
            MarkType back = MarkType::COUNT;
            ensure("the key picks it", markTypeForKey(key, back) && back == type);
            ensure("groups in order", markTypeGroup(type) >= last);
            last = markTypeGroup(type);
        }
        MarkType none;
        ensure("W is movement", !markTypeForKey('W', none));
        ensure_equals(static_cast<S32>(markTypeGroup(MarkType::LADDER)), static_cast<S32>(MarkGroup::VERTICAL));
        ensure_equals(static_cast<S32>(markTypeGroup(MarkType::WATER_BODY)), static_cast<S32>(MarkGroup::AREA));
    }

    template<> template<>
    void sknavannotate_object::test<6>()
    {
        set_test_name("the shape follows the type");
        const MarkType lines[] = { MarkType::MISSED_OPENING, MarkType::DOOR, MarkType::WINDOW, MarkType::LADDER, MarkType::ELEVATOR,
                                   MarkType::STAIR, MarkType::RAMP, MarkType::STREET, MarkType::BRIDGE, MarkType::LEDGE, MarkType::SPLIT };
        S32 line_count = 0;
        for (const MarkType t : lines)
        {
            ensure(markTypeName(t), markTypeKind(t) == MarkKind::LINE);
        }
        for (S32 t = 0; t < static_cast<S32>(MarkType::COUNT); ++t)
        {
            line_count += markTypeKind(static_cast<MarkType>(t)) == MarkKind::LINE ? 1 : 0;
        }
        ensure_equals("the rest are not lines", line_count, static_cast<S32>(sizeof(lines) / sizeof(lines[0])));
        ensure("a merge is one point where the spaces meet", markTypeKind(MarkType::MERGE) == MarkKind::POINT);
        ensure("a wrong portal is a point near it", markTypeKind(MarkType::WRONG) == MarkKind::POINT);
        ensure("a crossed portal names the cut", markTypeKind(MarkType::CROSS) == MarkKind::PORTAL);
    }
}
