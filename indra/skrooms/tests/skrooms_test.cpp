/**
 * @file skrooms_test.cpp
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

#include <catch2/catch_test_macros.hpp>


#include "../skroomsspaces.h"
#include "skroomstestscene.h"

#include "llstring.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>

namespace
{
    using namespace skworld;
    using namespace skrooms_test;

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

    std::string info(const Result& r)
    {
        std::string o;
        for (const Space& sp : r.mRooms->mSpaces)
        {
            o += llformat(" [%u b%d a%.1f z%.1f S%.2f ceil%.1f lift%d uf%d p%zu c(%.1f,%.1f)]", r.index(&sp), (int)sp.mBucket, sp.mFloorArea, sp.mFloorMin, sp.mSMean,
                          sp.mCeilingMax, (int)sp.mLift, (int)sp.mUnderFlight, sp.mPortals.size(), sp.mCentre.mV[VX], sp.mCentre.mV[VY]);
        }
        return o;
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
        { INFO("no memo read without one"); REQUIRE((full.mRooms->mReused) == (0u)); }
        { INFO("the incremental pass is the full pass"); REQUIRE((dump(incremental) == dump(full))); }
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

    /// A walled, roofed building whose street door is set into a facade recess under a high overhang.
    Scene recessed(bool shaft)
    {
        Scene s;
        s.wall(8.f, 12.f, 20.f, 12.25f, 8.f);
        s.wall(21.f, 12.f, 28.f, 12.25f, 8.f);
        s.box(20.f, 12.f, 21.f, 12.25f, 2.2f, 8.f, false);
        if (shaft)
        {
            s.wall(8.f, 27.75f, 14.75f, 28.f, 8.f);
            s.wall(15.75f, 27.75f, 28.f, 28.f, 8.f);
            s.box(14.75f, 27.75f, 15.75f, 28.f, 2.2f, 8.f, false);
        }
        else
        {
            s.wall(8.f, 27.75f, 28.f, 28.f, 8.f);
        }
        s.wall(8.f, 12.f, 8.25f, 28.f, 8.f);
        s.wall(27.75f, 12.f, 28.f, 28.f, 8.f);
        s.box(8.f, 12.f, 28.f, 28.f, 8.f, 8.25f);
        s.wall(19.f, 10.f, 19.25f, 12.f, 8.f);
        s.wall(21.75f, 10.f, 22.f, 12.f, 8.f);
        s.box(19.f, 10.f, 22.f, 12.f, 7.f, 7.25f);
        if (shaft)
        {
            // A 2 m shaft in a core behind the hall's back wall, 12 m tall and roofed, behind a door.
            s.box(8.f, 28.f, 14.25f, 31.f, 0.f, 12.f, false);
            s.box(16.25f, 28.f, 22.f, 31.f, 0.f, 12.f, false);
            s.box(14.25f, 30.25f, 16.25f, 31.f, 0.f, 12.f, false);
            s.box(8.f, 27.75f, 22.f, 28.f, 8.f, 12.f, false);
            s.box(8.f, 28.f, 22.f, 31.f, 12.f, 12.25f);
        }
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

namespace
{

    TEST_CASE("skrooms: an L-shaped room stays one space", "[skrooms]")
    {
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
        { INFO("both arms have a space"); REQUIRE((a && b)); }
        { INFO("one space"); REQUIRE((a == b)); }
        { INFO("not the outdoors"); REQUIRE((a != r.at(24.f, 24.f))); }
        { INFO("INDOORS"); REQUIRE((a->mBucket == EnclosureBucket::INDOORS)); }
        { INFO("walled"); REQUIRE((a->mL > 0.6f)); }
    }

    TEST_CASE("skrooms: two doorways in one wall are two portals", "[skrooms]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 3.5f);
        s.wall(10.f, 4.5f, 10.25f, 7.f);
        s.wall(10.f, 8.f, 10.25f, 10.f);
        s.box(10.f, 3.5f, 10.25f, 4.5f, 2.2f, 3.f, false);
        s.box(10.f, 7.f, 10.25f, 8.f, 2.2f, 3.f, false);
        const Result r = run(s.input(1));
        const Space* a = r.at(6.f, 6.f);
        const Space* b = r.at(14.f, 6.f);
        { INFO("two spaces"); REQUIRE((a && b && a != b)); }
        const std::vector<const Portal*> p = r.between(a, b);
        { INFO("two portals"); REQUIRE((p.size()) == (2u)); }
        for (const Portal* q : p)
        {
            { INFO("openings"); REQUIRE((q->mKind == PortalKind::OPENING)); }
            { INFO("with a lintel"); REQUIRE((q->lintel())); }
            { INFO("walled both sides"); REQUIRE((q->walled())); }
            { INFO("a doorway wide"); REQUIRE((q->mWidth > 0.7f && q->mWidth < 1.4f)); }
        }
    }

    TEST_CASE("skrooms: a double door is one portal spanning both leaves", "[skrooms]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 7.f, 10.25f, 10.f);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(10.125f, 5.5f, 1.f, 0.f, 1.f));
        rooms->mDoors.push_back(door(10.125f, 6.5f, 1.f, 0.f, 1.f));
        const Result r = run(s.input(1), rooms);
        const Space* a = r.at(6.f, 6.f);
        const Space* b = r.at(14.f, 6.f);
        { INFO("two spaces"); REQUIRE((a && b && a != b)); }
        const std::vector<const Portal*> p = r.between(a, b);
        { INFO("one portal"); REQUIRE((p.size()) == (1u)); }
        { INFO("a door"); REQUIRE((p[0]->mKind == PortalKind::DOOR)); }
        { INFO("spanning both leaves"); REQUIRE((std::fabs(p[0]->mWidth - 2.f) < 0.1f)); }
    }

    TEST_CASE("skrooms: a door on the floor above does not cut this floor", "[skrooms]")
    {
        Scene s = twoRooms();
        // A passage 2 m wide with no lintel: nothing cuts it.
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 7.f, 10.25f, 10.f);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(10.125f, 6.f, 1.f, 0.f, 2.f, 3.25f));
        const Result r = run(s.input(1), rooms);
        const Space* a = r.at(6.f, 6.f);
        { INFO("a space"); REQUIRE((a != nullptr)); }
        { INFO("one space"); REQUIRE((a == r.at(14.f, 6.f))); }
        { INFO("no door portal"); REQUIRE((r.count(a, PortalKind::DOOR) == 0)); }
    }

    TEST_CASE("skrooms: a stairwell behind a door is its own space, linked to its flight by a stair portal", "[skrooms][.flights]")
    {
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
        { INFO("all found"); REQUIRE((landing && hall && stair)); }
        { INFO("the flight is its own space"); REQUIRE((stair->mFlight && stair != landing && stair != hall)); }
        { INFO("the landing is not the hall"); REQUIRE((landing != hall)); }
        { INFO("a door between them"); REQUIRE((r.between(landing, hall).size()) == (1u)); }
        { INFO("a stair portal onto the flight"); REQUIRE((!r.between(landing, stair).empty() && r.between(landing, stair)[0]->mKind == PortalKind::STAIR)); }
    }

    TEST_CASE("skrooms: an open stair is cut at each floor, with no opening at its top", "[skrooms][.flights]")
    {
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 1.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        const Space* ground = r.at(6.f, 6.f);
        const Space* deck = r.at(24.f, 4.f, 3.75f);
        const Space* stair = r.at(16.f, 2.f, 2.2f);
        { INFO("all found"); REQUIRE((ground && deck && stair)); }
        { INFO("three spaces"); REQUIRE((ground != deck && stair != ground && stair != deck)); }
        { INFO("the flight's"); REQUIRE((stair->mFlight)); }
        { INFO("stair portals at the bottom and the top"); REQUIRE((!r.between(stair, ground).empty() && !r.between(stair, deck).empty())); }
        for (const Portal* p : r.between(stair, deck))
        {
            { INFO("no opening at the top"); REQUIRE((p->mKind == PortalKind::STAIR)); }
        }
        for (const Portal* p : r.between(stair, ground))
        {
            { INFO("nor at the bottom"); REQUIRE((p->mKind == PortalKind::STAIR)); }
        }
        const std::vector<StairLink> links = stairLinks(*r.mRooms);
        { INFO("the open stair draws one link"); REQUIRE((links.size()) == (1u)); }
        const Portal& from = r.mRooms->mPortals[links[0].mFrom];
        const Portal& to = r.mRooms->mPortals[links[0].mTo];
        { INFO("the link runs bottom to top"); REQUIRE((from.mSill < to.mSill)); }
        { INFO("it joins the two ends it has"); REQUIRE(((&from == r.between(stair, ground)[0] || &from == r.between(stair, deck)[0]) &&
               (&to == r.between(stair, ground)[0] || &to == r.between(stair, deck)[0]))); }
    }

    TEST_CASE("skrooms: a roof on posts is SHELTERED however deep under it, for want of walls", "[skrooms]")
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
        const Result r = run(s.input(2, 2));
        const Space* under = r.at(32.f, 32.f);
        { INFO("a space"); REQUIRE((under != nullptr)); }
        { INFO("deep enough to read as INDOORS by e alone"); REQUIRE((under->mEMean > 0.75f)); }
        { INFO("hardly walled"); REQUIRE((under->mL < 0.4f)); }
        { INFO("no sky map, no cover"); REQUIRE((under->mCoverMean == 0.f)); }
        { INFO("SHELTERED"); REQUIRE((under->mBucket == EnclosureBucket::SHELTERED)); }
    }

    TEST_CASE("skrooms: space ids survive a 0.25 m nudge of everything", "[skrooms]")
    {
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
        { INFO("some spaces"); REQUIRE((big >= 3)); }
        { INFO("every one kept its id"); REQUIRE((kept) == (big)); }
    }

    TEST_CASE("skrooms: a lift shaft is a space of its own, INDOORS", "[skrooms]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 6.f, 2.2f, 3.f, false);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mLifts.emplace_back(LLVector3d(14.f, 6.f, -0.5f), LLVector3d(16.f, 8.f, 20.f));
        const Result r = run(s.input(1), rooms);
        const Space* lift = r.at(15.f, 7.f);
        const Space* room = r.at(12.f, 4.f);
        { INFO("found"); REQUIRE((lift && room)); }
        { INFO("its own"); REQUIRE((lift != room && lift->mLift)); }
        { INFO("INDOORS"); REQUIRE((lift->mBucket == EnclosureBucket::INDOORS)); }
    }

    TEST_CASE("skrooms: a hall across a seam between blocks of tiles stays one space, and keeps it when one side is rebuilt", "[skrooms]")
    {
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
        { INFO("the other hall is one space"); REQUIRE((other && other == r.at(31.f, 23.f))); }
        const Space* hall = r.at(70.f, 8.f);
        { INFO("found"); REQUIRE((hall != nullptr)); }
        { INFO("one space both sides of the seam"); REQUIRE((hall == r.at(63.f, 8.f))); }
        { INFO("INDOORS"); REQUIRE((hall->mBucket == EnclosureBucket::INDOORS)); }

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
        { INFO("the moved tile and at most a ring tile cut again"); REQUIRE((again.mRooms->mDirtyTiles >= 1 && again.mRooms->mDirtyTiles <= 2)); }
        const Space* still = again.at(70.f, 8.f);
        { INFO("still one space"); REQUIRE((still && still == again.at(63.f, 8.f))); }
        { INFO("with its id"); REQUIRE((still->mId) == (hall->mId)); }
        const Space* kept = again.at(40.f, 23.f);
        { INFO("the other hall untouched"); REQUIRE((kept && kept == again.at(31.f, 23.f) && kept->mId == other->mId)); }

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
            { INFO("one space each time"); REQUIRE((h && h == round.at(63.f, 8.f))); }
            { INFO("the same id each time"); REQUIRE((h->mId) == (hall->mId)); }
            { INFO("no portal doubled"); REQUIRE((h->mPortals.size()) == (portals)); }
            const Space* o = round.at(40.f, 23.f);
            { INFO("the other hall holds"); REQUIRE((o && o->mId == other->mId)); }
            last = round.mRooms;
        }
    }

    TEST_CASE("skrooms: a switchback stair with a landing between its flights leaves neither flight an island", "[skrooms][.flights]")
    {
        Scene s;
        s.flight(8.f, 14.f, 1.f, 3.f, 0.f, 1.75f);
        s.box(14.f, 1.f, 17.f, 6.f, 0.f, 1.75f);                // the landing
        s.wall(8.f, 3.f, 14.f, 4.f, 5.f);
        s.flight(14.f, 8.f, 4.f, 6.f, 1.75f, 3.25f);
        s.box(4.f, 4.f, 8.f, 8.f, 3.f, 3.25f);                  // the floor it climbs to
        const Result r = run(s.input(1));
        const Space* landing = r.at(15.5f, 3.5f, 2.2f);
        { INFO("a landing"); REQUIRE((landing != nullptr && !landing->mFlight)); }
        { INFO("every flight linked at both ends"); REQUIRE((r.flightsLinked())); }
    }

    TEST_CASE("skrooms: a flight wider than a stair portal's run still meets its floors", "[skrooms][.flights]")
    {
        Scene s;
        s.box(20.f, 0.f, 28.f, 8.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 0.f, 8.f, 0.f, 3.25f);
        const Result r = run(s.input(1));
        { INFO("linked"); REQUIRE((r.flightsLinked())); }
        { INFO("to the deck"); REQUIRE((!r.between(r.at(16.f, 4.f, 2.9f), r.at(24.f, 4.f, 3.75f)).empty())); }
    }

    TEST_CASE("skrooms: a flight across a tile seam meets both its floors", "[skrooms][.flights]")
    {
        Scene s;
        s.box(36.f, 0.f, 44.f, 8.f, 3.f, 3.25f);
        s.flight(28.f, 36.f, 1.f, 3.f, 0.f, 3.25f);
        const Result r = run(s.input(2));
        { INFO("linked"); REQUIRE((r.flightsLinked())); }
        { INFO("to the deck"); REQUIRE((!r.between(r.at(34.f, 2.f, 2.8f), r.at(40.f, 4.f, 3.75f)).empty())); }
        { INFO("to the ground"); REQUIRE((!r.between(r.at(29.f, 2.f, 0.8f), r.at(20.f, 4.f)).empty())); }
    }

    TEST_CASE("skrooms: a floor that meets a flight only along its side is still linked to it", "[skrooms][.flights]")
    {
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
        { INFO("found"); REQUIRE((gallery && stair && stair->mFlight && gallery != stair)); }
        const std::vector<const Portal*> p = r.between(gallery, stair);
        { INFO("linked by a stair portal"); REQUIRE((!p.empty() && p[0]->mKind == PortalKind::STAIR)); }
    }

    TEST_CASE("skrooms: ground under a small platform far overhead, beside a tall wall, is OUTDOORS though the wall hides much sky", "[skrooms]")
    {
        Scene s;
        s.box(13.f, 13.f, 19.f, 19.f, 30.f, 30.5f);
        s.box(20.f, 0.f, 21.f, 32.f, 0.f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(16.1f, 16.1f, 0.5f), e))); }
        { INFO("the wall hides much of the sky"); REQUIRE((e.mS < 0.8f)); }
        { INFO("OUTDOORS at the gap"); REQUIRE((e.mBucket == EnclosureBucket::OUTDOORS)); }
        { INFO("OUTDOORS as a space"); REQUIRE((r.at(16.1f, 16.1f)->mBucket == EnclosureBucket::OUTDOORS)); }
    }

    TEST_CASE("skrooms: ground under a large low roof has almost no sky", "[skrooms]")
    {
        Scene s;
        s.box(2.f, 2.f, 62.f, 62.f, 5.f, 5.25f);
        auto in = s.input(2, 2);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e))); }
        { INFO("the sky is hidden"); REQUIRE((e.mS < 0.05f)); }
        { INFO("INDOORS by e"); REQUIRE((e.mE > 0.75f)); }
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

    TEST_CASE("skrooms: under a roof on posts, sky map and all, is SHELTERED", "[skrooms]")
    {
        bool walls[4] = { false, false, false, false };
        const Result r = roofOnPosts(walls);
        const Space* under = r.at(32.f, 32.f);
        { INFO("a space"); REQUIRE((under != nullptr)); }
        { INFO("deep enough to read as INDOORS by e alone"); REQUIRE((under->mEMean > 0.75f)); }
        { INFO("SHELTERED"); REQUIRE((under->mBucket == EnclosureBucket::SHELTERED)); }
    }

    TEST_CASE("skrooms: a roof with one wall and three open sides is SHELTERED", "[skrooms]")
    {
        bool walls[4] = { true, false, false, false };
        const Result r = roofOnPosts(walls);
        const Space* under = r.at(32.f, 32.f);
        { INFO("a space"); REQUIRE((under != nullptr)); }
        { INFO("deep enough to read as INDOORS by e alone"); REQUIRE((under->mEMean > 0.75f)); }
        { INFO("a quarter walled"); REQUIRE((under->mL > 0.15f && under->mL < 0.45f)); }
        { INFO("SHELTERED"); REQUIRE((under->mBucket == EnclosureBucket::SHELTERED)); }
    }

    TEST_CASE("skrooms: the walls of a roofless well hide the low sky, and a dense view reads as the map does", "[skrooms]")
    {
        Scene s;
        s.box(12.f, 12.f, 20.f, 13.f, 0.f, 10.f, false);
        s.box(12.f, 19.f, 20.f, 20.f, 0.f, 10.f, false);
        s.box(12.f, 12.f, 13.f, 20.f, 0.f, 10.f, false);
        s.box(19.f, 12.f, 20.f, 20.f, 0.f, 10.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const LLVector3d mid(16.5, 16.5, 2.0);
        const F32 walled = in->mSky->coverAbove(mid, 50.f);
        { INFO("most of the sky is behind the walls"); REQUIRE((walled > 0.6f && walled < 0.95f)); }
        { INFO("outside the well the sky is open"); REQUIRE((in->mSky->coverAbove(LLVector3d(4.5, 4.5, 2.0), 50.f) < 0.1f)); }
        Scene pole;
        pole.box(16.f, 3.f, 17.f, 4.f, 0.f, 36.f, false);
        auto pin = pole.input(1);
        pole.addSky(*pin);
        { INFO("a thin pole 12 m off hides a sliver, not its wedge"); REQUIRE((pin->mSky->coverAbove(LLVector3d(4.5, 4.5, 2.0), 50.f) < 0.04f)); }
        const SkyView view(*in->mSky, -40.0, -40.0, 72.0, 72.0);
        { INFO("the view agrees"); REQUIRE((std::fabs(view.coverAbove(mid, 50.f) - walled) < 1e-5f)); }
    }

    TEST_CASE("skrooms: a street between towers stays OUTDOORS with the sky map, though the towers hide most of its sky", "[skrooms]")
    {
        Scene s;
        s.box(0.f, 0.f, 12.f, 32.f, 0.f, 36.f, false);
        s.box(20.f, 0.f, 32.f, 32.f, 0.f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(16.f, 16.f, 0.5f), e))); }
        { INFO("the towers hide most of the sky"); REQUIRE((e.mS < 0.5f)); }
        { INFO("OUTDOORS at the gap"); REQUIRE((e.mBucket == EnclosureBucket::OUTDOORS)); }
        const Space* street = r.at(16.f, 16.f);
        { INFO("a space"); REQUIRE((street != nullptr)); }
        { INFO("nothing roofs it"); REQUIRE((street->mCoverMean < 0.05f)); }
        { INFO("OUTDOORS as a space"); REQUIRE((street->mBucket == EnclosureBucket::OUTDOORS)); }
    }

    TEST_CASE("skrooms: two yards on a terrace join through a doorway in the tall wall between them, its shadow on the sky no bar", "[skrooms]")
    {
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
        { INFO("spaces"); REQUIRE((west && east)); }
        Enclosure e;
        { INFO("walls hide much of the sky by them"); REQUIRE((r.mEnc->enclosureAt(LLVector3(10.5f, 8.f, 0.5f), e) && e.mS < 0.9f)); }
        { INFO("one space"); REQUIRE((west == east)); }
        { INFO("OUTDOORS"); REQUIRE((west->mBucket == EnclosureBucket::OUTDOORS)); }
    }

    TEST_CASE("skrooms: the spaces pass grows with the tile count: four times the field costs about four times as much", "[skrooms]")
    {
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
        { INFO("many seeds, one space"); REQUIRE((small.second->mSeeds > 64u * side * side && large.second->mSpaces.size() == 1)); }
        const F64 ratio = large.first / std::max(small.first, 1e-3);
        { INFO("4x the tiles within 6x the time, not " + std::to_string(ratio) + "x (" + std::to_string(small.first) + " ms, " +
                   std::to_string(large.first) + " ms)"); REQUIRE((ratio < 6.0)); }
    }

    TEST_CASE("skrooms: one tile rebuilt among scattered walls is cut again with its ring, not with the mesh", "[skrooms]")
    {
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
        { INFO("every tile cut the first time"); REQUIRE((first.mRooms->mDirtyTiles) == (25u)); }
        const Result again = field(first.mRooms, mid);
        { INFO("the moved tile and at most its four neighbours cut again"); REQUIRE((again.mRooms->mDirtyTiles >= 1 && again.mRooms->mDirtyTiles <= 5)); }
        { INFO("the same spaces"); REQUIRE((again.mRooms->mSpaces.size()) == (first.mRooms->mSpaces.size())); }
    }

    TEST_CASE("skrooms: a roof never joins the room under it, thin slab or thick, and every space's bounds hold its gaps", "[skrooms]")
    {
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
        { INFO("rooms found"); REQUIRE((west && tower)); }
        { INFO("the thin roof is not the room's"); REQUIRE((r.at(6.f, 6.f, 3.5f) != west)); }
        const Space* roof = r.at(16.f, 6.f, 3.5f);
        { INFO("a roof no one walks onto is a roof of its own, over the block and the rooms"); REQUIRE((roof && roof->mRoof && r.at(20.f, 6.f, 3.5f) == roof)); }
        { INFO("the tower's top is not the room 4 m below"); REQUIRE((r.at(27.f, 5.f, 7.5f) != tower)); }
        { INFO("no gap outside its space's bounds"); REQUIRE((r.outOfBounds()) == (0u)); }
    }

    TEST_CASE("skrooms: a flight walled in on every side, meeting no floor, is no flight space, so none is an island", "[skrooms]")
    {
        Scene s;
        s.flight(8.f, 16.f, 2.f, 4.f, 2.f, 3.f);
        s.wall(7.75f, 1.75f, 16.25f, 2.f, 5.f);
        s.wall(7.75f, 4.f, 16.25f, 4.25f, 5.f);
        s.wall(7.75f, 1.75f, 8.f, 4.25f, 5.f);
        s.wall(16.f, 1.75f, 16.25f, 4.25f, 5.f);
        const Result r = run(s.input(1));
        { INFO("its treads have a space"); REQUIRE((r.at(12.f, 3.f, 3.f) != nullptr)); }
        for (const Space& sp : r.mRooms->mSpaces)
        {
            { INFO("every flight space has a stair portal"); REQUIRE((!sp.mFlight || r.count(&sp, PortalKind::STAIR) > 0)); }
        }
    }

    TEST_CASE("skrooms: a pass reading the last one's memo is the pass that reads none: a doorway appears, a wall goes, a lintel goes, nothing moves", "[skrooms]")
    {
        const std::vector<F32> partitions = { 30.f, 40.f, 50.f, 60.f, 70.f };
        TileKey moved;
        moved.mX = 1;
        moved.mY = 0;
        const std::vector<DoorEntity> none;
        U32 reused = 0;
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms(partitions, { 40.f, 50.f }), 3, 1, moved, none, none, reused);
        { INFO("a doorway appears: the rest reused"); REQUIRE((reused > 0)); }
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms({ 30.f, -1.f, 50.f, 60.f, 70.f }, {}), 3, 1, moved, none, none, reused);
        { INFO("a wall goes: the rest reused"); REQUIRE((reused > 0)); }
        incrementalIsFull(rowOfRooms(partitions, {}), rowOfRooms(partitions, {}, 55.f), 3, 1, moved, none, none, reused);
        { INFO("a lintel goes: the rest reused"); REQUIRE((reused > 0)); }
        incrementalIsFull(rowOfRooms(partitions, { 40.f }), rowOfRooms(partitions, { 40.f }), 3, 1, moved, none, none, reused);
        { INFO("nothing moves: the rest reused"); REQUIRE((reused > 0)); }
        // A door the census finds changes what every cut near a door reads: nothing is reused.
        incrementalIsFull(rowOfRooms(partitions, {}), rowOfRooms(partitions, {}), 3, 1, moved, none, { door(45.5f, 13.875f, 0.f, 1.f, 1.f) }, reused);
        { INFO("a door appears: nothing reused"); REQUIRE((reused) == (0u)); }
    }

    TEST_CASE("skrooms: a memo is read only where its keys hold: a region's root moves across the mesh, indoorness moves in a tile whose air did not", "[skrooms]")
    {
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
        { INFO("a root moves: the rest reused"); REQUIRE((reused > 0)); }
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
        { INFO("a ceiling moves: the rest reused"); REQUIRE((reused > 0)); }
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
        { INFO("indoorness moves: the rest reused"); REQUIRE((reused > 0)); }
    }

    TEST_CASE("skrooms: replay a mesh's air through both passes, when a file is named", "[skrooms]")
    {
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
        { INFO("read a replay file"); REQUIRE((readReplay(path, enc_in, rooms, points))); }
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

    /// A roofed 8 x 8 m room with a recess off its north wall, through a
    /// slot under a lintel: the recess's inside is 2.5 m wide and depth deep.
    Scene roomWithRecess(F32 slot, F32 depth)
    {
        Scene s;
        s.wall(2.f, 2.f, 10.f, 2.25f);
        s.wall(2.f, 2.f, 2.25f, 10.f);
        s.wall(9.75f, 2.f, 10.f, 10.f);
        const F32 x0 = 6.f - slot * 0.5f, x1 = 6.f + slot * 0.5f;
        s.wall(2.f, 9.75f, x0, 10.f);
        s.wall(x1, 9.75f, 10.f, 10.f);
        s.box(x0, 9.75f, x1, 10.f, 2.2f, 3.f, false);
        s.wall(4.5f, 10.f, 4.75f, 10.f + depth + 0.25f);
        s.wall(7.25f, 10.f, 7.5f, 10.f + depth + 0.25f);
        s.wall(4.5f, 10.f + depth, 7.5f, 10.f + depth + 0.25f);
        s.box(2.f, 2.f, 10.f, 10.f + depth + 0.25f, 3.f, 3.25f);
        return s;
    }

    TEST_CASE("skrooms: a recess smaller than an avatar's floor joins its room, though the doorway into it has a lintel", "[skrooms]")
    {
        const Result r = run(roomWithRecess(1.f, 0.625f).input(1));
        const Space* room = r.at(6.f, 6.f);
        { INFO("the room has a space"); REQUIRE((room != nullptr)); }
        { INFO("the recess is the room's"); REQUIRE((r.at(6.f, 10.3f) == room)); }
        for (const Space& sp : r.mRooms->mSpaces)
        {
            { INFO("no space under an avatar's floor"); REQUIRE((sp.mFloorArea >= SpaceParams().mAvatarAreaM2)); }
        }
        // A closet as wide but deeper is a space of its own behind its doorway.
        const Result deep = run(roomWithRecess(1.f, 3.f).input(1));
        const Space* closet = deep.at(6.f, 11.5f);
        { INFO("the closet is its own space"); REQUIRE((closet && closet != deep.at(6.f, 6.f))); }
        { INFO("behind one opening"); REQUIRE((deep.between(closet, deep.at(6.f, 6.f)).size()) == (1u)); }
    }

    TEST_CASE("skrooms: a slot narrower than an avatar passes is no portal, and no way through to merge across", "[skrooms]")
    {
        const F32 passage = 2.f * SpaceParams().mAgentRadiusM;
        const Result r = run(roomWithRecess(0.5f, 3.f).input(1));
        const Space* room = r.at(6.f, 6.f);
        const Space* closet = r.at(6.f, 11.5f);
        { INFO("the room and the closet have spaces"); REQUIRE((room && closet)); }
        { INFO("the closet behind the slot is not the room"); REQUIRE((closet != room)); }
        { INFO("and no portal joins them"); REQUIRE((r.between(room, closet).empty())); }
        for (const Portal& q : r.mRooms->mPortals)
        {
            { INFO("no portal narrower than a passage"); REQUIRE((q.mWidth >= passage)); }
        }
        const Result wide = run(roomWithRecess(passage + 0.15f, 3.f).input(1));
        { INFO("a doorway a passage wide parts them with a portal"); REQUIRE((wide.between(wide.at(6.f, 6.f), wide.at(6.f, 11.5f)).size() == 1u)); }
    }

    TEST_CASE("skrooms: an opening spans its aperture jamb to jamb, under its lintel, not the boundary the watershed drew", "[skrooms]")
    {
        // A 3 m doorway beside a wall stub that bends the watershed's boundary.
        Scene s;
        s.wall(2.f, 2.f, 18.f, 2.25f);
        s.wall(2.f, 9.75f, 18.f, 10.f);
        s.wall(2.f, 2.f, 2.25f, 10.f);
        s.wall(17.75f, 2.f, 18.f, 10.f);
        s.wall(10.f, 2.f, 10.25f, 3.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.wall(7.f, 2.75f, 10.f, 3.f);
        s.box(10.f, 3.f, 10.25f, 6.f, 2.2f, 3.f, false);
        s.box(2.f, 2.f, 18.f, 10.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        const Space* a = r.at(6.f, 6.f);
        const Space* b = r.at(14.f, 6.f);
        { INFO("two rooms"); REQUIRE((a && b && a != b)); }
        const std::vector<const Portal*> p = r.between(a, b);
        { INFO("one opening"); REQUIRE((p.size()) == (1u)); }
        const Portal& q = *p[0];
        { INFO("an opening"); REQUIRE((q.mKind == PortalKind::OPENING)); }
        { INFO("its width is the clear width"); REQUIRE((std::fabs(q.mWidth - 3.f) < 0.02f)); }
        { INFO("its ends on the jambs"); REQUIRE((std::fabs(std::min(q.mA.mV[VY], q.mB.mV[VY]) - 3.f) < 0.02f && std::fabs(std::max(q.mA.mV[VY], q.mB.mV[VY]) - 6.f) < 0.02f)); }
        { INFO("its centre midway between them"); REQUIRE((std::fabs(q.mCentre.mV[VY] - 4.5f) < 0.02f)); }
        { INFO("in the wall"); REQUIRE((q.mCentre.mV[VX] > 9.9f && q.mCentre.mV[VX] < 10.35f)); }
        { INFO("square to it"); REQUIRE((std::fabs(q.mNormal.mV[VX]) > 0.99f)); }
        { INFO("its head the lintel"); REQUIRE((std::fabs(q.mHead - 2.2f) < 0.15f)); }
    }

    TEST_CASE("skrooms: a causeway between two pavilions, under a beam, is an opening walled on neither side: floor that ends is no jamb", "[skrooms]")
    {
        // Roofed both sides, so the outdoor rule leaves the cut alone; nothing below the causeway.
        Scene s;
        s.mGround[2] = s.mGround[0];
        s.box(2.f, 2.f, 12.f, 14.f, 1.75f, 2.f);
        s.box(12.f, 7.f, 14.f, 9.f, 1.75f, 2.f);
        s.box(14.f, 2.f, 24.f, 14.f, 1.75f, 2.f);
        s.box(2.f, 2.f, 12.f, 14.f, 4.5f, 4.75f, false);
        s.box(14.f, 2.f, 24.f, 14.f, 4.5f, 4.75f, false);
        s.box(12.75f, 7.f, 13.25f, 9.f, 3.9f, 4.2f, false);
        const Result r = run(s.input(1));
        const Space* west = r.at(5.f, 8.f, 2.5f);
        const Space* east = r.at(20.f, 8.f, 2.5f);
        { INFO("two pavilions"); REQUIRE((west && east && west != east)); }
        const std::vector<const Portal*> p = r.between(west, east);
        { INFO("one cut on the causeway"); REQUIRE((p.size()) == (1u)); }
        { INFO("an opening, for its lintel"); REQUIRE((p[0]->mKind == PortalKind::OPENING && p[0]->lintel())); }
        { INFO("walled on neither side"); REQUIRE((!p[0]->walled())); }
    }

    /// Two roofed rooms either side of a partition with a 2 m doorway under
    /// a lintel, both opening without one onto a corridor along their north
    /// side: the rooms meet through the doorway and around through the corridor.
    Scene loopedRooms(F32 lintel_z = 2.2f)
    {
        Scene s;
        s.wall(2.f, 2.f, 18.f, 2.25f);
        s.wall(2.f, 14.f, 18.f, 14.25f);
        s.wall(2.f, 2.f, 2.25f, 14.25f);
        s.wall(17.75f, 2.f, 18.f, 14.25f);
        s.wall(2.f, 10.f, 4.f, 10.25f);
        s.wall(6.f, 10.f, 14.f, 10.25f);
        s.wall(16.f, 10.f, 18.f, 10.25f);
        s.wall(10.f, 2.25f, 10.25f, 5.f);
        s.wall(10.f, 7.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 7.f, lintel_z, 3.f, false);
        s.box(2.f, 2.f, 18.f, 14.25f, 3.f, 3.25f);
        return s;
    }

    /// The spaces pass again over the same air, reading the last pass.
    Result rerun(const Scene& s, const Result& first)
    {
        std::shared_ptr<EnclosureInput> in = s.input(1);
        for (EnclosureInput::Tile& t : in->mTiles)
        {
            if (const RoomsTile* was = first.mRooms->tile(t.mKey))
            {
                t.mAir = was->mAir;
            }
        }
        EnclosurePass pass(in);
        while (!pass.step(1u << 30))
        {
        }
        Result r;
        r.mEnc = pass.takeProduct();
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mPrevious = first.mRooms;
        r.mRooms = computeSpaces(rooms, r.mEnc);
        return r;
    }

    TEST_CASE("skrooms: a doorway under a lintel parts its rooms though they meet again around it, and stays cut on the next pass", "[skrooms]")
    {
        const Scene s = loopedRooms();
        const Result r = run(s.input(1));
        const Space* west = r.at(6.f, 6.f);
        const Space* east = r.at(14.f, 6.f);
        { INFO("two rooms"); REQUIRE((west && east && west != east)); }
        { INFO("one aperture cut"); REQUIRE((r.mRooms->mApart.size()) == (1u)); }
        bool doorway = false;
        for (const Portal* q : r.between(west, east))
        {
            doorway = doorway || (q->mKind == PortalKind::OPENING && q->lintel() && std::fabs(q->mCentre.mV[VY] - 6.f) < 0.1f &&
                                  q->mCentre.mV[VX] > 9.9f && q->mCentre.mV[VX] < 10.35f && std::fabs(q->mWidth - 2.f) < 0.05f);
        }
        { INFO("an opening on the doorway, jamb to jamb"); REQUIRE((doorway)); }
        // A pass that cuts no tile again keeps the cut.
        const Result again = rerun(s, r);
        { INFO("nothing cut again"); REQUIRE((again.mRooms->mDirtyTiles) == (0u)); }
        { INFO("still two rooms"); REQUIRE((again.at(6.f, 6.f) && again.at(6.f, 6.f) != again.at(14.f, 6.f))); }
        { INFO("the cut kept"); REQUIRE((again.mRooms->mApart.size()) == (1u)); }
    }

    TEST_CASE("skrooms: a beam across a corridor is no doorway: its walls run on along both sides", "[skrooms]")
    {
        Scene s;
        s.wall(2.f, 2.f, 20.f, 2.25f);
        s.wall(2.f, 4.f, 20.f, 4.25f);
        s.wall(2.f, 2.f, 2.25f, 4.25f);
        s.wall(19.75f, 2.f, 20.f, 4.25f);
        s.box(10.f, 2.25f, 10.25f, 4.f, 2.2f, 3.f, false);
        s.box(2.f, 2.f, 20.f, 4.25f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        { INFO("no aperture cut"); REQUIRE((r.mRooms->mApart.empty())); }
        { INFO("one corridor"); REQUIRE((r.at(5.f, 3.f) && r.at(5.f, 3.f) == r.at(15.f, 3.f))); }
    }

    TEST_CASE("skrooms: a beam from a wall's end to a post is no doorway: a jamb's wall runs on past it", "[skrooms]")
    {
        Scene s;
        s.wall(2.f, 2.f, 18.f, 2.25f);
        s.wall(2.f, 9.75f, 18.f, 10.f);
        s.wall(2.f, 2.f, 2.25f, 10.f);
        s.wall(17.75f, 2.f, 18.f, 10.f);
        s.wall(10.f, 2.25f, 10.25f, 5.f);
        s.wall(9.9f, 7.f, 10.35f, 7.4f);
        s.box(10.f, 5.f, 10.25f, 7.f, 2.2f, 3.f, false);
        s.box(2.f, 2.f, 18.f, 10.f, 3.f, 3.25f);
        const Result r = run(s.input(1));
        { INFO("no aperture cut"); REQUIRE((r.mRooms->mApart.empty())); }
        // The same gap between two walls is one.
        s.wall(10.f, 7.4f, 10.25f, 9.75f);
        const Result walled = run(s.input(1));
        { INFO("a doorway's cut"); REQUIRE((walled.mRooms->mApart.size()) == (1u)); }
    }

    TEST_CASE("skrooms: an arch between open yards, or a lintel higher than a door's, cuts nothing", "[skrooms]")
    {
        // The looped rooms unroofed: open sky either side of the doorway.
        Scene yards = loopedRooms();
        yards.mSolids.pop_back();
        const Result open = run(yards.input(1));
        { INFO("no cut under open sky"); REQUIRE((open.mRooms->mApart.empty())); }
        // Roofed, with the lintel 3.75 m up under a 4.5 m ceiling.
        Scene hall = loopedRooms(3.75f);
        hall.mSolids.pop_back();
        for (Solid& w : hall.mSolids)
        {
            w.mZ1 = std::max(w.mZ1, 4.5f);
        }
        hall.box(2.f, 2.f, 18.f, 14.25f, 4.5f, 4.75f);
        const Result high = run(hall.input(1));
        { INFO("no cut under a lintel that high"); REQUIRE((high.mRooms->mApart.empty())); }
        { INFO("the door-high one is cut"); REQUIRE((run(loopedRooms().input(1)).mRooms->mApart.size() == 1u)); }
    }

    TEST_CASE("skrooms: a ceiling sloping across a doorway is no lintel to cut at", "[skrooms]")
    {
        Scene s = loopedRooms();
        s.mSolids.erase(std::find_if(s.mSolids.begin(), s.mSolids.end(), [](const Solid& w) { return w.mZ0 == 2.2f; }));
        for (S32 k = 0; k < 4; ++k)
        {
            s.box(10.f, 5.f + 0.5f * k, 10.25f, 5.5f + 0.5f * k, 2.f + 0.2f * k, 3.f, false);
        }
        const Result r = run(s.input(1));
        { INFO("no aperture cut"); REQUIRE((r.mRooms->mApart.empty())); }
    }

    TEST_CASE("skrooms: no stair portal is wider than a staircase, though a deck-wide flight meets its floors along 14 m", "[skrooms]")
    {
        Scene s;
        s.box(20.f, 0.f, 28.f, 16.f, 3.f, 3.25f);
        s.flight(12.f, 20.f, 1.f, 15.f, 3.25f);
        const Result r = run(s.input(1));
        for (const Portal& q : r.mRooms->mPortals)
        {
            { INFO("no stair portal wider than a staircase"); REQUIRE((q.mKind != PortalKind::STAIR || q.mWidth <= STAIR_MAX_WIDTH_M)); }
        }
    }

    /// The looped rooms four times over four tiles, once across the corner
    /// they share, each moved off the cell grid so the doorways' jambs and
    /// middles fall inside cells.
    Scene town()
    {
        Scene s;
        for (const LLVector2& at : { LLVector2(22.03f, 24.07f), LLVector2(1.11f, 44.29f), LLVector2(40.53f, 1.71f), LLVector2(0.37f, 0.93f) })
        {
            for (Solid b : loopedRooms().mSolids)
            {
                b.mX0 += at.mV[VX];
                b.mX1 += at.mV[VX];
                b.mY0 += at.mV[VY];
                b.mY1 += at.mV[VY];
                s.mSolids.push_back(b);
            }
        }
        return s;
    }

    TEST_CASE("skrooms: doorways cut from below find, reading their cells in bulk, what they find reading every sample", "[skrooms]")
    {
        for (const Scene& s : { loopedRooms(), town() })
        {
            const S32 tiles = s.mSolids.size() > 11 ? 2 : 1;
            auto framed = std::make_shared<RoomsInput>(), sampled = std::make_shared<RoomsInput>();
            sampled->mFrameApertures = false;
            const Result a = run(s.input(tiles, tiles), framed);
            const Result b = run(s.input(tiles, tiles), sampled);
            { INFO("some doorway cut from below"); REQUIRE((!b.mRooms->mApart.empty())); }
            { INFO("as many cuts"); REQUIRE((a.mRooms->mApart.size()) == (b.mRooms->mApart.size())); }
            for (size_t i = 0; i < a.mRooms->mApart.size(); ++i)
            {
                const SeedLink &p = a.mRooms->mApart[i], &q = b.mRooms->mApart[i];
                { INFO("the same cut"); REQUIRE((p.mA == q.mA && p.mB == q.mB && p.mSeedA == q.mSeedA && p.mSeedB == q.mSeedB)); }
            }
            { INFO("the same spaces and portals, to the bit"); REQUIRE((dump(a) == dump(b))); }
        }
    }

    TEST_CASE("skrooms: a flight's links run from its foot to each exit; a lone end makes none", "[skrooms]")
    {
        RoomsProduct product;
        product.mSpaces.resize(1);
        product.mSpaces[0].mFlight = true;
        Portal foot, exit_a, exit_b, door;
        foot.mKind = PortalKind::STAIR;
        foot.mSill = 1.f;
        exit_a.mKind = PortalKind::STAIR;
        exit_a.mSill = 4.f;
        exit_b.mKind = PortalKind::STAIR;
        exit_b.mSill = 7.f;
        door.mKind = PortalKind::DOOR;

        product.mPortals = { foot, exit_a, door };
        product.mSpaces[0].mPortals = { 0, 1, 2 };
        std::vector<StairLink> links = stairLinks(product);
        { INFO("foot to exit, the door ignored"); REQUIRE((links.size()) == (1u)); }
        { INFO("from the foot"); REQUIRE((links[0].mFrom) == (0u)); }
        { INFO("to the exit"); REQUIRE((links[0].mTo) == (1u)); }

        product.mPortals = { foot };
        product.mSpaces[0].mPortals = { 0 };
        { INFO("no link for a lone end"); REQUIRE((stairLinks(product).empty())); }

        product.mPortals = { exit_b, foot, exit_a };
        product.mSpaces[0].mPortals = { 0, 1, 2 };
        links = stairLinks(product);
        { INFO("a spiral links the foot to both exits"); REQUIRE((links.size()) == (2u)); }
        for (const StairLink& l : links)
        {
            { INFO("every link starts at the lowest sill"); REQUIRE((product.mPortals[l.mFrom].mSill) == (1.f)); }
            { INFO("and climbs"); REQUIRE((product.mPortals[l.mTo].mSill > product.mPortals[l.mFrom].mSill)); }
        }

        // Ends further apart in plan than a staircase's reach stay single:
        // a far cut is no end of this flight.
        Portal away = exit_a;
        away.mCentre.set(30.f, 0.f, 0.f);
        product.mPortals = { foot, away };
        product.mSpaces[0].mPortals = { 0, 1 };
        { INFO("nothing out of reach pairs"); REQUIRE((stairLinks(product).empty())); }

        // A space that lost its flight flag keeps no links: its portals belong
        // to no staircase.
        product.mSpaces[0].mFlight = false;
        { INFO("no links without the flight flag"); REQUIRE((stairLinks(product).empty())); }
    }

    TEST_CASE("skrooms: an L of cells traces the six runs of its outline", "[skrooms]")
    {
        const std::vector<PlanCell> cells = { { 0.f, 0.f, 3.f }, { 1.f, 0.f, 3.f }, { 2.f, 0.f, 3.f }, { 0.f, 1.f, 3.f },
                                              { 0.f, 0.f, 5.f } };   // a repeated column keeps its lowest floor
        const std::vector<PlanEdge> edges = tracePlan(cells, 1.f);
        { INFO("six runs"); REQUIRE((edges.size()) == (6u)); }
        for (const PlanEdge& e : edges)
        {
            { INFO("all at the lowest floor"); REQUIRE((std::fabs(e.mZ - 3.f) < 1e-4f)); }
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
        { INFO("the bottom runs on"); REQUIRE((has(0.f, 0.f, 3.f, 0.f))); }
        { INFO("the right stands"); REQUIRE((has(3.f, 0.f, 3.f, 1.f))); }
        { INFO("the notch's far side runs back"); REQUIRE((has(3.f, 1.f, 1.f, 1.f))); }
        { INFO("the notch's inner side stands"); REQUIRE((has(1.f, 1.f, 1.f, 2.f))); }
        { INFO("the top runs back"); REQUIRE((has(1.f, 2.f, 0.f, 2.f))); }
        { INFO("the left runs down to the start"); REQUIRE((has(0.f, 2.f, 0.f, 0.f))); }
    }

TEST_CASE("skrooms: a plan with a hole, a slope and a pinch traces them apart", "[skrooms]")
    {
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
        { INFO("the ring's four, the hole's four, the slope's eight, the pinch's eight"); REQUIRE((edges.size()) == (24u)); }
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
        { INFO("the ring's bottom is one run"); REQUIRE((has(0.f, 0.f, 3.f, 0.f, 3.f))); }
        { INFO("the hole's edge runs with the ring on its left"); REQUIRE((has(2.f, 1.f, 1.f, 1.f, 3.f))); }
        { INFO("the slope steps at each floor"); REQUIRE((has(6.f, 6.f, 7.f, 6.f, 3.2f))); }
        { INFO("the pinch's second cell stands alone"); REQUIRE((has(11.f, 11.f, 12.f, 11.f, 3.f))); }
    }

    TEST_CASE("skrooms: the spaces pass leaves the overlay's draw with the product", "[skrooms]")
    {
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
        { INFO("the draw came with the pass"); REQUIRE((draw != nullptr)); }
        { INFO("a draw entry per space"); REQUIRE((draw->mSpaces.size()) == (r.mEnc->mSpaceIds.size())); }
        { INFO("the room fills"); REQUIRE((!draw->mFills.empty())); }
        bool outlined = false;
        for (const SpaceDraw& sd : draw->mSpaces)
        {
            outlined = outlined || !sd.mOutline.empty();
            { INFO("full strength until a gather draws it"); REQUIRE((std::fabs(sd.mColor.mV[VALPHA] - 1.f) < 1e-4f)); }
        }
        { INFO("the room's outline traced"); REQUIRE((outlined)); }
        F32 area = 0.f;
        for (const SpacesDraw::Fill& f : draw->mFills)
        {
            { INFO("the fill names a space"); REQUIRE((f.mSpace < draw->mSpaces.size())); }
            { INFO("the fill sits at a floor"); REQUIRE((f.mZ > -10.f && f.mZ < 30.f)); }
            area += f.mSide * f.mSide;
        }
        { INFO("the fills cover the room's floor"); REQUIRE((area > 60.f)); }
    }

    TEST_CASE("skrooms: a pass over again leaves the landed products and their draw untouched", "[skrooms]")
    {
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
        { INFO("the first draw"); REQUIRE((first.mRooms->mDraw != nullptr)); }
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
        { INFO("the second draw"); REQUIRE((second.mRooms->mDraw != nullptr)); }
        { INFO("a different product, not a rewritten one"); REQUIRE((second.mRooms != first.mRooms)); }

        const std::vector<SpacesDraw::Fill>& now_fills = first.mRooms->mDraw->mFills;
        { INFO("the landed fills keep"); REQUIRE((fills.size() == now_fills.size() &&
               (fills.empty() || std::memcmp(fills.data(), now_fills.data(), fills.size() * sizeof(SpacesDraw::Fill)) == 0))); }

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
        { INFO("the landed outlines keep"); REQUIRE((kept)); }

        size_t c = 0;
        bool coarse_kept = true;
        for (const TileEnclosure& t : first.mEnc->mTiles)
        {
            for (const CoarseGap& g : t.mCoarse.mGaps)
            {
                coarse_kept = coarse_kept && c < coarse.size() && coarse[c++] == g.mSpace;
            }
        }
        { INFO("the landed coarse keeps"); REQUIRE((coarse_kept)); }
    }

    /// A 40 m square roof over 12..52 at the given height, with walls on the
    /// given sides of north, east, south, west; a south wall keeps a 2 m door.
    Result canopy(F32 height, bool walls[4])
    {
        Scene s;
        s.box(12.f, 12.f, 52.f, 52.f, height, height + 0.25f);
        for (F32 px : { 12.f, 51.75f })
        {
            for (F32 py : { 12.f, 51.75f })
            {
                s.wall(px, py, px + 0.25f, py + 0.25f, height);
            }
        }
        if (walls[0])
        {
            s.wall(12.f, 51.75f, 52.f, 52.f, height);
        }
        if (walls[1])
        {
            s.wall(51.75f, 12.f, 52.f, 52.f, height);
        }
        if (walls[2])
        {
            s.wall(12.f, 12.f, 31.f, 12.25f, height);
            s.wall(33.f, 12.f, 52.f, 12.25f, height);
        }
        if (walls[3])
        {
            s.wall(12.f, 12.f, 12.25f, 52.f, height);
        }
        auto in = s.input(2, 2);
        s.addSky(*in);
        return run(in);
    }

    TEST_CASE("skrooms: the middle of a 40 m roof on posts is SHELTERED at the gap, however deep, low roof or high", "[skrooms]")
    {
        for (F32 height : { 3.f, 6.f })
        {
            bool walls[4] = { false, false, false, false };
            const Result r = canopy(height, walls);
            Enclosure e;
            { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e))); }
            { INFO("deep enough to read as INDOORS by e alone"); REQUIRE((e.mE > 0.75f)); }
            { INFO("open at the sides but for the corner posts"); REQUIRE((e.mO > 0.7f)); }
            { INFO("a canopy"); REQUIRE((e.mCanopy)); }
            { INFO("SHELTERED"); REQUIRE((e.mBucket == EnclosureBucket::SHELTERED)); }
            // The coarse answers, what a far reader and evicted detail keep, agree.
            S32 canopies = 0;
            bool capped = true;
            for (const TileEnclosure& t : r.mEnc->mTiles)
            {
                for (const CoarseGap& g : t.mCoarse.mGaps)
                {
                    if (g.mFlags & ENCLOSURE_CANOPY)
                    {
                        ++canopies;
                        capped = capped && g.mBucket != static_cast<U8>(EnclosureBucket::INDOORS);
                    }
                }
            }
            { INFO("coarse canopies"); REQUIRE((canopies > 100)); }
            { INFO("capped in the coarse too"); REQUIRE((capped)); }
        }
    }

    TEST_CASE("skrooms: a walled 40 m hall with one door stays INDOORS in its middle", "[skrooms]")
    {
        for (F32 height : { 3.f, 6.f })
        {
            bool walls[4] = { true, true, true, true };
            const Result r = canopy(height, walls);
            Enclosure e;
            { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e))); }
            { INFO("walled on most sides"); REQUIRE((e.mO < 0.3f)); }
            { INFO("no canopy"); REQUIRE((!e.mCanopy && !e.mSealed)); }
            { INFO("INDOORS"); REQUIRE((e.mBucket == EnclosureBucket::INDOORS)); }
        }
    }

    TEST_CASE("skrooms: under a 40 m roof with one wall and three open sides is SHELTERED at the gap", "[skrooms]")
    {
        bool walls[4] = { true, false, false, false };
        const Result r = canopy(6.f, walls);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e))); }
        { INFO("deep enough to read as INDOORS by e alone"); REQUIRE((e.mE > 0.75f)); }
        { INFO("walled on the north only"); REQUIRE((e.mO > 0.5f && e.mO < 0.9f)); }
        { INFO("SHELTERED"); REQUIRE((e.mBucket == EnclosureBucket::SHELTERED)); }
    }

    TEST_CASE("skrooms: a street between towers is walled at the sides but unroofed, so still OUTDOORS", "[skrooms]")
    {
        Scene s;
        s.box(0.f, 0.f, 12.f, 32.f, 0.f, 36.f, false);
        s.box(20.f, 0.f, 32.f, 32.f, 0.f, 36.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(16.f, 16.f, 0.5f), e))); }
        { INFO("open only along the street"); REQUIRE((e.mO > 0.f && e.mO < 0.5f)); }
        { INFO("no canopy"); REQUIRE((!e.mCanopy)); }
        { INFO("OUTDOORS"); REQUIRE((e.mBucket == EnclosureBucket::OUTDOORS)); }
    }

    TEST_CASE("skrooms: a building with no door is INDOORS, and no canopy", "[skrooms]")
    {
        Scene s;
        s.wall(12.f, 12.f, 52.f, 12.25f, 6.f);
        s.wall(12.f, 51.75f, 52.f, 52.f, 6.f);
        s.wall(12.f, 12.f, 12.25f, 52.f, 6.f);
        s.wall(51.75f, 12.f, 52.f, 52.f, 6.f);
        s.box(12.f, 12.f, 52.f, 52.f, 6.f, 6.25f);
        auto in = s.input(2, 2);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure e;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(32.f, 32.f, 0.5f), e))); }
        { INFO("walled all round, no canopy"); REQUIRE((e.mO < 0.3f && !e.mCanopy)); }
        { INFO("INDOORS"); REQUIRE((e.mBucket == EnclosureBucket::INDOORS)); }
    }

    TEST_CASE("skrooms: a doorstep in a facade recess under a high overhang is no lift, and the door opens onto the street", "[skrooms]")
    {
        Scene s = recessed(false);
        auto in = s.input(1);
        s.addSky(*in);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(20.5f, 12.125f, 0.f, 1.f, 1.f));
        const Result r = run(in, rooms);
        for (const Space& sp : r.mRooms->mSpaces)
        {
            { INFO("no lift"); REQUIRE((!sp.mLift)); }
        }
        const Space* hall = r.at(18.f, 20.f);
        const Space* street = r.at(20.5f, 4.f);
        { INFO("found"); REQUIRE((hall && street && hall != street)); }
        { INFO("hall roofed"); REQUIRE((hall->mBucket != EnclosureBucket::OUTDOORS)); }
        { INFO("street not INDOORS"); REQUIRE((street->mBucket != EnclosureBucket::INDOORS)); }
        const Space* step = r.at(20.5f, 11.f);
        { INFO("doorstep not INDOORS"); REQUIRE((step && step->mBucket != EnclosureBucket::INDOORS)); }
        bool door_out = false;
        for (const Portal* q : r.between(hall, step))
        {
            door_out = door_out || q->mKind == PortalKind::DOOR;
        }
        { INFO("the door joins the hall to the doorstep"); REQUIRE((door_out)); }
    }

    TEST_CASE("skrooms: a tall roofed shaft behind a door is a lift", "[skrooms]")
    {
        Scene s = recessed(true);
        auto in = s.input(1);
        s.addSky(*in);
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mDoors.push_back(door(20.5f, 12.125f, 0.f, 1.f, 1.f));
        rooms->mDoors.push_back(door(15.25f, 27.875f, 0.f, 1.f, 1.f));
        const Result r = run(in, rooms);
        const Space* lift = r.at(15.25f, 29.25f);
        const Space* hall = r.at(18.f, 20.f);
        { INFO("found"); REQUIRE((lift && hall)); }
        { INFO("a lift of its own" + info(r)); REQUIRE((lift != hall && lift->mLift)); }
        { INFO("INDOORS"); REQUIRE((lift->mBucket == EnclosureBucket::INDOORS)); }
        U32 lifts = 0;
        for (const Space& sp : r.mRooms->mSpaces)
        {
            lifts += sp.mLift ? 1 : 0;
        }
        { INFO("only the shaft"); REQUIRE((lifts) == (1u)); }
    }

    TEST_CASE("skrooms: ground under an open flight is sheltered and opens onto the street under its stringer", "[skrooms][.flights]")
    {
        // A flight up +x from 4 m to 16 m onto a landing block, treads as thin slabs with air below;
        // walled on the north, and on the south a stringer hangs to 0.9 m over the street.
        Scene s;
        for (S32 k = 1; k <= 24; ++k)
        {
            const F32 x = 4.f + (k - 1) * 0.5f, z = 0.25f * k;
            s.box(x, 10.25f, x + 0.5f, 12.f, z - 0.25f, z, true, true);
        }
        s.box(16.f, 10.f, 20.f, 12.25f, 0.f, 6.f);
        s.wall(4.f, 12.f, 16.f, 12.25f, 6.f);
        s.box(4.f, 10.f, 16.f, 10.25f, 0.9f, 6.f, false);
        auto in = s.input(1);
        s.addSky(*in);
        const Result r = run(in);
        const Space* under = r.at(14.f, 11.1f, 0.5f);
        const Space* street = r.at(13.f, 5.f, 0.5f);
        { INFO(llformat("found %d %d", under ? 1 : 0, street ? 1 : 0)); REQUIRE((under && street && under != street)); }
        { INFO("under its flight" + info(r)); REQUIRE((under->mUnderFlight)); }
        { INFO("sheltered, not indoors"); REQUIRE((under->mBucket != EnclosureBucket::INDOORS)); }
        { INFO("opens onto the street"); REQUIRE((!r.between(under, street).empty())); }
    }

    TEST_CASE("skrooms: two shafts behind stacks of doors are two lifts; a stack of doors on a storeyed hall is none", "[skrooms]")
    {
        // Bad Space's pair: two 2.5 m shafts side by side, 30 m tall, each behind a census stack of doors
        // in the wall of a storeyed hall; a third stack faces a hall with a floor every 4 m.
        Scene s;
        s.wall(11.75f, 9.75f, 14.75f, 10.f, 30.f);
        s.wall(11.75f, 15.25f, 14.75f, 15.5f, 30.f);
        s.wall(11.75f, 9.75f, 12.f, 15.5f, 30.f);
        s.wall(12.f, 12.5f, 14.5f, 12.75f, 30.f);
        for (F32 y0 : { 10.f, 12.75f })
        {
            s.box(14.5f, y0, 14.75f, y0 + 0.5f, 0.f, 30.f, false);
            s.box(14.5f, y0 + 1.75f, 14.75f, y0 + 2.5f, 0.f, 30.f, false);
            s.box(14.5f, y0 + 0.5f, 14.75f, y0 + 1.75f, 2.2f, 30.f, false);
        }
        s.box(11.75f, 9.75f, 14.75f, 15.5f, 30.f, 30.25f);
        s.wall(14.75f, 4.f, 26.f, 4.25f, 30.f);
        s.wall(14.75f, 21.75f, 26.f, 22.f, 30.f);
        s.wall(25.75f, 4.f, 26.f, 22.f, 30.f);
        s.wall(14.5f, 4.f, 14.75f, 9.75f, 30.f);
        s.wall(14.5f, 15.5f, 14.75f, 22.f, 30.f);
        for (F32 z = 4.f; z < 30.f; z += 4.f)
        {
            s.box(14.75f, 4.f, 26.f, 22.f, z - 0.25f, z);
        }
        s.box(14.75f, 4.f, 26.f, 22.f, 30.f, 30.25f);
        auto in = s.input(1);
        s.addSky(*in);
        auto rooms = std::make_shared<RoomsInput>();
        for (F32 y0 : { 10.f, 12.75f })
        {
            rooms->mLifts.emplace_back(LLVector3d(14.5f, y0 + 0.5f, 0.f), LLVector3d(14.6f, y0 + 1.75f, 30.f));
            for (F32 z = 0.f; z < 28.f; z += 4.f)
            {
                rooms->mDoors.push_back(door(14.625f, y0 + 1.125f, 1.f, 0.f, 1.25f, z));
            }
        }
        rooms->mLifts.emplace_back(LLVector3d(25.75f, 12.f, 0.f), LLVector3d(25.85f, 13.25f, 30.f));
        const Result r = run(in, rooms);
        const Space* a = r.at(13.25f, 11.25f);
        const Space* b = r.at(13.25f, 14.f);
        { INFO("found"); REQUIRE((a && b)); }
        { INFO("each a lift" + info(r)); REQUIRE((a->mLift && b->mLift)); }
        { INFO("two shafts, two lifts" + info(r)); REQUIRE((a != b)); }
        U32 lifts = 0;
        for (const Space& sp : r.mRooms->mSpaces)
        {
            lifts += sp.mLift ? 1 : 0;
        }
        { INFO("only the shafts"); REQUIRE((lifts) == (2u)); }
        // The census alone, with no doors to probe from, finds the same two.
        rooms->mDoors.clear();
        const Result c = run(in, rooms);
        const Space* ca = c.at(13.25f, 11.25f);
        const Space* cb = c.at(13.25f, 14.f);
        { INFO("census lifts" + info(c)); REQUIRE((ca && cb && ca != cb && ca->mLift && cb->mLift)); }
    }

    TEST_CASE("skrooms: a rooftop and the roof of its stair hut are both roofs, linked to the building under them", "[skrooms]")
    {
        Scene s = twoRooms();
        // A hut on the rooftop with a door onto it, its own roof 2.75 m up.
        s.box(12.f, 4.f, 16.f, 4.25f, 3.25f, 6.f, false);
        s.box(12.f, 7.75f, 16.f, 8.f, 3.25f, 6.f, false);
        s.box(15.75f, 4.f, 16.f, 8.f, 3.25f, 6.f, false);
        s.box(12.f, 4.f, 12.25f, 5.5f, 3.25f, 6.f, false);
        s.box(12.f, 6.5f, 12.25f, 8.f, 3.25f, 6.f, false);
        s.box(12.f, 5.5f, 12.25f, 6.5f, 5.45f, 6.f, false);
        s.box(12.f, 4.f, 16.f, 8.f, 6.f, 6.25f);
        const Result r = run(s.input(1));
        const Space* west = r.at(6.f, 6.f);
        const Space* top = r.at(6.f, 6.f, 3.75f);
        const Space* hut = r.at(14.f, 6.f, 6.75f);
        { INFO("the room, the rooftop and the hut's roof" + info(r)); REQUIRE((west && top && hut && top != west && hut != top)); }
        { INFO("the rooftop is a roof, outdoors"); REQUIRE((top->mRoof && top->mBucket == EnclosureBucket::OUTDOORS)); }
        { INFO("the hut's roof is a roof, outdoors"); REQUIRE((hut->mRoof && hut->mBucket == EnclosureBucket::OUTDOORS)); }
        { INFO("the rooftop covers the rooms"); REQUIRE((std::find(top->mCovers.begin(), top->mCovers.end(), r.index(west)) != top->mCovers.end())); }
        { INFO("both in the building under them"); REQUIRE((west->mBuilding && top->mBuilding == west->mBuilding && hut->mBuilding == west->mBuilding)); }
    }

    TEST_CASE("skrooms: a frozen roof covers the spaces it covered: its covers index the product it is in", "[skrooms]")
    {
        // The building in the second tile, a wall in the first so the live spaces there come first.
        Scene s = twoRooms();
        s.mShift = 32.f;
        s.box(-28.f, 16.f, -20.f, 16.25f, 0.f, 3.f, false);
        s.box(-28.f, 16.f, -27.75f, 24.f, 0.f, 3.f, false);
        s.box(-20.25f, 16.f, -20.f, 24.f, 0.f, 3.f, false);
        s.box(-28.f, 23.75f, -20.f, 24.f, 0.f, 3.f, false);
        s.box(-28.f, 16.f, -20.f, 24.f, 3.f, 3.25f);
        auto in = s.input(2);
        const Result first = run(in);
        const Space* roof = first.at(38.f, 6.f, 3.75f);
        { INFO("a roof over the rooms" + info(first)); REQUIRE((roof && roof->mRoof && !roof->mCovers.empty())); }
        std::vector<U32> covered;
        for (U32 c : roof->mCovers)
        {
            covered.push_back(first.mRooms->mSpaces[c].mId);
        }
        // The building's tile is evicted; the pass runs again on the first tile alone.
        in->mPrevious = first.mEnc;
        in->mKept.push_back(*first.mEnc->tile(in->mTiles[1].mKey));
        in->mTiles.pop_back();
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mPrevious = first.mRooms;
        const Result second = run(in, rooms);
        const Space* frozen = nullptr;
        for (const Space& sp : second.mRooms->mSpaces)
        {
            frozen = sp.mId == roof->mId ? &sp : frozen;
        }
        { INFO("the roof kept, frozen" + info(second)); REQUIRE((frozen && frozen->mFrozen && frozen->mRoof)); }
        std::vector<U32> now;
        for (U32 c : frozen->mCovers)
        {
            { INFO("a cover in the product"); REQUIRE((c < second.mRooms->mSpaces.size())); }
            now.push_back(second.mRooms->mSpaces[c].mId);
        }
        { INFO("the same spaces covered"); REQUIRE((now == covered)); }
    }

    TEST_CASE("skrooms: an aperture cut whose lintel lies in a clean ring tile holds on an incremental pass", "[skrooms]")
    {
        // The doorway's lintel lies wholly in the first tile, the room past it in the moved second.
        for (F32 shift : { 21.25f, 21.5f, 21.625f, 21.75f, 22.f, 9.75f })
        {
            Scene before = loopedRooms();
            before.mShift = shift;
            Scene after = before;
            after.box(55.f - shift, 20.f, 56.f - shift, 21.f, 0.f, 3.f, false);
            const Result first = run(before.input(2));
            std::shared_ptr<EnclosureInput> in = after.input(2);
            for (EnclosureInput::Tile& t : in->mTiles)
            {
                const RoomsTile* was = first.mRooms->tile(t.mKey);
                if (was && t.mKey.mX == 0)
                {
                    t.mAir = was->mAir;
                }
            }
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mPrevious = first.mRooms;
            const Result incremental = run(in, rooms);
            const Result full = run(in);
            const std::string at = llformat(" at shift %.3f", shift);
            { INFO("the full pass parts the rooms" + at); REQUIRE((full.at(6.f + shift, 6.f) && full.at(6.f + shift, 6.f) != full.at(14.f + shift, 6.f))); }
            { INFO("so does the incremental pass" + at); REQUIRE((incremental.at(6.f + shift, 6.f) && incremental.at(6.f + shift, 6.f) != incremental.at(14.f + shift, 6.f))); }
            { INFO("one aperture cut either way" + at); REQUIRE((incremental.mRooms->mApart.size()) == (full.mRooms->mApart.size())); }
        }
    }

    TEST_CASE("skrooms: a doorway beside a wide opening parts its rooms or not alike wherever a tile seam crosses it", "[skrooms]")
    {
        // A partition across a hall: a wall, a 2 m doorway under a lintel, a short wall, then open to the north wall.
        for (F32 top : { 10.f, 20.f })
        {
            S32 parted = -1;
            for (F32 shift : { 0.f, 21.75f, 24.f })
            {
                Scene s;
                s.wall(2.f, 2.f, 18.f, 2.25f);
                s.wall(2.f, top, 18.f, top + 0.25f);
                s.wall(2.f, 2.f, 2.25f, top + 0.25f);
                s.wall(17.75f, 2.f, 18.f, top + 0.25f);
                s.wall(10.f, 2.25f, 10.25f, 3.f);
                s.box(10.f, 3.f, 10.25f, 5.f, 2.2f, 3.f, false);
                s.wall(10.f, 5.f, 10.25f, 6.f);
                s.box(2.f, 2.f, 18.f, top + 0.25f, 3.f, 3.25f);
                s.mShift = shift;
                const Result r = run(s.input(2));
                const Space* w = r.at(4.f + shift, 4.f);
                const Space* e = r.at(16.f + shift, 4.f);
                { INFO("both rooms found"); REQUIRE((w && e)); }
                const S32 now = w != e ? 1 : 0;
                { INFO(llformat("the same answer at shift %.2f", shift)); REQUIRE((parted < 0 || parted == now)); }
                { INFO("a cut where they part"); REQUIRE((r.mRooms->mApart.size()) == (static_cast<size_t>(now))); }
                parted = now;
            }
            { INFO("a short opening parts them, a long one does not"); REQUIRE((parted == (top < 15.f ? 1 : 0))); }
        }
    }

    TEST_CASE("skrooms: a low hall wider than the side reads is INDOORS in its middle; a canopy on posts is still SHELTERED", "[skrooms]")
    {
        Scene s;
        s.wall(4.f, 4.f, 68.f, 4.5f);
        s.wall(4.f, 67.5f, 68.f, 68.f);
        s.wall(4.f, 4.f, 4.5f, 30.f);
        s.wall(4.f, 32.f, 4.5f, 68.f);
        s.box(4.f, 30.f, 4.5f, 32.f, 2.2f, 3.f, false);
        s.wall(67.5f, 4.f, 68.f, 68.f);
        s.box(4.f, 4.f, 68.f, 68.f, 3.f, 3.25f);
        // A canopy on posts out past the hall, wider than a stall but within the side reads.
        s.box(76.f, 24.f, 88.f, 36.f, 3.f, 3.25f);
        for (F32 px : { 76.f, 87.5f })
        {
            for (F32 py : { 24.f, 35.5f })
            {
                s.box(px, py, px + 0.5f, py + 0.5f, 0.f, 3.f, false);
            }
        }
        auto in = s.input(3, 3);
        s.addSky(*in);
        const Result r = run(in);
        Enclosure mid, side, canopy;
        { INFO("read"); REQUIRE((r.mEnc->enclosureAt(LLVector3(36.f, 36.f, 0.5f), mid) && r.mEnc->enclosureAt(LLVector3(10.f, 36.f, 0.5f), side) &&
                           r.mEnc->enclosureAt(LLVector3(82.f, 30.f, 0.5f), canopy))); }
        { INFO("the hall's middle is no canopy"); REQUIRE((!mid.mCanopy)); }
        { INFO("the hall's middle is INDOORS"); REQUIRE((mid.mBucket == EnclosureBucket::INDOORS)); }
        { INFO("so is its side"); REQUIRE((side.mBucket == EnclosureBucket::INDOORS)); }
        { INFO("the canopy is one"); REQUIRE((canopy.mCanopy)); }
        { INFO("and SHELTERED"); REQUIRE((canopy.mBucket == EnclosureBucket::SHELTERED)); }
        const Space* hall = r.at(36.f, 36.f);
        { INFO("the hall is an INDOORS space" + info(r)); REQUIRE((hall && hall->mBucket == EnclosureBucket::INDOORS)); }
    }

    TEST_CASE("skrooms: a rooftop's over-roofed share is of the floor its cover was read on, pedestals spread onto it included", "[skrooms]")
    {
        // The rooftop reached from a stair hut's door, so a space of its own walking, with pedestals too small to stand on.
        Scene s = twoRooms();
        s.box(12.f, 4.f, 16.f, 4.25f, 3.25f, 6.f, false);
        s.box(12.f, 7.75f, 16.f, 8.f, 3.25f, 6.f, false);
        s.box(15.75f, 4.f, 16.f, 8.f, 3.25f, 6.f, false);
        s.box(12.f, 4.f, 12.25f, 5.5f, 3.25f, 6.f, false);
        s.box(12.f, 6.5f, 12.25f, 8.f, 3.25f, 6.f, false);
        s.box(12.f, 5.5f, 12.25f, 6.5f, 5.45f, 6.f, false);
        s.box(12.f, 4.f, 16.f, 8.f, 6.f, 6.25f);
        for (F32 px = 3.f; px < 11.f; px += 1.5f)
        {
            s.box(px, 3.f, px + 0.5f, 3.5f, 3.25f, 3.75f);
            s.box(px, 8.5f, px + 0.5f, 9.f, 3.25f, 3.75f);
        }
        const Result r = run(s.input(1));
        const Space* top = r.at(6.f, 6.f, 3.75f);
        { INFO("the rooftop" + info(r)); REQUIRE((top && top->mRoof)); }
        // Its floors, as the cover is read: walkable tops an avatar's head clears.
        F64 floor = 0.0, cover = 0.0;
        for (const TileEnclosure& te : r.mEnc->mTiles)
        {
            const TileAir& a = *te.mAir;
            for (const AirLeaf& leaf : a.mLeaves)
            {
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const AirGap& g = a.mGaps[leaf.mFirstGap + i];
                    const bool tall = g.mCeiling == AIR_CEILING_OPEN || (g.mCeiling - g.mFloor) * a.mUnit >= 1.f - 1e-3f;
                    if (te.mGaps[leaf.mFirstGap + i].mSpace == r.index(top) + 1 && (g.mFlags & AirGap::TOP) && tall)
                    {
                        floor += a.mCell * a.mCell * (1 << leaf.mLevel) * (1 << leaf.mLevel);
                    }
                }
            }
        }
        for (F32 m2 : top->mCoverM2)
        {
            cover += m2;
        }
        { INFO(llformat("the share is the cover over that floor: %.4f, %.2f / %.2f, seeds' floor %.2f", top->mOverRoofed, cover, floor, top->mFloorArea)); REQUIRE((floor > 0.0 && std::fabs(top->mOverRoofed - cover / floor) < 1e-3)); }
        for (const Space& sp : r.mRooms->mSpaces)
        {
            { INFO(llformat("a share, %.3f", sp.mOverRoofed)); REQUIRE((sp.mOverRoofed >= 0.f && sp.mOverRoofed <= 1.f)); }
        }
    }

    TEST_CASE("skrooms: a lift across a partition takes from each room the floor it took there, no more", "[skrooms]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 6.f, 2.2f, 3.f, false);
        const Result plain = run(s.input(1));
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mLifts.emplace_back(LLVector3d(9.f, 7.f, -0.5f), LLVector3d(11.f, 9.f, 20.f));
        const Result r = run(s.input(1), rooms);
        const Space* lift = r.at(9.5f, 8.f);
        const Space* west = r.at(6.f, 4.f);
        const Space* east = r.at(14.f, 4.f);
        { INFO("a lift and two rooms" + info(r)); REQUIRE((lift && west && east && lift->mLift && west != east && lift != west && lift != east)); }
        { INFO("the lift is on both sides"); REQUIRE((r.at(10.75f, 8.f) == lift)); }
        const F32 before = plain.at(6.f, 4.f)->mFloorArea + plain.at(14.f, 4.f)->mFloorArea;
        const F32 lost = before - west->mFloorArea - east->mFloorArea;
        { INFO(llformat("the rooms lose the lift's floor: lost %.2f, the lift %.2f", lost, lift->mFloorArea)); REQUIRE((std::fabs(lost - lift->mFloorArea) < 0.5f)); }
    }
}
