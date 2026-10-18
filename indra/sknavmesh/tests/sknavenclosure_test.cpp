/**
 * @file sknavenclosure_test.cpp
 * @brief Tests for the enclosure pass: S, D, e and buckets over synthetic air.
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

#include "../sknavbuild.h"
#include "../sknavenclosure.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace sknav;

    const F32 CELL = 0.5f;          // coarser than the viewer, to keep the tests quick
    const F32 UNIT = 0.125f;
    const S32 SIDE = 64;            // cells per 32 m tile
    const F32 TOP = 40.f;

    struct Solid
    {
        F32 mX0, mY0, mX1, mY1, mZ0, mZ1;
        bool mWalk;
    };

    /// Axis-aligned solids over ground, sampled at cell centres into tiles.
    struct Scene
    {
        std::vector<Solid> mSolids;
        bool mGround = true;
        F32 mGroundX0 = -1e9f, mGroundX1 = 1e9f;
        F32 mCell = CELL;           ///< the air's cell, COARSE_AIR_M for a tile built past the air radius

        void box(F32 x0, F32 y0, F32 x1, F32 y1, F32 z0, F32 z1, bool walk = true) { mSolids.push_back({ x0, y0, x1, y1, z0, z1, walk }); }

        /// A room of walls 0.5 m thick, 3 m high, roofed at 3 to 3.25 m.
        void room(F32 x0, F32 y0, F32 x1, F32 y1)
        {
            const F32 t = 0.5f;
            box(x0, y0, x1, y0 + t, 0.f, 3.f, false);
            box(x0, y1 - t, x1, y1, 0.f, 3.f, false);
            box(x0, y0, x0 + t, y1, 0.f, 3.f, false);
            box(x1 - t, y0, x1, y1, 0.f, 3.f, false);
            box(x0, y0, x1, y1, 3.f, 3.25f, true);
        }

        TileAirPtr tile(S32 kx, S32 ky) const
        {
            AirGrid g;
            g.mOrigin.set(kx * 32.f, ky * 32.f, 0.f);
            g.mCell = mCell;
            const S32 side = static_cast<S32>(std::lround(32.f / mCell));
            g.mUnit = UNIT;
            g.mWidth = g.mHeight = side;
            g.mStart.assign(side * side + 1, 0);
            struct Run
            {
                S32 mZ0, mZ1;
                bool mWalk;
            };
            std::vector<Run> runs;
            for (S32 iy = 0; iy < side; ++iy)
            {
                for (S32 ix = 0; ix < side; ++ix)
                {
                    const F32 x = g.mOrigin.mV[VX] + (ix + 0.5f) * mCell, y = g.mOrigin.mV[VY] + (iy + 0.5f) * mCell;
                    runs.clear();
                    if (mGround && x >= mGroundX0 && x < mGroundX1)
                    {
                        runs.push_back({ -8, 0, true });
                    }
                    for (const Solid& s : mSolids)
                    {
                        if (x >= s.mX0 && x < s.mX1 && y >= s.mY0 && y < s.mY1)
                        {
                            runs.push_back({ static_cast<S32>(std::lround(s.mZ0 / UNIT)), static_cast<S32>(std::lround(s.mZ1 / UNIT)), s.mWalk });
                        }
                    }
                    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.mZ0 < b.mZ0; });
                    std::vector<Run> merged;
                    for (const Run& r : runs)
                    {
                        if (!merged.empty() && r.mZ0 <= merged.back().mZ1)
                        {
                            if (r.mZ1 >= merged.back().mZ1)
                            {
                                merged.back().mWalk = r.mWalk;
                            }
                            merged.back().mZ1 = std::max(merged.back().mZ1, r.mZ1);
                        }
                        else
                        {
                            merged.push_back(r);
                        }
                    }
                    for (size_t i = 0; i < merged.size(); ++i)
                    {
                        AirGap gap;
                        gap.mFloor = static_cast<U16>(merged[i].mZ1 + 8);   // heights from -1 m
                        gap.mFlags = merged[i].mWalk ? AirGap::TOP : 0;
                        if (i + 1 < merged.size())
                        {
                            gap.mCeiling = static_cast<U16>(merged[i + 1].mZ0 + 8);
                        }
                        else
                        {
                            gap.mCeiling = static_cast<U16>(TOP / UNIT);
                            gap.mFlags |= AirGap::OPEN | AirGap::OWN_SKY;
                        }
                        g.mGaps.push_back(gap);
                    }
                    g.mStart[ix + iy * side + 1] = static_cast<U32>(g.mGaps.size());
                }
            }
            g.mOrigin.mV[VZ] = -1.f;
            computeClearance(g, 6, 4);
            auto air = std::make_shared<TileAir>();
            distilAir(g, 6, *air);
            airSeams(g, *air);
            return air;
        }

        std::shared_ptr<EnclosureInput> input(S32 tiles_x, S32 tiles_y = 1) const
        {
            auto in = std::make_shared<EnclosureInput>();
            for (S32 ky = 0; ky < tiles_y; ++ky)
            {
                for (S32 kx = 0; kx < tiles_x; ++kx)
                {
                    EnclosureInput::Tile t;
                    t.mKey.mX = kx;
                    t.mKey.mY = ky;
                    t.mAir = tile(kx, ky);
                    if (t.mAir && !t.mAir->mLeaves.empty())
                    {
                        in->mTiles.push_back(t);
                    }
                }
            }
            return in;
        }
    };

    Enclosure at(const EnclosureProductPtr& p, F32 x, F32 y, F32 z = 0.5f)
    {
        Enclosure e;
        p->enclosureAt(LLVector3(x, y, z), e);
        return e;
    }
}


TEST_CASE("an open field is open sky, depth 0, OUTDOORS", "[sknavenclosure]")
{
    Scene s;
    const EnclosureProductPtr p = computeEnclosure(s.input(2));
    const Enclosure e = at(p, 40.f, 10.f);
    CHECK((e.mS) == (1.f));
    CHECK((e.mD) == (0.f));
    CHECK(e.mBucket == EnclosureBucket::OUTDOORS);
    CHECK((p->mUnknownGaps) == (0u));
}

TEST_CASE("a small canopy on posts is SHELTERED under its middle", "[sknavenclosure]")
{
    Scene s;
    s.box(14.f, 14.f, 18.f, 18.f, 3.f, 3.25f);
    for (F32 px : { 14.f, 17.5f })
    {
        for (F32 py : { 14.f, 17.5f })
        {
            s.box(px, py, px + 0.5f, py + 0.5f, 0.f, 3.f, false);
        }
    }
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure e = at(p, 16.1f, 16.1f);
    CHECK((e.mS < 0.6f && e.mS > 0.2f));
    CHECK(e.mBucket == EnclosureBucket::SHELTERED);
    CHECK(at(p, 10.f, 10.f).mBucket == EnclosureBucket::OUTDOORS);
}

TEST_CASE("a closed room with a door is INDOORS deep inside, and its window gives light but not feet", "[sknavenclosure]")
{
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    // A door on the south wall: a gap in the wall under a lintel.
    s.mSolids[0].mX1 = 5.f;
    s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
    s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure deep = at(p, 14.f, 14.f);
    CHECK(deep.mBucket == EnclosureBucket::INDOORS);
    CHECK(deep.mS < 0.02f);
    CHECK(deep.mD > 8.f);
    CHECK(at(p, 5.5f, 2.f).mBucket == EnclosureBucket::OUTDOORS);

    // A window on the north wall from 1 to 2 m, right by the deep point.
    Scene w = s;
    w.mSolids[1].mX1 = 13.f;
    w.box(15.f, 15.5f, 16.f, 16.f, 0.f, 3.f, false);
    w.box(13.f, 15.5f, 15.f, 16.f, 0.f, 1.f, true);     // a sill wide enough to stand on
    w.box(13.f, 15.5f, 15.f, 16.f, 2.f, 3.f, false);
    const EnclosureProductPtr pw = computeEnclosure(w.input(1));
    const Enclosure lit = at(pw, 14.f, 14.f);
    CHECK(lit.mS > deep.mS + 0.1f);
    CHECK((lit.mD) == (deep.mD));
    CHECK(at(pw, 10.f, 10.f).mBucket == EnclosureBucket::INDOORS);
}

TEST_CASE("a room with only a window is INDOORS and sealed, or UNKNOWN with sealing off, never OUTDOORS", "[sknavenclosure]")
{
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    s.mSolids[1].mX1 = 9.f;
    s.box(11.f, 15.5f, 16.f, 16.f, 0.f, 3.f, false);
    s.box(9.f, 15.5f, 11.f, 16.f, 0.f, 1.f, false);
    s.box(9.f, 15.5f, 11.f, 16.f, 2.f, 3.f, false);
    s.box(6.f, 6.f, 8.f, 7.f, 0.f, 1.f, false);             // a desk, whose air is carried from the sealed floor
    auto in = s.input(1);
    const EnclosureProductPtr p = computeEnclosure(in);
    const Enclosure e = at(p, 10.f, 10.f);
    CHECK(e.mBucket == EnclosureBucket::INDOORS);
    CHECK(e.mSealed);
    CHECK((e.mD) == (ENCLOSURE_D_DEEP * ENCLOSURE_D_UNIT_M));
    CHECK(e.mS > 0.f);
    CHECK(at(p, 7.f, 6.5f, 1.5f).mSealed);
    CHECK((p->mSealedGaps > 0 && p->mUnknownGaps == 0));
    CHECK(!at(p, 25.f, 25.f).mSealed);

    in->mParams.mSealedIndoors = false;
    const EnclosureProductPtr off = computeEnclosure(in);
    CHECK(at(off, 10.f, 10.f).mBucket == EnclosureBucket::UNKNOWN);
    CHECK(off->mUnknownGaps > 0);
}

TEST_CASE("closed air over no walkable floor stays UNKNOWN", "[sknavenclosure]")
{
    Scene s;
    s.box(4.f, 4.f, 12.f, 12.f, 0.f, 2.f, false);           // a hollow block: unwalkable floor, closed lid
    s.box(4.f, 4.f, 12.f, 12.f, 4.f, 4.5f, false);
    s.box(4.f, 4.f, 12.f, 4.5f, 2.f, 4.f, false);
    s.box(4.f, 11.5f, 12.f, 12.f, 2.f, 4.f, false);
    s.box(4.f, 4.f, 4.5f, 12.f, 2.f, 4.f, false);
    s.box(11.5f, 4.f, 12.f, 12.f, 2.f, 4.f, false);
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure e = at(p, 8.f, 8.f, 3.f);
    CHECK(e.mBucket == EnclosureBucket::UNKNOWN);
    CHECK(!e.mSealed);
}

TEST_CASE("a street between towers, even under a skybridge, is OUTDOORS", "[sknavenclosure]")
{
    Scene s;
    s.box(0.f, 0.f, 12.f, 32.f, 0.f, 30.f, false);
    s.box(16.f, 0.f, 32.f, 32.f, 0.f, 30.f, false);
    s.box(12.f, 14.5f, 16.f, 16.5f, 15.f, 15.5f, false);
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    CHECK(at(p, 14.f, 5.f).mBucket == EnclosureBucket::OUTDOORS);
    const Enclosure under = at(p, 14.f, 15.5f);
    CHECK(under.mS < 1.f);
    CHECK(under.mBucket == EnclosureBucket::OUTDOORS);
}

TEST_CASE("the air over a desk carries the room's depth", "[sknavenclosure]")
{
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    s.mSolids[0].mX1 = 5.f;
    s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
    s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
    s.box(12.f, 12.f, 14.f, 13.f, 0.f, 1.f, false);     // a desk too high to step onto
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure desk = at(p, 13.f, 12.5f, 1.5f);
    const Enclosure floor = at(p, 13.f, 11.5f);
    CHECK(desk.mBucket != EnclosureBucket::UNKNOWN);
    CHECK(desk.mBucket == EnclosureBucket::INDOORS);
    CHECK((desk.mD >= floor.mD && desk.mD - floor.mD < 2.5f));
}

TEST_CASE("depth floods across a tile seam", "[sknavenclosure]")
{
    Scene s;
    s.room(24.f, 4.f, 48.f, 16.f);
    s.mSolids[2].mY1 = 9.f;         // a door in the west wall
    s.box(24.f, 10.f, 24.5f, 16.f, 0.f, 3.f, false);
    s.box(24.f, 9.f, 24.5f, 10.f, 2.25f, 3.f, false);
    const EnclosureProductPtr p = computeEnclosure(s.input(2));
    const Enclosure e = at(p, 44.f, 9.5f);
    CHECK(e.mBucket != EnclosureBucket::UNKNOWN);
    CHECK(e.mBucket == EnclosureBucket::INDOORS);
    CHECK((e.mD > 18.f && e.mD < 23.f));
}

TEST_CASE("buckets hold within the hysteresis of their edge", "[sknavenclosure]")
{
    CHECK(enclosureBucket(0.72f, EnclosureBucket::UNKNOWN, 0.05f) == EnclosureBucket::SHELTERED);
    CHECK(enclosureBucket(0.72f, EnclosureBucket::INDOORS, 0.05f) == EnclosureBucket::INDOORS);
    CHECK(enclosureBucket(0.69f, EnclosureBucket::INDOORS, 0.05f) == EnclosureBucket::SHELTERED);
    CHECK(enclosureBucket(0.28f, EnclosureBucket::OUTDOORS, 0.05f) == EnclosureBucket::OUTDOORS);
    CHECK(enclosureBucket(0.21f, EnclosureBucket::SHELTERED, 0.05f) == EnclosureBucket::SHELTERED);
    CHECK(enclosureBucket(0.19f, EnclosureBucket::SHELTERED, 0.05f) == EnclosureBucket::OUTDOORS);
    CHECK(enclosureBucket(0.9f, EnclosureBucket::OUTDOORS, 0.05f) == EnclosureBucket::INDOORS);

    // Against a previous generation that read everything INDOORS, a gap
    // whose e sits just under the edge keeps it; the rest do not.
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    s.mSolids[0].mX1 = 5.f;
    s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
    s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
    auto in = s.input(1);
    const EnclosureProductPtr first = computeEnclosure(in);
    auto previous = std::make_shared<EnclosureProduct>(*first);
    for (TileEnclosure& t : previous->mTiles)
    {
        for (GapEnclosure& g : t.mGaps)
        {
            g.mBucket = static_cast<U8>(EnclosureBucket::INDOORS);
        }
    }
    in->mPrevious = previous;
    const EnclosureProductPtr second = computeEnclosure(in);
    U32 held = 0, left = 0;
    const TileEnclosure& t = second->mTiles[0];
    for (size_t i = 0; i < t.mGaps.size(); ++i)
    {
        const GapEnclosure& g = t.mGaps[i];
        if (g.mD == ENCLOSURE_D_UNKNOWN)
        {
            continue;
        }
        const F32 e = g.mE / 255.f;
        if (e > 0.705f && e <= 0.745f)
        {
            held += g.mBucket == static_cast<U8>(EnclosureBucket::INDOORS);
            CHECK(g.mBucket == static_cast<U8>(EnclosureBucket::INDOORS));
            CHECK(first->mTiles[0].mGaps[i].mBucket == static_cast<U8>(EnclosureBucket::SHELTERED));
        }
        else if (e < 0.695f)
        {
            left += 1;
            CHECK(g.mBucket != static_cast<U8>(EnclosureBucket::INDOORS));
        }
    }
    CHECK(held > 0);
    CHECK(left > 0);

    // Rebuilt air, the same shape under new records: the metre's last bucket holds it.
    for (TileEnclosure& t : previous->mTiles)
    {
        for (CoarseGap& g : t.mCoarse.mGaps)
        {
            g.mBucket = static_cast<U8>(EnclosureBucket::INDOORS);
        }
    }
    auto rebuilt = s.input(1);
    rebuilt->mPrevious = previous;
    const EnclosureProductPtr third = computeEnclosure(rebuilt);
    CHECK(third->mTiles[0].mAir != first->mTiles[0].mAir);
    U32 kept = 0;
    for (const GapEnclosure& g : third->mTiles[0].mGaps)
    {
        const F32 e = g.mE / 255.f;
        if (g.mD != ENCLOSURE_D_UNKNOWN && e > 0.705f && e <= 0.745f)
        {
            CHECK(g.mBucket == static_cast<U8>(EnclosureBucket::INDOORS));
            ++kept;
        }
    }
    CHECK(kept > 0);
    // The coarse buckets hold the same way: against a previous generation
    // that read every metre SHELTERED, those just past 0.75 stay so.
    for (TileEnclosure& t : previous->mTiles)
    {
        for (CoarseGap& g : t.mCoarse.mGaps)
        {
            g.mBucket = static_cast<U8>(EnclosureBucket::SHELTERED);
        }
    }
    const EnclosureProductPtr fourth = computeEnclosure(rebuilt);
    U32 coarse_held = 0;
    for (const CoarseGap& g : fourth->mTiles[0].mCoarse.mGaps)
    {
        const F32 e = g.mE / 255.f;
        if (g.mD != ENCLOSURE_D_UNKNOWN && e > 0.755f && e <= 0.795f)
        {
            CHECK(g.mBucket == static_cast<U8>(EnclosureBucket::SHELTERED));
            ++coarse_held;
        }
    }
    CHECK(coarse_held > 0);
}

TEST_CASE("with the detail gone a read falls back to the coarse product", "[sknavenclosure]")
{
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    s.mSolids[0].mX1 = 5.f;
    s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
    s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    auto coarse = std::make_shared<EnclosureProduct>(*p);
    for (TileEnclosure& t : coarse->mTiles)
    {
        t.mAir.reset();
        t.mGaps.clear();
    }
    const Enclosure fine = at(p, 14.f, 14.f), rough = at(coarse, 14.f, 14.f);
    CHECK(rough.mBucket == EnclosureBucket::INDOORS);
    CHECK((std::fabs(rough.mE - fine.mE) < 0.05f && std::fabs(rough.mD - fine.mD) < 1.f));
    CHECK(at(coarse, 25.f, 25.f).mBucket == EnclosureBucket::OUTDOORS);
    const Enclosure roof = at(coarse, 10.f, 10.f, 3.5f);
    CHECK((roof.mS == 1.f && roof.mFloor > 3.f));
}

TEST_CASE("a mesh with no way out reads its neighbour's depth across their edge", "[sknavenclosure]")
{
    // Mesh B is a hall whose door opens onto mesh A's field at x = 32.
    Scene field;
    field.mGroundX1 = 32.f;
    const EnclosureProductPtr a = computeEnclosure(field.input(1));
    Scene hall;
    hall.mGroundX0 = 32.f;
    hall.mGroundX1 = 44.f;
    hall.mSolids.push_back({ 32.f, 4.f, 44.f, 4.5f, 0.f, 3.f, false });
    hall.mSolids.push_back({ 32.f, 15.5f, 44.f, 16.f, 0.f, 3.f, false });
    hall.mSolids.push_back({ 43.5f, 4.f, 44.f, 16.f, 0.f, 3.f, false });
    hall.mSolids.push_back({ 32.f, 4.f, 32.5f, 9.f, 0.f, 3.f, false });
    hall.mSolids.push_back({ 32.f, 10.f, 32.5f, 16.f, 0.f, 3.f, false });
    hall.mSolids.push_back({ 32.f, 9.f, 32.5f, 10.f, 2.25f, 3.f, false });
    hall.mSolids.push_back({ 32.f, 4.f, 44.f, 16.f, 3.f, 3.25f, true });
    // Only the hall's footprint has air in mesh B: keep tile 1, drop the rest.
    auto in = hall.input(2);
    in->mTiles.erase(std::remove_if(in->mTiles.begin(), in->mTiles.end(), [](const EnclosureInput::Tile& t) { return t.mKey.mX != 1; }),
                     in->mTiles.end());
    const EnclosureProductPtr alone = computeEnclosure(in);
    CHECK(at(alone, 40.f, 9.5f).mSealed);
    in->mNeighbours.push_back({ LLVector3d::zero, alone });
    CHECK(at(computeEnclosure(in), 40.f, 9.5f).mSealed);
    in->mNeighbours.clear();
    in->mNeighbours.push_back({ LLVector3d::zero, a });
    const EnclosureProductPtr joined = computeEnclosure(in);
    const Enclosure e = at(joined, 40.f, 9.5f);
    CHECK(!e.mSealed);
    CHECK(e.mBucket == EnclosureBucket::INDOORS);
    CHECK((e.mD > 6.f && e.mD < 11.f));
}

TEST_CASE("a solid overhead hides the share of sky its size and height give it, whoever published it", "[sknavenclosure]")
{
    Scene s;
    auto in = s.input(1);
    // A platform 10 m up over x, y 8 to 16, as another tile's solid tops.
    TileAir platform;
    platform.mCell = CELL;
    platform.mUnit = UNIT;
    platform.mTopCell = 2;
    platform.mTopWidth = platform.mTopHeight = 32;
    platform.mTopStart.assign(32 * 32 + 1, 0);
    for (S32 ty = 0; ty < 32; ++ty)
    {
        for (S32 tx = 0; tx < 32; ++tx)
        {
            if (tx >= 8 && tx < 16 && ty >= 8 && ty < 16)
            {
                platform.mTop.push_back({ 88, 92 });
            }
            platform.mTopStart[tx + ty * 32 + 1] = static_cast<U32>(platform.mTop.size());
        }
    }
    auto sky = std::make_shared<SkyMap>();
    sky->set(2, LLVector3d(0.0, 0.0, -1.0), platform);
    in->mSky = sky;
    const EnclosureProductPtr p = computeEnclosure(in);
    // An 8 m square 8.5 m over a standing head hides about a fifth of the sky.
    const Enclosure under = at(p, 12.1f, 12.1f);
    CHECK((under.mS < 0.9f && under.mS > 0.65f));
    CHECK(under.mBucket == EnclosureBucket::OUTDOORS);
    CHECK(at(p, 2.f, 2.f).mS > 0.9f);

    auto own = std::make_shared<SkyMap>();
    own->set(1, LLVector3d(0.0, 0.0, -1.0), platform);
    in->mSky = own;
    const Enclosure mine = at(computeEnclosure(in), 12.1f, 12.1f);
    CHECK(mine.mS < 0.9f);
    CHECK(std::fabs(mine.mS - under.mS) < 0.01f);
}

TEST_CASE("air under a desk on a sealed storey does not take the street's depth through the floor", "[sknavenclosure]")
{
    Scene s;
    s.box(8.f, 8.f, 24.f, 24.f, 4.f, 5.f);                  // the storey's floor slab over an open arcade
    s.box(8.f, 8.f, 24.f, 8.5f, 5.f, 8.f, false);
    s.box(8.f, 23.5f, 24.f, 24.f, 5.f, 8.f, false);
    s.box(8.f, 8.f, 8.5f, 24.f, 5.f, 8.f, false);
    s.box(23.5f, 8.f, 24.f, 24.f, 5.f, 8.f, false);
    s.box(8.f, 8.f, 24.f, 24.f, 8.f, 8.25f);
    s.box(9.f, 9.f, 11.f, 10.f, 5.75f, 6.f, false);         // a desk top, 0.75 m of air under it
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure under = at(p, 9.6f, 9.6f, 5.3f);
    CHECK(at(p, 9.6f, 9.6f).mBucket != EnclosureBucket::UNKNOWN);
    CHECK(under.mBucket != EnclosureBucket::OUTDOORS);
    CHECK(under.mBucket == at(p, 16.f, 16.f, 5.5f).mBucket);
}

TEST_CASE("depth carried into unwalkable air does not climb a facade", "[sknavenclosure]")
{
    Scene s;
    // Steps too narrow to stand on, a metre higher every half metre.
    for (S32 k = 1; k <= 12; ++k)
    {
        s.box(10.f + 0.5f * (k - 1), 10.f, 10.f + 0.5f * k, 12.f, 0.f, static_cast<F32>(k), false);
    }
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    CHECK(at(p, 10.25f, 11.f, 1.5f).mBucket != EnclosureBucket::UNKNOWN);
    CHECK(at(p, 15.75f, 11.f, 12.5f).mBucket == EnclosureBucket::UNKNOWN);
}

TEST_CASE("a sky map copy is a snapshot: later changes to the original leave it alone", "[sknavenclosure]")
{
    TileAir roof;
    roof.mCell = CELL;
    roof.mUnit = UNIT;
    roof.mTopCell = 2;
    roof.mTopWidth = roof.mTopHeight = 32;
    roof.mTopStart.assign(32 * 32 + 1, 0);
    for (S32 c = 0; c < 32 * 32; ++c)
    {
        roof.mTop.push_back({ 80, 88 });
        roof.mTopStart[c + 1] = static_cast<U32>(roof.mTop.size());
    }
    SkyMap live;
    live.set(1, LLVector3d(0.0, 0.0, 0.0), roof);
    const SkyMap snapshot = live;
    live.remove(1);
    live.set(2, LLVector3d(32.0, 0.0, 0.0), roof);
    CHECK(live.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));
    CHECK(!snapshot.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));
    CHECK(snapshot.skyOpenAt(LLVector3d(37.0, 5.0, 1.0)));
    CHECK((snapshot.cells()) == (static_cast<size_t>(32 * 32)));
}

TEST_CASE("an evicted tile keeps answering from its coarse product", "[sknavenclosure]")
{
    Scene s;
    s.room(36.f, 4.f, 48.f, 16.f);
    s.mSolids[0].mX1 = 37.f;
    s.box(38.f, 4.f, 48.f, 4.5f, 0.f, 3.f, false);
    s.box(37.f, 4.f, 38.f, 4.5f, 2.25f, 3.f, false);
    auto in = s.input(2);
    const EnclosureProductPtr first = computeEnclosure(in);
    // Tile 1's detail goes; the pass runs again on tile 0 alone.
    in->mPrevious = first;
    in->mKept.push_back(*first->tile(in->mTiles[1].mKey));
    in->mTiles.pop_back();
    const EnclosureProductPtr second = computeEnclosure(in);
    const TileEnclosure* kept = second->tile(first->mTiles[1].mKey);
    CHECK(kept != nullptr);
    CHECK((!kept->mAir && kept->mGaps.empty()));
    CHECK(at(second, 46.f, 14.f).mBucket == EnclosureBucket::INDOORS);
    CHECK(at(second, 10.f, 10.f).mBucket == EnclosureBucket::OUTDOORS);
}

TEST_CASE("a skybox over a terrain mesh takes no open sky from it: the terrain's air ends over its own top", "[sknavenclosure]")
{
    Scene field;
    auto field_in = field.input(1);
    // The ground's solid tops, as a built tile carries them: the field's own top is 0.
    auto tops = std::make_shared<TileAir>(*field_in->mTiles[0].mAir);
    tops->mTopCell = static_cast<S32>(1.f / CELL);
    tops->mTopWidth = tops->mTopHeight = 32;
    tops->mTopStart.assign(32 * 32 + 1, 0);
    tops->mTop.clear();
    for (S32 c = 0; c < 32 * 32; ++c)
    {
        tops->mTop.push_back({ 0, static_cast<U16>(1.f / UNIT) });
        tops->mTopStart[c + 1] = static_cast<U32>(tops->mTop.size());
    }
    field_in->mTiles[0].mAir = tops;
    const EnclosureProductPtr a = computeEnclosure(field_in);
    Scene box;
    box.mGround = false;
    box.box(4.f, 4.f, 16.f, 16.f, 19.75f, 20.f);
    box.box(4.f, 4.f, 16.f, 4.5f, 20.f, 23.f, false);
    box.box(4.f, 15.5f, 16.f, 16.f, 20.f, 23.f, false);
    box.box(4.f, 4.f, 4.5f, 16.f, 20.f, 23.f, false);
    box.box(15.5f, 4.f, 16.f, 16.f, 20.f, 23.f, false);
    box.box(4.f, 4.f, 16.f, 16.f, 23.f, 23.25f);
    auto in = box.input(1);
    in->mNeighbours.push_back({ LLVector3d::zero, a });
    const Enclosure e = at(computeEnclosure(in), 10.f, 10.f, 20.5f);
    CHECK(e.mS < 0.1f);
    CHECK(e.mBucket == EnclosureBucket::INDOORS);
}

TEST_CASE("the sky a gap reads does not depend on the order its tile's leaves are stored in", "[sknavenclosure]")
{
    Scene s;
    for (S32 k = 0; k < 6; ++k)
    {
        s.box(3.f + 4.5f * k, 4.f, 4.5f + 4.5f * k, 28.f, 0.f, 2.f + 0.75f * k, false);
    }
    s.room(6.f, 6.f, 13.f, 12.f);
    auto in = s.input(1);
    TileAir platform;
    platform.mCell = CELL;
    platform.mUnit = UNIT;
    platform.mTopCell = 2;
    platform.mTopWidth = platform.mTopHeight = 32;
    platform.mTopStart.assign(32 * 32 + 1, 0);
    for (S32 ty = 0; ty < 32; ++ty)
    {
        for (S32 tx = 0; tx < 32; ++tx)
        {
            if ((tx + ty) % 3 == 0)
            {
                platform.mTop.push_back({ static_cast<U16>(40 + 4 * (tx % 5)), static_cast<U16>(44 + 4 * (tx % 5)) });
            }
            platform.mTopStart[tx + ty * 32 + 1] = static_cast<U32>(platform.mTop.size());
        }
    }
    auto sky = std::make_shared<SkyMap>();
    sky->set(2, LLVector3d(0.0, 0.0, -1.0), platform);
    in->mSky = sky;
    const EnclosureProductPtr stored = computeEnclosure(in);

    // The same air with its leaves reversed, gaps gathered in their new order.
    const TileAir& a = *in->mTiles[0].mAir;
    auto reversed = std::make_shared<TileAir>(a);
    reversed->mLeaves.assign(a.mLeaves.rbegin(), a.mLeaves.rend());
    reversed->mGaps.clear();
    std::vector<U32> moved(a.mGaps.size());
    for (AirLeaf& l : reversed->mLeaves)
    {
        for (U32 i = 0; i < l.mGapCount; ++i)
        {
            moved[l.mFirstGap + i] = static_cast<U32>(reversed->mGaps.size());
            reversed->mGaps.push_back(a.mGaps[l.mFirstGap + i]);
        }
        l.mFirstGap = moved[l.mFirstGap];
    }
    auto again = std::make_shared<EnclosureInput>(*in);
    again->mTiles[0].mAir = reversed;
    const EnclosureProductPtr other = computeEnclosure(again);

    const std::vector<GapEnclosure>& x = stored->mTiles[0].mGaps;
    const std::vector<GapEnclosure>& y = other->mTiles[0].mGaps;
    CHECK((y.size()) == (x.size()));
    U32 differ = 0, read = 0;
    for (size_t g = 0; g < x.size(); ++g)
    {
        const GapEnclosure& b = y[moved[g]];
        differ += x[g].mS != b.mS || x[g].mCover != b.mCover || x[g].mOpen != b.mOpen;
        read += x[g].mCover > 0 && x[g].mCover < 255;
    }
    CHECK(read > 0);
    CHECK((differ) == (0u));
}

TEST_CASE("only detail air finer than the coarse cell reads as fine", "[sknavenclosure]")
{
    Scene s;
    s.room(4.f, 4.f, 16.f, 16.f);
    const EnclosureProductPtr p = computeEnclosure(s.input(1));
    const Enclosure fine = at(p, 10.f, 10.f);
    CHECK(fine.mBucket != EnclosureBucket::UNKNOWN);
    CHECK(fine.mFine);

    auto evicted = std::make_shared<EnclosureProduct>(*p);
    for (TileEnclosure& t : evicted->mTiles)
    {
        t.mAir.reset();
        t.mGaps.clear();
    }
    const Enclosure kept = at(evicted, 10.f, 10.f);
    CHECK(kept.mBucket != EnclosureBucket::UNKNOWN);
    CHECK(!kept.mFine);

    Scene past = s;
    past.mCell = COARSE_AIR_M;
    const Enclosure coarse = at(computeEnclosure(past.input(1)), 10.f, 10.f);
    CHECK(coarse.mBucket != EnclosureBucket::UNKNOWN);
    CHECK(!coarse.mFine);
}
