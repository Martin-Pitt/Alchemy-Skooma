/**
 * @file skairdistil_test.cpp
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

#include <catch2/catch_test_macros.hpp>

#include "../skairdistil.h"
#include "../skairskymap.h"

#include "Recast.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

namespace
{
    using namespace skworld;

    const S32 CLIMB = 6;
    const S32 OVERLAP = 4;

    /// A room at 0.125 m, 64 x 64 unless cropped: open floor, a wall across it at x = 20 with
    /// a doorway, a 0.25 m ramp of gentle floors, a mezzanine over one corner
    /// and a solid pillar.
    AirGrid room(S32 w = 64, S32 h = 64)
    {
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
                    floor.mFlags = AirGap::TOP;
                    if (ix >= 48 && iy >= 48)
                    {
                        floor.mCeiling = 24;
                        g.mGaps.push_back(floor);
                        AirGap upper;
                        upper.mFloor = 26;
                        upper.mCeiling = 400;
                        upper.mFlags = AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY;
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

    /// Per cell, its finest leaf, or -1: leaves come coarsest first, so finer ones overwrite.
    void cover(const TileAir& air, std::vector<S32>& out)
    {
        out.assign(static_cast<size_t>(air.mWidth) * air.mHeight, -1);
        for (size_t i = 0; i < air.mLeaves.size(); ++i)
        {
            const AirLeaf& l = air.mLeaves[i];
            const S32 s = 1 << l.mLevel;
            for (S32 y = l.mY; y < std::min(air.mHeight, l.mY + s); ++y)
            {
                for (S32 x = l.mX; x < std::min(air.mWidth, l.mX + s); ++x)
                {
                    out[x + y * air.mWidth] = static_cast<S32>(i);
                }
            }
        }
    }

    bool covers(const AirLeaf& l, S32 x, S32 y)
    {
        return x >= l.mX && y >= l.mY && x < l.mX + (1 << l.mLevel) && y < l.mY + (1 << l.mLevel);
    }

    /// The leaf holding each gap.
    std::vector<S32> leafOfGap(const TileAir& air)
    {
        std::vector<S32> out(air.mGaps.size(), -1);
        for (size_t i = 0; i < air.mLeaves.size(); ++i)
        {
            for (U32 k = 0; k < air.mLeaves[i].mGapCount; ++k)
            {
                out[air.mLeaves[i].mFirstGap + k] = static_cast<S32>(i);
            }
        }
        return out;
    }

    /// The level of the leaf holding the column's gap whose floor is nearest floor, or -1.
    S32 levelAt(const TileAir& air, S32 ix, S32 iy, S32 floor)
    {
        AirColumn column;
        airColumn(air, findLeafByKey(air, ix, iy), column);
        const std::vector<S32> owner = leafOfGap(air);
        S32 best = -1, dist = 1 << 30;
        for (U32 k = 0; k < column.mCount; ++k)
        {
            const S32 d = std::abs(static_cast<S32>(air.mGaps[column.mGap[k]].mFloor) - floor);
            if (d < dist)
            {
                dist = d;
                best = air.mLeaves[owner[column.mGap[k]]].mLevel;
            }
        }
        return best;
    }

    /// One open walkable floor per column at the given height, 64 x 64, with solid where wall says.
    AirGrid field(const std::function<S32(S32, S32)>& floor, const std::function<bool(S32, S32)>& wall = nullptr, S32 n = 64)
    {
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = g.mHeight = n;
        g.mStart.assign(static_cast<size_t>(n) * n + 1, 0);
        for (S32 iy = 0; iy < n; ++iy)
        {
            for (S32 ix = 0; ix < n; ++ix)
            {
                if (!wall || !wall(ix, iy))
                {
                    AirGap gap;
                    gap.mFloor = static_cast<U16>(floor(ix, iy));
                    gap.mCeiling = 400;
                    gap.mFlags = AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY;
                    g.mGaps.push_back(gap);
                }
                g.mStart[ix + iy * n + 1] = static_cast<U32>(g.mGaps.size());
            }
        }
        computeClearance(g, CLIMB, OVERLAP);
        return g;
    }

    /// The level of a column's one leaf, and its floor there on the leaf's plane; -1 for no air.
    S32 leafAt(const TileAir& air, S32 ix, S32 iy, S32* floor = nullptr)
    {
        AirColumn column;
        airColumn(air, findLeafByKey(air, ix, iy), column);
        if (!column.mCount)
        {
            return -1;
        }
        if (floor)
        {
            *floor = column.floorAt(air, 0, ix, iy);
        }
        return air.mLeaves[column.mLeaf[0]].mLevel;
    }

    /// A flat slab over ground that varies under it: the lower gaps change from column to column, the slab's top does not.
    AirGrid slab(const std::function<void(S32, S32, std::vector<AirGap>&)>& under, S32 n = 64)
    {
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = g.mHeight = n;
        g.mStart.assign(static_cast<size_t>(n) * n + 1, 0);
        std::vector<AirGap> column;
        for (S32 iy = 0; iy < n; ++iy)
        {
            for (S32 ix = 0; ix < n; ++ix)
            {
                column.clear();
                under(ix, iy, column);
                AirGap top;
                top.mFloor = 80;    // the slab, from 9.5 to 10 m
                top.mCeiling = 400;
                top.mFlags = AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY;
                if (column.empty() || column.back().mCeiling <= 76)
                {
                    column.push_back(top);
                }
                g.mGaps.insert(g.mGaps.end(), column.begin(), column.end());
                g.mStart[ix + iy * n + 1] = static_cast<U32>(g.mGaps.size());
            }
        }
        computeClearance(g, CLIMB, OVERLAP);
        return g;
    }

    /// Ground falling away a cell at a time in x, 1.5 m in all: the gap under the slab changes in every column.
    void dippingFloor(S32 ix, S32, std::vector<AirGap>& c)
    {
        AirGap under;
        under.mFloor = static_cast<U16>(12 + (ix % 32) * 3 / 8);
        under.mCeiling = 76;
        under.mFlags = AirGap::TOP;
        c.push_back(under);
    }

    /// A 1.5 m deep channel across y = 24..40 with a pipe along it splitting the air under the slab in two.
    void channel(S32, S32 iy, std::vector<AirGap>& c)
    {
        const bool in_channel = iy >= 24 && iy < 40, pipe = iy >= 30 && iy < 34;
        AirGap under;
        under.mFloor = in_channel ? 0 : 12;
        under.mFlags = AirGap::TOP;
        under.mCeiling = 76;
        if (pipe)
        {
            AirGap low = under;
            low.mCeiling = 20;
            c.push_back(low);
            under.mFloor = 28;
            under.mFlags = 0;
        }
        c.push_back(under);
    }

    /// A sealed pocket under the slab at a few cells, the slab solid down to the ground elsewhere.
    void pocket(S32 ix, S32 iy, std::vector<AirGap>& c)
    {
        if (ix >= 2 && ix < 4 && iy >= 2 && iy < 4)
        {
            AirGap under;
            under.mFloor = 40;
            under.mCeiling = 44;
            c.push_back(under);
        }
    }
}

namespace
{

    TEST_CASE("skairdistil: every column's gaps are its leaves' gaps; each within climb of the fine one", "[skairdistil]")
    {
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        const std::vector<S32> owner = leafOfGap(air);
        size_t coarse = 0;
        for (S32 iy = 0; iy < g.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < g.mWidth; ++ix)
            {
                const std::string at = llformat(" at (%d, %d)", ix, iy);
                AirColumn column;
                airColumn(air, findLeafByKey(air, ix, iy), column);
                { INFO("gap count" + at); CHECK(column.mCount == g.count(ix, iy)); }
                for (U32 k = 0; k < column.mCount; ++k)
                {
                    const AirGap& fine = g.gaps(ix, iy)[k];
                    const AirGap& rec = air.mGaps[column.mGap[k]];
                    const AirLeaf& l = air.mLeaves[owner[column.mGap[k]]];
                    coarse += l.mLevel > 0;
                    { INFO("the leaf covers the column" + at); CHECK(covers(l, ix, iy)); }
                    { INFO("flags" + at); CHECK(rec.mFlags == fine.mFlags); }
                    { INFO("floor within climb" + at); CHECK(std::abs(rec.mFloor - fine.mFloor) <= CLIMB); }
                    { INFO("a walkable floor within half of one" + at); CHECK((!(fine.mFlags & AirGap::TOP) || rec.mFloor - fine.mFloor <= CLIMB / 2)); }
                    { INFO("air inside the column's" + at); CHECK((rec.mFloor >= fine.mFloor && rec.mClear <= fine.mClear)); }
                    if (fine.mFlags & AirGap::OPEN)
                    {
                        { INFO("open ceiling" + at); CHECK(rec.mCeiling == AIR_CEILING_OPEN); }
                    }
                    else
                    {
                        { INFO("ceiling within climb" + at); CHECK((rec.mCeiling <= fine.mCeiling && fine.mCeiling - rec.mCeiling <= CLIMB)); }
                    }
                    const bool usable = (fine.mFlags & (AirGap::TOP | AirGap::OPEN)) || fine.mCeiling - fine.mFloor >= 8;
                    { INFO("no merged usable gap touches a wall" + at); CHECK((l.mLevel == 0 || fine.mClear > 0 || !usable)); }
                }
            }
        }
        { INFO("open floor merges"); CHECK(coarse > g.mGaps.size() / 2); }
        { INFO("far fewer leaves than columns"); CHECK(air.mLeaves.size() * 4 < static_cast<size_t>(g.mWidth * g.mHeight)); }
        { INFO("open floor away from the wall is a 1 m leaf"); CHECK(levelAt(air, 4, 40, 0) >= 3); }
        { INFO("beside the wall stays fine"); CHECK(levelAt(air, 19, 10, 0) == 0); }
    }

    TEST_CASE("skairdistil: a leaf's gap and an overlapping gap beside it differ by at most one level", "[skairdistil]")
    {
        AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        const std::vector<S32> owner = leafOfGap(air);
        S32 worst = 0;
        for (size_t li = 0; li < air.mLeaves.size(); ++li)
        {
            const AirLeaf& l = air.mLeaves[li];
            const S32 s = 1 << l.mLevel;
            for (S32 k = 0; k < 4 * s; ++k)
            {
                const S32 side = k / s, t = k % s;
                const S32 x = side == 0 ? l.mX - 1 : side == 1 ? l.mX + s : l.mX + t;
                const S32 y = side == 2 ? l.mY - 1 : side == 3 ? l.mY + s : l.mY + t;
                if (x < 0 || y < 0 || x >= air.mWidth || y >= air.mHeight)
                {
                    continue;
                }
                AirColumn column;
                airColumn(air, findLeafByKey(air, x, y), column);
                for (U32 i = 0; i < l.mGapCount; ++i)
                {
                    const AirGap& a = air.mGaps[l.mFirstGap + i];
                    for (U32 j = 0; j < column.mCount; ++j)
                    {
                        const AirGap& b = air.mGaps[column.mGap[j]];
                        const AirLeaf& o = air.mLeaves[owner[column.mGap[j]]];
                        const S32 ca = (a.mFlags & AirGap::OPEN) ? 0xffff : a.mCeiling, cb = (b.mFlags & AirGap::OPEN) ? 0xffff : b.mCeiling;
                        if (!covers(o, l.mX, l.mY) && std::min(ca, cb) - std::max(a.mFloor, b.mFloor) >= 1)
                        {
                            worst = std::max(worst, l.mLevel - o.mLevel);
                        }
                    }
                }
            }
        }
        { INFO("restricted"); CHECK(worst <= 1); }
    }

    TEST_CASE("skairdistil: a block whose own sky or flags change inside it does not merge", "[skairdistil]")
    {
        AirGrid g = room();
        TileAir before;
        distilAir(g, CLIMB, before);
        std::vector<S32> leaf_of;
        { INFO("a 1 m leaf before"); CHECK(levelAt(before, 4, 40, 0) >= 3); }
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
        { INFO("no leaf straddles the change"); CHECK(levelAt(air, 4, 40, 0) <= 2); }
        { INFO("inside it the flags hold"); CHECK((air.mGaps[air.mLeaves[leaf_of[1 + 40 * g.mWidth]].mFirstGap].mFlags & AirGap::OWN_SKY) == 0); }
    }

    TEST_CASE("skairdistil: seam strips are the tile's edge columns; cell for cell", "[skairdistil]")
    {
        const AirGrid g = room();
        TileAir air;
        airSeams(g, air);
        for (S32 side = 0; side < 4; ++side)
        {
            const SeamStrip& s = air.mSeams[side];
            const bool along_y = side < 2;
            const S32 n = along_y ? g.mHeight : g.mWidth;
            { INFO("cells along the side"); CHECK(static_cast<S32>(s.mStart.size()) == n + 1); }
            for (S32 i = 0; i < n; ++i)
            {
                const S32 ix = along_y ? (side == 0 ? 0 : g.mWidth - 1) : i;
                const S32 iy = along_y ? i : (side == 2 ? 0 : g.mHeight - 1);
                { INFO(llformat("gaps on side %d cell %d", side, i)); CHECK(s.count(i) == g.count(ix, iy)); }
                for (U32 k = 0; k < s.count(i); ++k)
                {
                    const AirGap& a = s.mGaps[s.mStart[i] + k];
                    const AirGap& b = g.gaps(ix, iy)[k];
                    { INFO("floor"); CHECK(a.mFloor == b.mFloor); }
                    { INFO("ceiling"); CHECK(a.mCeiling == ((b.mFlags & AirGap::OPEN) ? AIR_CEILING_OPEN : b.mCeiling)); }
                    { INFO("flags and clearance"); CHECK((a.mFlags == b.mFlags && a.mClear == b.mClear)); }
                }
            }
        }
    }

    TEST_CASE("skairdistil: solid tops merge per metre and the sky map forgets a roof that is gone", "[skairdistil]")
    {
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
        const TileTops& tops = *air.mTops;
        { INFO("metre cells across"); CHECK(tops.mTopWidth == 4); }
        { INFO("roofed cell: ground and roof"); CHECK(tops.mTopStart[1] - tops.mTopStart[0] == 2u); }
        { INFO("roof merged down to its lowest bottom"); CHECK(tops.mTop[tops.mTopStart[0] + 1].mBottom == 20); }
        { INFO("open cell: ground only"); CHECK(tops.mTopStart[2] - tops.mTopStart[1] == 1u); }

        SkyMap sky;
        const LLVector3d tile(1000.0, 2000.0, 30.0);
        sky.set(7, tile, air);
        const LLVector3d under(1000.5, 2000.5, 31.5), beside(1001.5, 2000.5, 31.5), over(1000.5, 2000.5, 36.0);
        { INFO("under the roof is covered"); CHECK(!sky.skyOpenAt(under)); }
        { INFO("beside it is open"); CHECK(sky.skyOpenAt(beside)); }
        { INFO("outside every tile is not open"); CHECK(!sky.skyOpenAt(LLVector3d(1010.5, 2000.5, 31.5))); }
        { INFO("above the roof is open"); CHECK(sky.skyOpenAt(over)); }
        { INFO("inside the roof is not"); CHECK(!sky.skyOpenAt(LLVector3d(1000.5, 2000.5, 35.2))); }
        { INFO("beyond reach is open"); CHECK(sky.skyOpenAt(under - LLVector3d(0, 0, 60.0), 50.f)); }
        const U64 v = sky.version();

        // A second tile's roof over the same cell, then the first republished roofless.
        sky.set(8, tile + LLVector3d(0, 0, 10.0), tops);
        TileTops bare = tops;
        bare.mTop.erase(bare.mTop.begin() + 1);
        for (size_t i = 1; i < bare.mTopStart.size(); ++i)
        {
            bare.mTopStart[i] -= 1;
        }
        sky.set(7, tile, bare);
        { INFO("the other tile's roof still covers"); CHECK(!sky.skyOpenAt(under)); }
        sky.remove(8);
        { INFO("a deleted roof is forgotten"); CHECK(sky.skyOpenAt(under)); }
        { INFO("changes are versioned"); CHECK(sky.version() > v); }
        sky.remove(7);
        { INFO("an unpublished tile is not open"); CHECK(!sky.skyOpenAt(beside)); }
        { INFO("nothing left"); CHECK(sky.cells() == static_cast<size_t>(0)); }
    }

    TEST_CASE("skairdistil: the packed form is the records back to back; little-endian", "[skairdistil]")
    {
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        airSeams(g, air);
        {
            auto tops = std::make_shared<TileTops>();
            tops->mTopStart.assign(1, 0);
            air.mTops = tops;
        }
        const std::vector<U8> blob = packTileAir(air);
        size_t seams = 0;
        for (const SeamStrip& s : air.mSeams)
        {
            seams += s.mStart.size() * 4 + s.mGaps.size() * 8;
        }
        { INFO("size"); CHECK(blob.size() == air.mLeaves.size() * 12 + air.mGaps.size() * 8 + seams + 4); }
        const AirLeaf& l = air.mLeaves[1];
        { INFO("second leaf's x"); CHECK(static_cast<S32>(blob[12] | (blob[13] << 8)) == static_cast<S32>(l.mX)); }
        { INFO("second leaf's parent"); CHECK(static_cast<S32>(blob[18] | (blob[19] << 8)) == static_cast<S32>(l.mParent)); }
        { INFO("second leaf's first gap"); CHECK(static_cast<U32>(blob[20] | (blob[21] << 8) | (blob[22] << 16) | (blob[23] << 24)) == l.mFirstGap); }
        { INFO("layout names the sections"); CHECK(tileAirLayout(air).find("\"seam_pos_y\"") != std::string::npos); }
    }

    TEST_CASE("skairdistil: a low closed gap walled in does not keep its block fine", "[skairdistil]")
    {
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
        { INFO("an open gap at a wall stays fine"); CHECK(air.mLeaves[leaf_of[0 + 40 * g.mWidth]].mLevel == 0); }
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
        { INFO("a crawlspace at a wall merges"); CHECK(air.mLeaves[leaf_of[0 + 40 * g.mWidth]].mLevel > 0); }
    }

    TEST_CASE("skairdistil: a column of more than 255 gaps is capped alike in its leaf and its seam", "[skairdistil]")
    {
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
        AirColumn column;
        airColumn(air, findLeafByKey(air, 0, 0), column);
        { INFO("leaf capped"); CHECK(column.mCount == 255u); }
        { INFO("seam capped"); CHECK(air.mSeams[TileAir::SIDE_NEG_X].count(0) == 255u); }
        { INFO("seam along y capped"); CHECK(air.mSeams[TileAir::SIDE_NEG_Y].count(0) == 255u); }
    }

    TEST_CASE("skairdistil: a merged leaf never has its floor at or over its ceiling", "[skairdistil]")
    {
        // Four thin closed gaps whose floors and ceilings each agree within climb,
        // but one's floor is over another's ceiling.
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = g.mHeight = 2;
        const U16 floors[4] = { 110, 110, 110, 115 };
        const U16 ceils[4] = { 114, 114, 114, 118 };
        g.mStart = { 0, 1, 2, 3, 4 };
        for (S32 i = 0; i < 4; ++i)
        {
            AirGap gap;
            gap.mFloor = floors[i];
            gap.mCeiling = ceils[i];
            gap.mClear = 1;
            g.mGaps.push_back(gap);
        }
        TileAir air;
        distilAir(g, CLIMB, air);
        for (const AirGap& gap : air.mGaps)
        {
            { INFO("ceiling over floor"); CHECK(((gap.mFlags & AirGap::OPEN) || gap.mCeiling > gap.mFloor)); }
        }
        { INFO("left fine"); CHECK(air.mGaps.size() == static_cast<size_t>(4)); }
    }

    TEST_CASE("skairdistil: leaves come in key order; coarsest first; linked to their parents and a cell's leaf by key is its finest", "[skairdistil]")
    {
        S32 nested = 0, solid = 0;
        for (const AirGrid& g : { room(64, 64), room(50, 37), slab(dippingFloor), slab(channel), slab(pocket) })
        {
            TileAir air;
            distilAir(g, CLIMB, air);
            for (size_t i = 0; i < air.mLeaves.size(); ++i)
            {
                const AirLeaf& l = air.mLeaves[i];
                if (i > 0)
                {
                    const AirLeaf& p = air.mLeaves[i - 1];
                    const U32 kp = airKey(p.mX, p.mY), kl = airKey(l.mX, l.mY);
                    { INFO("leaves in key order, coarsest first"); CHECK((kp < kl || (kp == kl && p.mLevel > l.mLevel))); }
                }
                // The parent is the last leaf before it that covers it.
                S32 want = -1;
                for (S32 j = static_cast<S32>(i) - 1; j >= 0 && want < 0; --j)
                {
                    want = air.mLeaves[j].mLevel > l.mLevel && covers(air.mLeaves[j], l.mX, l.mY) ? j : -1;
                }
                { INFO(llformat("parent of leaf %d", static_cast<S32>(i))); CHECK(parentLeaf(air, static_cast<S32>(i)) == want); }
            }
            std::vector<S32> leaf_of;
            cover(air, leaf_of);
            for (S32 iy = -1; iy <= g.mHeight; ++iy)
            {
                for (S32 ix = -1; ix <= g.mWidth; ++ix)
                {
                    const bool inside = ix >= 0 && iy >= 0 && ix < g.mWidth && iy < g.mHeight;
                    const S32 want = inside ? leaf_of[ix + iy * g.mWidth] : -1;
                    solid += inside && want < 0;
                    nested += want >= 0 && parentLeaf(air, want) >= 0;
                    { INFO(llformat("leaf at (%d, %d)", ix, iy)); CHECK(findLeafByKey(air, ix, iy) == want); }
                }
            }
        }
        { INFO("the fixtures have solid cells between leaves"); CHECK(solid > 0); }
        { INFO("and leaves inside leaves"); CHECK(nested > 0); }
    }

    TEST_CASE("skairdistil: face offsets pack last; after the tops and the layout names them", "[skairdistil]")
    {
        const AirGrid g = room();
        TileAir air;
        distilAir(g, CLIMB, air);
        airSeams(g, air);
        {
            auto tops = std::make_shared<TileTops>();
            tops->mTopStart.assign(1, 0);
            air.mTops = tops;
        }
        const std::vector<U8> before = packTileAir(air);
        air.mFaceOffsets = { FaceOffset(faceKey(3, 5, air.mWidth, TileAir::SIDE_POS_X), 7),
                             FaceOffset(faceKey(9, 2, air.mWidth, TileAir::SIDE_NEG_Y), 15) };
        const std::vector<U8> blob = packTileAir(air);
        { INFO("four bytes an entry"); CHECK(blob.size() == before.size() + 8); }
        { INFO("the sections before are untouched"); CHECK(std::equal(before.begin(), before.end(), blob.begin())); }
        for (size_t i = 0; i < 2; ++i)
        {
            const U8* p = &blob[before.size() + i * 4];
            const U32 bits = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<U32>(p[3]) << 24);
            { INFO("key"); CHECK(bits >> 5 == air.mFaceOffsets[i].key()); }
            { INFO("offset"); CHECK((bits & 31u) == air.mFaceOffsets[i].offset()); }
        }
        { INFO("column * 4 + side"); CHECK(air.mFaceOffsets[0].key() == static_cast<U32>(4 * (3 + 5 * air.mWidth) + 1)); }
        const std::string layout = tileAirLayout(air);
        { INFO("the record format"); CHECK((layout.find("\"face\":\"<I\"") != std::string::npos)); }
        { INFO("the bit fields"); CHECK(layout.find("\"face_bits\":{\"key_shift\":5,\"offset_mask\":31,\"steps\":16,") != std::string::npos); }
        { INFO("the last section"); CHECK(layout.find("[\"face_offsets\",\"face\",2]]}") != std::string::npos); }
    }

    TEST_CASE("skairdistil: a flat slab over a dipping floor keeps its own gap coarse; the floor under it fine", "[skairdistil]")
    {
        const AirGrid g = slab(dippingFloor);
        TileAir air;
        distilAir(g, CLIMB, air);
        for (S32 iy = 4; iy < 60; iy += 9)
        {
            for (S32 ix = 4; ix < 60; ix += 9)
            {
                { INFO(llformat("the slab's top is 2 m leaves at (%d, %d)", ix, iy)); CHECK(levelAt(air, ix, iy, 80) == AIR_MAX_LEVEL); }
            }
        }
        { INFO("the floor under it is finer"); CHECK(levelAt(air, 20, 20, 12 + 20 * 3 / 8) < AIR_MAX_LEVEL); }
    }

    TEST_CASE("skairdistil: a slab over a channel keeps its top coarse where the gaps under it change in number", "[skairdistil]")
    {
        const AirGrid g = slab(channel);
        TileAir air;
        distilAir(g, CLIMB, air);
        { INFO("over the bank"); CHECK(levelAt(air, 8, 8, 80) == AIR_MAX_LEVEL); }
        { INFO("over the channel's edge"); CHECK(levelAt(air, 8, 24, 80) == AIR_MAX_LEVEL); }
        { INFO("over the pipe"); CHECK(levelAt(air, 40, 31, 80) == AIR_MAX_LEVEL); }
        { INFO("under it the bank's edge is fine"); CHECK(levelAt(air, 8, 23, 12) < AIR_MAX_LEVEL); }
    }

    TEST_CASE("skairdistil: a wall through the slab still splits its top and its leaves step down to it", "[skairdistil]")
    {
        AirGrid g = slab([](S32, S32, std::vector<AirGap>& c) {
            AirGap under;
            under.mFloor = 12;
            under.mCeiling = 76;
            under.mFlags = AirGap::TOP;
            c.push_back(under);
        });
        // A solid column from the ground past the slab's top at x = 30..31: no air in it at all.
        AirGrid walled = g;
        walled.mGaps.clear();
        for (S32 iy = 0; iy < g.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < g.mWidth; ++ix)
            {
                if (ix != 30 && ix != 31)
                {
                    walled.mGaps.insert(walled.mGaps.end(), g.gaps(ix, iy), g.gaps(ix, iy) + g.count(ix, iy));
                }
                walled.mStart[ix + iy * g.mWidth + 1] = static_cast<U32>(walled.mGaps.size());
            }
        }
        computeClearance(walled, CLIMB, OVERLAP);
        TileAir air;
        distilAir(walled, CLIMB, air);
        { INFO("beside the wall the top is fine"); CHECK(levelAt(air, 29, 40, 80) == 0); }
        { INFO("and the floor under the slab"); CHECK(levelAt(air, 32, 40, 12) == 0); }
        { INFO("a step away it is coarser"); CHECK(levelAt(air, 27, 40, 80) >= 1); }
        { INFO("far from it 2 m leaves"); CHECK(levelAt(air, 56, 40, 80) == AIR_MAX_LEVEL); }
    }

    TEST_CASE("skairdistil: a walkable floor in steps merges only where a leaf's floors are within half a climb", "[skairdistil]")
    {
        // Treads of 0.25 m rising 4 units each, open overhead: each tread is within a climb of the next, two are not within half of one.
        AirGrid g;
        g.mCell = g.mUnit = 0.125f;
        g.mWidth = g.mHeight = 64;
        g.mStart.assign(64 * 64 + 1, 0);
        for (S32 iy = 0; iy < 64; ++iy)
        {
            for (S32 ix = 0; ix < 64; ++ix)
            {
                AirGap gap;
                gap.mFloor = static_cast<U16>(8 + 4 * (ix / 2));
                gap.mCeiling = 400;
                gap.mFlags = AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY;
                g.mGaps.push_back(gap);
                g.mStart[ix + iy * 64 + 1] = static_cast<U32>(g.mGaps.size());
            }
        }
        computeClearance(g, CLIMB, OVERLAP);
        TileAir air;
        distilAir(g, CLIMB, air);
        S32 worst = 0;
        for (S32 iy = 0; iy < 64; ++iy)
        {
            for (S32 ix = 0; ix < 64; ++ix)
            {
                AirColumn column;
                airColumn(air, findLeafByKey(air, ix, iy), column);
                { INFO("one gap"); CHECK(column.mCount == 1u); }
                worst = std::max(worst, air.mGaps[column.mGap[0]].mFloor - g.gaps(ix, iy)[0].mFloor);
            }
        }
        { INFO("no leaf spans two treads"); CHECK(worst <= CLIMB / 2); }
        { INFO("a tread's own cells merge"); CHECK(levelAt(air, 10, 10, 8 + 20) >= 1); }
    }

    // SKNAV_DISTIL_GRIDS names a folder of fine grids ("SKG1", width, height, counts as U16, gaps as
    // floor, ceiling, flags, clear) and SKNAV_DISTIL_OUT a folder: each grid's distilled leaves and gaps
    // go to a .leaves file of the same name, for an oracle to diff.
    TEST_CASE("skairdistil: distil fine grids from files; when a folder is named", "[skairdistil]")
    {
        const std::string in = LLStringUtil::getenv("SKNAV_DISTIL_GRIDS"), out = LLStringUtil::getenv("SKNAV_DISTIL_OUT");
        if (in.empty() || out.empty())
        {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(in))
        {
            if (entry.path().extension() != ".gaps")
            {
                continue;
            }
            std::ifstream f(entry.path(), std::ios::binary);
            std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            { INFO("a grid file"); CHECK((b.size() >= 12 && std::equal(b.begin(), b.begin() + 4, "SKG1"))); }
            AirGrid g;
            g.mCell = g.mUnit = 0.125f;
            memcpy(&g.mWidth, &b[4], 4);
            memcpy(&g.mHeight, &b[8], 4);
            const size_t cells = static_cast<size_t>(g.mWidth) * g.mHeight;
            g.mStart.assign(cells + 1, 0);
            size_t at = 12 + 2 * cells;
            for (size_t c = 0; c < cells; ++c)
            {
                U16 n;
                memcpy(&n, &b[12 + 2 * c], 2);
                g.mStart[c + 1] = g.mStart[c] + n;
                for (U16 k = 0; k < n; ++k, at += 6)
                {
                    AirGap gap;
                    memcpy(&gap.mFloor, &b[at], 2);
                    memcpy(&gap.mCeiling, &b[at + 2], 2);
                    gap.mFlags = static_cast<U8>(b[at + 4]);
                    gap.mClear = static_cast<U8>(b[at + 5]);
                    g.mGaps.push_back(gap);
                }
            }
            TileAir air;
            distilAir(g, CLIMB, air);
            const std::vector<U8> blob = packTileAir(air);
            std::ofstream o(out + "/" + entry.path().stem().string() + ".leaves", std::ios::binary);
            const U32 counts[2] = { static_cast<U32>(air.mLeaves.size()), static_cast<U32>(air.mGaps.size()) };
            o.write(reinterpret_cast<const char*>(counts), sizeof(counts));
            o.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(air.mLeaves.size() * 12 + air.mGaps.size() * 8));
        }
    }

    TEST_CASE("skairdistil: a uniform slope merges to the cap; its floors on the plane within a unit over every fine floor", "[skairdistil]")
    {
        // 15 % along x and 10 % along y: a 2 m leaf spans 4 units, more than a flat leaf may.
        const AirGrid g = field([](S32 ix, S32 iy) { return static_cast<S32>(std::ceil(0.15 * (ix + 0.5) + 0.1 * (iy + 0.5))); });
        TileAir air;
        distilAir(g, CLIMB, air);
        S32 worst = 0;
        for (S32 iy = 0; iy < 64; ++iy)
        {
            for (S32 ix = 0; ix < 64; ++ix)
            {
                S32 floor = 0;
                { INFO(llformat("a 2 m leaf at (%d, %d)", ix, iy)); CHECK(leafAt(air, ix, iy, &floor) == AIR_MAX_LEVEL); }
                const S32 fine = g.gaps(ix, iy)[0].mFloor;
                { INFO(llformat("the plane is not under the floor at (%d, %d)", ix, iy)); CHECK(floor >= fine); }
                worst = std::max(worst, floor - fine);
            }
        }
        { INFO("within a unit everywhere"); CHECK((worst <= AIR_PLANE_TOL == true)); }
        { INFO("one leaf per 2 m"); CHECK(air.mLeaves.size() == static_cast<size_t>(16)); }
    }

    TEST_CASE("skairdistil: a crease keeps fine leaves along its line only", "[skairdistil]")
    {
        // Level up to x = 21, then up at 40 %: the line runs through a 2 m, a 1 m and a 0.5 m block.
        const S32 crease = 21;
        const AirGrid g = field([crease](S32 ix, S32) { return ix < crease ? 0 : static_cast<S32>(std::ceil(0.4 * (ix - crease + 0.5))); });
        TileAir air;
        distilAir(g, CLIMB, air);
        for (S32 iy = 0; iy < 64; ++iy)
        {
            for (S32 ix = 0; ix < 64; ++ix)
            {
                const S32 level = leafAt(air, ix, iy);
                if (ix < 8 || ix >= 40)
                {
                    { INFO(llformat("2 m leaves away from it at (%d, %d)", ix, iy)); CHECK(level == AIR_MAX_LEVEL); }
                }
                if (ix == crease - 1 || ix == crease)
                {
                    { INFO(llformat("finer along it at (%d, %d)", ix, iy)); CHECK(level < AIR_MAX_LEVEL); }
                }
                S32 floor = 0;
                leafAt(air, ix, iy, &floor);
                { INFO(llformat("the floor holds at (%d, %d)", ix, iy)); CHECK((floor >= g.gaps(ix, iy)[0].mFloor && floor - g.gaps(ix, iy)[0].mFloor <= AIR_PLANE_TOL)); }
            }
        }
    }

    TEST_CASE("skairdistil: a big flat open area reaches the cap everywhere", "[skairdistil]")
    {
        const AirGrid g = field([](S32, S32) { return 12; });
        TileAir air;
        distilAir(g, CLIMB, air);
        { INFO("one leaf per 2 m"); CHECK(air.mLeaves.size() == static_cast<size_t>(16)); }
        for (const AirLeaf& l : air.mLeaves)
        {
            { INFO("at the cap"); CHECK(static_cast<S32>(l.mLevel) == AIR_MAX_LEVEL); }
            { INFO("flat"); CHECK((!air.mGaps[l.mFirstGap].mSlopeX && !air.mGaps[l.mFirstGap].mSlopeY && air.mGaps[l.mFirstGap].mFloor == 12)); }
        }
    }

    TEST_CASE("skairdistil: leaves step down to a wall a level at a time; on a slope as on the flat", "[skairdistil]")
    {
        for (S32 slope = 0; slope < 2; ++slope)
        {
            // A wall across the middle of an open floor, level or rising 10 % along y.
            const AirGrid g = field([slope](S32, S32 iy) { return slope ? static_cast<S32>(std::ceil(0.1 * (iy + 0.5))) : 0; },
                                    [](S32 ix, S32 iy) { return ix == 37 && iy >= 8 && iy < 56; });
            TileAir air;
            distilAir(g, CLIMB, air);
            for (S32 iy = 0; iy < 64; ++iy)
            {
                for (S32 ix = 0; ix < 64; ++ix)
                {
                    const S32 level = leafAt(air, ix, iy);
                    if (level < 0)
                    {
                        continue;
                    }
                    if ((ix == 36 || ix == 38) && iy >= 8 && iy < 56)
                    {
                        { INFO(llformat("fine against the wall at (%d, %d)", ix, iy)); CHECK(level == 0); }
                    }
                    if (ix < 16)
                    {
                        { INFO(llformat("2 m leaves away from it at (%d, %d)", ix, iy)); CHECK(level == AIR_MAX_LEVEL); }
                    }
                    if (ix >= 48)
                    {
                        { INFO(llformat("1 m leaves or coarser past the step-down at (%d, %d)", ix, iy)); CHECK(level >= AIR_MAX_LEVEL - 1); }
                    }
                    for (S32 d = 0; d < 2; ++d)
                    {
                        const S32 other = leafAt(air, ix + (d == 0), iy + (d == 1));
                        { INFO(llformat("a level at a time at (%d, %d)", ix, iy)); CHECK((other < 0 || std::abs(other - level) <= 1)); }
                    }
                }
            }
        }
    }

    TEST_CASE("skairdistil: a seam strip along a tilted 2 m leaf is its fine floors; the leaf's plane within a unit over them", "[skairdistil]")
    {
        const AirGrid g = field([](S32 ix, S32 iy) { return static_cast<S32>(std::ceil(0.2 * (ix + 0.5) + 0.05 * (iy + 0.5))); });
        TileAir air;
        distilAir(g, CLIMB, air);
        airSeams(g, air);
        for (S32 side = 0; side < 4; ++side)
        {
            const SeamStrip& strip = air.mSeams[side];
            { INFO("a cell per strip entry"); CHECK(strip.mStart.size() == static_cast<size_t>(65)); }
            for (S32 i = 0; i < 64; ++i)
            {
                const S32 ix = side == TileAir::SIDE_NEG_X ? 0 : side == TileAir::SIDE_POS_X ? 63 : i;
                const S32 iy = side == TileAir::SIDE_NEG_Y ? 0 : side == TileAir::SIDE_POS_Y ? 63 : i;
                { INFO("one gap per seam cell"); CHECK(strip.count(i) == 1u); }
                const AirGap& fine = strip.mGaps[strip.mStart[i]];
                { INFO("the strip is the fine floor"); CHECK(static_cast<S32>(fine.mFloor) == static_cast<S32>(g.gaps(ix, iy)[0].mFloor)); }
                { INFO("and flat"); CHECK((!fine.mSlopeX && !fine.mSlopeY)); }
                S32 floor = 0;
                { INFO(llformat("a 2 m leaf at the seam (%d, %d)", ix, iy)); CHECK(leafAt(air, ix, iy, &floor) == AIR_MAX_LEVEL); }
                { INFO(llformat("its plane meets the strip at (%d, %d)", ix, iy)); CHECK((floor >= fine.mFloor && floor - fine.mFloor <= AIR_PLANE_TOL)); }
            }
        }
    }

    TEST_CASE("skairdistil: coarse air upsampled keeps a tilted floor at every fine cell; split leaves included", "[skairdistil]")
    {
        // A 1 m grid rising 10 % along x and 20 % along y, each 2 x 2 m leaf eight times as wide when upsampled and split.
        AirGrid coarse = field([](S32 ix, S32 iy) { return static_cast<S32>(std::ceil(0.1 * (ix + 0.5) + 0.2 * (iy + 0.5))); }, nullptr, 16);
        coarse.mCell = coarse.mUnit = 1.f;
        TileAir air;
        distilAir(coarse, CLIMB, air);
        airSeams(coarse, air);
        const TileAirPtr fine = upsampleAir(air, 8);
        bool tilted = false, split = false;
        for (S32 iy = 0; iy < 16; ++iy)
        {
            for (S32 ix = 0; ix < 16; ++ix)
            {
                S32 at = 0;
                const S32 level = leafAt(air, ix, iy, &at);
                tilted = tilted || air.mGaps[air.mLeaves[findLeafByKey(air, ix, iy)].mFirstGap].mSlopeX != 0;
                split = split || level + 3 > AIR_MAX_LEVEL;
                // The middle of the coarse cell in fine cells: the floor there is eight times the coarse one.
                S32 up = 0;
                leafAt(*fine, ix * 8 + 4, iy * 8 + 4, &up);
                { INFO(llformat("the fine floor at (%d, %d) is the coarse one: %d vs %d", ix, iy, up, at * 8)); CHECK(std::abs(up - at * 8) <= 8); }
            }
        }
        { INFO("the coarse leaves tilt"); CHECK(tilted); }
        { INFO("and split when upsampled"); CHECK(split); }
    }

    TEST_CASE("skairdistil: a column under a ramp runs the crawl gap below the ramp; though the ramp's centre floor is lower", "[skairdistil]")
    {
        // A 4 x 4 leaf of ramp rising to its far corner, over a fine leaf at that corner holding a crawl gap under it.
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 4;
        AirGap ramp;
        ramp.mFloor = 20;
        ramp.mCeiling = AIR_CEILING_OPEN;
        ramp.mFlags = AirGap::TOP | AirGap::OPEN;
        ramp.mSlopeX = ramp.mSlopeY = 127;
        AirGap crawl;
        crawl.mFloor = 21;
        crawl.mCeiling = 24;
        crawl.mFlags = AirGap::TOP;
        air.mGaps = { ramp, crawl };
        AirLeaf coarse;
        coarse.mLevel = 2;
        coarse.mGapCount = 1;
        coarse.mFirstGap = 0;
        AirLeaf fine;
        fine.mX = fine.mY = 3;
        fine.mGapCount = 1;
        fine.mFirstGap = 1;
        air.mLeaves = { coarse, fine };
        linkLeaves(air);
        AirColumn column;
        airColumn(air, findLeafByKey(air, 3, 3), column);
        { INFO("two gaps"); CHECK(column.mCount == 2U); }
        { INFO("the ramp is over the crawl gap at the corner"); CHECK(column.floorAt(air, 1, 3, 3) > air.mGaps[column.mGap[0]].mCeiling); }
        { INFO("the crawl gap first"); CHECK(air.mGaps[column.mGap[0]].mCeiling == 24); }
    }

    TEST_CASE("skairdistil: coarse air upsampled only by a power of two; any other factor is refused", "[skairdistil]")
    {
        AirGrid coarse = field([](S32, S32) { return 2; }, nullptr, 16);
        coarse.mCell = coarse.mUnit = 1.f;
        TileAir air;
        distilAir(coarse, CLIMB, air);
        airSeams(coarse, air);
        { INFO("a factor of 4 upsamples"); CHECK(upsampleAir(air, 4) != nullptr); }
        for (S32 f : { 3, 5, 6, 10 })
        {
            const TileAirPtr fine = upsampleAir(air, f);
            if (!fine)
            {
                continue;
            }
            // Were it upsampled, every fine cell would have to lie under exactly one leaf of the coarse floor.
            std::vector<S32> hits(static_cast<size_t>(fine->mWidth) * fine->mHeight, 0);
            for (const AirLeaf& l : fine->mLeaves)
            {
                const S32 s = 1 << l.mLevel;
                for (S32 y = l.mY; y < l.mY + s; ++y)
                {
                    for (S32 x = l.mX; x < l.mX + s; ++x)
                    {
                        { INFO(llformat("factor %d: leaf (%d, %d) inside the tile", f, l.mX, l.mY)); CHECK((x < fine->mWidth && y < fine->mHeight)); }
                        ++hits[x + y * fine->mWidth];
                    }
                }
            }
            for (S32 h : hits)
            {
                { INFO(llformat("factor %d: each cell under one leaf", f).c_str()); CHECK(h == 1); }
            }
        }
    }

    TEST_CASE("skairdistil: air built again to the same cells is the same air; whatever its serial; a moved gap is not", "[skairdistil]")
    {
        AirGrid grid = field([](S32 x, S32) { return x < 8 ? 2 : 6; }, nullptr, 16);
        TileAir air;
        distilAir(grid, CLIMB, air);
        airSeams(grid, air);
        TileAir again;
        distilAir(grid, CLIMB, again);
        airSeams(grid, again);
        { INFO("serials differ"); CHECK(air.mSerial.mValue != again.mSerial.mValue); }
        { INFO("the same air"); CHECK(sameAir(air, again)); }
        TileAir copy = air;
        { INFO("a copy is the same air"); CHECK(sameAir(air, copy)); }
        copy.mGaps[0].mCeiling += 1;
        { INFO("a moved ceiling is other air"); CHECK(!sameAir(air, copy)); }
        copy = air;
        copy.mFaceOffsets.emplace_back(7u, 3u);
        { INFO("a face offset more is other air"); CHECK(!sameAir(air, copy)); }
        AirGrid moved = field([](S32 x, S32) { return x < 9 ? 2 : 6; }, nullptr, 16);
        TileAir other;
        distilAir(moved, CLIMB, other);
        airSeams(moved, other);
        { INFO("a moved step is other air"); CHECK(!sameAir(air, other)); }
    }
}
