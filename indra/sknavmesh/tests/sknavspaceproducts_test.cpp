/**
 * @file sknavspaceproducts_test.cpp
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

#include "../test/lltut.h"

#include "../sknavbuild.h"
#include "../sknavenclosure.h"
#include "../sknavlayout.h"
#include "../sknavprobes.h"
#include "../sknavskymap.h"
#include "../sknavspaceproducts.h"
#include "../sknavspaces.h"
#include "../sknavstamp.h"

#include "llformat.h"

#include "DetourNavMesh.h"

#include <algorithm>
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

    /// Open ground, a 3 m deck east of x = 16, and a flight of twelve 0.25 m steps up +x onto it.
    TileBuildInput deckAndStair()
    {
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(16.f, 4.f, 0.f, 26.f, 14.f, 3.f));
        for (S32 k = 0; k < 12; ++k)
        {
            in.mRecords.push_back(box(11.2f + k * 0.4f, 7.f, 0.f, 11.6f + k * 0.4f, 10.f, (k + 1) * 0.25f));
        }
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

        /// The storey of the poly under a point: inside it in plan, its height the nearest.
        S32 storeyAt(const StoreyProduct& st, F32 x, F32 y, F32 z) const
        {
            const dtMeshTile* tile = static_cast<const dtNavMesh*>(mNav)->getTileAt(0, 0, 0);
            S32 best = -1;
            F32 best_dz = 1.f;
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                bool pos = false, neg = false;
                for (S32 v = 0; v < poly.vertCount; ++v)
                {
                    const LLVector3 a = fromRecast(&tile->verts[poly.verts[v] * 3]);
                    const LLVector3 c = fromRecast(&tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3]);
                    const F32 cross = (c.mV[VX] - a.mV[VX]) * (y - a.mV[VY]) - (c.mV[VY] - a.mV[VY]) * (x - a.mV[VX]);
                    pos = pos || cross > 0.f;
                    neg = neg || cross < 0.f;
                }
                const bool inside = !(pos && neg);
                if (!inside)
                {
                    continue;
                }
                const F32 dz = std::fabs(fromRecast(&tile->verts[poly.verts[0] * 3]).mV[VZ] - z);
                if (dz < best_dz)
                {
                    best_dz = dz;
                    best = st.storeyOfPoly(static_cast<U64>(mNav->getPolyRefBase(tile) | static_cast<dtPolyRef>(p)));
                }
            }
            return best;
        }

        PolySpaceFn polySpace() const
        {
            return [this](const dtMeshTile*, U32 p) -> U32 {
                const std::vector<U16>& comp = mPolys.mStamp.mPolyComponent;
                return p < comp.size() && comp[p] < mBinding.mComponentSpaceId.size() ? mBinding.mComponentSpaceId[comp[p]] : 0u;
            };
        }
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

    template<> template<>
    void sknavspaceproducts_object::test<2>()
    {
        set_test_name("a deck reached by a flight is a storey of its own, joined to the ground's by one connector at the flight's ends");
        Built b(deckAndStair(), {});
        const dtMeshTile* dt = static_cast<const dtNavMesh*>(b.mNav)->getTileAt(0, 0, 0);
        ensure("drop links off the deck, whose Detour links carry no edge", dt && dt->header->offMeshConCount > 0);
        StoreyProduct st;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, st);
        std::cout << st.mStoreys.size() << " storeys, " << st.mConnectors.size() << " connectors, " << st.mFlightPolys << " of " << st.mPolys
                  << " polys on flights" << std::endl;
        for (const Storey& s : st.mStoreys)
        {
            std::cout << "  storey " << s.mId << " z " << s.mFloorZ << " area " << s.mArea << " spaces " << s.mSpaces.size() << std::endl;
        }
        const S32 sg = b.storeyAt(st, 5.f, 20.f, 0.f), sd = b.storeyAt(st, 21.f, 9.f, 3.f);
        ensure("ground and deck have storeys", sg >= 0 && sd >= 0);
        ensure("the deck is not the ground's storey", sg != sd);
        ensure("the ground's storey is the lower", st.mStoreys[sg].mFloorZ < 0.5f && st.mStoreys[sd].mFloorZ > 2.5f);
        ensure("flight polys found", st.mFlightPolys > 0);
        bool joined = false;
        for (const StoreyConnector& c : st.mConnectors)
        {
            if (static_cast<S32>(c.mStorey[0]) != sg || static_cast<S32>(c.mStorey[1]) != sd)
            {
                continue;
            }
            joined = true;
            std::cout << "  connector " << c.mEnd[0] << " -> " << c.mEnd[1] << (c.mPortals ? " at portals" : " at poly edges") << std::endl;
            ensure("its foot at the flight's foot", std::fabs(c.mEnd[0].mV[VX] - 11.2f) < 1.5f && c.mEnd[0].mV[VZ] < 0.75f);
            ensure("its head at the deck's edge", std::fabs(c.mEnd[1].mV[VX] - 16.f) < 1.5f && c.mEnd[1].mV[VZ] > 2.25f);
        }
        ensure("a connector from the ground's storey to the deck's", joined);

        // Told which flight each poly is on, the connector runs between the flight's own ends.
        const std::vector<Flight>& flights = b.mPolys.mFlights[0];
        ensure_equals("one flight", flights.size(), static_cast<size_t>(1));
        const PolyFlightFn flight_of = [&flights](const dtMeshTile*, U32 p) -> const Flight* {
            return std::find(flights[0].mPolys.begin(), flights[0].mPolys.end(), static_cast<U16>(p)) != flights[0].mPolys.end() ? &flights[0] : nullptr;
        };
        StoreyProduct by_flight;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, by_flight, flight_of);
        ensure_equals("one connector", by_flight.mConnectors.size(), static_cast<size_t>(1));
        const StoreyConnector& c = by_flight.mConnectors[0];
        ensure("the flight's", c.mFlight == flights[0].mKey && !c.mPortals);
        ensure("at its ends", dist_vec(c.mEnd[0], flights[0].mBottom) < 1e-4f && dist_vec(c.mEnd[1], flights[0].mTop) < 1e-4f);
        ensure("between the ground's storey and the deck's", static_cast<S32>(c.mStorey[0]) == sg && static_cast<S32>(c.mStorey[1]) == sd);

        // A deck under the least storey area stays one: the flight lands on it.
        StoreyProduct small;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, small, flight_of, 100.f);
        ensure_equals("a landing kept however small", small.mStoreys.size(), static_cast<size_t>(2));
        ensure_equals("still joined", small.mConnectors.size(), static_cast<size_t>(1));
    }

    template<> template<>
    void sknavspaceproducts_object::test<3>()
    {
        set_test_name("rooms through a door on one floor are one storey: a door is no climb");
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        StoreyProduct st;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, st);
        const S32 sw = st.storeyOf(west), se = st.storeyOf(east);
        ensure("both rooms have storeys", sw >= 0 && se >= 0);
        ensure_equals("the same storey", sw, se);
        ensure("no flight, no connector", st.mConnectors.empty());

        // A floor under the least storey area that no flight reaches makes none.
        StoreyProduct small;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, small, nullptr, 1e6f);
        ensure("no storey", small.mStoreys.empty() && small.mPolyStorey.empty() && small.storeyOf(west) < 0);
    }

    template<> template<>
    void sknavspaceproducts_object::test<4>()
    {
        set_test_name("the sheet's cells take their gap's space, the highest floor in the band, and nothing in a wall");
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        std::vector<SheetCell> cells;
        // A 1 m sheet over the house, floors from below the slab to under the roof.
        sheetSpaces(*b.mEnc, LLVector3(2.f, 2.f, -0.5f), LLVector3(18.f, 10.f, 2.f), 1.f, cells);
        ensure_equals("16 by 8 cells", cells.size(), static_cast<size_t>(16 * 8));
        const auto at = [&cells](F32 x, F32 y) -> const SheetCell& { return cells[static_cast<size_t>(x - 2.f) + static_cast<size_t>(y - 2.f) * 16]; };
        ensure_equals("the west room's cell", at(6.f, 6.f).mSpace, west);
        ensure_equals("the east room's cell", at(14.f, 6.f).mSpace, east);
        ensure("on the slab, not the roof", at(6.f, 6.f).mHit && at(6.f, 6.f).mFloor < 1.f);
        std::vector<SheetCell> roof;
        sheetSpaces(*b.mEnc, LLVector3(6.f, 6.f, 2.f), LLVector3(7.f, 7.f, 5.f), 1.f, roof);
        ensure("the roof in its own band", roof.size() == 1 && roof[0].mHit && roof[0].mFloor > 3.f);
    }

    template<> template<>
    void sknavspaceproducts_object::test<5>()
    {
        set_test_name("a space's floor from its polys: a square's cells at its height, and the west room's parts inside the room");
        const std::vector<std::vector<LLVector3>> square = { { LLVector3(0.f, 0.f, 2.4f), LLVector3(1.f, 0.f, 2.4f), LLVector3(1.f, 1.f, 2.4f),
                                                               LLVector3(0.f, 1.f, 2.4f) } };
        const auto r = polyRaster(square);
        ensure_equals("four half-metre cells", r.size(), static_cast<size_t>(4));
        U32 total = 0;
        for (const auto& c : r)
        {
            total += c.second;
            ensure_equals("at its metre", static_cast<S64>(c.first & 0x1fffff) - (1ll << 20), 2ll);
        }
        ensure_equals("64 fine cells", total, 64u);

        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f);
        const Space* sp = b.mRooms->byId(west);
        ensure("the west room", sp != nullptr);
        Space from_polys = *sp;
        from_polys.mRaster = polyRaster(spacePolys(*b.mNav, b.polySpace(), west));
        ensure("its polys rasterised", !from_polys.mRaster.empty());
        ProbeParams params;
        const std::vector<ProbePart> parts = spaceProbes(from_polys, params);
        ensure("parts from its polys", !parts.empty());
        for (const ProbePart& p : parts)
        {
            ensure(llformat("part at %.2f,%.2f inside the room", p.mOrigin.mV[VX], p.mOrigin.mV[VY]),
                   p.mOrigin.mV[VX] > 2.25f && p.mOrigin.mV[VX] < 10.f && p.mOrigin.mV[VY] > 2.25f && p.mOrigin.mV[VY] < 9.75f);
        }
    }
}
