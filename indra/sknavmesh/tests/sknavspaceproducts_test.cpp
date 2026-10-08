/**
 * @file sknavspaceproducts_test.cpp
 * @brief Walls with a space on each side.
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

#include "../sknavbuild.h"
#include "../sknavenclosure.h"
#include "../sknavlayout.h"
#include "../sknavskymap.h"
#include "../sknavspaceproducts.h"
#include "../sknavspaces.h"
#include "../sknavstamp.h"

#include "llformat.h"

#include "DetourNavMesh.h"

#include <iostream>
#include <memory>

namespace
{
    using namespace sknav;

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

    TileBuildInput tileInput()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;           // coarser than the viewer, to keep the tests quick
        in.mAir = true;
        in.mWallFaces = true;
        in.mEdges[0] = true;
        in.mTerrain.mOrigin.set(-6.f, -6.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 45;
        in.mTerrain.mZ.assign(45 * 45, 0.f);
        in.mTerrain.mWater = -10.f;
        return in;
    }

    /// Walled rooms either side of x = 10, joined by a doorway at y 5 to 7 with its leaf shut in it.
    TileBuildInput twoRoomsDoor(std::vector<DoorEntity>& doors)
    {
        TileBuildInput in = tileInput();
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

    /// A tile rasterised, its spaces cut, stamped, polygonised and bound, its Detour tile in a mesh.
    struct Built
    {
        TileBuildResult mResult;
        TileRasterPtr mRaster;
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;
        TileStampInput mInput;
        TileStamp mStamp;
        TileBuildResult mPolys;
        TileBinding mBinding;
        dtNavMesh* mNav = nullptr;

        Built(const TileBuildInput& in, const std::vector<DoorEntity>& doors)
        {
            mRaster = rasteriseTile(in, mResult);
            tut::ensure("rasterised: " + mResult.mError, mRaster && mResult.mAir);
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
            const TileEnclosure* te = mEnc->tile(TileKey{ 0, 0 });
            stampInput(*mResult.mAir, *te, *mRooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), mInput);
            stampTile(*mRaster->mChf, borderCells(in.mParams[0].mRadius, in.mCell), mRaster->mSpanGap, mInput, mRaster->mReal, mRaster->mFlights,
                      StampParams(), mStamp);
            mPolys = mResult;
            polygoniseTile(in, *mRaster, mPolys, nullptr, &mInput, &mStamp);
            tut::ensure("polygonised: " + mPolys.mError, mPolys.mStatus == TileBuildResult::SUCCESS && !mPolys.mData[0].empty());
            bindStamp(mPolys.mStamp, mInput, te, *mRooms, mBinding);
            mNav = allocMeshNav();
            tut::ensure("tile in", dtStatusSucceed(mNav->addTile(mPolys.mData[0].data(), static_cast<int>(mPolys.mData[0].size()), 0, 0, nullptr)));
        }
        ~Built() { dtFreeNavMesh(mNav); }

        U32 spaceAt(F32 x, F32 y, F32 z) const { return spaceIdAt(*mEnc, LLVector3(x, y, z + 0.5f)); }

    };
}

namespace tut
{
    struct sknavspaceproducts_data
    {
    };
    typedef test_group<sknavspaceproducts_data> sknavspaceproducts_group;
    typedef sknavspaceproducts_group::object sknavspaceproducts_object;
    tut::sknavspaceproducts_group sknavspaceproducts_tests("sknavspaceproducts");

    template<> template<>
    void sknavspaceproducts_object::test<1>()
    {
        set_test_name("a wall between two rooms has each room on its side, its floor's own on the segment's floor side");
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        ensure("two rooms, two spaces", west && east && west != east);

        std::vector<WallSegment> segs = b.mPolys.mSegments[0];
        bindWallSpaces(*b.mEnc, segs);
        U32 west_side = 0, east_side = 0, wrong = 0;
        for (const WallSegment& s : segs)
        {
            const F32 xa = s.mA.mV[VX], xb = s.mB.mV[VX], ya = std::min(s.mA.mV[VY], s.mB.mV[VY]), yb = std::max(s.mA.mV[VY], s.mB.mV[VY]);
            // Runs along the dividing wall, clear of the doorway and the corners.
            const F32 my = (ya + yb) * 0.5f;
            if (s.mKind != EdgeKind::WALL || std::fabs(xa - xb) > 0.05f || my < 2.5f || (my > 4.5f && my < 7.5f) || my > 9.f)
            {
                continue;
            }
            const bool on_west = xa < 10.f && xa > 9.f, on_east = xa > 10.25f && xa < 11.25f;
            const U32 own = on_west ? west : east, other = on_west ? east : west;
            if (!on_west && !on_east)
            {
                continue;
            }
            // The floor side is the left of A to B.
            const LLVector3 dir = s.mB - s.mA;
            const bool floor_left = (on_west && dir.mV[VY] > 0.f) || (on_east && dir.mV[VY] < 0.f);
            const U32 floor_slot = s.mSpace[floor_left ? 0 : 1], beyond_slot = s.mSpace[floor_left ? 1 : 0];
            wrong += floor_slot != own || beyond_slot != other ? 1 : 0;
            west_side += on_west ? 1 : 0;
            east_side += on_east ? 1 : 0;
        }
        ensure("segments along the wall from both rooms", west_side > 0 && east_side > 0);
        ensure_equals("each with its own room on its floor side and the other across the wall", wrong, 0u);

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
        ensure("faces on the dividing wall", seen > 0);
        ensure_equals("each facing one room with the other behind it", bad, 0u);
    }

}
