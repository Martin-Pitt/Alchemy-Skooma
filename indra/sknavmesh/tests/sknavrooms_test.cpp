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
}
