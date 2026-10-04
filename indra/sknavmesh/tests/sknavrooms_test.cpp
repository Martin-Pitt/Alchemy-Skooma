/**
 * @file sknavrooms_test.cpp
 * @brief Tests for the spaces pass: portals, spaces and their ids over synthetic air.
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

#include "../sknavspaces.h"
#include "sknavtestscene.h"

#include "llstring.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>

namespace
{
    using namespace sknav;
    using namespace sknav_test;

    struct Result
    {
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;

        const Space* at(F32 x, F32 y, F32 z = 0.5f) const
        {
            Enclosure e;
            if (!mEnc->enclosureAt(LLVector3(x, y, z), e) || !e.mSpace)
            {
                return nullptr;
            }
            return &mRooms->mSpaces[e.mSpace - 1];
        }

        U32 index(const Space* s) const { return s ? static_cast<U32>(s - mRooms->mSpaces.data()) : ~0u; }

        std::vector<const Portal*> between(const Space* a, const Space* b) const
        {
            std::vector<const Portal*> out;
            for (const Portal& p : mRooms->mPortals)
            {
                if ((p.mSpaces[0] == index(a) && p.mSpaces[1] == index(b)) || (p.mSpaces[0] == index(b) && p.mSpaces[1] == index(a)))
                {
                    out.push_back(&p);
                }
            }
            return out;
        }

        /// Every flight has a stair portal at each of its ends.
        bool flightsLinked() const
        {
            bool any = false;
            for (const Space& sp : mRooms->mSpaces)
            {
                if (sp.mFlight)
                {
                    any = true;
                    if (count(&sp, PortalKind::STAIR) < 2)
                    {
                        return false;
                    }
                }
            }
            return any;
        }

        /// Gaps whose floor lies outside their space's bounds.
        U32 outOfBounds() const
        {
            U32 n = 0;
            for (const TileEnclosure& te : mEnc->mTiles)
            {
                if (!te.mAir)
                {
                    continue;
                }
                const TileAir& a = *te.mAir;
                const S32 zoff = static_cast<S32>(std::lround(a.mOrigin.mV[VZ] / a.mUnit));
                for (const AirLeaf& leaf : a.mLeaves)
                {
                    const F32 x0 = a.mOrigin.mV[VX] + leaf.mX * a.mCell, y0 = a.mOrigin.mV[VY] + leaf.mY * a.mCell;
                    const F32 side = (1 << leaf.mLevel) * a.mCell;
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const U16 s = te.mGaps[leaf.mFirstGap + i].mSpace;
                        if (!s)
                        {
                            continue;
                        }
                        const Space& sp = mRooms->mSpaces[s - 1];
                        const F32 z = (a.mGaps[leaf.mFirstGap + i].mFloor + zoff) * a.mUnit;
                        n += x0 < sp.mMin.mV[VX] || y0 < sp.mMin.mV[VY] || x0 + side > sp.mMax.mV[VX] || y0 + side > sp.mMax.mV[VY] ||
                                     z < sp.mMin.mV[VZ] - 1e-3f || z > sp.mMax.mV[VZ] + 1e-3f
                                 ? 1
                                 : 0;
                    }
                }
            }
            return n;
        }

        U32 count(const Space* s, PortalKind kind) const
        {
            U32 n = 0;
            for (const Portal& p : mRooms->mPortals)
            {
                n += p.mKind == kind && (p.mSpaces[0] == index(s) || p.mSpaces[1] == index(s)) ? 1 : 0;
            }
            return n;
        }
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

    std::string dump(const Result& r)
    {
        return dumpRooms(*r.mEnc, *r.mRooms);
    }

    /// The rooms pass over a scene whose moved tile alone has new air, once
    /// reading the last pass's memo and once not: the two must agree. A
    /// pass with nothing moved runs first, so portals stand where the last
    /// pass put them, as they do in the viewer. Alter changes the enclosure
    /// product before the spaces pass reads it.
    void incrementalIsFull(const Scene& before, const Scene& after, S32 tiles_x, S32 tiles_y, const TileKey& moved,
                           const std::vector<DoorEntity>& doors_before, const std::vector<DoorEntity>& doors_after, U32& reused,
                           const std::function<void(EnclosureProduct&)>& alter = nullptr)
    {
        auto kept = [](std::shared_ptr<EnclosureInput> in, const RoomsProductPtr& last, const TileKey& moved_key) {
            for (EnclosureInput::Tile& t : in->mTiles)
            {
                const RoomsTile* was = last->tile(t.mKey);
                if (was && !(t.mKey == moved_key))
                {
                    t.mAir = was->mAir;
                }
            }
            return in;
        };
        auto spaces = [&alter](const std::shared_ptr<EnclosureInput>& in, const std::vector<DoorEntity>& doors, const RoomsProductPtr& previous, bool change) {
            EnclosurePass pass(in);
            while (!pass.step(1u << 30))
            {
            }
            Result r;
            r.mEnc = pass.takeProduct();
            if (alter && change)
            {
                alter(*r.mEnc);
            }
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mDoors = doors;
            rooms->mPrevious = previous;
            r.mRooms = computeSpaces(rooms, r.mEnc);
            return r;
        };
        TileKey nowhere;
        nowhere.mX = nowhere.mY = -99;
        const Result first = spaces(before.input(tiles_x, tiles_y), doors_before, nullptr, false);
        const Result settled = spaces(kept(before.input(tiles_x, tiles_y), first.mRooms, nowhere), doors_before, first.mRooms, false);
        std::shared_ptr<EnclosureInput> in = kept(after.input(tiles_x, tiles_y), settled.mRooms, moved);
        const Result incremental = spaces(in, doors_after, settled.mRooms, true);
        auto bare = std::make_shared<RoomsProduct>(*settled.mRooms);
        bare->mMemo.reset();
        const Result full = spaces(in, doors_after, bare, true);
        tut::ensure_equals("no memo read without one", full.mRooms->mReused, 0u);
        tut::ensure("the incremental pass is the full pass", dump(incremental) == dump(full));
        reused = incremental.mRooms->mReused;
    }

    /// A roofed corridor across three tiles with a room off it between each
    /// pair of partitions, a doorway into each, and doorways between rooms
    /// where a partition has one. Doorways are 1 m wide under a lintel,
    /// unless unroofed; a partition at x < 0 is left out.
    Scene rowOfRooms(const std::vector<F32>& partitions, const std::vector<F32>& through, F32 unroofed = -1.f)
    {
        Scene s;
        s.wall(20.f, 4.f, 76.f, 4.25f);
        s.wall(20.f, 27.75f, 76.f, 28.f);
        s.wall(20.f, 4.f, 20.25f, 28.f);
        s.wall(75.75f, 4.f, 76.f, 28.f);
        auto lintel = [&s, unroofed](F32 x0, F32 y0, F32 x1, F32 y1) {
            if (std::fabs(x0 - unroofed) > 0.01f)
            {
                s.box(x0, y0, x1, y1, 2.2f, 3.f, false);
            }
        };
        F32 x = 20.f;
        for (F32 d : { 25.f, 35.f, 45.f, 55.f, 65.f, 72.f })
        {
            s.wall(x, 13.75f, d, 14.f);
            lintel(d, 13.75f, d + 1.f, 14.f);
            x = d + 1.f;
        }
        s.wall(x, 13.75f, 76.f, 14.f);
        for (size_t i = 0; i < partitions.size(); ++i)
        {
            const F32 p = partitions[i];
            if (p < 0.f)
            {
                continue;
            }
            if (std::find(through.begin(), through.end(), p) != through.end())
            {
                s.wall(p, 14.f, p + 0.25f, 20.f);
                lintel(p, 20.f, p + 0.25f, 21.f);
                s.wall(p, 21.f, p + 0.25f, 28.f);
            }
            else
            {
                s.wall(p, 14.f, p + 0.25f, 28.f);
            }
        }
        s.box(20.f, 4.f, 76.f, 28.f, 3.f, 3.25f);
        return s;
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
}

namespace tut
{
    struct sknavrooms_data
    {
    };
    typedef test_group<sknavrooms_data> sknavrooms_group;
    typedef sknavrooms_group::object sknavrooms_object;
    tut::sknavrooms_group sknavrooms_tests("sknavrooms");

    template<> template<>
    void sknavrooms_object::test<1>()
    {
        set_test_name("an L-shaped room stays one space");
        // A wide arm and a long narrow one, which the watershed seeds apart.
        Scene s;
        s.wall(2.f, 2.f, 14.f, 2.25f);
        s.wall(2.f, 25.75f, 6.f, 26.f);
        s.wall(2.f, 2.f, 2.25f, 4.f);
        s.wall(2.f, 5.f, 2.25f, 26.f);
        s.box(2.f, 4.f, 2.25f, 5.f, 2.2f, 3.f, false);
        s.wall(13.75f, 2.f, 14.f, 8.f);
        s.wall(6.f, 7.75f, 14.f, 8.f);
        s.wall(5.75f, 8.f, 6.f, 26.f);
        s.box(2.f, 2.f, 14.f, 8.f, 3.f, 3.25f);
        s.box(2.f, 8.f, 6.f, 26.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        const Space* a = r.at(4.f, 24.f);
        const Space* b = r.at(12.f, 4.f);
        ensure("both arms have a space", a && b);
        ensure("one space", a == b);
        ensure("not the outdoors", a != r.at(24.f, 24.f));
        ensure("INDOORS", a->mBucket == EnclosureBucket::INDOORS);
        ensure("walled", a->mL > 0.6f);
    }

    template<> template<>
    void sknavrooms_object::test<2>()
    {
        set_test_name("two doorways in one wall are two portals");
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 3.5f);
        s.wall(10.f, 4.5f, 10.25f, 7.f);
        s.wall(10.f, 8.f, 10.25f, 10.f);
        s.box(10.f, 3.5f, 10.25f, 4.5f, 2.2f, 3.f, false);
        s.box(10.f, 7.f, 10.25f, 8.f, 2.2f, 3.f, false);
        const Result r = run(s.input(1));
        const Space* a = r.at(6.f, 6.f);
        const Space* b = r.at(14.f, 6.f);
        ensure("two spaces", a && b && a != b);
        const std::vector<const Portal*> p = r.between(a, b);
        ensure_equals("two portals", p.size(), 2u);
        for (const Portal* q : p)
        {
            ensure("openings", q->mKind == PortalKind::OPENING);
            ensure("with a lintel", q->lintel());
            ensure("walled both sides", q->walled());
            ensure("a doorway wide", q->mWidth > 0.7f && q->mWidth < 1.4f);
        }
    }

    template<> template<>
    void sknavrooms_object::test<3>()
    {
        set_test_name("a double door is one portal spanning both leaves");
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 7.f, 10.25f, 10.f);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(10.125f, 5.5f, 1.f, 0.f, 1.f));
        rooms->mDoors.push_back(door(10.125f, 6.5f, 1.f, 0.f, 1.f));
        const Result r = run(s.input(1), rooms);
        const Space* a = r.at(6.f, 6.f);
        const Space* b = r.at(14.f, 6.f);
        ensure("two spaces", a && b && a != b);
        const std::vector<const Portal*> p = r.between(a, b);
        ensure_equals("one portal", p.size(), 1u);
        ensure("a door", p[0]->mKind == PortalKind::DOOR);
        ensure("spanning both leaves", std::fabs(p[0]->mWidth - 2.f) < 0.1f);
    }

    template<> template<>
    void sknavrooms_object::test<4>()
    {
        set_test_name("a door on the floor above does not cut this floor");
        Scene s = twoRooms();
        // A passage 2 m wide with no lintel: nothing cuts it.
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 7.f, 10.25f, 10.f);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(10.125f, 6.f, 1.f, 0.f, 2.f, 3.25f));
        const Result r = run(s.input(1), rooms);
        const Space* a = r.at(6.f, 6.f);
        ensure("a space", a != nullptr);
        ensure("one space", a == r.at(14.f, 6.f));
        ensure("no door portal", r.count(a, PortalKind::DOOR) == 0);
    }

    template<> template<>
    void sknavrooms_object::test<5>()
    {
        set_test_name("a stairwell behind a door is its own space, linked to its flight by a stair portal");
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);            // a deck the stair climbs to
        s.flight(12.f, 20.f, 1.f, 3.f, 3.25f);
        s.wall(9.f, 0.75f, 20.f, 1.f, 6.f);
        s.wall(9.f, 3.f, 20.f, 3.25f, 6.f);
        s.wall(8.75f, 0.75f, 9.f, 1.5f, 6.f);
        s.wall(8.75f, 2.5f, 9.f, 3.25f, 6.f);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(8.875f, 2.f, 1.f, 0.f, 1.f));
        const Result r = run(s.input(1), rooms);
        const Space* landing = r.at(10.5f, 2.f);
        const Space* hall = r.at(4.f, 2.f);
        const Space* stair = r.at(16.f, 2.f, 2.2f);
        ensure("all found", landing && hall && stair);
        ensure("the flight is its own space", stair->mFlight && stair != landing && stair != hall);
        ensure("the landing is not the hall", landing != hall);
        ensure_equals("a door between them", r.between(landing, hall).size(), 1u);
        ensure("a stair portal onto the flight", !r.between(landing, stair).empty() && r.between(landing, stair)[0]->mKind == PortalKind::STAIR);
    }

    template<> template<>
    void sknavrooms_object::test<6>()
    {
        set_test_name("an open stair is cut at each floor, with no opening at its top");
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 1.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        const Space* ground = r.at(6.f, 6.f);
        const Space* deck = r.at(24.f, 4.f, 3.75f);
        const Space* stair = r.at(16.f, 2.f, 2.2f);
        ensure("all found", ground && deck && stair);
        ensure("three spaces", ground != deck && stair != ground && stair != deck);
        ensure("the flight's", stair->mFlight);
        ensure("stair portals at the bottom and the top", !r.between(stair, ground).empty() && !r.between(stair, deck).empty());
        for (const Portal* p : r.between(stair, deck))
        {
            ensure("no opening at the top", p->mKind == PortalKind::STAIR);
        }
        for (const Portal* p : r.between(stair, ground))
        {
            ensure("nor at the bottom", p->mKind == PortalKind::STAIR);
        }
    }

    template<> template<>
    void sknavrooms_object::test<7>()
    {
        set_test_name("a roof on posts is SHELTERED however deep under it, for want of walls");
        Scene s;
        s.mGround[0] = 2.f;
        s.mGround[1] = 2.f;
        s.mGround[2] = 62.f;
        s.mGround[3] = 62.f;
        s.box(2.5f, 2.5f, 61.5f, 61.5f, 3.f, 3.25f);
        for (F32 px : { 2.5f, 61.25f })
        {
            for (F32 py : { 2.5f, 61.25f })
            {
                s.box(px, py, px + 0.25f, py + 0.25f, 0.f, 3.f, false);
            }
        }
        const Result r = run(s.input(2, 2));
        const Space* under = r.at(32.f, 32.f);
        ensure("a space", under != nullptr);
        ensure("deep enough to read as INDOORS by e alone", under->mEMean > 0.75f);
        ensure("hardly walled", under->mL < 0.4f);
        ensure("no sky map, no cover", under->mCoverMean == 0.f);
        ensure("SHELTERED", under->mBucket == EnclosureBucket::SHELTERED);
    }

    template<> template<>
    void sknavrooms_object::test<8>()
    {
        set_test_name("space ids survive a 0.25 m nudge of everything");
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 6.f, 2.2f, 3.f, false);
        const Result before = run(s.input(1));
        // Ids no hash of the new layout could give back.
        auto named = std::make_shared<RoomsProduct>(*before.mRooms);
        for (size_t i = 0; i < named->mSpaces.size(); ++i)
        {
            named->mSpaces[i].mId = 1000 + static_cast<U32>(i);
        }
        s.mShift = 0.25f;
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mPrevious = named;
        const Result after = run(s.input(1), rooms);
        U32 kept = 0, big = 0;
        for (const Space& old : named->mSpaces)
        {
            if (old.mFloorArea < 4.f)
            {
                continue;
            }
            ++big;
            const Space* now = after.at(old.mCentre.mV[VX] + 0.25f, old.mCentre.mV[VY], old.mFloorMin + 0.5f);
            kept += now && now->mId == old.mId ? 1 : 0;
        }
        ensure("some spaces", big >= 3);
        ensure_equals("every one kept its id", kept, big);
    }

    template<> template<>
    void sknavrooms_object::test<9>()
    {
        set_test_name("a lift shaft is a space of its own, INDOORS");
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 6.f, 2.2f, 3.f, false);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mLifts.emplace_back(LLVector3d(14.f, 6.f, -0.5f), LLVector3d(16.f, 8.f, 20.f));
        const Result r = run(s.input(1), rooms);
        const Space* lift = r.at(15.f, 7.f);
        const Space* room = r.at(12.f, 4.f);
        ensure("found", lift && room);
        ensure("its own", lift != room && lift->mLift);
        ensure("INDOORS", lift->mBucket == EnclosureBucket::INDOORS);
    }

    template<> template<>
    void sknavrooms_object::test<11>()
    {
        set_test_name("a hall across a seam between blocks of tiles stays one space, and keeps it when one side is rebuilt");
        // Tiles 1 and 2 are cut in different blocks; the seam at x = 64 is
        // close enough to the door for indoorness to climb across it.
        Scene s;
        s.wall(62.f, 3.f, 80.f, 4.25f);         // thick enough to read as jambs past the seam
        s.wall(62.f, 11.75f, 80.f, 13.f);
        s.wall(79.75f, 4.f, 80.f, 12.f);
        s.wall(62.f, 4.f, 62.25f, 7.f);
        s.wall(62.f, 9.f, 62.25f, 12.f);
        s.box(62.f, 7.f, 62.25f, 9.f, 2.2f, 3.f, false);
        s.box(62.f, 4.f, 80.f, 12.f, 3.f, 3.25f);
        // And one across the seam at x = 32, inside a block, for the rebuild to leave alone.
        s.wall(30.f, 18.f, 48.f, 19.25f);
        s.wall(30.f, 26.75f, 48.f, 28.f);
        s.wall(47.75f, 19.f, 48.f, 27.f);
        s.wall(30.f, 19.f, 30.25f, 22.f);
        s.wall(30.f, 24.f, 30.25f, 27.f);
        s.box(30.f, 22.f, 30.25f, 24.f, 2.2f, 3.f, false);
        s.box(30.f, 19.f, 48.f, 27.f, 3.f, 3.25f);
        const Result r = run(s.input(3));
        const Space* other = r.at(40.f, 23.f);
        ensure("the other hall is one space", other && other == r.at(31.f, 23.f));
        const Space* hall = r.at(70.f, 8.f);
        ensure("found", hall != nullptr);
        ensure("one space both sides of the seam", hall == r.at(63.f, 8.f));
        ensure("INDOORS", hall->mBucket == EnclosureBucket::INDOORS);

        // Something moves in the far tile only: the near ones keep their seeds.
        s.box(90.f, 25.f, 91.f, 26.f, 0.f, 1.f, false);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mPrevious = r.mRooms;
        std::shared_ptr<EnclosureInput> in = s.input(3);
        for (EnclosureInput::Tile& t : in->mTiles)
        {
            if (t.mKey.mX < 2)
            {
                t.mAir = r.mRooms->tile(t.mKey)->mAir;
            }
        }
        const Result again = run(in, rooms);
        ensure("the moved tile and at most a ring tile cut again", again.mRooms->mDirtyTiles >= 1 && again.mRooms->mDirtyTiles <= 2);
        const Space* still = again.at(70.f, 8.f);
        ensure("still one space", still && still == again.at(63.f, 8.f));
        ensure_equals("with its id", still->mId, hall->mId);
        const Space* kept = again.at(40.f, 23.f);
        ensure("the other hall untouched", kept && kept == again.at(31.f, 23.f) && kept->mId == other->mId);

        // Rebuilt again and again, the seam neither doubles a portal nor moves an id.
        const size_t portals = still->mPortals.size();
        RoomsProductPtr last = again.mRooms;
        for (S32 k = 0; k < 4; ++k)
        {
            Scene t = s;
            if (k & 1)
            {
                t.box(85.f, 20.f, 86.f, 21.f, 0.f, 1.f, false);
            }
            auto next_rooms = std::make_shared<RoomsInput>();
            next_rooms->mPrevious = last;
            std::shared_ptr<EnclosureInput> next_in = t.input(3);
            for (EnclosureInput::Tile& tile : next_in->mTiles)
            {
                const RoomsTile* was = last->tile(tile.mKey);
                if (tile.mKey.mX < 2 && was)
                {
                    tile.mAir = was->mAir;
                }
            }
            const Result round = run(next_in, next_rooms);
            const Space* h = round.at(70.f, 8.f);
            ensure("one space each time", h && h == round.at(63.f, 8.f));
            ensure_equals("the same id each time", h->mId, hall->mId);
            ensure_equals("no portal doubled", h->mPortals.size(), portals);
            const Space* o = round.at(40.f, 23.f);
            ensure("the other hall holds", o && o->mId == other->mId);
            last = round.mRooms;
        }
    }

    template<> template<>
    void sknavrooms_object::test<13>()
    {
        set_test_name("a switchback stair with a landing between its flights leaves neither flight an island");
        Scene s;
        s.flight(8.f, 14.f, 1.f, 3.f, 0.f, 1.75f);
        s.box(14.f, 1.f, 17.f, 6.f, 0.f, 1.75f);                // the landing
        s.wall(8.f, 3.f, 14.f, 4.f, 5.f);
        s.flight(14.f, 8.f, 4.f, 6.f, 1.75f, 3.25f);
        s.box(4.f, 4.f, 8.f, 8.f, 3.f, 3.25f);                  // the floor it climbs to
        const Result r = run(s.input(1));
        const Space* landing = r.at(15.5f, 3.5f, 2.2f);
        ensure("a landing", landing != nullptr && !landing->mFlight);
        ensure("every flight linked at both ends", r.flightsLinked());
    }

    template<> template<>
    void sknavrooms_object::test<14>()
    {
        set_test_name("a flight wider than a stair portal's run still meets its floors");
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 0.f, 8.f, 0.f, 3.25f);
        const Result r = run(s.input(1));
        ensure("linked", r.flightsLinked());
        ensure("to the deck", !r.between(r.at(16.f, 4.f, 2.9f), r.at(24.f, 4.f, 3.75f)).empty());
    }

    template<> template<>
    void sknavrooms_object::test<15>()
    {
        set_test_name("a flight across a tile seam meets both its floors");
        Scene s;
        s.box(36.f, 0.f, 44.f, 8.f, 3.f, 3.25f);
        s.flight(28.f, 36.f, 1.f, 3.f, 0.f, 3.25f);
        const Result r = run(s.input(2));
        ensure("linked", r.flightsLinked());
        ensure("to the deck", !r.between(r.at(34.f, 2.f, 2.8f), r.at(40.f, 4.f, 3.75f)).empty());
        ensure("to the ground", !r.between(r.at(29.f, 2.f, 0.8f), r.at(20.f, 4.f)).empty());
    }

    template<> template<>
    void sknavrooms_object::test<16>()
    {
        set_test_name("a floor that meets a flight only along its side is still linked to it");
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 1.f, 3.f, 0.f, 3.25f);
        s.box(13.f, 3.f, 16.f, 6.f, 0.f, 1.25f);                // a gallery beside the flight's middle
        s.wall(12.75f, 3.f, 13.f, 6.25f, 4.f);
        s.wall(16.f, 3.f, 16.25f, 6.25f, 4.f);
        s.wall(12.75f, 6.f, 16.25f, 6.25f, 4.f);
        const Result r = run(s.input(1));
        const Space* gallery = r.at(14.5f, 5.f, 1.75f);
        const Space* stair = r.at(16.f, 2.f, 2.9f);
        ensure("found", gallery && stair && stair->mFlight && gallery != stair);
        const std::vector<const Portal*> p = r.between(gallery, stair);
        ensure("linked by a stair portal", !p.empty() && p[0]->mKind == PortalKind::STAIR);
    }

    template<> template<>
    void sknavrooms_object::test<17>()
    {
        set_test_name("ground under a small platform far overhead, beside a tall wall, is OUTDOORS though the wall hides much sky");
        Scene s;
        s.box(13.f, 13.f, 19.f, 19.f, 30.f, 30.5f);
        s.box(20.f, 0.f, 21.f, 32.f, 0.f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        ensure("read", r.mEnc->enclosureAt(LLVector3(16.1f, 16.1f, 0.5f), e));
        ensure("the wall hides much of the sky", e.mS < 0.8f);
        ensure("OUTDOORS at the gap", e.mBucket == EnclosureBucket::OUTDOORS);
        ensure("OUTDOORS as a space", r.at(16.1f, 16.1f)->mBucket == EnclosureBucket::OUTDOORS);
    }

    template<> template<>
    void sknavrooms_object::test<18>()
    {
        set_test_name("ground under a large low roof has almost no sky");
        Scene s;
        s.box(2.f, 2.f, 62.f, 62.f, 5.f, 5.25f);
        auto in = s.input(2, 2);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        ensure("read", r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e));
        ensure("the sky is hidden", e.mS < 0.05f);
        ensure("INDOORS by e", e.mE > 0.75f);
    }

    /// A 60 m roof on posts at 3 m, with walls on the given sides of north, east, south, west.
    Result roofOnPosts(bool walls[4])
    {
        Scene s;
        s.mGround[0] = 2.f;
        s.mGround[1] = 2.f;
        s.mGround[2] = 62.f;
        s.mGround[3] = 62.f;
        s.box(2.5f, 2.5f, 61.5f, 61.5f, 3.f, 3.25f);
        for (F32 px : { 2.5f, 61.25f })
        {
            for (F32 py : { 2.5f, 61.25f })
            {
                s.box(px, py, px + 0.25f, py + 0.25f, 0.f, 3.f, false);
            }
        }
        if (walls[0])
        {
            s.box(2.5f, 61.f, 61.5f, 61.5f, 0.f, 3.f, false);
        }
        if (walls[1])
        {
            s.box(61.f, 2.5f, 61.5f, 61.5f, 0.f, 3.f, false);
        }
        if (walls[2])
        {
            s.box(2.5f, 2.5f, 61.5f, 3.f, 0.f, 3.f, false);
        }
        if (walls[3])
        {
            s.box(2.5f, 2.5f, 3.f, 61.5f, 0.f, 3.f, false);
        }
        auto in = s.input(2, 2);
        s.addSky(*in);
        return run(in);
    }

    template<> template<>
    void sknavrooms_object::test<19>()
    {
        set_test_name("under a roof on posts, sky map and all, is SHELTERED");
        bool walls[4] = { false, false, false, false };
        const Result r = roofOnPosts(walls);
        const Space* under = r.at(32.f, 32.f);
        ensure("a space", under != nullptr);
        ensure("deep enough to read as INDOORS by e alone", under->mEMean > 0.75f);
        ensure("SHELTERED", under->mBucket == EnclosureBucket::SHELTERED);
    }

    template<> template<>
    void sknavrooms_object::test<20>()
    {
        set_test_name("a roof with one wall and three open sides is SHELTERED");
        bool walls[4] = { true, false, false, false };
        const Result r = roofOnPosts(walls);
        const Space* under = r.at(32.f, 32.f);
        ensure("a space", under != nullptr);
        ensure("deep enough to read as INDOORS by e alone", under->mEMean > 0.75f);
        ensure("a quarter walled", under->mL > 0.15f && under->mL < 0.45f);
        ensure("SHELTERED", under->mBucket == EnclosureBucket::SHELTERED);
    }

    template<> template<>
    void sknavrooms_object::test<21>()
    {
        set_test_name("the walls of a roofless well hide the low sky, and a dense view reads as the map does");
        Scene s;
        s.box(12.f, 12.f, 20.f, 13.f, 0.f, 10.f, false);
        s.box(12.f, 19.f, 20.f, 20.f, 0.f, 10.f, false);
        s.box(12.f, 12.f, 13.f, 20.f, 0.f, 10.f, false);
        s.box(19.f, 12.f, 20.f, 20.f, 0.f, 10.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const LLVector3d mid(16.5, 16.5, 2.0);
        const F32 walled = in->mSky->coverAbove(mid, 50.f);
        ensure("most of the sky is behind the walls", walled > 0.6f && walled < 0.95f);
        ensure("outside the well the sky is open", in->mSky->coverAbove(LLVector3d(4.5, 4.5, 2.0), 50.f) < 0.1f);
        Scene pole;
        pole.box(16.f, 3.f, 17.f, 4.f, 0.f, 36.f, false);
        auto pin = pole.input(1);
        pole.addSky(*pin);
        ensure("a thin pole 12 m off hides a sliver, not its wedge", pin->mSky->coverAbove(LLVector3d(4.5, 4.5, 2.0), 50.f) < 0.04f);
        const SkyView view(*in->mSky, -40.0, -40.0, 72.0, 72.0);
        ensure("the view agrees", std::fabs(view.coverAbove(mid, 50.f) - walled) < 1e-5f);
    }

    template<> template<>
    void sknavrooms_object::test<22>()
    {
        set_test_name("a street between towers stays OUTDOORS with the sky map, though the towers hide most of its sky");
        Scene s;
        s.box(0.f, 0.f, 12.f, 32.f, 0.f, 36.f, false);
        s.box(20.f, 0.f, 32.f, 32.f, 0.f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        ensure("read", r.mEnc->enclosureAt(LLVector3(16.f, 16.f, 0.5f), e));
        ensure("the towers hide most of the sky", e.mS < 0.5f);
        ensure("OUTDOORS at the gap", e.mBucket == EnclosureBucket::OUTDOORS);
        const Space* street = r.at(16.f, 16.f);
        ensure("a space", street != nullptr);
        ensure("nothing roofs it", street->mCoverMean < 0.05f);
        ensure("OUTDOORS as a space", street->mBucket == EnclosureBucket::OUTDOORS);
    }

    template<> template<>
    void sknavrooms_object::test<23>()
    {
        set_test_name("two yards on a terrace join through a doorway in the tall wall between them, its shadow on the sky no bar");
        Scene s;
        s.mGround[0] = 2.f;
        s.mGround[1] = 2.f;
        s.mGround[2] = 18.f;
        s.mGround[3] = 10.f;
        s.box(9.75f, 0.f, 10.f, 5.f, 0.f, 36.f, false);
        s.box(9.75f, 6.f, 10.f, 32.f, 0.f, 36.f, false);
        s.box(9.75f, 5.f, 10.f, 6.f, 2.2f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        const Space* west = r.at(6.f, 6.f);
        const Space* east = r.at(14.f, 6.f);
        ensure("spaces", west && east);
        Enclosure e;
        ensure("walls hide much of the sky by them", r.mEnc->enclosureAt(LLVector3(10.5f, 8.f, 0.5f), e) && e.mS < 0.9f);
        ensure("one space", west == east);
        ensure("OUTDOORS", west->mBucket == EnclosureBucket::OUTDOORS);
    }

    template<> template<>
    void sknavrooms_object::test<24>()
    {
        set_test_name("the spaces pass grows with the tile count: four times the field costs about four times as much");
        // A field of posts 2 m apart: a watershed seed between every four,
        // all one outdoor space. SKNAV_SCALING runs it at 16 and 64 tiles,
        // where a cost that grows with seeds times the space shows.
        Scene s;
        for (S32 j = 0; j < 16; ++j)
        {
            for (S32 i = 0; i < 16; ++i)
            {
                s.wall(i * 2.f + 0.875f, j * 2.f + 0.875f, i * 2.f + 1.125f, j * 2.f + 1.125f);
            }
        }
        const TileAirPtr one = s.tile(0, 0);
        auto town = [&one](S32 side) {
            auto in = std::make_shared<EnclosureInput>();
            for (S32 ky = 0; ky < side; ++ky)
            {
                for (S32 kx = 0; kx < side; ++kx)
                {
                    auto air = std::make_shared<TileAir>(*one);
                    air->mOrigin.set(kx * 32.f, ky * 32.f, one->mOrigin.mV[VZ]);
                    EnclosureInput::Tile t;
                    t.mKey.mX = kx;
                    t.mKey.mY = ky;
                    t.mAir = air;
                    in->mTiles.push_back(t);
                }
            }
            EnclosurePass pass(in);
            while (!pass.step(1u << 30))
            {
            }
            std::shared_ptr<EnclosureProduct> enc = pass.takeProduct();
            // The best of a few, so a busy machine does not fail it.
            F64 best = 1e30;
            RoomsProductPtr rooms;
            for (S32 k = 0; k < 3; ++k)
            {
                const auto t0 = std::chrono::steady_clock::now();
                rooms = computeSpaces(std::make_shared<RoomsInput>(), enc);
                best = std::min(best, std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            return std::make_pair(best, rooms);
        };
        const S32 side = LLStringUtil::getenv("SKNAV_SCALING").empty() ? 2 : 4;
        const auto small = town(side), large = town(2 * side);
        ensure("many seeds, one space", small.second->mSeeds > 64u * side * side && large.second->mSpaces.size() == 1);
        const F64 ratio = large.first / std::max(small.first, 1e-3);
        ensure("4x the tiles within 6x the time, not " + std::to_string(ratio) + "x (" + std::to_string(small.first) + " ms, " +
                   std::to_string(large.first) + " ms)",
               ratio < 6.0);
    }

    template<> template<>
    void sknavrooms_object::test<25>()
    {
        set_test_name("one tile rebuilt among scattered walls is cut again with its ring, not with the mesh");
        // Walls of every length strewn about, so basins run on past a ring
        // tile's far edges, where a cut of the ring alone sees them otherwise.
        Scene s;
        U32 lcg = 12345u;
        auto rnd = [&lcg](F32 lo, F32 hi) {
            lcg = lcg * 1664525u + 1013904223u;
            return lo + (hi - lo) * static_cast<F32>(lcg >> 8) / static_cast<F32>(1u << 24);
        };
        for (S32 k = 0; k < 24; ++k)
        {
            const F32 x = rnd(1.f, 29.f), y = rnd(1.f, 29.f), len = rnd(1.f, 12.f);
            if (k & 1)
            {
                s.wall(x, y, std::min(31.f, x + len), y + 0.25f);
            }
            else
            {
                s.wall(x, y, x + 0.25f, std::min(31.f, y + len));
            }
        }
        const TileAirPtr one = s.tile(0, 0);
        auto field = [&one](const RoomsProductPtr& previous, const TileKey& moved) {
            auto in = std::make_shared<EnclosureInput>();
            for (S32 ky = 0; ky < 5; ++ky)
            {
                for (S32 kx = 0; kx < 5; ++kx)
                {
                    EnclosureInput::Tile t;
                    t.mKey.mX = kx;
                    t.mKey.mY = ky;
                    const RoomsTile* was = previous ? previous->tile(t.mKey) : nullptr;
                    if (was && !(t.mKey == moved))
                    {
                        t.mAir = was->mAir;
                    }
                    else
                    {
                        auto air = std::make_shared<TileAir>(*one);
                        air->mOrigin.set(kx * 32.f, ky * 32.f, one->mOrigin.mV[VZ]);
                        t.mAir = air;
                    }
                    in->mTiles.push_back(t);
                }
            }
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mPrevious = previous;
            return run(in, rooms);
        };
        TileKey mid;
        mid.mX = mid.mY = 2;
        const Result first = field(nullptr, mid);
        ensure_equals("every tile cut the first time", first.mRooms->mDirtyTiles, 25u);
        const Result again = field(first.mRooms, mid);
        ensure("the moved tile and at most its four neighbours cut again", again.mRooms->mDirtyTiles >= 1 && again.mRooms->mDirtyTiles <= 5);
        ensure_equals("the same spaces", again.mRooms->mSpaces.size(), first.mRooms->mSpaces.size());
    }

    template<> template<>
    void sknavrooms_object::test<26>()
    {
        set_test_name("a roof joins the room under it across a thin slab, not across a thick one, and every space's bounds hold its gaps");
        Scene s = twoRooms();
        // A solid block beside the rooms, its top flush with their roof, and a
        // tower over a small room whose roof lies 4 m of solid above its ceiling.
        s.box(18.f, 2.f, 22.f, 10.f, 0.f, 3.25f);
        s.wall(24.f, 2.f, 30.f, 2.25f);
        s.wall(24.f, 7.75f, 30.f, 8.f);
        s.wall(24.f, 2.f, 24.25f, 8.f);
        s.wall(29.75f, 2.f, 30.f, 8.f);
        s.box(24.f, 2.f, 30.f, 8.f, 3.f, 7.f);
        s.box(24.f, 2.f, 30.f, 8.f, 7.f, 7.25f);
        const Result r = run(s.input(1));
        const Space* west = r.at(6.f, 6.f);
        const Space* tower = r.at(27.f, 5.f);
        ensure("rooms found", west && tower);
        ensure("the thin roof is the room's", r.at(6.f, 6.f, 3.5f) == west);
        ensure("the roof over the block joins the roof beside it", r.at(20.f, 6.f, 3.5f) == r.at(16.f, 6.f, 3.5f) && r.at(20.f, 6.f, 3.5f));
        ensure("the tower's top is not the room 4 m below", r.at(27.f, 5.f, 7.5f) != tower);
        ensure_equals("no gap outside its space's bounds", r.outOfBounds(), 0u);
    }

    template<> template<>
    void sknavrooms_object::test<27>()
    {
        set_test_name("a flight walled in on every side, meeting no floor, is no flight space, so none is an island");
        Scene s;
        s.flight(8.f, 16.f, 2.f, 4.f, 2.f, 3.f);
        s.wall(7.75f, 1.75f, 16.25f, 2.f, 5.f);
        s.wall(7.75f, 4.f, 16.25f, 4.25f, 5.f);
        s.wall(7.75f, 1.75f, 8.f, 4.25f, 5.f);
        s.wall(16.f, 1.75f, 16.25f, 4.25f, 5.f);
        const Result r = run(s.input(1));
        ensure("its treads have a space", r.at(12.f, 3.f, 3.f) != nullptr);
        for (const Space& sp : r.mRooms->mSpaces)
        {
            ensure("every flight space has a stair portal", !sp.mFlight || r.count(&sp, PortalKind::STAIR) > 0);
        }
    }

    template<> template<>
    void sknavrooms_object::test<28>()
    {
        set_test_name("a pass reading the last one's memo is the pass that reads none: a doorway appears, a wall goes, a lintel goes, nothing moves");
        const std::vector<F32> partitions = { 30.f, 40.f, 50.f, 60.f, 70.f };
        TileKey moved;
        moved.mX = 1;
        moved.mY = 0;
        const std::vector<DoorEntity> none;
        U32 reused = 0;
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms(partitions, { 40.f, 50.f }), 3, 1, moved, none, none, reused);
        ensure("a doorway appears: the rest reused", reused > 0);
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms({ 30.f, -1.f, 50.f, 60.f, 70.f }, {}), 3, 1, moved, none, none, reused);
        ensure("a wall goes: the rest reused", reused > 0);
        incrementalIsFull(rowOfRooms(partitions, {}), rowOfRooms(partitions, {}, 55.f), 3, 1, moved, none, none, reused);
        ensure("a lintel goes: the rest reused", reused > 0);
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms(partitions, { 40.f }), 3, 1, moved, none, none, reused);
        ensure("nothing moves: the rest reused", reused > 0);
        // A door the census finds changes what every cut near a door reads: nothing is reused.
        incrementalIsFull(rowOfRooms(partitions, {}), rowOfRooms(partitions, {}), 3, 1, moved, none, { door(45.5f, 13.875f, 0.f, 1.f, 1.f) }, reused);
        ensure_equals("a door appears: nothing reused", reused, 0u);
    }

    template<> template<>
    void sknavrooms_object::test<29>()
    {
        set_test_name("a memo is read only where its keys hold: a region's root moves across the mesh, indoorness moves in a tile whose air did not");
        TileKey moved;
        moved.mX = 1;
        moved.mY = 0;
        const std::vector<DoorEntity> none;
        U32 reused = 0;
        // Two rooms wholly in the last tile, a doorway between them; the second
        // joins a room in the moved tile when the wall between them goes, so
        // the pair's first region is now the other one.
        auto strips = [](F32 wall_top) {
            Scene s = rowOfRooms({ 30.f, 40.f, 50.f, -1.f, -1.f }, {});
            s.wall(63.75f, 14.f, 64.f, wall_top);
            s.wall(64.f, 20.75f, 69.f, 21.f);
            s.box(69.f, 20.75f, 70.f, 21.f, 2.2f, 3.f, false);
            s.wall(70.f, 20.75f, 76.f, 21.f);
            return s;
        };
        incrementalIsFull(strips(28.f), strips(21.f), 3, 1, moved, none, none, reused);
        ensure("a root moves: the rest reused", reused > 0);
        // A doorway just past the seam, a lintel over it: a ceiling lowered on
        // the moved tile's side, within reach of its probes, takes the lintel away.
        auto seam = [](bool lowered) {
            Scene s = rowOfRooms({ 30.f, 40.f, 50.f, 64.5f, 70.f }, { 64.5f });
            if (lowered)
            {
                s.box(61.f, 14.f, 64.f, 28.f, 2.3f, 3.f, false);
            }
            return s;
        };
        incrementalIsFull(seam(false), seam(true), 3, 1, moved, none, none, reused);
        ensure("a ceiling moves: the rest reused", reused > 0);
        // An opening with jambs and no lintel, in the last tile: indoorness alone decides it.
        Scene s = rowOfRooms({ 30.f, 40.f, 50.f, 60.f, -1.f }, {});
        s.wall(67.5f, 14.f, 67.75f, 20.f);
        s.wall(67.5f, 21.f, 67.75f, 28.f);
        TileKey nowhere;
        nowhere.mX = nowhere.mY = -99;
        incrementalIsFull(s, s, 3, 1, nowhere, none, none, reused, [](EnclosureProduct& enc) {
            for (TileEnclosure& te : enc.mTiles)
            {
                if (!te.mAir || te.mKey.mX != 2)
                {
                    continue;
                }
                for (const AirLeaf& leaf : te.mAir->mLeaves)
                {
                    if (te.mAir->mOrigin.mV[VX] + leaf.mX * te.mAir->mCell >= 67.5f)
                    {
                        continue;
                    }
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        te.mGaps[leaf.mFirstGap + i].mE = 0;
                        te.mGaps[leaf.mFirstGap + i].mD = 0;
                    }
                }
            }
        });
        ensure("indoorness moves: the rest reused", reused > 0);
    }

    template<> template<>
    void sknavrooms_object::test<12>()
    {
        set_test_name("replay a mesh's air through both passes, when a file is named");
        // The format is written by a tool beside the navmesh exports: tiles'
        // level-0 air, doors, lift boxes, and points to answer spaces at.
        const std::string path = LLStringUtil::getenv("SKNAV_ROOMS_REPLAY");
        if (path.empty())
        {
            return;
        }
        std::shared_ptr<EnclosureInput> enc_in;
        std::shared_ptr<RoomsInput> rooms;
        std::vector<LLVector3d> points;
        ensure("read a replay file", readReplay(path, enc_in, rooms, points));
        const LLVector3d origin = enc_in->mMeshOrigin;

        const auto t0 = std::chrono::steady_clock::now();
        EnclosurePass pass(enc_in);
        while (!pass.step(1u << 30))
        {
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::shared_ptr<EnclosureProduct> enc = pass.takeProduct();
        SpacesPass spaces(rooms, enc);
        while (!spaces.step(1u << 30))
        {
        }
        const auto t2 = std::chrono::steady_clock::now();
        const RoomsProductPtr r = spaces.product();

        std::ofstream out(path + ".out.json");
        out << "{\"enclosure_ms\":" << std::chrono::duration<F64, std::milli>(t1 - t0).count()
            << ",\"spaces_ms\":" << std::chrono::duration<F64, std::milli>(t2 - t1).count() << ",\"stages\":{";
        for (S32 k = 0; k < SpacesPass::NUM_STAGES; ++k)
        {
            out << (k ? "," : "") << '"' << SpacesPass::stageName(k) << "\":" << spaces.stageMillis(k);
        }
        out << "},\"cells\":" << r->mCells << ",\"seeds\":" << r->mSeeds << ",\"spaces\":[";
        const F64 ox = origin.mdV[0], oy = origin.mdV[1], oz = origin.mdV[2];
        for (size_t i = 0; i < r->mSpaces.size(); ++i)
        {
            const Space& sp = r->mSpaces[i];
            out << (i ? "," : "") << "{\"id\":" << sp.mId << ",\"area\":" << sp.mFloorArea << ",\"bucket\":" << static_cast<S32>(sp.mBucket)
                << ",\"L\":" << sp.mL << ",\"e\":" << sp.mEMean << ",\"S\":" << sp.mSMean << ",\"lift\":" << sp.mLift << ",\"flight\":" << sp.mFlight
                << ",\"floor\":[" << sp.mFloorMin + oz << ',' << sp.mFloorMax + oz << "],\"centre\":[" << sp.mCentre.mV[0] + ox << ','
                << sp.mCentre.mV[1] + oy << "]}";
        }
        out << "],\"portals\":[";
        for (size_t i = 0; i < r->mPortals.size(); ++i)
        {
            const Portal& p = r->mPortals[i];
            out << (i ? "," : "") << "{\"kind\":\"" << portalKindName(p.mKind) << "\",\"spaces\":[" << p.mSpaces[0] << ',' << p.mSpaces[1]
                << "],\"a\":[" << p.mA.mV[0] + ox << ',' << p.mA.mV[1] + oy << "],\"b\":[" << p.mB.mV[0] + ox << ',' << p.mB.mV[1] + oy
                << "],\"centre\":[" << p.mCentre.mV[0] + ox << ',' << p.mCentre.mV[1] + oy << "],\"normal\":[" << p.mNormal.mV[0] << ','
                << p.mNormal.mV[1] << "],\"sill\":" << p.mSill + oz << ",\"head\":" << (p.lintel() ? p.mHead + oz : 0.0)
                << ",\"width\":" << p.mWidth << ",\"cues\":" << static_cast<S32>(p.mCues) << '}';
        }
        out << "],\"answers\":[";
        for (size_t i = 0; i < points.size(); ++i)
        {
            Enclosure e;
            const LLVector3 local(points[i] - origin);
            const S32 space = enc->enclosureAt(local, e) ? static_cast<S32>(e.mSpace) - 1 : -1;
            out << (i ? "," : "") << space;
        }
        out << "]}\n";
    }

    template<> template<>
    void sknavrooms_object::test<30>()
    {
        set_test_name("an L of cells traces the six runs of its outline");
        const std::vector<PlanCell> cells = { { 0.f, 0.f, 3.f }, { 1.f, 0.f, 3.f }, { 2.f, 0.f, 3.f }, { 0.f, 1.f, 3.f },
                                              { 0.f, 0.f, 5.f } };   // a repeated column keeps its lowest floor
        const std::vector<PlanEdge> edges = tracePlan(cells, 1.f);
        ensure_equals("six runs", edges.size(), 6u);
        for (const PlanEdge& e : edges)
        {
            ensure("all at the lowest floor", std::fabs(e.mZ - 3.f) < 1e-4f);
        }
        const auto has = [&edges](F32 x0, F32 y0, F32 x1, F32 y1)
        {
            for (const PlanEdge& e : edges)
            {
                if (std::fabs(e.mX0 - x0) < 1e-4f && std::fabs(e.mY0 - y0) < 1e-4f &&
                    std::fabs(e.mX1 - x1) < 1e-4f && std::fabs(e.mY1 - y1) < 1e-4f)
                {
                    return true;
                }
            }
            return false;
        };
        ensure("the bottom runs on", has(0.f, 0.f, 3.f, 0.f));
        ensure("the right stands", has(3.f, 0.f, 3.f, 1.f));
        ensure("the notch's far side runs back", has(3.f, 1.f, 1.f, 1.f));
        ensure("the notch's inner side stands", has(1.f, 1.f, 1.f, 2.f));
        ensure("the top runs back", has(1.f, 2.f, 0.f, 2.f));
        ensure("the left runs down to the start", has(0.f, 2.f, 0.f, 0.f));
    }

template<> template<>
    void sknavrooms_object::test<31>()
    {
        set_test_name("a plan with a hole, a slope and a pinch traces them apart");
        std::vector<PlanCell> cells;
        for (S32 y = 0; y < 3; ++y)
        {
            for (S32 x = 0; x < 3; ++x)
            {
                if (x != 1 || y != 1)
                {
                    cells.push_back({ F32(x), F32(y), 3.f });
                }
            }
        }
        for (S32 x = 5; x < 8; ++x)
        {
            cells.push_back({ F32(x), 6.f, 3.f + 0.2f * (x - 5) });   // a slope, one floor a step
        }
        cells.push_back({ 10.f, 10.f, 3.f });
        cells.push_back({ 11.f, 11.f, 3.f });   // two cells touching at a corner only
        const std::vector<PlanEdge> edges = tracePlan(cells, 1.f);
        ensure_equals("the ring's four, the hole's four, the slope's eight, the pinch's eight",
                      edges.size(), 24u);
        const auto has = [&edges](F32 x0, F32 y0, F32 x1, F32 y1, F32 z)
        {
            for (const PlanEdge& e : edges)
            {
                if (std::fabs(e.mX0 - x0) < 1e-4f && std::fabs(e.mY0 - y0) < 1e-4f &&
                    std::fabs(e.mX1 - x1) < 1e-4f && std::fabs(e.mY1 - y1) < 1e-4f && std::fabs(e.mZ - z) < 1e-4f)
                {
                    return true;
                }
            }
            return false;
        };
        ensure("the ring's bottom is one run", has(0.f, 0.f, 3.f, 0.f, 3.f));
        ensure("the hole's edge runs with the ring on its left", has(2.f, 1.f, 1.f, 1.f, 3.f));
        ensure("the slope steps at each floor", has(6.f, 6.f, 7.f, 6.f, 3.2f));
        ensure("the pinch's second cell stands alone", has(11.f, 11.f, 12.f, 11.f, 3.f));
    }

    template<> template<>
    void sknavrooms_object::test<32>()
    {
        set_test_name("the spaces pass leaves the overlay's draw with the product");
        // The L-shaped room of test 1.
        Scene s;
        s.wall(2.f, 2.f, 14.f, 2.25f);
        s.wall(2.f, 25.75f, 6.f, 26.f);
        s.wall(2.f, 2.f, 2.25f, 4.f);
        s.wall(2.f, 5.f, 2.25f, 26.f);
        s.box(2.f, 4.f, 2.25f, 5.f, 2.2f, 3.f, false);
        s.wall(13.75f, 2.f, 14.f, 8.f);
        s.wall(6.f, 7.75f, 14.f, 8.f);
        s.wall(5.75f, 8.f, 6.f, 26.f);
        s.box(2.f, 2.f, 14.f, 8.f, 3.f, 3.25f);
        s.box(2.f, 8.f, 6.f, 26.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        const SpacesDrawPtr draw = r.mRooms->mDraw;
        ensure("the draw came with the pass", draw != nullptr);
        ensure_equals("a draw entry per space", draw->mSpaces.size(), r.mEnc->mSpaceIds.size());
        ensure("the room fills", !draw->mFills.empty());
        bool outlined = false;
        for (const SpaceDraw& sd : draw->mSpaces)
        {
            outlined = outlined || !sd.mOutline.empty();
            ensure("full strength until a gather draws it", std::fabs(sd.mColor.mV[VALPHA] - 1.f) < 1e-4f);
        }
        ensure("the room's outline traced", outlined);
        F32 area = 0.f;
        for (const SpacesDraw::Fill& f : draw->mFills)
        {
            ensure("the fill names a space", f.mSpace < draw->mSpaces.size());
            ensure("the fill sits at a floor", f.mZ > -10.f && f.mZ < 30.f);
            area += f.mSide * f.mSide;
        }
        ensure("the fills cover the room's floor", area > 60.f);
    }

    template<> template<>
    void sknavrooms_object::test<33>()
    {
        set_test_name("a pass over again leaves the landed products and their draw untouched");
        // The rooms product's draw is only ever fresh if landed products are
        // replaced rather than rewritten: a pass that mutates the answers a
        // landed draw was traced from would leave the two disagreeing.
        Scene s;
        s.wall(2.f, 2.f, 14.f, 2.25f);
        s.wall(2.f, 25.75f, 6.f, 26.f);
        s.wall(2.f, 2.f, 2.25f, 4.f);
        s.wall(2.f, 5.f, 2.25f, 26.f);
        s.box(2.f, 4.f, 2.25f, 5.f, 2.2f, 3.f, false);
        s.wall(13.75f, 2.f, 14.f, 8.f);
        s.wall(6.f, 7.75f, 14.f, 8.f);
        s.wall(5.75f, 8.f, 6.f, 26.f);
        s.box(2.f, 2.f, 14.f, 8.f, 3.f, 3.25f);
        s.box(2.f, 8.f, 6.f, 26.f, 3.f, 3.25f);
        const Result first = run(s.input(1));
        ensure("the first draw", first.mRooms->mDraw != nullptr);
        const std::vector<SpacesDraw::Fill> fills = first.mRooms->mDraw->mFills;
        std::vector<size_t> runs;
        std::vector<PlanEdge> flat;
        for (const SpaceDraw& sd : first.mRooms->mDraw->mSpaces)
        {
            runs.push_back(sd.mOutline.size());
            flat.insert(flat.end(), sd.mOutline.begin(), sd.mOutline.end());
        }
        std::vector<U16> coarse;
        for (const TileEnclosure& t : first.mEnc->mTiles)
        {
            for (const CoarseGap& g : t.mCoarse.mGaps)
            {
                coarse.push_back(g.mSpace);
            }
        }

        // A second pass, reading the first's products as its previous, must
        // answer the same world without touching what it read.
        std::shared_ptr<EnclosureInput> enc_in = s.input(1);
        enc_in->mPrevious = first.mEnc;
        auto rooms_in = std::make_shared<RoomsInput>();
        rooms_in->mPrevious = first.mRooms;
        const Result second = run(enc_in, rooms_in);
        ensure("the second draw", second.mRooms->mDraw != nullptr);
        ensure("a different product, not a rewritten one", second.mRooms != first.mRooms);

        const std::vector<SpacesDraw::Fill>& now_fills = first.mRooms->mDraw->mFills;
        ensure("the landed fills keep", fills.size() == now_fills.size() &&
               (fills.empty() || std::memcmp(fills.data(), now_fills.data(), fills.size() * sizeof(SpacesDraw::Fill)) == 0));

        std::vector<PlanEdge> now_flat;
        bool kept = first.mRooms->mDraw->mSpaces.size() == runs.size();
        for (size_t k = 0; kept && k < first.mRooms->mDraw->mSpaces.size(); ++k)
        {
            const std::vector<PlanEdge>& outline = first.mRooms->mDraw->mSpaces[k].mOutline;
            kept = outline.size() == runs[k];
            now_flat.insert(now_flat.end(), outline.begin(), outline.end());
        }
        kept = kept && now_flat.size() == flat.size();
        for (size_t k = 0; kept && k < flat.size(); ++k)
        {
            kept = std::fabs(now_flat[k].mX0 - flat[k].mX0) < 1e-4f && std::fabs(now_flat[k].mY0 - flat[k].mY0) < 1e-4f &&
                   std::fabs(now_flat[k].mX1 - flat[k].mX1) < 1e-4f && std::fabs(now_flat[k].mY1 - flat[k].mY1) < 1e-4f &&
                   std::fabs(now_flat[k].mZ - flat[k].mZ) < 1e-4f;
        }
        ensure("the landed outlines keep", kept);

        size_t c = 0;
        bool coarse_kept = true;
        for (const TileEnclosure& t : first.mEnc->mTiles)
        {
            for (const CoarseGap& g : t.mCoarse.mGaps)
            {
                coarse_kept = coarse_kept && c < coarse.size() && coarse[c++] == g.mSpace;
            }
        }
        ensure("the landed coarse keeps", coarse_kept);
    }
}
