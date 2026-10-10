/**
 * @file sknavwallruns_test.cpp
 * @brief Wall runs over built tiles: what stands beside a floor, measured from that floor.
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
#include "../sknavwallruns.h"

#include "DetourNavMesh.h"

#include <cmath>
#include <iostream>
#include <memory>

namespace
{
    using namespace sknav;

    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, 1, 0);
        r->mAxes[2].set(0, 0, 1);
        r->mSolid = true;
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
    }

    /// A slab tilted about x: centre, half length along x, half width up the slope, half thickness, slope in degrees.
    RecordPtr tilted(const LLVector3& c, F32 hx, F32 hy, F32 hz, F32 deg)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter = c;
        r->mHalf.set(hx, hy, hz);
        const F32 a = deg * DEG_TO_RAD;
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, std::cos(a), std::sin(a));
        r->mAxes[2].set(0, -std::sin(a), std::cos(a));
        r->mSolid = true;
        const F32 ey = std::fabs(hy * std::cos(a)) + std::fabs(hz * std::sin(a)), ez = std::fabs(hy * std::sin(a)) + std::fabs(hz * std::cos(a));
        r->mMin.set(c.mV[VX] - hx, c.mV[VY] - ey, c.mV[VZ] - ez);
        r->mMax.set(c.mV[VX] + hx, c.mV[VY] + ey, c.mV[VZ] + ez);
        return r;
    }

    TileBuildInput tileInput()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 9.f;
        in.mCell = 0.25f;           // coarser than the viewer, to keep the tests quick
        in.mAir = true;
        in.mEdges[0] = true;
        in.mTerrain.mOrigin.set(-6.f, -6.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 45;
        in.mTerrain.mZ.assign(45 * 45, 0.f);
        in.mTerrain.mWater = -10.f;
        return in;
    }

    /// A tile built as the viewer publishes it: spaces cut, polys stamped and bound, and its wall runs.
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
        std::vector<U32> mPolySpace;
        std::vector<WallRun> mRuns;

        explicit Built(const TileBuildInput& in)
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
            mRooms = computeSpaces(rooms, mEnc);
            const TileEnclosure* te = mEnc->tile(TileKey{ 0, 0 });
            stampInput(*mResult.mAir, *te, *mRooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 9.f), mInput);
            stampTile(*mRaster->mChf, borderCells(in.mParams[0].mRadius, in.mCell), mRaster->mSpanGap, mInput, mRaster->mReal, mRaster->mFlights,
                      StampParams(), mStamp);
            mPolys = mResult;
            polygoniseTile(in, *mRaster, mPolys, nullptr, &mInput, &mStamp);
            tut::ensure("polygonised: " + mPolys.mError, mPolys.mStatus == TileBuildResult::SUCCESS && !mPolys.mData[0].empty());
            bindStamp(mPolys.mStamp, mInput, te, *mRooms, mBinding);
            mNav = allocMeshNav();
            tut::ensure("tile in", dtStatusSucceed(mNav->addTile(mPolys.mData[0].data(), static_cast<int>(mPolys.mData[0].size()), 0, 0, nullptr)));
            for (const U16 c : mPolys.mStamp.mPolyComponent)
            {
                mPolySpace.push_back(c < mBinding.mComponentSpaceId.size() ? mBinding.mComponentSpaceId[c] : 0u);
            }
            const EnclosureProduct& ep = *mEnc;
            const WallAirReader reader({ { mResult.mAir, te } }, [&ep](U16 s) { return s <= ep.mSpaceIds.size() ? ep.mSpaceIds[s - 1] : 0u; });
            const dtMeshTile* dt = static_cast<const dtNavMesh*>(mNav)->getTileAt(0, 0, 0);
            WallRunParams wp;
            wp.mRadius = in.mParams[0].mRadius;
            wp.mClimb = in.mParams[0].mClimb;
            wp.mHeight = in.mParams[0].mHeight;
            buildWallRuns(*dt, mPolySpace, reader, wp, mRuns);
        }
        ~Built() { dtFreeNavMesh(mNav); }

        U32 spaceAt(F32 x, F32 y, F32 z) const { return spaceIdAt(*mEnc, LLVector3(x, y, z)); }

        /// Runs on a floor near z whose every point lies in the plan box, by class: metres.
        F32 metres(WallClass c, F32 z, F32 x0, F32 y0, F32 x1, F32 y1) const
        {
            F32 sum = 0.f;
            for (const WallRun& r : mRuns)
            {
                if (r.mClass != c || std::fabs(r.mFloorZ - z) > 0.3f || !within(r, x0, y0, x1, y1))
                {
                    continue;
                }
                for (size_t i = 1; i < r.mLine.size(); ++i)
                {
                    sum += dist_vec(r.mLine[i], r.mLine[i - 1]);
                }
            }
            return sum;
        }

        static bool within(const WallRun& r, F32 x0, F32 y0, F32 x1, F32 y1)
        {
            for (const LLVector3& p : r.mLine)
            {
                if (p.mV[VX] < x0 || p.mV[VX] > x1 || p.mV[VY] < y0 || p.mV[VY] > y1)
                {
                    return false;
                }
            }
            return true;
        }

        void dump(F32 z) const
        {
            for (const WallRun& r : mRuns)
            {
                if (std::fabs(r.mFloorZ - z) > 0.3f)
                {
                    continue;
                }
                std::cout << "  " << wallClassName(r.mClass) << " floor " << r.mFloorZ << " top " << r.mTopMin << ".." << r.mTopMax << " face " << r.mFace
                          << " " << r.mLine.front() << " -> " << r.mLine.back() << " (" << r.mLine.size() << " pts) space " << r.mSpace << " across "
                          << r.mAcross << std::endl;
            }
        }
    };
}

namespace tut
{
    struct sknavwallruns_data
    {
    };
    typedef test_group<sknavwallruns_data> sknavwallruns_group;
    typedef sknavwallruns_group::object sknavwallruns_object;
    tut::sknavwallruns_group sknavwallruns_tests("sknavwallruns");

    template<> template<>
    void sknavwallruns_object::test<1>()
    {
        set_test_name("a parapet a metre over a roof is hatched, the 3 m building under it solid from the ground");
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(6.f, 6.f, 0.f, 18.f, 18.f, 3.f));
        in.mRecords.push_back(box(6.f, 6.f, 3.f, 18.f, 6.25f, 4.f));
        in.mRecords.push_back(box(6.f, 17.75f, 3.f, 18.f, 18.f, 4.f));
        in.mRecords.push_back(box(6.f, 6.f, 3.f, 6.25f, 18.f, 4.f));
        in.mRecords.push_back(box(17.75f, 6.f, 3.f, 18.f, 18.f, 4.f));
        Built b(in);
        b.dump(3.f);
        const F32 roof_hatched = b.metres(WallClass::HATCHED, 3.f, 6.f, 6.f, 18.f, 18.f);
        const F32 roof_other = b.metres(WallClass::SOLID, 3.f, 6.f, 6.f, 18.f, 18.f) + b.metres(WallClass::NONE, 3.f, 6.f, 6.f, 18.f, 18.f);
        std::cout << "roof: hatched " << roof_hatched << " m, other " << roof_other << " m" << std::endl;
        ensure("the roof's boundary is hatched", roof_hatched > 30.f);
        ensure("and nothing else", roof_other < 0.1f * roof_hatched);
        for (const WallRun& r : b.mRuns)
        {
            if (r.mClass == WallClass::HATCHED && std::fabs(r.mFloorZ - 3.f) < 0.3f)
            {
                ensure("the parapet's top a metre over the roof", std::fabs(r.mTopMax - 1.f) <= 0.3f);
                ensure("half cover", r.mCover == Cover::HALF);
            }
        }
        const F32 ground_solid = b.metres(WallClass::SOLID, 0.f, 4.f, 4.f, 20.f, 20.f);
        ensure("the ground round the building is walled solid", ground_solid > 30.f);
    }

    template<> template<>
    void sknavwallruns_object::test<2>()
    {
        set_test_name("a 3 m wall is solid, its top 3 m over the floor, its face found, its far side's space read");
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(4.f, 12.f, 0.f, 24.f, 12.5f, 3.f));
        Built b(in);
        b.dump(0.f);
        const U32 north = b.spaceAt(14.f, 15.f, 0.75f);
        ensure("the ground past the wall has a space", north != 0);
        U32 seen = 0;
        for (const WallRun& r : b.mRuns)
        {
            const LLVector3 mid = (r.mLine.front() + r.mLine.back()) * 0.5f;
            if (std::fabs(r.mFloorZ) > 0.3f || mid.mV[VX] < 6.f || mid.mV[VX] > 22.f || std::fabs(mid.mV[VY] - 11.65f) > 0.3f)
            {
                continue;
            }
            ++seen;
            ensure("solid", r.mClass == WallClass::SOLID && r.mLow && r.mHigh);
            ensure("3 m tall", std::fabs(r.mTopMax - 3.f) <= 0.25f);
            ensure("full cover", r.mCover == Cover::FULL);
            ensure("its face at the radius out", r.mFace > 0.15f && r.mFace < 0.6f);
            ensure("the ground past the wall across it", r.mAcross == north);
        }
        ensure("runs along the wall's south side", seen > 0);
    }

    template<> template<>
    void sknavwallruns_object::test<3>()
    {
        set_test_name("a landing over understructure has no wall below it; the understructure walls the ground");
        TileBuildInput in = tileInput();
        // A 2.25 m base standing a metre past the landing on every side, the landing on a post 0.5 m over it.
        in.mRecords.push_back(box(8.f, 8.f, 0.f, 16.f, 16.f, 2.25f));
        in.mRecords.push_back(box(11.5f, 11.5f, 2.25f, 12.5f, 12.5f, 2.5f));
        in.mRecords.push_back(box(9.f, 9.f, 2.5f, 15.f, 15.f, 2.75f));
        Built b(in);
        b.dump(2.75f);
        const F32 walled = b.metres(WallClass::HATCHED, 2.75f, 8.f, 8.f, 16.f, 16.f) + b.metres(WallClass::SOLID, 2.75f, 8.f, 8.f, 16.f, 16.f);
        const F32 open = b.metres(WallClass::NONE, 2.75f, 8.f, 8.f, 16.f, 16.f);
        std::cout << "landing: walled " << walled << " m, none " << open << " m" << std::endl;
        ensure("the landing's edges are measured", open > 15.f);
        ensure_equals("none of them walled by the solid under the landing", walled, 0.f);
        ensure("the base walls the ground", b.metres(WallClass::SOLID, 0.f, 6.f, 6.f, 18.f, 18.f) > 25.f);
    }

    template<> template<>
    void sknavwallruns_object::test<4>()
    {
        set_test_name("a wall stepping from 1 m to 3 m splits its run where it steps");
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(4.f, 12.f, 0.f, 14.f, 12.5f, 1.f));
        in.mRecords.push_back(box(14.f, 12.f, 0.f, 24.f, 12.5f, 3.f));
        Built b(in);
        b.dump(0.f);
        // Runs along the wall's south side: a hatched one and a solid one meet where it steps.
        std::vector<const WallRun*> low, high;
        for (const WallRun& r : b.mRuns)
        {
            const LLVector3 mid = (r.mLine.front() + r.mLine.back()) * 0.5f;
            if (std::fabs(r.mFloorZ) > 0.3f || std::fabs(mid.mV[VY] - 11.6f) > 0.3f || mid.mV[VX] < 4.f || mid.mV[VX] > 24.f)
            {
                continue;
            }
            (r.mClass == WallClass::HATCHED ? low : high).push_back(&r);
            if (r.mClass == WallClass::HATCHED)
            {
                ensure("the low half a metre tall", std::fabs(r.mTopMax - 1.f) <= 0.25f);
            }
            if (r.mClass == WallClass::SOLID)
            {
                ensure("the high half three", std::fabs(r.mTopMin - 3.f) <= 0.25f);
            }
        }
        ensure("hatched runs along the low half", !low.empty());
        ensure("solid runs along the high half", !high.empty());
        F32 step_x = -1.f;
        for (const WallRun* l : low)
        {
            for (const WallRun* h : high)
            {
                for (const LLVector3& p : { l->mLine.front(), l->mLine.back() })
                {
                    if (dist_vec(p, h->mLine.front()) < 0.01f || dist_vec(p, h->mLine.back()) < 0.01f)
                    {
                        step_x = p.mV[VX];
                    }
                }
            }
        }
        ensure("a hatched run and a solid one meet", step_x >= 0.f);
        ensure("where the wall steps", std::fabs(step_x - 14.f) <= 0.5f);
    }

    template<> template<>
    void sknavwallruns_object::test<5>()
    {
        set_test_name("a roof's gap carries no space of the room under it");
        TileBuildInput in = tileInput();
        // A room with a doorway, under a steep gable no one walks.
        const auto wall = [&in](F32 x0, F32 y0, F32 x1, F32 y1) { in.mRecords.push_back(box(x0, y0, 0.f, x1, y1, 3.f)); };
        wall(4.f, 4.f, 20.f, 4.25f);
        wall(4.f, 9.75f, 20.f, 10.f);
        wall(4.f, 4.f, 4.25f, 10.f);
        wall(19.75f, 4.f, 20.f, 6.f);
        wall(19.75f, 8.f, 20.f, 10.f);
        in.mRecords.push_back(box(19.75f, 6.f, 2.5f, 20.f, 8.f, 3.f));
        const F32 slope = 55.f, half = 3.f / std::cos(slope * DEG_TO_RAD) * 0.5f, rise = 3.f * std::tan(slope * DEG_TO_RAD);
        in.mRecords.push_back(tilted(LLVector3(12.f, 5.5f, 3.f + rise * 0.5f), 8.f, half, 0.15f, slope));
        in.mRecords.push_back(tilted(LLVector3(12.f, 8.5f, 3.f + rise * 0.5f), 8.f, half, 0.15f, -slope));
        Built b(in);
        for (const F32 y : { 4.75f, 7.f })
        {
            std::cout << "gaps at y " << y << ":";
            b.mEnc->gapsAt(12.f, y, [](const Enclosure& e) { std::cout << " [" << e.mFloor << ", " << e.mCeiling << ") space " << e.mSpaceId; });
            std::cout << std::endl;
        }
        const U32 room = b.spaceAt(12.f, 7.f, 0.5f);
        ensure("the room has a space", room != 0);
        // Under and over the roof's south slope, a quarter of the way up.
        const U32 under = b.spaceAt(12.f, 4.75f, 2.f);
        const U32 roof = b.spaceAt(12.f, 4.75f, 6.f);
        std::cout << "room " << room << " under the roof " << under << " over it " << roof << std::endl;
        ensure("the air under the roof is the room's", under == room);
        ensure("the air over it is not", roof != room);
    }

    template<> template<>
    void sknavwallruns_object::test<6>()
    {
        set_test_name("a 0.3 m wall has its far face 0.3 m behind its near one; a deep block is thick");
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(4.f, 12.1f, 0.f, 24.f, 12.4f, 3.f));
        in.mRecords.push_back(box(4.f, 20.f, 0.f, 24.f, 26.f, 3.f));
        Built b(in);
        U32 thin = 0, deep = 0;
        for (const WallRun& r : b.mRuns)
        {
            const LLVector3 mid = (r.mLine.front() + r.mLine.back()) * 0.5f;
            if (std::fabs(r.mFloorZ) > 0.3f || r.mClass != WallClass::SOLID || mid.mV[VX] < 6.f || mid.mV[VX] > 22.f)
            {
                continue;
            }
            if (std::fabs(mid.mV[VY] - 11.75f) < 0.3f)
            {
                ++thin;
                std::cout << "thin: face " << r.mFace << " far " << r.mFar << " depth " << r.mDepth << std::endl;
                ensure("a far face", r.mFar > r.mFace && !r.mThick);
                ensure("0.3 m behind the near one", std::fabs(r.mFar - r.mFace - 0.3f) < 0.07f && std::fabs(r.mDepth - 0.3f) < 0.07f);
            }
            else if (std::fabs(mid.mV[VY] - 19.65f) < 0.3f)
            {
                ++deep;
                std::cout << "deep: face " << r.mFace << " far " << r.mFar << " thick " << r.mThick << std::endl;
                ensure("thick, with no far face", r.mThick && r.mFar < 0.f);
            }
        }
        ensure("runs along both", thin > 0 && deep > 0);
    }
}
