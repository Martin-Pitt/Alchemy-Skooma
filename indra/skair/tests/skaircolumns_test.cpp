/**
 * @file skaircolumns_test.cpp
 * @brief The air's own column raster: merges, bodies, cuts, bridges and the cull, and its outcome against Recast's.
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

#include "../skaircolumns.h"
#include "../skairsoup.h"
#include "skairfixture.h"
#include "skairtestscene.h"

#include "llformat.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{
    using namespace skworld;
    using namespace airscene;
    using namespace airfixture;

    /// A frame of whole metres: a 4 m square of columns at 1 m, heights from 0 to 100 m in 1 m units.
    ColumnFrame unitFrame(S32 climb = 1)
    {
        ColumnFrame f;
        f.mOrigin.set(0.f, 0.f, 0.f);
        f.mCell = 1.f;
        f.mUnit = 1.f;
        f.mWidth = 4;
        f.mHeightRange = 100.f;
        f.mClimb = climb;
        return f;
    }

    void addTri(AirSoup& s, const LLVector3& a, const LLVector3& b, const LLVector3& c, U8 code, U8 flags = 0)
    {
        for (const LLVector3* p : { &a, &b, &c })
        {
            s.mVerts.insert(s.mVerts.end(), p->mV, p->mV + 3);
        }
        s.mCodes.push_back(code);
        s.mFlags.push_back(flags);
    }

    /// A horizontal quad over [x0, x1] by [y0, y1] at z.
    void addSlab(AirSoup& s, F32 x0, F32 y0, F32 x1, F32 y1, F32 z, U8 code, U8 flags = 0)
    {
        addTri(s, LLVector3(x0, y0, z), LLVector3(x1, y0, z), LLVector3(x1, y1, z), code, flags);
        addTri(s, LLVector3(x0, y0, z), LLVector3(x1, y1, z), LLVector3(x0, y1, z), code, flags);
    }

    /// A closed axis-aligned box as twelve triangles, its range a body to fill.
    void addBox(AirSoup& s, const LLVector3& lo, const LLVector3& hi, U8 top_code)
    {
        const S32 begin = s.ntris();
        addSlab(s, lo.mV[VX], lo.mV[VY], hi.mV[VX], hi.mV[VY], hi.mV[VZ], top_code);
        addSlab(s, lo.mV[VX], lo.mV[VY], hi.mV[VX], hi.mV[VY], lo.mV[VZ], 0);
        const LLVector3 c[4] = { { lo.mV[VX], lo.mV[VY], 0 }, { hi.mV[VX], lo.mV[VY], 0 }, { hi.mV[VX], hi.mV[VY], 0 }, { lo.mV[VX], hi.mV[VY], 0 } };
        for (S32 k = 0; k < 4; ++k)
        {
            const LLVector3 a = c[k], b = c[(k + 1) % 4];
            addTri(s, LLVector3(a.mV[VX], a.mV[VY], lo.mV[VZ]), LLVector3(b.mV[VX], b.mV[VY], lo.mV[VZ]), LLVector3(b.mV[VX], b.mV[VY], hi.mV[VZ]), 0);
            addTri(s, LLVector3(a.mV[VX], a.mV[VY], lo.mV[VZ]), LLVector3(b.mV[VX], b.mV[VY], hi.mV[VZ]), LLVector3(a.mV[VX], a.mV[VY], hi.mV[VZ]), 0);
        }
        s.mSolids.emplace_back(begin, s.ntris());
    }

    std::vector<std::array<S32, 3>> column(const ColumnRaster& r, S32 ix, S32 iy)
    {
        std::vector<std::array<S32, 3>> out;
        const S32 c = ix + iy * r.mFrame.mWidth;
        for (U32 k = r.mStart[c]; k < r.mStart[c + 1]; ++k)
        {
            out.push_back({ r.mBottom[k], r.mTop[k], r.mCode[k] });
        }
        return out;
    }

    ColumnRaster raster(const ColumnFrame& f, const AirSoup& s, const TerrainWindow* t = nullptr, ColumnRasterStats* stats = nullptr,
                        ColumnRaster* after_cut = nullptr, ColumnRaster* after_bridges = nullptr)
    {
        ColumnSource src;
        src.mSoup = &s;
        src.mTerrain = t;
        src.mAfterCut = after_cut;
        src.mAfterBridges = after_bridges;
        ColumnRaster out;
        rasteriseColumns(f, src, out, stats);
        return out;
    }

    // ---- outcome against Recast's states -------------------------------------

    /// How one state of our raster compares with Recast's, span by span where a column's spans pair up.
    struct StateDiff
    {
        U64 mSpansTheirs = 0, mSpansOurs = 0;
        U64 mColumnsDiffer = 0;     ///< columns whose span heights differ
        U64 mUnpaired = 0;          ///< spans in columns whose counts differ
        U64 mFloorOff = 0;          ///< paired spans whose top differs
        U64 mBottomOff = 0;
        U64 mWalkFlips = 0;         ///< paired spans walkable (code > 0) in one only
        U64 mCodeDiffers = 0;

        void add(const StateDiff& o)
        {
            mSpansTheirs += o.mSpansTheirs;
            mSpansOurs += o.mSpansOurs;
            mColumnsDiffer += o.mColumnsDiffer;
            mUnpaired += o.mUnpaired;
            mFloorOff += o.mFloorOff;
            mBottomOff += o.mBottomOff;
            mWalkFlips += o.mWalkFlips;
            mCodeDiffers += o.mCodeDiffers;
        }
        F64 frac(U64 n) const { return mSpansTheirs ? static_cast<F64>(n) / mSpansTheirs : 0.0; }
        std::string text() const
        {
            return llformat("spans %llu/%llu, columns differ %llu, unpaired %llu, tops off %llu (%.4f%%), bottoms off %llu (%.4f%%), "
                            "walk flips %llu (%.4f%%), codes differ %llu",
                            (unsigned long long)mSpansTheirs, (unsigned long long)mSpansOurs, (unsigned long long)mColumnsDiffer,
                            (unsigned long long)mUnpaired, (unsigned long long)mFloorOff, 100.0 * frac(mFloorOff), (unsigned long long)mBottomOff,
                            100.0 * frac(mBottomOff), (unsigned long long)mWalkFlips, 100.0 * frac(mWalkFlips), (unsigned long long)mCodeDiffers);
        }
    };

    StateDiff compareState(const ColumnRaster& ours, const RecastColumns& theirs)
    {
        StateDiff d;
        const S32 W = ours.mFrame.mWidth;
        for (S32 iy = 0; iy < W; ++iy)
        {
            for (S32 ix = 0; ix < W; ++ix)
            {
                const S32 c = ix + iy * W, rc = theirs.column(ix, iy);
                const U32 a = theirs.mStart[rc], na = theirs.mStart[rc + 1] - a;
                const U32 b = ours.mStart[c], nb = ours.mStart[c + 1] - b;
                d.mSpansTheirs += na;
                d.mSpansOurs += nb;
                bool same = na == nb;
                if (na != nb)
                {
                    d.mUnpaired += std::max(na, nb) - std::min(na, nb);
                }
                for (U32 k = 0; k < std::min(na, nb); ++k)
                {
                    const U32 v = theirs.mSpans[a + k];
                    const bool top = RecastColumns::smax(v) != ours.mTop[b + k], bottom = RecastColumns::smin(v) != ours.mBottom[b + k];
                    same = same && !top && !bottom;
                    d.mFloorOff += top;
                    d.mBottomOff += bottom;
                    d.mWalkFlips += (RecastColumns::area(v) != 0) != (ours.mCode[b + k] != 0);
                    d.mCodeDiffers += RecastColumns::area(v) != ours.mCode[b + k];
                }
                d.mColumnsDiffer += !same;
            }
        }
        return d;
    }

    /// Our raster's states for a captured fixture: the three before the walk rules, then the walked raster.
    struct Ours
    {
        ColumnRaster mCut, mBridged, mCulled, mWindow, mWalked;
        ColumnRasterStats mStats;
    };

    /// The walk parameters the fixture's raster was filtered with: the air's at AVATAR's climb and the steeper slope.
    ColumnWalkInput walkInput(const Fixture& fx)
    {
        const LLSD& a = fx.mParams["avatar"];
        const LLSD& t = fx.mParams["top"];
        const F32 climb = static_cast<F32>(a["climb"].asReal());
        ColumnWalkInput in;
        in.mTop = walkParams(climb, static_cast<F32>(t["height"].asReal()), std::max(static_cast<F32>(t["slope"].asReal()), static_cast<F32>(a["slope"].asReal())),
                             fx.mCell, fx.mUnit);
        in.mAvatar = walkParams(climb, static_cast<F32>(a["height"].asReal()), static_cast<F32>(a["slope"].asReal()), fx.mCell, fx.mUnit);
        in.mAvatarInset = fx.mBorder - fx.mAvatarBorder;
        return in;
    }

    void rasteriseFixture(const Fixture& fx, Ours& out, bool walk)
    {
        AirSoup soup;
        fx.soup(soup);
        TerrainWindow land;
        fx.terrain(land);
        const ColumnFrame frame = fx.frame();
        ColumnSource src;
        src.mSoup = &soup;
        src.mTerrain = &land;
        src.mAfterCut = &out.mCut;
        src.mAfterBridges = &out.mBridged;
        rasteriseColumns(frame, src, out.mCulled, &out.mStats);
        if (!walk)
        {
            return;
        }
        std::vector<U8> in_window;
        S32 rect[4];
        const bool windowed = leafWindow(frame, soup, in_window, rect);
        if (windowed)
        {
            ColumnSource w;
            w.mSoup = &soup;
            w.mTerrain = &land;
            w.mDropLeaves = true;
            w.mRect = rect;
            w.mBridgeColumns = in_window.data();
            rasteriseColumns(frame, w, out.mWindow, &out.mStats);
        }
        out.mWalked = out.mCulled;
        walkColumns(out.mWalked, walkInput(fx), windowed ? &out.mWindow : nullptr, windowed ? in_window.data() : nullptr, &out.mStats);
    }

    /// The walked raster against the fixture's filtered states: TOP before and after its rim rule, and AVATAR_FLOOR.
    struct WalkDiff
    {
        U64 mSpans = 0;             ///< spans paired with the fixture's, in columns of equal counts
        U64 mTopPreFlips = 0;
        U64 mTopFlips = 0;
        U64 mTopFlipsLost = 0;
        U64 mAvatarFlips = 0;
        U64 mAvatarFlipsLost = 0;
        U64 mAreaFlips = 0;         ///< both TOP, their areas differ

        void add(const WalkDiff& o)
        {
            mSpans += o.mSpans;
            mTopPreFlips += o.mTopPreFlips;
            mTopFlips += o.mTopFlips;
            mTopFlipsLost += o.mTopFlipsLost;
            mAvatarFlips += o.mAvatarFlips;
            mAvatarFlipsLost += o.mAvatarFlipsLost;
            mAreaFlips += o.mAreaFlips;
        }
        F64 frac(U64 n) const { return mSpans ? static_cast<F64>(n) / mSpans : 0.0; }
        std::string text() const
        {
            return llformat("spans %llu, TOP pre-rim flips %llu (%.4f%%), TOP flips %llu (%.4f%%, %llu lost), AVATAR_FLOOR flips %llu (%.4f%%, %llu lost), "
                            "area flips %llu (%.4f%%)",
                            (unsigned long long)mSpans, (unsigned long long)mTopPreFlips, 100.0 * frac(mTopPreFlips), (unsigned long long)mTopFlips,
                            100.0 * frac(mTopFlips), (unsigned long long)mTopFlipsLost, (unsigned long long)mAvatarFlips, 100.0 * frac(mAvatarFlips),
                            (unsigned long long)mAvatarFlipsLost, (unsigned long long)mAreaFlips, 100.0 * frac(mAreaFlips));
        }
    };

    WalkDiff compareWalk(const ColumnRaster& ours, const Fixture& fx)
    {
        WalkDiff d;
        const RecastColumns topl = fx.columns("TOPL"), topf = fx.columns("TOPF");
        const std::vector<U32> avfs = fx.array<U32>("AVFS");
        const std::vector<U8> avfl = fx.array<U8>("AVFL");
        const S32 W = ours.mFrame.mWidth;
        for (S32 iy = 0; iy < W; ++iy)
        {
            for (S32 ix = 0; ix < W; ++ix)
            {
                const S32 c = ix + iy * W, rc = topf.column(ix, iy);
                const U32 a = topf.mStart[rc], na = topf.mStart[rc + 1] - a;
                const U32 b = ours.mStart[c], nb = ours.mStart[c + 1] - b;
                if (na != nb)
                {
                    continue;
                }
                for (U32 k = 0; k < na; ++k)
                {
                    const U8 w = ours.mWalk[b + k];
                    const U32 f = topf.mSpans[a + k];
                    const bool their_top = RecastColumns::area(f) != 0, our_top = (w & TOP_WALK) != 0;
                    const bool their_pre = RecastColumns::area(topl.mSpans[topl.mStart[rc] + k]) != 0, our_pre = (w & TOP_PRE_LEDGE) != 0;
                    const bool their_av = (avfl[avfs[rc] + k] & 0x40) != 0, our_av = (w & AVATAR_WALK) != 0;
                    ++d.mSpans;
                    d.mTopPreFlips += their_pre != our_pre;
                    d.mTopFlips += their_top != our_top;
                    d.mTopFlipsLost += their_top && !our_top;
                    d.mAvatarFlips += their_av != our_av;
                    d.mAvatarFlipsLost += their_av && !our_av;
                    d.mAreaFlips += their_top && our_top && areaOfCode(static_cast<U8>(RecastColumns::area(f))) != areaOfCode(ours.mCode[b + k]);
                }
            }
        }
        return d;
    }

    /// A raster's columns as the oracle reads them: tag, width, then per column a U16 count and its spans as
    /// U16 bottom, U16 top, U8 code, U8 walk.
    void dumpColumns(std::ofstream& out, const char tag[5], const ColumnRaster& r)
    {
        std::vector<U8> b;
        const S32 w = r.mFrame.mWidth;
        b.insert(b.end(), reinterpret_cast<const U8*>(&w), reinterpret_cast<const U8*>(&w) + 4);
        for (S32 c = 0; c < r.mFrame.columns(); ++c)
        {
            const U16 n = static_cast<U16>(r.count(c));
            b.insert(b.end(), reinterpret_cast<const U8*>(&n), reinterpret_cast<const U8*>(&n) + 2);
        }
        for (size_t k = 0; k < r.spans(); ++k)
        {
            const U16 v[2] = { r.mBottom[k], r.mTop[k] };
            b.insert(b.end(), reinterpret_cast<const U8*>(v), reinterpret_cast<const U8*>(v) + 4);
            b.push_back(r.mCode[k]);
            b.push_back(r.mWalk[k]);
        }
        const U32 size = static_cast<U32>(b.size());
        out.write(tag, 4);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(b.data()), b.size());
    }
}

TEST_CASE("skaircolumns: a slab rasterises one span a column, its top rounded up a unit", "[skaircolumns]")
{
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.5f, AREA_CODE_BASE);
    const ColumnRaster r = raster(unitFrame(), s);
    for (S32 iy = 0; iy < 4; ++iy)
    {
        for (S32 ix = 0; ix < 4; ++ix)
        {
            CHECK(column(r, ix, iy) == std::vector<std::array<S32, 3>>{ { 10, 11, AREA_CODE_BASE } });
        }
    }
}

TEST_CASE("skaircolumns: a flat floor on a unit boundary spans that unit and the next, as Recast's", "[skaircolumns]")
{
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.f, AREA_CODE_BASE);
    const ColumnRaster r = raster(unitFrame(), s);
    CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 10, 11, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: a height a rounding error off a boundary is on it", "[skaircolumns]")
{
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.f - 2e-4f, AREA_CODE_BASE);
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 20.f - 0.01f, AREA_CODE_BASE);
    const ColumnRaster r = raster(unitFrame(), s);
    CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 10, 11, AREA_CODE_BASE }, { 19, 20, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: tops within climb keep the higher code; past it the higher top's, in either order", "[skaircolumns]")
{
    // A floor at 10.5 with a wall rising to 20 over it, both in one column: the wall's top is no floor.
    for (S32 order = 0; order < 2; ++order)
    {
        AirSoup s;
        const auto floor = [&]() { addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.5f, AREA_CODE_BASE); };
        const auto wall = [&]() {
            addTri(s, LLVector3(0.5f, 0.f, 10.f), LLVector3(0.5f, 4.f, 10.f), LLVector3(0.5f, 4.f, 20.f), 0);
            addTri(s, LLVector3(0.5f, 0.f, 10.f), LLVector3(0.5f, 4.f, 20.f), LLVector3(0.5f, 0.f, 20.f), 0);
        };
        if (order)
        {
            wall();
            floor();
        }
        else
        {
            floor();
            wall();
        }
        ColumnRasterStats stats;
        const ColumnRaster r = raster(unitFrame(), s, nullptr, &stats);
        INFO("order " << order);
        CHECK(column(r, 0, 1) == std::vector<std::array<S32, 3>>{ { 10, 20, 0 } });
        CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 10, 11, AREA_CODE_BASE } });
        // Recast would have kept the floor's code over the wall's top had the wall come first.
        CHECK((stats.mTopWins > 0) == (order == 1));
    }
    // A step within climb of the floor takes the higher code whichever comes first.
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.5f, AREA_OBJECT);
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 11.5f, AREA_CODE_BASE);
    const ColumnRaster r = raster(unitFrame(2), s);
    CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 10, 12, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: spans that only touch merge; a unit apart they stay two", "[skaircolumns]")
{
    AirSoup s;
    addTri(s, LLVector3(0.f, 0.f, 0.f), LLVector3(4.f, 0.f, 0.f), LLVector3(4.f, 0.f, 10.f), 0);   // a wall on y = 0, columns (*, 0)
    addTri(s, LLVector3(0.f, 0.f, 10.f), LLVector3(4.f, 0.f, 10.f), LLVector3(4.f, 0.f, 20.f), 0);
    addTri(s, LLVector3(0.f, 0.f, 21.f), LLVector3(4.f, 0.f, 21.f), LLVector3(4.f, 0.f, 30.f), 0);
    const ColumnRaster r = raster(unitFrame(), s);
    CHECK(column(r, 3, 0) == std::vector<std::array<S32, 3>>{ { 0, 20, 0 }, { 21, 30, 0 } });
}

TEST_CASE("skaircolumns: a closed box is solid from its floor to its roof, the roof's code on top", "[skaircolumns]")
{
    AirSoup s;
    addBox(s, LLVector3(0.f, 0.f, 5.f), LLVector3(4.f, 4.f, 9.f), AREA_CODE_BASE);
    const ColumnRaster r = raster(unitFrame(), s);
    CHECK(column(r, 1, 2) == std::vector<std::array<S32, 3>>{ { 5, 10, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: an exclusion volume clears the codes of what it overlaps, and only that", "[skaircolumns]")
{
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 10.5f, AREA_CODE_BASE);
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 30.5f, AREA_CODE_BASE);
    // A cut over x < 2 from 9 to 12.
    AirSoup box;
    addBox(box, LLVector3(0.f, 0.f, 9.f), LLVector3(2.f, 4.f, 12.f), 0);
    s.mCutVerts = box.mVerts;
    s.mCuts = box.mSolids;
    const ColumnRaster r = raster(unitFrame(), s);
    CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 10, 11, 0 }, { 30, 31, AREA_CODE_BASE } });
    CHECK(column(r, 2, 1) == std::vector<std::array<S32, 3>>{ { 10, 11, AREA_CODE_BASE }, { 30, 31, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: a crevice one column wide is bridged at the higher side; a wall in it is not", "[skaircolumns]")
{
    for (S32 walled = 0; walled < 2; ++walled)
    {
        AirSoup s;
        addSlab(s, 0.f, 0.f, 1.f, 4.f, 10.5f, AREA_CODE_BASE);     // column x = 0
        addSlab(s, 2.f, 0.f, 4.f, 4.f, 11.5f, AREA_CODE_BASE);     // columns x = 2, 3; x = 1 is the crevice
        if (walled)
        {
            addTri(s, LLVector3(1.5f, 0.f, 0.f), LLVector3(1.5f, 4.f, 0.f), LLVector3(1.5f, 4.f, 30.f), 0);
            addTri(s, LLVector3(1.5f, 0.f, 0.f), LLVector3(1.5f, 4.f, 30.f), LLVector3(1.5f, 0.f, 30.f), 0);
        }
        ColumnRasterStats stats;
        const ColumnRaster r = raster(unitFrame(2), s, nullptr, &stats);
        INFO("walled " << walled);
        if (walled)
        {
            CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 0, 30, 0 } });
        }
        else
        {
            CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 11, 12, AREA_CODE_BASE } });
            CHECK(stats.mBridges == 2u);    // rows 1 and 2; the grid's edge rows are not scanned
        }
    }
}

TEST_CASE("skaircolumns: the land culls what lies under it and trims what reaches through it", "[skaircolumns]")
{
    AirSoup s;
    addSlab(s, 0.f, 0.f, 4.f, 4.f, 3.5f, AREA_CODE_BASE);       // buried
    addBox(s, LLVector3(0.f, 0.f, 2.f), LLVector3(4.f, 4.f, 8.f), AREA_CODE_BASE);
    TerrainWindow t;
    t.mOrigin.set(-1.f, -1.f, 0.f);
    t.mSpacing = 2.f;
    t.mWidth = t.mHeight = 4;
    t.mZ.assign(16, 5.f);
    const ColumnRaster r = raster(unitFrame(), s, &t);
    CHECK(column(r, 1, 1) == std::vector<std::array<S32, 3>>{ { 5, 9, AREA_CODE_BASE } });
}

TEST_CASE("skaircolumns: a column taller than the stack merges on the heap", "[skaircolumns]")
{
    AirSoup s;
    for (S32 k = 0; k < 80; ++k)
    {
        addSlab(s, 0.f, 0.f, 4.f, 4.f, 2.f * k + 0.5f, AREA_CODE_BASE);
    }
    ColumnFrame f = unitFrame(0);
    f.mHeightRange = 200.f;
    ColumnRasterStats stats;
    const ColumnRaster r = raster(f, s, nullptr, &stats);
    CHECK(r.count(5) == 80u);
    CHECK(stats.mHeapColumns == 16u);
}

// The synthetic scenes against Recast's raster of them: the same soup, the states before the walk rules.
TEST_CASE("skaircolumns: the synthetic scenes raster as Recast's within tolerance", "[skaircolumns]")
{
    for (const Scene& scene : fixtureScenes())
    {
        RasterCapture capture;
        TileAirResult recast;
        REQUIRE(rasteriseTile(scene.mInput, recast, nullptr, &capture));
        AirSoup soup;
        TileAirResult r;
        REQUIRE(buildAirSoup(scene.mInput, soup, r));
        ColumnFrame f = tileFrame(scene.mInput.mTileX, scene.mInput.mTileY, scene.mInput.mTileSize, scene.mInput.mZMin, scene.mInput.mZMax,
                                  scene.mInput.mCell, capture.mBorder, scene.mInput.mAvatar.mClimb);
        CHECK(f.mOrigin.mV[VX] == capture.mBmin[2]);
        CHECK(f.mOrigin.mV[VY] == capture.mBmin[0]);
        CHECK(f.mOrigin.mV[VZ] == capture.mBmin[1]);
        CHECK(f.mClimb == capture.mClimb);
        Ours ours;
        ColumnSource src;
        src.mSoup = &soup;
        src.mTerrain = &scene.mInput.mTerrain;
        src.mAfterCut = &ours.mCut;
        src.mAfterBridges = &ours.mBridged;
        rasteriseColumns(f, src, ours.mCulled, &ours.mStats);
        const auto recastColumns = [](const RasterColumns& c) {
            RecastColumns out;
            out.mWidth = c.mWidth;
            out.mHeight = c.mHeight;
            out.mStart = c.mStart;
            out.mSpans = c.mSpans;
            return out;
        };
        const StateDiff d0 = compareState(ours.mCut, recastColumns(capture.mRasterised));
        const StateDiff d1 = compareState(ours.mBridged, recastColumns(capture.mBridged));
        const StateDiff d2 = compareState(ours.mCulled, recastColumns(capture.mCulled));
        INFO(scene.mName << "\n  cut     " << d0.text() << "\n  bridged " << d1.text() << "\n  culled  " << d2.text());
        // Heights: Recast's float32 puts a face a rounding error off a column edge in the column past it, ours
        // in the one it is in (the hollow box's front wall); nothing else moves.
        CHECK(d2.frac(d2.mFloorOff + d2.mBottomOff + d2.mUnpaired) <= 0.005);
        // Codes: the merge's "top wins" takes a wall's null code where Recast kept a floor's under it.
        CHECK(d2.frac(d2.mWalkFlips) <= 0.005);
    }
}

// Run by hand: SKAIR_FIXTURES names the captured fixtures (a folder of sets), SKAIR_FIXTURES_OUT where our
// raster's states go for the oracle's diff, SKAIR_CAPTURE_THREADS the workers.
TEST_CASE("skaircolumns: the captured fixtures raster as Recast's within tolerance", "[skaircolumns][.fixtures]")
{
    const char* dir_env = std::getenv("SKAIR_FIXTURES");
    if (!dir_env || !*dir_env)
    {
        SKIP("SKAIR_FIXTURES is not set");
    }
    const char* out_env = std::getenv("SKAIR_FIXTURES_OUT");
    const char* threads_env = std::getenv("SKAIR_CAPTURE_THREADS");
    const S32 threads = threads_env ? std::max(1, std::atoi(threads_env)) : 4;
    std::vector<std::filesystem::path> files;
    for (const auto& set : std::filesystem::directory_iterator(dir_env))
    {
        if (set.is_directory())
        {
            for (const auto& f : std::filesystem::directory_iterator(set.path()))
            {
                if (f.path().extension() == ".skaf")
                {
                    files.push_back(f.path());
                }
            }
        }
    }
    std::sort(files.begin(), files.end());
    std::mutex lock;
    std::map<std::string, StateDiff[3]> sets;
    std::map<std::string, ColumnRasterStats> timing;
    std::map<std::string, WalkDiff> walks;
    std::atomic<size_t> next{ 0 }, failed{ 0 };
    std::vector<std::thread> pool;
    for (S32 k = 0; k < threads; ++k)
    {
        pool.emplace_back([&]() {
            for (size_t i = next++; i < files.size(); i = next++)
            {
                Fixture fx;
                if (!readFixture(files[i], fx))
                {
                    ++failed;
                    continue;
                }
                Ours ours;
                rasteriseFixture(fx, ours, true);
                const WalkDiff wd = compareWalk(ours.mWalked, fx);
                const StateDiff d[3] = { compareState(ours.mCut, fx.columns("RAS0")), compareState(ours.mBridged, fx.columns("RAS1")),
                                         compareState(ours.mCulled, fx.columns("RAS2")) };
                if (out_env && *out_env)
                {
                    const std::filesystem::path dir = std::filesystem::path(out_env) / files[i].parent_path().filename();
                    std::filesystem::create_directories(dir);
                    std::ofstream out(dir / (files[i].stem().string() + ".skcr"), std::ios::binary);
                    out.write("SKCR", 4);
                    dumpColumns(out, "RAS0", ours.mCut);
                    dumpColumns(out, "RAS1", ours.mBridged);
                    dumpColumns(out, "RAS2", ours.mCulled);
                    dumpColumns(out, "WALK", ours.mWalked);
                }
                std::lock_guard<std::mutex> guard(lock);
                const std::string set = files[i].parent_path().filename().string();
                for (S32 s = 0; s < 3; ++s)
                {
                    sets[set][s].add(d[s]);
                }
                walks[set].add(wd);
                ColumnRasterStats& t = timing[set];
                t.mClipMs += ours.mStats.mClipMs;
                t.mFillSolidMs += ours.mStats.mFillSolidMs;
                t.mCountMs += ours.mStats.mCountMs;
                t.mMergeMs += ours.mStats.mMergeMs;
                t.mCutMs += ours.mStats.mCutMs;
                t.mBridgesMs += ours.mStats.mBridgesMs;
                t.mCullMs += ours.mStats.mCullMs;
                t.mTopWalkMs += ours.mStats.mTopWalkMs;
                t.mAvatarWalkMs += ours.mStats.mAvatarWalkMs;
                t.mTopWins += ours.mStats.mTopWins;
                t.mHeapColumns += ours.mStats.mHeapColumns;
            }
        });
    }
    for (std::thread& t : pool)
    {
        t.join();
    }
    CHECK(failed == 0u);
    for (const auto& [set, d] : sets)
    {
        const ColumnRasterStats& t = timing[set];
        const WalkDiff& w = walks[set];
        WARN(set << "\n  cut     " << d[0].text() << "\n  bridged " << d[1].text() << "\n  culled  " << d[2].text() << "\n  walked  " << w.text()
                 << llformat("\n  ms: clip %.0f fillsolid %.0f count+fill %.0f merge %.0f cut %.0f bridges %.0f cull %.0f top walk %.0f avatar walk %.0f; "
                             "top wins %u, heap columns %u",
                             t.mClipMs, t.mFillSolidMs, t.mCountMs, t.mMergeMs, t.mCutMs, t.mBridgesMs, t.mCullMs, t.mTopWalkMs, t.mAvatarWalkMs, t.mTopWins,
                             t.mHeapColumns));
        CHECK(w.frac(w.mTopFlips) <= 0.002);
        CHECK(w.frac(w.mAvatarFlips) <= 0.01);
        CHECK(w.frac(w.mAreaFlips) <= 0.001);
        // Caps on the outcome against Recast's (plan 1.8), each class explained in the tolerance report: a face lying
        // on a column edge goes to its solid's side only, so the lip Recast stood a cell outside every grid-aligned
        // wall is gone (tops, AVATAR); "top wins" leaves a wall's top no floor (walk flips); heights a float's noise
        // off a unit boundary are on it (bottoms).
        CHECK(d[2].frac(d[2].mFloorOff) <= 0.005);
        CHECK(d[2].frac(d[2].mBottomOff + d[2].mUnpaired) <= 0.01);
        CHECK(d[2].frac(d[2].mWalkFlips) <= 0.005);
    }
}
