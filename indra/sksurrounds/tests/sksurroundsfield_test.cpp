/**
 * @file sksurroundsfield_test.cpp
 * @brief The distant regions' selection and edge levels against the surrounds oracle.
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

#include "sksurroundsfield.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstring>

namespace
{
    using namespace SKSurrounds;

    // Optimised builds may contract and reassociate (/fp:fast, -ffp-contract=fast), and arm64 fuses where x64
    // does not, so a level may land one ULP from the oracle's; the levels are positive, so their bits order as they do.
    bool withinUlp(F32 got, U32 want)
    {
        const S64 d = static_cast<S64>(std::bit_cast<U32>(got)) - static_cast<S64>(want);
        return d >= -1 && d <= 1;
    }

    struct FixtureCamera
    {
        F64 mX, mY, mZ;
        S32 mAgentX, mAgentY;
        S32 mFarCount;
        U64 mLeafHash, mNeighbourHash;
    };

    struct FixtureLeaf
    {
        S32 mCamera, mLevel, mX, mY, mW, mE, mS, mN;
    };

    struct FixtureTile
    {
        S32 mKind, mLevel, mX, mY; // kind 0 live, 1 ring, 2 far
    };

    struct FixtureEdge
    {
        S32         mCamera;
        FixtureTile mFine, mCoarse;
        S32         mAX, mAY, mBX, mBY;
        U32         mZA, mZB, mFineLevel, mCoarseLevel;
        S32         mFineHalf, mCoarseSplit; // 0 on same-size edges
    };

#include "sksurroundsfixture.inl"

    constexpr F32 DENSITY = 48.f;

    U64 fnv(U64 h, S32 v)
    {
        const U32 u = U32(v);
        for (S32 i = 0; i < 4; ++i)
        {
            h ^= (u >> (8 * i)) & 0xff;
            h *= 0x100000001b3ull;
        }
        return h;
    }

    void selectFor(Selection& sel, const FixtureCamera& c)
    {
        Cell live[9];
        S32  n = 0;
        for (S32 i = -1; i <= 1; ++i)
            for (S32 j = -1; j <= 1; ++j)
                live[n++] = { c.mAgentX + i, c.mAgentY + j };
        const F64 cam[3] = { c.mX, c.mY, c.mZ };
        sel.select(cam, live);
    }

    F32 sideLevel(const FixtureCamera& c, const FixtureTile& t, S32 ax, S32 ay, U32 za, S32 bx, S32 by, U32 zb)
    {
        const F64 cam[3]   = { c.mX, c.mY, c.mZ };
        const F64 frame[2] = { F64(t.mX) * REGION_METRES, F64(t.mY) * REGION_METRES };
        F32       origin[3];
        if (t.mKind == 0)
        {
            const F64 agent[2] = { F64(c.mAgentX) * REGION_METRES, F64(c.mAgentY) * REGION_METRES };
            liveOrigin(cam, agent, frame, origin);
        }
        else
        {
            farOrigin(cam, frame, origin);
        }
        const S32 fx = t.mX * REGION_METRES, fy = t.mY * REGION_METRES;
        const F32 a[3] = { F32(ax - fx), F32(ay - fy), std::bit_cast<F32>(za) };
        const F32 b[3] = { F32(bx - fx), F32(by - fy), std::bit_cast<F32>(zb) };
        return edgeLevel(a, b, origin, DENSITY);
    }

    TEST_CASE("sksurroundsfield: far nodes and their neighbours match the oracle's", "[sksurroundsfield]")
    {
        Selection sel;
        for (size_t ci = 0; ci < std::size(FIXTURE_CAMERAS); ++ci)
        {
            CAPTURE(ci);
            const FixtureCamera& c = FIXTURE_CAMERAS[ci];
            selectFor(sel, c);
            U64 hl = 0xcbf29ce484222325ull, hn = hl;
            S32 farNodes = 0, live = 0, ring = 0;
            for (const Node& n : sel.nodes())
            {
                if (n.mBand != Band::Far)
                {
                    (n.mBand == Band::Live ? live : ring)++;
                    continue;
                }
                ++farNodes;
                for (S32 v : { S32(n.mLevel), n.mX, n.mY })
                {
                    hl = fnv(hl, v);
                    hn = fnv(hn, v);
                }
                for (S8 v : n.mNeighbour)
                    hn = fnv(hn, v);
                for (S8 v : n.mNeighbour)
                    CHECK((v == NO_NEIGHBOUR || std::abs(v - n.mLevel) <= 1));
            }
            CHECK(live == 9);
            CHECK(ring == 16);
            CHECK(farNodes == c.mFarCount);
            CHECK(hl == c.mLeafHash);
            CHECK(hn == c.mNeighbourHash);
        }
    }

    TEST_CASE("sksurroundsfield: three cameras' far nodes, node by node", "[sksurroundsfield]")
    {
        Selection sel;
        S32       cam = -1;
        size_t    k   = 0;
        for (const FixtureLeaf& l : FIXTURE_LEAVES)
        {
            if (l.mCamera != cam)
            {
                cam = l.mCamera;
                selectFor(sel, FIXTURE_CAMERAS[cam]);
                k = 25;
            }
            REQUIRE(k < sel.nodes().size());
            const Node& n = sel.nodes()[k++];
            CAPTURE(cam, l.mLevel, l.mX, l.mY);
            CHECK(n.mLevel == l.mLevel);
            CHECK(n.mX == l.mX);
            CHECK(n.mY == l.mY);
            CHECK(n.mNeighbour[SIDE_WEST] == l.mW);
            CHECK(n.mNeighbour[SIDE_EAST] == l.mE);
            CHECK(n.mNeighbour[SIDE_SOUTH] == l.mS);
            CHECK(n.mNeighbour[SIDE_NORTH] == l.mN);
        }
    }

    TEST_CASE("sksurroundsfield: shared edges' levels are the oracle's within an ULP", "[sksurroundsfield]")
    {
        for (const FixtureEdge& e : FIXTURE_EDGES)
        {
            const FixtureCamera& c = FIXTURE_CAMERAS[e.mCamera];
            CAPTURE(e.mCamera, e.mAX, e.mAY, e.mBX, e.mBY, e.mFine.mKind, e.mCoarse.mKind);
            const F32 lf = sideLevel(c, e.mFine, e.mAX, e.mAY, e.mZA, e.mBX, e.mBY, e.mZB);
            const F32 lc = sideLevel(c, e.mCoarse, e.mAX, e.mAY, e.mZA, e.mBX, e.mBY, e.mZB);
            CHECK(withinUlp(lf, e.mFineLevel));
            CHECK(withinUlp(lc, e.mCoarseLevel));
            if (e.mFineHalf)
            {
                const S32 m = fineHalfLevel(lc, e.mFine.mKind != 2);
                CHECK(m == e.mFineHalf);
                CHECK(coarseEdgeLevel(m) == e.mCoarseSplit);
            }
        }
    }

    TEST_CASE("sksurroundsfield: edge level and parameter pins", "[sksurroundsfield]")
    {
        const F32 a[3] = { 0, 0, 20 }, b[3] = { 16, 0, 20 };
        const F32 away[3] = { 8, 100, 20 }, onEdge[3] = { 8, 0, 20 };
        const F32 want   = (16.f * 48.f) / 100.f;
        CHECK(withinUlp(edgeLevel(a, b, away, 48.f), std::bit_cast<U32>(want)));
        CHECK(edgeLevel(a, b, onEdge, 48.f) == 64.f);
        CHECK(edgeLevel(a, b, onEdge, 0.f) == 16.f);

        CHECK(fineHalfLevel(8.f, true) == 5);   // parent 2 mod 4 halves to an even 4: odd rounds up
        CHECK(fineHalfLevel(8.f, false) == 4);
        CHECK(fineHalfLevel(1.f, false) == 1);
        CHECK(fineHalfLevel(64.f, true) == 31);
        CHECK(fineHalfLevel(64.f, false) == 32);

        // The fine side's endpoints are the parent's ends and its midpoint, exactly.
        CHECK(parentParameter(0, 5, true) == 0.f);
        CHECK(parentParameter(5, 5, true) == 0.5f);
        CHECK(parentParameter(5, 5, false) == 1.f);
        for (S32 k = 0; k <= 7; ++k)
            CHECK(parentParameter(k, 7, false) == 1.f - parentParameter(7 - k, 7, true));
    }

    TEST_CASE("sksurroundsfield: a node reads the map tile of its own size, south up", "[sksurroundsfield]")
    {
        Node n;
        n.mLevel = 3;
        n.mX     = 1000;
        n.mY     = 1024;
        MapTile t = mapTile(n);
        CHECK(t.mLevel == 4);
        CHECK(t.mX == 1000);
        CHECK(t.mY == 1024);
        CHECK(t.mMetres == 2048.f);
        F32 uv[2];
        mapUV(t, 512.f, 1536.f, uv);
        CHECK(uv[0] == 0.25f);
        CHECK(uv[1] == 0.75f);

        n.mLevel = LEVEL_SPECIAL;
        n.mBand  = Band::Ring;
        n.mX     = 997;
        t        = mapTile(n);
        CHECK(t.mLevel == 1);
        CHECK(t.mX == 997);
        CHECK(t.mMetres == 256.f);
    }
}
