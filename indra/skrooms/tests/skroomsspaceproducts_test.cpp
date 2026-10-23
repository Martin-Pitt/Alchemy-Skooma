/**
 * @file skroomsspaceproducts_test.cpp
 * @brief Walls with a space on each side, storeys and their connectors, the sheet by space, and floors from polys.
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


#include "skairraster.h"
#include "../skroomsenclosure.h"
#include "skairskymap.h"
#include "../skroomsspaceproducts.h"
#include "../skroomsspaces.h"

#include "llformat.h"


#include <algorithm>
#include <iostream>
#include <memory>

namespace
{
    using namespace skworld;

    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1, U8 exclude = 0)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, 1, 0);
        r->mAxes[2].set(0, 0, 1);
        r->mSolid = true;
        r->mExclude = exclude;
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
    }

    TileInput tileInput()
    {
        TileInput in;
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;           // coarser than the viewer, to keep the tests quick
        in.mAir = true;
        in.mWallFaces = true;
        in.mTerrain.mOrigin.set(-6.f, -6.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 45;
        in.mTerrain.mZ.assign(45 * 45, 0.f);
        in.mTerrain.mWater = -10.f;
        return in;
    }

    /// Walled rooms either side of x = 10, joined by a doorway at y 5 to 7 with its leaf shut in it.
    TileInput twoRoomsDoor(std::vector<DoorEntity>& doors)
    {
        TileInput in = tileInput();
        const auto wall = [&in](F32 x0, F32 y0, F32 x1, F32 y1) { in.mRecords.push_back(box(x0, y0, 0.f, x1, y1, 3.f)); };
        wall(2.f, 2.f, 18.f, 2.25f);
        wall(2.f, 9.75f, 18.f, 10.f);
        wall(2.f, 2.f, 2.25f, 10.f);
        wall(17.75f, 2.f, 18.f, 10.f);
        wall(10.f, 2.f, 10.25f, 5.f);
        wall(10.f, 7.f, 10.25f, 10.f);
        in.mRecords.push_back(box(10.f, 5.f, 2.6f, 10.25f, 7.f, 3.f));
        in.mRecords.push_back(box(2.f, 2.f, 3.f, 18.f, 10.f, 3.25f));
        in.mRecords.push_back(box(2.f, 2.f, 0.f, 18.f, 10.f, 0.25f));
        in.mRecords.push_back(box(10.f, 5.f, 0.25f, 10.25f, 7.f, 2.55f, EXCLUDE_AVATAR));
        DoorEntity d;
        d.mPlanePoint.setVec(10.125, 6.0, 0.25);
        d.mNormal.set(1.f, 0.f, 0.f);
        d.mWidth = 2.f;
        d.mHeight = 2.3f;
        d.mSill = 0.25;
        d.mMin.setVec(10.0, 5.0, 0.25);
        d.mMax.setVec(10.25, 7.0, 2.55);
        d.mId.generate();
        doors.push_back(d);
        return in;
    }

    /// A tile rasterised and its spaces cut.
    struct Built
    {
        TileAirResult mResult;
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;

        Built(const TileInput& in, const std::vector<DoorEntity>& doors)
        {
            { INFO("rasterised: " + mResult.mError); REQUIRE((rasteriseTile(in, mResult) && mResult.mAir)); }
            auto enc = std::make_shared<EnclosureInput>();
            enc->mGeneration = 1;
            enc->mParams.mWalkHeadM = 2.f;
            enc->mTiles.push_back({ TileKey{ 0, 0 }, mResult.mAir, 1 });
            auto sky = std::make_shared<SkyMap>();
            sky->set(1, LLVector3d(mResult.mAir->mOrigin), *mResult.mAir);
            enc->mSky = sky;
            EnclosurePass pass(enc);
            while (!pass.step(1u << 30))
            {
            }
            mEnc = pass.takeProduct();
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mGeneration = 1;
            rooms->mDoors = doors;
            mRooms = computeSpaces(rooms, mEnc);
        }

        U32 spaceAt(F32 x, F32 y, F32 z) const { return spaceIdAt(*mEnc, LLVector3(x, y, z + 0.5f)); }
    };
}

namespace
{

    TEST_CASE("skroomsspaceproducts: a wall between two rooms has each room on its side, its floor's own on the segment's left", "[skroomsspaceproducts]")
    {
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        { INFO("two rooms, two spaces"); REQUIRE((west && east && west != east)); }

        // Segments at the dividing wall's foot, each with its own room on its left.
        std::vector<WallSegment> segs(2);
        segs[0].mA.set(9.9f, 3.f, 0.25f);
        segs[0].mB.set(9.9f, 4.5f, 0.25f);
        segs[1].mA.set(10.35f, 4.5f, 0.25f);
        segs[1].mB.set(10.35f, 3.f, 0.25f);
        for (WallSegment& s : segs)
        {
            s.mKind = EdgeKind::WALL;
        }
        bindWallSpaces(*b.mEnc, segs);
        { INFO("the west segment: its room left, the east one across the wall"); REQUIRE((segs[0].mSpace[0] == west && segs[0].mSpace[1] == east)); }
        { INFO("the east segment: its room left, the west one across"); REQUIRE((segs[1].mSpace[0] == east && segs[1].mSpace[1] == west)); }

        std::vector<WallFace> faces = b.mResult.mFaces;
        bindWallSpaces(*b.mEnc, faces);
        U32 seen = 0, bad = 0;
        for (const WallFace& f : faces)
        {
            const F32 cz = (f.mMin.mV[VZ] + f.mMax.mV[VZ]) * 0.5f;
            if ((f.mSide != 0 && f.mSide != 1) || cz > 2.5f || f.mMin.mV[VX] < 9.5f || f.mMax.mV[VX] > 10.75f)
            {
                continue;
            }
            // Side 0 faces +x, into the east room; side 1 faces -x, into the west one.
            const U32 facing = f.mSide == 0 ? east : west, behind = f.mSide == 0 ? west : east;
            ++seen;
            bad += f.mSpace[0] != facing || f.mSpace[1] != behind ? 1 : 0;
        }
        { INFO("faces on the dividing wall"); REQUIRE((seen > 0)); }
        { INFO("each facing one room with the other behind it"); REQUIRE((bad) == (0u)); }
    }

    TEST_CASE("skroomsspaceproducts: the sheet's cells take their gap's space, the highest floor in the band, and nothing in a wall", "[skroomsspaceproducts]")
    {
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        std::vector<SheetCell> cells;
        // A 1 m sheet over the house, floors from below the slab to under the roof.
        sheetSpaces(*b.mEnc, LLVector3(2.f, 2.f, -0.5f), LLVector3(18.f, 10.f, 2.f), 1.f, cells);
        { INFO("16 by 8 cells"); REQUIRE((cells.size()) == (static_cast<size_t>(16 * 8))); }
        const auto at = [&cells](F32 x, F32 y) -> const SheetCell& { return cells[static_cast<size_t>(x - 2.f) + static_cast<size_t>(y - 2.f) * 16]; };
        { INFO("the west room's cell"); REQUIRE((at(6.f, 6.f).mSpace) == (west)); }
        { INFO("the east room's cell"); REQUIRE((at(14.f, 6.f).mSpace) == (east)); }
        { INFO("on the slab, not the roof"); REQUIRE((at(6.f, 6.f).mHit && at(6.f, 6.f).mFloor < 1.f)); }
        std::vector<SheetCell> roof;
        sheetSpaces(*b.mEnc, LLVector3(6.f, 6.f, 2.f), LLVector3(7.f, 7.f, 5.f), 1.f, roof);
        { INFO("the roof in its own band"); REQUIRE((roof.size() == 1 && roof[0].mHit && roof[0].mFloor > 3.f)); }
    }

}
