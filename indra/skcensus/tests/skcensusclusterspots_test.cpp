/**
 * @file skcensusclusterspots_test.cpp
 * @brief Cluster probe spots: split by region, one per occupied cell, on the largest clump.
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

#include "../skcensusclusterspots.h"

#include <catch2/catch_test_macros.hpp>

#include <cfloat>

namespace
{
    using namespace skworld;

    RecordPtr box(F64 ox, F64 x0, F64 y0, F64 x1, F64 y1)
    {
        auto r = std::make_shared<Record>();
        r->mOrigin.set(ox, 0.0, 0.0);
        r->mMin.set(x0, y0, 20.0);
        r->mMax.set(x1, y1, 30.0);
        return r;
    }

    Cluster clusterOf(std::initializer_list<RecordPtr> records)
    {
        Cluster c;
        c.mMin.set(DBL_MAX, DBL_MAX, DBL_MAX);
        c.mMax.set(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (const RecordPtr& r : records)
        {
            c.mRecords.push_back(r);
            for (S32 k = 0; k < 3; ++k)
            {
                c.mMin.mdV[k] = llmin(c.mMin.mdV[k], r->mMin.mdV[k]);
                c.mMax.mdV[k] = llmax(c.mMax.mdV[k], r->mMax.mdV[k]);
            }
        }
        return c;
    }

    bool insideAny(F64 x, F64 y, std::initializer_list<RecordPtr> records)
    {
        for (const RecordPtr& r : records)
        {
            if (x >= r->mMin.mdV[VX] && x <= r->mMax.mdV[VX] && y >= r->mMin.mdV[VY] && y <= r->mMax.mdV[VY])
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("two buildings either side of a region edge give two spots, neither in the gap", "[skcensusclusterspots]")
{
    const RecordPtr west = box(0.0, 236.0, 100.0, 252.0, 116.0);
    const RecordPtr east = box(256.0, 260.0, 100.0, 276.0, 116.0);
    const Cluster c = clusterOf({ west, east });
    std::vector<ClusterSpot> spots;
    clusterSpots(c, 32.0, spots);
    REQUIRE(spots.size() == 2);
    for (const ClusterSpot& s : spots)
    {
        CHECK(insideAny(s.mX, s.mY, { west, east }));
        CHECK((s.mX < 252.0 || s.mX > 260.0));
    }
}

TEST_CASE("two buildings across a street in one cell take the larger one", "[skcensusclusterspots]")
{
    const RecordPtr big = box(0.0, 0.0, 0.0, 12.0, 20.0);
    const RecordPtr small = box(0.0, 18.0, 0.0, 24.0, 6.0);
    const Cluster c = clusterOf({ big, small });
    std::vector<ClusterSpot> spots;
    clusterSpots(c, 32.0, spots);
    REQUIRE(spots.size() == 1);
    CHECK(spots[0].mX == 6.0);
    CHECK(spots[0].mY == 10.0);
    CHECK(spots[0].mMax.mdV[VX] == 12.0);
}

TEST_CASE("an L-shaped building gives one spot per occupied cell and none for the empty one", "[skcensusclusterspots]")
{
    // Two wings meeting at the corner: cells (0,0), (1,0) and (0,1) are occupied, (1,1) is empty.
    const RecordPtr south = box(0.0, 0.0, 0.0, 64.0, 30.0);
    const RecordPtr west = box(0.0, 0.0, 30.0, 30.0, 64.0);
    const Cluster c = clusterOf({ south, west });
    std::vector<ClusterSpot> spots;
    clusterSpots(c, 32.0, spots);
    CHECK(spots.size() == 3);
    for (const ClusterSpot& s : spots)
    {
        CHECK(insideAny(s.mX, s.mY, { south, west }));
        CHECK(!(s.mX > 32.0 && s.mY > 32.0));
    }
}
