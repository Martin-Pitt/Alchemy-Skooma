/**
 * @file sknavstamp_test.cpp
 * @brief The space stamp: spans to components, door-leaf floors, colours and the palette's overflow cascade.
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
#include "../sknavbuild.h"
#include "../sknavenclosure.h"
#include "../sknavlayout.h"
#include "../sknavskymap.h"
#include "../sknavspacefilter.h"
#include "../sknavspaces.h"
#include "../sknavstamp.h"

#include "llformat.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"
#include "RecastAlloc.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <set>

namespace
{
    using namespace sknav;

    struct FreeChf
    {
        void operator()(rcCompactHeightfield* c) const { rcFreeCompactHeightfield(c); }
    };
    typedef std::unique_ptr<rcCompactHeightfield, FreeChf> ChfPtr;

    /// A flat floor of w by h cells, one span each, its area code from code(x, z); 0 leaves the cell empty.
    ChfPtr floorOf(S32 w, S32 h, const std::function<U8(S32, S32)>& code, F32 cs = 0.25f)
    {
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        const F32 bmin[3] = { 0.f, 0.f, 0.f }, bmax[3] = { w * cs, 10.f, h * cs };
        tut::ensure("heightfield", rcCreateHeightfield(&ctx, *hf, w, h, bmin, bmax, cs, cs));
        for (S32 z = 0; z < h; ++z)
        {
            for (S32 x = 0; x < w; ++x)
            {
                const U8 a = code(x, z);
                if (a)
                {
                    rcAddSpan(&ctx, *hf, x, z, 0, 4, a, 1);
                }
            }
        }
        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        tut::ensure("compact", rcBuildCompactHeightfield(&ctx, 8, 1, *hf, *chf));
        rcFreeHeightField(hf);
        return ChfPtr(chf);
    }

    /// The span in cell (x, z), or -1.
    S32 spanAt(const rcCompactHeightfield& chf, S32 x, S32 z)
    {
        const rcCompactCell& c = chf.cells[x + z * chf.width];
        return c.count ? static_cast<S32>(c.index) : -1;
    }

    /// One gap per span, numbered in span order, with no gap where gap_of says -1.
    std::vector<U32> gapsOf(const rcCompactHeightfield& chf, const std::function<S32(S32, S32)>& gap_of)
    {
        std::vector<U32> out(chf.spanCount, SPAN_NO_GAP);
        for (S32 z = 0; z < chf.height; ++z)
        {
            for (S32 x = 0; x < chf.width; ++x)
            {
                const S32 i = spanAt(chf, x, z);
                if (i >= 0)
                {
                    const S32 g = gap_of(x, z);
                    out[i] = g < 0 ? SPAN_NO_GAP : static_cast<U32>(g);
                }
            }
        }
        return out;
    }

    /// Adjacent components, by a span connection, sharing a colour: zero when the stamp holds.
    U32 clashes(const rcCompactHeightfield& chf, const TileStamp& st)
    {
        U32 n = 0;
        for (S32 z = 0; z < chf.height; ++z)
        {
            for (S32 x = 0; x < chf.width; ++x)
            {
                const S32 i = spanAt(chf, x, z);
                if (i < 0 || st.mSpanComponent[i] == STAMP_NONE)
                {
                    continue;
                }
                for (S32 d = 0; d < 4; ++d)
                {
                    const S32 con = rcGetCon(chf.spans[i], d);
                    if (con == RC_NOT_CONNECTED)
                    {
                        continue;
                    }
                    const S32 j = static_cast<S32>(chf.cells[(x + rcGetDirOffsetX(d)) + (z + rcGetDirOffsetY(d)) * chf.width].index) + con;
                    const U16 a = st.mSpanComponent[i], b = st.mSpanComponent[j];
                    n += b != STAMP_NONE && a != b && st.mComponentColour[a] == st.mComponentColour[b] ? 1 : 0;
                }
            }
        }
        return n;
    }

    std::vector<U8> identityReal()
    {
        std::vector<U8> real(64);
        for (S32 i = 0; i < 64; ++i)
        {
            real[i] = i < AREA_CODE_BASE ? static_cast<U8>(i) : AREA_OBJECT;
        }
        return real;
    }

    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1, U8 exclude = 0)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, 1, 0);
        r->mAxes[2].set(0, 0, 1);
        r->mSolid = true;
        r->mExclude = exclude;
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
    }

    TileBuildInput tileInput()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;           // coarser than the viewer, to keep the tests quick
        in.mAir = true;
        in.mTerrain.mOrigin.set(-6.f, -6.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 45;
        in.mTerrain.mZ.assign(45 * 45, 0.f);
        in.mTerrain.mWater = -10.f;
        return in;
    }

    /// A tile rasterised, its spaces cut, its spans stamped and its polys built with the stamp.
    struct Stamped
    {
        TileBuildResult mResult;
        TileRasterPtr mRaster;
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;
        TileStampInput mInput;
        TileStamp mStamp;
        TileBuildResult mPolys;
        TileBuildResult mPlain;     ///< the same raster with no stamp

        Stamped(const TileBuildInput& in, const std::vector<DoorEntity>& doors)
        {
            mRaster = rasteriseTile(in, mResult);
            tut::ensure("rasterised: " + mResult.mError, mRaster && mResult.mAir);
            auto enc = std::make_shared<EnclosureInput>();
            enc->mGeneration = 1;
            enc->mParams.mWalkHeadM = 2.f;
            enc->mTiles.push_back({ TileKey{ 0, 0 }, mResult.mAir, 1 });
            auto sky = std::make_shared<SkyMap>();
            sky->set(1, LLVector3d(mResult.mAir->mOrigin), *mResult.mAir);
            enc->mSky = sky;
            EnclosurePass pass(enc);
            while (!pass.step(1u << 30))
            {
            }
            mEnc = pass.takeProduct();
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mGeneration = 1;
            rooms->mDoors = doors;
            mRooms = computeSpaces(rooms, mEnc);
            stampInput(*mResult.mAir, *mEnc->tile(TileKey{ 0, 0 }), *mRooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), mInput);
            const S32 border = borderCells(in.mParams[0].mRadius, in.mCell);
            stampTile(*mRaster->mChf, border, mRaster->mSpanGap, mInput, mRaster->mReal, mRaster->mFlights, StampParams(), mStamp);
            mPolys = mResult;
            polygoniseTile(in, *mRaster, mPolys, nullptr, &mInput, &mStamp);
            mPlain = mResult;
            polygoniseTile(in, *mRaster, mPlain);
        }

        /// The space at an SL point on the floor, as an index into the product, or -1.
        S32 spaceAt(F32 x, F32 y) const
        {
            Enclosure e;
            return mEnc->enclosureAt(LLVector3(x, y, 0.5f), e) && e.mSpace ? e.mSpace - 1 : -1;
        }
    };

    /// Each poly's outline in SL plan, from a tile's Detour data.
    struct DetourPolys
    {
        dtNavMesh* mNav = nullptr;
        dtNavMeshQuery* mQuery = nullptr;
        std::vector<U8> mBytes;

        explicit DetourPolys(const std::vector<U8>& data) : mBytes(data)
        {
            mNav = allocMeshNav();
            mQuery = dtAllocNavMeshQuery();
            tut::ensure("tile in", !mBytes.empty() && dtStatusSucceed(mNav->addTile(mBytes.data(), static_cast<int>(mBytes.size()), 0, 0, nullptr)));
            tut::ensure("query", dtStatusSucceed(mQuery->init(mNav, 4096)));
        }
        ~DetourPolys()
        {
            dtFreeNavMeshQuery(mQuery);
            dtFreeNavMesh(mNav);
        }
        const dtMeshTile* tile() const { return mNav->getTileAt(0, 0, 0); }
        /// The poly's least and greatest SL x; a poly off the house's floor reads as nowhere.
        void spanX(S32 p, F32& lo, F32& hi) const
        {
            const dtMeshTile* t = tile();
            const dtPoly& poly = t->polys[p];
            lo = 1e9f;
            hi = -1e9f;
            F32 top = -1e9f, ylo = 1e9f, yhi = -1e9f;
            for (S32 v = 0; v < poly.vertCount; ++v)
            {
                const F32 x = t->verts[poly.verts[v] * 3 + 2];
                lo = std::min(lo, x);
                hi = std::max(hi, x);
                top = std::max(top, t->verts[poly.verts[v] * 3 + 1]);
                ylo = std::min(ylo, t->verts[poly.verts[v] * 3]);
                yhi = std::max(yhi, t->verts[poly.verts[v] * 3]);
            }
            // Only the house's floor: the roof and the ground round it are other spaces.
            if (top > 1.5f || ylo < 2.f || yhi > 10.f || lo < 2.f || hi > 18.f)
            {
                lo = hi = -100.f;
            }
        }
    };

    /// The walled pair of rooms either side of x = 10, joined by a double doorway at y 5 to 7 with its leaf shut in it.
    TileBuildInput twoRoomsDoor(std::vector<DoorEntity>& doors)
    {
        TileBuildInput in = tileInput();
        const auto wall = [&in](F32 x0, F32 y0, F32 x1, F32 y1) { in.mRecords.push_back(box(x0, y0, 0.f, x1, y1, 3.f)); };
        wall(2.f, 2.f, 18.f, 2.25f);
        wall(2.f, 9.75f, 18.f, 10.f);
        wall(2.f, 2.f, 2.25f, 10.f);
        wall(17.75f, 2.f, 18.f, 10.f);
        wall(10.f, 2.f, 10.25f, 5.f);
        wall(10.f, 7.f, 10.25f, 10.f);
        in.mRecords.push_back(box(10.f, 5.f, 2.6f, 10.25f, 7.f, 3.f));                         // lintel, over an avatar's 2 m
        in.mRecords.push_back(box(2.f, 2.f, 3.f, 18.f, 10.f, 3.25f));                        // roof
        in.mRecords.push_back(box(2.f, 2.f, 0.f, 18.f, 10.f, 0.25f));                          // the floor slab
        in.mRecords.push_back(box(10.f, 5.f, 0.25f, 10.25f, 7.f, 2.55f, EXCLUDE_AVATAR));      // the shut leaf, on it
        DoorEntity d;
        d.mPlanePoint.setVec(10.125, 6.0, 0.25);
        d.mNormal.set(1.f, 0.f, 0.f);
        d.mWidth = 2.f;
        d.mHeight = 2.3f;
        d.mSill = 0.25;
        d.mMin.setVec(10.0, 5.0, 0.25);
        d.mMax.setVec(10.25, 7.0, 2.55);
        d.mId.generate();
        doors.push_back(d);
        return in;
    }

    /// A 7 by 10 grid of 4-cell squares, each its own space, its slope code from code(cx, cz).
    struct Grid70
    {
        ChfPtr mChf;
        std::vector<U32> mGaps;
        TileStampInput mInput;

        explicit Grid70(const std::function<U8(S32, S32)>& code)
        {
            mChf = floorOf(28, 40, [&code](S32 x, S32 z) { return code(x / 4, z / 4); });
            mGaps = gapsOf(*mChf, [](S32 x, S32 z) { return (x / 4) + (z / 4) * 7; });
            mInput.mGapComponent.resize(70);
            for (U16 g = 0; g < 70; ++g)
            {
                mInput.mGapComponent[g] = g;
                mInput.mComponentSpace.push_back(g);
            }
        }
    };
}

namespace tut
{
    struct sknavstamp_data
    {
    };
    typedef test_group<sknavstamp_data> sknavstamp_group;
    typedef sknavstamp_group::object sknavstamp_object;
    tut::sknavstamp_group sknavstamp_tests("sknavstamp");

    template<> template<>
    void sknavstamp_object::test<1>()
    {
        set_test_name("two spaces side by side take different colours, and every span its own space's palette code");
        const ChfPtr chf = floorOf(16, 8, [](S32, S32) { return AREA_CODE_BASE; });
        const std::vector<U32> gaps = gapsOf(*chf, [](S32 x, S32) { return x < 8 ? 0 : 1; });
        TileStampInput in;
        in.mGapComponent = { 0, 1 };
        in.mComponentSpace = { 5, 9 };
        TileStamp st;
        stampTile(*chf, 0, gaps, in, identityReal(), {}, StampParams(), st);
        ensure_equals("two colours", std::set<U8>(st.mComponentColour.begin(), st.mComponentColour.end()).size(), 2u);
        ensure_equals("no clash", clashes(*chf, st), 0u);
        const U8 left = st.mSpanCode[spanAt(*chf, 2, 2)], right = st.mSpanCode[spanAt(*chf, 12, 2)];
        ensure("each side one code, and they differ", left && right && left != right);
        ensure_equals("the code unfolds to the slope code", st.mPalette[left].mSlope, AREA_CODE_BASE);
        ensure_equals("it fits", st.mOverflow, TileStamp::FIT);
    }

    template<> template<>
    void sknavstamp_object::test<2>()
    {
        set_test_name("a door leaf's floor, with no gap of the air, is split at the portal's plane, each half to its side's space");
        // Two rooms along SL x (the compact heightfield's z), the leaf's floor in cells 7 and 8 with no gap,
        // the portal at SL x = 2 m facing +x.
        TileStampInput in;
        in.mGapComponent = { 0, 1 };
        in.mComponentSpace = { 0, 1 };
        StampPortal p;
        p.mCentre.set(2.f, 1.f, 1.f);
        p.mNormal.set(1.f, 0.f, 0.f);
        p.mA.set(2.f, 0.f, 0.f);
        p.mB.set(2.f, 2.f, 0.f);
        p.mSide[0] = 0;
        p.mSide[1] = 1;
        p.mKind = static_cast<U8>(PortalKind::OPENING);   // a door's whole threshold splits at its plane; this tests gapless floor alone
        in.mPortals.push_back(p);
        const ChfPtr across = floorOf(8, 16, [](S32, S32) { return AREA_CODE_BASE; });
        const std::vector<U32> gaps_across = gapsOf(*across, [](S32, S32 z) { return z < 7 ? 0 : z > 8 ? 1 : -1; });
        TileStamp st;
        stampTile(*across, 0, gaps_across, in, identityReal(), {}, StampParams(), st);
        ensure_equals("both rows of the leaf's floor took a side", st.mDoorSpans, 16u);
        ensure_equals("the near half is the first room's", st.mSpanComponent[spanAt(*across, 3, 7)], U16(0));
        ensure_equals("the far half is the second's", st.mSpanComponent[spanAt(*across, 3, 8)], U16(1));
        ensure_equals("nothing flooded", st.mFloodSpans, 0u);
        ensure_equals("nothing left unspaced", st.mUnspacedSpans, 0u);
        ensure("and they are coloured apart", st.mComponentColour[0] != st.mComponentColour[1]);

        // Out of the portal's reach, a gapless floor takes the space of the floor it joins.
        TileStampInput far_in = in;
        far_in.mPortals[0].mA.set(30.f, 0.f, 0.f);
        far_in.mPortals[0].mB.set(30.f, 2.f, 0.f);
        far_in.mPortals[0].mCentre.set(30.f, 1.f, 1.f);
        stampTile(*across, 0, gaps_across, far_in, identityReal(), {}, StampParams(), st);
        ensure_equals("flooded instead", st.mFloodSpans, 16u);
        ensure_equals("from the nearer room", st.mSpanComponent[spanAt(*across, 3, 7)], U16(0));
        ensure_equals("each side from its own", st.mSpanComponent[spanAt(*across, 3, 8)], U16(1));
    }

    template<> template<>
    void sknavstamp_object::test<3>()
    {
        set_test_name("colours follow the partition, not the ids: renamed spaces stamp the same spans and the same pattern");
        const Grid70 grid([](S32, S32) { return AREA_CODE_BASE; });
        TileStamp a, b;
        stampTile(*grid.mChf, 0, grid.mGaps, grid.mInput, identityReal(), {}, StampParams(), a);
        TileStampInput renamed = grid.mInput;
        for (U32& s : renamed.mComponentSpace)
        {
            s = 1000 - s;
        }
        stampTile(*grid.mChf, 0, grid.mGaps, renamed, identityReal(), {}, StampParams(), b);
        ensure("the same codes", a.mSpanCode == b.mSpanCode);
        ensure("the same colours", a.mComponentColour == b.mComponentColour);
        ensure_equals("a grid of squares needs few colours", *std::max_element(a.mComponentColour.begin(), a.mComponentColour.end()), U8(2));
        ensure_equals("no clash", clashes(*grid.mChf, a), 0u);
    }

    template<> template<>
    void sknavstamp_object::test<4>()
    {
        set_test_name("70 spaces over the palette go down the cascade, every stage still stamping every span");
        const std::vector<U8> real = identityReal();
        // Every square its own non-flight code: 70 codes, so landings merge to one and it fits.
        {
            const Grid70 grid([](S32 cx, S32 cz) { return static_cast<U8>(AREA_CODE_BASE + (cx + cz * 7) % 58); });
            StampParams thirty;
            thirty.mPaletteMax = 30;
            TileStamp st;
            stampTile(*grid.mChf, 0, grid.mGaps, grid.mInput, real, {}, thirty, st);
            ensure_equals("codes merged", static_cast<S32>(st.mOverflow), static_cast<S32>(TileStamp::CODES_MERGED));
            ensure("within the palette", st.mPairs <= 30u);
            ensure_equals("no clash", clashes(*grid.mChf, st), 0u);
        }
        // Flight codes never merge: 40 of them over two colours is 80 pairs, so spaces fuse.
        std::vector<Flight> flights(1);
        for (S32 c = 0; c < 40; ++c)
        {
            flights[0].mCodes.push_back(static_cast<U8>(AREA_CODE_BASE + c));
        }
        {
            const Grid70 grid([](S32 cx, S32 cz) { return static_cast<U8>(AREA_CODE_BASE + (cx + cz * 7) % 35); });
            StampParams forty;
            forty.mPaletteMax = 40;
            TileStamp st;
            stampTile(*grid.mChf, 0, grid.mGaps, grid.mInput, real, flights, forty, st);
            ensure_equals("spaces fused", static_cast<S32>(st.mOverflow), static_cast<S32>(TileStamp::SPACES_FUSED));
            ensure("some fused", st.mFused > 0);
            ensure("within the palette", st.mPairs <= 40u);
        }
        // A palette smaller than the flights' codes: one colour, then flights merged too.
        {
            const Grid70 grid([](S32 cx, S32 cz) { return static_cast<U8>(AREA_CODE_BASE + (cx + cz * 7) % 40); });
            StampParams small;
            small.mPaletteMax = 8;
            TileStamp st;
            stampTile(*grid.mChf, 0, grid.mGaps, grid.mInput, real, flights, small, st);
            ensure_equals("one colour", static_cast<S32>(st.mOverflow), static_cast<S32>(TileStamp::ONE_COLOUR));
            ensure("within the palette", st.mPairs <= 8u);
        }
        // Whatever the stage, no walkable span is left without a code.
        for (S32 cap : { 63, 30, 8, 2 })
        {
            const Grid70 grid([](S32 cx, S32 cz) { return static_cast<U8>(AREA_CODE_BASE + (cx + cz * 7) % 40); });
            StampParams p;
            p.mPaletteMax = cap;
            TileStamp st;
            stampTile(*grid.mChf, 0, grid.mGaps, grid.mInput, real, flights, p, st);
            U32 bare = 0;
            for (S32 i = 0; i < grid.mChf->spanCount; ++i)
            {
                bare += grid.mChf->areas[i] && !st.mSpanCode[i] ? 1 : 0;
            }
            ensure_equals(llformat("cap %d: every span stamped", cap), bare, 0u);
            ensure(llformat("cap %d: within it", cap), st.mPairs <= static_cast<U32>(cap));
        }
    }

    template<> template<>
    void sknavstamp_object::test<5>()
    {
        set_test_name("a tile's border is colour 0 with no space, and a gap with no space stays unspaced");
        const ChfPtr chf = floorOf(12, 12, [](S32, S32) { return AREA_CODE_BASE; });
        const std::vector<U32> gaps = gapsOf(*chf, [](S32 x, S32) { return x < 6 ? 0 : 1; });
        TileStampInput in;
        in.mGapComponent = { 0, STAMP_NONE };
        in.mComponentSpace = { 3 };
        TileStamp st;
        stampTile(*chf, 2, gaps, in, identityReal(), {}, StampParams(), st);
        ensure_equals("border span", st.mSpanComponent[spanAt(*chf, 0, 5)], STAMP_NONE);
        ensure_equals("border colour", st.mPalette[st.mSpanCode[spanAt(*chf, 0, 5)]].mColour, U8(0));
        ensure_equals("unspaced gap", st.mSpanComponent[spanAt(*chf, 8, 5)], STAMP_NONE);
        ensure_equals("unspaced spans counted", st.mUnspacedSpans, 4u * 8u);
        ensure("the space is coloured", st.mPalette[st.mSpanCode[spanAt(*chf, 3, 5)]].mColour >= 1);
    }

    template<> template<>
    void sknavstamp_object::test<6>()
    {
        set_test_name("two rooms through a door give disjoint poly sets, split at the leaf's plane");
        std::vector<DoorEntity> doors;
        const TileBuildInput in = twoRoomsDoor(doors);
        const Stamped s(in, doors);
        const S32 a = s.spaceAt(6.f, 6.f), b = s.spaceAt(14.f, 6.f);
        ensure("two rooms, two spaces", a >= 0 && b >= 0 && a != b);
        bool door = false;
        for (const Portal& p : s.mRooms->mPortals)
        {
            door = door || (p.mKind == PortalKind::DOOR && ((p.mSpaces[0] == static_cast<U32>(a) && p.mSpaces[1] == static_cast<U32>(b)) ||
                                                            (p.mSpaces[0] == static_cast<U32>(b) && p.mSpaces[1] == static_cast<U32>(a))));
        }
        ensure("a door portal between them", door);
        ensure("the leaf's floor took the sides of its plane", s.mStamp.mDoorSpans > 0);
        ensure_equals("stamped and pure", s.mPolys.mStamp.mImpurePolys, 0u);

        // No poly reaches into both rooms: none runs from short of the leaf to past it.
        const auto crossing = [](const std::vector<U8>& data, U32& left, U32& right) {
            const DetourPolys d(data);
            U32 n = 0;
            left = right = 0;
            for (S32 p = 0; p < d.tile()->header->polyCount; ++p)
            {
                F32 lo, hi;
                d.spanX(p, lo, hi);
                n += lo < 10.f - 0.13f && hi > 10.25f + 0.13f ? 1 : 0;
                left += hi <= 10.25f && lo > 2.f ? 1 : 0;
                right += lo >= 10.f && hi < 18.f ? 1 : 0;
            }
            return n;
        };
        U32 left = 0, right = 0, plain_left = 0, plain_right = 0;
        const U32 stamped = crossing(s.mPolys.mData[0], left, right);
        const U32 plain = crossing(s.mPlain.mData[0], plain_left, plain_right);
        std::cout << "polys through the doorway: " << plain << " unstamped, " << stamped << " stamped" << std::endl;
        ensure_equals("no stamped poly in both rooms", stamped, 0u);
        ensure("polys in each room", left > 0 && right > 0);

        // Each room's polys are the component of its space, on its side.
        U16 ca = STAMP_NONE, cb = STAMP_NONE;
        for (size_t c = 0; c < s.mInput.mComponentSpace.size(); ++c)
        {
            ca = s.mInput.mComponentSpace[c] == static_cast<U32>(a) ? static_cast<U16>(c) : ca;
            cb = s.mInput.mComponentSpace[c] == static_cast<U32>(b) ? static_cast<U16>(c) : cb;
        }
        const DetourPolys d(s.mPolys.mData[0]);
        U32 wrong = 0, in_a = 0, in_b = 0;
        for (S32 p = 0; p < d.tile()->header->polyCount; ++p)
        {
            F32 lo, hi;
            d.spanX(p, lo, hi);
            if (lo <= -99.f)
            {
                continue;
            }
            const U16 c = s.mPolys.mStamp.mPolyComponent[p];
            in_a += c == ca ? 1 : 0;
            in_b += c == cb ? 1 : 0;
            wrong += (c == ca && hi > 10.125f + 0.13f) || (c == cb && lo < 10.125f - 0.13f) ? 1 : 0;
        }
        ensure("polys of both spaces", in_a > 0 && in_b > 0);
        ensure_equals("each on its own side of the plane", wrong, 0u);
    }

    template<> template<>
    void sknavstamp_object::test<7>()
    {
        set_test_name("a space priced at ten turns a path around it, and one priced at nothing bars it: the filter dispatches virtually");
        TileBuildInput in = tileInput();
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised", raster && r.mAir);
        // A band across the middle, open past y = 20, is the second space.
        TileStampInput stamp_in;
        stamp_in.mGapComponent.assign(r.mAir->mGaps.size(), 0);
        for (const AirLeaf& l : r.mAir->mLeaves)
        {
            const F32 x = (l.mX + 0.5f * (1 << l.mLevel)) * r.mAir->mCell, y = (l.mY + 0.5f * (1 << l.mLevel)) * r.mAir->mCell;
            for (U32 k = 0; k < l.mGapCount; ++k)
            {
                stamp_in.mGapComponent[l.mFirstGap + k] = x >= 12.f && x < 20.f && y < 20.f ? 1 : 0;
            }
        }
        stamp_in.mComponentSpace = { 0, 1 };
        TileStamp st;
        stampTile(*raster->mChf, borderCells(in.mParams[0].mRadius, in.mCell), raster->mSpanGap, stamp_in, raster->mReal, raster->mFlights,
                  StampParams(), st);
        TileBuildResult polys = r;
        polygoniseTile(in, *raster, polys, nullptr, &stamp_in, &st);
        const DetourPolys d(polys.mData[0]);
        const std::vector<U16>& comp = polys.mStamp.mPolyComponent;
        const auto space_of = [&comp](const dtMeshTile*, U32 p) -> U32 { return p < comp.size() && comp[p] != STAMP_NONE ? comp[p] + 1u : 0u; };
        const auto route = [&](const dtQueryFilter& f, F32& length, bool& through) {
            F32 a[3], b[3], sa[3], sb[3];
            toRecast(LLVector3(4.f, 4.f, 0.f), a);
            toRecast(LLVector3(28.f, 4.f, 0.f), b);
            const F32 ext[3] = { 1.f, 2.f, 1.f };
            dtPolyRef ra = 0, rb = 0;
            d.mQuery->findNearestPoly(a, ext, &f, &ra, sa);
            d.mQuery->findNearestPoly(b, ext, &f, &rb, sb);
            dtPolyRef path[256];
            int n = 0;
            ensure("a path", ra && rb && dtStatusSucceed(d.mQuery->findPath(ra, rb, sa, sb, &f, path, &n, 256)) && n > 0 && path[n - 1] == rb);
            through = false;
            for (int i = 0; i < n; ++i)
            {
                const dtMeshTile* t = nullptr;
                const dtPoly* p = nullptr;
                d.mNav->getTileAndPolyByRef(path[i], &t, &p);
                through = through || space_of(t, static_cast<U32>(p - t->polys)) == 2;
            }
            F32 straight[256 * 3];
            int ns = 0;
            d.mQuery->findStraightPath(sa, sb, path, n, straight, nullptr, nullptr, &ns, 256);
            length = 0.f;
            for (int i = 1; i < ns; ++i)
            {
                length += dtVdist(&straight[(i - 1) * 3], &straight[i * 3]);
            }
        };
        F32 plain_len = 0.f, costed_len = 0.f, barred_len = 0.f;
        bool plain_through = false, costed_through = true, barred_through = true;
        SpaceQueryFilter plain(space_of);
        route(plain, plain_len, plain_through);
        ensure("the plain path runs straight through the band", plain_through && plain_len < 26.f);
        SpaceQueryFilter costed(space_of);
        costed.setSpaceCost(2, 10.f);
        route(costed, costed_len, costed_through);
        ensure("priced at ten, the path goes round", !costed_through && costed_len > 40.f);
        SpaceQueryFilter barred(space_of);
        barred.setSpaceCost(2, 0.f);
        route(barred, barred_len, barred_through);
        ensure("barred, it goes round too", !barred_through);
        ensure_equals("a price kept as set", costed.spaceCost(2), 10.f);
        SpaceQueryFilter cheap(space_of);
        cheap.setSpaceCost(2, 0.25f);
        ensure_equals("raised to one", cheap.spaceCost(2), 1.f);
        // The per-tile form reads the same spaces through its cached table.
        std::vector<U32> table(comp.size());
        for (U32 p = 0; p < table.size(); ++p)
        {
            table[p] = space_of(nullptr, p);
        }
        const auto spaces_of = [&table](const dtMeshTile*) { return SpaceQueryFilter::TileSpaces{ table.data(), static_cast<U32>(table.size()) }; };
        SpaceQueryFilter tiled_costed(spaces_of), tiled_barred(spaces_of);
        tiled_costed.setSpaceCost(2, 10.f);
        tiled_barred.setSpaceCost(2, 0.f);
        F32 tiled_len = 0.f;
        bool tiled_through = true;
        route(tiled_costed, tiled_len, tiled_through);
        ensure("by tile, priced at ten, the same way round", !tiled_through && tiled_len == costed_len);
        tiled_through = true;
        route(tiled_barred, tiled_len, tiled_through);
        ensure("by tile, barred, round too", !tiled_through);
    }

    template<> template<>
    void sknavstamp_object::test<8>()
    {
        set_test_name("renamed spaces keep the pattern, so no polys are built again; only the binding moves");
        std::vector<DoorEntity> doors;
        const Stamped s(twoRoomsDoor(doors), doors);
        auto renamed = std::make_shared<RoomsProduct>(*s.mRooms);
        for (Space& sp : renamed->mSpaces)
        {
            sp.mId ^= 0x5a5a5a5au;
        }
        TileStampInput again;
        stampInput(*s.mResult.mAir, *s.mEnc->tile(TileKey{ 0, 0 }), *renamed, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), again);
        ensure("the same pattern", again.mPattern == s.mInput.mPattern);
        TileBinding before, after;
        bindStamp(s.mPolys.mStamp, s.mInput, s.mEnc->tile(TileKey{ 0, 0 }), *s.mRooms, before);
        bindStamp(s.mPolys.mStamp, again, s.mEnc->tile(TileKey{ 0, 0 }), *renamed, after);
        ensure("a new binding", before.mHash != after.mHash);
        ensure("the same flags", before.mPolyFlags == after.mPolyFlags);
        ensure("the ids renamed", !after.mComponentSpaceId.empty() && after.mComponentSpaceId[0] == (before.mComponentSpaceId[0] ^ 0x5a5a5a5au));
        bool door_flag = false, bucket = false;
        for (const U16 f : before.mPolyFlags)
        {
            door_flag = door_flag || (f & POLY_DOOR);
            bucket = bucket || (f & POLY_INDOORS);
        }
        ensure("polys on the door flagged DOOR", door_flag);
        ensure("the rooms INDOORS", bucket);
        // A partition that moved is a new pattern.
        auto split = std::make_shared<EnclosureProduct>(*s.mEnc);
        TileEnclosure& te = split->mTiles[0];
        for (GapEnclosure& g : te.mGaps)
        {
            g.mSpace = g.mSpace == static_cast<U16>(s.spaceAt(14.f, 6.f) + 1) ? static_cast<U16>(s.spaceAt(6.f, 6.f) + 1) : g.mSpace;
        }
        TileStampInput merged;
        stampInput(*s.mResult.mAir, te, *s.mRooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), merged);
        ensure("a merge is a new pattern", merged.mPattern != s.mInput.mPattern);
    }

    template<> template<>
    void sknavstamp_object::test<9>()
    {
        set_test_name("a tile past the radius stamps from coarse air in its own cells, every cell reading its coarse cell's gaps");
        std::vector<DoorEntity> doors;
        TileBuildInput coarse_in = twoRoomsDoor(doors);
        coarse_in.mCell = COARSE_AIR_M;
        coarse_in.mAirPad = airPadCells(coarse_in.mCell);
        TileBuildResult cr;
        rasteriseTile(coarse_in, cr);
        ensure("coarse air", cr.mAir && cr.mAir->mCell == 1.f);
        const TileAirPtr fine = upsampleAir(*cr.mAir, 4);
        ensure_equals("in quarter-metre cells", fine->mWidth, 128);
        ensure("leaves in key order", std::is_sorted(fine->mLeaves.begin(), fine->mLeaves.end(), [](const AirLeaf& x, const AirLeaf& y) {
                   return airKey(x.mX, x.mY) < airKey(y.mX, y.mY);
               }));
        U32 differ = 0;
        for (S32 iy = 0; iy < fine->mHeight; ++iy)
        {
            for (S32 ix = 0; ix < fine->mWidth; ++ix)
            {
                const S32 lf = findLeafByKey(*fine, ix, iy), lc = findLeafByKey(*cr.mAir, ix / 4, iy / 4);
                if ((lf < 0) != (lc < 0))
                {
                    ++differ;
                    continue;
                }
                if (lf < 0)
                {
                    continue;
                }
                const AirLeaf& a = fine->mLeaves[lf];
                const AirLeaf& b = cr.mAir->mLeaves[lc];
                differ += a.mGapCount != b.mGapCount ? 1 : 0;
                for (U32 k = 0; k < std::min(a.mGapCount, b.mGapCount); ++k)
                {
                    const AirGap& g = fine->mGaps[a.mFirstGap + k];
                    const AirGap& h = cr.mAir->mGaps[b.mFirstGap + k];
                    differ += g.mFloor != h.mFloor * 4 || (h.mCeiling != AIR_CEILING_OPEN && g.mCeiling != h.mCeiling * 4) || g.mFlags != h.mFlags ? 1 : 0;
                }
            }
        }
        ensure_equals("every fine cell is its coarse cell", differ, 0u);
        ensure_equals("seams cell for cell", fine->mSeams[TileAir::SIDE_POS_X].mStart.size(), 129u);

        // The tile at its own cell without air of its own takes that air, and its spans find gaps in it.
        TileBuildInput far_in = twoRoomsDoor(doors);
        far_in.mAir = false;
        far_in.mCoarseAir = true;
        TileBuildResult fr;
        const TileRasterPtr raster = rasteriseTile(far_in, fr);
        ensure("coarse air taken", raster && fr.mAir && fr.mCoarseAir && fr.mAirBuilt);
        U32 walk = 0, found = 0;
        const rcCompactHeightfield& chf = *raster->mChf;
        const S32 border = borderCells(far_in.mParams[0].mRadius, far_in.mCell);
        for (S32 z = border; z < chf.height - border; ++z)
        {
            for (S32 x = border; x < chf.width - border; ++x)
            {
                const rcCompactCell& c = chf.cells[x + z * chf.width];
                for (U32 i = c.index; i < c.index + c.count; ++i)
                {
                    if (chf.areas[i])
                    {
                        ++walk;
                        found += raster->mSpanGap[i] != SPAN_NO_GAP ? 1 : 0;
                    }
                }
            }
        }
        std::cout << found << " of " << walk << " spans found a coarse gap" << std::endl;
        ensure("most spans find a coarse gap", walk > 0 && found * 10 >= walk * 9);
    }

    template<> template<>
    void sknavstamp_object::test<10>()
    {
        set_test_name("doors on a slant split their thresholds span for span as the rule reads, the first door in order taking a span two hold");
        // A diagonal door, an oblique one whose normal is not square to its run, and a third over the second's threshold.
        TileStampInput in;
        in.mGapComponent = { 0 };
        in.mComponentSpace = { 0, 1, 2, 3, 4, 5, 6 };
        const auto door = [&in](F32 ax, F32 ay, F32 bx, F32 by, F32 nx, F32 ny, U16 s0, U16 s1) {
            StampPortal p;
            p.mA.set(ax, ay, 0.f);
            p.mB.set(bx, by, 0.f);
            p.mCentre.set((ax + bx) * 0.5f, (ay + by) * 0.5f, 0.5f);
            p.mNormal.set(nx, ny, 0.f);
            p.mNormal.normVec();
            p.mSide[0] = s0;
            p.mSide[1] = s1;
            p.mKind = static_cast<U8>(PortalKind::DOOR);
            in.mPortals.push_back(p);
        };
        door(2.f, 2.f, 6.f, 6.f, 1.f, -1.f, 1, 2);
        door(1.f, 6.f, 3.f, 7.5f, 0.2f, 1.f, 3, 4);
        door(1.5f, 6.2f, 2.5f, 7.f, 1.f, 0.f, 5, 6);
        const ChfPtr chf = floorOf(32, 32, [](S32, S32) { return AREA_CODE_BASE; });
        const std::vector<U32> gaps = gapsOf(*chf, [](S32, S32) { return 0; });
        TileStamp st;
        stampTile(*chf, 0, gaps, in, identityReal(), {}, StampParams(), st);
        U32 differ = 0, doors = 0;
        std::set<U16> seen;
        for (S32 z = 0; z < 32; ++z)
        {
            for (S32 x = 0; x < 32; ++x)
            {
                const F32 px = (z + 0.5f) * 0.25f, py = (x + 0.5f) * 0.25f;
                U16 want = 0;
                for (const StampPortal& p : in.mPortals)
                {
                    const F32 dx = p.mB.mV[VX] - p.mA.mV[VX], dy = p.mB.mV[VY] - p.mA.mV[VY], len = std::sqrt(dx * dx + dy * dy);
                    const F32 along = ((px - p.mA.mV[VX]) * dx + (py - p.mA.mV[VY]) * dy) / len;
                    const F32 across = (px - p.mCentre.mV[VX]) * p.mNormal.mV[VX] + (py - p.mCentre.mV[VY]) * p.mNormal.mV[VY];
                    if (along >= 0.f && along <= len && std::fabs(across) <= STAMP_THRESHOLD_M)
                    {
                        want = p.mSide[across < 0.f ? 0 : 1];
                        ++doors;
                        break;
                    }
                }
                const U16 got = st.mSpanComponent[spanAt(*chf, x, z)];
                differ += got != want ? 1 : 0;
                seen.insert(got);
            }
        }
        ensure_equals("every span as the rule reads it", differ, 0u);
        ensure_equals("every threshold span counted", st.mDoorSpans, doors);
        ensure("both sides of the slanted doors and the first of the overlapping pair", seen.count(1) && seen.count(2) && seen.count(3) && seen.count(4));
    }

    template<> template<>
    void sknavstamp_object::test<11>()
    {
        set_test_name("each portal edge carries its portal's index into the mesh's portals, the portal its key names");
        std::vector<DoorEntity> doors;
        const Stamped s(twoRoomsDoor(doors), doors);
        // A portal on another mesh, first in the list, is in no tile's stamp, so the tile's portals and the mesh's are numbered apart.
        auto rooms = std::make_shared<RoomsProduct>(*s.mRooms);
        Portal other = rooms->mPortals.front();
        other.mCrossMesh = true;
        rooms->mPortals.insert(rooms->mPortals.begin(), other);
        TileStampInput input;
        stampInput(*s.mResult.mAir, *s.mEnc->tile(TileKey{ 0, 0 }), *rooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), input);
        TileBinding b;
        bindStamp(s.mPolys.mStamp, input, s.mEnc->tile(TileKey{ 0, 0 }), *rooms, b);
        ensure("portal edges", !b.mPortalKeys.empty());
        ensure_equals("an index per edge", b.mPortalIndex.size(), b.mPortalKeys.size());
        for (size_t e = 0; e < b.mPortalIndex.size(); ++e)
        {
            const U32 i = b.mPortalIndex[e];
            ensure("an index past the other mesh's", i > 0 && i < rooms->mPortals.size());
            const Portal& p = rooms->mPortals[i];
            ensure("the keyed portal", portalKey(rooms->mSpaces[p.mSpaces[0]].mId, rooms->mSpaces[p.mSpaces[1]].mId, static_cast<U8>(p.mKind), p.mCentre) ==
                                           b.mPortalKeys[e]);
        }
    }

    template<> template<>
    void sknavstamp_object::test<12>()
    {
        set_test_name("an indoor floor under a roof keeps its space when Recast retriangulates its poly across regions");
        // Two storeys of 8 by 8 cells: a room's floor (top 4) in space 0, a roof (top 24) in none, outside every region.
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        const F32 cs = 0.25f, bmin[3] = { 0.f, 0.f, 0.f }, bmax[3] = { 2.f, 10.f, 2.f };
        ensure("heightfield", rcCreateHeightfield(&ctx, *hf, 8, 8, bmin, bmax, cs, cs));
        for (S32 z = 0; z < 8; ++z)
        {
            for (S32 x = 0; x < 8; ++x)
            {
                rcAddSpan(&ctx, *hf, x, z, 0, 4, AREA_CODE_BASE, 1);
                rcAddSpan(&ctx, *hf, x, z, 20, 24, AREA_CODE_BASE, 1);
            }
        }
        rcCompactHeightfield* raw = rcAllocCompactHeightfield();
        ensure("compact", rcBuildCompactHeightfield(&ctx, 8, 1, *hf, *raw));
        rcFreeHeightField(hf);
        const ChfPtr chf(raw);
        std::vector<U32> gaps(chf->spanCount, SPAN_NO_GAP);
        TileStamp st;
        st.mSpanComponent.assign(chf->spanCount, STAMP_NONE);
        st.mComponentColour = { 1 };
        for (S32 i = 0; i < chf->spanCount; ++i)
        {
            const bool floor = chf->spans[i].y < 10;
            chf->spans[i].reg = floor ? 1 : 0;
            gaps[i] = floor ? 0u : 1u;
            st.mSpanComponent[i] = floor ? 0 : STAMP_NONE;
        }
        TileStampInput in;
        in.mGapComponent = { 0, STAMP_NONE };
        in.mComponentSpace = { 7 };

        // One quad over the floor, its region RC_MULTIPLE_REGS as Recast leaves a poly retriangulated across regions.
        rcPolyMesh* pm = rcAllocPolyMesh();
        pm->nvp = 6;
        pm->nverts = 4;
        pm->npolys = 1;
        pm->maxpolys = 1;
        pm->cs = cs;
        pm->ch = cs;
        rcVcopy(pm->bmin, bmin);
        rcVcopy(pm->bmax, bmax);
        pm->verts = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * 12, RC_ALLOC_PERM));
        const unsigned short verts[12] = { 0, 4, 0, 0, 4, 8, 8, 4, 8, 8, 4, 0 };
        std::copy(verts, verts + 12, pm->verts);
        pm->polys = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * 12, RC_ALLOC_PERM));
        std::fill(pm->polys, pm->polys + 12, RC_MESH_NULL_IDX);
        for (unsigned short k = 0; k < 4; ++k)
        {
            pm->polys[k] = k;
        }
        pm->regs = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short), RC_ALLOC_PERM));
        pm->regs[0] = RC_MULTIPLE_REGS;
        pm->flags = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short), RC_ALLOC_PERM));
        pm->flags[0] = 1;
        pm->areas = static_cast<unsigned char*>(rcAlloc(1, RC_ALLOC_PERM));
        pm->areas[0] = AREA_CODE_BASE;
        TileStampGeom geom;
        stampGeometry(*pm, *chf, 0, gaps, in, st, geom);
        rcFreePolyMesh(pm);
        ensure_equals("the floor's poly is in the room's space", geom.mPolyComponent[0], 0);
        ensure_equals("and stands in the floor's gap, not the roof's", geom.mPolyGap[0], 0u);
    }

    template<> template<>
    void sknavstamp_object::test<13>()
    {
        set_test_name("a poly in no space that shares an edge with a spaced poly takes its neighbour's space");
        const ChfPtr chf = floorOf(16, 8, [](S32, S32) { return AREA_CODE_BASE; });
        std::vector<U32> gaps(chf->spanCount, 0u);
        TileStamp st;
        st.mSpanComponent.assign(chf->spanCount, STAMP_NONE);
        st.mComponentColour = { 1 };
        for (S32 z = 0; z < 8; ++z)
        {
            for (S32 x = 0; x < 16; ++x)
            {
                const S32 i = spanAt(*chf, x, z);
                chf->spans[i].reg = 1;
                st.mSpanComponent[i] = x < 8 ? 0 : STAMP_NONE;
            }
        }
        TileStampInput in;
        in.mGapComponent = { 0 };
        in.mComponentSpace = { 3 };
        // Two quads, west over the spaced half, east over the unspaced half, sharing the edge at x = 8.
        rcPolyMesh* pm = rcAllocPolyMesh();
        pm->nvp = 6;
        pm->nverts = 6;
        pm->npolys = 2;
        pm->maxpolys = 2;
        pm->cs = 0.25f;
        pm->ch = 0.25f;
        rcVcopy(pm->bmin, chf->bmin);
        rcVcopy(pm->bmax, chf->bmax);
        const unsigned short verts[18] = { 0, 4, 0, 0, 4, 8, 8, 4, 8, 8, 4, 0, 16, 4, 8, 16, 4, 0 };
        pm->verts = static_cast<unsigned short*>(rcAlloc(sizeof(verts), RC_ALLOC_PERM));
        std::copy(verts, verts + 18, pm->verts);
        pm->polys = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * 24, RC_ALLOC_PERM));
        std::fill(pm->polys, pm->polys + 24, RC_MESH_NULL_IDX);
        const unsigned short a[4] = { 0, 1, 2, 3 }, b[4] = { 3, 2, 4, 5 };
        for (S32 k = 0; k < 4; ++k)
        {
            pm->polys[k] = a[k];
            pm->polys[12 + k] = b[k];
        }
        pm->polys[6 + 2] = 1;   // west's edge 2-3 is east's
        pm->polys[12 + 6 + 0] = 0;
        pm->regs = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * 2, RC_ALLOC_PERM));
        pm->regs[0] = pm->regs[1] = 1;
        pm->flags = static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * 2, RC_ALLOC_PERM));
        pm->flags[0] = pm->flags[1] = 1;
        pm->areas = static_cast<unsigned char*>(rcAlloc(2, RC_ALLOC_PERM));
        pm->areas[0] = pm->areas[1] = AREA_CODE_BASE;
        TileStampGeom geom;
        stampGeometry(*pm, *chf, 0, gaps, in, st, geom);
        rcFreePolyMesh(pm);
        ensure_equals("the west poly is spaced", geom.mPolyComponent[0], 0);
        ensure_equals("the east poly took its neighbour's space", geom.mPolyComponent[1], 0);
        ensure_equals("one joined", geom.mJoinedPolys, 1u);
    }
}
