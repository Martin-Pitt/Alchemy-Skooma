/**
 * @file skroomswind_test.cpp
 * @brief Tests for wind over synthetic air: lee, venturi, calm rooms, draughts, courtyards.
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


#include "../skroomsexport.h"
#include "../skroomswind.h"
#include "skroomstestscene.h"

#include "llstring.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
    using namespace skworld;
    using namespace skrooms_test;

    struct Wind
    {
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;
        WindFieldPtr mField;

        Wind(const std::shared_ptr<EnclosureInput>& in, const WindLevel& level = WindLevel())
        {
            EnclosurePass pass(in);
            while (!pass.step(1u << 30))
            {
            }
            mEnc = pass.takeProduct();
            mRooms = computeSpaces(std::make_shared<RoomsInput>(), mEnc);
            mField = buildWindField(*mEnc, mRooms.get(), level, WindParams());
        }

        WindSlabPtr slab(F32 heading) const { return computeWindSlab(*mField, mRooms.get(), heading, WindParams()); }
    };

    F32 factor(const WindSlabPtr& s, F32 x, F32 y)
    {
        const WindCell* c = s->at(x, y);
        return c ? c->factor() : -1.f;
    }

    U8 flags(const WindSlabPtr& s, F32 x, F32 y)
    {
        const WindCell* c = s->at(x, y);
        return c ? c->mFlags : 0;
    }
}

namespace
{

    TEST_CASE("skroomswind: a lone box casts a lee of 8 heights", "[skroomswind]")
    {
        // 5.5 m tall: 4 m above the slab, so the lee recovers over 32 m.
        Scene s;
        s.box(20.f, 26.f, 24.f, 38.f, 0.f, 5.5f, false);
        const Wind w(s.input(4, 2));
        const WindSlabPtr e = w.slab(0.f);
        { INFO("solid at the slab"); REQUIRE(((flags(e, 22.f, 32.f) & WindCell::BLOCKED) != 0)); }
        const F32 at8 = factor(e, 32.25f, 32.25f), at24 = factor(e, 48.25f, 32.25f), at40 = factor(e, 64.25f, 32.25f);
        { INFO("sheltered at 8 m"); REQUIRE((at8 > 0.15f && at8 < 0.4f)); }
        { INFO("recovering at 24 m"); REQUIRE((at24 > 0.6f && at24 < 0.9f)); }
        { INFO("free at 40 m"); REQUIRE((at40 > 0.9f)); }
        { INFO("free upwind"); REQUIRE((factor(e, 8.25f, 32.25f) > 0.9f)); }
    }

    TEST_CASE("skroomswind: a tall box's lee is capped at 60 m", "[skroomswind]")
    {
        Scene s;
        s.box(10.f, 26.f, 14.f, 38.f, 0.f, 30.f, false);
        const Wind w(s.input(4, 2));
        const WindSlabPtr e = w.slab(0.f);
        // Uncapped, 8 x 28.5 m would leave 30 m at a fifth of the stream.
        const F32 at30 = factor(e, 44.25f, 32.25f), at62 = factor(e, 76.25f, 32.25f);
        { INFO("half at 30 m"); REQUIRE((at30 > 0.4f && at30 < 0.65f)); }
        { INFO("free past 60 m"); REQUIRE((at62 > 0.9f)); }
    }

    TEST_CASE("skroomswind: a gap between two boxes speeds up", "[skroomswind]")
    {
        Scene s;
        s.box(24.f, 10.f, 34.f, 30.5f, 0.f, 6.f, false);
        s.box(24.f, 33.5f, 34.f, 54.f, 0.f, 6.f, false);
        const Wind w(s.input(2, 2));
        const WindSlabPtr e = w.slab(0.f);
        const F32 gap = factor(e, 32.25f, 32.25f);
        { INFO("faster than the stream"); REQUIRE((gap > 1.3f)); }
        { INFO("faster than open ground"); REQUIRE((gap > factor(e, 8.25f, 60.25f) + 0.3f)); }
    }

    TEST_CASE("skroomswind: a closed room is calm", "[skroomswind]")
    {
        Scene s;
        s.wall(4.f, 4.f, 16.f, 4.25f);
        s.wall(4.f, 15.75f, 16.f, 16.f);
        s.wall(4.f, 4.f, 4.25f, 16.f);
        s.wall(15.75f, 4.f, 16.f, 16.f);
        s.box(4.f, 4.f, 16.f, 16.f, 3.f, 3.25f);
        const Wind w(s.input(1));
        for (F32 heading : { 0.f, 135.f })
        {
            const WindSlabPtr e = w.slab(heading);
            { INFO("indoors"); REQUIRE(((flags(e, 10.f, 10.f) & WindCell::INDOORS) != 0)); }
            { INFO("calm"); REQUIRE((factor(e, 10.f, 10.f) < 0.1f)); }
            { INFO("windy outside"); REQUIRE((factor(e, 25.f, 25.f) > 0.5f)); }
        }
    }

    TEST_CASE("skroomswind: a doorway between open and closed sides draughts", "[skroomswind]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 10.f);
        const Wind w(s.input(1));
        const WindSlabPtr e = w.slab(0.f);
        const WindCell* c = e->at(2.75f, 5.5f);
        { INFO("a cell inside the doorway"); REQUIRE((c != nullptr)); }
        { INFO("draughts"); REQUIRE(((c->mFlags & WindCell::DRAUGHT) != 0 && c->factor() > 0.15f)); }
        const LLVector2 flow = LLVector2(1.f, 0.f) + c->deflection();
        { INFO("inward, with the wind"); REQUIRE((flow.mV[VX] > 0.5f)); }
        { INFO("calm deep inside"); REQUIRE((factor(e, 7.f, 8.5f) < 0.1f)); }
        { INFO("calm in the closed room"); REQUIRE((factor(e, 14.f, 6.f) < 0.1f)); }
    }

    TEST_CASE("skroomswind: a walled courtyard is damped and swirls", "[skroomswind]")
    {
        Scene s;
        s.wall(6.f, 6.f, 26.f, 6.25f);
        s.wall(6.f, 25.75f, 26.f, 26.f);
        s.wall(6.f, 6.f, 6.25f, 26.f);
        s.wall(25.75f, 6.f, 26.f, 26.f);
        const Wind w(s.input(1));
        const WindSlabPtr e = w.slab(0.f);
        const WindCell* c = e->at(22.f, 16.5f);
        { INFO("a courtyard"); REQUIRE((c && (c->mFlags & WindCell::COURTYARD) != 0)); }
        { INFO("damped"); REQUIRE((c->factor() < 0.6f)); }
        { INFO("swirling"); REQUIRE((c->deflection().length() > 0.2f)); }
        { INFO("open ground is not"); REQUIRE(((flags(e, 29.f, 29.f) & WindCell::COURTYARD) == 0)); }
    }

    TEST_CASE("skroomswind: rotating the heading rotates the result", "[skroomswind]")
    {
        Scene s;
        s.box(30.f, 30.f, 34.f, 34.f, 0.f, 5.5f, false);
        const Wind w(s.input(2, 2));
        const WindSlabPtr east = w.slab(0.f), north = w.slab(90.f);
        F32 worst = 0.f;
        for (F32 d = 2.f; d < 28.f; d += 1.f)
        {
            worst = std::max(worst, std::fabs(factor(east, 32.25f + d, 32.25f) - factor(north, 32.25f, 32.25f + d)));
            worst = std::max(worst, std::fabs(factor(east, 32.25f - d, 32.25f) - factor(north, 32.25f, 32.25f - d)));
        }
        { INFO("the same lee, turned"); REQUIRE((worst < 0.05f)); }
        { INFO("sheltered behind"); REQUIRE((factor(north, 32.25f, 40.25f) < 0.5f)); }
        { INFO("not beside"); REQUIRE((factor(north, 40.25f, 32.25f) > 0.8f)); }
    }

    TEST_CASE("skroomswind: a band over a mesh with no terrain stands on its lowest floor", "[skroomswind]")
    {
        Scene s;
        s.box(0.f, 0.f, 64.f, 32.f, 0.f, 4.f);      // a platform with no terrain under it
        s.wall(30.f, 0.f, 30.5f, 32.f, 7.f);
        s.mGround[2] = 0.f;
        auto in = std::make_shared<WindInput>();
        const Wind w(s.input(2));
        in->mEnclosure = w.mEnc;
        in->mRooms = w.mRooms;
        in->mBands = { WindLevel() };
        const WindProductPtr p = computeWind(in);
        { INFO("16 slabs"); REQUIRE((p->mSlabs.size()) == (static_cast<size_t>(WIND_HEADINGS))); }
        const WindSlabPtr e = p->slab(-16, 0);
        { INFO("heading wraps"); REQUIRE((e && e->mHeading == 0)); }
        const WindCell* c = e->at(40.f, 16.f);
        { INFO("open air above the platform"); REQUIRE((c && !(c->mFlags & WindCell::BLOCKED))); }
        { INFO("a wall on it"); REQUIRE(((e->at(30.25f, 16.f)->mFlags & WindCell::BLOCKED) != 0)); }
        { INFO("sheltered behind it"); REQUIRE((e->at(34.f, 16.f)->factor() < 0.5f)); }
    }

    TEST_CASE("skroomswind: the export carries no wall-clock time, so the same air exports the same bytes", "[skroomswind]")
    {
        Scene s;
        s.box(0.f, 0.f, 64.f, 32.f, 0.f, 4.f);
        s.wall(30.f, 0.f, 30.5f, 32.f, 7.f);
        auto in = std::make_shared<WindInput>();
        const Wind w(s.input(2));
        in->mEnclosure = w.mEnc;
        in->mRooms = w.mRooms;
        in->mBands = { WindLevel() };
        WindProduct a = *computeWind(in);
        WindProduct b = a;
        a.mMillis = 1.5f;
        b.mMillis = 2500.25f;
        std::ostringstream ea, eb;
        writeExportSpaces(ea, LLVector3d(), *w.mRooms, &a);
        writeExportSpaces(eb, LLVector3d(), *w.mRooms, &b);
        { INFO("wind exported"); REQUIRE((ea.str().find("\"wind\"") != std::string::npos)); }
        { INFO("byte-identical"); REQUIRE((ea.str() == eb.str())); }
    }

    TEST_CASE("skroomswind: a pass's slabs, sharing the band's weight blur, match slabs computed alone", "[skroomswind]")
    {
        Scene s;
        s.box(20.f, 20.f, 26.f, 24.f, 0.f, 5.5f, false);
        s.wall(36.f, 8.f, 36.5f, 30.f, 6.f);
        auto in = std::make_shared<WindInput>();
        const Wind w(s.input(4, 2));
        in->mEnclosure = w.mEnc;
        in->mRooms = w.mRooms;
        in->mBands = { WindLevel() };
        const WindProductPtr p = computeWind(in);
        { INFO("16 slabs"); REQUIRE((p->mSlabs.size()) == (static_cast<size_t>(WIND_HEADINGS))); }
        S32 differ = 0, venturi = 0;
        for (S32 h = 0; h < WIND_HEADINGS; ++h)
        {
            const WindSlabPtr alone = w.slab(windHeadingDeg(h));
            const WindSlabPtr shared = p->slab(h, 0);
            { INFO("same size"); REQUIRE((shared->mCells.size()) == (alone->mCells.size())); }
            for (size_t i = 0; i < alone->mCells.size(); ++i)
            {
                const WindCell &a = alone->mCells[i], &b = shared->mCells[i];
                differ += a.mFactor != b.mFactor || a.mDeflect[0] != b.mDeflect[0] || a.mDeflect[1] != b.mDeflect[1] || a.mFlags != b.mFlags;
                venturi += !(a.mFlags & WindCell::BLOCKED) && a.mFactor > 125;
            }
        }
        { INFO("identical cells"); REQUIRE((differ) == (0)); }
        { INFO("some air speeds up between the solids"); REQUIRE((venturi > 0)); }
    }

    TEST_CASE("skroomswind: a portal's normal runs from its first space into its second, and a draught follows the wind", "[skroomswind]")
    {
        Scene s = twoRooms();
        s.wall(10.f, 2.f, 10.25f, 5.f);
        s.wall(10.f, 6.f, 10.25f, 10.f);
        s.box(10.f, 5.f, 10.25f, 6.f, 2.2f, 3.f, false);
        const Wind w(s.input(1));
        { INFO("portals"); REQUIRE((!w.mRooms->mPortals.empty())); }
        for (const Portal& p : w.mRooms->mPortals)
        {
            const LLVector3 n = p.mNormal * 0.75f;
            Enclosure a, b;
            { INFO("air both sides"); REQUIRE((w.mEnc->enclosureAt(p.mCentre - n + LLVector3(0.f, 0.f, 0.5f), a) &&
                                         w.mEnc->enclosureAt(p.mCentre + n + LLVector3(0.f, 0.f, 0.5f), b))); }
            { INFO("behind is the first"); REQUIRE((static_cast<U32>(a.mSpace)) == (p.mSpaces[0] + 1)); }
            { INFO("ahead is the second"); REQUIRE((static_cast<U32>(b.mSpace)) == (p.mSpaces[1] + 1)); }
        }
        // Where a draught turns the air inside the west door, it runs along the door: in or out with the wind, in along the wall.
        auto sign = [&w](F32 heading) {
            const WindSlabPtr e = w.slab(heading);
            const F32 rad = heading * DEG_TO_RAD;
            S32 in = 0, out = 0;
            for (F32 x = 2.25f; x < 4.5f; x += 0.5f)
            {
                const WindCell* c = e->at(x, 5.5f);
                if ((c->mFlags & WindCell::DRAUGHT) && c->deflection().length() > 0.1f)
                {
                    const F32 fx = (LLVector2(std::cos(rad), std::sin(rad)) + c->deflection()).mV[VX];
                    in += fx > 0.5f ? 1 : 0;
                    out += fx < -0.5f ? 1 : 0;
                }
            }
            return in && !out ? 1 : (out && !in ? -1 : 0);
        };
        { INFO("in, with the wind"); REQUIRE((sign(45.f)) == (1)); }
        { INFO("out, with the wind"); REQUIRE((sign(135.f)) == (-1)); }
        { INFO("in from the open side, along the wall"); REQUIRE((sign(90.f)) == (1)); }
    }

    TEST_CASE("skroomswind: replay a mesh's air into wind, when a file is named", "[skroomswind]")
    {
        // SKNAV_WIND_REPLAY names a rooms replay file; SKNAV_WIND_Z the slab's global height.
        const std::string path = LLStringUtil::getenv("SKNAV_WIND_REPLAY");
        if (path.empty())
        {
            return;
        }
        std::shared_ptr<EnclosureInput> enc_in;
        std::shared_ptr<RoomsInput> rooms;
        std::vector<LLVector3d> points;
        { INFO("read a replay file"); REQUIRE((readReplay(path, enc_in, rooms, points, true))); }
        const LLVector3d origin = enc_in->mMeshOrigin;
        EnclosurePass pass(enc_in);
        while (!pass.step(1u << 30))
        {
        }
        std::shared_ptr<EnclosureProduct> enc = pass.takeProduct();
        const RoomsProductPtr r = computeSpaces(rooms, enc);
        WindLevel level;
        level.mAbsolute = true;
        level.mZ = static_cast<F32>(std::stod(LLStringUtil::getenv("SKNAV_WIND_Z")) - origin.mdV[VZ]);
        const auto t0 = std::chrono::steady_clock::now();
        WindFieldPtr f = buildWindField(*enc, r.get(), level, WindParams());
        const auto t1 = std::chrono::steady_clock::now();
        // SKNAV_WIND_CLIP "x0 y0 x1 y1", global: nothing outside stands, as in an oracle's window.
        const std::string clip = LLStringUtil::getenv("SKNAV_WIND_CLIP");
        if (!clip.empty())
        {
            F64 b[4];
            std::istringstream(clip) >> b[0] >> b[1] >> b[2] >> b[3];
            auto cut = std::make_shared<WindField>(*f);
            for (S32 y = 0; y < cut->mHeight; ++y)
            {
                for (S32 x = 0; x < cut->mWidth; ++x)
                {
                    const F64 gx = origin.mdV[VX] + cut->mOrigin.mV[VX] + (x + 0.5) * cut->mCell;
                    const F64 gy = origin.mdV[VY] + cut->mOrigin.mV[VY] + (y + 0.5) * cut->mCell;
                    if (gx < b[0] || gy < b[1] || gx >= b[2] || gy >= b[3])
                    {
                        const size_t i = x + static_cast<size_t>(y) * cut->mWidth;
                        cut->mFlags[i] = WindCell::UNSEEN;
                        cut->mSolid[i] = 0.f;
                    }
                }
            }
            f = cut;
        }
        std::ofstream out(path + ".wind.bin", std::ios::binary);
        auto put = [&out](const auto& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        const F64 ox = f->mOrigin.mV[VX] + origin.mdV[VX], oy = f->mOrigin.mV[VY] + origin.mdV[VY];
        put(ox);
        put(oy);
        put(f->mCell);
        put(f->mWidth);
        put(f->mHeight);
        put(static_cast<F32>(std::chrono::duration<F64, std::milli>(t1 - t0).count()));
        for (F32 heading : { 0.f, 45.f, 90.f })
        {
            const auto t2 = std::chrono::steady_clock::now();
            const WindSlabPtr s = computeWindSlab(*f, r.get(), heading, WindParams());
            put(static_cast<F32>(std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t2).count()));
            for (const WindCell& c : s->mCells)
            {
                const F32 v = (c.mFlags & WindCell::BLOCKED) ? -1.f : c.factor();
                put(v);
                put(c.mFlags);
            }
        }
    }

    TEST_CASE("skroomswind: air whose enclosure is UNKNOWN is calm and never a courtyard", "[skroomswind]")
    {
        // The closed room of test 4 with sealing off: no way in, so its air has no answer.
        Scene s;
        s.wall(4.f, 4.f, 16.f, 4.25f);
        s.wall(4.f, 15.75f, 16.f, 16.f);
        s.wall(4.f, 4.f, 4.25f, 16.f);
        s.wall(15.75f, 4.f, 16.f, 16.f);
        s.box(4.f, 4.f, 16.f, 16.f, 3.f, 3.25f);
        auto in = s.input(1);
        in->mParams.mSealedIndoors = false;
        const Wind w(in);
        Enclosure e;
        { INFO("the room's air is UNKNOWN"); REQUIRE((w.mEnc->enclosureAt(LLVector3(10.f, 10.f, 0.5f), e) && e.mBucket == EnclosureBucket::UNKNOWN)); }
        for (F32 heading : { 0.f, 135.f })
        {
            const WindSlabPtr slab = w.slab(heading);
            const U8 f = flags(slab, 10.f, 10.f);
            { INFO("flagged UNKNOWN"); REQUIRE(((f & WindCell::UNKNOWN) != 0)); }
            { INFO("not INDOORS"); REQUIRE(((f & WindCell::INDOORS) == 0)); }
            { INFO("no courtyard"); REQUIRE(((f & WindCell::COURTYARD) == 0)); }
            { INFO("calm"); REQUIRE((factor(slab, 10.f, 10.f) < 0.1f)); }
            { INFO("windy outside"); REQUIRE((factor(slab, 25.f, 25.f) > 0.5f)); }
            { INFO("outside is known"); REQUIRE(((flags(slab, 25.f, 25.f) & WindCell::UNKNOWN) == 0)); }
        }
    }
}
