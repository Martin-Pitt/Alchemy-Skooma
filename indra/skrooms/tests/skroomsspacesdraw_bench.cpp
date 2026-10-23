/**
 * @file skroomsspacesdraw_bench.cpp
 * @brief Milliseconds to draw a region's spaces: tracing them once, and
 *        culling the traced draw to the camera, which is all a gather does.
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

// A synthetic region: eight by eight tiles at the viewer's air, carved into
// rooms of 20 by 14 m behind 2 m corridors, every fourth room a sealed shell
// the pass would leave unknown, a slope in the rooms of every second column,
// and a mezzanine storey over part of some rooms. The trace is what the
// spaces pass now spends once per re-cut on its worker; the cull is what
// every overlay gather spends on it, per camera step.

#include "linden_common.h"

#include "altestbenchmark.h"

#include "../skroomsenclosure.h"
#include "../skroomsspaces.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
    using namespace skworld;

    constexpr S32 TILES = 8;            // tiles a side
    constexpr S32 COARSE = 36;          // coarse cells a tile a side: the 32 m span plus 2 m of pad
    constexpr S32 ROOM_W = 20, ROOM_H = 14, HALL = 2;

    // The space a cell of the region belongs to and its floor, or space 0
    // for no answer.
    U32 cellAt(S32 gx, S32 gy, U16& floor_units)
    {
        floor_units = 0;
        if (gx < 0 || gy < 0 || gx >= TILES * 32 || gy >= TILES * 32)
        {
            return 0;
        }
        const S32 rx = gx % ROOM_W, ry = gy % ROOM_H;
        if (rx < HALL || ry < HALL)
        {
            return 0;   // the corridors between the rooms hold no floor
        }
        if (((gx / ROOM_W) ^ (gy / ROOM_H)) % 4 == 3)
        {
            return 0;   // sealed shells
        }
        const U32 room = static_cast<U32>((gy / ROOM_H) * (TILES * 32 / ROOM_W) + (gx / ROOM_W));
        const bool slope = (gx / ROOM_W) % 2 == 1;
        const bool storey = rx > ROOM_W / 2 && ry > ROOM_H / 2 && (gy / ROOM_H) % 3 == 1;
        const F32 z = (slope ? rx * 0.04f : 0.f) + (storey ? 2.f : 0.f);
        floor_units = static_cast<U16>(std::lround(z / 0.125));
        return storey ? room * 2 + 1 : room * 2;
    }

    struct Region
    {
        EnclosureProduct mEnc;
        RoomsProduct mRooms;

        Region()
        {
            const S32 rooms_x = TILES * 32 / ROOM_W, rooms_y = TILES * 32 / ROOM_H;
            mEnc.mSpaceIds.reserve(static_cast<size_t>(rooms_x) * rooms_y * 2);
            for (U32 i = 0; i < static_cast<U32>(rooms_x * rooms_y * 2); ++i)
            {
                mEnc.mSpaceIds.push_back(0x1000 + i);
            }
            mEnc.mTiles.reserve(static_cast<size_t>(TILES) * TILES);
            std::vector<U32> starts;
            std::vector<CoarseGap> gaps;
            for (S32 ty = 0; ty < TILES; ++ty)
            {
                for (S32 tx = 0; tx < TILES; ++tx)
                {
                    TileEnclosure t;
                    t.mKey = { tx, ty };
                    t.mOrigin.set(tx * 32.0f - 2.0f, ty * 32.0f - 2.0f, 0.f);
                    t.mCell = 0.125f;
                    t.mUnit = 0.125f;
                    t.mWidth = t.mHeight = COARSE * 8;
                    CoarseAir& c = t.mCoarse;
                    c.mCells = 8;
                    c.mWidth = c.mHeight = COARSE;
                    starts.clear();
                    gaps.clear();
                    starts.push_back(0);
                    for (S32 cy = 0; cy < COARSE; ++cy)
                    {
                        for (S32 cx = 0; cx < COARSE; ++cx)
                        {
                            U16 units;
                            const U32 space = cellAt(tx * 32 - 2 + cx, ty * 32 - 2 + cy, units);
                            if (space)
                            {
                                CoarseGap g;
                                g.mFloor = units;
                                g.mSpace = static_cast<U16>(std::min<U32>(space, 0xffff));
                                gaps.push_back(g);
                            }
                            starts.push_back(static_cast<U32>(gaps.size()));
                        }
                    }
                    c.mStart = std::move(starts);
                    c.mGaps = std::move(gaps);
                    mEnc.mTiles.push_back(std::move(t));
                }
            }
        }
    };

}

namespace
{
    TEST_CASE("skroomsspacesdraw_bench: the spaces overlay's draw traced once and culled per camera", "[skroomsspacesdraw_bench][!benchmark]")
    {
    ll_test::skipUnlessOptimised();
    Region region;
    std::printf("region %d x %d tiles, %zu gaps over %zu cells, %zu spaces\n",
                TILES, TILES,
                std::accumulate(region.mEnc.mTiles.begin(), region.mEnc.mTiles.end(), size_t(0),
                                [](size_t n, const TileEnclosure& t) { return n + t.mCoarse.mGaps.size(); }),
                size_t(TILES) * TILES * COARSE * COARSE,
                region.mEnc.mSpaceIds.size());

    SpacesDrawPtr draw;
    draw = buildSpacesDraw(region.mEnc, region.mRooms);
    BENCHMARK("trace once")
    {
        return buildSpacesDraw(region.mEnc, region.mRooms);
    };
    std::printf("trace: %zu fills, %zu outline runs, %.1f MB\n",
                draw->mFills.size(),
                std::accumulate(draw->mSpaces.begin(), draw->mSpaces.end(), size_t(0),
                                [](size_t n, const SpaceDraw& s) { return n + s.mOutline.size(); }),
                draw->bytes() / (1024.0 * 1024.0));

    // The gather's part: cull the draw to a 96 m reach and count what emits,
    // from four cameras the region's corners put the reach over.
    const F64 reach = 96.0, reach2 = reach * reach;
    U32 seen = 0;
    BENCHMARK("cull to four reaches")
    {
        seen = 0;
        for (const LLVector3d camera : { LLVector3d(48, 48, 0), LLVector3d(208, 48, 0),
                                         LLVector3d(128, 128, 0), LLVector3d(208, 208, 0) })
        {
            for (const SpacesDraw::Fill& f : draw->mFills)
            {
                const F64 dx = f.mX + f.mSide * 0.5 - camera.mdV[VX], dy = f.mY + f.mSide * 0.5 - camera.mdV[VY];
                if (dx * dx + dy * dy <= reach2)
                {
                    ++seen;
                }
            }
            for (const SpaceDraw& s : draw->mSpaces)
            {
                for (const PlanEdge& e : s.mOutline)
                {
                    const F64 dx = (e.mX0 + e.mX1) * 0.5 - camera.mdV[VX], dy = (e.mY0 + e.mY1) * 0.5 - camera.mdV[VY];
                    if (dx * dx + dy * dy <= reach2)
                    {
                        ++seen;
                    }
                }
            }
        }
        return seen;
    };
    std::printf("cull emits %u quads and runs over four reaches\n", seen);
    }
}
