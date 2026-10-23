/**
 * @file skroomsgraph_test.cpp
 * @brief Tests for the room graph: spaces joined across doors and openings, with their footprints and hashes.
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

#include "skroomstestscene.h"
#include "../skroomsgraph.h"

#include "llstring.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

namespace
{
    using namespace skworld;
    using namespace skrooms_test;

    struct Result
    {
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;

        /// The index of the space holding a point, or ~0u.
        U32 at(F32 x, F32 y, F32 z = 0.5f) const
        {
            Enclosure e;
            if (!mEnc->enclosureAt(LLVector3(x, y, z), e) || !e.mSpace)
            {
                return ~0u;
            }
            return e.mSpace - 1;
        }

        const Space& space(U32 index) const { return mRooms->mSpaces[index]; }
    };

    Result run(const std::shared_ptr<EnclosureInput>& enc_in, std::shared_ptr<RoomsInput> rooms_in = nullptr)
    {
        EnclosurePass pass(enc_in);
        while (!pass.step(1u << 30))
        {
        }
        Result r;
        r.mEnc = pass.takeProduct();
        if (!rooms_in)
        {
            rooms_in = std::make_shared<RoomsInput>();
        }
        r.mRooms = computeSpaces(rooms_in, r.mEnc);
        return r;
    }

    DoorEntity door(F32 x, F32 y, F32 nx, F32 ny, F32 width, F32 sill = 0.f, F32 height = 2.2f)
    {
        DoorEntity d;
        d.mPlanePoint.setVec(x, y, sill);
        d.mNormal.set(nx, ny, 0.f);
        d.mWidth = width;
        d.mHeight = height;
        d.mSill = sill;
        d.mId.generate();
        const F32 hx = std::fabs(ny) * width * 0.5f + 0.05f, hy = std::fabs(nx) * width * 0.5f + 0.05f;
        d.mMin.setVec(x - hx, y - hy, sill);
        d.mMax.setVec(x + hx, y + hy, sill + height);
        return d;
    }

    /// The plan cells of a set of spaces' rasters, by RASTER_XY_M index.
    std::set<std::pair<S32, S32>> rasterCells(const Result& r, const std::vector<U32>& space_ids)
    {
        std::set<std::pair<S32, S32>> out;
        for (U32 id : space_ids)
        {
            for (const auto& c : r.mRooms->byId(id)->mRaster)
            {
                const PlanCell cell = rasterCell(c.first);
                out.emplace(static_cast<S32>(std::lround(cell.mX / RASTER_XY_M)), static_cast<S32>(std::lround(cell.mY / RASTER_XY_M)));
            }
        }
        return out;
    }

    std::set<std::pair<S32, S32>> maskCells(const Room& room)
    {
        std::set<std::pair<S32, S32>> out;
        for (S32 y = 0; y < room.mCellH; ++y)
        {
            for (S32 x = 0; x < room.mCellW; ++x)
            {
                if (room.mMask[static_cast<size_t>(x) + static_cast<size_t>(y) * room.mCellW])
                {
                    out.emplace(room.mCellX0 + x, room.mCellY0 + y);
                }
            }
        }
        return out;
    }

    bool hasPortal(const Room& room, U32 portal)
    {
        return std::find(room.mPortals.begin(), room.mPortals.end(), portal) != room.mPortals.end();
    }

    /// The portal between two spaces, by index, or ~0u.
    U32 portalBetween(const Result& r, U32 a, U32 b)
    {
        for (U32 i = 0; i < r.mRooms->mPortals.size(); ++i)
        {
            const Portal& p = r.mRooms->mPortals[i];
            if ((p.mSpaces[0] == a && p.mSpaces[1] == b) || (p.mSpaces[0] == b && p.mSpaces[1] == a))
            {
                return i;
            }
        }
        return ~0u;
    }

    /// twoRooms with a wall at x = 10 holding a 1 m gap at y 5.5 to 6.5.
    Scene splitRooms()
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.5f);
        s.wall(10.f, 6.5f, 10.25f, 10.f);
        return s;
    }

    /// test<5> of the TUT rooms tests, roofed: a hall, a door, a landing, a
    /// roofed flight and a walled deck room at 3.25 m it climbs into.
    Scene stairHouse()
    {
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 1.f, 3.f, 3.25f);
        s.wall(9.f, 0.75f, 20.f, 1.f, 6.f);
        s.wall(9.f, 3.f, 20.f, 3.25f, 6.f);
        s.wall(8.75f, 0.75f, 9.f, 1.5f, 6.f);
        s.wall(8.75f, 2.5f, 9.f, 8.f, 6.f);
        s.box(9.f, 0.75f, 20.f, 3.25f, 6.f, 6.25f);
        s.wall(2.f, 0.75f, 9.f, 1.f, 3.f);
        s.wall(2.f, 7.75f, 9.f, 8.f, 3.f);
        s.wall(2.f, 0.75f, 2.25f, 8.f, 3.f);
        s.box(2.f, 0.75f, 9.f, 8.f, 3.f, 3.25f);
        s.box(20.f, 0.f, 28.f, 0.25f, 3.25f, 6.f, false);
        s.box(20.f, 7.75f, 28.f, 8.f, 3.25f, 6.f, false);
        s.box(27.75f, 0.f, 28.f, 8.f, 3.25f, 6.f, false);
        s.box(20.f, 0.f, 20.25f, 1.f, 3.25f, 6.f, false);
        s.box(20.f, 3.f, 20.25f, 8.f, 3.25f, 6.f, false);
        s.box(20.f, 0.f, 28.f, 8.f, 6.f, 6.25f, false);
        return s;
    }

    std::shared_ptr<RoomsInput> stairDoor()
    {
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(8.875f, 2.f, 1.f, 0.f, 1.f));
        return rooms;
    }

    TEST_CASE("skroomsgraph: a room with a doorway to the street is one room whose mask is its space's raster", "[skroomsgraph]")
    {
        const Result r = run(twoRooms().input(1));
        const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
        const U32 inside = r.at(6.f, 6.f), street = r.at(24.f, 24.f);
        REQUIRE(inside != ~0u);
        REQUIRE(street != ~0u);
        const Room* room = g->ofSpace(inside);
        REQUIRE(room != nullptr);
        CHECK(g->mRooms.size() == 1);
        CHECK(room->mSpaces == std::vector<U32>{ r.space(inside).mId });
        CHECK(room->mId == r.space(inside).mId);
        CHECK(g->ofSpace(street) == nullptr);
        const U32 doorway = portalBetween(r, inside, street);
        REQUIRE(doorway != ~0u);
        CHECK(r.mRooms->mPortals[doorway].mKind == PortalKind::OPENING);
        CHECK(hasPortal(*room, doorway));
        CHECK_FALSE(room->mTruncated);
        CHECK(maskCells(*room) == rasterCells(r, room->mSpaces));
        CHECK(room->mFloorMin == r.space(inside).mFloorMin);
        CHECK(room->mFloorMax == r.space(inside).mFloorMax);

        // A cap below its box keeps the box alone, and the box answers for every cell in it.
        RoomGraphParams capped;
        capped.mMaxMaskCells = 16;
        const RoomGraphPtr box_only = buildRoomGraph(*r.mRooms, capped);
        REQUIRE(box_only->mRooms.size() == 1);
        const Room& b = box_only->mRooms[0];
        CHECK(b.mTruncated);
        CHECK(b.mMask.empty());
        CHECK(b.mCellW == room->mCellW);
        CHECK(b.mHash == room->mHash);
        CHECK(b.covers(b.mCellX0, b.mCellY0));
        CHECK_FALSE(b.covers(b.mCellX0 - 1, b.mCellY0));
    }

    TEST_CASE("skroomsgraph: two rooms joined by an opening or a lintelled doorway or a door are one room", "[skroomsgraph]")
    {
        const auto check = [](const Result& r, size_t members, PortalKind kind) {
            const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
            const U32 west = r.at(6.f, 6.f), east = r.at(14.f, 6.f);
            REQUIRE(west != ~0u);
            REQUIRE(east != ~0u);
            const Room* room = g->ofSpace(west);
            REQUIRE(room != nullptr);
            CHECK(g->ofSpace(east) == room);
            CHECK(room->mSpaces.size() == members);
            CHECK(room->mId == llmin(r.space(west).mId, r.space(east).mId));
            CHECK(maskCells(*room) == rasterCells(r, room->mSpaces));
            if (members == 2)
            {
                const U32 cut = portalBetween(r, west, east);
                REQUIRE(cut != ~0u);
                CHECK(r.mRooms->mPortals[cut].mKind == kind);
                CHECK_FALSE(hasPortal(*room, cut));
            }
        };
        {
            INFO("a floor-to-ceiling gap is no cue: the spaces pass leaves one space, and so one room");
            check(run(splitRooms().input(1)), 1, PortalKind::OPENING);
        }
        {
            INFO("a lintel over the gap cuts an opening, which the room crosses");
            Scene s = splitRooms();
            s.box(10.f, 5.5f, 10.25f, 6.5f, 2.2f, 3.f, false);
            const Result r = run(s.input(1));
            check(r, 2, PortalKind::OPENING);
            const U32 cut = portalBetween(r, r.at(6.f, 6.f), r.at(14.f, 6.f));
            REQUIRE(cut != ~0u);
            CHECK(r.mRooms->mPortals[cut].lintel());
        }
        {
            INFO("a door in the gap cuts a door, which the room crosses too");
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mDoors.push_back(door(10.125f, 6.f, 1.f, 0.f, 1.f));
            check(run(splitRooms().input(1), rooms), 2, PortalKind::DOOR);
        }
    }

    TEST_CASE("skroomsgraph: a lintelled doorway onto a step the pass climbs is one room", "[skroomsgraph]")
    {
        Scene s = splitRooms();
        s.box(10.f, 5.5f, 10.25f, 6.5f, 2.2f, 3.f, false);
        s.box(10.f, 2.25f, 17.75f, 9.75f, 0.f, 0.7f);
        const Result r = run(s.input(1));
        const U32 west = r.at(6.f, 6.f), east = r.at(14.f, 6.f, 1.2f);
        REQUIRE(west != ~0u);
        REQUIRE(east != ~0u);
        REQUIRE(west != east);
        CHECK(r.space(east).mFloorMin - r.space(west).mFloorMax > 0.6f);
        const U32 cut = portalBetween(r, west, east);
        REQUIRE(cut != ~0u);
        CHECK(r.mRooms->mPortals[cut].mKind == PortalKind::OPENING);
        const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
        const Room* room = g->ofSpace(west);
        REQUIRE(room != nullptr);
        CHECK(g->ofSpace(east) == room);
        CHECK(room->mSpaces.size() == 2);

        // A slack below the step, as the guard first had, splits the storey.
        RoomGraphParams tight;
        tight.mSillSlackM = 0.6f;
        const RoomGraphPtr apart = buildRoomGraph(*r.mRooms, tight);
        CHECK(apart->ofSpace(west) != apart->ofSpace(east));
    }

    TEST_CASE("skroomsgraph: a stair joins no storey", "[skroomsgraph][.flights]")
    {
        const Result r = run(stairHouse().input(1), stairDoor());
        const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
        const U32 hall = r.at(4.f, 4.f), landing = r.at(10.5f, 2.f), flight = r.at(16.f, 2.f, 2.2f), deck = r.at(24.f, 4.f, 3.75f);
        REQUIRE(hall != ~0u);
        REQUIRE(landing != ~0u);
        REQUIRE(flight != ~0u);
        REQUIRE(deck != ~0u);
        CHECK(r.space(flight).mFlight);
        const Room* below = g->ofSpace(hall);
        REQUIRE(below != nullptr);
        std::vector<U32> both = { r.space(hall).mId, r.space(landing).mId };
        std::sort(both.begin(), both.end());
        CHECK(below->mSpaces == both);
        CHECK(g->ofSpace(landing) == below);
        CHECK(g->ofSpace(flight) == nullptr);
        const Room* above = g->ofSpace(deck);
        REQUIRE(above != nullptr);
        CHECK(above != below);
        CHECK(above->mSpaces == std::vector<U32>{ r.space(deck).mId });
        CHECK(below->mFloorMax < 3.f);
        CHECK(above->mFloorMin > 3.f);

        const U32 door_portal = portalBetween(r, hall, landing), foot = portalBetween(r, landing, flight),
                  head = portalBetween(r, flight, deck);
        REQUIRE(door_portal != ~0u);
        REQUIRE(foot != ~0u);
        REQUIRE(head != ~0u);
        CHECK(r.mRooms->mPortals[door_portal].mKind == PortalKind::DOOR);
        CHECK(r.mRooms->mPortals[foot].mKind == PortalKind::STAIR);
        CHECK_FALSE(hasPortal(*below, door_portal));
        CHECK(hasPortal(*below, foot));
        CHECK(hasPortal(*above, head));
        CHECK_FALSE(hasPortal(*above, foot));
    }

    TEST_CASE("skroomsgraph: a ramp is a room of its own and chains no storeys", "[skroomsgraph]")
    {
        // A hall at 0, a ramp up to 3 and a floor at 3, as the pass would leave them, joined by openings.
        RoomsProduct product;
        const F32 bands[3][2] = { { 0.f, 0.f }, { 0.f, 3.f }, { 3.f, 3.f } };
        for (U32 i = 0; i < 3; ++i)
        {
            Space sp;
            sp.mId = 10 * (i + 1);
            sp.mBucket = EnclosureBucket::INDOORS;
            sp.mBuilding = 10;
            sp.mFloorMin = bands[i][0];
            sp.mFloorMax = bands[i][1];
            product.mSpaces.push_back(sp);
        }
        for (U32 i = 0; i < 2; ++i)
        {
            Portal p;
            p.mKind = PortalKind::OPENING;
            p.mSpaces[0] = i;
            p.mSpaces[1] = i + 1;
            p.mSill = bands[i + 1][i];
            product.mPortals.push_back(p);
            product.mSpaces[i].mPortals.push_back(i);
            product.mSpaces[i + 1].mPortals.push_back(i);
        }

        const RoomGraphPtr g = buildRoomGraph(product);
        REQUIRE(g->mRooms.size() == 3);
        const Room* hall = g->ofSpace(0);
        const Room* ramp = g->ofSpace(1);
        const Room* upper = g->ofSpace(2);
        REQUIRE(hall != nullptr);
        REQUIRE(ramp != nullptr);
        REQUIRE(upper != nullptr);
        CHECK(hall->mSpaces == std::vector<U32>{ 10 });
        CHECK(ramp->mSpaces == std::vector<U32>{ 20 });
        CHECK(upper->mSpaces == std::vector<U32>{ 30 });
        CHECK(hall->mPortals == std::vector<U32>{ 0 });
        CHECK(ramp->mPortals == std::vector<U32>{ 0, 1 });
        CHECK(upper->mPortals == std::vector<U32>{ 1 });

        // Below the bound the same space is a floor, and the walk carries on through it.
        RoomGraphParams flat;
        flat.mRampBandM = 4.f;
        const RoomGraphPtr one = buildRoomGraph(product, flat);
        REQUIRE(one->mRooms.size() == 1);
        CHECK(one->mRooms[0].mSpaces == std::vector<U32>({ 10, 20, 30 }));
    }

    TEST_CASE("skroomsgraph: identity holds and the hash moves only where the air moved", "[skroomsgraph]")
    {
        // The split rooms and, elsewhere on the tile, a room of its own behind a lintelled doorway.
        Scene s = splitRooms();
        s.wall(20.f, 16.f, 30.f, 16.25f);
        s.wall(20.f, 25.75f, 30.f, 26.f);
        s.wall(29.75f, 16.f, 30.f, 26.f);
        s.wall(20.f, 16.f, 20.25f, 20.5f);
        s.wall(20.f, 21.5f, 20.25f, 26.f);
        s.box(20.f, 20.5f, 20.25f, 21.5f, 2.2f, 3.f, false);
        s.box(20.f, 16.f, 30.f, 26.f, 3.f, 3.25f);

        // Ids are tracked from the last product, as the viewer runs it.
        const Result first = run(s.input(1));
        auto again = std::make_shared<RoomsInput>();
        again->mPrevious = first.mRooms;
        const Result before = run(s.input(1), again);
        s.box(14.f, 6.f, 15.f, 7.f, 0.f, 1.f, false);    // a crate in the east half
        auto moved = std::make_shared<RoomsInput>();
        moved->mPrevious = before.mRooms;
        const Result after = run(s.input(1), moved);

        const RoomGraphPtr g0 = buildRoomGraph(*before.mRooms), g1 = buildRoomGraph(*after.mRooms);
        const Room* split0 = g0->ofSpace(before.at(6.f, 6.f));
        const Room* split1 = g1->ofSpace(after.at(6.f, 6.f));
        const Room* other0 = g0->ofSpace(before.at(25.f, 21.f));
        const Room* other1 = g1->ofSpace(after.at(25.f, 21.f));
        REQUIRE(split0 != nullptr);
        REQUIRE(split1 != nullptr);
        REQUIRE(other0 != nullptr);
        REQUIRE(other1 != nullptr);
        CHECK(other0 != split0);
        CHECK(g1->ofSpace(after.at(14.f, 3.f)) == split1);
        CHECK(split1->mId == split0->mId);
        CHECK(split1->mHash != split0->mHash);
        CHECK(other1->mId == other0->mId);
        CHECK(other1->mHash == other0->mHash);
        CHECK(g1->byId(split0->mId) == split1);
        CHECK(g1->byId(other0->mId) == other1);
    }

    TEST_CASE("skroomsgraph: ofSpace, roomOfId and byId agree with the walk", "[skroomsgraph][.flights]")
    {
        const Result r = run(stairHouse().input(1), stairDoor());
        const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
        REQUIRE(g->mRoomOfSpace.size() == r.mRooms->mSpaces.size());
        CHECK(g->mGeneration == r.mRooms->mGeneration);
        for (size_t i = 1; i < g->mRooms.size(); ++i)
        {
            CHECK(g->mRooms[i - 1].mId < g->mRooms[i].mId);
        }
        for (const Room& room : g->mRooms)
        {
            CHECK(g->byId(room.mId) == &room);
            CHECK(std::is_sorted(room.mSpaces.begin(), room.mSpaces.end()));
            CHECK(room.mId == room.mSpaces.front());
            for (U32 id : room.mSpaces)
            {
                CHECK(g->roomOfId(id) == &room);
            }
        }
        U32 roofs = 0;
        for (U32 i = 0; i < r.mRooms->mSpaces.size(); ++i)
        {
            const Space& sp = r.space(i);
            const Room* room = g->ofSpace(i);
            INFO("space " << i << " id " << sp.mId);
            CHECK(g->roomOfId(sp.mId) == room);
            CHECK((room != nullptr) == roomMember(sp));
            if (room)
            {
                CHECK(std::binary_search(room->mSpaces.begin(), room->mSpaces.end(), sp.mId));
            }
            if (sp.mRoof || sp.mFlight || sp.mBucket == EnclosureBucket::OUTDOORS)
            {
                CHECK(g->mRoomOfSpace[i] == ~0u);
            }
            roofs += sp.mRoof ? 1 : 0;
        }
        CHECK(roofs > 0);
        CHECK(g->mRoomOfSpace[r.at(24.f, 24.f)] == ~0u);
        CHECK(g->mRoomOfSpace[r.at(16.f, 2.f, 2.2f)] == ~0u);
        U32 unknown = 12345;
        while (r.mRooms->byId(unknown))
        {
            ++unknown;
        }
        CHECK(g->roomOfId(unknown) == nullptr);
        CHECK(g->byId(unknown) == nullptr);
        CHECK(g->ofSpace(static_cast<U32>(r.mRooms->mSpaces.size())) == nullptr);
        CHECK(g->bytes() > sizeof(RoomGraph));
    }

    /// A top view in VISUALS.md's style, written as PPM.
    struct TopView
    {
        static constexpr S32 SIZE = 800;
        static constexpr F32 MARGIN = 32.f;
        static constexpr F32 SCALE = (SIZE - 2 * MARGIN) / 32.f;   ///< pixels per metre over the 32 m tile

        std::vector<F32> mRgb = std::vector<F32>(static_cast<size_t>(SIZE) * SIZE * 3);

        TopView()
        {
            for (size_t i = 0; i < mRgb.size(); i += 3)
            {
                mRgb[i] = 0x10 / 255.f;
                mRgb[i + 1] = 0x10 / 255.f;
                mRgb[i + 2] = 0x14 / 255.f;
            }
        }

        static F32 px(F32 x) { return MARGIN + x * SCALE; }
        static F32 py(F32 y) { return SIZE - MARGIN - y * SCALE; }

        void blend(S32 x, S32 y, const LLColor4& c)
        {
            if (x < 0 || y < 0 || x >= SIZE || y >= SIZE)
            {
                return;
            }
            F32* p = &mRgb[(static_cast<size_t>(y) * SIZE + x) * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                p[k] = p[k] * (1.f - c.mV[VW]) + c.mV[k] * c.mV[VW];
            }
        }

        /// A world rectangle, filled; hatched with a diagonal every hatch pixels when hatch > 0.
        void rect(F32 x0, F32 y0, F32 x1, F32 y1, const LLColor4& c, S32 hatch = 0)
        {
            for (S32 y = static_cast<S32>(std::ceil(py(y1))); y < static_cast<S32>(std::ceil(py(y0))); ++y)
            {
                for (S32 x = static_cast<S32>(std::ceil(px(x0))); x < static_cast<S32>(std::ceil(px(x1))); ++x)
                {
                    if (hatch <= 0 || (x + y) % hatch == 0)
                    {
                        blend(x, y, c);
                    }
                }
            }
        }

        void line(F32 x0, F32 y0, F32 x1, F32 y1, const LLColor4& c)
        {
            const F32 ax = px(x0), ay = py(y0), bx = px(x1), by = py(y1);
            const S32 steps = llmax(1, static_cast<S32>(std::ceil(llmax(std::fabs(bx - ax), std::fabs(by - ay)))));
            for (S32 i = 0; i <= steps; ++i)
            {
                const F32 t = static_cast<F32>(i) / steps;
                blend(static_cast<S32>(ax + (bx - ax) * t), static_cast<S32>(ay + (by - ay) * t), c);
            }
        }

        void dot(F32 x, F32 y, F32 radius_px, const LLColor4& c)
        {
            const F32 cx = px(x), cy = py(y);
            for (S32 j = static_cast<S32>(cy - radius_px); j <= static_cast<S32>(cy + radius_px); ++j)
            {
                for (S32 i = static_cast<S32>(cx - radius_px); i <= static_cast<S32>(cx + radius_px); ++i)
                {
                    if ((i - cx) * (i - cx) + (j - cy) * (j - cy) <= radius_px * radius_px)
                    {
                        blend(i, j, c);
                    }
                }
            }
        }

        void write(const std::string& file) const
        {
            std::ofstream out(file, std::ios::binary);
            out << "P6\n" << SIZE << " " << SIZE << "\n255\n";
            for (const F32 f : mRgb)
            {
                out.put(static_cast<char>(llclamp(static_cast<S32>(f * 255.f + 0.5f), 0, 255)));
            }
        }
    };

    TEST_CASE("skroomsgraph: visual dump", "[skroomsgraph][.][visual]")
    {
        const std::string out_dir = LLStringUtil::getenv("SK_ROOMGRAPH_OUT");
        if (out_dir.empty())
        {
            SKIP("SK_ROOMGRAPH_OUT names no folder");
        }
        std::filesystem::create_directories(out_dir);
        const Scene scene = stairHouse();
        const Result r = run(scene.input(1), stairDoor());
        const RoomGraphPtr g = buildRoomGraph(*r.mRooms);
        const LLColor4 palette[] = { LLColor4(0.f, 0.86f, 0.9f, 1.f), LLColor4(0.9f, 0.f, 0.78f, 1.f), LLColor4(1.f, 0.7f, 0.f, 1.f),
                                     LLColor4(0.59f, 0.35f, 1.f, 1.f) };
        const LLColor4 solid(0.24f, 0.24f, 0.26f, 1.f), frame(0.45f, 0.45f, 0.48f, 1.f), hatch(0.6f, 0.6f, 0.62f, 0.8f);
        std::ofstream legend(out_dir + "/rooms.txt");
        for (size_t i = 0; i < g->mRooms.size(); ++i)
        {
            const Room& room = g->mRooms[i];
            legend << "room " << i << " id " << room.mId << " colour " << i % 4 << " band " << room.mFloorMin << ".." << room.mFloorMax
                   << " members";
            for (U32 id : room.mSpaces)
            {
                legend << ' ' << id;
            }
            legend << " portals";
            for (U32 p : room.mPortals)
            {
                legend << ' ' << p << ':' << portalKindName(r.mRooms->mPortals[p].mKind);
            }
            legend << '\n';
        }

        struct Level
        {
            const char* mName;
            F32 mLo, mHi;   ///< the storey band a room or portal is drawn on
            F32 mCut;       ///< solids crossing this height draw as walls
            F32 mStartX, mStartY;
        };
        for (const Level& level : { Level{ "level0", -0.5f, 2.5f, 1.f, 4.f, 4.f }, Level{ "level1", 2.5f, 5.5f, 4.25f, 24.f, 4.f } })
        {
            TopView view;
            for (size_t i = 0; i < g->mRooms.size(); ++i)
            {
                const Room& room = g->mRooms[i];
                if (room.mFloorMax < level.mLo || room.mFloorMin >= level.mHi)
                {
                    continue;
                }
                LLColor4 fill = palette[i % 4];
                fill.mV[VW] = 0.45f;
                for (S32 y = 0; y < room.mCellH; ++y)
                {
                    for (S32 x = 0; x < room.mCellW; ++x)
                    {
                        const S32 cx = room.mCellX0 + x, cy = room.mCellY0 + y;
                        if (!room.covers(cx, cy))
                        {
                            continue;
                        }
                        const F32 x0 = cx * RASTER_XY_M, y0 = cy * RASTER_XY_M, x1 = x0 + RASTER_XY_M, y1 = y0 + RASTER_XY_M;
                        view.rect(x0, y0, x1, y1, fill);
                        // The footprint's outline, where a side has no cell across it.
                        if (!room.covers(cx - 1, cy)) view.line(x0, y0, x0, y1, palette[i % 4]);
                        if (!room.covers(cx + 1, cy)) view.line(x1, y0, x1, y1, palette[i % 4]);
                        if (!room.covers(cx, cy - 1)) view.line(x0, y0, x1, y0, palette[i % 4]);
                        if (!room.covers(cx, cy + 1)) view.line(x0, y1, x1, y1, palette[i % 4]);
                    }
                }
            }
            for (const Solid& sd : scene.mSolids)
            {
                if (sd.mZ0 <= level.mCut && sd.mZ1 > level.mCut && !sd.mFlight)
                {
                    view.rect(sd.mX0, sd.mY0, sd.mX1, sd.mY1, solid);
                }
            }
            for (const Space& sp : r.mRooms->mSpaces)
            {
                if (!sp.mFlight)
                {
                    continue;
                }
                for (const auto& c : sp.mRaster)
                {
                    const PlanCell cell = rasterCell(c.first);
                    view.rect(cell.mX, cell.mY, cell.mX + RASTER_XY_M, cell.mY + RASTER_XY_M, hatch, 6);
                }
            }
            for (const Portal& p : r.mRooms->mPortals)
            {
                if (p.mSill < level.mLo || p.mSill >= level.mHi)
                {
                    continue;
                }
                const LLColor4 c = p.mKind == PortalKind::DOOR ? LLColor4(1.f, 0.2f, 0.2f, 1.f)
                                   : p.mKind == PortalKind::STAIR ? LLColor4(0.6f, 1.f, 0.2f, 1.f)
                                                                  : LLColor4(0.f, 0.86f, 0.9f, 1.f);
                view.line(p.mA.mV[VX], p.mA.mV[VY], p.mB.mV[VX], p.mB.mV[VY], c);
                view.dot(p.mCentre.mV[VX], p.mCentre.mV[VY], 5.f, c);
            }
            view.dot(level.mStartX, level.mStartY, 6.f, LLColor4(1.f, 1.f, 1.f, 1.f));
            view.line(0.f, 0.f, 32.f, 0.f, frame);
            view.line(32.f, 0.f, 32.f, 32.f, frame);
            view.line(32.f, 32.f, 0.f, 32.f, frame);
            view.line(0.f, 32.f, 0.f, 0.f, frame);
            view.write(out_dir + "/" + level.mName + ".ppm");
        }
    }
}
