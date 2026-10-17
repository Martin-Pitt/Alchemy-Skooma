/**
 * @file sknavenclosurerepair_test.cpp
 * @brief Tests for the enclosure repair: a pass that mends the last answers gives what a pass from nothing gives.
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

#include "sknavtestscene.h"

#include "llstring.h"

#include <cstdio>
#include <iostream>
#include <sstream>

namespace
{
    using namespace sknav_test;

    /// Every answer a reader can see, floats to the bit.
    std::string dump(const EnclosureProduct& p)
    {
        std::ostringstream o;
        o << std::hexfloat;
        o << "G" << p.mGapCount << ',' << p.mUnknownGaps << ',' << p.mArea << ',' << p.mUnknownArea << ',' << p.mSealedGaps << ',' << p.mSummary << '\n';
        for (const TileEnclosure& t : p.mTiles)
        {
            o << "T" << t.mKey.mX << ',' << t.mKey.mY << ',' << (t.mAir ? 1 : 0) << ',' << t.mWidth << ',' << t.mHeight << ',' << t.mCell << ','
              << t.mUnit << ':' << t.mOrigin.mV[0] << ',' << t.mOrigin.mV[1] << ',' << t.mOrigin.mV[2] << '\n';
            for (size_t g = 0; g < t.mGaps.size(); ++g)
            {
                const GapEnclosure& e = t.mGaps[g];
                o << g << ':' << e.mSpace << ',' << S32(e.mS) << ',' << S32(e.mE) << ',' << S32(e.mD) << ',' << S32(e.mBucket) << ',' << S32(e.mFlags)
                  << ',' << S32(e.mCover) << ',' << S32(e.mOpen) << '\n';
            }
            const CoarseAir& c = t.mCoarse;
            o << "C" << c.mCells << ',' << c.mWidth << ',' << c.mHeight << ';';
            for (U32 s : c.mStart)
            {
                o << s << ',';
            }
            o << '\n';
            for (const CoarseGap& g : c.mGaps)
            {
                o << g.mFloor << ',' << g.mCeiling << ',' << g.mSpace << ',' << S32(g.mS) << ',' << S32(g.mE) << ',' << S32(g.mD) << ',' << S32(g.mFlags) << ','
                  << S32(g.mOpen) << ',' << S32(g.mBucket) << '\n';
            }
        }
        for (U32 id : p.mSpaceIds)
        {
            o << 'i' << id;
        }
        return o.str();
    }

    /// The first line two dumps differ on, for a message that says where.
    std::string firstDifference(const std::string& a, const std::string& b)
    {
        std::istringstream sa(a), sb(b);
        std::string la, lb;
        for (S32 line = 1;; ++line)
        {
            const bool ga = static_cast<bool>(std::getline(sa, la)), gb = static_cast<bool>(std::getline(sb, lb));
            if (!ga && !gb)
            {
                return "";
            }
            if (!ga || !gb || la != lb)
            {
                return llformat("line %d: repaired '%s' vs full '%s'", line, ga ? la.c_str() : "<end>", gb ? lb.c_str() : "<end>");
            }
        }
    }

    struct Run
    {
        EnclosureProductPtr mProduct;
        bool mRepaired = false;
        U32 mAnswered = 0;
        bool mLayoutMoved = true;
        F32 mStageMs[EnclosurePass::NUM_STAGES] = {};
        F32 mMs = 0.f;
    };

    Run run(const std::shared_ptr<const EnclosureInput>& in)
    {
        EnclosurePass pass(in);
        while (!pass.step(1u << 30))
        {
        }
        Run r;
        r.mProduct = pass.product();
        r.mRepaired = pass.repaired();
        r.mAnswered = pass.tilesAnswered();
        r.mLayoutMoved = pass.layoutMoved();
        for (S32 s = 0; s < EnclosurePass::NUM_STAGES; ++s)
        {
            r.mStageMs[s] = pass.stageMillis(s);
            r.mMs += r.mStageMs[s];
        }
        return r;
    }

    /// The pass from nothing over the same input: the same last answers, for their buckets, without the memo.
    Run full(const std::shared_ptr<const EnclosureInput>& in)
    {
        auto copy = std::make_shared<EnclosureInput>(*in);
        if (copy->mPrevious)
        {
            auto previous = std::make_shared<EnclosureProduct>(*copy->mPrevious);
            previous->mMemo.reset();
            copy->mPrevious = previous;
        }
        return run(copy);
    }

    /// Repair and full pass over one input must agree to the bit; the repair is what goes on.
    Run check(const std::string& what, const std::shared_ptr<const EnclosureInput>& in, bool expect_repair = true)
    {
        const Run r = run(in), f = full(in);
        INFO(what);
        CHECK(!f.mRepaired);
        CHECK(r.mRepaired == expect_repair);
        const std::string dr = dump(*r.mProduct), df = dump(*f.mProduct);
        INFO(firstDifference(dr, df));
        CHECK(dr == df);
        return r;
    }

    /// The input with some tiles' air built again from a scene, the rest's air shared, on the last answers.
    std::shared_ptr<EnclosureInput> edited(const EnclosureInput& from, const Scene& s, const std::vector<TileKey>& tiles, const EnclosureProductPtr& previous,
                                           bool sky)
    {
        auto in = std::make_shared<EnclosureInput>(from);
        in->mPrevious = previous;
        in->mGeneration = from.mGeneration + 1;
        for (const TileKey& key : tiles)
        {
            bool found = false;
            for (auto it = in->mTiles.begin(); it != in->mTiles.end(); ++it)
            {
                if (it->mKey == key)
                {
                    it->mAir = s.tile(key.mX, key.mY);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                EnclosureInput::Tile t;
                t.mKey = key;
                t.mAir = s.tile(key.mX, key.mY);
                in->mTiles.push_back(t);
            }
        }
        if (sky)
        {
            s.addSky(*in);
        }
        return in;
    }

    TileKey key(S32 x, S32 y)
    {
        TileKey k;
        k.mX = x;
        k.mY = y;
        return k;
    }

    /// Two rooms with a door between them and one out, across the seam of two tiles.
    Scene building()
    {
        Scene s = twoRooms();
        s.wall(9.875f, 2.25f, 10.125f, 5.f);
        s.wall(9.875f, 6.f, 10.125f, 9.75f);
        s.box(9.875f, 5.f, 10.125f, 6.f, 2.2f, 3.f, false);
        // In the east room a desk, whose air is carried from the floor around it, and a shut cabinet on legs, whose air is the floor's below it.
        s.box(12.f, 3.5f, 13.5f, 4.5f, 0.f, 0.8f, false);
        s.box(13.f, 7.f, 14.f, 8.f, 1.f, 1.125f, false);
        s.box(13.f, 7.f, 13.25f, 8.f, 1.f, 2.125f, false);
        s.box(13.75f, 7.f, 14.f, 8.f, 1.f, 2.125f, false);
        s.box(13.f, 7.f, 14.f, 7.25f, 1.f, 2.125f, false);
        s.box(13.f, 7.75f, 14.f, 8.f, 1.f, 2.125f, false);
        s.box(13.f, 7.f, 14.f, 8.f, 2.f, 2.125f, false);
        s.mShift = 24.f;
        return s;
    }

    void editSequence(bool sky)
    {
        const std::string tag = sky ? "with sky: " : "no sky: ";
        Scene s = building();
        auto in = s.input(2);
        if (sky)
        {
            s.addSky(*in);
        }
        Run r = check(tag + "first publication", in, false);

        // The way out shut: the building is sealed.
        Scene shut = s;
        shut.box(2.f, 5.f, 2.25f, 6.f, 0.f, 2.2f, false);
        in = edited(*in, shut, { key(0, 0) }, r.mProduct, sky);
        r = check(tag + "door shut", in);
        CHECK(r.mLayoutMoved);

        // And open again.
        in = edited(*in, s, { key(0, 0) }, r.mProduct, sky);
        r = check(tag + "door open", in);

        // The door between the rooms shut, in the other tile.
        Scene inner = s;
        inner.box(9.875f, 5.f, 10.125f, 6.f, 0.f, 2.2f, false);
        in = edited(*in, inner, { key(1, 0) }, r.mProduct, sky);
        r = check(tag + "inner door shut", in);

        // The wall between the rooms gone.
        Scene open = twoRooms();
        open.mShift = 24.f;
        in = edited(*in, open, { key(1, 0) }, r.mProduct, sky);
        r = check(tag + "wall removed", in);

        // Nothing moved: nothing is answered again, and the layout stands.
        {
            auto same = std::make_shared<EnclosureInput>(*in);
            same->mPrevious = r.mProduct;
            const Run again = check(tag + "nothing moved", same);
            CHECK(again.mAnswered == 0u);
            CHECK(!again.mLayoutMoved);
            CHECK(dump(*again.mProduct) == dump(*r.mProduct));
        }

        // Tile 1's detail evicted: its coarse answers stand without it.
        {
            auto evicted = std::make_shared<EnclosureInput>(*in);
            evicted->mPrevious = r.mProduct;
            const TileEnclosure* t1 = r.mProduct->tile(key(1, 0));
            REQUIRE(t1 != nullptr);
            evicted->mKept.push_back(*t1);
            evicted->mKept.back().mAir.reset();
            evicted->mKept.back().mGaps.clear();
            evicted->mTiles.erase(std::remove_if(evicted->mTiles.begin(), evicted->mTiles.end(),
                                                 [](const EnclosureInput::Tile& t) { return t.mKey == key(1, 0); }),
                                  evicted->mTiles.end());
            r = check(tag + "tile evicted", evicted);
            // And back, as new air.
            in = edited(*evicted, open, { key(1, 0) }, r.mProduct, sky);
            in->mKept.clear();
            r = check(tag + "tile back", in);
        }

        // The mesh grows a tile.
        in = edited(*in, open, { key(0, 1) }, r.mProduct, sky);
        r = check(tag + "tile added", in);

        // A room sealed with only a window, whole in tile 0.
        Scene windowed = open;
        windowed.box(2.f, 5.f, 2.25f, 6.f, 0.f, 1.f, false);
        windowed.box(2.f, 5.f, 2.25f, 6.f, 2.f, 2.2f, false);
        in = edited(*in, windowed, { key(0, 0) }, r.mProduct, sky);
        r = check(tag + "room sealed but for a window", in);

        // A second way out, east in tile 1, then the west one shut and opened:
        // what the west door gave is lost and found again from the east.
        Scene east = s;
        east.mSolids[2].mY1 = 5.f;
        east.wall(17.75f, 6.f, 18.f, 10.f);
        east.box(17.75f, 5.f, 18.f, 6.f, 2.2f, 3.f, false);
        in = edited(*in, east, { key(0, 0), key(1, 0) }, r.mProduct, sky);
        r = check(tag + "east door made", in);
        Scene east_shut = east;
        east_shut.box(2.f, 5.f, 2.25f, 6.f, 0.f, 2.2f, false);
        in = edited(*in, east_shut, { key(0, 0) }, r.mProduct, sky);
        r = check(tag + "west door shut beside an east one", in);
        in = edited(*in, east, { key(0, 0) }, r.mProduct, sky);
        r = check(tag + "west door open beside an east one", in);
    }

    TEST_CASE("sknavenclosurerepair: doors walls and tiles changing one at a time repair to what a full pass gives without a sky map", "[sknavenclosurerepair]")
    {
        editSequence(false);
    }

    TEST_CASE("sknavenclosurerepair: the same changes repair to what a full pass gives under a sky map", "[sknavenclosurerepair]")
    {
        editSequence(true);
    }

    TEST_CASE("sknavenclosurerepair: a neighbouring mesh's change repairs a mesh whose air stands to what a full pass gives", "[sknavenclosurerepair]")
    {
        // Mesh A is the building; mesh B a tile east of it, whose roof shades A's edge once it is built.
        Scene a = building();
        Scene b;
        b.mGround[0] = 64.f;
        Scene roofed = b;
        roofed.box(64.f, 0.f, 72.f, 32.f, 3.f, 3.25f);
        auto in_b = std::make_shared<EnclosureInput>();
        in_b->mTiles.push_back({ key(2, 0), b.tile(2, 0), 0 });
        const Run b0 = run(in_b);

        auto in_a = a.input(2);
        in_a->mNeighbours.push_back({ LLVector3d(), b0.mProduct });
        Run ra = check("first publication beside B", in_a, false);

        auto in_b1 = edited(*in_b, roofed, { key(2, 0) }, b0.mProduct, false);
        const Run b1 = run(in_b1);
        CHECK(b1.mProduct->mSummary != b0.mProduct->mSummary);

        auto in_a1 = std::make_shared<EnclosureInput>(*in_a);
        in_a1->mPrevious = ra.mProduct;
        in_a1->mNeighbours[0].mProduct = b1.mProduct;
        ra = check("B roofed", in_a1);

        // B gone: A's edge opens again.
        auto in_a2 = std::make_shared<EnclosureInput>(*in_a1);
        in_a2->mPrevious = ra.mProduct;
        in_a2->mNeighbours.clear();
        check("B gone", in_a2);
    }

    TEST_CASE("sknavenclosurerepair: a block of rooms with one door moved repairs exactly and answers fewer tiles", "[sknavenclosurerepair]")
    {
        // Six by six rooms of 8 m over three by three tiles, each with a door south.
        Scene s;
        for (S32 j = 0; j < 6; ++j)
        {
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 x0 = 4.f + i * 14.f, y0 = 4.f + j * 14.f, x1 = x0 + 8.f, y1 = y0 + 8.f;
                s.wall(x0, y0, x0 + 3.5f, y0 + 0.25f);
                s.wall(x0 + 4.5f, y0, x1, y0 + 0.25f);
                s.box(x0 + 3.5f, y0, x0 + 4.5f, y0 + 0.25f, 2.2f, 3.f, false);
                s.wall(x0, y1 - 0.25f, x1, y1);
                s.wall(x0, y0, x0 + 0.25f, y1);
                s.wall(x1 - 0.25f, y0, x1, y1);
                s.box(x0, y0, x1, y1, 3.f, 3.25f);
            }
        }
        auto in = s.input(3, 3);
        s.addSky(*in);
        const Run first = check("block: first publication", in, false);

        // The door of the room at (2, 2), in tile (1, 1), shut and opened again.
        Scene shut = s;
        shut.box(4.f + 2 * 14.f + 3.5f, 4.f + 2 * 14.f, 4.f + 2 * 14.f + 4.5f, 4.f + 2 * 14.f + 0.25f, 0.f, 2.2f, false);
        auto in_shut = edited(*in, shut, { key(1, 1) }, first.mProduct, false);
        in_shut->mSky = in->mSky;
        const Run full_shut = full(in_shut);
        const Run shut_run = check("block: door shut", in_shut);
        auto in_open = edited(*in_shut, s, { key(1, 1) }, shut_run.mProduct, false);
        in_open->mSky = in->mSky;
        const Run full_open = full(in_open);
        const Run open_run = check("block: door open", in_open);

        std::ostringstream o;
        auto line = [&o](const char* what, const Run& r) {
            o << what << ": " << llformat("%.2f", r.mMs) << " ms (";
            for (S32 k = 0; k < EnclosurePass::NUM_STAGES; ++k)
            {
                o << EnclosurePass::stageName(k) << ' ' << llformat("%.2f", r.mStageMs[k]) << (k + 1 < EnclosurePass::NUM_STAGES ? ", " : "");
            }
            o << "), tiles answered " << r.mAnswered << ", layout moved " << r.mLayoutMoved << '\n';
        };
        line("first publication", first);
        line("door shut, full", full_shut);
        line("door shut, repair", shut_run);
        line("door open, full", full_open);
        line("door open, repair", open_run);
        std::cout << o.str();
        const std::string out = LLStringUtil::getenv("SKNAV_REPAIR_TIMES");
        if (!out.empty())
        {
            if (FILE* f = fopen(out.c_str(), "a"))
            {
                fputs(o.str().c_str(), f);
                fclose(f);
            }
        }
        CHECK(shut_run.mAnswered < 9);
        CHECK(open_run.mAnswered < 9);
    }
}
