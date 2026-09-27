/**
 * @file sknavwind_test.cpp
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

#include "../test/lltut.h"

#include "../sknavwind.h"
#include "sknavtestscene.h"

#include "llstring.h"

#include <chrono>
#include <cmath>
#include <fstream>

namespace
{
    using namespace sknav;
    using namespace sknav_test;

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

namespace tut
{
    struct sknavwind_data
    {
    };
    typedef test_group<sknavwind_data> sknavwind_group;
    typedef sknavwind_group::object sknavwind_object;
    tut::sknavwind_group sknavwind_tests("sknavwind");

    template<> template<>
    void sknavwind_object::test<1>()
    {
        set_test_name("a lone box casts a lee of 8 heights");
        // 5.5 m tall: 4 m above the slab, so the lee recovers over 32 m.
        Scene s;
        s.box(20.f, 26.f, 24.f, 38.f, 0.f, 5.5f, false);
        const Wind w(s.input(4, 2));
        const WindSlabPtr e = w.slab(0.f);
        ensure("solid at the slab", (flags(e, 22.f, 32.f) & WindCell::BLOCKED) != 0);
        const F32 at8 = factor(e, 32.25f, 32.25f), at24 = factor(e, 48.25f, 32.25f), at40 = factor(e, 64.25f, 32.25f);
        ensure("sheltered at 8 m", at8 > 0.15f && at8 < 0.4f);
        ensure("recovering at 24 m", at24 > 0.6f && at24 < 0.9f);
        ensure("free at 40 m", at40 > 0.9f);
        ensure("free upwind", factor(e, 8.25f, 32.25f) > 0.9f);
    }

    template<> template<>
    void sknavwind_object::test<2>()
    {
        set_test_name("a tall box's lee is capped at 60 m");
        Scene s;
        s.box(10.f, 26.f, 14.f, 38.f, 0.f, 30.f, false);
        const Wind w(s.input(4, 2));
        const WindSlabPtr e = w.slab(0.f);
        // Uncapped, 8 x 28.5 m would leave 30 m at a fifth of the stream.
        const F32 at30 = factor(e, 44.25f, 32.25f), at62 = factor(e, 76.25f, 32.25f);
        ensure("half at 30 m", at30 > 0.4f && at30 < 0.65f);
        ensure("free past 60 m", at62 > 0.9f);
    }

    template<> template<>
    void sknavwind_object::test<3>()
    {
        set_test_name("a gap between two boxes speeds up");
        Scene s;
        s.box(24.f, 10.f, 34.f, 30.5f, 0.f, 6.f, false);
        s.box(24.f, 33.5f, 34.f, 54.f, 0.f, 6.f, false);
        const Wind w(s.input(2, 2));
        const WindSlabPtr e = w.slab(0.f);
        const F32 gap = factor(e, 32.25f, 32.25f);
        ensure("faster than the stream", gap > 1.3f);
        ensure("faster than open ground", gap > factor(e, 8.25f, 60.25f) + 0.3f);
    }

    template<> template<>
    void sknavwind_object::test<4>()
    {
        set_test_name("a closed room is calm");
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
            ensure("indoors", (flags(e, 10.f, 10.f) & WindCell::INDOORS) != 0);
            ensure("calm", factor(e, 10.f, 10.f) < 0.1f);
            ensure("windy outside", factor(e, 25.f, 25.f) > 0.5f);
        }
    }

    template<> template<>
    void sknavwind_object::test<5>()
    {
        set_test_name("a doorway between open and closed sides draughts");
        Scene s = twoRooms();
        const Wind w(s.input(1));
        const WindSlabPtr e = w.slab(0.f);
        const WindCell* c = e->at(2.75f, 5.5f);
        ensure("a cell inside the doorway", c != nullptr);
        ensure("draughts", (c->mFlags & WindCell::DRAUGHT) != 0 && c->factor() > 0.15f);
        const LLVector2 flow = LLVector2(1.f, 0.f) + c->deflection();
        ensure("inward, with the wind", flow.mV[VX] > 0.5f);
        ensure("calm deep inside", factor(e, 7.f, 8.5f) < 0.1f);
        ensure("calm in the closed room", factor(e, 14.f, 6.f) < 0.1f);
    }

    template<> template<>
    void sknavwind_object::test<6>()
    {
        set_test_name("a walled courtyard is damped and swirls");
        Scene s;
        s.wall(6.f, 6.f, 26.f, 6.25f);
        s.wall(6.f, 25.75f, 26.f, 26.f);
        s.wall(6.f, 6.f, 6.25f, 26.f);
        s.wall(25.75f, 6.f, 26.f, 26.f);
        const Wind w(s.input(1));
        const WindSlabPtr e = w.slab(0.f);
        const WindCell* c = e->at(22.f, 16.5f);
        ensure("a courtyard", c && (c->mFlags & WindCell::COURTYARD) != 0);
        ensure("damped", c->factor() < 0.6f);
        ensure("swirling", c->deflection().length() > 0.2f);
        ensure("open ground is not", (flags(e, 29.f, 29.f) & WindCell::COURTYARD) == 0);
    }

    template<> template<>
    void sknavwind_object::test<7>()
    {
        set_test_name("rotating the heading rotates the result");
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
        ensure("the same lee, turned", worst < 0.05f);
        ensure("sheltered behind", factor(north, 32.25f, 40.25f) < 0.5f);
        ensure("not beside", factor(north, 40.25f, 32.25f) > 0.8f);
    }

    template<> template<>
    void sknavwind_object::test<8>()
    {
        set_test_name("a band follows the ground, and a pass builds every heading");
        Scene s;
        s.box(0.f, 0.f, 64.f, 32.f, 0.f, 4.f);      // a raised ground, as terrain
        s.mGround[2] = 0.f;
        auto in = std::make_shared<WindInput>();
        const Wind w(s.input(2));
        in->mEnclosure = w.mEnc;
        in->mRooms = w.mRooms;
        in->mBands = { WindLevel() };
        const WindProductPtr p = computeWind(in);
        ensure_equals("16 slabs", p->mSlabs.size(), static_cast<size_t>(WIND_HEADINGS));
        const WindSlabPtr e = p->slab(-16, 0);
        ensure("heading wraps", e && e->mHeading == 0);
        const WindCell* c = e->at(40.f, 16.f);
        ensure("open air above the platform", c && !(c->mFlags & WindCell::BLOCKED));
    }

    template<> template<>
    void sknavwind_object::test<9>()
    {
        set_test_name("replay a mesh's air into wind, when a file is named");
        // SKNAV_WIND_REPLAY names a rooms replay file; SKNAV_WIND_Z the slab's global height.
        const std::string path = LLStringUtil::getenv("SKNAV_WIND_REPLAY");
        if (path.empty())
        {
            return;
        }
        std::shared_ptr<EnclosureInput> enc_in;
        std::shared_ptr<RoomsInput> rooms;
        std::vector<LLVector3d> points;
        ensure("read a replay file", readReplay(path, enc_in, rooms, points));
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
        const WindFieldPtr f = buildWindField(*enc, r.get(), level, WindParams());
        const auto t1 = std::chrono::steady_clock::now();
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
}
