/**
 * @file sknavtilecache.cpp
 * @brief Tile products on disk under a session-stable content key, and the air budget's eviction order.
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

#include "sknavtilecache.h"
#include "sknavfingerprint.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <thread>

namespace sknav
{
    namespace
    {
        constexpr U32 MAGIC = 0x544e4b53;           // 'SKNT'
        constexpr U32 PLACEHOLDER_TAG = 0x534b4e56;  // the census's 'SKNV' object-cache ids

        U64 quant(U64 h, F64 v, F64 step)
        {
            return mix(h, static_cast<U64>(std::llround(v / step)));
        }

        U64 quantVec(U64 h, const LLVector3& v, F64 step)
        {
            for (S32 i = 0; i < 3; ++i)
            {
                h = quant(h, v.mV[i], step);
            }
            return h;
        }

        U64 quantVecD(U64 h, const LLVector3d& v, F64 step)
        {
            for (S32 i = 0; i < 3; ++i)
            {
                h = quant(h, v.mdV[i], step);
            }
            return h;
        }

        U64 mixId(U64 h, const LLUUID& id)
        {
            for (U8 b : id.mData)
            {
                h = mix(h, b);
            }
            return h;
        }

        /// Keys of a multiset, sorted so arrival order cannot move the hash
        /// and mixed in sequence so two equal parts do not cancel out.
        U64 mixKeys(U64 h, const std::vector<RecordPtr>& records)
        {
            std::vector<U64> keys;
            keys.reserve(records.size());
            for (const RecordPtr& r : records)
            {
                keys.push_back(r ? recordContentKey(*r) : 0);
            }
            std::sort(keys.begin(), keys.end());
            h = mix(h, keys.size());
            for (U64 k : keys)
            {
                h = mix(h, k);
            }
            return h;
        }

        U64 checksum(const U8* data, size_t size)
        {
            U64 h = SEED;
            for (size_t i = 0; i < size; ++i)
            {
                h = mix(h, data[i]);
            }
            return h;
        }

        std::filesystem::path pathFromUtf8(const std::string& s)
        {
            return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
        }

        class Writer
        {
        public:
            std::vector<U8> mOut;

            template <typename T> void pod(const T& v)
            {
                const U8* p = reinterpret_cast<const U8*>(&v);
                mOut.insert(mOut.end(), p, p + sizeof(T));
            }
            void vec(const LLVector3& v)
            {
                pod(v.mV[0]);
                pod(v.mV[1]);
                pod(v.mV[2]);
            }
            void bytes(const std::vector<U8>& b) { mOut.insert(mOut.end(), b.begin(), b.end()); }
            /// A section's tag and a length patched in when it closes.
            size_t open(U32 tag)
            {
                pod(tag);
                pod(U32(0));
                return mOut.size();
            }
            void close(size_t start)
            {
                const U32 len = static_cast<U32>(mOut.size() - start);
                std::memcpy(mOut.data() + start - sizeof(U32), &len, sizeof(len));
            }
        };

        class Reader
        {
        public:
            Reader(const U8* data, size_t size) : mData(data), mSize(size) {}

            template <typename T> bool pod(T& v)
            {
                if (mSize - mPos < sizeof(T))
                {
                    return false;
                }
                std::memcpy(&v, mData + mPos, sizeof(T));
                mPos += sizeof(T);
                return true;
            }
            bool vec(LLVector3& v) { return pod(v.mV[0]) && pod(v.mV[1]) && pod(v.mV[2]); }
            /// A count no larger than the bytes left could hold at per bytes each.
            bool count(U32& n, size_t per)
            {
                return pod(n) && static_cast<U64>(n) * per <= mSize - mPos;
            }
            bool bytes(std::vector<U8>& out, size_t n)
            {
                if (mSize - mPos < n)
                {
                    return false;
                }
                out.assign(mData + mPos, mData + mPos + n);
                mPos += n;
                return true;
            }
            size_t left() const { return mSize - mPos; }

            const U8* mData;
            size_t mSize;
            size_t mPos = 0;
        };

        constexpr U32 tag(char a, char b, char c, char d)
        {
            return static_cast<U32>(a) | static_cast<U32>(b) << 8 | static_cast<U32>(c) << 16 | static_cast<U32>(d) << 24;
        }
        constexpr U32 TAG_META = tag('M', 'E', 'T', 'A');
        constexpr U32 TAG_DETOUR[NUM_CLASSES] = { tag('D', 'T', '0', ' '), tag('D', 'T', '1', ' ') };
        constexpr U32 TAG_FACES = tag('F', 'A', 'C', 'E');
        constexpr U32 TAG_SEGMENTS[NUM_CLASSES] = { tag('S', 'E', 'G', '0'), tag('S', 'E', 'G', '1') };
        constexpr U32 TAG_LINKS[NUM_CLASSES] = { tag('L', 'N', 'K', '0'), tag('L', 'N', 'K', '1') };
        constexpr U32 TAG_AIR = tag('A', 'I', 'R', ' ');
        constexpr U32 TAG_FLIGHTS[NUM_CLASSES] = { tag('F', 'L', 'T', '0'), tag('F', 'L', 'T', '1') };

        constexpr size_t GAP_BYTES = 8;

        void writeGaps(Writer& w, const std::vector<AirGap>& gaps)
        {
            w.pod(static_cast<U32>(gaps.size()));
            for (const AirGap& g : gaps)
            {
                w.pod(g.mFloor);
                w.pod(g.mCeiling);
                w.pod(g.mFlags);
                w.pod(g.mClear);
                w.pod(g.mSeed);
            }
        }

        bool readGaps(Reader& r, std::vector<AirGap>& gaps)
        {
            U32 n;
            if (!r.count(n, GAP_BYTES))
            {
                return false;
            }
            gaps.resize(n);
            for (AirGap& g : gaps)
            {
                if (!(r.pod(g.mFloor) && r.pod(g.mCeiling) && r.pod(g.mFlags) && r.pod(g.mClear) && r.pod(g.mSeed)))
                {
                    return false;
                }
            }
            return true;
        }

        void writeU32s(Writer& w, const std::vector<U32>& v)
        {
            w.pod(static_cast<U32>(v.size()));
            for (U32 x : v)
            {
                w.pod(x);
            }
        }

        bool readU32s(Reader& r, std::vector<U32>& v)
        {
            U32 n;
            if (!r.count(n, sizeof(U32)))
            {
                return false;
            }
            v.resize(n);
            for (U32& x : v)
            {
                r.pod(x);
            }
            return true;
        }

        /// Offsets that start at zero, never fall and end at the record count.
        bool offsetsFit(const std::vector<U32>& start, size_t cells, size_t records)
        {
            if (start.size() != cells + 1 || start.front() != 0 || start.back() != records)
            {
                return false;
            }
            return std::is_sorted(start.begin(), start.end());
        }

        void writeAir(Writer& w, const TileAir& air)
        {
            w.vec(air.mOrigin);
            w.pod(air.mCell);
            w.pod(air.mUnit);
            w.pod(air.mWidth);
            w.pod(air.mHeight);
            w.pod(static_cast<U32>(air.mLeaves.size()));
            for (const AirLeaf& l : air.mLeaves)
            {
                w.pod(l.mX);
                w.pod(l.mY);
                w.pod(l.mLevel);
                w.pod(l.mGapCount);
                w.pod(l.mFirstGap);
            }
            writeGaps(w, air.mGaps);
            for (const SeamStrip& s : air.mSeams)
            {
                writeU32s(w, s.mStart);
                writeGaps(w, s.mGaps);
            }
            w.pod(air.mTopCell);
            w.pod(air.mTopWidth);
            w.pod(air.mTopHeight);
            writeU32s(w, air.mTopStart);
            w.pod(static_cast<U32>(air.mTop.size()));
            for (const TopInterval& t : air.mTop)
            {
                w.pod(t.mBottom);
                w.pod(t.mTop);
            }
        }

        bool readAir(Reader& r, TileAir& air)
        {
            U32 n;
            if (!(r.vec(air.mOrigin) && r.pod(air.mCell) && r.pod(air.mUnit) && r.pod(air.mWidth) && r.pod(air.mHeight)
                  && r.count(n, 10)))
            {
                return false;
            }
            if (air.mWidth < 0 || air.mHeight < 0)
            {
                return false;
            }
            air.mLeaves.resize(n);
            for (AirLeaf& l : air.mLeaves)
            {
                r.pod(l.mX);
                r.pod(l.mY);
                r.pod(l.mLevel);
                r.pod(l.mGapCount);
                r.pod(l.mFirstGap);
            }
            if (!readGaps(r, air.mGaps))
            {
                return false;
            }
            for (const AirLeaf& l : air.mLeaves)
            {
                if (static_cast<U64>(l.mFirstGap) + l.mGapCount > air.mGaps.size())
                {
                    return false;
                }
            }
            for (S32 side = 0; side < 4; ++side)
            {
                SeamStrip& s = air.mSeams[side];
                if (!readU32s(r, s.mStart) || !readGaps(r, s.mGaps))
                {
                    return false;
                }
                const size_t cells = side < 2 ? air.mHeight : air.mWidth;
                if (!s.mStart.empty() && !offsetsFit(s.mStart, cells, s.mGaps.size()))
                {
                    return false;
                }
            }
            if (!(r.pod(air.mTopCell) && r.pod(air.mTopWidth) && r.pod(air.mTopHeight) && readU32s(r, air.mTopStart)
                  && r.count(n, 4)))
            {
                return false;
            }
            air.mTop.resize(n);
            for (TopInterval& t : air.mTop)
            {
                r.pod(t.mBottom);
                r.pod(t.mTop);
            }
            if (air.mTopWidth < 0 || air.mTopHeight < 0)
            {
                return false;
            }
            return air.mTopStart.empty()
                || offsetsFit(air.mTopStart, static_cast<size_t>(air.mTopWidth) * air.mTopHeight, air.mTop.size());
        }
    }

    U64 finishKey(U64 h)
    {
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebull;
        return h ^ (h >> 31);
    }

    U64 recordContentKey(const Record& record)
    {
        U64 h = SEED;
        U32 tail;
        std::memcpy(&tail, record.mPart.mData + 12, sizeof(tail));
        // A placeholder from the region's object cache is named by its local
        // id, which the sim reassigns; its box is what it is.
        if (tail != PLACEHOLDER_TAG)
        {
            h = mixId(h, record.mPart);
        }
        h = mix(h, static_cast<U64>(record.mRole));
        h = mix(h, static_cast<U64>(record.mProvenance));
        h = mix(h, static_cast<U64>(record.mNavRole));
        h = mix(h, static_cast<U64>(record.mShape));
        h = mix(h, record.mDynamic);
        h = mix(h, record.mSolid);
        h = mix(h, static_cast<U64>(record.mFeature));
        h = mix(h, record.mExclude);
        h = quantVecD(h, record.mOrigin, KEY_POSITION_M);
        h = quantVec(h, record.mCenter, KEY_POSITION_M);
        for (const LLVector3& axis : record.mAxes)
        {
            h = quantVec(h, axis, KEY_UNIT);
        }
        h = quantVec(h, record.mHalf, KEY_POSITION_M);
        h = quantVecD(h, record.mMin, KEY_POSITION_M);
        h = quantVecD(h, record.mMax, KEY_POSITION_M);
        h = mix(h, record.mConvexTris.size());
        for (S32 n : record.mConvexTris)
        {
            h = mix(h, static_cast<U64>(n));
        }
        if (record.mTriangles)
        {
            h = mix(h, record.mTriangles->size());
            for (const LLVector3& v : *record.mTriangles)
            {
                h = quantVec(h, v, KEY_POSITION_M);
            }
        }
        return h;
    }

    // Whoever adds a field to TileBuildInput keys it below, then updates this size.
    static_assert(sizeof(void*) != 8 || sizeof(TileBuildInput) == 232, "a new TileBuildInput field must go into tileContentKey");

    U64 tileContentKey(const TileBuildInput& input)
    {
        // The format, Detour's tile layout and this build's own algorithms.
        U64 h = mix(SEED, TILE_PRODUCT_VERSION);
        h = mix(h, static_cast<U64>(DT_NAVMESH_VERSION));
        h = mix(h, SKNAV_BUILD_FINGERPRINT);
        // The mesh origin is on the lattice, so exact; the product is relative to it.
        h = mixF64(h, input.mMeshOrigin.mdV[VX]);
        h = mixF64(h, input.mMeshOrigin.mdV[VY]);
        h = mixF64(h, input.mMeshOrigin.mdV[VZ]);
        h = mix(h, static_cast<U32>(input.mTileX));
        h = mix(h, static_cast<U32>(input.mTileY));
        h = mixF32(h, input.mTileSize);
        h = quant(h, input.mZMin, KEY_POSITION_M);
        h = quant(h, input.mZMax, KEY_POSITION_M);
        h = mixF32(h, input.mCell);
        h = mix(h, static_cast<U64>(input.mPartition));
        h = mixF32(h, input.mDetailSampleCells);
        h = mixF32(h, input.mDetailMaxErrorCells);
        h = mixF32(h, input.mWallMinHeight);
        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            h = mix(h, input.mClasses[c]);
            h = mixF32(h, input.mParams[c].mHeight);
            h = mixF32(h, input.mParams[c].mRadius);
            h = mixF32(h, input.mParams[c].mClimb);
            h = mixF32(h, input.mParams[c].mSlope);
            h = mix(h, input.mEdges[c]);
            h = mixF32(h, input.mDropMax[c]);
        }
        h = mix(h, input.mWallFaces);
        h = mixF32(h, input.mDropSpacing);
        h = mix(h, input.mAir);
        h = mix(h, static_cast<U32>(input.mAirPad));
        h = mixF32(h, input.mAirMinGap);

        // The land by its samples, never by when a patch last arrived.
        const TerrainWindow& t = input.mTerrain;
        h = quantVec(h, t.mOrigin, KEY_POSITION_M);
        h = mixF32(h, t.mSpacing);
        h = mix(h, static_cast<U32>(t.mWidth));
        h = mix(h, static_cast<U32>(t.mHeight));
        h = quant(h, t.mWater, KEY_POSITION_M);
        h = mix(h, t.mZ.size());
        for (F32 z : t.mZ)
        {
            h = quant(h, z, KEY_POSITION_M);
        }

        h = mixKeys(h, input.mRecords);
        h = mixKeys(h, input.mCuts);
        return finishKey(h);
    }

    std::vector<U8> packTileProduct(const TileBuildResult& result, U64 key)
    {
        Writer w;
        w.pod(MAGIC);
        w.pod(TILE_PRODUCT_VERSION);
        w.pod(key);

        size_t s = w.open(TAG_META);
        w.pod(static_cast<U8>(result.mStatus));
        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            w.pod(result.mPolys[c]);
        }
        w.pod(result.mTriangles);
        w.pod(result.mMillis);
        w.pod(result.mExcludeMillis);
        w.pod(static_cast<U8>(result.mAirBuilt));
        w.close(s);

        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            if (!result.mData[c].empty())
            {
                s = w.open(TAG_DETOUR[c]);
                w.bytes(result.mData[c]);
                w.close(s);
            }
        }

        s = w.open(TAG_FACES);
        w.pod(static_cast<U32>(result.mFaces.size()));
        for (const WallFace& f : result.mFaces)
        {
            w.vec(f.mMin);
            w.vec(f.mMax);
            w.pod(f.mSide);
            w.pod(f.mArea);
        }
        w.close(s);

        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            s = w.open(TAG_SEGMENTS[c]);
            w.pod(static_cast<U32>(result.mSegments[c].size()));
            for (const WallSegment& g : result.mSegments[c])
            {
                w.vec(g.mA);
                w.vec(g.mB);
                w.pod(static_cast<U8>(g.mKind));
                w.pod(g.mValue);
                w.pod(g.mArea);
            }
            w.close(s);

            s = w.open(TAG_LINKS[c]);
            w.pod(static_cast<U32>(result.mLinks[c].size()));
            for (const DropLink& l : result.mLinks[c])
            {
                w.vec(l.mFrom);
                w.vec(l.mTo);
                w.pod(l.mHeight);
                w.pod(static_cast<U8>(l.mKind));
            }
            w.close(s);

            s = w.open(TAG_FLIGHTS[c]);
            w.pod(static_cast<U32>(result.mFlights[c].size()));
            for (const Flight& f : result.mFlights[c])
            {
                w.pod(f.mKey);
                w.pod(f.mCode);
                w.pod(f.mRise);
                w.pod(f.mPlane[0]);
                w.pod(f.mPlane[1]);
                w.pod(f.mPlane[2]);
                w.pod(f.mZMin);
                w.pod(f.mZMax);
                w.vec(f.mTopNosingA);
                w.vec(f.mTopNosingB);
                w.pod(static_cast<U32>(f.mTreads.size()));
                for (const FlightTread& t : f.mTreads)
                {
                    w.pod(t.mZ);
                    w.vec(t.mNosingA);
                    w.vec(t.mNosingB);
                    w.pod(t.mRise);
                    w.pod(t.mGoing);
                }
                w.pod(static_cast<U32>(f.mFootprint.size()));
                for (const LLVector2& p : f.mFootprint)
                {
                    w.pod(p.mV[0]);
                    w.pod(p.mV[1]);
                }
                w.pod(static_cast<U32>(f.mPolys.size()));
                for (const U16 p : f.mPolys)
                {
                    w.pod(p);
                }
            }
            w.close(s);
        }

        if (result.mAir)
        {
            s = w.open(TAG_AIR);
            writeAir(w, *result.mAir);
            w.close(s);
        }

        w.pod(checksum(w.mOut.data(), w.mOut.size()));
        return std::move(w.mOut);
    }

    bool unpackTileProduct(const U8* data, size_t size, U64 key, TileBuildResult& out)
    {
        if (!data || size < 16 + sizeof(U64))
        {
            return false;
        }
        const size_t body = size - sizeof(U64);
        U64 sum;
        std::memcpy(&sum, data + body, sizeof(sum));
        if (sum != checksum(data, body))
        {
            return false;
        }
        Reader r(data, body);
        U32 magic, version;
        U64 stored;
        if (!(r.pod(magic) && r.pod(version) && r.pod(stored)) || magic != MAGIC || version != TILE_PRODUCT_VERSION || stored != key)
        {
            return false;
        }

        TileBuildResult res;
        bool meta = false;
        while (r.left() > 0)
        {
            U32 t, len;
            if (!r.pod(t) || !r.pod(len) || len > r.left())
            {
                return false;
            }
            Reader sec(r.mData + r.mPos, len);
            r.mPos += len;
            U32 n;
            if (t == TAG_META)
            {
                U8 status, air_built;
                if (!sec.pod(status) || status > TileBuildResult::FAILED)
                {
                    return false;
                }
                res.mStatus = static_cast<TileBuildResult::Status>(status);
                for (S32 c = 0; c < NUM_CLASSES; ++c)
                {
                    sec.pod(res.mPolys[c]);
                }
                if (!(sec.pod(res.mTriangles) && sec.pod(res.mMillis) && sec.pod(res.mExcludeMillis) && sec.pod(air_built)))
                {
                    return false;
                }
                res.mAirBuilt = air_built != 0;
                meta = true;
                continue;
            }
            bool ok = true;
            for (S32 c = 0; c < NUM_CLASSES; ++c)
            {
                if (t == TAG_DETOUR[c])
                {
                    ok = sec.bytes(res.mData[c], len);
                }
                else if (t == TAG_SEGMENTS[c])
                {
                    ok = sec.count(n, 30);
                    res.mSegments[c].resize(ok ? n : 0);
                    for (WallSegment& g : res.mSegments[c])
                    {
                        U8 kind;
                        sec.vec(g.mA);
                        sec.vec(g.mB);
                        sec.pod(kind);
                        sec.pod(g.mValue);
                        sec.pod(g.mArea);
                        ok = ok && kind <= static_cast<U8>(EdgeKind::OVERHANG);
                        g.mKind = static_cast<EdgeKind>(kind);
                    }
                }
                else if (t == TAG_FLIGHTS[c])
                {
                    ok = sec.count(n, 77);
                    res.mFlights[c].resize(ok ? n : 0);
                    for (Flight& f : res.mFlights[c])
                    {
                        U32 m = 0;
                        ok = ok && sec.pod(f.mKey) && sec.pod(f.mCode) && sec.pod(f.mRise) && sec.pod(f.mPlane[0]) && sec.pod(f.mPlane[1]) &&
                             sec.pod(f.mPlane[2]) && sec.pod(f.mZMin) && sec.pod(f.mZMax) && sec.vec(f.mTopNosingA) && sec.vec(f.mTopNosingB) &&
                             sec.count(m, 36);
                        f.mTreads.resize(ok ? m : 0);
                        for (FlightTread& t : f.mTreads)
                        {
                            ok = ok && sec.pod(t.mZ) && sec.vec(t.mNosingA) && sec.vec(t.mNosingB) && sec.pod(t.mRise) && sec.pod(t.mGoing);
                        }
                        ok = ok && sec.count(m, 8);
                        f.mFootprint.resize(ok ? m : 0);
                        for (LLVector2& p : f.mFootprint)
                        {
                            ok = ok && sec.pod(p.mV[0]) && sec.pod(p.mV[1]);
                        }
                        ok = ok && sec.count(m, 2);
                        f.mPolys.resize(ok ? m : 0);
                        for (U16& p : f.mPolys)
                        {
                            ok = ok && sec.pod(p);
                        }
                    }
                }
                else if (t == TAG_LINKS[c])
                {
                    ok = sec.count(n, 29);
                    res.mLinks[c].resize(ok ? n : 0);
                    for (DropLink& l : res.mLinks[c])
                    {
                        U8 kind;
                        sec.vec(l.mFrom);
                        sec.vec(l.mTo);
                        sec.pod(l.mHeight);
                        sec.pod(kind);
                        ok = ok && kind <= static_cast<U8>(LinkKind::ELEVATOR);
                        l.mKind = static_cast<LinkKind>(kind);
                    }
                }
            }
            if (t == TAG_FACES)
            {
                ok = sec.count(n, 26);
                res.mFaces.resize(ok ? n : 0);
                for (WallFace& f : res.mFaces)
                {
                    sec.vec(f.mMin);
                    sec.vec(f.mMax);
                    sec.pod(f.mSide);
                    sec.pod(f.mArea);
                }
            }
            else if (t == TAG_AIR)
            {
                auto air = std::make_shared<TileAir>();
                ok = readAir(sec, *air);
                res.mAir = air;
            }
            // Any other tag is a section this version does not know: skipped.
            if (!ok)
            {
                return false;
            }
        }
        if (!meta)
        {
            return false;
        }
        out = std::move(res);
        return true;
    }

    TileAirPtr coarseAir(const TileAir& air)
    {
        auto out = std::make_shared<TileAir>();
        out->mOrigin = air.mOrigin;
        out->mCell = air.mCell;
        out->mUnit = air.mUnit;
        out->mWidth = air.mWidth;
        out->mHeight = air.mHeight;
        out->mTopCell = air.mTopCell;
        out->mTopWidth = air.mTopWidth;
        out->mTopHeight = air.mTopHeight;
        out->mTopStart = air.mTopStart;
        out->mTop = air.mTop;
        return out;
    }

    TileDiskCache::TileDiskCache(const std::string& dir, U64 cap_bytes) : mDir(dir), mCap(cap_bytes)
    {
    }

    void TileDiskCache::index()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = pathFromUtf8(mDir);
        fs::create_directories(root, ec);
        struct Found
        {
            U64 mKey;
            U64 mBytes;
            fs::file_time_type mTime;
        };
        std::vector<Found> found;
        std::vector<std::string> leftovers;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path& p = it->path();
            // A write a crash cut short never got renamed into place.
            if (p.extension() == ".tmp")
            {
                leftovers.push_back(mDir + "/" + p.filename().string());
                continue;
            }
            if (p.extension() != ".snt")
            {
                continue;
            }
            const std::string stem = p.stem().string();
            char* stop = nullptr;
            const U64 key = std::strtoull(stem.c_str(), &stop, 16);
            std::error_code e2;
            const U64 bytes = it->file_size(e2);
            if (stem.size() != 16 || !stop || *stop || e2)
            {
                continue;
            }
            found.push_back({ key, bytes, it->last_write_time(e2) });
        }
        removeFiles(leftovers);
        // The oldest file was the least recently used last session.
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mTime < b.mTime; });
        std::vector<std::string> doomed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            // Anything stored while this ran is newer than what was found.
            std::unordered_map<U64, Entry> live;
            live.swap(mEntries);
            mBytes = 0;
            for (const Found& f : found)
            {
                if (!live.count(f.mKey))
                {
                    mEntries[f.mKey] = { f.mBytes, ++mClock };
                    mBytes += f.mBytes;
                }
            }
            for (const auto& e : live)
            {
                mEntries[e.first] = { e.second.mBytes, ++mClock };
                mBytes += e.second.mBytes;
            }
            pruneLocked(doomed);
        }
        removeFiles(doomed);
    }

    std::string TileDiskCache::pathOf(U64 key) const
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.snt", static_cast<unsigned long long>(key));
        return mDir + "/" + name;
    }

    bool TileDiskCache::load(U64 key, std::vector<U8>& out)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto it = mEntries.find(key);
            if (it == mEntries.end())
            {
                return false;
            }
            it->second.mUsed = ++mClock;
        }
        std::ifstream file(pathFromUtf8(pathOf(key)), std::ios::binary);
        if (!file)
        {
            erase(key);
            return false;
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        out.resize(size > 0 ? static_cast<size_t>(size) : 0);
        if (size <= 0 || !file.read(reinterpret_cast<char*>(out.data()), size))
        {
            erase(key);
            return false;
        }
        file.close();
        // Its time on disk is what orders it next session.
        std::error_code ec;
        std::filesystem::last_write_time(pathFromUtf8(pathOf(key)), std::filesystem::file_time_type::clock::now(), ec);
        return true;
    }

    bool TileDiskCache::store(U64 key, const std::vector<U8>& bytes)
    {
        namespace fs = std::filesystem;
        const std::string path = pathOf(key);
        // Written aside and renamed, so a reader never sees half a file.
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), ".%zx.tmp", std::hash<std::thread::id>()(std::this_thread::get_id()));
        const fs::path tmp = pathFromUtf8(path + suffix);
        {
            std::error_code dir_ec;
            fs::create_directories(pathFromUtf8(mDir), dir_ec);     // index() may not have run yet
        }
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
            {
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, pathFromUtf8(path), ec);
        if (ec)
        {
            fs::remove(tmp, ec);
            return false;
        }
        std::vector<std::string> doomed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            Entry& e = mEntries[key];
            mBytes = mBytes - e.mBytes + bytes.size();
            e.mBytes = bytes.size();
            e.mUsed = ++mClock;
            pruneLocked(doomed);
        }
        removeFiles(doomed);
        return true;
    }

    void TileDiskCache::erase(U64 key)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto it = mEntries.find(key);
            if (it != mEntries.end())
            {
                mBytes -= it->second.mBytes;
                mEntries.erase(it);
            }
        }
        removeFiles({ pathOf(key) });
    }

    bool TileDiskCache::contains(U64 key) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEntries.count(key) != 0;
    }

    void TileDiskCache::setCap(U64 cap_bytes)
    {
        std::vector<std::string> doomed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mCap = cap_bytes;
            pruneLocked(doomed);
        }
        removeFiles(doomed);
    }

    U64 TileDiskCache::bytes() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mBytes;
    }

    size_t TileDiskCache::entries() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEntries.size();
    }

    size_t TileDiskCache::pendingRemovals() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mDoomed.size();
    }

    void TileDiskCache::pruneLocked(std::vector<std::string>& doomed)
    {
        // Files a remove failed on before are tried again with every prune.
        doomed.insert(doomed.end(), mDoomed.begin(), mDoomed.end());
        mDoomed.clear();
        if (mBytes <= mCap)
        {
            return;
        }
        std::vector<std::pair<U64, U64>> order;     // used, key
        order.reserve(mEntries.size());
        for (const auto& e : mEntries)
        {
            order.emplace_back(e.second.mUsed, e.first);
        }
        std::sort(order.begin(), order.end());
        for (const auto& o : order)
        {
            if (mBytes <= mCap)
            {
                break;
            }
            const auto it = mEntries.find(o.second);
            mBytes -= it->second.mBytes;
            mEntries.erase(it);
            doomed.push_back(pathOf(o.second));
        }
    }

    void TileDiskCache::removeFiles(const std::vector<std::string>& paths)
    {
        std::vector<std::string> failed;
        for (const std::string& path : paths)
        {
            std::error_code ec;
            if (!std::filesystem::remove(pathFromUtf8(path), ec) && ec)
            {
                failed.push_back(path);
            }
        }
        if (!failed.empty())
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mDoomed.insert(mDoomed.end(), failed.begin(), failed.end());
        }
    }

    F64 accumulateNearness(F64 score, F64 distance, F64 radius, F64 dt, F64 decay_seconds)
    {
        const F64 r = std::max(radius, 1.0);
        return score * std::exp(-dt / std::max(decay_seconds, 1e-3)) + dt * r / (r + std::max(distance, 0.0));
    }

    std::vector<size_t> chooseEvictions(const std::vector<EvictionCandidate>& candidates, size_t budget_bytes, F64 protect_distance)
    {
        size_t total = 0;
        for (const EvictionCandidate& c : candidates)
        {
            total += c.mBytes;
        }
        std::vector<size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const EvictionCandidate& x = candidates[a];
            const EvictionCandidate& y = candidates[b];
            return x.mScore != y.mScore ? x.mScore < y.mScore : x.mDistance > y.mDistance;
        });
        std::vector<size_t> out;
        for (size_t i : order)
        {
            if (total <= budget_bytes)
            {
                break;
            }
            if (candidates[i].mDistance < protect_distance)
            {
                continue;
            }
            total -= candidates[i].mBytes;
            out.push_back(i);
        }
        return out;
    }
}
