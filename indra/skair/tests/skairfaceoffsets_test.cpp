/**
 * @file skairfaceoffsets_test.cpp
 * @brief Face offsets: where a wall's true face sits inside its solid cell.
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

#include <catch2/catch_test_macros.hpp>

#include "../skair.h"
#include "../skairdistil.h"
#include "../skairraster.h"
#include "../skairfaceoffsets.h"
#include "../skairwalls.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using namespace skworld;

    constexpr F32 CELL = 0.125f;
    constexpr F32 UNIT = 0.1f;
    constexpr S32 CLIMB = 2;

    /// Three columns along x at 0.125 m: air, solid through the jamb band, air.
    AirGrid jambGrid()
    {
        AirGrid g;
        g.mCell = CELL;
        g.mUnit = UNIT;
        g.mWidth = 3;
        g.mHeight = 1;
        AirGap floor;
        floor.mFloor = 0;
        floor.mCeiling = 40;
        floor.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap above;
        above.mFloor = 25;
        above.mCeiling = 40;
        g.mGaps = { floor, above, floor };
        g.mStart = { 0, 1, 2, 3 };
        return g;
    }

    /// A soup of SL quads, nine floats a triangle.
    struct Soup
    {
        std::vector<F32> mVerts;

        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
        {
            for (const LLVector3* t : { &a, &b, &c, &a, &c, &d })
            {
                mVerts.insert(mVerts.end(), t->mV, t->mV + 3);
            }
        }
        /// A wall in the plane x, across the cell and up past the band.
        void wallX(F32 x) { quad(LLVector3(x, -1.f, 0.f), LLVector3(x, 2.f, 0.f), LLVector3(x, 2.f, 3.f), LLVector3(x, -1.f, 3.f)); }
        /// A slab at height z over x0 to x1.
        void slab(F32 x0, F32 x1, F32 z) { quad(LLVector3(x0, -1.f, z), LLVector3(x1, -1.f, z), LLVector3(x1, 2.f, z), LLVector3(x0, 2.f, z)); }
        S32 ntris() const { return static_cast<S32>(mVerts.size() / 9); }
    };

    TEST_CASE("skairfaceoffsets: a jamb 0.3 of a cell in reads 4 steps of 16; rounded towards the air and its far face its own", "[skairfaceoffsets]")
    {
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.wallX(2.f * CELL - 0.5f * CELL);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.ntris(), CLIMB, table, &stats);
        { INFO("both sides of the solid column face a floor"); CHECK(stats.mSides == 2u); }
        { INFO("both are off the boundary"); CHECK(table.size() == static_cast<size_t>(2)); }
        { INFO("the -x face"); CHECK(faceOffset(table, faceKey(1, 0, 3, 0)) == 4.f / 16.f); }
        { INFO("the +x face"); CHECK(faceOffset(table, faceKey(1, 0, 3, 1)) == 8.f / 16.f); }
        { INFO("the residual is under a step"); CHECK((stats.mResidualSum >= 0.0 && stats.mResidualSum < 2.0 * CELL / 16.0)); }
    }

    TEST_CASE("skairfaceoffsets: a wall on the cell edge gives an offset of 0 and no entry", "[skairfaceoffsets]")
    {
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL);
        s.wallX(2.f * CELL);
        s.slab(CELL, 2.f * CELL, 0.6f);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.ntris(), CLIMB, table, &stats);
        { INFO("measured"); CHECK(stats.mMeasured == 2u); }
        { INFO("no entry"); CHECK(table.empty()); }
        { INFO("offset 0"); CHECK(faceOffset(table, faceKey(1, 0, 3, 0)) == 0.f); }
    }

    TEST_CASE("skairfaceoffsets: a side measured over two floors keeps the nearer face and one floor seeing the boundary keeps none", "[skairfaceoffsets]")
    {
        // Column 1 is solid through both floors' bands: the lower sees a jamb 0.3 in, the upper one 0.5 in.
        AirGrid g = jambGrid();
        AirGap upper;
        upper.mFloor = 30;
        upper.mCeiling = 60;
        upper.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap lower = g.mGaps[0];
        lower.mCeiling = 25;
        g.mGaps = { lower, upper, g.mGaps[1], lower, upper };
        g.mGaps[2].mFloor = 55;
        g.mGaps[2].mCeiling = 60;
        g.mStart = { 0, 2, 3, 5 };
        Soup s;
        s.quad(LLVector3(1.3f * CELL, -1.f, 0.f), LLVector3(1.3f * CELL, 2.f, 0.f), LLVector3(1.3f * CELL, 2.f, 2.f), LLVector3(1.3f * CELL, -1.f, 2.f));
        s.quad(LLVector3(1.5f * CELL, -1.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 5.f), LLVector3(1.5f * CELL, -1.f, 5.f));
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.ntris(), CLIMB, table, &stats);
        { INFO("each side measured for both floors"); CHECK(stats.mMeasured == 4u); }
        { INFO("the nearer face, 4 steps"); CHECK(faceOffset(table, faceKey(1, 0, 3, 0)) == 4.f / 16.f); }
        // The upper floor's band meets a wall within a step of the boundary.
        s.quad(LLVector3(CELL + 0.002f, -1.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 5.f), LLVector3(CELL + 0.002f, -1.f, 5.f));
        computeFaceOffsets(g, s.mVerts.data(), s.ntris(), CLIMB, table);
        { INFO("none"); CHECK(faceOffset(table, faceKey(1, 0, 3, 0)) == 0.f); }
        { INFO("another side keeps its own"); CHECK(faceOffset(table, faceKey(1, 0, 3, 1)) > 0.f); }
    }

    TEST_CASE("skairfaceoffsets: a wall face on its cell boundary moves back to the true face behind it", "[skairfaceoffsets]")
    {
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 0.f);     // the floor it stands on runs under it
        std::vector<WallFace> faces(1);
        faces[0].mMin.set(CELL, 0.f, 0.f);
        faces[0].mMax.set(CELL, CELL, 2.f);
        faces[0].mSide = 1;     // looks -x, solid at +x
        alignWallFaces(faces, s.mVerts.data(), s.ntris(), CELL, UNIT);
        { INFO("moved to the wall"); CHECK((std::fabs(faces[0].mMin.mV[VX] - 1.3f * CELL) < 1e-4f && std::fabs(faces[0].mMax.mV[VX] - 1.3f * CELL) < 1e-4f)); }
        { INFO("along it, unmoved"); CHECK(faces[0].mMax.mV[VY] == CELL); }
    }

    TEST_CASE("skairfaceoffsets: a slab resting on the top of the jamb band only touches it and leaves the jamb's offset alone", "[skairfaceoffsets]")
    {
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 15 * UNIT);
        std::vector<FaceOffset> table;
        computeFaceOffsets(g, s.mVerts.data(), s.ntris(), CLIMB, table);
        { INFO("the -x face"); CHECK(faceOffset(table, faceKey(1, 0, 3, 0)) == 4.f / 16.f); }
    }
    TEST_CASE("skairfaceoffsets: a padded table crops to the tile's own columns", "[skairfaceoffsets]")
    {
        std::vector<FaceOffset> padded = { FaceOffset(faceKey(1, 1, 8, 2), 3), FaceOffset(faceKey(2, 3, 8, 0), 5), FaceOffset(faceKey(7, 7, 8, 1), 9) };
        std::vector<FaceOffset> out;
        cropFaceOffsets(padded, 8, 2, 4, 4, out);
        { INFO("one inside"); CHECK(out.size() == static_cast<size_t>(1)); }
        { INFO("at its own column"); CHECK(out[0].key() == faceKey(0, 1, 4, 0)); }
        { INFO("with its offset"); CHECK(out[0].offset() == 5u); }
    }
}
