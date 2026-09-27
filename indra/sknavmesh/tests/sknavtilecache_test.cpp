/**
 * @file sknavtilecache_test.cpp
 * @brief Tests for the tile product cache: content keys, the disk format and the eviction order.
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

#include "../sknavtilecache.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace
{
    using namespace sknav;

    RecordPtr box(U8 id, const LLVector3& centre, const LLVector3& half)
    {
        auto r = std::make_shared<Record>();
        r->mPart.mData[0] = id;
        r->mRoot = r->mPart;
        r->mShape = Shape::BOX;
        r->mOrigin.set(256000.0, 256000.0, 0.0);
        r->mCenter = centre;
        r->mAxes[0].set(1.f, 0.f, 0.f);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mHalf = half;
        r->mMin = r->mOrigin + LLVector3d(centre - half);
        r->mMax = r->mOrigin + LLVector3d(centre + half);
        return r;
    }

    RecordPtr soup(U8 id)
    {
        auto r = std::make_shared<Record>();
        r->mPart.mData[0] = id;
        r->mOrigin.set(256000.0, 256000.0, 0.0);
        auto tris = std::make_shared<std::vector<LLVector3>>();
        tris->push_back(LLVector3(1.f, 1.f, 20.f));
        tris->push_back(LLVector3(5.f, 1.f, 20.f));
        tris->push_back(LLVector3(1.f, 5.f, 20.f));
        r->mTriangles = tris;
        r->mMin = r->mOrigin + LLVector3d(1.0, 1.0, 20.0);
        r->mMax = r->mOrigin + LLVector3d(5.0, 5.0, 20.0);
        return r;
    }

    TileBuildInput input(const std::vector<RecordPtr>& records)
    {
        TileBuildInput in;
        in.mMeshOrigin.set(256000.0, 256000.0, 0.0);
        in.mTileX = 2;
        in.mTileY = 3;
        in.mZMin = 19.f;
        in.mZMax = 40.f;
        in.mRecords = records;
        in.mTerrain.mWidth = 2;
        in.mTerrain.mHeight = 2;
        in.mTerrain.mZ = { 20.f, 20.5f, 21.f, 21.25f };
        return in;
    }

    TileAir air()
    {
        TileAir a;
        a.mOrigin.set(64.f, 96.f, 19.f);
        a.mCell = a.mUnit = 0.125f;
        a.mWidth = 2;
        a.mHeight = 3;
        AirGap g;
        g.mFloor = 4;
        g.mCeiling = AIR_CEILING_OPEN;
        g.mFlags = AirGap::WALKABLE | AirGap::OPEN;
        g.mClear = 7;
        g.mSeed = 3;
        a.mGaps = { g, g };
        a.mLeaves.push_back({ 0, 0, 1, 2, 0 });
        for (S32 side = 0; side < 4; ++side)
        {
            const S32 n = side < 2 ? a.mHeight : a.mWidth;
            a.mSeams[side].mStart.assign(n + 1, 0);
            a.mSeams[side].mStart[n] = 1;
            a.mSeams[side].mGaps = { g };
        }
        a.mTopCell = 8;
        a.mTopWidth = 1;
        a.mTopHeight = 1;
        a.mTopStart = { 0, 1 };
        a.mTop = { { 0, 4 } };
        return a;
    }

    TileBuildResult result()
    {
        TileBuildResult r;
        r.mStatus = TileBuildResult::SUCCESS;
        r.mData[0] = { 1, 2, 3, 4, 5 };
        r.mPolys[0] = 7;
        r.mTriangles = 99;
        r.mMillis = 12.5f;
        r.mFaces.push_back({ LLVector3(1.f, 2.f, 3.f), LLVector3(1.f, 4.f, 6.f), 2, 1 });
        r.mSegments[1].push_back({ LLVector3(0.f, 0.f, 1.f), LLVector3(3.f, 0.f, 1.f), EdgeKind::LEDGE, -1.f, 2 });
        r.mLinks[0].push_back({ LLVector3(1.f, 1.f, 5.f), LLVector3(2.f, 1.f, 1.f), 4.f, LinkKind::DROP });
        r.mAir = std::make_shared<TileAir>(air());
        r.mAirBuilt = true;
        return r;
    }

    bool sameGaps(const std::vector<AirGap>& a, const std::vector<AirGap>& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const AirGap& x, const AirGap& y) {
                   return x.mFloor == y.mFloor && x.mCeiling == y.mCeiling && x.mFlags == y.mFlags && x.mClear == y.mClear && x.mSeed == y.mSeed;
               });
    }

    std::string scratch(const char* name)
    {
        const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p.string();
    }
}

namespace tut
{
    struct sknavtilecache_data
    {
    };
    typedef test_group<sknavtilecache_data> sknavtilecache_group;
    typedef sknavtilecache_group::object sknavtilecache_object;
    tut::sknavtilecache_group sknavtilecache_tests("sknavtilecache");

    template<> template<>
    void sknavtilecache_object::test<1>()
    {
        set_test_name("a product survives packing and unpacking whole");
        const TileBuildResult in = result();
        const std::vector<U8> bytes = packTileProduct(in, 42);
        TileBuildResult out;
        ensure("unpacks", unpackTileProduct(bytes.data(), bytes.size(), 42, out));
        ensure_equals("status", out.mStatus, TileBuildResult::SUCCESS);
        ensure("detour", out.mData[0] == in.mData[0] && out.mData[1].empty());
        ensure_equals("polys", out.mPolys[0], 7);
        ensure_equals("triangles", out.mTriangles, 99);
        ensure_equals("faces", out.mFaces.size(), size_t(1));
        ensure("face", out.mFaces[0].mMax == in.mFaces[0].mMax && out.mFaces[0].mSide == 2 && out.mFaces[0].mArea == 1);
        ensure_equals("segments", out.mSegments[1].size(), size_t(1));
        ensure("segment", out.mSegments[1][0].mKind == EdgeKind::LEDGE && out.mSegments[1][0].mValue == -1.f && out.mSegments[1][0].mB == in.mSegments[1][0].mB);
        ensure("link", out.mLinks[0].size() == 1 && out.mLinks[0][0].mTo == in.mLinks[0][0].mTo && out.mLinks[0][0].mHeight == 4.f);
        ensure("air built", out.mAirBuilt && out.mAir);
        const TileAir& a = *out.mAir;
        const TileAir& b = *in.mAir;
        ensure("air grid", a.mOrigin == b.mOrigin && a.mWidth == b.mWidth && a.mHeight == b.mHeight && a.mCell == b.mCell);
        ensure("air gaps", sameGaps(a.mGaps, b.mGaps));
        ensure("air leaves", a.mLeaves.size() == 1 && a.mLeaves[0].mLevel == 1 && a.mLeaves[0].mGapCount == 2);
        ensure("air seams", a.mSeams[3].mStart == b.mSeams[3].mStart && sameGaps(a.mSeams[3].mGaps, b.mSeams[3].mGaps));
        ensure("air tops", a.mTopStart == b.mTopStart && a.mTop.size() == 1 && a.mTop[0].mTop == 4);
        ensure("repacks to the same bytes", packTileProduct(out, 42) == bytes);
    }

    template<> template<>
    void sknavtilecache_object::test<2>()
    {
        set_test_name("a damaged, short, foreign or stale entry is refused");
        const std::vector<U8> bytes = packTileProduct(result(), 42);
        TileBuildResult out;
        out.mTriangles = -5;
        ensure("another key", !unpackTileProduct(bytes.data(), bytes.size(), 43, out));
        for (size_t i : { size_t(0), size_t(5), bytes.size() / 2, bytes.size() - 1 })
        {
            std::vector<U8> bad = bytes;
            bad[i] ^= 0x10;
            ensure("a flipped byte", !unpackTileProduct(bad.data(), bad.size(), 42, out));
        }
        ensure("short", !unpackTileProduct(bytes.data(), bytes.size() - 9, 42, out));
        ensure("empty", !unpackTileProduct(bytes.data(), 0, 42, out));
        ensure_equals("a refused entry leaves the output alone", out.mTriangles, -5);
    }

    template<> template<>
    void sknavtilecache_object::test<3>()
    {
        set_test_name("the key holds under reordered records and moves with the content");
        const std::vector<RecordPtr> recs = { box(1, LLVector3(10.f, 10.f, 21.f), LLVector3(2.f, 2.f, 1.f)),
                                              box(2, LLVector3(20.f, 12.f, 22.f), LLVector3(1.f, 3.f, 2.f)), soup(3),
                                              box(4, LLVector3(5.f, 25.f, 21.5f), LLVector3(0.5f, 0.5f, 0.5f)) };
        const U64 key = tileContentKey(input(recs));
        std::mt19937 rng(7);
        for (S32 i = 0; i < 10; ++i)
        {
            std::vector<RecordPtr> shuffled = recs;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            ensure_equals("reordered", tileContentKey(input(shuffled)), key);
        }
        // Copies of the same records, as a new session builds them.
        std::vector<RecordPtr> copies;
        for (const RecordPtr& r : recs)
        {
            copies.push_back(std::make_shared<Record>(*r));
        }
        ensure_equals("fresh copies", tileContentKey(input(copies)), key);

        std::vector<RecordPtr> moved = recs;
        moved[1] = box(2, LLVector3(20.05f, 12.f, 22.f), LLVector3(1.f, 3.f, 2.f));
        ensure("moved by 5 cm", tileContentKey(input(moved)) != key);
        std::vector<RecordPtr> jitter = recs;
        jitter[1] = box(2, LLVector3(20.001f, 12.f, 22.f), LLVector3(1.f, 3.f, 2.f));
        ensure_equals("moved by a millimetre", tileContentKey(input(jitter)), key);

        TileBuildInput land = input(recs);
        land.mTerrain.mZ[2] += 0.5f;
        ensure("land moved", tileContentKey(land) != key);
        TileBuildInput params = input(recs);
        params.mAir = true;
        ensure("air asked for", tileContentKey(params) != key);

        // A pair of equal parts must not cancel out, as xor would.
        const RecordPtr a = box(9, LLVector3(1.f, 1.f, 21.f), LLVector3(1.f, 1.f, 1.f));
        ensure("a pair is not nothing", tileContentKey(input({ a, a, recs[0] })) != tileContentKey(input({ recs[0] })));
    }

    template<> template<>
    void sknavtilecache_object::test<4>()
    {
        set_test_name("a placeholder's local id is not part of its key");
        RecordPtr a = box(0, LLVector3(10.f, 10.f, 21.f), LLVector3(2.f, 2.f, 1.f));
        auto b = std::make_shared<Record>(*a);
        auto c = std::make_shared<Record>(*a);
        const U32 tag = 0x534b4e56;
        const U32 local_b = 1234, local_c = 99887;
        memcpy(b->mPart.mData + 8, &local_b, sizeof(local_b));
        memcpy(b->mPart.mData + 12, &tag, sizeof(tag));
        memcpy(c->mPart.mData + 8, &local_c, sizeof(local_c));
        memcpy(c->mPart.mData + 12, &tag, sizeof(tag));
        ensure_equals("same box, another local id", recordContentKey(*b), recordContentKey(*c));
        auto d = std::make_shared<Record>(*a);
        d->mPart.mData[1] = 5;
        ensure("a real part id is", recordContentKey(*d) != recordContentKey(*a));
    }

    template<> template<>
    void sknavtilecache_object::test<5>()
    {
        set_test_name("eviction takes the least near first and never the protected");
        std::vector<EvictionCandidate> c(4);
        c[0] = { 100, 5.0, 300.0 };
        c[1] = { 100, 1.0, 400.0 };
        c[2] = { 100, 0.5, 100.0 };     // least near, but inside the protected distance
        c[3] = { 100, 3.0, 500.0 };
        const std::vector<size_t> out = chooseEvictions(c, 200, 192.0);
        ensure_equals("two go", out.size(), size_t(2));
        ensure_equals("first", out[0], size_t(1));
        ensure_equals("second", out[1], size_t(3));
        ensure("under budget, none", chooseEvictions(c, 400, 0.0).empty());
        ensure_equals("only protected ones left, the total stays over", chooseEvictions(c, 0, 1000.0).size(), size_t(0));

        // A tile sat by for a minute outlasts one passed for a second.
        F64 sat = 0.0, passed = 0.0;
        passed = accumulateNearness(passed, 0.0, 128.0, 1.0);
        for (S32 i = 0; i < 60; ++i)
        {
            sat = accumulateNearness(sat, 50.0, 128.0, 1.0);
            passed = accumulateNearness(passed, 800.0, 128.0, 1.0);
        }
        ensure("sat by outranks passed", sat > passed);
    }

    template<> template<>
    void sknavtilecache_object::test<6>()
    {
        set_test_name("the disk cache stores, reloads and prunes least recently used");
        const std::string dir = scratch("sknavtilecache_test");
        const std::vector<U8> blob(1000, 7);
        {
            TileDiskCache cache(dir, 2500);
            ensure("store 1", cache.store(1, blob));
            ensure("store 2", cache.store(2, blob));
            std::vector<U8> got;
            ensure("load 1", cache.load(1, got) && got == blob);     // 1 is now newer than 2
            ensure("store 3", cache.store(3, blob));
            ensure("2 pruned", !cache.contains(2));
            ensure("1 kept", cache.contains(1) && cache.contains(3));
            ensure_equals("bytes", cache.bytes(), U64(2000));
            cache.erase(3);
            ensure("erased", !cache.contains(3) && !cache.load(3, got));
        }
        TileDiskCache again(dir, 2500);
        ensure_equals("reindexed", again.entries(), size_t(1));
        std::vector<U8> got;
        ensure("reloaded", again.load(1, got) && got == blob);
        again.setCap(0);
        ensure_equals("cap to nothing", again.entries(), size_t(0));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    template<> template<>
    void sknavtilecache_object::test<7>()
    {
        set_test_name("an evicted tile keeps its solid tops and nothing else");
        const TileAir full = air();
        const TileAirPtr coarse = coarseAir(full);
        ensure("no leaves or gaps", coarse->mLeaves.empty() && coarse->mGaps.empty() && coarse->mSeams[0].mGaps.empty());
        ensure("tops", coarse->mTopStart == full.mTopStart && coarse->mTop.size() == full.mTop.size() && coarse->mTopCell == 8);
        ensure("grid", coarse->mOrigin == full.mOrigin && coarse->mWidth == full.mWidth);
    }
}
