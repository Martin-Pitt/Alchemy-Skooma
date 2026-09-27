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

#include "../test/lltut.h"

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
            g.mCell = CELL;
            g.mUnit = UNIT;
            g.mWidth = g.mHeight = SIDE;
            g.mStart.assign(SIDE * SIDE + 1, 0);
            struct Run
            {
                S32 mZ0, mZ1;
                bool mWalk;
            };
            std::vector<Run> runs;
            for (S32 iy = 0; iy < SIDE; ++iy)
            {
                for (S32 ix = 0; ix < SIDE; ++ix)
                {
                    const F32 x = g.mOrigin.mV[VX] + (ix + 0.5f) * CELL, y = g.mOrigin.mV[VY] + (iy + 0.5f) * CELL;
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
                        gap.mFlags = merged[i].mWalk ? AirGap::WALKABLE : 0;
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
                    g.mStart[ix + iy * SIDE + 1] = static_cast<U32>(g.mGaps.size());
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

namespace tut
{
    struct sknavenclosure_data
    {
    };
    typedef test_group<sknavenclosure_data> sknavenclosure_group;
    typedef sknavenclosure_group::object sknavenclosure_object;
    tut::sknavenclosure_group sknavenclosure_tests("sknavenclosure");

    template<> template<>
    void sknavenclosure_object::test<1>()
    {
        set_test_name("an open field is open sky, depth 0, OUTDOORS");
        Scene s;
        const EnclosureProductPtr p = computeEnclosure(s.input(2));
        const Enclosure e = at(p, 40.f, 10.f);
        ensure_equals("S", e.mS, 1.f);
        ensure_equals("D", e.mD, 0.f);
        ensure("OUTDOORS", e.mBucket == EnclosureBucket::OUTDOORS);
        ensure_equals("nothing unknown", p->mUnknownGaps, 0u);
    }

    template<> template<>
    void sknavenclosure_object::test<2>()
    {
        set_test_name("a small canopy on posts is SHELTERED under its middle");
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
        ensure("S well under 1", e.mS < 0.6f && e.mS > 0.2f);
        ensure("SHELTERED", e.mBucket == EnclosureBucket::SHELTERED);
        ensure("outside it OUTDOORS", at(p, 10.f, 10.f).mBucket == EnclosureBucket::OUTDOORS);
    }

    template<> template<>
    void sknavenclosure_object::test<3>()
    {
        set_test_name("a closed room with a door is INDOORS deep inside, and its window gives light but not feet");
        Scene s;
        s.room(4.f, 4.f, 16.f, 16.f);
        // A door on the south wall: a gap in the wall under a lintel.
        s.mSolids[0].mX1 = 5.f;
        s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
        s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
        const EnclosureProductPtr p = computeEnclosure(s.input(1));
        const Enclosure deep = at(p, 14.f, 14.f);
        ensure("INDOORS deep inside", deep.mBucket == EnclosureBucket::INDOORS);
        ensure("far from the sky", deep.mS < 0.02f);
        ensure("deep", deep.mD > 8.f);
        ensure("OUTDOORS outside the door", at(p, 5.5f, 2.f).mBucket == EnclosureBucket::OUTDOORS);

        // A window on the north wall from 1 to 2 m, right by the deep point.
        Scene w = s;
        w.mSolids[1].mX1 = 13.f;
        w.box(15.f, 15.5f, 16.f, 16.f, 0.f, 3.f, false);
        w.box(13.f, 15.5f, 15.f, 16.f, 0.f, 1.f, true);     // a sill wide enough to stand on
        w.box(13.f, 15.5f, 15.f, 16.f, 2.f, 3.f, false);
        const EnclosureProductPtr pw = computeEnclosure(w.input(1));
        const Enclosure lit = at(pw, 14.f, 14.f);
        ensure("the window lets light in", lit.mS > deep.mS + 0.1f);
        ensure_equals("but not feet", lit.mD, deep.mD);
        ensure("and the room's middle stays INDOORS", at(pw, 10.f, 10.f).mBucket == EnclosureBucket::INDOORS);
    }

    template<> template<>
    void sknavenclosure_object::test<4>()
    {
        set_test_name("a room with only a window is UNKNOWN, never OUTDOORS");
        Scene s;
        s.room(4.f, 4.f, 16.f, 16.f);
        s.mSolids[1].mX1 = 9.f;
        s.box(11.f, 15.5f, 16.f, 16.f, 0.f, 3.f, false);
        s.box(9.f, 15.5f, 11.f, 16.f, 0.f, 1.f, false);
        s.box(9.f, 15.5f, 11.f, 16.f, 2.f, 3.f, false);
        const EnclosureProductPtr p = computeEnclosure(s.input(1));
        const Enclosure e = at(p, 10.f, 10.f);
        ensure("UNKNOWN", e.mBucket == EnclosureBucket::UNKNOWN);
        ensure("some light", e.mS > 0.f);
        ensure("counted", p->mUnknownGaps > 0);
    }

    template<> template<>
    void sknavenclosure_object::test<5>()
    {
        set_test_name("a street between towers, even under a skybridge, is OUTDOORS");
        Scene s;
        s.box(0.f, 0.f, 12.f, 32.f, 0.f, 30.f, false);
        s.box(16.f, 0.f, 32.f, 32.f, 0.f, 30.f, false);
        s.box(12.f, 14.5f, 16.f, 16.5f, 15.f, 15.5f, false);
        const EnclosureProductPtr p = computeEnclosure(s.input(1));
        ensure("the street", at(p, 14.f, 5.f).mBucket == EnclosureBucket::OUTDOORS);
        const Enclosure under = at(p, 14.f, 15.5f);
        ensure("under the bridge is not open sky", under.mS < 1.f);
        ensure("but still OUTDOORS", under.mBucket == EnclosureBucket::OUTDOORS);
    }

    template<> template<>
    void sknavenclosure_object::test<6>()
    {
        set_test_name("the air over a desk carries the room's depth");
        Scene s;
        s.room(4.f, 4.f, 16.f, 16.f);
        s.mSolids[0].mX1 = 5.f;
        s.box(6.f, 4.f, 16.f, 4.5f, 0.f, 3.f, false);
        s.box(5.f, 4.f, 6.f, 4.5f, 2.25f, 3.f, false);
        s.box(12.f, 12.f, 14.f, 13.f, 0.f, 1.f, false);     // a desk too high to step onto
        const EnclosureProductPtr p = computeEnclosure(s.input(1));
        const Enclosure desk = at(p, 13.f, 12.5f, 1.5f);
        const Enclosure floor = at(p, 13.f, 11.5f);
        ensure("desk air is not UNKNOWN", desk.mBucket != EnclosureBucket::UNKNOWN);
        ensure("desk air is INDOORS", desk.mBucket == EnclosureBucket::INDOORS);
        ensure("the floor's depth plus the way onto it", desk.mD >= floor.mD && desk.mD - floor.mD < 2.5f);
    }

    template<> template<>
    void sknavenclosure_object::test<7>()
    {
        set_test_name("depth floods across a tile seam");
        Scene s;
        s.room(24.f, 4.f, 48.f, 16.f);
        s.mSolids[2].mY1 = 9.f;         // a door in the west wall
        s.box(24.f, 10.f, 24.5f, 16.f, 0.f, 3.f, false);
        s.box(24.f, 9.f, 24.5f, 10.f, 2.25f, 3.f, false);
        const EnclosureProductPtr p = computeEnclosure(s.input(2));
        const Enclosure e = at(p, 44.f, 9.5f);
        ensure("reached", e.mBucket != EnclosureBucket::UNKNOWN);
        ensure("INDOORS past the seam", e.mBucket == EnclosureBucket::INDOORS);
        ensure("about 20 m in", e.mD > 18.f && e.mD < 23.f);
    }

    template<> template<>
    void sknavenclosure_object::test<8>()
    {
        set_test_name("buckets hold within the hysteresis of their edge");
        ensure("fresh 0.72 is SHELTERED", enclosureBucket(0.72f, EnclosureBucket::UNKNOWN, 0.05f) == EnclosureBucket::SHELTERED);
        ensure("0.72 stays INDOORS", enclosureBucket(0.72f, EnclosureBucket::INDOORS, 0.05f) == EnclosureBucket::INDOORS);
        ensure("0.69 leaves INDOORS", enclosureBucket(0.69f, EnclosureBucket::INDOORS, 0.05f) == EnclosureBucket::SHELTERED);
        ensure("0.28 stays OUTDOORS", enclosureBucket(0.28f, EnclosureBucket::OUTDOORS, 0.05f) == EnclosureBucket::OUTDOORS);
        ensure("0.21 stays SHELTERED", enclosureBucket(0.21f, EnclosureBucket::SHELTERED, 0.05f) == EnclosureBucket::SHELTERED);
        ensure("0.19 leaves SHELTERED", enclosureBucket(0.19f, EnclosureBucket::SHELTERED, 0.05f) == EnclosureBucket::OUTDOORS);
        ensure("0.9 from OUTDOORS", enclosureBucket(0.9f, EnclosureBucket::OUTDOORS, 0.05f) == EnclosureBucket::INDOORS);

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
                ensure("held", g.mBucket == static_cast<U8>(EnclosureBucket::INDOORS));
                ensure("was SHELTERED fresh", first->mTiles[0].mGaps[i].mBucket == static_cast<U8>(EnclosureBucket::SHELTERED));
            }
            else if (e < 0.695f)
            {
                left += 1;
                ensure("released", g.mBucket != static_cast<U8>(EnclosureBucket::INDOORS));
            }
        }
        ensure("some gaps sat in the band", held > 0);
        ensure("some left it", left > 0);
    }

    template<> template<>
    void sknavenclosure_object::test<9>()
    {
        set_test_name("with the detail gone a read falls back to the coarse product");
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
        ensure("INDOORS coarse", rough.mBucket == EnclosureBucket::INDOORS);
        ensure("close to the detail", std::fabs(rough.mE - fine.mE) < 0.05f && std::fabs(rough.mD - fine.mD) < 1.f);
        ensure("OUTDOORS coarse", at(coarse, 25.f, 25.f).mBucket == EnclosureBucket::OUTDOORS);
        const Enclosure roof = at(coarse, 10.f, 10.f, 3.5f);
        ensure("the roof's own gap", roof.mS == 1.f && roof.mFloor > 3.f);
    }

    template<> template<>
    void sknavenclosure_object::test<10>()
    {
        set_test_name("a mesh with no way out reads its neighbour's depth across their edge");
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
        ensure("alone the hall floor is UNKNOWN", at(alone, 40.f, 9.5f).mBucket == EnclosureBucket::UNKNOWN);
        in->mNeighbours.push_back({ LLVector3d::zero, a });
        const EnclosureProductPtr joined = computeEnclosure(in);
        const Enclosure e = at(joined, 40.f, 9.5f);
        ensure("with the field beside it, INDOORS", e.mBucket == EnclosureBucket::INDOORS);
        ensure("about 8 m in", e.mD > 6.f && e.mD < 11.f);
    }
}

namespace tut
{
    template<> template<>
    void sknavenclosure_object::test<11>()
    {
        set_test_name("another mesh's solid overhead shelters; the tile's own does not");
        Scene s;
        auto in = s.input(1);
        in->mTiles[0].mSkyContributor = 1;
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
        ensure("sheltered under it", at(p, 12.1f, 12.1f).mS < 0.5f);
        ensure("open beside it", at(p, 2.f, 2.f).mS == 1.f);

        auto own = std::make_shared<SkyMap>();
        own->set(1, LLVector3d(0.0, 0.0, -1.0), platform);
        in->mSky = own;
        ensure("its own solid is already in its air", at(computeEnclosure(in), 12.1f, 12.1f).mS == 1.f);
    }
}
