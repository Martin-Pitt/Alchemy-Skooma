/**
 * @file skoverviewcuttable_test.cpp
 * @brief SKOverviewCutTable's rows and when they are due to publish, without GL.
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

#include "../skoverviewcuttable.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>

namespace
{
    using Row = SKOverviewCutTable::Row;

    // What a publish would carry, or nothing.
    struct Taken
    {
        bool mDue = false;
        const Row* mRows = nullptr;
        U32 mCount = 0;
    };

    Taken take(SKOverviewCutTable& table)
    {
        Taken t;
        t.mDue = table.takePending(t.mRows, t.mCount);
        return t;
    }

    TEST_CASE("skoverviewcuttable: an unchanged table publishes nothing", "[skoverviewcuttable]")
    {
        SKOverviewCutTable table;
        table.resize(4);
        table.row(2).mCut = 12.5f;
        REQUIRE(take(table).mDue);
        CHECK_FALSE(take(table).mDue);
        // Rows asked for and given the values they hold are no change.
        table.row(2).mCut = 12.5f;
        table.row(0).mCut = SKOverviewCutTable::WHOLE;
        CHECK_FALSE(take(table).mDue);
    }

    TEST_CASE("skoverviewcuttable: one changed row publishes the whole table once", "[skoverviewcuttable]")
    {
        SKOverviewCutTable table;
        table.resize(3000);
        REQUIRE(take(table).mDue);
        table.row(2999).mCut = 40.f;
        table.row(2999).mMaxZ = 48.f;
        const Taken t = take(table);
        REQUIRE(t.mDue);
        REQUIRE(t.mRows != nullptr);
        CHECK(t.mCount == 3000);
        CHECK(t.mRows[2999].mCut == 40.f);
        CHECK(t.mRows[2999].mMaxZ == 48.f);
        CHECK(t.mRows[0].mCut == SKOverviewCutTable::WHOLE);
        CHECK_FALSE(take(table).mDue);
    }

    TEST_CASE("skoverviewcuttable: a resized table is pending until its first publish", "[skoverviewcuttable]")
    {
        SKOverviewCutTable table;
        table.resize(8);
        REQUIRE(take(table).mDue);
        table.row(5).mCut = 3.f;
        REQUIRE(take(table).mDue);
        // A new build of the same size with every row whole is still due: what was published held a cut.
        table.resize(8);
        const Taken t = take(table);
        REQUIRE(t.mDue);
        CHECK(t.mCount == 8);
        CHECK(t.mRows[5].mCut == SKOverviewCutTable::WHOLE);
        // So is one the same as the last publish: a build's rows were never published, whatever they hold.
        table.resize(8);
        CHECK(take(table).mDue);
        CHECK_FALSE(take(table).mDue);
        // And an empty one, which has nothing to carry.
        table.resize(0);
        const Taken empty = take(table);
        CHECK(empty.mDue);
        CHECK(empty.mCount == 0);
    }

    TEST_CASE("skoverviewcuttable: whole and hidden encode as the sentinels the census uses", "[skoverviewcuttable]")
    {
        // overviewGeomV.glsl tests a cut against these: whole above 1e29, hidden below -1e29.
        CHECK(SKOverviewCutTable::WHOLE == 1.0e30f);
        CHECK(SKOverviewCutTable::HIDDEN == -1.0e30f);
        CHECK(SKOverviewCutTable::WHOLE > 1.0e29f);
        CHECK(SKOverviewCutTable::HIDDEN < -1.0e29f);
        // A row is one vec4: cut, flags, lowest z, highest z, at 0, 4, 8 and 12.
        CHECK(sizeof(Row) == 16);
        CHECK(offsetof(Row, mCut) == 0);
        CHECK(offsetof(Row, mFlags) == 4);
        CHECK(offsetof(Row, mMinZ) == 8);
        CHECK(offsetof(Row, mMaxZ) == 12);
        const Row fresh;
        CHECK(fresh.mCut == SKOverviewCutTable::WHOLE);
        CHECK(fresh.mFlags == 0.f);
        SKOverviewCutTable table;
        table.resize(2);
        table.row(1).mCut = SKOverviewCutTable::HIDDEN;
        const Taken t = take(table);
        REQUIRE(t.mDue);
        CHECK(t.mRows[0].mCut == SKOverviewCutTable::WHOLE);
        CHECK(t.mRows[1].mCut == SKOverviewCutTable::HIDDEN);
        // The bytes the shader reads are the float's own.
        F32 bits = 0.f;
        memcpy(&bits, reinterpret_cast<const U8*>(t.mRows) + sizeof(Row), sizeof(F32));
        CHECK(bits == -1.0e30f);
    }
}
