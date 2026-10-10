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

#include "Recast.h"
#include "RecastAlloc.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace
{
    using namespace sknav;

    const U8 OBJECT = 2;
    const F32 WALK_SLOPE_DEG = 74.f;    ///< AVATAR's slope, confirmed in-world

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

        /// Walkable by the live 74 degree slope, as the oracle marks it; or marked at the air's steeper slope, as the build does.
        SlopePartition partition(F32 climb = 0.75f, F32 marked_deg = WALK_SLOPE_DEG) const
        {
            const size_t n = mT.size() / 3;
            std::vector<U8> areas(n, 0), two(n, 0);
            const F32 up = std::cos(marked_deg * DEG_TO_RAD);
            for (size_t t = 0; t < n; ++t)
            {
                LLVector3 nn = (mV[mT[t * 3 + 1]] - mV[mT[t * 3]]) % (mV[mT[t * 3 + 2]] - mV[mT[t * 3]]);
                nn.normVec();
                areas[t] = nn.mV[VZ] >= up ? OBJECT : 0;
            }
            return partitionSlopes(mV, mT, areas, two, OBJECT, climb, WALK_SLOPE_DEG);
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
        s.box(x, -2.f, 0, x + 1.5f, 2.f, z);
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

    Soup ramp(F32 run = 4.f, F32 rise = 1.5f)
    {
        Soup s;
        s.box(-4, -2, -0.5f, 0, 2, 0);
        s.quad(LLVector3(0, -0.75f, 0), LLVector3(run, -0.75f, rise), LLVector3(run, 0.75f, rise), LLVector3(0, 0.75f, 0));
        s.box(run, -2, 0, run + 3, 2, rise);
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
        TerrainWindow t;
        t.mOrigin.set(-2.f, -2.f, 0.f);
        t.mWidth = t.mHeight = 37;
        t.mZ.assign(37 * 37, 0.f);
        t.mWater = -10.f;
        in.mTerrain = t;
        return in;
    }

    std::vector<F32> rises(const SlopePartition& p)
    {
        std::vector<F32> n;
        for (const Flight& f : p.mFlights)
        {
            n.push_back(std::round(f.mRise * 100.f) / 100.f);
        }
        std::sort(n.begin(), n.end());
        return n;
    }

    bool near3(const LLVector3& a, const LLVector3& b, F32 tol = 0.2f)
    {
        return (a - b).length() < tol;
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
        set_test_name("a straight flight is one stair from the foot of its treads to the landing at their top");
        const SlopePartition p = straight().partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        ensure_equals("landings", p.mLandings, 2);
        const Flight& f = p.mFlights[0];
        ensure("a stair by its treads", f.mKind == ConnectorKind::STAIR && f.mRule == ConnectorRule::TREADS && f.mTreads == 8);
        ensure(llformat("bottom %.2f %.2f %.2f", f.mBottom.mV[VX], f.mBottom.mV[VY], f.mBottom.mV[VZ]), near3(f.mBottom, LLVector3(0.f, 0.f, 0.f)));
        ensure(llformat("top %.2f %.2f %.2f", f.mTop.mV[VX], f.mTop.mV[VY], f.mTop.mV[VZ]), near3(f.mTop, LLVector3(2.4f, 0.f, 2.25f)));
        ensure(llformat("rise %.3f", f.mRise), std::fabs(f.mRise - 2.25f) < 1e-3f);
        ensure(llformat("bottom edge across the flight: %.2f", (f.mBottomB - f.mBottomA).length()),
               std::fabs((f.mBottomB - f.mBottomA).length() - 1.5f) < 0.1f && std::fabs(f.mBottomA.mV[VX] - f.mBottomB.mV[VX]) < 0.05f);
        ensure(llformat("plane climbs along x: %.3f %.3f", f.mPlane[0], f.mPlane[1]),
               std::fabs(f.mPlane[0] - 2.25f / 2.4f) < 0.05f && std::fabs(f.mPlane[1]) < 0.01f);
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
        ensure("two flights of a metre and a half", rises(p) == std::vector<F32>({ 1.5f, 1.5f }));
        ensure_equals("landings", p.mLandings, 3);
    }

    template<> template<>
    void sknavflights_object::test<3>()
    {
        set_test_name("a wide stair's treads are treads by depth, not area");
        const SlopePartition p = straight(6, 0.2f, 0.3f, 4.f).partition();
        ensure("one flight of six", p.mFlights.size() == 1 && p.mFlights[0].mTreads == 6 && rises(p) == std::vector<F32>({ 1.4f }));
        ensure_equals("landings", p.mLandings, 2);
    }

    template<> template<>
    void sknavflights_object::test<4>()
    {
        set_test_name("a curb top stays a landing, and a climb that small is no stair");
        const SlopePartition p = curb().partition();
        ensure("one tread: the threshold between the two risers", p.mTreads == 1);
        ensure_equals("road, sidewalk and porch are landings", p.mLandings, 3);
        ensure("a step that size is no flight", p.mFlights.empty());
    }

    template<> template<>
    void sknavflights_object::test<5>()
    {
        set_test_name("a gentle slope group between two floors is a ramp");
        const SlopePartition p = ramp().partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        const Flight& f = p.mFlights[0];
        ensure("a ramp by its grade", f.mKind == ConnectorKind::RAMP && f.mRule == ConnectorRule::GRADE);
        ensure("from foot to top", near3(f.mBottom, LLVector3(0.f, 0.f, 0.f)) && near3(f.mTop, LLVector3(4.f, 0.f, 1.5f)));
        ensure_equals("landings", p.mLandings, 2);
        ensure_equals("slopes", p.mSlopes, 1);
    }

    template<> template<>
    void sknavflights_object::test<6>()
    {
        set_test_name("a spiral has no straight run, so it makes no flight");
        const SlopePartition p = spiral().partition();
        ensure_equals("treads", p.mTreads, 12);
        ensure_equals("landings", p.mLandings, 2);
        ensure_equals("flights", p.mFlights.size(), 0u);
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
        const SlopePartition p = partitionSlopes(s.mV, s.mT, areas, two, OBJECT, 0.75f, WALK_SLOPE_DEG);
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
        const std::vector<U8>& fc = p.mFlights[0].mCodes;
        ensure("the flight carries its code", std::find(fc.begin(), fc.end(), flight_code) != fc.end());
    }

    template<> template<>
    void sknavflights_object::test<9>()
    {
        set_test_name("polys map to a flight by its codes over its footprint");
        SlopePartition p = straight().partition();
        const U8 code = p.mFlights[0].mCodes.front();
        U8 other = 1;
        while (std::find(p.mFlights[0].mCodes.begin(), p.mFlights[0].mCodes.end(), other) != p.mFlights[0].mCodes.end())
        {
            ++other;
        }
        const std::vector<LLVector3> centroids = { LLVector3(1.2f, 0.f, 1.f), LLVector3(1.2f, 0.f, 1.f), LLVector3(-2.f, 0.f, 0.f),
                                                   LLVector3(1.2f, 5.f, 1.f), LLVector3(1.2f, 0.f, 9.f) };
        const std::vector<U8> codes = { code, other, code, code, code };
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
                out << "flight " << static_cast<S32>(f.mKind) << ' ' << static_cast<S32>(f.mRule) << ' ' << static_cast<S32>(f.mTreads) << ' '
                    << f.mRise << ' ' << f.mRun;
                for (const LLVector3& v : { f.mBottom, f.mTop })
                {
                    out << ' ' << v.mV[VX] << ' ' << v.mV[VY] << ' ' << v.mV[VZ];
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
        ensure("a stair of eight treads", f.mKind == ConnectorKind::STAIR && f.mTreads == 8);
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
        // three 0.25 m steps on 0.4 m treads, 1.2 m wide, then a landing
        for (S32 k = 1; k <= 3; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.4f, 15.4f, 0.f, 10.f + k * 0.4f, 16.6f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(11.2f, 13.f, 0.f, 15.f, 19.f, 1.f));
        // No erosion and little headroom, so only the fragment merge could lose it.
        in.mParams[0].mHeight = 0.5f;
        in.mParams[0].mRadius = 0.f;
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        const S32 w = static_cast<S32>(AgentClass::AVATAR);
        ensure_equals("one flight", r.mFlights[w].size(), 1u);
        ensure_equals("three treads", static_cast<S32>(r.mFlights[w][0].mTreads), 3);
        ensure("polys", !r.mFlights[w][0].mPolys.empty());
    }

    template<> template<>
    void sknavflights_object::test<13>()
    {
        set_test_name("the weld tells apart points a power of two millimetres apart");
        Soup s = straight();
        const Soup other = straight();
        const S32 base = static_cast<S32>(s.mV.size());
        for (const LLVector3& v : other.mV)
        {
            s.mV.push_back(v + LLVector3(2097.152f, 0.f, 0.f));
        }
        for (const S32 i : other.mT)
        {
            s.mT.push_back(base + i);
        }
        const SlopePartition p = s.partition();
        ensure("two flights of eight", p.mFlights.size() == 2 && p.mFlights[0].mTreads == 8 && p.mFlights[1].mTreads == 8);
    }

    template<> template<>
    void sknavflights_object::test<14>()
    {
        set_test_name("running out of codes shares them within an area, and never loses one");
        Soup s = midflight();
        const size_t objects = s.mT.size() / 3;
        s.box(20, -2, -0.5f, 24, 2, 0);  // stands in for submerged ground
        const size_t n = s.mT.size() / 3;
        std::vector<U8> areas(n, 0), two(n, 0);
        for (size_t t = 0; t < n; ++t)
        {
            LLVector3 nn = (s.mV[s.mT[t * 3 + 1]] - s.mV[s.mT[t * 3]]) % (s.mV[s.mT[t * 3 + 2]] - s.mV[s.mT[t * 3]]);
            nn.normVec();
            areas[t] = nn.mV[VZ] > 0.5f ? (t < objects ? OBJECT : 3) : 0;
        }
        const SlopePartition p = partitionSlopes(s.mV, s.mT, areas, two, OBJECT, 0.75f, WALK_SLOPE_DEG, 3);
        ensure_equals("two codes and null", p.mReal.size(), 3u);
        for (size_t t = 0; t < n; ++t)
        {
            if (areas[t])
            {
                ensure_equals("each triangle keeps its area", p.mReal[p.mCodes[t]], areas[t]);
            }
        }
    }

    template<> template<>
    void sknavflights_object::test<15>()
    {
        set_test_name("a needle the weld collapses keeps its area");
        Soup s = straight();
        s.tri(LLVector3(10.f, 0.f, 0.f), LLVector3(12.f, 0.f, 0.f), LLVector3(11.f, 0.0004f, 0.f));
        const SlopePartition p = s.partition();
        const U8 code = p.mCodes.back();
        ensure("the needle has a code", code != 0);
        ensure_equals("of its own area", p.mReal[code], OBJECT);
        ensure_equals("the stair is unchanged", p.mFlights.size(), 1u);
    }

    // Timing for a large soup: SKNAV_FLIGHTS_TIME names a replay file, whose
    // first soup is built as tile (0, 0) for both classes, 16 m in from its
    // corner; the milliseconds go to "<file>.time".
    template<> template<>
    void sknavflights_object::test<16>()
    {
        set_test_name("time a tile build of a large soup, when one is named");
        const std::string path = LLStringUtil::getenv("SKNAV_FLIGHTS_TIME");
        if (path.empty())
        {
            return;
        }
        std::ifstream in(path);
        std::string word;
        in >> word >> word;
        auto tris = std::make_shared<std::vector<LLVector3>>();
        F32 v[3];
        while (in >> v[0] >> v[1] >> v[2])
        {
            tris->emplace_back(v[0] + 16.f, v[1] + 16.f, v[2]);
        }
        auto r = std::make_shared<Record>();
        r->mShape = Shape::TRIANGLES;
        r->mTriangles = tris;
        r->mMin.set(0, 0, -100);
        r->mMax.set(32, 32, 100);
        TileBuildInput tin = stairTile();
        tin.mCell = 0.125f;
        tin.mZMin = -20.f;
        tin.mZMax = 40.f;
        tin.mParams[0].mSlope = 74.f;
        tin.mTopParams.mSlope = 89.f;
        tin.mRecords.push_back(r);
        std::ofstream out(path + ".time");
        for (S32 k = 0; k < 3; ++k)
        {
            const TileBuildResult res = buildTile(tin);
            out << res.mMillis << " ms, " << res.mTriangles << " triangles, " << res.mFlights[0].size() << " flights\n";
        }
    }

    template<> template<>
    void sknavflights_object::test<17>()
    {
        set_test_name("a flight across a tile seam has one key in both tiles");
        std::set<U64> keys;
        size_t copies = 0;
        for (S32 tx = 0; tx < 2; ++tx)
        {
            TileBuildInput in = stairTile();
            in.mTileX = tx;
            in.mTerrain.mWidth = 70;
            in.mTerrain.mZ.assign(70 * 37, 0.f);
            // eight 0.25 m steps on 0.3 m treads along x, across x = 32
            for (S32 k = 1; k <= 8; ++k)
            {
                in.mRecords.push_back(boxRecord(30.8f + (k - 1) * 0.3f, 14.f, 0.f, 30.8f + k * 0.3f, 18.f, k * 0.25f));
            }
            in.mRecords.push_back(boxRecord(33.2f, 12.f, 0.f, 37.f, 20.f, 2.25f));
            const TileBuildResult r = buildTile(in);
            ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
            for (const Flight& f : r.mFlights[0])
            {
                keys.insert(f.mKey);
                ++copies;
            }
        }
        ensure_equals("a copy in each tile", copies, 2u);
        ensure_equals("one key", keys.size(), 1u);
    }

    template<> template<>
    void sknavflights_object::test<18>()
    {
        set_test_name("a flight marks the treads on its plane, not the floor under it");
        AirGrid g;
        g.mOrigin.set(0.f, 0.f, 0.f);
        g.mCell = 0.125f;
        g.mUnit = 0.125f;
        g.mWidth = g.mHeight = 32;
        g.mStart.assign(32 * 32 + 1, 0);
        for (S32 i = 0; i < 32 * 32; ++i)
        {
            const F32 x = (i % 32 + 0.5f) * 0.125f;
            // A floor in the flight's height range but well under its plane.
            AirGap under;
            under.mFloor = 14;
            under.mCeiling = 15;
            under.mFlags = AirGap::TOP;
            AirGap tread;
            tread.mFloor = static_cast<U16>(std::lround((2.f + 0.25f * x) / 0.125f));
            tread.mCeiling = 100;
            tread.mFlags = AirGap::TOP | AirGap::OPEN;
            g.mGaps.push_back(under);
            g.mGaps.push_back(tread);
            g.mStart[i + 1] = static_cast<U32>(g.mGaps.size());
        }
        Flight f;
        f.mFootprint = { LLVector2(0.f, 0.f), LLVector2(4.f, 0.f), LLVector2(4.f, 2.f), LLVector2(0.f, 2.f) };
        f.mPlane[0] = 0.25f;
        f.mPlane[2] = 2.f;
        f.mZMin = 2.f;
        f.mZMax = 3.f;
        f.mRise = 0.125f;
        markFlights(g, { f });
        ensure("a tread inside", (g.gaps(10, 4)[1].mFlags & AirGap::FLIGHT) != 0);
        ensure("not the floor under it", (g.gaps(10, 4)[0].mFlags & AirGap::FLIGHT) == 0);
        ensure("not outside the footprint", (g.gaps(10, 20)[1].mFlags & AirGap::FLIGHT) == 0);
    }

    template<> template<>
    void sknavflights_object::test<19>()
    {
        set_test_name("a tile's air marks its flight's treads, and nothing else");
        TileBuildInput in = stairTile();
        in.mAir = true;
        for (S32 k = 1; k <= 8; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 14.f, 0.f, 10.f + k * 0.3f, 18.f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(12.4f, 12.f, 0.f, 16.f, 20.f, 2.25f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        ensure("a flight", !r.mFlights[0].empty());
        ensure("air", r.mAir != nullptr);
        U32 marked = 0, stray = 0;
        for (const AirLeaf& leaf : r.mAir->mLeaves)
        {
            const F32 x = r.mAir->mOrigin.mV[VX] + (leaf.mX + 0.5f) * r.mAir->mCell;
            for (U32 i = 0; i < leaf.mGapCount; ++i)
            {
                if (r.mAir->mGaps[leaf.mFirstGap + i].mFlags & AirGap::FLIGHT)
                {
                    ++marked;
                    stray += x < 9.5f || x > 13.f ? 1 : 0;
                }
            }
        }
        ensure("treads marked", marked > 0);
        ensure_equals("nothing off the stair", stray, 0u);
    }

    template<> template<>
    void sknavflights_object::test<20>()
    {
        set_test_name("a chain that rises less than half a metre is no staircase");
        Soup s = straight(8, 0.05f);
        const SlopePartition p = s.partition();
        ensure_equals("the shallow treads still chain", p.mTreads, 8);
        ensure_equals("but no flight comes of them", p.mFlights.size(), 0u);
        const SlopePartition real = straight().partition();
        ensure_equals("a staircase still makes its flight", real.mFlights.size(), 1u);
        ensure("it rises half a metre", real.mFlights[0].mZMax - real.mFlights[0].mZMin >= FLIGHT_MIN_RISE_M);
    }

    template<> template<>
    void sknavflights_object::test<21>()
    {
        set_test_name("a ramp sunk into its landing ends where it comes out of it, not on the floor below");
        // The ramp runs on under the landing slab down to a lower floor that it never reaches underfoot.
        Soup s;
        s.box(-5, -2, -0.5f, -0.8f, 2, -0.3f);
        s.box(-0.8f, -2, -0.5f, 1.2f, 2, 0.6f);
        s.quad(LLVector3(-0.8f, -0.75f, -0.3f), LLVector3(4.8f, -0.75f, 1.8f), LLVector3(4.8f, 0.75f, 1.8f), LLVector3(-0.8f, 0.75f, -0.3f));
        s.box(4.8f, -2, 0, 8, 2, 1.8f);
        const SlopePartition p = s.partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        const Flight& f = p.mFlights[0];
        ensure(llformat("bottom on the landing where the ramp reaches it: %.2f %.2f %.2f", f.mBottom.mV[VX], f.mBottom.mV[VY], f.mBottom.mV[VZ]),
               near3(f.mBottom, LLVector3(1.6f, 0.f, 0.6f)));
        ensure(llformat("top %.2f %.2f %.2f", f.mTop.mV[VX], f.mTop.mV[VY], f.mTop.mV[VZ]), near3(f.mTop, LLVector3(4.8f, 0.f, 1.8f)));
    }

    template<> template<>
    void sknavflights_object::test<22>()
    {
        set_test_name("two ramps at right angles from one step are two flights, the step joining neither across the turn");
        Soup s;
        s.box(-6, -6, -0.5f, 8, 8, 0);
        s.box(0, 0, 0, 0.5f, 0.5f, 0.2f);
        s.quad(LLVector3(0.5f, -1.f, 0.2f), LLVector3(4.5f, -1.f, 2.2f), LLVector3(4.5f, 0.5f, 2.2f), LLVector3(0.5f, 0.5f, 0.2f));
        s.box(4.5f, -2.f, 0, 7.f, 1.f, 2.2f);
        s.quad(LLVector3(-1.f, 0.5f, 0.2f), LLVector3(0.5f, 0.5f, 0.2f), LLVector3(0.5f, 4.5f, 2.2f), LLVector3(-1.f, 4.5f, 2.2f));
        s.box(-2.f, 4.5f, 0, 1.f, 7.f, 2.2f);
        const SlopePartition p = s.partition();
        ensure_equals("two flights", p.mFlights.size(), 2u);
        bool along_x = false, along_y = false;
        for (const Flight& f : p.mFlights)
        {
            const LLVector3 run = f.mTop - f.mBottom;
            along_x = along_x || (run.mV[VX] > 3.5f && std::fabs(run.mV[VY]) < 0.2f);
            along_y = along_y || (run.mV[VY] > 3.5f && std::fabs(run.mV[VX]) < 0.2f);
            ensure(llformat("each as wide as its ramp: %.2f", (f.mBottomB - f.mBottomA).length()), std::fabs((f.mBottomB - f.mBottomA).length() - 1.5f) < 0.2f);
        }
        ensure("one up each ramp", along_x && along_y);
    }

    // The flight a stair builds stands on the raster before any polys; moved
    // off its footprint, nothing of its codes is left under it.
    template<> template<>
    void sknavflights_object::test<23>()
    {
        set_test_name("a flight stands on the compact heightfield where its codes are, and not elsewhere");
        TileBuildInput in = stairTile();
        for (S32 k = 1; k <= 8; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 14.f, 0.f, 10.f + k * 0.3f, 18.f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(12.4f, 12.f, 0.f, 17.f, 20.f, 2.25f));
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised: " + r.mError, raster && raster->mChf);
        ensure_equals("one flight", raster->mFlights.size(), 1u);
        ensure("it stands", raster->mStanding.size() == 1 && raster->mStanding[0] == 1);
        const S32 border = borderCells(in.mParams[0].mRadius, in.mCell);
        Flight moved = raster->mFlights[0];
        for (LLVector2& p : moved.mFootprint)
        {
            p.mV[VY] += 8.f;
        }
        ensure("off its footprint it does not", !flightSurvives(*raster->mChf, border, moved, in.mParams[0].mClimb));
        Flight raised = raster->mFlights[0];
        raised.mZMin += 4.f;
        raised.mZMax += 4.f;
        ensure("nor above its heights", !flightSurvives(*raster->mChf, border, raised, in.mParams[0].mClimb));
    }

    namespace
    {
        U64 rasterDigest(const TileRaster& t)
        {
            U64 h = 1469598103934665603ULL;
            const auto eat = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; ++i)
                {
                    h = (h ^ static_cast<const U8*>(p)[i]) * 1099511628211ULL;
                }
            };
            const rcCompactHeightfield& c = *t.mChf;
            eat(c.cells, sizeof(rcCompactCell) * c.width * c.height);
            eat(c.spans, sizeof(rcCompactSpan) * c.spanCount);
            eat(c.areas, c.spanCount);
            eat(t.mEdges.mRuns.data(), t.mEdges.mRuns.size() * sizeof(U16));
            eat(t.mReal.data(), t.mReal.size());
            return h;
        }

        bool sameTile(const TileBuildResult& a, const TileBuildResult& b)
        {
            if (a.mStatus != b.mStatus || a.mData[0] != b.mData[0] || a.mSegments[0].size() != b.mSegments[0].size() ||
                a.mLinks[0].size() != b.mLinks[0].size() || a.mFlights[0].size() != b.mFlights[0].size())
            {
                return false;
            }
            for (size_t i = 0; i < a.mFlights[0].size(); ++i)
            {
                if (a.mFlights[0][i].mPolys != b.mFlights[0][i].mPolys)
                {
                    return false;
                }
            }
            return true;
        }

        TileBuildInput stagedStair()
        {
            TileBuildInput in = stairTile();
            in.mAir = true;
            in.mEdges[0] = true;
            for (S32 k = 1; k <= 8; ++k)
            {
                in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 14.f, 0.f, 10.f + k * 0.3f, 18.f, k * 0.25f));
            }
            in.mRecords.push_back(boxRecord(12.4f, 12.f, 0.f, 17.f, 20.f, 2.25f));
            in.mRecords.push_back(boxRecord(20.f, 4.f, 0.f, 21.f, 28.f, 3.f));
            return in;
        }

        S32 sAllocsLeft = 0;
        void* failingAlloc(size_t size, rcAllocHint)
        {
            return sAllocsLeft-- > 0 ? malloc(size) : nullptr;
        }
        void plainFree(void* p)
        {
            free(p);
        }
    }

    // The raster stage hands the polygon stage a raster it never changes:
    // polygonised twice, or after a polygon stage that failed, it makes the
    // tile the whole pipeline makes.
    template<> template<>
    void sknavflights_object::test<24>()
    {
        set_test_name("a raster polygonises again to the same tile, and is left as it was");
        const TileBuildInput in = stagedStair();
        const TileBuildResult whole = buildTile(in);
        ensure("built: " + whole.mError, whole.mStatus == TileBuildResult::SUCCESS && !whole.mFlights[0].empty());
        TileBuildResult staged;
        const TileRasterPtr raster = rasteriseTile(in, staged);
        ensure("rasterised", raster != nullptr);
        const U64 before = rasterDigest(*raster);
        TileBuildResult first = staged, second = staged;
        polygoniseTile(in, *raster, first);
        polygoniseTile(in, *raster, second);
        ensure("the first is the pipeline's tile", sameTile(first, whole));
        ensure("the second is the first", sameTile(second, first));
        ensure("the raster is as it was", rasterDigest(*raster) == before);
    }

    template<> template<>
    void sknavflights_object::test<25>()
    {
        set_test_name("a raster copy Recast's allocator refuses fails the polygon stage and leaves the raster whole");
        const TileBuildInput in = stagedStair();
        TileBuildResult staged;
        const TileRasterPtr raster = rasteriseTile(in, staged);
        ensure("rasterised", raster != nullptr);
        const U64 before = rasterDigest(*raster);
        // The copy's header, cells, spans and areas: refused at each in turn. Recast's own
        // passes are not written to survive a refusal, so nothing later is.
        bool failed_somewhere = false;
        for (S32 allowed : { 0, 1, 2, 3 })
        {
            TileBuildResult refused = staged;
            sAllocsLeft = allowed;
            rcAllocSetCustom(failingAlloc, plainFree);
            polygoniseTile(in, *raster, refused);
            rcAllocSetCustom(nullptr, nullptr);
            failed_somewhere = failed_somewhere || (refused.mStatus == TileBuildResult::FAILED && refused.mData[0].empty());
            ensure(llformat("the raster is whole after %d allocations", allowed), rasterDigest(*raster) == before);
        }
        ensure("the copy was refused", failed_somewhere);
        TileBuildResult again = staged;
        polygoniseTile(in, *raster, again);
        ensure("and polygonises as before", sameTile(again, buildTile(in)));
    }

    // Every floor AVATAR keeps finds the air's gap over it: a kerb the
    // narrow-step pass lowers by the gap over its own top, a roof by the
    // upper of its column's two gaps.
    template<> template<>
    void sknavflights_object::test<26>()
    {
        set_test_name("each AVATAR span finds the air's gap over the top it was rasterised at");
        TileBuildInput in = stagedStair();
        in.mRecords.push_back(boxRecord(4.05f, 4.f, 0.f, 4.2f, 10.f, 0.5f));     // a kerb inside one column, lowered to the floor
        in.mRecords.push_back(boxRecord(22.f, 4.f, 3.f, 30.f, 12.f, 3.25f));     // a roof over open floor
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised with air", raster && r.mAir);
        ensure("bound to the air", raster->mAir.lock() == r.mAir);
        const rcCompactHeightfield& chf = *raster->mChf;
        ensure_equals("a gap per span", raster->mSpanGap.size(), static_cast<size_t>(chf.spanCount));
        const S32 border = borderCells(in.mParams[0].mRadius, in.mCell);
        const S32 climb = static_cast<S32>(std::floor(in.mParams[0].mClimb / in.mCell));
        S32 spans = 0, roofs = 0, kerbs = 0;
        for (S32 z = border; z < chf.height - border; ++z)
        {
            for (S32 x = border; x < chf.width - border; ++x)
            {
                const rcCompactCell& cell = chf.cells[x + z * chf.width];
                for (U32 i = cell.index; i < cell.index + cell.count; ++i)
                {
                    if (chf.areas[i] == RC_NULL_AREA)
                    {
                        continue;
                    }
                    ++spans;
                    const U32 g = raster->mSpanGap[i];
                    ensure(llformat("span %d,%d has a gap", x, z), g != SPAN_NO_GAP && g < r.mAir->mGaps.size());
                    const AirGap& gap = r.mAir->mGaps[g];
                    const S32 top = chf.spans[i].y;
                    // The gap's floor over the span's own cell: the air's x runs along chf z.
                    AirColumn column;
                    airColumn(*r.mAir, findLeafByKey(*r.mAir, z - border, x - border), column);
                    const U32 k = static_cast<U32>(std::find(column.mGap, column.mGap + column.mCount, g) - column.mGap);
                    ensure(llformat("span %d,%d's gap is over its cell", x, z), k < column.mCount);
                    const S32 floor = column.floorAt(*r.mAir, k, z - border, x - border);
                    // A kerb's gap is over its own top, up to its height above the floor it was lowered to.
                    ensure(llformat("span %d,%d stands under its gap's floor", x, z), floor + 1 >= top && floor <= top + 2 * climb);
                    ensure(llformat("span %d,%d under its gap's ceiling", x, z), top < gap.mCeiling);
                    const F32 sx = chf.bmin[2] + (z + 0.5f) * chf.cs, sy = chf.bmin[0] + (x + 0.5f) * chf.cs;
                    roofs += sx > 23.f && sx < 29.f && sy > 5.f && sy < 11.f && chf.bmin[1] + floor * chf.ch > 3.f ? 1 : 0;
                    kerbs += sx > 4.f && sx < 4.25f && sy > 5.f && sy < 9.f && floor > top ? 1 : 0;
                }
            }
        }
        ensure("spans", spans > 1000);
        ensure("the roof's spans find the upper gap", roofs > 50);
        ensure("the kerb's spans find the gap over the kerb", kerbs > 0);
    }

    template<> template<>
    void sknavflights_object::test<27>()
    {
        set_test_name("a steep slope with no treads and no name is a stair: a boat's stairs simplified to a ramp");
        const SlopePartition p = ramp(3.8f, 1.8f).partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        const Flight& f = p.mFlights[0];
        ensure("a stair by its grade", f.mKind == ConnectorKind::STAIR && f.mRule == ConnectorRule::GRADE);
    }

    template<> template<>
    void sknavflights_object::test<28>()
    {
        set_test_name("a flight's ends span its walkable treads, not its rails; a side the tile cuts off keeps its span");
        // A flight up +x over 4 m whose structure is 6 m wide, rails and all; its polys walk 2 m of it.
        Flight f;
        f.mBottom = LLVector3(10.f, 10.f, 0.f);
        f.mTop = LLVector3(14.f, 10.f, 2.f);
        f.mBottomA = LLVector3(10.f, 7.f, 0.f);
        f.mBottomB = LLVector3(10.f, 13.f, 0.f);
        f.mTopA = LLVector3(14.f, 7.f, 2.f);
        f.mTopB = LLVector3(14.f, 13.f, 2.f);
        f.mPolys = { 0 };
        const std::vector<std::vector<LLVector3>> polys = { { LLVector3(10.f, 9.f, 0.f), LLVector3(14.f, 9.f, 2.f), LLVector3(14.f, 11.f, 2.f), LLVector3(10.f, 11.f, 0.f) } };
        std::vector<Flight> flights = { f };
        fitFlightEdges(flights, polys, 0.3f, LLVector2(0.f, 0.f), LLVector2(32.f, 32.f));
        const Flight& g = flights[0];
        ensure("bottom across the treads", std::fabs(g.mBottomA.mV[VY] - 8.7f) < 1e-3f && std::fabs(g.mBottomB.mV[VY] - 11.3f) < 1e-3f);
        ensure("top across the treads", std::fabs(g.mTopA.mV[VY] - 8.7f) < 1e-3f && std::fabs(g.mTopB.mV[VY] - 11.3f) < 1e-3f);
        ensure("ends stay put along the run", g.mBottomA.mV[VX] == 10.f && g.mTopB.mV[VX] == 14.f && g.mTopA.mV[VZ] == 2.f);
        // The tile ends at y = 11: the treads may go on past it.
        flights = { f };
        fitFlightEdges(flights, polys, 0.3f, LLVector2(0.f, 0.f), LLVector2(32.f, 11.f));
        ensure("cut side kept, other side fitted", std::fabs(flights[0].mBottomB.mV[VY] - 13.f) < 1e-3f && std::fabs(flights[0].mBottomA.mV[VY] - 8.7f) < 1e-3f);
    }
    template<> template<>
    void sknavflights_object::test<29>()
    {
        set_test_name("two stair wedges with a low hull skirt between them are two narrow flights, not one across the skirt");
        // The galleon: 49 degree wedges up to a quarterdeck, and a 12 degree skirt 6 m wide rising 0.43 m between them.
        Soup s;
        s.box(-6, -4, -0.5f, 0, 4, 0);
        s.box(1.45f, -4, 0, 6, 4, 1.68f);
        for (const F32 y : { -3.f, 2.f })
        {
            s.quad(LLVector3(0, y, 0), LLVector3(1.45f, y, 1.68f), LLVector3(1.45f, y + 0.88f, 1.68f), LLVector3(0, y + 0.88f, 0));
        }
        s.quad(LLVector3(-0.5f, -3.f, 0.01f), LLVector3(1.44f, -3.f, 0.44f), LLVector3(1.44f, 3.f, 0.44f), LLVector3(-0.5f, 3.f, 0.01f));
        const SlopePartition p = s.partition();
        ensure_equals("flights", p.mFlights.size(), 2u);
        for (const Flight& f : p.mFlights)
        {
            ensure(llformat("as wide as a wedge: %.2f", (f.mBottomB - f.mBottomA).length()), std::fabs((f.mBottomB - f.mBottomA).length() - 0.88f) < 0.1f);
            ensure(llformat("up the wedge: rise %.2f run %.2f", f.mRise, f.mRun), std::fabs(f.mRise - 1.68f) < 0.1f && std::fabs(f.mRun - 1.45f) < 0.2f);
        }
    }

    template<> template<>
    void sknavflights_object::test<30>()
    {
        set_test_name("a gentle ramp climbing a metre stays a ramp, beside a steep stair it does not join");
        Soup s = ramp(7.f, 1.f);
        s.quad(LLVector3(0, 0.75f, 0), LLVector3(1.2f, 0.75f, 1.f), LLVector3(1.2f, 1.95f, 1.f), LLVector3(0, 1.95f, 0));
        s.box(1.2f, 0.75f, 0, 7.f, 2.f, 1.f);
        const SlopePartition p = s.partition();
        ensure_equals("flights", p.mFlights.size(), 2u);
        bool ramp_found = false;
        for (const Flight& f : p.mFlights)
        {
            ramp_found = ramp_found || (f.mKind == ConnectorKind::RAMP && std::fabs(f.mRun - 7.f) < 0.3f && std::fabs((f.mBottomB - f.mBottomA).length() - 1.5f) < 0.15f);
        }
        ensure("the ramp, at its own width and run", ramp_found);
    }

    template<> template<>
    void sknavflights_object::test<31>()
    {
        set_test_name("a 60 degree wedge is a flight and a 78 degree face is a wall, at AVATAR's 74 degree slope");
        const SlopePartition sixty = ramp(2.4f / std::tan(60.f * DEG_TO_RAD), 2.4f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("the 60 degree wedge", sixty.mFlights.size(), 1u);
        const F32 grade = std::atan2(sixty.mFlights[0].mRise, sixty.mFlights[0].mRun) * RAD_TO_DEG;
        ensure(llformat("at its grade: %.1f", grade), std::fabs(grade - 60.f) < 3.f);
        const SlopePartition steep = ramp(3.f / std::tan(78.f * DEG_TO_RAD), 3.f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("the 78 degree face", steep.mFlights.size(), 0u);
    }

    template<> template<>
    void sknavflights_object::test<32>()
    {
        set_test_name("a stair landing on a deck of small hull pieces at slightly different heights lands on the whole deck");
        Soup s;
        s.box(-4, -2, -0.5f, 0, 2, 0);
        s.quad(LLVector3(0, -0.75f, 0), LLVector3(2.5f, -0.75f, 1.5f), LLVector3(2.5f, 0.75f, 1.5f), LLVector3(0, 0.75f, 0));
        // nine 1.4 m pieces, each under the 4 m2 a floor needs, alternating 5 cm apart
        for (S32 i = 0; i < 3; ++i)
        {
            for (S32 j = 0; j < 3; ++j)
            {
                const F32 x = 2.5f + i * 1.4f, y = -2.1f + j * 1.4f;
                s.box(x, y, 0.f, x + 1.4f, y + 1.4f, 1.5f + ((i + j) % 2) * 0.05f);
            }
        }
        const SlopePartition p = s.partition();
        ensure_equals("flights", p.mFlights.size(), 1u);
        ensure(llformat("up onto the deck: rise %.2f", p.mFlights[0].mRise), std::fabs(p.mFlights[0].mRise - 1.5f) < 0.1f);
    }

    template<> template<>
    void sknavflights_object::test<33>()
    {
        set_test_name("a narrow stair erosion leaves no polygon on keeps its record, marked eroded, linking its floors");
        TileBuildInput in = stairTile();
        in.mParams[0].mSlope = WALK_SLOPE_DEG;
        // six steps 0.88 m wide between two walls: the 0.3 m radius erodes them all away
        for (S32 k = 1; k <= 6; ++k)
        {
            in.mRecords.push_back(boxRecord(10.f + (k - 1) * 0.3f, 15.56f, 0.f, 10.f + k * 0.3f, 16.44f, k * 0.25f));
        }
        in.mRecords.push_back(boxRecord(10.f, 14.f, 0.f, 11.8f, 15.56f, 3.5f));
        in.mRecords.push_back(boxRecord(10.f, 16.44f, 0.f, 11.8f, 18.f, 3.5f));
        in.mRecords.push_back(boxRecord(11.8f, 12.f, 0.f, 16.f, 20.f, 1.5f));
        const TileBuildResult r = buildTile(in);
        ensure("built: " + r.mError, r.mStatus == TileBuildResult::SUCCESS);
        const S32 w = static_cast<S32>(AgentClass::AVATAR);
        ensure_equals("one flight", r.mFlights[w].size(), 1u);
        const Flight& f = r.mFlights[w][0];
        ensure("no polys", f.mPolys.empty());
        ensure("eroded", f.mEroded);
        ensure(llformat("from the ground to the landing: %.2f to %.2f", f.mBottom.mV[VZ], f.mTop.mV[VZ]),
               std::fabs(f.mBottom.mV[VZ]) < 0.2f && std::fabs(f.mTop.mV[VZ] - 1.5f) < 0.2f);
    }

    template<> template<>
    void sknavflights_object::test<34>()
    {
        set_test_name("stepped stairs are judged by riser against the climb, whatever their grade");
        const SlopePartition p = straight(6, 0.5f, 0.12f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("flights", p.mFlights.size(), 1u);
        const F32 grade = std::atan2(p.mFlights[0].mRise, p.mFlights[0].mRun) * RAD_TO_DEG;
        ensure(llformat("steeper than the slope: %.1f", grade), grade > WALK_SLOPE_DEG);
        ensure("a stair by its treads", p.mFlights[0].mRule == ConnectorRule::TREADS);
        ensure_equals("risers past the climb are platforms", straight(6, 0.8f, 0.3f).partition(0.75f, TOP_SLOPE_DEG).mFlights.size(), 0u);
    }

    /// A slope up +x from a floor to a deck, half_width either side, with a lateral bow of z += bow * y * y.
    Soup wedge(F32 deg, F32 rise, F32 half_width, F32 bow = 0.f)
    {
        Soup s;
        const F32 run = rise / std::tan(deg * DEG_TO_RAD);
        s.box(-4, -4, -0.5f, 0, 4, 0);
        s.grid(0.f, -half_width, run, half_width, std::min(run, half_width) / 8.f, [&](F32 x, F32 y) { return x / run * rise + bow * y * y; });
        s.box(run, -4, 0, run + 4, 4, rise);
        return s;
    }

    template<> template<>
    void sknavflights_object::test<35>()
    {
        set_test_name("a curved column face at 60 degrees is no flight; flat, it is a climb");
        ensure_equals("curved", wedge(60.f, 3.f, 1.f, 0.23f).partition(0.75f, TOP_SLOPE_DEG).mFlights.size(), 0u);
        const SlopePartition flat = wedge(60.f, 3.f, 1.f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("flat", flat.mFlights.size(), 1u);
    }

    template<> template<>
    void sknavflights_object::test<36>()
    {
        set_test_name("a flat 60 degree cliff between decks is a climb, not a stair");
        const SlopePartition p = wedge(60.f, 3.f, 2.f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("flights", p.mFlights.size(), 1u);
        ensure("a climb", p.mFlights[0].mKind == ConnectorKind::CLIMB);
    }

    template<> template<>
    void sknavflights_object::test<37>()
    {
        set_test_name("a 0.88 m wedge just past 52 degrees between decks is a stair: a ship's stair");
        const SlopePartition p = wedge(52.5f, 1.8f, 0.44f).partition(0.75f, TOP_SLOPE_DEG);
        ensure_equals("flights", p.mFlights.size(), 1u);
        ensure("a stair", p.mFlights[0].mKind == ConnectorKind::STAIR);
    }
}
