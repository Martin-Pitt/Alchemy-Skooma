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

    /// Open ground, a 3 m deck east of x = 16, and a 60 degree face 6 m wide climbing +x onto it, unstepped.
    TileBuildInput deckAndClimb()
    {
        TileBuildInput in = tileInput();
        in.mRecords.push_back(box(16.f, 4.f, 0.f, 26.f, 14.f, 3.f));
        const F32 c = 0.5f, s = 0.8660254f, half_run = 1.7320508f, half_depth = 0.5f;
        auto r = std::make_shared<Record>(*box(0.f, 0.f, 0.f, 1.f, 1.f, 1.f));
        r->mAxes[0].set(c, 0.f, s);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(-s, 0.f, c);
        r->mHalf.set(half_run, 3.f, half_depth);
        // The top face runs from the ground at x = 16 - 2 half_run c to the deck's edge at x = 16, z = 3.
        r->mCenter = LLVector3(16.f - half_run * c, 9.f, 3.f - half_run * s) - r->mAxes[2] * half_depth;
        r->mMin.set(r->mCenter.mV[VX] - 1.3f, 6.f, r->mCenter.mV[VZ] - 1.8f);
        r->mMax.set(r->mCenter.mV[VX] + 1.3f, 12.f, r->mCenter.mV[VZ] + 1.8f);
        in.mRecords.push_back(r);
        in.mParams[0].mSlope = 74.f;    // AVATAR's, so the face is walkable
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
        ensure("a stair, not a climb", c.mKind == ConnectorKind::STAIR && by_flight.mClimbLinks.empty());
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

        // A floor under the least storey area that a space holds is still a storey, and every spaced poly has one.
        StoreyProduct small;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, small, nullptr, 1e6f);
        ensure("the rooms keep their storey", small.storeyOf(west) >= 0 && small.storeyOf(east) >= 0);
        const dtNavMesh& nav = *b.mNav;
        U32 spaced = 0, homeless = 0;
        for (S32 t = 0; t < nav.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
            {
                if (b.polySpace()(tile, static_cast<U32>(p)))
                {
                    ++spaced;
                    homeless += small.storeyOfPoly(static_cast<U64>(nav.getPolyRefBase(tile) | static_cast<dtPolyRef>(p))) < 0 ? 1 : 0;
                }
            }
        }
        ensure("spaced polys", spaced > 0);
        ensure_equals("every spaced poly has a storey", homeless, 0u);

        // A scrap in no space that no flight reaches makes none.
        StoreyProduct none;
        buildStoreys(*b.mNav, [](const dtMeshTile*, U32) { return 0u; }, *b.mRooms, none, nullptr, 1e6f);
        ensure("no storey", none.mStoreys.empty() && none.mPolyStorey.empty());
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

    template<> template<>
    void sknavspaceproducts_object::test<6>()
    {
        set_test_name("a climb joins the ground's storey to the deck's at its own ends, kind CLIMB, and links their spaces when it splits them");
        Built b(deckAndClimb(), {});
        const std::vector<Flight>& flights = b.mPolys.mFlights[0];
        const Flight* climb = nullptr;
        for (const Flight& f : flights)
        {
            std::cout << "  flight kind " << static_cast<S32>(f.mKind) << " " << f.mBottom << " -> " << f.mTop << " polys " << f.mPolys.size() << std::endl;
            climb = f.mKind == ConnectorKind::CLIMB ? &f : climb;
        }
        ensure("the face is a climb", climb != nullptr);
        const PolyFlightFn flight_of = [&flights](const dtMeshTile*, U32 p) -> const Flight* {
            for (const Flight& f : flights)
            {
                if (std::find(f.mPolys.begin(), f.mPolys.end(), static_cast<U16>(p)) != f.mPolys.end())
                {
                    return &f;
                }
            }
            return nullptr;
        };
        StoreyProduct st;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, st, flight_of);
        const S32 sg = b.storeyAt(st, 5.f, 20.f, 0.f), sd = b.storeyAt(st, 21.f, 9.f, 3.f);
        ensure("ground and deck are two storeys", sg >= 0 && sd >= 0 && sg != sd);
        U32 joined = 0;
        for (const StoreyConnector& c : st.mConnectors)
        {
            std::cout << "  connector " << c.mStorey[0] << "-" << c.mStorey[1] << " kind " << static_cast<S32>(c.mKind) << std::endl;
            if (static_cast<S32>(c.mStorey[0]) == sg && static_cast<S32>(c.mStorey[1]) == sd && c.mFlight == climb->mKey)
            {
                ++joined;
                ensure("kind CLIMB", c.mKind == ConnectorKind::CLIMB);
                ensure("at the climb's own ends", dist_vec(c.mEnd[0], climb->mBottom) < 1e-4f && dist_vec(c.mEnd[1], climb->mTop) < 1e-4f);
            }
        }
        ensure_equals("one connector for the climb", joined, 1u);

        const U32 ground = b.spaceAt(13.f, 9.f, 0.f), deck = b.spaceAt(18.f, 9.f, 3.f);
        std::cout << "  spaces ground " << ground << " deck " << deck << ", " << st.mClimbLinks.size() << " climb links" << std::endl;
        ensure("both ends in spaces", ground && deck);
        if (ground != deck)
        {
            ensure_equals("one climb link", st.mClimbLinks.size(), static_cast<size_t>(1));
            const ClimbLink& l = st.mClimbLinks[0];
            ensure("foot space first, then the top's", l.mSpace[0] == ground && l.mSpace[1] == deck);
            ensure("the climb's", l.mFlight == climb->mKey);
            ensure("found by either space", st.climbLinksOf(ground).size() == 1 && st.climbLinksOf(deck).size() == 1);
        }
        else
        {
            ensure("one space needs no link", st.mClimbLinks.empty());
        }

        // Its polys shared with a flight that lands nowhere: the climb's own ends still join the storeys.
        Flight other;
        other.mKey = climb->mKey + 1;
        other.mBottom = other.mTop = LLVector3(5.f, 20.f, 0.f);
        const PolyFlightFn shared = [&](const dtMeshTile* t, U32 p) -> const Flight* {
            const Flight* f = flight_of(t, p);
            return f == climb && p != climb->mPolys[0] ? &other : f;
        };
        StoreyProduct mixed;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, mixed, shared);
        U32 by_ends = 0;
        for (const StoreyConnector& c : mixed.mConnectors)
        {
            by_ends += c.mFlight == climb->mKey && c.mKind == ConnectorKind::CLIMB && static_cast<S32>(c.mStorey[0]) == sg && static_cast<S32>(c.mStorey[1]) == sd ? 1 : 0;
        }
        ensure_equals("joined by the climb's own ends", by_ends, 1u);

        // The deck a space of its own: the climb links the two, foot first.
        const PolySpaceFn base_space = b.polySpace();
        const dtMeshTile* dt = static_cast<const dtNavMesh*>(b.mNav)->getTileAt(0, 0, 0);
        const U32 deck_id = ground + 1;
        const PolySpaceFn split = [&](const dtMeshTile* t, U32 p) -> U32 {
            const dtPoly& poly = dt->polys[p];
            return fromRecast(&dt->verts[poly.verts[0] * 3]).mV[VZ] > 2.5f && fromRecast(&dt->verts[poly.verts[1] * 3]).mV[VZ] > 2.5f ? deck_id : base_space(t, p);
        };
        StoreyProduct apart;
        buildStoreys(*b.mNav, split, *b.mRooms, apart, flight_of);
        ensure_equals("one climb link", apart.mClimbLinks.size(), static_cast<size_t>(1));
        const ClimbLink& l = apart.mClimbLinks[0];
        ensure("foot space first, then the top's", l.mSpace[0] == ground && l.mSpace[1] == deck_id);
        ensure("the climb's, at its ends", l.mFlight == climb->mKey && dist_vec(l.mFoot, climb->mBottom) < 1e-4f && dist_vec(l.mTop, climb->mTop) < 1e-4f);
        ensure("found by either space", apart.climbLinksOf(ground).size() == 1 && apart.climbLinksOf(deck_id).size() == 1 && apart.climbLinksOf(7u).empty());
        // A climb makes no portal, so no seam either where its polys meet the deck's.
        for (const SeamPortal& sp : apart.mSeamPortals)
        {
            ensure("no seam across the climb", !(sp.mSpace[0] == std::min(ground, deck_id) && sp.mSpace[1] == std::max(ground, deck_id)));
        }
    }

    template<> template<>
    void sknavspaceproducts_object::test<7>()
    {
        set_test_name("a flight's ends meet the ground's polys at its foot and the deck's at its head, none of its own");
        Built b(deckAndStair(), {});
        const std::vector<Flight>& flights = b.mPolys.mFlights[0];
        ensure_equals("one flight", flights.size(), static_cast<size_t>(1));
        const Flight& f = flights[0];
        const PolyFlightFn flight_of = [&f](const dtMeshTile*, U32 p) -> const Flight* {
            return std::find(f.mPolys.begin(), f.mPolys.end(), static_cast<U16>(p)) != f.mPolys.end() ? &f : nullptr;
        };
        StoreyProduct st;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, st, flight_of, 10.f, { &f });
        const FlightEnds* fe = st.flightEnds(f.mKey);
        ensure("its ends", fe != nullptr);
        const dtNavMesh& nav = *b.mNav;
        for (S32 e = 0; e < 2; ++e)
        {
            ensure(llformat("end %d meets polys", e), !fe->mPolys[e].empty() && fe->mGap[e] == EndGap::NONE);
            for (const U64 ref : fe->mPolys[e])
            {
                const dtMeshTile* t = nullptr;
                const dtPoly* p = nullptr;
                ensure("a poly", dtStatusSucceed(nav.getTileAndPolyByRef(static_cast<dtPolyRef>(ref), &t, &p)));
                const U32 ip = static_cast<U32>(p - t->polys);
                ensure("not the flight's own", !flight_of(t, ip));
                F32 lo = 1e30f, hi = -1e30f;
                for (S32 v = 0; v < p->vertCount; ++v)
                {
                    lo = std::min(lo, fromRecast(&t->verts[p->verts[v] * 3]).mV[VZ]);
                    hi = std::max(hi, fromRecast(&t->verts[p->verts[v] * 3]).mV[VZ]);
                }
                const F32 z = e == 0 ? f.mBottom.mV[VZ] : f.mTop.mV[VZ];
                ensure(llformat("end %d at its height: %.2f..%.2f for %.2f", e, lo, hi, z), lo <= z + FLIGHT_END_DZ_M && hi >= z - FLIGHT_END_DZ_M);
            }
        }
        const S32 sg = b.storeyAt(st, 5.f, 20.f, 0.f), sd = b.storeyAt(st, 21.f, 9.f, 3.f);
        ensure("their storeys", fe->mStorey[0] == sg && fe->mStorey[1] == sd);

        // An end edge drawn up the run, short of its floor, still meets it across the links of the flight's own polys.
        Flight shy = f;
        LLVector3 up = f.mTop - f.mBottom;
        up.mV[VZ] = 0.f;
        up.normVec();
        shy.mBottomA += up * 0.6f;
        shy.mBottomB += up * 0.6f;
        std::vector<FlightEnds> shy_ends;
        flightEnds(nav, { &shy }, [&f, &shy](const dtMeshTile* t, U32 p) -> const Flight* {
            return std::find(f.mPolys.begin(), f.mPolys.end(), static_cast<U16>(p)) != f.mPolys.end() ? &shy : nullptr;
        }, shy_ends);
        ensure("linked at its foot", shy_ends.size() == 1 && !shy_ends[0].mPolys[0].empty());

        // Without polys of its own its ends still meet the floors; with no floor there either, an end says it was eroded.
        Flight bare = f;
        bare.mKey ^= 1;
        bare.mPolys.clear();
        std::vector<FlightEnds> ends;
        flightEnds(nav, { &bare }, nullptr, ends);
        ensure("bare, still meeting the floors", ends.size() == 1 && !ends[0].mPolys[0].empty() && !ends[0].mPolys[1].empty());
        for (LLVector3* v : { &bare.mBottom, &bare.mTop, &bare.mBottomA, &bare.mBottomB, &bare.mTopA, &bare.mTopB })
        {
            v->mV[VZ] += 20.f;
        }
        flightEnds(nav, { &bare }, nullptr, ends);
        ensure("in the air, eroded", ends.size() == 1 && ends[0].mGap[0] == EndGap::ERODED && ends[0].mGap[1] == EndGap::ERODED);
    }

    template<> template<>
    void sknavspaceproducts_object::test<8>()
    {
        set_test_name("two spaces whose polys share an edge with no portal between them get a seam portal there, none where a portal stands");
        std::vector<DoorEntity> doors;
        Built b(twoRoomsDoor(doors), doors);
        const U32 west = b.spaceAt(6.f, 6.f, 0.25f), east = b.spaceAt(14.f, 6.f, 0.25f);
        ensure("two rooms", west && east && west != east);
        StoreyProduct with;
        buildStoreys(*b.mNav, b.polySpace(), *b.mRooms, with);
        ensure("the door joins them already: no seam", with.mSeamPortals.empty());
        // The same polys with the door's portal gone.
        RoomsProduct bare(*b.mRooms);
        bare.mPortals.clear();
        StoreyProduct seam;
        buildStoreys(*b.mNav, b.polySpace(), bare, seam);
        ensure_equals("one seam portal", seam.mSeamPortals.size(), size_t(1));
        const SeamPortal& sp = seam.mSeamPortals.front();
        ensure("between the rooms", sp.mSpace[0] == std::min(west, east) && sp.mSpace[1] == std::max(west, east));
        ensure("in the doorway", std::fabs(sp.mCentre.mV[VX] - 10.f) < 0.75f && sp.mLength > 0.3f);
        ensure("no flight, so no stair", sp.mKind != SeamPortal::STAIR);
    }
}
