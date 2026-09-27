/**
 * @file sknavflights_test.cpp
 * @brief The slope partition on the oracle's fixtures: flights, landings, a curb, a spiral, folded ground.
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
#include "../sknavflights.h"

#include "llstring.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace
{
    using namespace sknav;

    const U8 OBJECT = 2;

    /// A triangle soup as the oracle builds its fixtures, SL axes.
    struct Soup
    {
        std::vector<LLVector3> mV;
        std::vector<S32> mT;

        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c)
        {
            const S32 n = static_cast<S32>(mV.size());
            mV.insert(mV.end(), { a, b, c });
            mT.insert(mT.end(), { n, n + 1, n + 2 });
        }
        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
        {
            tri(a, b, c);
            tri(a, c, d);
        }
        void box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
        {
            typedef LLVector3 P;
            quad(P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1));
            quad(P(x0, y0, z0), P(x0, y1, z0), P(x1, y1, z0), P(x1, y0, z0));
            quad(P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1));
            quad(P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1));
            quad(P(x1, y1, z0), P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1));
            quad(P(x0, y1, z0), P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1));
        }
        void prism(const std::vector<LLVector2>& poly, F32 z0, F32 z1)
        {
            const size_t k = poly.size();
            const auto at = [](const LLVector2& p, F32 z) { return LLVector3(p.mV[0], p.mV[1], z); };
            for (size_t i = 1; i + 1 < k; ++i)
            {
                tri(at(poly[0], z1), at(poly[i], z1), at(poly[i + 1], z1));
                tri(at(poly[0], z0), at(poly[i + 1], z0), at(poly[i], z0));
            }
            for (size_t i = 0; i < k; ++i)
            {
                const LLVector2 &a = poly[i], &b = poly[(i + 1) % k];
                quad(at(a, z0), at(b, z0), at(b, z1), at(a, z1));
            }
        }
        template <typename Fn> void grid(F32 x0, F32 y0, F32 x1, F32 y1, F32 step, Fn&& f)
        {
            const S32 nx = static_cast<S32>(std::lround((x1 - x0) / step)), ny = static_cast<S32>(std::lround((y1 - y0) / step));
            const S32 base = static_cast<S32>(mV.size());
            for (S32 j = 0; j <= ny; ++j)
            {
                for (S32 i = 0; i <= nx; ++i)
                {
                    const F32 x = x0 + i * step, y = y0 + j * step;
                    mV.emplace_back(x, y, f(x, y));
                }
            }
            const auto idx = [&](S32 i, S32 j) { return base + j * (nx + 1) + i; };
            for (S32 j = 0; j < ny; ++j)
            {
                for (S32 i = 0; i < nx; ++i)
                {
                    mT.insert(mT.end(), { idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j), idx(i + 1, j + 1), idx(i, j + 1) });
                }
            }
        }

        /// Walkable by the live 74 degree slope, as the oracle marks it.
        SlopePartition partition(F32 climb = 0.75f) const
        {
            const size_t n = mT.size() / 3;
            std::vector<U8> areas(n, 0), two(n, 0);
            const F32 up = std::cos(74.f * DEG_TO_RAD);
            for (size_t t = 0; t < n; ++t)
            {
                LLVector3 nn = (mV[mT[t * 3 + 1]] - mV[mT[t * 3]]) % (mV[mT[t * 3 + 2]] - mV[mT[t * 3]]);
                nn.normVec();
                areas[t] = nn.mV[VZ] >= up ? OBJECT : 0;
            }
            return partitionSlopes(mV, mT, areas, two, OBJECT, climb);
        }
    };

    Soup straight(S32 steps = 8, F32 rise = 0.25f, F32 going = 0.3f, F32 width = 1.5f)
    {
        Soup s;
        s.box(-4, -2, -0.5f, 0, 2, 0);
        for (S32 k = 1; k <= steps; ++k)
        {
            s.box((k - 1) * going, -width / 2, 0, k * going, width / 2, k * rise);
        }
        const F32 x = steps * going;
        s.box(x, -2, 0, x + 3, 2, (steps + 1) * rise);
        return s;
    }

    Soup midflight()
    {
        Soup s;
        s.box(-4, -2, -0.5f, 0, 2, 0);
        F32 z = 0, x = 0;
        for (S32 k = 0; k < 5; ++k)
        {
            z += 0.25f;
            s.box(x, -0.75f, 0, x + 0.3f, 0.75f, z);
            x += 0.3f;
        }
        z += 0.25f;
        s.box(x, -0.75f, 0, x + 1.5f, 0.75f, z);
        x += 1.5f;
        for (S32 k = 0; k < 5; ++k)
        {
            z += 0.25f;
            s.box(x, -0.75f, 0, x + 0.3f, 0.75f, z);
            x += 0.3f;
        }
        s.box(x, -2, 0, x + 3, 2, z + 0.25f);
        return s;
    }

    Soup curb()
    {
        Soup s;
        s.box(-4, -4, -0.5f, 0, 4, 0);
        s.box(0, -4, 0, 3, 4, 0.15f);
        s.box(3, -4, 0, 3.2f, 4, 0.35f);
        s.box(3.2f, -4, 0, 6, 4, 0.5f);
        return s;
    }

    Soup ramp()
    {
        Soup s;
        s.box(-4, -2, -0.5f, 0, 2, 0);
        s.quad(LLVector3(0, -0.75f, 0), LLVector3(4, -0.75f, 1.5f), LLVector3(4, 0.75f, 1.5f), LLVector3(0, 0.75f, 0));
        s.box(4, -2, 0, 7, 2, 1.5f);
        return s;
    }

    Soup spiral(S32 steps = 12, F32 turn = 30.f, F32 r0 = 0.3f, F32 r1 = 1.6f, F32 rise = 0.22f)
    {
        Soup s;
        s.box(-3, -3, -0.5f, 3, 0, 0);
        for (S32 k = 0; k < steps; ++k)
        {
            const F32 a0 = k * turn * DEG_TO_RAD, a1 = (k + 1) * turn * DEG_TO_RAD;
            s.prism({ LLVector2(r0 * std::cos(a0), r0 * std::sin(a0)), LLVector2(r1 * std::cos(a0), r1 * std::sin(a0)),
                      LLVector2(r1 * std::cos(a1), r1 * std::sin(a1)), LLVector2(r0 * std::cos(a1), r0 * std::sin(a1)) },
                     0, (k + 1) * rise);
        }
        const F32 a = steps * turn * DEG_TO_RAD, c = std::cos(a), sn = std::sin(a);
        s.prism({ LLVector2(r0 * c, r0 * sn), LLVector2(3 * c, 3 * sn), LLVector2(3 * c - 2 * sn, 3 * sn + 2 * c),
                  LLVector2(r0 * c - 2 * sn, r0 * sn + 2 * c) },
                0, (steps + 1) * rise);
        return s;
    }

    Soup compound()
    {
        Soup s;
        const F32 t35 = std::tan(35.f * DEG_TO_RAD);
        s.grid(0, 0, 25, 10, 0.5f, [&](F32 x, F32 y) {
            if (x < 10)
            {
                return t35 * std::fabs(y - 5) * 0.5f + 0.1f * x;
            }
            if (x < 20)
            {
                return 0.9f * std::sin((x - 10) * F_PI / 5) * 0.5f + 0.3f * std::sin(y * F_PI / 5);
            }
            return 0.5f;
        });
        return s;
    }

    RecordPtr boxRecord(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
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

    /// Flat land under tile (0, 0), both classes, the weather class unshrunk.
    TileBuildInput stairTile()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 6.f;
        in.mCell = 0.25f;
        in.mPartition = Partition::SLOPE;
        in.mClasses[1] = true;
        in.mParams[1].mHeight = 0.5f;
        in.mParams[1].mRadius = 0.f;
        TerrainWindow t;
        t.mOrigin.set(-2.f, -2.f, 0.f);
        t.mWidth = t.mHeight = 37;
        t.mZ.assign(37 * 37, 0.f);
        t.mWater = -10.f;
        in.mTerrain = t;
        return in;
    }

    std::vector<size_t> treadCounts(const SlopePartition& p)
    {
        std::vector<size_t> n;
        for (const Flight& f : p.mFlights)
        {
            n.push_back(f.mTreads.size());
        }
        std::sort(n.begin(), n.end());
        return n;
    }
}

namespace tut
{
    struct sknavflights_data
    {
    };
    typedef test_group<sknavflights_data> sknavflights_group;
    typedef sknavflights_group::object sknavflights_object;
    tut::sknavflights_group sknavflights_tests("sknavflights");

    template<> template<>
    void sknavflights_object::test<1>()
    {
        set_test_name("a straight flight is one flight of eight treads between two landings");
        const SlopePartition p = straight().partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        ensure_equals("landings", p.mLandings, 2);
        const Flight& f = p.mFlights[0];
        ensure_equals("treads", f.mTreads.size(), 8u);
        for (size_t k = 0; k < f.mTreads.size(); ++k)
        {
            const FlightTread& t = f.mTreads[k];
            ensure(llformat("tread %d at %.3f", (S32)k, t.mZ), std::fabs(t.mZ - 0.25f * (k + 1)) < 1e-3f);
            ensure(llformat("rise %.3f", t.mRise), std::fabs(t.mRise - 0.25f) < 1e-3f);
            // the nosing is the tread's lower edge, across the flight at x = 0.3 k
            ensure(llformat("nosing x %.3f", t.mNosingA.mV[VX]), std::fabs(t.mNosingA.mV[VX] - 0.3f * k) < 1e-3f);
            ensure("nosing across", std::fabs(t.mNosingA.mV[VX] - t.mNosingB.mV[VX]) < 1e-3f);
        }
        ensure(llformat("plane climbs along x: %.3f %.3f", f.mPlane[0], f.mPlane[1]),
               std::fabs(f.mPlane[0] - 0.25f / 0.3f) < 0.01f && std::fabs(f.mPlane[1]) < 0.01f);
        ensure(llformat("mean rise %.3f", f.mRise), std::fabs(f.mRise - 0.25f) < 1e-3f);
        ensure("footprint", f.mFootprint.size() >= 4);
        // treads share their flight's region; landings keep their own
        std::set<S32> regions;
        for (size_t t = 0; t < p.mRegion.size(); ++t)
        {
            if (p.mRegion[t] >= 0)
            {
                regions.insert(p.mRegion[t]);
            }
        }
        ensure_equals("regions: flight and two landings", regions.size(), 3u);
    }

    template<> template<>
    void sknavflights_object::test<2>()
    {
        set_test_name("a mid-flight landing splits the stair into two flights");
        const SlopePartition p = midflight().partition();
        ensure("two flights of five", treadCounts(p) == std::vector<size_t>({ 5, 5 }));
        ensure_equals("landings", p.mLandings, 3);
    }

    template<> template<>
    void sknavflights_object::test<3>()
    {
        set_test_name("a wide stair's treads are treads by depth, not area");
        const SlopePartition p = straight(6, 0.2f, 0.3f, 4.f).partition();
        ensure("one flight of six", treadCounts(p) == std::vector<size_t>({ 6 }));
        ensure_equals("landings", p.mLandings, 2);
    }

    template<> template<>
    void sknavflights_object::test<4>()
    {
        set_test_name("a curb top stays a landing; only the threshold between two risers is a tread");
        const SlopePartition p = curb().partition();
        ensure("one flight of one", treadCounts(p) == std::vector<size_t>({ 1 }));
        ensure_equals("road, sidewalk and porch are landings", p.mLandings, 3);
        ensure(llformat("the tread is the threshold at %.2f", p.mFlights[0].mTreads[0].mZ), std::fabs(p.mFlights[0].mTreads[0].mZ - 0.35f) < 1e-3f);
    }

    template<> template<>
    void sknavflights_object::test<5>()
    {
        set_test_name("a ramp is a slope group, not a flight");
        const SlopePartition p = ramp().partition();
        ensure_equals("flights", p.mFlights.size(), 0u);
        ensure_equals("landings", p.mLandings, 2);
        ensure_equals("slopes", p.mSlopes, 1);
    }

    template<> template<>
    void sknavflights_object::test<6>()
    {
        set_test_name("a spiral is one flight of twelve treads");
        const SlopePartition p = spiral().partition();
        ensure("one flight of twelve", treadCounts(p) == std::vector<size_t>({ 12 }));
        ensure_equals("landings", p.mLandings, 2);
        for (const FlightTread& t : p.mFlights[0].mTreads)
        {
            ensure(llformat("rise %.3f", t.mRise), std::fabs(t.mRise - 0.22f) < 1e-3f);
        }
    }

    template<> template<>
    void sknavflights_object::test<7>()
    {
        set_test_name("compound folded ground has no false flights");
        const SlopePartition p = compound().partition();
        ensure_equals("flights", p.mFlights.size(), 0u);
        ensure_equals("treads", p.mTreads, 0);
        ensure_equals("slope groups, as the oracle", p.mSlopes, 4);
        ensure_equals("level groups, as the oracle", p.mLandings, 5);
    }

    template<> template<>
    void sknavflights_object::test<8>()
    {
        set_test_name("touching regions get different codes, and codes keep the area order");
        Soup s = straight();
        const size_t objects = s.mT.size() / 3;
        s.box(10, -2, -0.5f, 14, 2, 0);  // stands in for ground
        const size_t n = s.mT.size() / 3;
        std::vector<U8> areas(n, 0), two(n, 0);
        for (size_t t = 0; t < n; ++t)
        {
            LLVector3 nn = (s.mV[s.mT[t * 3 + 1]] - s.mV[s.mT[t * 3]]) % (s.mV[s.mT[t * 3 + 2]] - s.mV[s.mT[t * 3]]);
            nn.normVec();
            areas[t] = nn.mV[VZ] > 0.5f ? (t < objects ? OBJECT : 1) : 0;
        }
        const SlopePartition p = partitionSlopes(s.mV, s.mT, areas, two, OBJECT, 0.75f);
        for (size_t c = 2; c < p.mReal.size(); ++c)
        {
            ensure("code order is area order", p.mReal[c] >= p.mReal[c - 1]);
        }
        const S32 flight = p.mRegion[0 + 12 * 1]; // the first tread box's top
        S32 bottom = -1, top = -1;
        U8 flight_code = 0, bottom_code = 0, top_code = 0, ground_code = 0;
        for (size_t t = 0; t < n; ++t)
        {
            if (!p.mCodes[t])
            {
                continue;
            }
            const LLVector3 c = (s.mV[s.mT[t * 3]] + s.mV[s.mT[t * 3 + 1]] + s.mV[s.mT[t * 3 + 2]]) / 3.f;
            if (t >= objects)
            {
                ground_code = p.mCodes[t];
            }
            else if (c.mV[VX] < 0)
            {
                bottom = p.mRegion[t];
                bottom_code = p.mCodes[t];
            }
            else if (c.mV[VX] > 2.4f)
            {
                top = p.mRegion[t];
                top_code = p.mCodes[t];
            }
            else
            {
                flight_code = p.mCodes[t];
            }
        }
        ensure("three regions", flight >= 0 && bottom >= 0 && top >= 0 && flight != bottom && flight != top);
        ensure("the flight differs from both landings", flight_code != bottom_code && flight_code != top_code);
        ensure_equals("the ground's code is the ground's", p.mReal[ground_code], (U8)1);
        ensure("the flight's code is an object's", p.mReal[flight_code] == OBJECT);
        ensure_equals("the flight carries its code", p.mFlights[0].mCode, flight_code);
    }

    template<> template<>
    void sknavflights_object::test<9>()
    {
        set_test_name("polys map to a flight by code over its footprint");
        SlopePartition p = straight().partition();
        const U8 code = p.mFlights[0].mCode;
        const std::vector<LLVector3> centroids = { LLVector3(1.2f, 0.f, 1.f), LLVector3(1.2f, 0.f, 1.f), LLVector3(-2.f, 0.f, 0.f),
                                                   LLVector3(1.2f, 5.f, 1.f), LLVector3(1.2f, 0.f, 9.f) };
        const std::vector<U8> codes = { code, static_cast<U8>(code + 1), code, code, code };
        mapFlightPolys(p.mFlights, centroids, codes, 0.75f);
        ensure_equals("flight kept", p.mFlights.size(), 1u);
        ensure("only the poly on the flight", p.mFlights[0].mPolys == std::vector<U16>({ 0 }));
        mapFlightPolys(p.mFlights, { LLVector3(-2.f, 0.f, 0.f) }, { code }, 0.75f);
        ensure_equals("a flight with no polys is dropped", p.mFlights.size(), 0u);
    }

    // Offline replay for the oracle diff: SKNAV_FLIGHTS_REPLAY names a text
    // file of soups ("soup <name>" then one triangle of nine floats per line),
    // and each soup's partition goes to the file after it, "<file>.out".
    template<> template<>
    void sknavflights_object::test<10>()
    {
        set_test_name("replay soups from a file, when one is named");
        const std::string path = LLStringUtil::getenv("SKNAV_FLIGHTS_REPLAY");
        if (path.empty())
        {
            return;
        }
        std::ifstream in(path);
        std::ofstream out(path + ".out");
        ensure("replay file", in.good() && out.good());
        std::string word, name;
        Soup s;
        const auto flush = [&]() {
            if (name.empty())
            {
                return;
            }
            const SlopePartition p = s.partition();
            out << "soup " << name << ' ' << s.mT.size() / 3 << " slopes " << p.mSlopes << " landings " << p.mLandings << " treads "
                << p.mTreads << " flights " << p.mFlights.size() << '\n';
            for (const Flight& f : p.mFlights)
            {
                out << "flight " << f.mTreads.size() << ' ' << f.mRise << ' ' << f.mZMin << ' ' << f.mZMax;
                for (const FlightTread& t : f.mTreads)
                {
                    const LLVector3 m = (t.mNosingA + t.mNosingB) * 0.5f;
                    out << ' ' << m.mV[VX] << ' ' << m.mV[VY] << ' ' << t.mZ;
                }
                out << '\n';
            }
            s = Soup();
        };
        while (in >> word)
        {
            if (word == "soup")
            {
                flush();
                in >> name;
                continue;
            }
            F32 v[9];
            v[0] = std::stof(word);
            for (S32 k = 1; k < 9; ++k)
            {
                in >> v[k];
            }
            s.tri(LLVector3(v[0], v[1], v[2]), LLVector3(v[3], v[4], v[5]), LLVector3(v[6], v[7], v[8]));
        }
        flush();
    }
    template<> template<>
    void sknavflights_object::test<11>()
    {
        set_test_name("a tile build publishes the flight with the polys it produced");
        TileBuildInput in = stairTile();
        // eight 0.25 m steps on 0.3 m treads along x, from 10 m, then a landing
        for (S32 k = 1; k <= 8; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 14.f, 0.f, 10.f + k * 0.3f, 18.f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(12.4f, 12.f, 0.f, 17.f, 20.f, 2.25f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        ensure_equals("one flight", r.mFlights[0].size(), 1u);
        const Flight& f = r.mFlights[0][0];
        ensure_equals("eight treads", f.mTreads.size(), 8u);
        ensure("polys", !f.mPolys.empty());
        for (const U16 p : f.mPolys)
        {
            ensure("a poly index in the tile", p < r.mPolys[0]);
        }
        ensure_equals("the other partitions publish none", buildTile([&] { TileBuildInput w = in; w.mPartition = Partition::WATERSHED; return w; }()).mFlights[0].size(), 0u);
    }

    template<> template<>
    void sknavflights_object::test<12>()
    {
        set_test_name("a short narrow flight is smaller than a sliver and still survives");
        TileBuildInput in = stairTile();
        // three 0.25 m steps on 0.3 m treads, 0.9 m wide, then a landing
        for (S32 k = 1; k <= 3; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 15.5f, 0.f, 10.f + k * 0.3f, 16.4f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(10.9f, 13.f, 0.f, 15.f, 19.f, 1.f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        const S32 w = static_cast<S32>(AgentClass::WEATHER);
        ensure_equals("one flight", r.mFlights[w].size(), 1u);
        ensure_equals("three treads", r.mFlights[w][0].mTreads.size(), 3u);
        ensure("polys", !r.mFlights[w][0].mPolys.empty());
    }
}
