/**
 * @file sknavresidency_test.cpp
 * @brief Tests for settled air on disk: the spill round trip, the settle gate and the coarse fallback.
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
#include "../sknavenclosure.h"
#include "../sknavresidency.h"
#include "../sknavskymap.h"
#include "../sknavspaces.h"
#include "../sknavstamp.h"
#include "../sknavtilecache.h"

#include "Recast.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace
{
    using namespace sknav;

    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
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

    /// A roofed room with a doorway on open ground, so the tile has indoor and outdoor air.
    TileBuildInput roomTile()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 8.f;
        in.mCell = 0.25f;
        in.mAir = true;
        in.mTerrain.mOrigin.set(-6.f, -6.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 45;
        in.mTerrain.mZ.assign(45 * 45, 0.f);
        in.mTerrain.mWater = -10.f;
        in.mRecords = {
            box(8.f, 8.f, 0.f, 20.f, 8.5f, 3.f),     // south wall
            box(8.f, 19.5f, 0.f, 20.f, 20.f, 3.f),   // north wall
            box(8.f, 8.f, 0.f, 8.5f, 20.f, 3.f),     // west wall
            box(19.5f, 8.f, 0.f, 20.f, 12.f, 3.f),   // east wall, a doorway from 12 to 13.5
            box(19.5f, 13.5f, 0.f, 20.f, 20.f, 3.f),
            box(8.f, 8.f, 3.f, 20.f, 20.f, 3.3f),    // roof
        };
        return in;
    }

    struct Passes
    {
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;
        TileStampInput mStampInput;
        TileBuildResult mPolys;
    };

    /// Enclosure, spaces, stamp input and stamped polys over one tile's air and raster.
    Passes run(const TileBuildInput& in, const TileBuildResult& raster_result, const TileAirPtr& air, const TileRaster& raster)
    {
        Passes p;
        auto enc = std::make_shared<EnclosureInput>();
        enc->mGeneration = 1;
        enc->mParams.mWalkHeadM = 2.f;
        enc->mTiles.push_back({ TileKey{ 0, 0 }, air, 1 });
        auto sky = std::make_shared<SkyMap>();
        sky->set(1, LLVector3d(air->mOrigin), *air);
        enc->mSky = sky;
        EnclosurePass pass(enc);
        while (!pass.step(1u << 30))
        {
        }
        p.mEnc = pass.takeProduct();
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mGeneration = 1;
        p.mRooms = computeSpaces(rooms, p.mEnc);
        stampInput(*air, *p.mEnc->tile(TileKey{ 0, 0 }), *p.mRooms, LLVector3(0.f, 0.f, -1.f), LLVector3(32.f, 32.f, 8.f), p.mStampInput);
        TileStamp stamp;
        stampTile(*raster.mChf, borderCells(in.mParams[0].mRadius, in.mCell), raster.mSpanGap, p.mStampInput, raster.mReal, raster.mFlights,
                  StampParams(), stamp);
        p.mPolys = raster_result;
        polygoniseTile(in, raster, p.mPolys, nullptr, &p.mStampInput, &stamp);
        return p;
    }

    bool sameGaps(const EnclosureProduct& a, const EnclosureProduct& b)
    {
        if (a.mTiles.size() != b.mTiles.size())
        {
            return false;
        }
        for (size_t t = 0; t < a.mTiles.size(); ++t)
        {
            const auto& x = a.mTiles[t].mGaps;
            const auto& y = b.mTiles[t].mGaps;
            if (x.size() != y.size())
            {
                return false;
            }
            for (size_t i = 0; i < x.size(); ++i)
            {
                if (x[i].mSpace != y[i].mSpace || x[i].mS != y[i].mS || x[i].mE != y[i].mE || x[i].mD != y[i].mD || x[i].mBucket != y[i].mBucket ||
                    x[i].mFlags != y[i].mFlags || x[i].mCover != y[i].mCover || x[i].mOpen != y[i].mOpen)
                {
                    return false;
                }
            }
        }
        return a.mSummary == b.mSummary;
    }
}

namespace tut
{
    struct sknavresidency_data
    {
    };
    typedef test_group<sknavresidency_data> sknavresidency_group;
    typedef sknavresidency_group::object sknavresidency_object;
    tut::sknavresidency_group sknavresidency_tests("sknavresidency");

    template<> template<>
    void sknavresidency_object::test<1>()
    {
        set_test_name("evicted then reloaded, a tile's air and raster are the same bytes, and its enclosure, spaces and stamped polys the same");
        const TileBuildInput in = roomTile();
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised: " + r.mError, raster && raster->mChf && r.mAir && !r.mAir->mLeaves.empty());
        const U64 key = spillKey(0x5eedull);
        ensure("a key of its own", key != 0x5eedull && key != stampedKey(0x5eedull, 0));
        const std::vector<U8> bytes = packSpill(r.mAir.get(), raster.get(), key);
        TileAirPtr air;
        TileRasterPtr back;
        ensure("unpacks", unpackSpill(bytes.data(), bytes.size(), key, air, back));
        ensure("both came back", air && back && back->mChf);
        ensure("the raster points at the air read with it", back->mAir.lock() == air);
        ensure("repacks to the same bytes", packSpill(air.get(), back.get(), key) == bytes);
        ensure("no bigger than it was", air->bytes() <= r.mAir->bytes() && back->bytes() <= raster->bytes());
        // Field by field, so a field the disk form leaves out shows here.
        const TileAir& x = *r.mAir;
        ensure("the same leaves", x.mLeaves.size() == air->mLeaves.size() &&
                                      std::equal(x.mLeaves.begin(), x.mLeaves.end(), air->mLeaves.begin(), [](const AirLeaf& p, const AirLeaf& q) {
                                          return p.mX == q.mX && p.mY == q.mY && p.mLevel == q.mLevel && p.mGapCount == q.mGapCount &&
                                                 p.mParent == q.mParent && p.mFirstGap == q.mFirstGap;
                                      }));
        ensure("the same gaps", x.mGaps.size() == air->mGaps.size() &&
                                    std::equal(x.mGaps.begin(), x.mGaps.end(), air->mGaps.begin(), [](const AirGap& p, const AirGap& q) {
                                        return p.mFloor == q.mFloor && p.mCeiling == q.mCeiling && p.mFlags == q.mFlags && p.mClear == q.mClear &&
                                               p.mSeed == q.mSeed;
                                    }));
        ensure("the same face offsets and tops", x.mFaceOffsets.size() == air->mFaceOffsets.size() && x.mTop.size() == air->mTop.size() &&
                                                     x.mTopStart == air->mTopStart && x.mOrigin == air->mOrigin);
        ensure("the same span gaps and codes", back->mSpanGap == raster->mSpanGap && back->mReal == raster->mReal &&
                                                   back->mStanding == raster->mStanding && back->mFlights.size() == raster->mFlights.size());
        ensure("the same spans", back->mChf->spanCount == raster->mChf->spanCount &&
                                     std::memcmp(back->mChf->spans, raster->mChf->spans, sizeof(rcCompactSpan) * raster->mChf->spanCount) == 0);
        // The air in a tile product, which carries every field the cache keeps, is the same too.
        TileBuildResult a = r, b = r;
        b.mAir = air;
        ensure("the same in a product", packTileProduct(a, 1) == packTileProduct(b, 1));

        const Passes before = run(in, r, r.mAir, *raster);
        const Passes after = run(in, r, air, *back);
        ensure("the same enclosure", sameGaps(*before.mEnc, *after.mEnc));
        ensure_equals("the same spaces", after.mRooms->mSpaces.size(), before.mRooms->mSpaces.size());
        ensure("the same space ids", after.mEnc->mSpaceIds == before.mEnc->mSpaceIds);
        ensure("the same gap components", after.mStampInput.mGapComponent == before.mStampInput.mGapComponent);
        ensure("the same stamp pattern", after.mStampInput.mPattern == before.mStampInput.mPattern);
        ensure("stamped polys", !before.mPolys.mData[0].empty() && before.mPolys.mStamp.mStamped);
        ensure("the same Detour bytes", after.mPolys.mData[0] == before.mPolys.mData[0]);
        ensure("the same poly components", after.mPolys.mStamp.mPolyComponent == before.mPolys.mStamp.mPolyComponent);
    }

    template<> template<>
    void sknavresidency_object::test<2>()
    {
        set_test_name("a spill that is damaged, short, foreign or stale is refused, and leaves what it was given alone");
        const TileBuildInput in = roomTile();
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised", raster && r.mAir);
        const U64 key = spillKey(7);
        std::vector<U8> bytes = packSpill(r.mAir.get(), raster.get(), key);
        TileAirPtr air = r.mAir;
        TileRasterPtr held = raster;
        ensure("another key", !unpackSpill(bytes.data(), bytes.size(), spillKey(8), air, held));
        ensure("short", !unpackSpill(bytes.data(), 12, key, air, held));
        std::vector<U8> damaged = bytes;
        damaged[damaged.size() / 2] ^= 0x40;
        ensure("damaged", !unpackSpill(damaged.data(), damaged.size(), key, air, held));
        std::vector<U8> stale = bytes;
        stale[4] ^= 1;  // the version
        ensure("stale", !unpackSpill(stale.data(), stale.size(), key, air, held));
        ensure("untouched", air == r.mAir && held == raster);
        // Air alone, the raster having gone for the budget, comes back alone.
        bytes = packSpill(r.mAir.get(), nullptr, key);
        TileAirPtr only;
        TileRasterPtr none = raster;
        ensure("air alone", unpackSpill(bytes.data(), bytes.size(), key, only, none) && only && !none);
    }

    template<> template<>
    void sknavresidency_object::test<3>()
    {
        set_test_name("nothing settles while a product that reads the air is pending, and a new product joins the gate");
        SettleGate gate;
        LLUUID a, b;
        a.generate();
        b.generate();
        std::set<LLUUID> spaces_pending = { a }, stamp_pending, probes_pending = { b };
        gate.add("spaces", [&](const LLUUID& m) { return spaces_pending.count(m) > 0; });
        gate.add("stamp", [&](const LLUUID& m) { return stamp_pending.count(m) > 0; });
        gate.add("room probes", [&](const LLUUID& m) { return probes_pending.count(m) > 0; }, false);
        ensure("a waits on its spaces", !gate.settled(a) && std::string(gate.pending(a)) == "spaces");
        ensure("b waits on its probes", !gate.settled(b) && std::string(gate.pending(b)) == "room probes");
        ensure("which read no air: b's detail stays on disk", !gate.needsDetail(b));
        ensure("a's spaces bring its detail back", gate.needsDetail(a));
        spaces_pending.clear();
        stamp_pending.insert(a);
        ensure("then on its stamp", std::string(gate.pending(a)) == "stamp");
        stamp_pending.clear();
        ensure("a settles", gate.settled(a));
        // A later product, such as sound probes, holds every mesh it has not read.
        std::set<LLUUID> sound_done;
        gate.add("sound probes", [&](const LLUUID& m) { return sound_done.count(m) == 0; });
        ensure_equals("four products", gate.size(), size_t(4));
        ensure("a waits on it", !gate.settled(a) && std::string(gate.pending(a)) == "sound probes");
        sound_done.insert(a);
        ensure("a settles again", gate.settled(a));
        ensure("b still on its probes", std::string(gate.pending(b)) == "room probes");
    }

    template<> template<>
    void sknavresidency_object::test<4>()
    {
        set_test_name("with its detail on disk a tile answers from coarse air, and with it back answers as before");
        const TileBuildInput in = roomTile();
        TileBuildResult r;
        const TileRasterPtr raster = rasteriseTile(in, r);
        ensure("rasterised", raster && r.mAir);
        const Passes p = run(in, r, r.mAir, *raster);
        const EnclosureProductPtr full = p.mEnc;
        const RoomsProductPtr rooms = p.mRooms;
        const TileKey key{ 0, 0 };
        const EnclosureProductPtr away = withTileAir(*full, { { key, nullptr } });
        const RoomsProductPtr rooms_away = withTileAir(*rooms, { { key, nullptr } });
        ensure("the landed products are untouched", full->tile(key)->mAir == r.mAir && rooms->tile(key)->mAir == r.mAir);
        ensure("the copies let the detail go", !away->tile(key)->mAir && !rooms_away->tile(key)->mAir);
        ensure("the spaces stay", rooms_away->mSpaces.size() == rooms->mSpaces.size() && rooms_away->mPortals.size() == rooms->mPortals.size());
        ensure("the per-gap answers stay", sameGaps(*away, *full));
        // Inside the room and out in the open: the coarse cell answers with its own bucket.
        U32 coarse_reads = 0;
        for (const LLVector3& at : { LLVector3(14.f, 14.f, 0.5f), LLVector3(3.f, 3.f, 0.5f), LLVector3(26.f, 26.f, 0.5f) })
        {
            Enclosure detail, coarse;
            ensure("detail answers", full->enclosureAt(at, detail));
            ensure("coarse answers", away->enclosureAt(at, coarse));
            ensure("the same bucket", coarse.mBucket == detail.mBucket);
            ++coarse_reads;
        }
        ensure_equals("every point read", coarse_reads, 3u);
        // The reloaded air under another pointer answers exactly as the first did.
        const std::vector<U8> bytes = packSpill(r.mAir.get(), nullptr, 1);
        TileAirPtr reloaded;
        TileRasterPtr none;
        ensure("reloaded", unpackSpill(bytes.data(), bytes.size(), 1, reloaded, none));
        const EnclosureProductPtr back = withTileAir(*away, { { key, reloaded } });
        ensure("pointed at the reloaded air", back->tile(key)->mAir == reloaded);
        for (F32 x = 1.f; x < 32.f; x += 2.5f)
        {
            for (F32 y = 1.f; y < 32.f; y += 2.5f)
            {
                Enclosure e0, e1;
                const LLVector3 at(x, y, 0.5f);
                const bool a = full->enclosureAt(at, e0), b = back->enclosureAt(at, e1);
                ensure("same reach", a == b);
                ensure("same answer", !a || (e0.mS == e1.mS && e0.mD == e1.mD && e0.mE == e1.mE && e0.mSpace == e1.mSpace && e0.mFloor == e1.mFloor));
            }
        }
    }
}
