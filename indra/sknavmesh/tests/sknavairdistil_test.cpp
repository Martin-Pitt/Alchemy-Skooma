/**
 * @file sknavairdistil_test.cpp
 * @brief The air quadtree, its seams, its solid tops and the sky map built from them.
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

#include "../sknavairdistil.h"
#include "../sknavskymap.h"

#include "Recast.h"

#include <cstdlib>

namespace
{
    using namespace sknav;

    const S32 CLIMB = 6;
    const S32 OVERLAP = 4;

    /// A 64 x 64 room at 0.125 m: open floor, a wall across it at x = 20 with
    /// a doorway, a 0.25 m ramp of gentle floors, a mezzanine over one corner
    /// and a solid pillar.
    AirGrid room()
    {
        const S32 w = 64, h = 64;
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = w;
        g.mHeight = h;
        g.mStart.assign(static_cast<size_t>(w) * h + 1, 0);
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const bool wall = ix == 20 && (iy < 30 || iy > 37);
                const bool pillar = ix >= 44 && ix < 48 && iy >= 8 && iy < 12;
                if (!wall && !pillar)
                {
                    AirGap floor;
                    floor.mFloor = ix > 24 && ix < 40 ? static_cast<U16>((ix - 24) / 8) : 0;  // a step every metre, within climb
                    floor.mFlags = AirGap::WALKABLE;
                    if (ix >= 48 && iy >= 48)
                    {
                        floor.mCeiling = 24;
                        g.mGaps.push_back(floor);
                        AirGap upper;
                        upper.mFloor = 26;
                        upper.mCeiling = 400;
                        upper.mFlags = AirGap::WALKABLE | AirGap::OPEN | AirGap::OWN_SKY;
                        g.mGaps.push_back(upper);
                    }
                    else
                    {
                        floor.mCeiling = 400;
                        floor.mFlags |= AirGap::OPEN | AirGap::OWN_SKY;
                        g.mGaps.push_back(floor);
                    }
                }
                g.mStart[ix + iy * w + 1] = static_cast<U32>(g.mGaps.size());
            }
        }
        computeClearance(g, CLIMB, OVERLAP);
        return g;
    }

    /// Per cell, the leaf covering it, or -1; false if any cell is covered twice.
    bool cover(const TileAir& air, std::vector<S32>& out)
    {
        out.assign(static_cast<size_t>(air.mWidth) * air.mHeight, -1);
        for (size_t i = 0; i < air.mLeaves.size(); ++i)
        {
            const AirLeaf& l = air.mLeaves[i];
            const S32 s = 1 << l.mLevel;
            for (S32 y = l.mY; y < l.mY + s; ++y)
            {
                for (S32 x = l.mX; x < l.mX + s; ++x)
                {
                    if (x >= air.mWidth || y >= air.mHeight || out[x + y * air.mWidth] >= 0)
                    {
                        return false;
                    }
                    out[x + y * air.mWidth] = static_cast<S32>(i);
                }
            }
        }
        return true;
    }
}

namespace tut
{
    struct sknavairdistil_data
    {
    };
    typedef test_group<sknavairdistil_data> sknavairdistil_group;
    typedef sknavairdistil_group::object sknavairdistil_object;
    tut::sknavairdistil_group sknavairdistil_tests("sknavairdistil");

    template<> template<>
    void sknavairdistil_object::test<1>()
    {
        set_test_name("a merged leaf covers exactly the cells it replaced, gaps within climb");
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        std::vector<S32> leaf_of;
        ensure("no cell covered twice", cover(air, leaf_of));
        size_t coarse = 0;
        for (S32 iy = 0; iy < g.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < g.mWidth; ++ix)
            {
                const S32 li = leaf_of[ix + iy * g.mWidth];
                const std::string at = llformat(" at (%d, %d)", ix, iy);
                if (g.count(ix, iy) == 0)
                {
                    ensure("a solid column has no leaf" + at, li < 0);
                    continue;
                }
                ensure("every column with air is covered" + at, li >= 0);
                const AirLeaf& l = air.mLeaves[li];
                coarse += l.mLevel > 0;
                ensure_equals("gap count" + at, static_cast<U32>(l.mGapCount), g.count(ix, iy));
                for (U32 k = 0; k < l.mGapCount; ++k)
                {
                    const AirGap& fine = g.gaps(ix, iy)[k];
                    const AirGap& rec = air.mGaps[l.mFirstGap + k];
                    ensure_equals("flags" + at, rec.mFlags, fine.mFlags);
                    ensure("floor within climb" + at, std::abs(rec.mFloor - fine.mFloor) <= CLIMB);
                    ensure("air inside the column's" + at, rec.mFloor >= fine.mFloor && rec.mClear <= fine.mClear);
                    if (fine.mFlags & AirGap::OPEN)
                    {
                        ensure_equals("open ceiling" + at, rec.mCeiling, AIR_CEILING_OPEN);
                    }
                    else
                    {
                        ensure("ceiling within climb" + at, rec.mCeiling <= fine.mCeiling && fine.mCeiling - rec.mCeiling <= CLIMB);
                    }
                }
                if (l.mLevel > 0)
                {
                    for (U32 k = 0; k < l.mGapCount; ++k)
                    {
                        const AirGap& fine = g.gaps(ix, iy)[k];
                        const bool usable = (fine.mFlags & (AirGap::WALKABLE | AirGap::OPEN)) || fine.mCeiling - fine.mFloor >= 8;
                        ensure("no merged usable cell touches a wall" + at, fine.mClear > 0 || !usable);
                    }
                }
            }
        }
        ensure("open floor merges", coarse > g.mGaps.size() / 2);
        ensure("far fewer leaves than columns", air.mLeaves.size() * 4 < static_cast<size_t>(g.mWidth * g.mHeight));
        ensure("open floor away from the wall is a 1 m leaf", air.mLeaves[leaf_of[4 + 40 * g.mWidth]].mLevel >= 3);
        ensure_equals("beside the wall stays fine", static_cast<S32>(air.mLeaves[leaf_of[19 + 10 * g.mWidth]].mLevel), 0);
    }

    template<> template<>
    void sknavairdistil_object::test<2>()
    {
        set_test_name("touching leaves differ by at most one level");
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        std::vector<S32> leaf_of;
        ensure("covered once", cover(air, leaf_of));
        S32 worst = 0;
        for (S32 iy = 0; iy < g.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < g.mWidth; ++ix)
            {
                const S32 a = leaf_of[ix + iy * g.mWidth];
                const S32 right = ix + 1 < g.mWidth ? leaf_of[ix + 1 + iy * g.mWidth] : -1;
                const S32 up = iy + 1 < g.mHeight ? leaf_of[ix + (iy + 1) * g.mWidth] : -1;
                for (S32 b : { right, up })
                {
                    if (a >= 0 && b >= 0)
                    {
                        worst = std::max(worst, std::abs(air.mLeaves[a].mLevel - air.mLeaves[b].mLevel));
                    }
                }
            }
        }
        ensure("restricted", worst <= 1);
    }

    template<> template<>
    void sknavairdistil_object::test<3>()
    {
        set_test_name("a block whose own sky or flags change inside it does not merge");
        AirGrid g = room();
        TileAir before;
        distilAir(g, CLIMB, before);
        std::vector<S32> leaf_of;
        cover(before, leaf_of);
        ensure("a 1 m leaf before", before.mLeaves[leaf_of[4 + 40 * g.mWidth]].mLevel >= 3);
        // Clear the own-sky bit over a patch straddling that leaf's corner.
        for (S32 iy = 36; iy < 44; ++iy)
        {
            for (S32 ix = 0; ix < 4; ++ix)
            {
                AirGap& gap = g.mGaps[g.mStart[ix + iy * g.mWidth]];
                gap.mFlags &= static_cast<U8>(~AirGap::OWN_SKY);
            }
        }
        TileAir air;
        distilAir(g, CLIMB, air);
        cover(air, leaf_of);
        ensure("no leaf straddles the change", air.mLeaves[leaf_of[4 + 40 * g.mWidth]].mLevel <= 2);
        ensure("inside it the flags hold", (air.mGaps[air.mLeaves[leaf_of[1 + 40 * g.mWidth]].mFirstGap].mFlags & AirGap::OWN_SKY) == 0);
    }

    template<> template<>
    void sknavairdistil_object::test<4>()
    {
        set_test_name("seam strips are the tile's edge columns, cell for cell");
        const AirGrid g = room();
        TileAir air;
        airSeams(g, air);
        for (S32 side = 0; side < 4; ++side)
        {
            const SeamStrip& s = air.mSeams[side];
            const bool along_y = side < 2;
            const S32 n = along_y ? g.mHeight : g.mWidth;
            ensure_equals("cells along the side", static_cast<S32>(s.mStart.size()), n + 1);
            for (S32 i = 0; i < n; ++i)
            {
                const S32 ix = along_y ? (side == 0 ? 0 : g.mWidth - 1) : i;
                const S32 iy = along_y ? i : (side == 2 ? 0 : g.mHeight - 1);
                ensure_equals(llformat("gaps on side %d cell %d", side, i), s.count(i), g.count(ix, iy));
                for (U32 k = 0; k < s.count(i); ++k)
                {
                    const AirGap& a = s.mGaps[s.mStart[i] + k];
                    const AirGap& b = g.gaps(ix, iy)[k];
                    ensure("floor", a.mFloor == b.mFloor);
                    ensure("ceiling", a.mCeiling == ((b.mFlags & AirGap::OPEN) ? AIR_CEILING_OPEN : b.mCeiling));
                    ensure("flags and clearance", a.mFlags == b.mFlags && a.mClear == b.mClear);
                }
            }
        }
    }

    template<> template<>
    void sknavairdistil_object::test<5>()
    {
        set_test_name("solid tops merge per metre, and the sky map forgets a roof that is gone");
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        const F32 bmin[3] = { 0.f, 0.f, 0.f };
        const F32 bmax[3] = { 4.f, 40.f, 4.f };
        // 16 x 16 cells of 0.25 m with a one-cell border: 14 x 14 inside.
        rcCreateHeightfield(&ctx, *hf, 16, 16, bmin, bmax, 0.25f, 0.25f);
        for (S32 x = 1; x < 15; ++x)
        {
            for (S32 z = 1; z < 15; ++z)
            {
                rcAddSpan(&ctx, *hf, x, z, 0, 4, RC_WALKABLE_AREA, 1);
            }
        }
        // A roof over Recast x 1..4, z 1..4: the first metre cell of the tile.
        for (S32 x = 1; x < 5; ++x)
        {
            for (S32 z = 1; z < 5; ++z)
            {
                rcAddSpan(&ctx, *hf, x, z, 20 + (x & 1), 22, RC_NULL_AREA, 1);
            }
        }
        TileAir air;
        air.mCell = 0.25f;
        air.mUnit = 0.25f;
        airTops(*hf, 1, 4, air);
        rcFreeHeightField(hf);
        ensure_equals("metre cells across", air.mTopWidth, 4);
        ensure_equals("roofed cell: ground and roof", air.mTopStart[1] - air.mTopStart[0], 2u);
        ensure_equals("roof merged down to its lowest bottom", air.mTop[air.mTopStart[0] + 1].mBottom, 20);
        ensure_equals("open cell: ground only", air.mTopStart[2] - air.mTopStart[1], 1u);

        SkyMap sky;
        const LLVector3d tile(1000.0, 2000.0, 30.0);
        sky.set(7, tile, air);
        const LLVector3d under(1000.5, 2000.5, 31.5), beside(1001.5, 2000.5, 31.5), over(1000.5, 2000.5, 36.0);
        ensure("under the roof is covered", !sky.skyOpenAt(under));
        ensure("beside it is open", sky.skyOpenAt(beside));
        ensure("above the roof is open", sky.skyOpenAt(over));
        ensure("inside the roof is not", !sky.skyOpenAt(LLVector3d(1000.5, 2000.5, 35.2)));
        ensure("beyond reach is open", sky.skyOpenAt(under - LLVector3d(0, 0, 60.0), 50.f));
        const U64 v = sky.version();

        // A second tile's roof over the same cell, then the first republished roofless.
        TileAir other = air;
        sky.set(8, tile + LLVector3d(0, 0, 10.0), other);
        TileAir bare = air;
        bare.mTop.erase(bare.mTop.begin() + 1);
        for (size_t i = 1; i < bare.mTopStart.size(); ++i)
        {
            bare.mTopStart[i] -= 1;
        }
        sky.set(7, tile, bare);
        ensure("the other tile's roof still covers", !sky.skyOpenAt(under));
        sky.remove(8);
        ensure("a deleted roof is forgotten", sky.skyOpenAt(under));
        ensure("changes are versioned", sky.version() > v);
        sky.remove(7);
        ensure_equals("nothing left", sky.cells(), static_cast<size_t>(0));
    }

    template<> template<>
    void sknavairdistil_object::test<6>()
    {
        set_test_name("the packed form is the records back to back, little-endian");
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        airSeams(g, air);
        air.mTopStart.assign(1, 0);
        const std::vector<U8> blob = packTileAir(air);
        size_t seams = 0;
        for (const SeamStrip& s : air.mSeams)
        {
            seams += s.mStart.size() * 4 + s.mGaps.size() * 8;
        }
        ensure_equals("size", blob.size(), air.mLeaves.size() * 10 + air.mGaps.size() * 8 + seams + 4);
        const AirLeaf& l = air.mLeaves[1];
        ensure_equals("second leaf's x", static_cast<S32>(blob[10] | (blob[11] << 8)), static_cast<S32>(l.mX));
        ensure_equals("second leaf's first gap", static_cast<U32>(blob[16] | (blob[17] << 8) | (blob[18] << 16) | (blob[19] << 24)), l.mFirstGap);
        ensure("layout names the sections", tileAirLayout(air).find("\"seam_pos_y\"") != std::string::npos);
    }

    template<> template<>
    void sknavairdistil_object::test<7>()
    {
        set_test_name("a low closed gap walled in does not keep its block fine");
        AirGrid g = room();
        // Under a 2 m block of open floor, a 0.5 m crawlspace walled at one corner.
        for (S32 iy = 32; iy < 48; ++iy)
        {
            for (S32 ix = 0; ix < 16; ++ix)
            {
                AirGap& gap = g.mGaps[g.mStart[ix + iy * g.mWidth]];
                gap.mFlags = AirGap::OPEN | AirGap::OWN_SKY;
                gap.mClear = (ix == 0 && iy == 40) ? 0 : gap.mClear;
            }
        }
        TileAir air;
        distilAir(g, CLIMB, air);
        std::vector<S32> leaf_of;
        cover(air, leaf_of);
        ensure("an open gap at a wall stays fine", air.mLeaves[leaf_of[0 + 40 * g.mWidth]].mLevel == 0);
        for (S32 iy = 32; iy < 48; ++iy)
        {
            for (S32 ix = 0; ix < 16; ++ix)
            {
                AirGap& gap = g.mGaps[g.mStart[ix + iy * g.mWidth]];
                gap.mFlags = 0;
                gap.mCeiling = gap.mFloor + 4;
            }
        }
        distilAir(g, CLIMB, air);
        cover(air, leaf_of);
        ensure("a crawlspace at a wall merges", air.mLeaves[leaf_of[0 + 40 * g.mWidth]].mLevel > 0);
    }

    template<> template<>
    void sknavairdistil_object::test<8>()
    {
        set_test_name("a column of more than 255 gaps is capped alike in its leaf and its seam");
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = g.mHeight = 2;
        g.mStart = { 0, 0, 0, 0, 0 };
        for (S32 i = 0; i < 300; ++i)
        {
            AirGap gap;
            gap.mFloor = static_cast<U16>(i * 10);
            gap.mCeiling = static_cast<U16>(i * 10 + 5);
            gap.mClear = 1;
            g.mGaps.push_back(gap);
        }
        g.mStart = { 0, 300, 300, 300, 300 };
        TileAir air;
        distilAir(g, CLIMB, air);
        airSeams(g, air);
        ensure_equals("leaf capped", static_cast<U32>(air.mLeaves[0].mGapCount), 255u);
        ensure_equals("seam capped", air.mSeams[TileAir::SIDE_NEG_X].count(0), 255u);
        ensure_equals("seam along y capped", air.mSeams[TileAir::SIDE_NEG_Y].count(0), 255u);
    }
}
