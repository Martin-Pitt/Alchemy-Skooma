/**
 * @file skairraster_test.cpp
 * @brief The solid raster and the air read off it: floors, fills, bridges, the cull, the two views, and fixture capture.
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

#include "skairtestscene.h"

#include "../skaircensus.h"
#include "../skairdistil.h"
#include "../skairraster.h"
#include "../skairstats.h"
#include "skcensusrecord.h"

#include "llformat.h"
#include "Recast.h"

#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
    using namespace skworld;
    using namespace airscene;

    TileAirResult build(const TileInput& in, TileBuildStats* stats = nullptr, RasterCapture* capture = nullptr)
    {
        TileAirResult r;
        rasteriseTile(in, r, stats, capture);
        return r;
    }

    bool built(const TileAirResult& r)
    {
        return r.mStatus == TileAirResult::SUCCESS;
    }

    template<typename T> void putBytes(std::vector<U8>& out, const T& v)
    {
        const U8* p = reinterpret_cast<const U8*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }

    /// Everything a tile's air products are, as bytes: the packed air, then faces, ledges and run-off.
    std::vector<U8> productBytes(const TileAirResult& r)
    {
        std::vector<U8> out;
        if (r.mAir)
        {
            out = packTileAir(*r.mAir);
        }
        putBytes(out, static_cast<U32>(r.mFaces.size()));
        for (const WallFace& f : r.mFaces)
        {
            putBytes(out, f.mMin);
            putBytes(out, f.mMax);
            putBytes(out, f.mSide);
            putBytes(out, f.mArea);
        }
        putBytes(out, static_cast<U32>(r.mLedges.size()));
        for (const WallSegment& s : r.mLedges)
        {
            putBytes(out, s.mA);
            putBytes(out, s.mB);
            putBytes(out, static_cast<U8>(s.mKind));
            putBytes(out, s.mValue);
        }
        putBytes(out, static_cast<U32>(r.mRunoff.size()));
        for (const DropLink& l : r.mRunoff)
        {
            putBytes(out, l.mFrom);
            putBytes(out, l.mTo);
            putBytes(out, l.mHeight);
        }
        return out;
    }

    U64 hashBytes(const std::vector<U8>& bytes)
    {
        U64 h = SEED;
        for (U8 b : bytes)
        {
            h = mix(h, b);
        }
        return h;
    }

    // ---- fixture capture -------------------------------------------------

    /// A tile's fixture: tagged sections, the whole zlib-compressed behind a header.
    struct FixtureWriter
    {
        std::vector<U8> mBody;

        void section(const char tag[5], const std::vector<U8>& bytes)
        {
            mBody.insert(mBody.end(), tag, tag + 4);
            putBytes(mBody, static_cast<U32>(bytes.size()));
            mBody.insert(mBody.end(), bytes.begin(), bytes.end());
        }

        void text(const char tag[5], const std::string& s)
        {
            section(tag, std::vector<U8>(s.begin(), s.end()));
        }

        template<typename T> void array(const char tag[5], const std::vector<T>& v)
        {
            const U8* p = reinterpret_cast<const U8*>(v.data());
            section(tag, std::vector<U8>(p, p + v.size() * sizeof(T)));
        }

        /// Width, height, then each column's span count (U16) and the packed spans.
        void columns(const char tag[5], const RasterColumns& c)
        {
            std::vector<U8> out;
            putBytes(out, c.mWidth);
            putBytes(out, c.mHeight);
            for (size_t i = 0; i + 1 < c.mStart.size(); ++i)
            {
                putBytes(out, static_cast<U16>(c.mStart[i + 1] - c.mStart[i]));
            }
            const U8* p = reinterpret_cast<const U8*>(c.mSpans.data());
            out.insert(out.end(), p, p + c.mSpans.size() * sizeof(U32));
            section(tag, out);
        }

        /// The grid's frame, then each column's gap count (U16) and the gaps, 8 bytes each.
        void grid(const char tag[5], const AirGrid& g)
        {
            std::vector<U8> out;
            putBytes(out, g.mOrigin);
            putBytes(out, g.mCell);
            putBytes(out, g.mUnit);
            putBytes(out, g.mWidth);
            putBytes(out, g.mHeight);
            for (size_t i = 0; i + 1 < g.mStart.size(); ++i)
            {
                putBytes(out, static_cast<U16>(g.mStart[i + 1] - g.mStart[i]));
            }
            for (const AirGap& gap : g.mGaps)
            {
                putBytes(out, gap.mFloor);
                putBytes(out, gap.mCeiling);
                putBytes(out, gap.mFlags);
                putBytes(out, gap.mClear);
                putBytes(out, gap.mSlopeX);
                putBytes(out, gap.mSlopeY);
            }
            section(tag, out);
        }

        bool write(const std::filesystem::path& path) const
        {
            uLongf size = compressBound(static_cast<uLong>(mBody.size()));
            std::vector<U8> packed(size);
            if (compress2(packed.data(), &size, mBody.data(), static_cast<uLong>(mBody.size()), 6) != Z_OK)
            {
                return false;
            }
            std::ofstream out(path, std::ios::binary);
            const char magic[4] = { 'S', 'K', 'A', 'F' };
            const U32 version = 1, raw = static_cast<U32>(mBody.size());
            out.write(magic, 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&raw), 4);
            out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }
    };

    /// Spans per column of a raster state, over its columns holding any.
    struct ColumnStats
    {
        U64 mSpans = 0;
        U64 mColumns = 0;       ///< columns holding a span
        U64 mAll = 0;           ///< every column
        U32 mMax = 0;
        U64 mHistogram[9] = {}; ///< columns by span count, 8 = eight or more

        void add(const RasterColumns& c)
        {
            for (size_t i = 0; i + 1 < c.mStart.size(); ++i)
            {
                const U32 n = c.mStart[i + 1] - c.mStart[i];
                mSpans += n;
                mColumns += n ? 1 : 0;
                ++mAll;
                mMax = std::max(mMax, n);
                ++mHistogram[std::min<U32>(n, 8)];
            }
        }
        void add(const ColumnStats& o)
        {
            mSpans += o.mSpans;
            mColumns += o.mColumns;
            mAll += o.mAll;
            mMax = std::max(mMax, o.mMax);
            for (S32 i = 0; i < 9; ++i)
            {
                mHistogram[i] += o.mHistogram[i];
            }
        }
        std::string json() const
        {
            std::ostringstream s;
            s << "{\"spans\":" << mSpans << ",\"columns\":" << mAll << ",\"filled\":" << mColumns << ",\"max\":" << mMax << ",\"histogram\":[";
            for (S32 i = 0; i < 9; ++i)
            {
                s << (i ? "," : "") << mHistogram[i];
            }
            s << "]}";
            return s.str();
        }
    };

    constexpr S32 STATES = 6;
    const char* const STATE_NAMES[STATES] = { "rasterised", "bridged", "culled", "top_pre_ledge", "top_filtered", "avatar_filtered" };

    const RasterColumns& state(const RasterCapture& c, S32 i)
    {
        const RasterColumns* all[STATES] = { &c.mRasterised, &c.mBridged, &c.mCulled, &c.mTopPreLedge, &c.mTopFiltered, &c.mAvatarFiltered };
        return *all[i];
    }

    /// Write a tile's fixture: its key, the raster's states, the air and its products.
    void writeFixture(const std::filesystem::path& path, const std::string& meta, const RasterCapture& c, const TileAirResult& r)
    {
        FixtureWriter w;
        w.text("META", meta);
        std::vector<U8> frame;
        putBytes(frame, c.mBmin);
        putBytes(frame, c.mCell);
        putBytes(frame, c.mUnit);
        putBytes(frame, c.mBorder);
        putBytes(frame, c.mAvatarBorder);
        w.section("GRID", frame);
        w.array("CODE", c.mCodes);
        w.array("REAL", c.mReal);
        w.columns("RAS0", c.mRasterised);
        w.columns("RAS1", c.mBridged);
        w.columns("RAS2", c.mCulled);
        w.columns("TOPL", c.mTopPreLedge);
        w.columns("TOPF", c.mTopFiltered);
        w.columns("AVAF", c.mAvatarFiltered);
        w.array("OWNC", c.mOwnColumn);
        w.array("WIND", c.mInWindow);
        w.array("AVFS", c.mAvatarFloorStart);
        w.array("AVFL", c.mAvatarFloor);
        w.grid("PADA", c.mPadded);
        std::vector<U32> offsets;
        for (const FaceOffset& f : c.mPaddedFaceOffsets)
        {
            offsets.push_back(f.mBits);
        }
        w.array("PFOF", offsets);
        if (r.mAir)
        {
            w.array("TAIR", packTileAir(*r.mAir));
            w.text("TAIL", tileAirLayout(*r.mAir));
        }
        w.array("PROD", productBytes(r));
        w.write(path);
    }

    std::vector<std::string> splitList(const std::string& text)
    {
        std::vector<std::string> out;
        std::stringstream s(text);
        std::string item;
        while (std::getline(s, item, ';'))
        {
            if (!item.empty())
            {
                out.push_back(item);
            }
        }
        return out;
    }
}

TEST_CASE("skairraster: the coordinate map is a proper rotation and round-trips", "[skairraster]")
{
    const LLVector3 sl(1.f, 2.f, 3.f);
    F32 rc[3];
    toRecast(sl, rc);
    CHECK(rc[2] == 1.f);
    CHECK(rc[0] == 2.f);
    CHECK(rc[1] == 3.f);
    CHECK(fromRecast(rc) == sl);
}

TEST_CASE("skairraster: a block on the land is a floor to the air and to AVATAR; the land around it too", "[skairraster]")
{
    TileInput in = airTile(0, 0);
    in.mTerrain = flat(0.f);
    in.mRecords.push_back(box(14.f, 8.f, 0.f, 16.f, 24.f, 2.f));
    const TileAirResult r = build(in);
    INFO(r.mError);
    REQUIRE(built(r));
    CHECK(topFloorAt(r, 15.f, 16.f, 2.f));
    CHECK(avatarFloorAt(r, 15.f, 16.f, 2.f));
    CHECK(avatarFloorAt(r, 4.f, 16.f, 0.f));
    CHECK(!airFloorAt(r, 15.f, 16.f, 0.f));
    CHECK(!r.mFaces.empty());
}

TEST_CASE("skairraster: three storeys 3 m apart are three floors AVATAR stands on", "[skairraster]")
{
    TileInput in = airTile(0, 0);
    in.mTerrain = flat(0.f);
    for (S32 s = 1; s <= 3; ++s)
    {
        in.mRecords.push_back(box(8.f, 8.f, s * 3.f - 0.2f, 24.f, 24.f, s * 3.f));
    }
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    for (S32 s = 0; s <= 3; ++s)
    {
        INFO("storey " << s);
        CHECK(avatarFloorAt(r, 16.f, 16.f, s * 3.f));
    }
}

TEST_CASE("skairraster: two tiles of a mesh agree on their seam", "[skairraster]")
{
    TileAirResult r[2];
    for (S32 tx = 0; tx < 2; ++tx)
    {
        TileInput in = airTile(tx, 0);
        in.mTerrain = flat(0.f, 37, tx * 32.f - 2.f);
        in.mRecords.push_back(box(28.f, 10.f, 0.f, 36.f, 12.f, 1.5f));    // across the seam
        r[tx] = build(in);
        REQUIRE(built(r[tx]));
        REQUIRE(r[tx].mAir);
    }
    const SeamStrip& east = r[0].mAir->mSeams[TileAir::SIDE_POS_X];
    const SeamStrip& west = r[1].mAir->mSeams[TileAir::SIDE_NEG_X];
    REQUIRE(east.mStart.size() == west.mStart.size());
    for (size_t i = 0; i + 1 < east.mStart.size(); ++i)
    {
        INFO("cell " << i);
        REQUIRE(east.count(static_cast<S32>(i)) == west.count(static_cast<S32>(i)));
        for (U32 k = 0; k < east.count(static_cast<S32>(i)); ++k)
        {
            CHECK(east.mGaps[east.mStart[i] + k].mFloor == west.mGaps[west.mStart[i] + k].mFloor);
        }
    }
}

TEST_CASE("skairraster: a record from another region lands where it should and an empty tile says so", "[skairraster]")
{
    Record r;
    r.mShape = Shape::BOX;
    r.mOrigin.set(256.0, 0.0, 0.0);
    r.mCenter.set(1.f, 1.f, 1.f);
    r.mHalf.set(1.f, 1.f, 1.f);
    r.mAxes[0].set(1, 0, 0);
    r.mAxes[1].set(0, 1, 0);
    r.mAxes[2].set(0, 0, 1);
    std::vector<LLVector3> out;
    emitRecord(r, LLVector3d(224.0, 0.0, 0.0), out);
    CHECK(out.size() == 36u);
    F32 minx = 1e9f, maxx = -1e9f;
    for (const LLVector3& p : out)
    {
        minx = std::min(minx, p.mV[VX]);
        maxx = std::max(maxx, p.mV[VX]);
    }
    CHECK(std::fabs(minx - 32.f) < 1e-4f);
    CHECK(std::fabs(maxx - 34.f) < 1e-4f);

    TileAirResult empty;
    CHECK(!rasteriseTile(tile(3, 3), empty));
    CHECK(empty.mStatus == TileAirResult::EMPTY);
}

TEST_CASE("skairraster: a hull is floor whichever way it winds and solid inside", "[skairraster]")
{
    const std::vector<LLVector3> down = {
        LLVector3(4.f, 4.f, 2.f), LLVector3(4.f, 12.f, 2.f), LLVector3(12.f, 12.f, 2.f),
        LLVector3(4.f, 4.f, 2.f), LLVector3(12.f, 12.f, 2.f), LLVector3(12.f, 4.f, 2.f),
    };
    std::vector<LLVector3> body;
    emitRecord(*box(16.f, 16.f, 1.f, 28.f, 28.f, 7.f), LLVector3d(), body);
    TileInput in = airTile(0, 0);
    in.mTerrain = flat(0.f);
    in.mRecords.push_back(hull(down));
    in.mRecords.push_back(hull(body));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    CHECK(avatarFloorAt(r, 8.f, 8.f, 2.f));
    // Under the box the land keeps a metre of air: the air's slope stands there, no avatar fits.
    CHECK(topFloorAt(r, 22.f, 22.f, 0.f));
    CHECK(!avatarFloorAt(r, 22.f, 22.f, 0.f));
    CHECK(!airFloorAt(r, 22.f, 22.f, 1.5f, 1.f));
    CHECK(avatarFloorAt(r, 22.f, 22.f, 7.f));
}

TEST_CASE("skairraster: an 80 degree ramp is floor to the air's slope and none of AVATAR's", "[skairraster]")
{
    const std::vector<LLVector3> ramp = {
        LLVector3(4.f, 4.f, 0.f), LLVector3(12.f, 4.f, 0.f), LLVector3(12.f, 7.5f, 20.f),
        LLVector3(4.f, 4.f, 0.f), LLVector3(12.f, 7.5f, 20.f), LLVector3(4.f, 7.5f, 20.f),
    };
    TileInput in = airTile(0, 0);
    in.mZMax = 24.f;
    in.mTerrain = flat(0.f);
    in.mRecords.push_back(soup(ramp, Provenance::EXACT));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    CHECK(topFloorAt(r, 8.f, 5.6f, 10.f, 0.5f));
    CHECK(!avatarFloorAt(r, 8.f, 5.6f, 10.f, 0.5f));
}

TEST_CASE("skairraster: a crevice one column wide is bridged; two columns or a wall in it are not", "[skairraster]")
{
    const auto crevice = [](F32 far_edge, bool wall) {
        TileInput in = airTile(0, 0);
        in.mTerrain = flat(0.f);
        in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.7f, 2.f), Provenance::EXACT));
        in.mRecords.push_back(soup(slab(4.f, far_edge, 12.f, 20.f, 2.f), Provenance::EXACT));
        if (wall)
        {
            const std::vector<LLVector3> face = {
                LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 4.f),
                LLVector3(4.f, 11.875f, 2.f), LLVector3(12.f, 11.875f, 4.f), LLVector3(4.f, 11.875f, 4.f),
            };
            in.mRecords.push_back(soup(face, Provenance::EXACT));
        }
        TileBuildStats stats;
        const TileAirResult r = build(in, &stats);
        REQUIRE(built(r));
        return std::make_pair(stats.mTop.mBridges, topFloorAt(r, 8.f, 11.875f, 2.f, 0.3f));
    };
    const auto bridged = crevice(12.05f, false);
    CHECK(bridged.first > 0);
    CHECK(bridged.second);
    CHECK(!crevice(12.05f, true).second);
    CHECK(!crevice(13.2f, false).second);
}

TEST_CASE("skairraster: nothing below the land survives the cull", "[skairraster]")
{
    TileInput in = airTile(0, 0);
    in.mZMin = -4.f;
    in.mTerrain = flat(0.f);
    // A buried roof 2.5 m down keeps 2 m of headroom to the land above it: only the cull takes it out.
    in.mRecords.push_back(box(14.f, 14.f, -4.f, 18.f, 18.f, -2.5f));
    in.mRecords.push_back(box(20.f, 14.f, -3.5f, 24.f, 18.f, 2.5f));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    CHECK(!airFloorAt(r, 16.f, 16.f, -2.5f, 0.5f));
    CHECK(!airFloorAt(r, 22.f, 16.f, -3.f, 0.6f));
    CHECK(avatarFloorAt(r, 16.f, 16.f, 0.f));
    CHECK(avatarFloorAt(r, 22.f, 16.f, 2.5f));
}

TEST_CASE("skairraster: a slab a step above the land keeps both sides of its edge floor", "[skairraster]")
{
    TileInput in = airTile(0, 0);
    in.mTerrain = flat(0.25f);
    in.mAvatar = { 1.25f, 0.3f, 0.75f, 74.f };
    in.mRecords.push_back(box(0.f, -2.f, -0.5f, 36.f, 16.05f, 0.5f));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    for (F32 x = 4.f; x < 28.f; x += 4.f)
    {
        INFO("x " << x);
        CHECK(topFloorAt(r, x, 16.4f, 0.25f));
        CHECK(topFloorAt(r, x, 15.7f, 0.5f));
    }
}

TEST_CASE("skairraster: a box sunk into the land is solid to its bottom with its roof a floor", "[skairraster]")
{
    TileInput in = airTile(0, 0);
    in.mTerrain = flat(0.f);
    in.mRecords.push_back(box(8.f, 8.f, -0.5f, 24.f, 24.f, 3.f));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    CHECK(!airFloorAt(r, 16.f, 16.f, 0.f));
    CHECK(avatarFloorAt(r, 16.f, 16.f, 3.f));
    CHECK(avatarFloorAt(r, 4.f, 4.f, 0.f));
}

TEST_CASE("skairraster: a tunnel through one closed surface is a floor under a roof", "[skairraster]")
{
    const std::vector<Scene> scenes = fixtureScenes();
    const auto it = std::find_if(scenes.begin(), scenes.end(), [](const Scene& s) { return s.mName == "tunnel"; });
    REQUIRE(it != scenes.end());
    TileInput in = it->mInput;
    in.mCell = 0.25f;
    in.mAirPad = airPadCells(in.mCell);
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    const AirGap* g = gapAt(r, 16.f, 16.f, 0.5f);
    REQUIRE(g);
    CHECK((g->mFlags & AirGap::AVATAR_FLOOR) != 0);
    CHECK(std::fabs((r.mAir->mOrigin.mV[VZ] + g->mCeiling * r.mAir->mUnit) - 3.f) < 0.3f);
    CHECK(!airFloorAt(r, 11.f, 16.f, 0.f, 1.f));
    CHECK(avatarFloorAt(r, 11.f, 16.f, 4.f));
}

TEST_CASE("skairraster: a flat floor at a real height is one height in every tile", "[skairraster]")
{
    const F32 z = 2649.f;
    const F32 up = std::nextafter(z, 3000.f), down = std::nextafter(z, 0.f);
    const std::vector<LLVector3> floor = {
        LLVector3(20.f, 20.f, up), LLVector3(44.f, 20.f, down), LLVector3(44.f, 44.f, up),
        LLVector3(20.f, 20.f, down), LLVector3(44.f, 44.f, up), LLVector3(20.f, 44.f, up),
    };
    for (S32 solid = 0; solid < 2; ++solid)
    {
        F32 lo = 1e9f, hi = -1e9f;
        for (S32 tx = 0; tx < 2; ++tx)
        {
            for (S32 ty = 0; ty < 2; ++ty)
            {
                TileInput in = airTile(tx, ty, CELL_M);
                in.mWallFaces = in.mRunoff = false;
                in.mZMin = 2640.f + 3.375f * (tx + ty); // tiles start their rasters at different heights
                in.mZMax = 2660.f;
                in.mRecords.push_back(solid ? box(20.f, 20.f, z - 0.75f, 44.f, 44.f, z) : soup(floor, Provenance::EXACT));
                const TileAirResult r = build(in);
                REQUIRE(built(r));
                REQUIRE(r.mAir);
                for (const AirGap& g : r.mAir->mGaps)
                {
                    if (g.mFlags & AirGap::TOP)
                    {
                        const F32 h = r.mAir->mOrigin.mV[VZ] + g.mFloor * r.mAir->mUnit;
                        lo = llmin(lo, h);
                        hi = llmax(hi, h);
                    }
                }
            }
        }
        INFO((solid ? "solid" : "surface") << ": floors " << lo << ".." << hi);
        CHECK(hi - lo < 0.01f);
    }
}

TEST_CASE("skairraster: a record AVATAR leaves out is still the air's and the solid after it is still filled", "[skairraster]")
{
    for (S32 exclude = 0; exclude < 2; ++exclude)
    {
        TileInput in = airTile(0, 0);
        in.mTerrain = flat(0.f);
        auto platform = std::make_shared<Record>(*box(10.f, 10.f, 0.f, 20.f, 20.f, 2.f));
        platform->mExclude = exclude ? EXCLUDE_AVATAR : 0;
        in.mRecords.push_back(platform);
        std::vector<LLVector3> body;
        emitRecord(*box(22.f, 22.f, 1.f, 28.f, 28.f, 5.f), LLVector3d(), body);
        in.mRecords.push_back(hull(body));
        const TileAirResult r = build(in);
        REQUIRE(built(r));
        INFO("exclude " << exclude);
        CHECK(topFloorAt(r, 15.f, 15.f, 2.f));
        CHECK(avatarFloorAt(r, 15.f, 15.f, 2.f) == !exclude);
        CHECK(topFloorAt(r, 25.f, 25.f, 5.f));
        CHECK(!topFloorAt(r, 25.f, 25.f, 2.f, 0.6f));
        CHECK(!r.mFaces.empty());
    }
}

TEST_CASE("skairraster: a TOP-only surface merged over a floor leaves AVATAR's view as it was", "[skairraster]")
{
    const auto run = [](F32 top_slope) {
        TileInput in = airTile(0, 0);
        in.mTerrain = flat(0.f);
        in.mAvatar.mSlope = 74.f;
        in.mTopParams.mSlope = top_slope;
        in.mRecords.push_back(box(10.f, 10.f, 0.f, 20.f, 20.f, 1.f));
        // An 80 degree face rising half a metre off the platform, inside the merge threshold.
        std::vector<LLVector3> face;
        for (F32 x = 11.f; x < 19.f; x += 1.f)
        {
            const LLVector3 a(x, 10.5f, 1.f), b(x + 0.088f, 10.5f, 1.5f), c(x + 0.088f, 19.5f, 1.5f), d(x, 19.5f, 1.f);
            face.insert(face.end(), { a, b, c, a, c, d });
        }
        in.mRecords.push_back(soup(face, Provenance::EXACT));
        TileBuildStats stats;
        REQUIRE(built(build(in, &stats)));
        return stats.mClass[0].mHfAreaHash;
    };
    CHECK(run(89.f) == run(74.f));
}

TEST_CASE("skairraster: a floor across AVATAR's grid edge filters as in AVATAR's own grid", "[skairraster]")
{
    const auto run = [](bool air, TileBuildStats& stats) {
        TileInput in = tile(0, 0);
        in.mTerrain = flat(0.f, 45, -6.f, -6.f);
        in.mAir = air;
        in.mAirPad = 16;    // past AVATAR's border, so the raster is wider with air
        in.mFaceOffsets = false;
        in.mRecords.push_back(box(-3.f, -3.f, 1.5f, 35.f, 35.f, 2.f));
        in.mRecords.push_back(box(12.f, 12.f, 2.f, 14.f, 14.f, 4.f));
        return build(in, &stats);
    };
    TileBuildStats wide, own;
    REQUIRE(built(run(true, wide)));
    REQUIRE(built(run(false, own)));
    CHECK(wide.mClass[0].mHfBytes > own.mClass[0].mHfBytes);
    CHECK(wide.mClass[0].mHfAreaHash == own.mClass[0].mHfAreaHash);
}

TEST_CASE("skairraster: a bridge only the air lays stays out of AVATAR's view", "[skairraster]")
{
    const auto run = [](F32 top_slope, TileBuildStats& stats) {
        TileInput in = airTile(0, 0);
        in.mTerrain = flat(0.f);
        in.mAvatar.mSlope = 74.f;
        in.mTopParams.mSlope = top_slope;
        in.mRecords.push_back(box(8.f, 10.f, 0.f, 11.9f, 20.f, 1.f));
        const LLVector3 a(12.3f, 10.f, 1.f), b(12.388f, 10.f, 1.5f), c(12.388f, 20.f, 1.5f), d(12.3f, 20.f, 1.f);
        in.mRecords.push_back(soup({ a, b, c, a, c, d }, Provenance::EXACT));
        return build(in, &stats);
    };
    TileBuildStats with_stats, without_stats;
    REQUIRE(built(run(89.f, with_stats)));
    REQUIRE(built(run(74.f, without_stats)));
    CHECK(with_stats.mTopOnlyBridges > 0);
    CHECK(without_stats.mTopOnlyBridges == 0);
    CHECK(with_stats.mOwnColumns > 0);
    CHECK(with_stats.mClass[0].mHfAreaHash == without_stats.mClass[0].mHfAreaHash);
}

TEST_CASE("skairraster: AVATAR's view round a left-out door leaf is as if it were not there", "[skairraster]")
{
    struct Case
    {
        const char* mName;
        bool mFrame;
        bool mLeafFirst;
        bool mFloor;
    };
    const Case cases[] = { { "flush leaf", false, false, true }, { "framed leaf", true, false, true }, { "framed leaf first", true, true, true },
                           { "leaf alone", false, true, false } };
    for (const Case& c : cases)
    {
        const auto run = [&](bool leaf, TileBuildStats& stats) {
            TileInput in = airTile(0, 0);
            in.mTerrain = flat(0.f);
            in.mFaceOffsets = false;
            auto door = std::make_shared<Record>(*box(15.9f, 12.f, 1.f, 16.1f, 14.f, 3.5f));
            door->mExclude = EXCLUDE_AVATAR;
            if (leaf && c.mLeafFirst)
            {
                in.mRecords.push_back(door);
            }
            if (c.mFloor)
            {
                in.mRecords.push_back(box(4.f, 4.f, 0.f, 28.f, 28.f, 1.f));
            }
            if (c.mFrame)
            {
                in.mRecords.push_back(box(15.8f, 11.f, 1.f, 16.2f, 12.f, 4.f));
                in.mRecords.push_back(box(15.8f, 14.f, 1.f, 16.2f, 15.f, 4.f));
                in.mRecords.push_back(box(15.8f, 12.f, 3.5f, 16.2f, 14.f, 4.f));
            }
            if (leaf && !c.mLeafFirst)
            {
                in.mRecords.push_back(door);
            }
            return build(in, &stats);
        };
        INFO(c.mName);
        TileBuildStats with_stats, without_stats;
        const TileAirResult with_leaf = run(true, with_stats), without = run(false, without_stats);
        REQUIRE(built(with_leaf));
        REQUIRE(built(without));
        CHECK(with_stats.mWindowCells > 0);
        CHECK(without_stats.mWindowCells == 0);
        CHECK(with_stats.mClass[0].mHfAreaHash == without_stats.mClass[0].mHfAreaHash);
        // The air and the faces keep the leaf: framed, it closes the doorway's air; free, its top is a floor.
        const bool kept = c.mFrame ? airFloorAt(without, 16.f, 13.f, 1.f) && !airFloorAt(with_leaf, 16.f, 13.f, 1.f)
                                   : airFloorAt(with_leaf, 16.f, 13.f, 3.5f) && !airFloorAt(without, 16.f, 13.f, 3.5f);
        CHECK(kept);
        CHECK(productBytes(with_leaf) != productBytes(without));
    }
}

TEST_CASE("skairraster: a tile without its own air reads the coarse air", "[skairraster]")
{
    TileInput in = tile(0, 0, CELL_M);
    in.mTerrain = flat(0.f);
    in.mCoarseAir = true;
    in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 3.f));
    const TileAirResult r = build(in);
    REQUIRE(built(r));
    REQUIRE(r.mAir);
    CHECK(r.mCoarseAir);
    CHECK(r.mAir->mCell == CELL_M);
    CHECK(airFloorAt(r, 16.f, 16.f, 3.f, 1.f));
}

TEST_CASE("skairraster: a capture holds every stage and changes nothing it captures", "[skairraster]")
{
    const std::vector<Scene> scenes = fixtureScenes();
    for (const Scene& s : scenes)
    {
        INFO(s.mName);
        RasterCapture capture;
        const TileAirResult plain = build(s.mInput), captured = build(s.mInput, nullptr, &capture);
        REQUIRE(built(plain));
        REQUIRE(built(captured));
        CHECK(productBytes(plain) == productBytes(captured));
        CHECK(!capture.mRasterised.empty());
        CHECK(!capture.mCulled.empty());
        CHECK(!capture.mTopFiltered.empty());
        CHECK(!capture.mTopPreLedge.empty());
        CHECK(!capture.mAvatarFiltered.empty());
        CHECK(capture.mAvatarFloorStart.size() == capture.mCulled.mStart.size());
        CHECK(capture.mAvatarFloor.size() == capture.mCulled.mSpans.size());
        CHECK(!capture.mPadded.empty());
        // The ledge filter only takes floors away.
        U32 pre = 0, post = 0;
        for (U32 v : capture.mTopPreLedge.mSpans)
        {
            pre += (v >> 26) != 0;
        }
        for (U32 v : capture.mTopFiltered.mSpans)
        {
            post += (v >> 26) != 0;
        }
        CHECK(post <= pre);
    }
}

// The synthetic scenes' products, hashed: what the Recast raster made of them when the fixtures were captured.
// A raster that replaces it is held to the fixtures by outcome, not to these; they say the Recast one moved.
TEST_CASE("skairraster: the synthetic fixture scenes build to their golden products", "[skairraster]")
{
    const std::pair<const char*, U64> golden[] = {
        { "stairs", 0x5d1b14ea2b963748ull },
        { "closed box", 0x9f479d0ad00ae08dull },
        { "hollow box", 0x80806eaf04fbf099ull },
        { "tunnel", 0x97fb617a5cf6574bull },
        { "door leaf", 0x73a8e6b66495cdc8ull },
        { "crevice", 0xf7b23878dd7f0264ull },
        { "buried prim", 0xb1ca5eb37bd8006bull },
        { "exclusion cut", 0x8bb1a87791edf2caull },
        { "under-water slope", 0x746c42bdf557a153ull },
        { "megaprim", 0xbd8b3c0f53fb8929ull },
    };
    const std::vector<Scene> scenes = fixtureScenes();
    REQUIRE(scenes.size() == std::size(golden));
    for (size_t i = 0; i < scenes.size(); ++i)
    {
        const TileAirResult r = build(scenes[i].mInput);
        INFO(scenes[i].mName << ": " << r.mError);
        REQUIRE(built(r));
        CHECK(r.mAir);
        const U64 h = hashBytes(productBytes(r));
        INFO(llformat("{ \"%s\", 0x%016llxull },", scenes[i].mName.c_str(), static_cast<unsigned long long>(h)));
        CHECK(std::string(golden[i].first) == scenes[i].mName);
        CHECK(h == golden[i].second);
    }
}

// What stock Recast 1.6.0 does at the edges of its merge and filter tests, which a raster of our own
// need not copy but must know: each case sets up a column on its exact boundary.
TEST_CASE("skairraster: Recast's merge and filter boundaries", "[skairraster]")
{
    rcContext ctx(false);
    const F32 bmin[3] = { 0.f, 0.f, 0.f }, bmax[3] = { 3.f, 100.f, 3.f };
    const auto field = [&](rcHeightfield& hf) { REQUIRE(rcCreateHeightfield(&ctx, hf, 3, 3, bmin, bmax, 1.f, 0.1f)); };
    const auto spans = [](const rcHeightfield& hf, S32 x, S32 z) {
        std::vector<std::array<U32, 3>> out;
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            out.push_back({ s->smin, s->smax, s->area });
        }
        return out;
    };

    SECTION("merge: tops exactly climb apart keep the higher area; one more keeps the incoming span's")
    {
        rcHeightfield hf;
        field(hf);
        rcAddSpan(&ctx, hf, 1, 1, 0, 10, 5, 3);
        rcAddSpan(&ctx, hf, 1, 1, 5, 13, 1, 3);    // tops 3 apart, climb 3
        rcAddSpan(&ctx, hf, 2, 1, 0, 10, 5, 3);
        rcAddSpan(&ctx, hf, 2, 1, 5, 14, 1, 3);    // tops 4 apart
        const auto a = spans(hf, 1, 1), b = spans(hf, 2, 1);
        REQUIRE(a.size() == 1);
        REQUIRE(b.size() == 1);
        CHECK(a[0] == std::array<U32, 3>{ 0, 13, 5 });
        CHECK(b[0] == std::array<U32, 3>{ 0, 14, 1 });
    }
    SECTION("merge: spans that only touch merge")
    {
        rcHeightfield hf;
        field(hf);
        rcAddSpan(&ctx, hf, 1, 1, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 1, 1, 10, 20, 1, 0);
        rcAddSpan(&ctx, hf, 2, 1, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 2, 1, 11, 20, 1, 0);
        CHECK(spans(hf, 1, 1).size() == 1);
        CHECK(spans(hf, 2, 1).size() == 2);
    }
    SECTION("headroom: a gap exactly the height keeps its floor; one less clears it")
    {
        rcHeightfield hf;
        field(hf);
        rcAddSpan(&ctx, hf, 1, 1, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 1, 1, 30, 31, 0, 0);   // gap 20
        rcAddSpan(&ctx, hf, 2, 1, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 2, 1, 29, 30, 0, 0);   // gap 19
        rcFilterWalkableLowHeightSpans(&ctx, 20, hf);
        CHECK(spans(hf, 1, 1)[0][2] == 1u);
        CHECK(spans(hf, 2, 1)[0][2] == 0u);
    }
    SECTION("headroom: a column's last span is open above")
    {
        rcHeightfield hf;
        field(hf);
        rcAddSpan(&ctx, hf, 1, 1, 990, 995, 1, 0);  // five units under the field's top
        rcFilterWalkableLowHeightSpans(&ctx, 20, hf);
        CHECK(spans(hf, 1, 1)[0][2] == 1u);
    }
    SECTION("step-up: an obstacle exactly climb above a floor becomes floor; one more stays obstacle; no chaining")
    {
        rcHeightfield hf;
        field(hf);
        rcAddSpan(&ctx, hf, 0, 0, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 0, 0, 11, 13, 0, 0);   // top 3 above, climb 3
        rcAddSpan(&ctx, hf, 0, 0, 14, 16, 0, 0);   // 3 above that: chained, not promoted
        rcAddSpan(&ctx, hf, 1, 0, 0, 10, 1, 0);
        rcAddSpan(&ctx, hf, 1, 0, 11, 14, 0, 0);   // 4 above
        rcFilterLowHangingWalkableObstacles(&ctx, 3, hf);
        const auto a = spans(hf, 0, 0), b = spans(hf, 1, 0);
        REQUIRE(a.size() == 3);
        CHECK(a[1][2] == 1u);
        CHECK(a[2][2] == 0u);
        CHECK(b[1][2] == 0u);
    }
    SECTION("ledge: a drop of exactly climb to a neighbour keeps the floor; one more clears it; the grid edge is a drop")
    {
        // A floor at 10 in the middle column, its neighbours at 7 (drop 3) or 6 (drop 4).
        for (S32 drop = 3; drop <= 4; ++drop)
        {
            rcHeightfield hf;
            field(hf);
            for (S32 z = 0; z < 3; ++z)
            {
                for (S32 x = 0; x < 3; ++x)
                {
                    const U16 top = x == 1 && z == 1 ? 10 : static_cast<U16>(10 - drop);
                    rcAddSpan(&ctx, hf, x, z, 0, top, 1, 0);
                }
            }
            rcFilterLedgeSpans(&ctx, 20, 3, hf);
            INFO("drop " << drop);
            CHECK(spans(hf, 1, 1)[0][2] == (drop == 3 ? 1u : 0u));
            CHECK(spans(hf, 0, 0)[0][2] == 0u);     // the grid's edge
        }
    }
}

// Run by hand: SKAIR_CAPTURE_DIR names the output folder, SKAIR_CAPTURE_CENSUS the census files (';'
// between them), SKAIR_CAPTURE_THREADS the workers. Every tile of every census is rasterised with its air
// and dumped with each raster stage, as fixtures a raster of our own is checked against.
TEST_CASE("skairraster: capture the Recast raster's fixtures", "[skairraster][.capture]")
{
    const char* dir_env = std::getenv("SKAIR_CAPTURE_DIR");
    if (!dir_env || !*dir_env)
    {
        SKIP("SKAIR_CAPTURE_DIR is not set");
    }
    const std::filesystem::path root(dir_env);
    std::filesystem::create_directories(root);
    const char* threads_env = std::getenv("SKAIR_CAPTURE_THREADS");
    const S32 threads = threads_env ? std::max(1, std::atoi(threads_env)) : 4;

    std::ofstream summary(root / "summary.json");
    summary << "{\"sets\":[";
    bool first_set = true;

    // The synthetic scenes first.
    {
        const std::filesystem::path out = root / "synthetic";
        std::filesystem::create_directories(out);
        std::ofstream manifest(out / "manifest.json");
        manifest << "{\"tiles\":[";
        const std::vector<Scene> scenes = fixtureScenes();
        for (size_t i = 0; i < scenes.size(); ++i)
        {
            RasterCapture capture;
            const TileAirResult r = build(scenes[i].mInput, nullptr, &capture);
            std::string file = scenes[i].mName;
            std::replace(file.begin(), file.end(), ' ', '-');
            const std::string meta = llformat("{\"scene\":\"%s\",\"hash\":\"%016llx\"}", scenes[i].mName.c_str(),
                                              static_cast<unsigned long long>(hashBytes(productBytes(r))));
            writeFixture(out / (file + ".skaf"), meta, capture, r);
            manifest << (i ? "," : "") << "{\"file\":\"" << file << ".skaf\",\"meta\":" << meta << "}";
        }
        manifest << "]}\n";
    }

    const char* census_env = std::getenv("SKAIR_CAPTURE_CENSUS");
    for (const std::string& path : splitList(census_env ? census_env : ""))
    {
        CensusFile census;
        AirCensusParams params;
        std::string error;
        const CensusSections sections = { airParamsSection(params) };
        INFO(path);
        REQUIRE(readCensusFile(path, census, &error, sections));
        std::vector<CensusTile> tiles;
        const U32 refused = layCensusTiles(census, params, tiles);
        const std::string set = std::filesystem::path(path).stem().string();
        const std::filesystem::path out = root / set;
        std::filesystem::create_directories(out);

        std::mutex lock;
        std::vector<std::string> entries(tiles.size());
        ColumnStats totals[STATES];
        U64 bytes = 0, built_tiles = 0, empty_tiles = 0, failed = 0;
        F64 raster_ms = 0.0;
        std::atomic<size_t> next{ 0 };
        std::vector<std::thread> pool;
        for (S32 k = 0; k < threads; ++k)
        {
            pool.emplace_back([&]() {
                for (size_t i = next++; i < tiles.size(); i = next++)
                {
                    const CensusTile& t = tiles[i];
                    RasterCapture capture;
                    TileBuildStats stats;
                    TileAirResult r;
                    const bool ok = rasteriseTile(*t.mInput, r, &stats, &capture);
                    const std::string file = llformat("%s_%d_%d.skaf", t.mMesh.asString().c_str(), t.mKey.mX, t.mKey.mY);
                    ColumnStats tile_stats[STATES];
                    std::string meta = llformat(
                        "{\"census\":\"%s\",\"mesh\":\"%s\",\"terrain\":%s,\"key\":[%d,%d],\"mesh_origin\":[%.3f,%.3f,%.3f],\"over_land\":%s,"
                        "\"in_air_radius\":%s,\"records\":%zu,\"cuts\":%zu,\"z\":[%.4f,%.4f],\"status\":%d,\"triangles\":%d,\"ms\":%.3f",
                        set.c_str(), t.mMesh.asString().c_str(), t.mTerrain ? "true" : "false", t.mKey.mX, t.mKey.mY,
                        t.mInput->mMeshOrigin.mdV[VX], t.mInput->mMeshOrigin.mdV[VY], t.mInput->mMeshOrigin.mdV[VZ],
                        t.mOverLand ? "true" : "false", t.mInAirRadius ? "true" : "false", t.mInput->mRecords.size(), t.mInput->mCuts.size(),
                        t.mInput->mZMin, t.mInput->mZMax, static_cast<S32>(r.mStatus), r.mTriangles, r.mMillis);
                    if (ok)
                    {
                        for (S32 s = 0; s < STATES; ++s)
                        {
                            tile_stats[s].add(state(capture, s));
                        }
                        meta += ",\"columns\":{";
                        for (S32 s = 0; s < STATES; ++s)
                        {
                            meta += llformat("%s\"%s\":", s ? "," : "", STATE_NAMES[s]) + tile_stats[s].json();
                        }
                        meta += llformat("},\"faces\":%zu,\"ledges\":%zu,\"runoff\":%zu,\"hash\":\"%016llx\"", r.mFaces.size(), r.mLedges.size(),
                                         r.mRunoff.size(), static_cast<unsigned long long>(hashBytes(productBytes(r))));
                    }
                    meta += "}";
                    if (ok)
                    {
                        writeFixture(out / file, meta, capture, r);
                    }
                    std::lock_guard<std::mutex> guard(lock);
                    entries[i] = llformat("{\"file\":\"%s\",\"meta\":", ok ? file.c_str() : "") + meta + "}";
                    if (ok)
                    {
                        ++built_tiles;
                        raster_ms += r.mMillis;
                        bytes += std::filesystem::file_size(out / file);
                        for (S32 s = 0; s < STATES; ++s)
                        {
                            totals[s].add(tile_stats[s]);
                        }
                    }
                    else if (r.mStatus == TileAirResult::EMPTY)
                    {
                        ++empty_tiles;
                    }
                    else
                    {
                        ++failed;
                    }
                }
            });
        }
        for (std::thread& t : pool)
        {
            t.join();
        }
        std::ofstream manifest(out / "manifest.json");
        manifest << "{\"census\":\"" << jsonEscape(path) << "\",\"params\":";
        writeAirParams(manifest, params);
        manifest << ",\"tiles\":[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            manifest << (i ? ",\n" : "\n") << entries[i];
        }
        manifest << "]}\n";

        summary << (first_set ? "" : ",") << "\n{\"set\":\"" << set << "\",\"census\":\"" << jsonEscape(path) << "\",\"tiles\":" << tiles.size()
                << ",\"built\":" << built_tiles << ",\"empty\":" << empty_tiles << ",\"failed\":" << failed << ",\"refused_meshes\":" << refused
                << ",\"bytes\":" << bytes << ",\"raster_ms\":" << raster_ms << ",\"columns\":{";
        for (S32 s = 0; s < STATES; ++s)
        {
            summary << (s ? "," : "") << "\"" << STATE_NAMES[s] << "\":" << totals[s].json();
        }
        summary << "}}";
        first_set = false;
        CHECK(failed == 0);
    }
    summary << "]}\n";
}
