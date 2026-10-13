/**
 * @file sknavspaces.cpp
 * @brief A mesh's air cut into spaces at door, stair and opening portals.
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

#include "sknavspaces.h"

#include "sknavstairalign.h"
#include "sknavstamp.h"
#include "sknavprofile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace sknav;

    constexpr U32 NONE = 0xffffffffu;
    constexpr S32 OPEN_TOP = INT_MAX / 4;   ///< an open ceiling, in mesh units
    constexpr S32 GROW_STEPS = 64;          ///< a watershed level grows its regions this far before seeding new ones
    constexpr S32 MERGE_ROUNDS = 50;
    constexpr F32 OPEN_HEIGHT_M = 20.f;     ///< what an open ceiling counts for in a volume
    constexpr F32 LIFT_TALL_M = 5.f;        ///< a shaft behind a door has this much headroom
    constexpr F32 LIFT_REACH_M = 3.f;
    constexpr F32 LIFT_SHAFT_M = 10.f;      ///< air this tall runs up through storeys, and may see a little more
    constexpr F32 LIFT_STACK_M = 0.5f;      ///< a census shaft thinner than this is a stack of doors, not the shaft beside it
    constexpr F32 OPEN_MIN_S = 0.25f;       ///< unroofed air seeing this much sky is never indoors
    constexpr F32 OPEN_MAX_COVER = 0.05f;
    constexpr F64 ROOF_MIN_M2 = 2.0;        ///< a smaller patch of floor no space steps onto stays in none
    constexpr F64 ROOF_OPEN_SHARE = 0.5;    ///< a roof's floor this much under open sky
    constexpr F64 ROOF_COVER_SHARE = 0.5;   ///< and this much over roofed floors
    constexpr F32 ROOF_COVERS_SHARE = 0.25f;    ///< a roof covers a space it lies over this much of
    constexpr F32 UNDER_FLIGHT_SHARE = 0.3f;    ///< a space this much under an outdoor flight's air is sheltered by it
    constexpr F32 UNDER_FLIGHT_REACH_M = 2.f;   ///< and opens onto ground this far off across low headroom
    constexpr F32 RASTER_XY_M = 0.5f;
    constexpr F32 RASTER_Z_M = 1.f;
    constexpr U32 STAIR_RUN_EDGES = 3;
    constexpr F32 WALL_END_REACH = 2.5f;    ///< facing wall ends this many agent heights apart may hold a cut
    constexpr F32 WALL_END_RUN_M = 0.5f;    ///< a wall runs this far behind its end, or it is a post
    constexpr S32 STAIR_END_UNITS = 2;      ///< a floor this close to a tread it meets is at an end of the flight
    constexpr F32 SCORE_REACH_M = 2.5f;     ///< a cut's probes land no further than this from it
    constexpr F32 JAMB_TOP_M = 1.5f;        ///< a jamb stands solid from a step above the floor to this
    constexpr S32 APERTURE_ANGLES = 18;     ///< chords tried within 45 degrees either side of a cut
    constexpr F32 APERTURE_MAX_M = 3.f;     ///< a doorway cut from its lintel is no wider than this
    constexpr F32 APERTURE_HEAD_M = 3.5f;   ///< and its lintel no higher than this over its sill
    constexpr F32 JAMB_WALL_M = 0.75f;      ///< a jamb's wall is thinner than twice this across the cut
    constexpr F32 JAMB_RUN_M = 1.f;         ///< and runs on this far along it past the jamb, or the jamb is a post
    constexpr F32 MEMO_BLOCK_SHARE = 0.5f;  ///< a pass cutting more of its mesh's tiles than this reads no memo

    enum Stage : S32
    {
        STAGE_PREPARE = 0,
        STAGE_BATCHES,      ///< cells, the transform and the watershed, a block of moved tiles at a time
        STAGE_SEEDS,
        STAGE_MERGE,
        STAGE_SPACES,
        STAGE_IDS,
        STAGE_DONE,
    };

    enum SideKind : U8
    {
        SIDE_LINKED = 0,
        SIDE_WALL = 1,      ///< no floor to step to
        SIDE_UNKNOWN = 2,   ///< past the cells the pass holds: neither
    };

    // The oracle's order: +x, -x, +y, -y.
    const S32 DX[4] = { 1, -1, 0, 0 };
    const S32 DY[4] = { 0, 0, 1, -1 };

    struct UnionFind
    {
        std::vector<U32> mParent;

        void reset(size_t n)
        {
            mParent.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                mParent[i] = static_cast<U32>(i);
            }
        }

        U32 find(U32 i)
        {
            while (mParent[i] != i)
            {
                mParent[i] = mParent[mParent[i]];
                i = mParent[i];
            }
            return i;
        }

        /// The smaller root survives, as in the oracle.
        bool unite(U32 a, U32 b)
        {
            a = find(a);
            b = find(b);
            if (a == b)
            {
                return false;
            }
            if (a < b)
            {
                mParent[b] = a;
            }
            else
            {
                mParent[a] = b;
            }
            return true;
        }
    };

    struct DoorSpan
    {
        LLVector2 mA;
        LLVector2 mB;
        LLVector2 mC;
        LLVector2 mD;       ///< along the opening
        LLVector2 mN;       ///< through it
        F32 mLo = 0.f;      ///< mesh-local z
        F32 mHi = 0.f;
        F32 mWidth = 0.f;
        LLUUID mId;
    };

    /// Per seed, what spaces are measured from.
    struct SeedInfo
    {
        U32 mTile = 0;
        U32 mLocal = 0;
        bool mFlight = false;
        F64 mWeight = 0.0;      ///< m² of leaf gaps
        F64 mS = 0.0;
        F64 mCover = 0.0;
        F64 mSideOpen = 0.0;
        F64 mE = 0.0;
        F64 mKnown = 0.0;
        F32 mEMin = 1.f;
        F32 mEMax = 0.f;
        F64 mVolume = 0.0;
        F64 mCeilSum = 0.0;
        F64 mCeilArea = 0.0;
        F32 mCeilMin = 1e30f;
        F32 mCeilMax = -1e30f;
        F64 mOpenArea = 0.0;
    };

    struct Edge
    {
        U32 mA;
        U32 mB;
        LLVector2 mMid;
        LLVector2 mCellA;
        LLVector2 mCellB;
        S32 mFloorA;
        S32 mFloorB;
        U32 mTileA;
        U32 mGapA;
        U32 mTileB;
        U32 mGapB;
        bool mFlight;       ///< either side is on a flight
    };

    struct Run
    {
        LLVector2 mC;
        LLVector2 mA;
        LLVector2 mB;
        LLVector2 mD;
        LLVector2 mN;       ///< from the first region into the second
        F32 mWidth = 0.f;
        S32 mFloor = 0;
        U32 mEdges = 0;
        bool mFlight = false;
        U64 mKey = 0;
        std::vector<U32> mIds;  ///< its edges
    };

    struct Scored
    {
        bool mPortal = false;
        PortalKind mKind = PortalKind::OPENING;
        U8 mCues = 0;
        F32 mHead = 0.f;
        S32 mDoor = -1;
        bool mAperture = false;     ///< solid jambs either side: the cut spans mJambA to mJambB
        LLVector2 mJambA;
        LLVector2 mJambB;
    };

    struct Probe
    {
        U32 mTile = 0;
        U32 mGap = 0;
        S32 mFloor = 0;
        S32 mCeil = 0;
    };

    constexpr U64 RASTER_Z = 0x1fffff;
    constexpr U64 RASTER_Y = RASTER_Z << 21;
    constexpr U64 RASTER_YZ = RASTER_Y | RASTER_Z;

    U64 rasterKey(F32 x, F32 y, F32 z)
    {
        const U64 bias = 1ull << 20;
        const U64 kx = static_cast<U64>(static_cast<S64>(std::floor(x / RASTER_XY_M)) + static_cast<S64>(bias)) & 0x1fffff;
        const U64 ky = static_cast<U64>(static_cast<S64>(std::floor(y / RASTER_XY_M)) + static_cast<S64>(bias)) & 0x1fffff;
        const U64 kz = static_cast<U64>(static_cast<S64>(std::floor(z / RASTER_Z_M)) + static_cast<S64>(bias)) & 0x1fffff;
        return (kx << 42) | (ky << 21) | kz;
    }

    /// A 64-bit finaliser: every key bit reaches the low bits a table indexes by.
    U64 spread(U64 x)
    {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    /// Sort a raster by key and sum the counts of equal keys.
    void sumRaster(std::vector<std::pair<U64, U32>>& raster)
    {
        std::sort(raster.begin(), raster.end(), [](const std::pair<U64, U32>& a, const std::pair<U64, U32>& b) { return a.first < b.first; });
        size_t n = 0;
        for (size_t i = 0; i < raster.size(); ++i)
        {
            if (n && raster[n - 1].first == raster[i].first)
            {
                raster[n - 1].second += raster[i].second;
            }
            else
            {
                raster[n++] = raster[i];
            }
        }
        raster.resize(n);
    }

    F32 pointSegment(const LLVector2& p, const LLVector2& a, const LLVector2& b)
    {
        const LLVector2 ab = b - a;
        const F32 l2 = ab * ab;
        const F32 t = l2 > 1e-12f ? llclamp(((p - a) * ab) / l2, 0.f, 1.f) : 0.f;
        return dist_vec(p, a + ab * t);
    }

    F32 cross2(const LLVector2& a, const LLVector2& b)
    {
        return a.mV[VX] * b.mV[VY] - a.mV[VY] * b.mV[VX];
    }

    F32 segmentSegment(const LLVector2& a, const LLVector2& b, const LLVector2& c, const LLVector2& d)
    {
        const F32 d1 = cross2(b - a, c - a), d2 = cross2(b - a, d - a), d3 = cross2(d - c, a - c), d4 = cross2(d - c, b - c);
        if (((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f)))
        {
            return 0.f;
        }
        return std::min(std::min(pointSegment(a, c, d), pointSegment(b, c, d)), std::min(pointSegment(c, a, b), pointSegment(d, a, b)));
    }

    F32 median(std::vector<F32>& v)
    {
        if (v.empty())
        {
            return std::numeric_limits<F32>::quiet_NaN();
        }
        const size_t m = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + m, v.end());
        if (v.size() & 1)
        {
            return v[m];
        }
        const F32 hi = v[m];
        return 0.5f * (hi + *std::max_element(v.begin(), v.begin() + m));
    }

    /// The long axis of a 2x2 covariance.
    LLVector2 principal(F64 sxx, F64 sxy, F64 syy, F64& l1, F64& l2)
    {
        const F64 tr = sxx + syy, det = sxx * syy - sxy * sxy;
        const F64 disc = std::sqrt(std::max(0.0, tr * tr * 0.25 - det));
        l1 = tr * 0.5 + disc;
        l2 = std::max(0.0, tr * 0.5 - disc);
        LLVector2 v;
        if (std::fabs(sxy) > 1e-12)
        {
            v.set(static_cast<F32>(l1 - syy), static_cast<F32>(sxy));
        }
        else
        {
            v = sxx >= syy ? LLVector2(1.f, 0.f) : LLVector2(0.f, 1.f);
        }
        v.normVec();
        return v;
    }

    /// Doors as apertures in the mesh's frame, collinear leaves paired into one.
    std::vector<DoorSpan> doorSpans(const std::vector<DoorEntity>& doors, const LLVector3d& origin)
    {
        std::vector<DoorSpan> spans;
        for (const DoorEntity& d : doors)
        {
            DoorSpan s;
            s.mId = d.mId;
            if (d.mPlaneKnown && d.mWidth > 0.f)
            {
                const LLVector3d p = d.mPlanePoint - origin;
                s.mN.set(d.mNormal.mV[VX], d.mNormal.mV[VY]);
                if (s.mN.normVec() < 1e-6f)
                {
                    s.mN.set(1.f, 0.f);
                }
                s.mD.set(-s.mN.mV[VY], s.mN.mV[VX]);
                s.mC.set(static_cast<F32>(p.mdV[VX]), static_cast<F32>(p.mdV[VY]));
                s.mWidth = d.mWidth;
                s.mLo = static_cast<F32>(d.mSill - origin.mdV[VZ]);
                s.mHi = s.mLo + d.mHeight;
            }
            else
            {
                // No rest pose: the box's long horizontal axis stands for the opening.
                const LLVector3d lo = d.mMin - origin, hi = d.mMax - origin;
                const F32 ex = static_cast<F32>(hi.mdV[VX] - lo.mdV[VX]), ey = static_cast<F32>(hi.mdV[VY] - lo.mdV[VY]);
                s.mD = ex >= ey ? LLVector2(1.f, 0.f) : LLVector2(0.f, 1.f);
                s.mN.set(-s.mD.mV[VY], s.mD.mV[VX]);
                s.mC.set(static_cast<F32>((lo.mdV[VX] + hi.mdV[VX]) * 0.5), static_cast<F32>((lo.mdV[VY] + hi.mdV[VY]) * 0.5));
                s.mWidth = std::max(ex, ey);
                s.mLo = static_cast<F32>(lo.mdV[VZ]);
                s.mHi = static_cast<F32>(hi.mdV[VZ]);
            }
            s.mA = s.mC - s.mD * (s.mWidth * 0.5f);
            s.mB = s.mC + s.mD * (s.mWidth * 0.5f);
            spans.push_back(s);
        }
        // Double doors: leaves in one plane at one sill, at most 1.2 m apart.
        std::vector<DoorSpan> out;
        std::vector<bool> used(spans.size(), false);
        for (size_t i = 0; i < spans.size(); ++i)
        {
            if (used[i])
            {
                continue;
            }
            DoorSpan p = spans[i];
            std::vector<size_t> group(1, i);
            for (size_t j = i + 1; j < spans.size(); ++j)
            {
                const DoorSpan& q = spans[j];
                if (used[j] || std::fabs(q.mLo - p.mLo) > 0.3f || std::fabs(p.mN * q.mN) < 0.95f || std::fabs((q.mC - p.mC) * p.mN) > 0.25f)
                {
                    continue;
                }
                F32 tp0 = p.mA * p.mD, tp1 = p.mB * p.mD, tq0 = q.mA * p.mD, tq1 = q.mB * p.mD;
                if (tp0 > tp1) std::swap(tp0, tp1);
                if (tq0 > tq1) std::swap(tq0, tq1);
                if (std::max(tp0, tq0) - std::min(tp1, tq1) <= 1.2f)
                {
                    group.push_back(j);
                    used[j] = true;
                }
            }
            if (group.size() > 1)
            {
                F32 t0 = 1e30f, t1 = -1e30f, lo = p.mLo, hi = p.mHi;
                for (size_t k : group)
                {
                    const DoorSpan& q = spans[k];
                    for (const LLVector2& v : { q.mA, q.mB })
                    {
                        const F32 t = (v - p.mC) * p.mD;
                        t0 = std::min(t0, t);
                        t1 = std::max(t1, t);
                    }
                    lo = std::min(lo, q.mLo);
                    hi = std::max(hi, q.mHi);
                }
                const LLVector2 c = p.mC;
                p.mA = c + p.mD * t0;
                p.mB = c + p.mD * t1;
                p.mC = (p.mA + p.mB) * 0.5f;
                p.mWidth = t1 - t0;
                p.mLo = lo;
                p.mHi = hi;
            }
            out.push_back(p);
        }
        return out;
    }

    /// Two 64-bit chains in which every input bit reaches every output bit: what a memo entry is trusted by.
    struct MemoKey
    {
        U64 mA = SEED;
        U64 mB = 0x9e3779b97f4a7c15ull;

        void feed(U64 v)
        {
            mA = spread(mA ^ spread(v + 0x2545f4914f6cdd1dull));
            mB = spread(mB + ((v << 29) | (v >> 35)) * 0xff51afd7ed558ccdull);
        }
        void feed(F32 f)
        {
            U32 bits;
            std::memcpy(&bits, &f, sizeof(bits));
            feed(static_cast<U64>(bits));
        }
        void feed(const LLVector2& v)
        {
            feed(v.mV[VX]);
            feed(v.mV[VY]);
        }
    };
}

namespace sknav
{
    struct MergeMemo
    {
        struct Runs
        {
            U64 mCheck = 0;
            std::vector<Run> mRuns;     ///< mIds are positions in the group, mKey unset
        };
        struct Score
        {
            U64 mCheck = 0;
            Scored mScored;
        };
        U64 mContext = 0;               ///< parameters and doors: a memo made under others is not read
        std::unordered_map<U64, Runs> mRuns;
        std::unordered_map<U64, Score> mScores;

        size_t bytes() const
        {
            size_t n = sizeof(MergeMemo) + mScores.size() * (sizeof(Score) + 2 * sizeof(U64));
            for (const auto& r : mRuns)
            {
                n += sizeof(Runs) + 2 * sizeof(U64) + r.second.mRuns.capacity() * sizeof(Run);
                for (const Run& run : r.second.mRuns)
                {
                    n += run.mIds.capacity() * sizeof(U32);
                }
            }
            return n;
        }
    };

    struct FloorMemo
    {
        struct Comp
        {
            U32 mCells = 0;
            U32 mOpen = 0;              ///< sides with no floor to step to
            U32 mDrops = 0;             ///< of those, sides over a floor well below
            bool mEdge = false;         ///< a side onto a tile without detail
        };
        /// A link from one of the tile's cells to a neighbour's floor cell.
        struct Cross
        {
            U32 mComp;
            U8 mDir;
            U16 mX, mY;                 ///< the neighbour's fine cell
            U32 mGap;                   ///< the neighbour's gap
        };
        static U64 cellKey(S32 x, S32 y, U32 gap) { return (static_cast<U64>(static_cast<U16>(x)) << 48) | (static_cast<U64>(static_cast<U16>(y)) << 32) | gap; }
        struct Tile
        {
            TileAirPtr mAir;
            TileAirPtr mNear[4];        ///< the neighbours' airs, by DX/DY
            S8 mNearState[4] = {};      ///< 0 held, -1 past the mesh, -2 without detail
            std::vector<Comp> mComps;
            std::vector<Cross> mCross;
            std::vector<std::pair<U64, U32>> mBorder;   ///< the edge columns' floor cells by cellKey, sorted, to their components

            U32 borderComp(S32 x, S32 y, U32 gap) const
            {
                const U64 k = cellKey(x, y, gap);
                const auto it = std::lower_bound(mBorder.begin(), mBorder.end(), std::make_pair(k, U32(0)));
                return it != mBorder.end() && it->first == k ? it->second : U32(~0u);
            }
            size_t bytes() const
            {
                return sizeof(Tile) + mComps.capacity() * sizeof(Comp) + mCross.capacity() * sizeof(Cross) + mBorder.capacity() * sizeof(std::pair<U64, U32>);
            }
        };
        S32 mClimb = 0, mWalkHead = 0, mWalkOverlap = 0;
        std::map<TileKey, std::shared_ptr<const Tile>> mTiles;

        size_t bytes() const
        {
            size_t n = sizeof(FloorMemo);
            for (const auto& t : mTiles)
            {
                n += sizeof(TileKey) + 32 + t.second->bytes();
            }
            return n;
        }
    };

    const char* portalKindName(PortalKind kind)
    {
        switch (kind)
        {
            case PortalKind::DOOR: return "door";
            case PortalKind::STAIR: return "stair";
            default: return "opening";
        }
    }

    const Space* RoomsProduct::byId(U32 id) const
    {
        for (const Space& s : mSpaces)
        {
            if (s.mId == id)
            {
                return &s;
            }
        }
        return nullptr;
    }

    const RoomsTile* RoomsProduct::tile(const TileKey& key) const
    {
        const auto it = std::lower_bound(mTiles.begin(), mTiles.end(), key, [](const RoomsTile& t, const TileKey& k) { return t.mKey < k; });
        return it != mTiles.end() && it->mKey == key ? &*it : nullptr;
    }

    U64 RoomsProduct::stampKey(const TileKey& key) const
    {
        const auto it = std::lower_bound(mStampKeys.begin(), mStampKeys.end(), key, [](const std::pair<TileKey, U64>& t, const TileKey& k) { return t.first < k; });
        return it != mStampKeys.end() && it->first == key ? it->second : 0;
    }

    size_t RoomsProduct::bytes() const
    {
        size_t n = sizeof(RoomsProduct) + mPortals.capacity() * sizeof(Portal) + mApart.capacity() * sizeof(SeedLink);
        for (const Space& s : mSpaces)
        {
            n += sizeof(Space) + (s.mPortals.capacity() + s.mCovers.capacity() + s.mCoverM2.capacity()) * sizeof(U32) + s.mRaster.capacity() * sizeof(std::pair<U64, U32>);
        }
        for (const RoomsTile& t : mTiles)
        {
            n += sizeof(RoomsTile) + t.mSeeds.capacity() * sizeof(U16);
            for (const SeedStats& s : t.mStats)
            {
                n += sizeof(SeedStats) + s.mRaster.capacity() * sizeof(std::pair<U64, U32>);
            }
        }
        n += mStampKeys.capacity() * sizeof(std::pair<TileKey, U64>);
        return n + (mMemo ? mMemo->bytes() : 0) + (mFloors ? mFloors->bytes() : 0) + (mDraw ? mDraw->bytes() : 0);
    }

    struct SpacesPass::State
    {
        struct TileInfo
        {
            TileKey mKey;
            TileAirPtr mAir;
            U32 mEnc = 0;               ///< index into the enclosure product's tiles
            S32 mZOff = 0;              ///< its units from the mesh's z
            bool mDirty = true;
            bool mInBlock = false;      ///< cut by the batch running now
            bool mWork = false;         ///< in its cells: the block and the ring round it
            S32 mBatch = -1;            ///< the batch that cut its seeds this pass
            U32 mSeedBase = 0;
            std::vector<S32> mLeafMap;  ///< per fine cell, its leaf or -1
            std::vector<U32> mColStart; ///< work tiles: per fine cell, its first floor cell
            std::vector<S32> mSeedLabels;   ///< per seed cut this pass, its watershed label
            RoomsTile mRooms;
        };

        struct Cell
        {
            U32 mTile;
            U16 mX;
            U16 mY;
            U32 mGap;
            S32 mFloor;
            S32 mCeil;
            bool mFlight;
        };

        std::shared_ptr<const RoomsInput> mInput;
        std::shared_ptr<EnclosureProduct> mEnc;
        std::shared_ptr<RoomsProduct> mProduct;
        S32 mStage = STAGE_PREPARE;
        F64 mStageMs[NUM_STAGES] = {};

        F32 mCell = CELL_M;
        F32 mUnit = CELL_M;
        S32 mClimb = 6;
        S32 mWalkHead = 8;
        S32 mWalkOverlap = 8;

        std::vector<TileInfo> mTiles;
        /// Tile indices over the box of the mesh's keys, NONE where it has none.
        std::vector<U32> mTileGrid;
        S32 mGridX = 0, mGridY = 0, mGridW = 0, mGridH = 0;
        U32 tileAt(const TileKey& k) const
        {
            const S32 x = k.mX - mGridX, y = k.mY - mGridY;
            return x < 0 || y < 0 || x >= mGridW || y >= mGridH ? NONE : mTileGrid[static_cast<size_t>(y) * mGridW + x];
        }
        /// Tiles whose air moved, cut together with the ring round them;
        /// a tile a cut sees a seam to disagree with is cut again.
        std::vector<std::vector<U32>> mBatches;
        size_t mBatch = 0;
        S32 mLabelBase = 0;             ///< watershed labels are numbered across batches
        /// Per batch, the label it gave a ring tile's gap along a tile edge:
        /// what tells a seam its two sides are one basin.
        std::vector<std::unordered_map<U64, S32>> mShadow;
        /// Per batch, the label it gave a ring tile's gap along an edge with another ring tile: what tells kept seeds the cut parts or joins.
        std::vector<std::unordered_map<U64, S32>> mSeamShadow;

        std::vector<Cell> mCells;
        std::vector<U32> mLink;         ///< 4 per cell
        std::vector<U8> mSide;          ///< 4 per cell
        std::vector<F32> mDist;
        std::vector<U8> mAvatar;
        std::vector<S32> mLabel;

        std::vector<SeedInfo> mSeeds;
        std::vector<S32> mSeedLabel;    ///< the watershed label a freshly cut seed came from, else -1
        mutable UnionFind mRegions;     ///< seeds to regions; runs() reads it through find()
        std::vector<Edge> mEdges;
        std::vector<DoorSpan> mDoors;
        std::vector<Portal> mPortals;   ///< spaces hold region roots until the spaces stage
        std::vector<S32> mPortalDoor;   ///< per portal, its door span, or -1
        std::unordered_map<U64, Scored> mScored;
        const MergeMemo* mOldMemo = nullptr;    ///< the last pass's, when this one may read it
        std::shared_ptr<MergeMemo> mMemo;       ///< what this pass used, for the next
        std::vector<U64> mTileKey;              ///< per tile, its air and indoorness, as a score reads them
        std::vector<MemoKey> mEdgeKey;          ///< per edge, what its runs read of it
        std::vector<LLVector3> mOldPortals;
        /// Old portals and doors by the square metre cells they reach, so a cut scores against those near it only.
        std::unordered_map<U64, std::vector<U32>> mOldGrid;
        std::unordered_map<U64, std::vector<U32>> mDoorGrid;
        static U64 gridKey(S32 x, S32 y) { return (static_cast<U64>(static_cast<U32>(x)) << 32) | static_cast<U32>(y); }

        // spaces
        std::vector<U32> mSeedSpace;    ///< per seed, its space + 1
        std::vector<std::vector<U32>> mGapSpace;    ///< per tile, per gap, space + 1

        const SpaceParams& params() const { return mInput->mParams; }
        const TileAir& air(U32 t) const { return *mTiles[t].mAir; }

        F32 cellX(const TileInfo& t, S32 ix) const { return t.mAir->mOrigin.mV[VX] + (ix + 0.5f) * mCell; }
        F32 cellY(const TileInfo& t, S32 iy) const { return t.mAir->mOrigin.mV[VY] + (iy + 0.5f) * mCell; }
        S32 floorOf(U32 t, const AirGap& g) const { return g.mFloor + mTiles[t].mZOff; }
        /// A column's k-th floor at its cell, on its plane.
        S32 floorAt(U32 t, const AirColumn& col, U32 k, S32 ix, S32 iy) const { return col.floorAt(*mTiles[t].mAir, k, ix, iy) + mTiles[t].mZOff; }
        S32 ceilOf(U32 t, const AirGap& g) const
        {
            return (g.mFlags & AirGap::OPEN) || g.mCeiling == AIR_CEILING_OPEN ? OPEN_TOP : g.mCeiling + mTiles[t].mZOff;
        }
        bool floorGap(U32 t, const AirGap& g) const { return (g.mFlags & AirGap::TOP) && ceilOf(t, g) - floorOf(t, g) >= mWalkHead; }

        /// The tile and fine cell a (possibly out of range) cell of tile t is,
        /// or -1 past the mesh's tiles and -2 in a tile without detail.
        S32 resolve(U32 t, S32 ix, S32 iy, U32& tile_out, S32& x_out, S32& y_out) const;
        /// The gaps over a fine cell: count 0 when it has none.
        U32 column(U32 t, S32 ix, S32 iy, AirColumn& out) const;
        bool probe(F32 x, F32 y, S32 floor, S32 tol, Probe& out) const;
        /// Square each stair portal to the treads of the flight it enters (sknavstairalign.h).
        void squareStairPortals(std::vector<StairEnd>& ends, const std::vector<size_t>& portals);
        /// Ground under an outdoor flight with no portal opens onto the ground it reaches under the flight's low end.
        void joinUnderFlights(std::vector<Space>& out);
        /// Whether solid stands between lo and hi (mesh units) over a fine cell.
        bool blocked(U32 t, S32 ix, S32 iy, S32 lo, S32 hi) const;
        /// Whether solid stands between lo and hi (mesh units) at a point.
        bool solidAt(F32 x, F32 y, S32 lo, S32 hi) const;
        /// The aperture a cut lies in: the shortest chord through it between solid jambs, framed for callers trying many cuts on few bearings.
        /// With true_faces the chord runs between the jambs' true faces (sknavfaceoffsets.h), not their cells'.
        bool aperture(const Run& run, LLVector2& a, LLVector2& b, bool framed = false, bool true_faces = false) const;
        /// A ray's cell face at t moved to the wall's true face, by the air's face offsets for a floor (mesh units).
        F32 trueFace(const LLVector2& from, const LLVector2& dir, F32 t, S32 floor) const;
        static constexpr S32 RAYS = 2 * APERTURE_ANGLES + 1;
        static constexpr S32 WINDOW = 64;       ///< cells a side of a window, a row of them to a word
        static constexpr size_t RAY_FANS = 4;
        static constexpr S32 BAND = 4;          ///< samples a march reads the cells of at once
        /// How many cells round its centre's a ray's samples can be read from.
        S32 frameReach() const { return static_cast<S32>(std::ceil(SCORE_REACH_M / mCell)) + 2; }
        /// The rays aperture tries about a bearing, and as rows of bits the cells round the centre's each side's and each band's samples can be read from.
        struct RayFan
        {
            F32 mBase = 0.f;
            std::array<LLVector2, RAYS> mDirs;
            S32 mBandCount = 0;
            std::vector<U64> mMasks;                    ///< per mask and row
            std::vector<std::pair<S32, S32>> mSpans;    ///< per mask, its first and last row
            S32 sideMask(S32 side) const { return side; }
            S32 bandMask(S32 side, S32 i, S32 band) const { return 2 + (side * RAYS + i) * mBandCount + band; }
            /// The band's samples and all nearer.
            S32 reachMask(S32 side, S32 i, S32 band) const { return 2 + (2 * RAYS + side * RAYS + i) * mBandCount + band; }
        };
        mutable std::vector<RayFan> mFans;      ///< for the last few bearings
        mutable size_t mFanNext = 0;
        const RayFan& rayFan(F32 base) const;
        /// Where solid stands between lo and hi over a window of a tile's cells, a row of bits a word.
        struct Window
        {
            U32 mTile = NONE;
            const TileAir* mAir = nullptr;
            S32 mX0 = 0, mY0 = 0;
            U64 mRows[WINDOW];
        };
        mutable Window mWindows[4];
        mutable S32 mWindowCount = 0, mWindowsLo = INT_MIN, mWindowsHi = INT_MIN;
        /// Where a centre's frame of reach cells either side lies in the windows of the tiles its samples can read.
        struct Frame
        {
            struct Place
            {
                S32 mWindow, mX, mY;
            };
            Place mPlaces[4];
            S32 mCount = 0;
            bool mOneTile = false;      ///< every sample reads the one tile
        };
        void frameAt(const LLVector2& c, S32 lo, S32 hi, Frame& frame) const;
        /// Where solid stands between lo and hi over a tile's cells, filled eight by eight cells as first read.
        struct SolidMap
        {
            S32 mWords = 0, mBlocksW = 0;
            std::vector<U64> mBits;
            std::vector<U8> mFilled;    ///< per block of eight by eight cells
            /// Cells x0 to x1 of row iy, from bit 0.
            U64 row(S32 iy, S32 x0, S32 x1) const;
        };
        struct SolidKey
        {
            U32 mTile;
            S32 mLo, mHi;
            bool operator==(const SolidKey& o) const { return mTile == o.mTile && mLo == o.mLo && mHi == o.mHi; }
        };
        struct SolidKeyHash
        {
            size_t operator()(const SolidKey& k) const
            {
                const U64 h = (static_cast<U64>(k.mTile) * 0x9E3779B97F4A7C15ull) ^ ((static_cast<U64>(static_cast<U32>(k.mLo)) << 32 | static_cast<U32>(k.mHi)) * 0xC2B2AE3D27D4EB4Full);
                return static_cast<size_t>(h ^ (h >> 29));
            }
        };
        mutable std::unordered_map<SolidKey, SolidMap, SolidKeyHash> mSolidMaps;
        /// The tile's solid map, its cells x0 to x1 by y0 to y1 filled.
        const SolidMap& solidMap(U32 t, S32 x0, S32 x1, S32 y0, S32 y1, S32 lo, S32 hi) const;

        void prepare();
        void runBatch(size_t b);
        bool recut();
        S32 mRecuts = 0;
        void buildCells();
        void transform();
        void cutSeeds(size_t b);
        /// Floors an avatar stands on, judged per walkable component of the whole mesh: each tile's
        /// components and their links out, taken from the last pass while its air and its neighbours' stand.
        std::shared_ptr<FloorMemo> mFloors;
        std::vector<std::shared_ptr<const FloorMemo::Tile>> mFloorTiles;
        std::vector<std::vector<U32>> mFloorCells;  ///< per tile, per floor cell its component, once a cut needs them
        std::vector<U32> mFloorBase;                ///< per tile, its first component in the mesh's
        std::vector<U8> mFloorAvatar;               ///< per component of the mesh
        void floors();
        std::shared_ptr<FloorMemo::Tile> tileFloors(U32 t, std::vector<U32>& cells) const;
        void wallEnds();
        U32 mWallEndCuts = 0;
        void apertureCuts();
        bool jambEnds(const Run& r) const;
        /// The first floor off a cut on one side whose ceiling clears its lintel.
        bool pastLintel(const Run& r, F32 sgn, Probe& out) const;
        /// How far along its normal a cut sits from the middle of the wall its jambs end.
        F32 wallMiddle(const Run& r) const;
        U32 mApertureCuts = 0;
        std::vector<Run> mForced;       ///< aperture cuts, whose sides no merge joins
        /// The floor cell of a work tile at a point, within a climb of floor, or NONE.
        U32 cellAt(const LLVector2& q, S32 floor) const;
        /// Sever the links crossing a cut, noting each for undo when asked.
        void sever(const Run& r, std::vector<std::pair<U32, U32>>* undo);
        void number();
        void unite(U32 a, U32 b);
        void edges();
        void absorbFragments();
        void collectApart();
        bool keptApart(UnionFind& uf, U32 u, U32 v) const;
        /// Drop the cuts that keep two regions apart.
        void releaseApart(UnionFind& uf, U32 u, U32 v);
        std::vector<std::pair<U32, U32>> mApart;   ///< seeds either side of an aperture cut
        void merge();
        bool demoteFlatFlights(F32 min_rise);
        std::vector<Run> runs(const std::vector<U32>& edge_ids, U32 ru) const;
        Scored score(const Run& run);
        Scored scoreFresh(const Run& run, bool relaxed);
        bool relaxedAt(const Run& run) const;
        void memoContext();
        void spaces();
        void ids();
        bool runStage();
    };

    S32 SpacesPass::State::resolve(U32 t, S32 ix, S32 iy, U32& tile_out, S32& x_out, S32& y_out) const
    {
        const TileAir& a = air(t);
        if (ix >= 0 && iy >= 0 && ix < a.mWidth && iy < a.mHeight)
        {
            tile_out = t;
            x_out = ix;
            y_out = iy;
            return 0;
        }
        TileKey key = mTiles[t].mKey;
        if (ix < 0)
        {
            key.mX -= 1;
            ix += a.mWidth;
        }
        else if (ix >= a.mWidth)
        {
            key.mX += 1;
            ix -= a.mWidth;
        }
        if (iy < 0)
        {
            key.mY -= 1;
            iy += a.mHeight;
        }
        else if (iy >= a.mHeight)
        {
            key.mY += 1;
            iy -= a.mHeight;
        }
        if (key == mTiles[t].mKey)
        {
            tile_out = t;
        }
        else
        {
            const U32 it = tileAt(key);
            if (it == NONE)
            {
                const TileEnclosure* kept = mEnc->tile(key);
                return kept ? -2 : -1;
            }
            tile_out = it;
        }
        const TileAir& b = air(tile_out);
        if (ix < 0 || iy < 0 || ix >= b.mWidth || iy >= b.mHeight)
        {
            return -1;
        }
        x_out = ix;
        y_out = iy;
        return 0;
    }

    U32 SpacesPass::State::column(U32 t, S32 ix, S32 iy, AirColumn& out) const
    {
        const TileInfo& ti = mTiles[t];
        airColumn(*ti.mAir, ti.mLeafMap[ix + iy * ti.mAir->mWidth], out);
        return out.mCount;
    }

    bool SpacesPass::State::probe(F32 x, F32 y, S32 floor, S32 tol, Probe& out) const
    {
        TileKey key;
        key.mX = tileIndex(x);
        key.mY = tileIndex(y);
        const U32 t = tileAt(key);
        if (t == NONE)
        {
            return false;
        }
        const TileAir& a = air(t);
        const S32 ix = static_cast<S32>(std::floor((x - a.mOrigin.mV[VX]) / mCell));
        const S32 iy = static_cast<S32>(std::floor((y - a.mOrigin.mV[VY]) / mCell));
        if (ix < 0 || iy < 0 || ix >= a.mWidth || iy >= a.mHeight)
        {
            return false;
        }
        AirColumn col;
        for (U32 i = 0; i < column(t, ix, iy, col); ++i)
        {
            const AirGap& g = a.mGaps[col.mGap[i]];
            const S32 f = floorAt(t, col, i, ix, iy);
            if (f >= floor - tol && f <= floor + tol && floorGap(t, g))
            {
                out.mTile = t;
                out.mGap = col.mGap[i];
                out.mFloor = f;
                out.mCeil = ceilOf(t, g);
                return true;
            }
        }
        return false;
    }

    void SpacesPass::State::squareStairPortals(std::vector<StairEnd>& ends, const std::vector<size_t>& portals)
    {
        if (ends.empty())
        {
            return;
        }
        const S32 tol = std::max(1, static_cast<S32>(std::ceil(STAIR_TREAD_DZ_M / mUnit)));
        const FlightFloor flight_floor = [this, tol](F32 x, F32 y, F32 z, F32& out) {
            TileKey key;
            key.mX = tileIndex(x);
            key.mY = tileIndex(y);
            const U32 t = tileAt(key);
            if (t == NONE)
            {
                return false;
            }
            const TileAir& a = air(t);
            const S32 ix = static_cast<S32>(std::floor((x - a.mOrigin.mV[VX]) / mCell));
            const S32 iy = static_cast<S32>(std::floor((y - a.mOrigin.mV[VY]) / mCell));
            if (ix < 0 || iy < 0 || ix >= a.mWidth || iy >= a.mHeight)
            {
                return false;
            }
            const S32 want = static_cast<S32>(std::lround(z / mUnit));
            S32 best = INT_MAX, found = 0;
            AirColumn col;
            for (U32 i = 0; i < column(t, ix, iy, col); ++i)
            {
                const AirGap& g = a.mGaps[col.mGap[i]];
                const S32 f = floorOf(t, g);
                if ((g.mFlags & AirGap::FLIGHT) && floorGap(t, g) && std::abs(f - want) <= tol && std::abs(f - want) < best)
                {
                    best = std::abs(f - want);
                    found = f;
                }
            }
            if (best == INT_MAX)
            {
                return false;
            }
            out = found * mUnit;
            return true;
        };
        alignStairEnds(flight_floor, ends);
        for (size_t i = 0; i < ends.size(); ++i)
        {
            const StairEnd& e = ends[i];
            if (!e.mAligned)
            {
                continue;
            }
            Portal& q = mPortals[portals[i]];
            const F32 z = q.mSill;
            LLVector2 n(e.mA.mV[VY] - e.mB.mV[VY], e.mB.mV[VX] - e.mA.mV[VX]);
            n.normVec();
            if (n.mV[VX] * q.mNormal.mV[VX] + n.mV[VY] * q.mNormal.mV[VY] < 0.f)
            {
                n = -n;
            }
            const LLVector2 c = (e.mA + e.mB) * 0.5f;
            q.mA.set(e.mA.mV[VX], e.mA.mV[VY], z);
            q.mB.set(e.mB.mV[VX], e.mB.mV[VY], z);
            q.mCentre.set(c.mV[VX], c.mV[VY], z);
            q.mNormal.set(n.mV[VX], n.mV[VY], 0.f);
            q.mWidth = dist_vec(e.mA, e.mB);
        }
    }

    bool SpacesPass::State::blocked(U32 t, S32 ix, S32 iy, S32 lo, S32 hi) const
    {
        U32 tile;
        S32 x, y;
        if (resolve(t, ix, iy, tile, x, y) != 0)
        {
            return false;
        }
        AirColumn col;
        const U32 count = column(tile, x, y, col);
        if (!count)
        {
            return false;   // nothing stands there at all
        }
        const TileAir& a = air(tile);
        for (U32 i = 0; i < count; ++i)
        {
            const AirGap& g = a.mGaps[col.mGap[i]];
            if (floorAt(tile, col, i, x, y) <= lo && ceilOf(tile, g) >= hi)
            {
                return false;
            }
        }
        return true;
    }

    bool SpacesPass::State::solidAt(F32 x, F32 y, S32 lo, S32 hi) const
    {
        TileKey key;
        key.mX = tileIndex(x);
        key.mY = tileIndex(y);
        const U32 t = tileAt(key);
        if (t == NONE)
        {
            return false;
        }
        const TileAir& a = air(t);
        return blocked(t, static_cast<S32>(std::floor((x - a.mOrigin.mV[VX]) / mCell)), static_cast<S32>(std::floor((y - a.mOrigin.mV[VY]) / mCell)), lo, hi);
    }

    const SpacesPass::State::SolidMap& SpacesPass::State::solidMap(U32 t, S32 x0, S32 x1, S32 y0, S32 y1, S32 lo, S32 hi) const
    {
        SolidMap& m = mSolidMaps[SolidKey{ t, lo, hi }];
        const TileAir& a = air(t);
        if (m.mFilled.empty())
        {
            m.mWords = (a.mWidth + 63) >> 6;
            m.mBlocksW = (a.mWidth + 7) >> 3;
            m.mBits.assign(static_cast<size_t>(m.mWords) * a.mHeight, 0);
            m.mFilled.assign(static_cast<size_t>(m.mBlocksW) * ((a.mHeight + 7) >> 3), 0);
        }
        for (S32 by = y0 >> 3; by <= y1 >> 3; ++by)
        {
            for (S32 bx = x0 >> 3; bx <= x1 >> 3; ++bx)
            {
                U8& filled = m.mFilled[static_cast<size_t>(bx + by * m.mBlocksW)];
                if (filled)
                {
                    continue;
                }
                filled = 1;
                for (S32 iy = by * 8; iy < std::min(by * 8 + 8, a.mHeight); ++iy)
                {
                    for (S32 ix = bx * 8; ix < std::min(bx * 8 + 8, a.mWidth); ++ix)
                    {
                        if (blocked(t, ix, iy, lo, hi))
                        {
                            m.mBits[static_cast<size_t>(iy * m.mWords + (ix >> 6))] |= 1ull << (ix & 63);
                        }
                    }
                }
            }
        }
        return m;
    }

    U64 SpacesPass::State::SolidMap::row(S32 iy, S32 x0, S32 x1) const
    {
        const U64* r = &mBits[static_cast<size_t>(iy * mWords)];
        const S32 word = x0 >> 6, off = x0 & 63;
        U64 v = r[word] >> off;
        if (off && word + 1 < mWords)
        {
            v |= r[word + 1] << (64 - off);
        }
        const S32 n = x1 - x0 + 1;
        return n >= 64 ? v : v & ((1ull << n) - 1);
    }

    const SpacesPass::State::RayFan& SpacesPass::State::rayFan(F32 base) const
    {
        for (const RayFan& f : mFans)
        {
            if (f.mBase == base)
            {
                return f;
            }
        }
        RayFan* slot = nullptr;
        if (mFans.size() < RAY_FANS)
        {
            mFans.reserve(RAY_FANS);
            slot = &mFans.emplace_back();
        }
        else
        {
            slot = &mFans[mFanNext];
            mFanNext = (mFanNext + 1) % RAY_FANS;
        }
        RayFan& f = *slot;
        f.mBase = base;
        for (S32 i = 0; i < RAYS; ++i)
        {
            const S32 k = (i & 1) ? (i + 1) / 2 : -(i / 2);
            const F32 th = base + k * (F_PI * 0.25f / APERTURE_ANGLES);
            f.mDirs[i].set(std::cos(th), std::sin(th));
        }
        // A sample is read from the cell its offset from the centre falls in or the next, wherever in its cell the centre lies.
        const S32 reach = frameReach();
        S32 samples = 0;
        for (F32 t = mCell * 0.5f; t <= SCORE_REACH_M; t += mCell * 0.5f)
        {
            ++samples;
        }
        f.mBandCount = (samples + BAND - 1) / BAND;
        const S32 masks = 2 + 4 * RAYS * f.mBandCount;
        f.mMasks.assign(static_cast<size_t>(masks) * WINDOW, 0);
        f.mSpans.assign(masks, std::make_pair(WINDOW, -1));
        for (S32 side = 0; side < 2 && 2 * reach < WINDOW; ++side)
        {
            for (S32 i = 0; i < RAYS; ++i)
            {
                const LLVector2 dir = side ? -f.mDirs[i] : f.mDirs[i];
                S32 j = 0;
                for (F32 t = mCell * 0.5f; t <= SCORE_REACH_M; t += mCell * 0.5f, ++j)
                {
                    const F32 x = dir.mV[VX] * t / mCell, y = dir.mV[VY] * t / mCell;
                    for (S32 dy = static_cast<S32>(std::floor(y - 0.01f)); dy <= static_cast<S32>(std::floor(y + 1.01f)); ++dy)
                    {
                        for (S32 dx = static_cast<S32>(std::floor(x - 0.01f)); dx <= static_cast<S32>(std::floor(x + 1.01f)); ++dx)
                        {
                            auto set = [&](S32 mask) {
                                f.mMasks[static_cast<size_t>(mask * WINDOW + dy + reach)] |= 1ull << (dx + reach);
                                f.mSpans[mask].first = std::min(f.mSpans[mask].first, dy + reach);
                                f.mSpans[mask].second = std::max(f.mSpans[mask].second, dy + reach);
                            };
                            set(f.sideMask(side));
                            set(f.bandMask(side, i, j / BAND));
                            for (S32 band = j / BAND; band < f.mBandCount; ++band)
                            {
                                set(f.reachMask(side, i, band));
                            }
                        }
                    }
                }
            }
        }
        return f;
    }

    void SpacesPass::State::frameAt(const LLVector2& c, S32 lo, S32 hi, Frame& frame) const
    {
        // Windows are kept while frames fall inside them, and built anew round a frame that leaves them.
        const S32 reach = frameReach();
        const F32 r = (reach + 1) * mCell;
        const F32 x0 = c.mV[VX] - r, x1 = c.mV[VX] + r, y0 = c.mV[VY] - r, y1 = c.mV[VY] + r;
        frame.mCount = 0;
        bool fits = mWindowsLo == lo && mWindowsHi == hi;
        for (S32 ky = tileIndex(y0); ky <= tileIndex(y1); ++ky)
        {
            for (S32 kx = tileIndex(x0); kx <= tileIndex(x1); ++kx)
            {
                TileKey key;
                key.mX = kx;
                key.mY = ky;
                const U32 it = tileAt(key);
                if (it == NONE)
                {
                    continue;
                }
                const TileAir& ta = air(it);
                const S32 ic = static_cast<S32>(std::floor((c.mV[VX] - ta.mOrigin.mV[VX]) / mCell));
                const S32 jc = static_cast<S32>(std::floor((c.mV[VY] - ta.mOrigin.mV[VY]) / mCell));
                S32 w = 0;
                while (w < mWindowCount && mWindows[w].mTile != it)
                {
                    ++w;
                }
                fits = fits && w < mWindowCount && ic - reach >= mWindows[w].mX0 && ic + reach < mWindows[w].mX0 + WINDOW &&
                       jc - reach >= mWindows[w].mY0 && jc + reach < mWindows[w].mY0 + WINDOW;
                Frame::Place& place = frame.mPlaces[frame.mCount++];
                place.mWindow = w;
                place.mX = ic - reach;
                place.mY = jc - reach;
            }
        }
        frame.mOneTile = frame.mCount == 1 && tileIndex(x0) == tileIndex(x1) && tileIndex(y0) == tileIndex(y1);
        if (fits)
        {
            for (S32 k = 0; k < frame.mCount; ++k)
            {
                Frame::Place& place = frame.mPlaces[k];
                place.mX -= mWindows[place.mWindow].mX0;
                place.mY -= mWindows[place.mWindow].mY0;
            }
            return;
        }
        mWindowsLo = lo;
        mWindowsHi = hi;
        mWindowCount = 0;
        S32 k = 0;
        for (S32 ky = tileIndex(y0); ky <= tileIndex(y1); ++ky)
        {
            for (S32 kx = tileIndex(x0); kx <= tileIndex(x1); ++kx)
            {
                TileKey key;
                key.mX = kx;
                key.mY = ky;
                const U32 it = tileAt(key);
                if (it == NONE)
                {
                    continue;
                }
                Frame::Place& place = frame.mPlaces[k++];
                place.mWindow = mWindowCount++;
                Window& win = mWindows[place.mWindow];
                const TileAir& ta = air(it);
                win.mTile = it;
                win.mAir = &ta;
                win.mX0 = place.mX - (WINDOW - (2 * reach + 1)) / 2;
                win.mY0 = place.mY - (WINDOW - (2 * reach + 1)) / 2;
                place.mX -= win.mX0;
                place.mY -= win.mY0;
                std::fill(win.mRows, win.mRows + WINDOW, 0ull);
                const S32 gx0 = std::max(win.mX0, 0), gx1 = std::min(win.mX0 + WINDOW - 1, ta.mWidth - 1);
                const S32 gy0 = std::max(win.mY0, 0), gy1 = std::min(win.mY0 + WINDOW - 1, ta.mHeight - 1);
                if (gx0 <= gx1 && gy0 <= gy1)
                {
                    const SolidMap& m = solidMap(it, gx0, gx1, gy0, gy1, lo, hi);
                    for (S32 iy = gy0; iy <= gy1; ++iy)
                    {
                        win.mRows[iy - win.mY0] = m.row(iy, gx0, gx1) << (gx0 - win.mX0);
                    }
                }
                // Off the grid, the cells a point of the tile's footprint can still be read from, through its neighbours.
                const S32 fx0 = std::max(win.mX0, static_cast<S32>(std::floor((static_cast<F32>(kx * TILE_M) - ta.mOrigin.mV[VX]) / mCell)) - 1);
                const S32 fx1 = std::min(win.mX0 + WINDOW - 1, static_cast<S32>(std::floor((static_cast<F32>((kx + 1) * TILE_M) - ta.mOrigin.mV[VX]) / mCell)) + 1);
                const S32 fy0 = std::max(win.mY0, static_cast<S32>(std::floor((static_cast<F32>(ky * TILE_M) - ta.mOrigin.mV[VY]) / mCell)) - 1);
                const S32 fy1 = std::min(win.mY0 + WINDOW - 1, static_cast<S32>(std::floor((static_cast<F32>((ky + 1) * TILE_M) - ta.mOrigin.mV[VY]) / mCell)) + 1);
                for (S32 iy = fy0; iy <= fy1; ++iy)
                {
                    const bool row_off = iy < 0 || iy >= ta.mHeight;
                    for (S32 ix = fx0; ix <= fx1; ++ix)
                    {
                        if (!row_off && ix >= 0 && ix < ta.mWidth)
                        {
                            ix = ta.mWidth - 1;     // the grid's own cells are in already
                            continue;
                        }
                        if (blocked(it, ix, iy, lo, hi))
                        {
                            win.mRows[iy - win.mY0] |= 1ull << (ix - win.mX0);
                        }
                    }
                }
            }
        }
    }

    bool SpacesPass::State::aperture(const Run& run, LLVector2& a, LLVector2& b, bool framed, bool true_faces) const
    {
        // Across a doorway the shortest chord between solid on both sides runs
        // jamb to jamb; tried near the cut's own direction, then again from
        // the chord's middle. A floor's edge is no jamb: nothing stands there.
        const S32 lo = run.mFloor + mClimb + 1, hi = run.mFloor + static_cast<S32>(std::lround(JAMB_TOP_M / mUnit));
        const F32 step = mCell * 0.5f;
        // Framed, a call reads its samples' cells in bulk, unless they are too fine for a window's rows.
        const F32 base = std::atan2(run.mD.mV[VY], run.mD.mV[VX]);
        framed = framed && 2 * frameReach() < WINDOW;
        LLVector2 own[RAYS];
        const RayFan* fan = framed ? &rayFan(base) : nullptr;
        const LLVector2* dirs = framed ? fan->mDirs.data() : own;
        for (S32 i = 0; i < RAYS && !framed; ++i)
        {
            const S32 k = (i & 1) ? (i + 1) / 2 : -(i / 2);
            const F32 th = base + k * (F_PI * 0.25f / APERTURE_ANGLES);
            own[i].set(std::cos(th), std::sin(th));
        }
        Frame frame;
        // solidAt, read from the windows.
        auto solid = [&](const LLVector2& from, const LLVector2& dir, F32 t) {
            const LLVector2 q = from + dir * t;
            if (!framed)
            {
                return solidAt(q.mV[VX], q.mV[VY], lo, hi);
            }
            const Frame::Place* place = frame.mPlaces;
            if (!frame.mOneTile)
            {
                TileKey key;
                key.mX = tileIndex(q.mV[VX]);
                key.mY = tileIndex(q.mV[VY]);
                const U32 it = tileAt(key);
                while (place != frame.mPlaces + frame.mCount && mWindows[place->mWindow].mTile != it)
                {
                    ++place;
                }
                if (place == frame.mPlaces + frame.mCount)
                {
                    return false;
                }
            }
            const Window& w = mWindows[place->mWindow];
            const S32 ix = static_cast<S32>(std::floor((q.mV[VX] - w.mAir->mOrigin.mV[VX]) / mCell));
            const S32 iy = static_cast<S32>(std::floor((q.mV[VY] - w.mAir->mOrigin.mV[VY]) / mCell));
            return ((w.mRows[iy - w.mY0] >> (ix - w.mX0)) & 1) != 0;
        };
        // Whether any cell the samples a mask holds can be read from is solid.
        auto reads = [&](S32 mask) {
            if (!framed)
            {
                return true;
            }
            const U64* m = &fan->mMasks[static_cast<size_t>(mask * WINDOW)];
            const auto& span = fan->mSpans[mask];
            U64 hit = 0;
            for (S32 k = 0; k < frame.mCount; ++k)
            {
                const Frame::Place& place = frame.mPlaces[k];
                const U64* rows = mWindows[place.mWindow].mRows + place.mY;
                for (S32 r = span.first; r <= span.second; ++r)
                {
                    hit |= (m[r] << place.mX) & rows[r];
                }
            }
            return hit != 0;
        };
        // The first solid sample along a ray, 0 when there is none short of limit.
        auto march = [&](const LLVector2& from, S32 side, S32 i, F32 limit) {
            const LLVector2 dir = side ? -dirs[i] : dirs[i];
            S32 j = 0, band = -1;
            bool any = false;
            for (F32 t = step; t <= SCORE_REACH_M && t - step < limit; t += step, ++j)
            {
                if (j / BAND != band)
                {
                    band = j / BAND;
                    any = !framed || reads(fan->bandMask(side, i, band));
                }
                if (any && solid(from, dir, t))
                {
                    return t;
                }
            }
            return 0.f;
        };
        // That sample's face, found to a sixty-fourth of a cell.
        auto face = [&](const LLVector2& from, const LLVector2& dir, F32 t) {
            F32 free = t - step, hit = t;
            while (hit - free > mCell / 64.f)
            {
                const F32 mid = (free + hit) * 0.5f;
                (solid(from, dir, mid) ? hit : free) = mid;
            }
            return hit;
        };
        LLVector2 c = run.mC;
        for (S32 pass = 0; pass < 2; ++pass)
        {
            if (solidAt(c.mV[VX], c.mV[VY], lo, hi))
            {
                return false;
            }
            if (framed)
            {
                frameAt(c, lo, hi, frame);
            }
            // With nothing solid within reach one way, no ray finds a jamb.
            if (framed && (!reads(fan->sideMask(0)) || !reads(fan->sideMask(1))))
            {
                return false;
            }
            // Nearer the cut's own direction wins a tie.
            F32 best = 1e30f;
            for (S32 i = 0; i < RAYS; ++i)
            {
                const F32 limit = best - mCell * 0.25f;
                if (framed)
                {
                    // The bands a march short of limit can reach, and a band more.
                    const S32 last = limit < SCORE_REACH_M + step ? std::min(fan->mBandCount - 1, static_cast<S32>(limit / step) / BAND + 1) : fan->mBandCount - 1;
                    if (!reads(fan->reachMask(0, i, last)) || !reads(fan->reachMask(1, i, last)))
                    {
                        continue;
                    }
                }
                const LLVector2& u = dirs[i];
                const LLVector2 v = -u;
                // Each face lies within a step short of its sample, so faces too far apart for a shorter chord go unsought.
                const F32 s0 = march(c, 0, i, limit);
                if (s0 == 0.f)
                {
                    continue;
                }
                const F32 s1 = march(c, 1, i, limit - (s0 - step) + mCell / 32.f);
                if (s1 == 0.f || (s0 - step) + (s1 - step) >= limit + mCell / 64.f)
                {
                    continue;
                }
                F32 t0 = face(c, u, s0), t1 = face(c, v, s1);
                if (true_faces)
                {
                    t0 = trueFace(c, u, t0, run.mFloor);
                    t1 = trueFace(c, v, t1, run.mFloor);
                }
                if (t0 + t1 < limit)
                {
                    best = t0 + t1;
                    a = c - u * t1;
                    b = c + u * t0;
                }
            }
            if (best > 1e29f)
            {
                return false;
            }
            c = (a + b) * 0.5f;
        }
        return true;
    }

    F32 SpacesPass::State::trueFace(const LLVector2& from, const LLVector2& dir, F32 t, S32 floor) const
    {
        const LLVector2 hit = from + dir * t, free = from + dir * (t - mCell / 32.f);
        TileKey key;
        key.mX = tileIndex(hit.mV[VX]);
        key.mY = tileIndex(hit.mV[VY]);
        const U32 it = tileAt(key);
        if (it == NONE || air(it).mFaceOffsets.empty())
        {
            return t;
        }
        const TileAir& a = air(it);
        const F32 ox = a.mOrigin.mV[VX], oy = a.mOrigin.mV[VY];
        const S32 ix = static_cast<S32>(std::floor((hit.mV[VX] - ox) / mCell)), iy = static_cast<S32>(std::floor((hit.mV[VY] - oy) / mCell));
        const S32 dx = ix - static_cast<S32>(std::floor((free.mV[VX] - ox) / mCell)), dy = iy - static_cast<S32>(std::floor((free.mV[VY] - oy) / mCell));
        if (std::abs(dx) + std::abs(dy) != 1 || ix < 0 || iy < 0 || ix >= a.mWidth || iy >= a.mHeight)
        {
            return t;   // through a corner, or off the grid: the cell face stands
        }
        const S32 side = dx == 1 ? TileAir::SIDE_NEG_X : dx == -1 ? TileAir::SIDE_POS_X : dy == 1 ? TileAir::SIDE_NEG_Y : TileAir::SIDE_POS_Y;
        const F32 off = faceOffset(a.mFaceOffsets, faceKey(ix, iy, a.mWidth, side));
        if (off <= 0.f)
        {
            return t;
        }
        // The face plane, and the ray's way out of the solid cell, which it may not pass.
        const S32 axis = side < 2 ? VX : VY;
        const F32 x0 = ox + ix * mCell, y0 = oy + iy * mCell;
        const F32 lo = axis == VX ? x0 : y0;
        const F32 plane = (side & 1) == 0 ? lo + off * mCell : lo + (1.f - off) * mCell;
        if (std::fabs(dir.mV[axis]) < 1e-6f)
        {
            return t;
        }
        F32 exit = 1e30f;
        for (S32 k = 0; k < 2; ++k)
        {
            const F32 base = k == VX ? x0 : y0;
            if (dir.mV[k] > 1e-6f)
            {
                exit = std::min(exit, (base + mCell - from.mV[k]) / dir.mV[k]);
            }
            else if (dir.mV[k] < -1e-6f)
            {
                exit = std::min(exit, (base - from.mV[k]) / dir.mV[k]);
            }
        }
        return llclamp((plane - from.mV[axis]) / dir.mV[axis], t, std::max(t, exit));
    }

    void SpacesPass::State::prepare()
    {
        SK_NAV_ZONE("nav.spaces.prepare");
        const RoomsInput& in = *mInput;
        const SpaceParams& p = params();
        mProduct = std::make_shared<RoomsProduct>();
        mProduct->mGeneration = in.mGeneration;
        for (size_t i = 0; i < mEnc->mTiles.size(); ++i)
        {
            const TileEnclosure& te = mEnc->mTiles[i];
            if (!te.mAir || te.mAir->mLeaves.empty())
            {
                continue;
            }
            TileInfo ti;
            ti.mKey = te.mKey;
            ti.mAir = te.mAir;
            ti.mEnc = static_cast<U32>(i);
            mCell = te.mAir->mCell;
            mUnit = te.mAir->mUnit;
            ti.mZOff = static_cast<S32>(std::lround(te.mAir->mOrigin.mV[VZ] / te.mAir->mUnit));
            const RoomsTile* prev = in.mPrevious ? in.mPrevious->tile(te.mKey) : nullptr;
            ti.mDirty = !prev || prev->mAir != te.mAir || prev->mSeeds.size() != te.mAir->mGaps.size();
            if (!ti.mDirty)
            {
                ti.mRooms = *prev;
            }
            mTiles.push_back(std::move(ti));
        }
        if (!mTiles.empty())
        {
            S32 x1 = mTiles[0].mKey.mX, y1 = mTiles[0].mKey.mY;
            mGridX = x1;
            mGridY = y1;
            for (const TileInfo& ti : mTiles)
            {
                mGridX = std::min(mGridX, ti.mKey.mX);
                mGridY = std::min(mGridY, ti.mKey.mY);
                x1 = std::max(x1, ti.mKey.mX);
                y1 = std::max(y1, ti.mKey.mY);
            }
            mGridW = x1 - mGridX + 1;
            mGridH = y1 - mGridY + 1;
            mTileGrid.assign(static_cast<size_t>(mGridW) * mGridH, NONE);
            for (U32 t = 0; t < mTiles.size(); ++t)
            {
                mTileGrid[static_cast<size_t>(mTiles[t].mKey.mY - mGridY) * mGridW + (mTiles[t].mKey.mX - mGridX)] = t;
            }
        }
        mClimb = static_cast<S32>(std::lround(p.mClimbM / mUnit));
        mWalkHead = static_cast<S32>(std::ceil(p.mWalkHeadM / mUnit - 1e-3f));
        mWalkOverlap = static_cast<S32>(std::lround(p.mWalkOverlapM / mUnit));
        for (TileInfo& ti : mTiles)
        {
            const TileAir& a = *ti.mAir;
            ti.mLeafMap.assign(static_cast<size_t>(a.mWidth) * a.mHeight, -1);
            for (size_t l = 0; l < a.mLeaves.size(); ++l)
            {
                const AirLeaf& leaf = a.mLeaves[l];
                const S32 s = 1 << leaf.mLevel;
                for (S32 y = leaf.mY; y < std::min(a.mHeight, leaf.mY + s); ++y)
                {
                    for (S32 x = leaf.mX; x < std::min(a.mWidth, leaf.mX + s); ++x)
                    {
                        ti.mLeafMap[x + y * a.mWidth] = static_cast<S32>(l);
                    }
                }
            }
        }
        // Every tile whose air moved is cut in one go, with the ring round them.
        std::vector<U32> dirty;
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            if (mTiles[t].mDirty)
            {
                dirty.push_back(t);
                ++mProduct->mDirtyTiles;
            }
        }
        if (!dirty.empty())
        {
            mBatches.push_back(std::move(dirty));
        }
        mShadow.resize(mBatches.size());
        mSeamShadow.resize(mBatches.size());
        mDoors = doorSpans(in.mDoors, in.mMeshOrigin);
        if (in.mPrevious)
        {
            for (const Portal& q : in.mPrevious->mPortals)
            {
                if (q.mKind == PortalKind::OPENING)
                {
                    mOldGrid[gridKey(static_cast<S32>(std::floor(q.mCentre.mV[VX])), static_cast<S32>(std::floor(q.mCentre.mV[VY])))].push_back(
                        static_cast<U32>(mOldPortals.size()));
                    mOldPortals.push_back(q.mCentre);
                }
            }
        }
        const F32 reach = mInput->mParams.mDoorReachM;
        for (U32 i = 0; i < mDoors.size(); ++i)
        {
            const DoorSpan& d = mDoors[i];
            const S32 x0 = static_cast<S32>(std::floor(std::min(d.mA.mV[VX], d.mB.mV[VX]) - reach));
            const S32 x1 = static_cast<S32>(std::floor(std::max(d.mA.mV[VX], d.mB.mV[VX]) + reach));
            const S32 y0 = static_cast<S32>(std::floor(std::min(d.mA.mV[VY], d.mB.mV[VY]) - reach));
            const S32 y1 = static_cast<S32>(std::floor(std::max(d.mA.mV[VY], d.mB.mV[VY]) + reach));
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    mDoorGrid[gridKey(x, y)].push_back(i);
                }
            }
        }
        memoContext();
    }

    void SpacesPass::State::memoContext()
    {
        SK_NAV_ZONE("nav.spaces.memo");
        // What any score or run reads besides its own cut and tiles: the parameters and the doors.
        MemoKey context;
        F32 values[sizeof(SpaceParams) / sizeof(F32)];
        static_assert(sizeof(SpaceParams) == sizeof(values), "SpaceParams holds F32s only");
        std::memcpy(values, &params(), sizeof(values));
        for (F32 v : values)
        {
            context.feed(v);
        }
        context.feed(mCell);
        context.feed(mUnit);
        context.feed(static_cast<U64>(mDoors.size()));
        for (const DoorSpan& d : mDoors)
        {
            for (const LLVector2& v : { d.mA, d.mB, d.mC, d.mD, d.mN })
            {
                context.feed(v);
            }
            context.feed(d.mLo);
            context.feed(d.mHi);
            context.feed(d.mWidth);
            U64 id[2];
            std::memcpy(id, d.mId.mData, sizeof(id));
            context.feed(id[0]);
            context.feed(id[1]);
        }
        mMemo = std::make_shared<MergeMemo>();
        mMemo->mContext = context.mA;
        // A tile as probes read it: its air, and whether and how indoors each gap is.
        mTileKey.assign(mTiles.size(), 0);
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            MemoKey k;
            k.feed(static_cast<U64>(reinterpret_cast<uintptr_t>(mTiles[t].mAir.get())));
            const std::vector<GapEnclosure>& gaps = mEnc->mTiles[mTiles[t].mEnc].mGaps;
            k.feed(static_cast<U64>(gaps.size()));
            U64 word = 0;
            for (size_t g = 0; g < gaps.size(); ++g)
            {
                word = (word << 16) | (gaps[g].mD == ENCLOSURE_D_UNKNOWN ? 0x100u : 0u) | gaps[g].mE;
                if ((g & 3) == 3)
                {
                    k.feed(word);
                    word = 0;
                }
            }
            k.feed(word);
            mTileKey[t] = k.mA;
        }
        // A tile's air is who it is: the last pass's product holds every air its memo names, so none is reused.
        const RoomsProductPtr& prev = mInput->mPrevious;
        if (prev && prev->mMemo && prev->mMemo->mContext == mMemo->mContext && mProduct->mDirtyTiles <= MEMO_BLOCK_SHARE * mTiles.size())
        {
            mOldMemo = prev->mMemo.get();
        }
    }

    void SpacesPass::State::runBatch(size_t b)
    {
        SK_NAV_ZONE("nav.spaces.cut.batch");
        // The block's tiles and the ring round them: enough air on every
        // side for the transform, which stops at mClearMaxM, to be right.
        for (TileInfo& ti : mTiles)
        {
            ti.mInBlock = false;
            ti.mWork = false;
            ti.mColStart.clear();
        }
        for (U32 t : mBatches[b])
        {
            mTiles[t].mInBlock = true;
            mTiles[t].mBatch = static_cast<S32>(b);
        }
        for (TileInfo& ti : mTiles)
        {
            for (S32 dy = -1; dy <= 1 && !ti.mWork; ++dy)
            {
                for (S32 dx = -1; dx <= 1 && !ti.mWork; ++dx)
                {
                    TileKey k = ti.mKey;
                    k.mX += dx;
                    k.mY += dy;
                    const U32 it = tileAt(k);
                    ti.mWork = it != NONE && mTiles[it].mInBlock;
                }
            }
        }
        buildCells();
        transform();
        cutSeeds(b);
        std::vector<Cell>().swap(mCells);
        std::vector<U32>().swap(mLink);
        std::vector<U8>().swap(mSide);
        std::vector<F32>().swap(mDist);
        std::vector<U8>().swap(mAvatar);
        std::vector<S32>().swap(mLabel);
        for (TileInfo& ti : mTiles)
        {
            std::vector<U32>().swap(ti.mColStart);
        }
    }

    void SpacesPass::State::wallEnds()
    {
        SK_NAV_ZONE("nav.spaces.cut.wallends");
        // Walls that stop in open air make no saddle for the watershed to
        // cut at: pair up facing wall ends, score the gap between them as
        // any cut is scored, and where it is a portal sever the floor across
        // it, so the regions either side seed apart.
        struct End
        {
            LLVector2 mP;
            LLVector2 mT;       ///< pointing past the wall's end, into the gap
            S32 mFloor;
        };
        const F32 reach = WALL_END_REACH * params().mWalkHeadM;
        const S32 run_cells = static_cast<S32>(std::lround(WALL_END_RUN_M / mCell));
        std::vector<End> ends;
        const size_t n = mCells.size();
        for (U32 c = 0; c < n; ++c)
        {
            const Cell& cell = mCells[c];
            if (!mTiles[cell.mTile].mInBlock || !mAvatar[c])
            {
                continue;
            }
            const TileInfo& ti = mTiles[cell.mTile];
            for (S32 d = 0; d < 4; ++d)
            {
                if (mSide[c * 4 + d] != SIDE_WALL ||
                    !blocked(cell.mTile, cell.mX + DX[d], cell.mY + DY[d], cell.mFloor + mClimb, cell.mFloor + static_cast<S32>(std::lround(2.f / mUnit))))
                {
                    continue;
                }
                // Along the wall both ways: the wall ends where the next cell has none on that side.
                for (S32 t = 0; t < 4; ++t)
                {
                    if (DX[t] * DX[d] + DY[t] * DY[d] != 0)
                    {
                        continue;
                    }
                    const U32 next = mLink[c * 4 + t];
                    if (next == NONE || mSide[next * 4 + d] == SIDE_WALL)
                    {
                        continue;
                    }
                    // And runs on behind it far enough to be a wall, not a post.
                    S32 len = 0;
                    const S32 back = t ^ 1;
                    for (U32 k = c; k != NONE && len < run_cells && mSide[k * 4 + d] == SIDE_WALL; k = mLink[k * 4 + back])
                    {
                        ++len;
                    }
                    if (len < run_cells)
                    {
                        continue;
                    }
                    End e;
                    e.mP.set(cellX(ti, cell.mX) + (DX[d] + DX[t]) * 0.5f * mCell, cellY(ti, cell.mY) + (DY[d] + DY[t]) * 0.5f * mCell);
                    e.mT.set(static_cast<F32>(DX[t]), static_cast<F32>(DY[t]));
                    e.mFloor = cell.mFloor;
                    ends.push_back(e);
                }
            }
        }
        std::unordered_map<U64, std::vector<U32>> grid;
        auto key = [reach](const LLVector2& p) {
            return (static_cast<U64>(static_cast<U32>(static_cast<S32>(std::floor(p.mV[VX] / reach)))) << 32) |
                   static_cast<U32>(static_cast<S32>(std::floor(p.mV[VY] / reach)));
        };
        for (U32 i = 0; i < ends.size(); ++i)
        {
            grid[key(ends[i].mP)].push_back(i);
        }
        Probe pr;
        std::vector<std::pair<F32, std::pair<U32, U32>>> pairs;
        for (U32 i = 0; i < ends.size(); ++i)
        {
            const End& a = ends[i];
            const S32 gx = static_cast<S32>(std::floor(a.mP.mV[VX] / reach)), gy = static_cast<S32>(std::floor(a.mP.mV[VY] / reach));
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const auto it = grid.find((static_cast<U64>(static_cast<U32>(gx + dx)) << 32) | static_cast<U32>(gy + dy));
                    if (it == grid.end())
                    {
                        continue;
                    }
                    for (U32 j : it->second)
                    {
                        const End& b = ends[j];
                        const LLVector2 ab = b.mP - a.mP;
                        const F32 len = ab.length();
                        if (j <= i || len < 0.5f || len > reach || std::abs(a.mFloor - b.mFloor) > mClimb ||
                            ab * a.mT < 0.7f * len || -(ab * b.mT) < 0.7f * len)
                        {
                            continue;
                        }
                        pairs.push_back({ len, { i, j } });
                    }
                }
            }
        }
        // Nearest pairs first; an end serves one cut.
        std::sort(pairs.begin(), pairs.end());
        std::vector<bool> used(ends.size(), false);
        for (const auto& pq : pairs)
        {
            const End& a = ends[pq.second.first];
            const End& b = ends[pq.second.second];
            if (used[pq.second.first] || used[pq.second.second])
            {
                continue;
            }
            Run r;
            r.mA = a.mP;
            r.mB = b.mP;
            r.mD = b.mP - a.mP;
            r.mWidth = r.mD.normVec();
            r.mN.set(-r.mD.mV[VY], r.mD.mV[VX]);
            r.mC = (a.mP + b.mP) * 0.5f;
            r.mFloor = std::min(a.mFloor, b.mFloor);
            r.mKey = mix(mix(mix(SEED, 0x5741ll), static_cast<U64>(std::lround(r.mC.mV[VX] * 8.f))), static_cast<U64>(std::lround(r.mC.mV[VY] * 8.f)));
            // Floor all the way across.
            bool open = true;
            for (F32 t = mCell; t < r.mWidth - mCell * 0.5f && open; t += mCell)
            {
                const LLVector2 q = r.mA + r.mD * t;
                open = probe(q.mV[VX], q.mV[VY], r.mFloor, mClimb, pr);
            }
            // Doors cut where their leaves are; this is for what has none.
            const Scored cut = open ? score(r) : Scored();
            if (!cut.mPortal || cut.mKind != PortalKind::OPENING)
            {
                continue;
            }
            used[pq.second.first] = used[pq.second.second] = true;
            ++mWallEndCuts;
            sever(r, nullptr);
        }
    }

    U32 SpacesPass::State::cellAt(const LLVector2& q, S32 floor) const
    {
        TileKey k;
        k.mX = tileIndex(q.mV[VX]);
        k.mY = tileIndex(q.mV[VY]);
        const U32 it = tileAt(k);
        if (it == NONE || !mTiles[it].mWork)
        {
            return NONE;
        }
        const TileInfo& ti = mTiles[it];
        const S32 ix = static_cast<S32>(std::floor((q.mV[VX] - ti.mAir->mOrigin.mV[VX]) / mCell));
        const S32 iy = static_cast<S32>(std::floor((q.mV[VY] - ti.mAir->mOrigin.mV[VY]) / mCell));
        if (ix < 0 || iy < 0 || ix >= ti.mAir->mWidth || iy >= ti.mAir->mHeight)
        {
            return NONE;
        }
        const S32 col = ix + iy * ti.mAir->mWidth;
        for (U32 c = ti.mColStart[col]; c < ti.mColStart[col + 1]; ++c)
        {
            if (std::abs(mCells[c].mFloor - floor) <= mClimb)
            {
                return c;
            }
        }
        return NONE;
    }

    void SpacesPass::State::sever(const Run& r, std::vector<std::pair<U32, U32>>* undo)
    {
        // Sever the links crossing the segment, from the cells along it.
        std::vector<U32> near_cells;
        for (F32 t = 0.f; t <= r.mWidth; t += mCell * 0.5f)
        {
            for (F32 side : { -1.f, 0.f, 1.f })
            {
                const LLVector2 q = r.mA + r.mD * t + r.mN * (side * mCell);
                TileKey k;
                k.mX = tileIndex(q.mV[VX]);
                k.mY = tileIndex(q.mV[VY]);
                const U32 it = tileAt(k);
                if (it == NONE || !mTiles[it].mWork)
                {
                    continue;
                }
                const TileInfo& ti = mTiles[it];
                const S32 ix = static_cast<S32>(std::floor((q.mV[VX] - ti.mAir->mOrigin.mV[VX]) / mCell));
                const S32 iy = static_cast<S32>(std::floor((q.mV[VY] - ti.mAir->mOrigin.mV[VY]) / mCell));
                if (ix < 0 || iy < 0 || ix >= ti.mAir->mWidth || iy >= ti.mAir->mHeight)
                {
                    continue;
                }
                const S32 col = ix + iy * ti.mAir->mWidth;
                for (U32 c = ti.mColStart[col]; c < ti.mColStart[col + 1]; ++c)
                {
                    if (std::abs(mCells[c].mFloor - r.mFloor) <= mClimb)
                    {
                        near_cells.push_back(c);
                    }
                }
            }
        }
        for (U32 c : near_cells)
        {
            const Cell& cell = mCells[c];
            const TileInfo& ti = mTiles[cell.mTile];
            const LLVector2 pc(cellX(ti, cell.mX), cellY(ti, cell.mY));
            for (S32 d = 0; d < 4; ++d)
            {
                const U32 o = mLink[c * 4 + d];
                if (o == NONE)
                {
                    continue;
                }
                const LLVector2 po(pc.mV[VX] + DX[d] * mCell, pc.mV[VY] + DY[d] * mCell);
                const F32 sc = (pc - r.mA) * r.mN, so = (po - r.mA) * r.mN;
                const F32 along = ((pc + po) * 0.5f - r.mA) * r.mD;
                if ((sc < 0.f) != (so < 0.f) && along >= 0.f && along <= r.mWidth)
                {
                    if (undo)
                    {
                        undo->emplace_back(c * 4 + d, o);
                        undo->emplace_back(o * 4 + (d ^ 1), c);
                    }
                    mLink[c * 4 + d] = NONE;
                    mLink[o * 4 + (d ^ 1)] = NONE;
                }
            }
        }
    }

    void SpacesPass::State::apertureCuts()
    {
        SK_NAV_ZONE("nav.spaces.cut.apertures");
        // A doorway whose rooms the watershed sees as one basin makes no
        // saddle to cut at: find it from below, where the ceiling drops to a
        // lintel, take the aperture between its jambs, and sever the floor
        // there when a lintel spans it, floor runs through it, its jambs are
        // the ends of a wall across it rather than the sides of a corridor
        // along it, and each side then holds an avatar's floor.
        const SpaceParams& p = params();
        const S32 drop = static_cast<S32>(std::lround(p.mLintelDropM / mUnit));
        const F32 grid = 0.5f;
        std::unordered_set<U64> tried;
        std::vector<LLVector2> cuts;
        std::vector<std::pair<U32, U32>> undo;
        std::vector<U32> stack;
        std::unordered_set<U32> seen;
        const F32 passage = 2.f * p.mAgentRadiusM;
        const U32 min_cells = static_cast<U32>(std::ceil(p.mAvatarAreaM2 / (mCell * mCell)));
        // Floor reached from a point without crossing a severed link, up to what an avatar needs.
        auto holds = [&](const LLVector2& at, S32 floor) {
            const U32 start = cellAt(at, floor);
            if (start == NONE || !mAvatar[start])
            {
                return false;
            }
            seen.clear();
            seen.insert(start);
            stack.assign(1, start);
            while (!stack.empty() && seen.size() < min_cells)
            {
                const U32 k = stack.back();
                stack.pop_back();
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = mLink[k * 4 + d];
                    if (o != NONE && mAvatar[o] && seen.insert(o).second)
                    {
                        stack.push_back(o);
                    }
                }
            }
            return seen.size() >= min_cells;
        };
        // A ring tile's lintel near the block may cut a doorway whose far side the block holds, as a full pass would.
        const F32 ring_reach = SCORE_REACH_M + APERTURE_MAX_M;
        const auto nearBlock = [&](const Cell& cell) {
            const TileInfo& ti = mTiles[cell.mTile];
            const F32 lx = (cell.mX + 0.5f) * mCell, ly = (cell.mY + 0.5f) * mCell;
            const F32 wx = ti.mAir->mWidth * mCell, wy = ti.mAir->mHeight * mCell;
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    TileKey k = ti.mKey;
                    k.mX += dx;
                    k.mY += dy;
                    const U32 it = tileAt(k);
                    if (it != NONE && mTiles[it].mInBlock && (dx < 0 ? lx : dx > 0 ? wx - lx : 0.f) <= ring_reach &&
                        (dy < 0 ? ly : dy > 0 ? wy - ly : 0.f) <= ring_reach)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        const auto blockSide = [&](const Run& r) {
            Probe pr;
            return (pastLintel(r, 1.f, pr) && mTiles[pr.mTile].mInBlock) || (pastLintel(r, -1.f, pr) && mTiles[pr.mTile].mInBlock);
        };
        const size_t n = mCells.size();
        for (U32 c = 0; c < n; ++c)
        {
            const Cell& cell = mCells[c];
            const bool ring = !mTiles[cell.mTile].mInBlock;
            if (!mAvatar[c] || cell.mFlight || cell.mCeil >= OPEN_TOP || (ring && !nearBlock(cell)))
            {
                continue;
            }
            for (S32 d = 0; d < 4; ++d)
            {
                const U32 o = mLink[c * 4 + d];
                if (o == NONE || mCells[o].mCeil < cell.mCeil + drop)
                {
                    continue;
                }
                const TileInfo& ti = mTiles[cell.mTile];
                const LLVector2 at(cellX(ti, cell.mX), cellY(ti, cell.mY));
                const U64 key = (static_cast<U64>(static_cast<U32>(static_cast<S32>(std::floor(at.mV[VX] / grid)))) << 40) ^
                                (static_cast<U64>(static_cast<U32>(static_cast<S32>(std::floor(at.mV[VY] / grid)))) << 20) ^
                                (static_cast<U64>(static_cast<U32>(cell.mFloor / (mClimb + 1))) << 1) ^ static_cast<U64>(d >> 1);
                if (!tried.insert(key).second)
                {
                    continue;
                }
                // The edge of the lintel need not lie between its jambs: try
                // the chord from each step back under it, and keep the
                // shortest a doorway's jambs end.
                Run probe_run;
                probe_run.mD.set(static_cast<F32>(-DY[d]), static_cast<F32>(DX[d]));
                probe_run.mFloor = cell.mFloor;
                const LLVector2 back(static_cast<F32>(-DX[d]), static_cast<F32>(-DY[d]));
                Run r;
                r.mWidth = 1e30f;
                Probe under;
                for (F32 t = 0.f; t <= JAMB_WALL_M + 1e-3f; t += mCell)
                {
                    probe_run.mC = at + back * t;
                    if (!probe(probe_run.mC.mV[VX], probe_run.mC.mV[VY], cell.mFloor, mClimb, under) || under.mCeil >= cell.mCeil + drop)
                    {
                        break;
                    }
                    Run q;
                    if (!aperture(probe_run, q.mA, q.mB, mInput->mFrameApertures))
                    {
                        continue;
                    }
                    q.mD = q.mB - q.mA;
                    q.mWidth = q.mD.normVec();
                    q.mN.set(-q.mD.mV[VY], q.mD.mV[VX]);
                    q.mC = (q.mA + q.mB) * 0.5f;
                    q.mFloor = cell.mFloor;
                    if (q.mWidth >= passage && q.mWidth <= APERTURE_MAX_M && q.mWidth < r.mWidth && jambEnds(q))
                    {
                        r = q;
                    }
                }
                bool near_cut = r.mWidth > APERTURE_MAX_M;
                for (const LLVector2& q : cuts)
                {
                    near_cut = near_cut || dist_vec(q, r.mC) < APERTURE_MAX_M * 0.5f;
                }
                if (near_cut)
                {
                    continue;
                }
                r.mKey = mix(mix(mix(SEED, 0x41505254ll), static_cast<U64>(std::lround(r.mC.mV[VX] * 8.f))), static_cast<U64>(std::lround(r.mC.mV[VY] * 8.f)));
                // Floor all the way across, under a lintel as level as a door's: a sloping ceiling is no lintel.
                bool open = true;
                Probe pr;
                S32 ceil_lo = INT_MAX, ceil_hi = INT_MIN;
                for (F32 t = mCell; t < r.mWidth - mCell * 0.5f && open; t += mCell)
                {
                    const LLVector2 q = r.mA + r.mD * t;
                    open = probe(q.mV[VX], q.mV[VY], r.mFloor, mClimb, pr);
                    ceil_lo = std::min(ceil_lo, pr.mCeil);
                    ceil_hi = std::max(ceil_hi, pr.mCeil);
                }
                if (!open || ceil_hi - ceil_lo > drop)
                {
                    continue;
                }
                const Scored s = score(r);
                if (!(s.mCues & CUE_LINTEL) || !(s.mCues & CUE_JAMBS) || s.mDoor >= 0 || s.mHead - r.mFloor * mUnit > APERTURE_HEAD_M)
                {
                    continue;
                }
                // A doorway leads in under a roof: an arch between two stretches of open sky parts nothing.
                Probe in_a, in_b;
                if (!pastLintel(r, 1.f, in_a) || !pastLintel(r, -1.f, in_b) || (in_a.mCeil >= OPEN_TOP && in_b.mCeil >= OPEN_TOP))
                {
                    continue;
                }
                // Cut down the middle of the wall, not along a face of it, so the run cut there stands under the lintel.
                const LLVector2 shift = r.mN * wallMiddle(r);
                Probe mid;
                if (probe(r.mC.mV[VX] + shift.mV[VX], r.mC.mV[VY] + shift.mV[VY], r.mFloor, mClimb, mid) && mid.mCeil + drop <= std::min(in_a.mCeil, in_b.mCeil))
                {
                    r.mA += shift;
                    r.mB += shift;
                    r.mC += shift;
                }
                // A cut wholly among clean tiles is kept from the last pass, not made again.
                if (ring && !blockSide(r))
                {
                    continue;
                }
                undo.clear();
                sever(r, &undo);
                const F32 side = 0.5f;
                if (undo.empty() || !holds(r.mC + r.mN * side, r.mFloor) || !holds(r.mC - r.mN * side, r.mFloor))
                {
                    for (const auto& u : undo)
                    {
                        mLink[u.first] = u.second;
                    }
                    continue;
                }
                cuts.push_back(r.mC);
                mForced.push_back(r);
                ++mApertureCuts;
            }
        }
    }

    bool SpacesPass::State::pastLintel(const Run& r, F32 sgn, Probe& out) const
    {
        // The first floor off the cut, on one side, whose ceiling clears the lintel.
        Probe at;
        if (!probe(r.mC.mV[VX], r.mC.mV[VY], r.mFloor, mClimb, at))
        {
            return false;
        }
        const S32 drop = static_cast<S32>(std::lround(params().mLintelDropM / mUnit));
        for (F32 t = mCell; t <= SCORE_REACH_M * 0.5f; t += mCell)
        {
            const LLVector2 q = r.mC + r.mN * (sgn * t);
            if (probe(q.mV[VX], q.mV[VY], r.mFloor, mClimb, out) && out.mCeil >= at.mCeil + drop)
            {
                return true;
            }
        }
        return false;
    }

    F32 SpacesPass::State::wallMiddle(const Run& r) const
    {
        // Just inside each jamb, the wall's faces either side of the cut.
        const S32 lo = r.mFloor + mClimb + 1, hi = r.mFloor + static_cast<S32>(std::lround(JAMB_TOP_M / mUnit));
        F32 sum = 0.f;
        for (S32 k = 0; k < 2; ++k)
        {
            const LLVector2 j = k == 0 ? r.mA - r.mD * (mCell * 1.5f) : r.mB + r.mD * (mCell * 1.5f);
            F32 face[2] = { 0.f, 0.f };
            for (S32 side = 0; side < 2; ++side)
            {
                const F32 sgn = side == 0 ? 1.f : -1.f;
                for (F32 t = mCell * 0.25f; t <= JAMB_WALL_M; t += mCell * 0.25f)
                {
                    face[side] = t;
                    const LLVector2 q = j + r.mN * (sgn * t);
                    if (!solidAt(q.mV[VX], q.mV[VY], lo, hi))
                    {
                        break;
                    }
                }
            }
            sum += 0.5f * (face[0] - face[1]);
        }
        return 0.5f * sum;
    }

    bool SpacesPass::State::jambEnds(const Run& r) const
    {
        // A jamb ends a wall that runs across the cut: just inside it, solid,
        // and a step off the wall to one side at least, air at a door's
        // height. A corridor's walls run on along both sides of a beam.
        const S32 lo = r.mFloor + mClimb + 1, hi = r.mFloor + static_cast<S32>(std::lround(JAMB_TOP_M / mUnit));
        for (S32 k = 0; k < 2; ++k)
        {
            const LLVector2 out = k == 0 ? -r.mD : r.mD;
            bool ends = false;
            for (const F32 depth : { 1.f, 1.5f })
            {
                const LLVector2 j = (k == 0 ? r.mA : r.mB) + out * (mCell * depth);
                if (!solidAt(j.mV[VX], j.mV[VY], lo, hi))
                {
                    continue;
                }
                for (F32 sgn : { 1.f, -1.f })
                {
                    const LLVector2 q = j + r.mN * (sgn * JAMB_WALL_M);
                    ends = ends || !solidAt(q.mV[VX], q.mV[VY], lo, hi);
                }
            }
            // And the wall runs on past the jamb along the cut, or it is a post.
            for (F32 t = mCell * 1.5f; ends && t <= JAMB_RUN_M + 1e-3f; t += mCell)
            {
                const LLVector2 j = (k == 0 ? r.mA : r.mB) + out * t;
                ends = solidAt(j.mV[VX], j.mV[VY], lo, hi);
            }
            if (!ends)
            {
                return false;
            }
        }
        return true;
    }

    bool SpacesPass::State::recut()
    {
        SK_NAV_ZONE("nav.spaces.cut.recut");
        // A clean ring tile whose kept seeds split its edge where the cut
        // just saw one basin, or join what it saw as two, would make the
        // seam disagree from pass to pass: cut it again with the rest.
        if (mRecuts >= 2 || mBatches.empty())
        {
            return false;
        }
        std::vector<U32> stale;
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            if (ti.mDirty || !ti.mWork)
            {
                continue;
            }
            std::unordered_map<U32, S32> seed_label;
            std::unordered_map<S32, U32> label_seed;
            bool agrees = true;
            for (const auto& entry : mShadow[0])
            {
                if ((entry.first >> 32) != t)
                {
                    continue;
                }
                const U32 seed = ti.mRooms.mSeeds[static_cast<U32>(entry.first)];
                const auto a = seed_label.emplace(seed, entry.second).first;
                const auto b = label_seed.emplace(entry.second, seed).first;
                if (!seed || a->second != entry.second || b->second != seed)
                {
                    agrees = false;
                    break;
                }
            }
            if (!agrees)
            {
                stale.push_back(t);
            }
        }
        // A basin of the block's that runs on along a seam between ring tiles, into kept seeds no
        // earlier pass joined to the rest of it: those tiles are cut again with the block.
        {
            std::unordered_set<S32> block_labels;
            for (const TileInfo& ti : mTiles)
            {
                if (ti.mInBlock)
                {
                    block_labels.insert(ti.mSeedLabels.begin(), ti.mSeedLabels.end());
                }
            }
            std::unordered_map<U64, U32> index;
            UnionFind joined;
            joined.reset(0);
            auto at = [&index, &joined](U32 t, U32 seed) {
                const auto it = index.emplace((static_cast<U64>(t) << 32) | seed, static_cast<U32>(index.size())).first;
                if (it->second == joined.mParent.size())
                {
                    joined.mParent.push_back(it->second);
                }
                return it->second;
            };
            // label -> one kept seed of it reached from the block, then any reached along a ring seam.
            std::unordered_map<S32, std::pair<U32, U32>> reached;
            std::vector<std::tuple<S32, U32, U32>> seam;
            for (size_t pass = 0; pass < 2; ++pass)
            {
                for (const auto& [where, label] : pass ? mSeamShadow[0] : mShadow[0])
                {
                    const U32 t = static_cast<U32>(where >> 32);
                    const TileInfo& ti = mTiles[t];
                    const U32 gap = static_cast<U32>(where);
                    if (ti.mDirty || !block_labels.count(label) || gap >= ti.mRooms.mSeeds.size() || !ti.mRooms.mSeeds[gap])
                    {
                        continue;
                    }
                    const U32 k = at(t, ti.mRooms.mSeeds[gap]);
                    if (pass)
                    {
                        seam.emplace_back(label, k, t);
                    }
                    else
                    {
                        reached.emplace(label, std::make_pair(k, t));
                    }
                }
            }
            if (!seam.empty() && mInput->mPrevious)
            {
                for (const SeedLink& l : mInput->mPrevious->mLinks)
                {
                    const U32 a = tileAt(l.mA), b = tileAt(l.mB);
                    if (a != NONE && b != NONE && !mTiles[a].mDirty && !mTiles[b].mDirty)
                    {
                        joined.unite(at(a, l.mSeedA), at(b, l.mSeedB));
                    }
                }
            }
            for (const auto& [label, k, t] : seam)
            {
                auto it = reached.emplace(label, std::make_pair(k, t)).first;
                if (joined.find(it->second.first) != joined.find(k))
                {
                    stale.push_back(t);
                    stale.push_back(it->second.second);
                }
            }
            std::sort(stale.begin(), stale.end());
            stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
        }
        if (stale.empty())
        {
            return false;
        }
        ++mRecuts;
        for (U32 t : stale)
        {
            mTiles[t].mDirty = true;
            mBatches[0].push_back(t);
            ++mProduct->mDirtyTiles;
        }
        for (TileInfo& ti : mTiles)
        {
            ti.mBatch = -1;
            ti.mSeedLabels.clear();
        }
        mShadow[0].clear();
        mSeamShadow[0].clear();
        mForced.clear();
        mLabelBase = 0;
        mBatch = 0;
        return true;
    }

    void SpacesPass::State::buildCells()
    {
        SK_NAV_ZONE("nav.spaces.cells");
        size_t gaps = 0;
        for (const TileInfo& ti : mTiles)
        {
            if (!ti.mWork)
            {
                continue;
            }
            for (const AirLeaf& leaf : ti.mAir->mLeaves)
            {
                gaps += static_cast<size_t>(leaf.mGapCount) << (2 * leaf.mLevel);
            }
        }
        mCells.reserve(gaps);
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            TileInfo& ti = mTiles[t];
            if (!ti.mWork)
            {
                continue;
            }
            const TileAir& a = *ti.mAir;
            AirColumn col;
            ti.mColStart.assign(static_cast<size_t>(a.mWidth) * a.mHeight + 1, 0);
            for (S32 iy = 0; iy < a.mHeight; ++iy)
            {
                for (S32 ix = 0; ix < a.mWidth; ++ix)
                {
                    const S32 c = ix + iy * a.mWidth;
                    ti.mColStart[c] = static_cast<U32>(mCells.size());
                    airColumn(a, ti.mLeafMap[c], col);
                    for (U32 i = 0; i < col.mCount; ++i)
                    {
                        const AirGap& g = a.mGaps[col.mGap[i]];
                        if (floorGap(t, g))
                        {
                            mCells.push_back({ t, static_cast<U16>(ix), static_cast<U16>(iy), col.mGap[i], floorAt(t, col, i, ix, iy), ceilOf(t, g),
                                               (g.mFlags & AirGap::FLIGHT) != 0 });
                        }
                    }
                }
            }
            ti.mColStart.back() = static_cast<U32>(mCells.size());
        }
        const size_t n = mCells.size();
        mProduct->mCells += static_cast<U32>(n);
        mLink.assign(n * 4, NONE);
        mSide.assign(n * 4, SIDE_WALL);
        // A column's neighbour columns once for all its cells: most lie in its own tile.
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            if (!ti.mWork)
            {
                continue;
            }
            const S32 w = ti.mAir->mWidth, h = ti.mAir->mHeight;
            for (S32 iy = 0; iy < h; ++iy)
            {
                for (S32 ix = 0; ix < w; ++ix)
                {
                    const U32 c0 = ti.mColStart[ix + iy * w], c1 = ti.mColStart[ix + iy * w + 1];
                    if (c0 == c1)
                    {
                        continue;
                    }
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const S32 nx = ix + DX[d], ny = iy + DY[d];
                        U32 o0, o1;
                        if (nx >= 0 && ny >= 0 && nx < w && ny < h)
                        {
                            o0 = ti.mColStart[nx + ny * w];
                            o1 = ti.mColStart[nx + ny * w + 1];
                        }
                        else
                        {
                            U32 u;
                            S32 x, y;
                            const S32 r = resolve(t, nx, ny, u, x, y);
                            if (r == -2 || (r == 0 && !mTiles[u].mWork))
                            {
                                for (U32 c = c0; c < c1; ++c)
                                {
                                    mSide[c * 4 + d] = SIDE_UNKNOWN;
                                }
                                continue;
                            }
                            if (r != 0)
                            {
                                continue;
                            }
                            const TileInfo& ui = mTiles[u];
                            const S32 col = x + y * ui.mAir->mWidth;
                            o0 = ui.mColStart[col];
                            o1 = ui.mColStart[col + 1];
                        }
                        for (U32 c = c0; c < c1; ++c)
                        {
                            const Cell& cell = mCells[c];
                            U32 best = NONE;
                            S32 best_step = 0;
                            for (U32 o = o0; o < o1; ++o)
                            {
                                const Cell& other = mCells[o];
                                const S32 step = std::abs(other.mFloor - cell.mFloor);
                                const S32 overlap = std::min(other.mCeil, cell.mCeil) - std::max(other.mFloor, cell.mFloor);
                                if (step <= mClimb && overlap >= mWalkOverlap && (best == NONE || step < best_step))
                                {
                                    best = o;
                                    best_step = step;
                                }
                            }
                            if (best != NONE)
                            {
                                mLink[c * 4 + d] = best;
                                mSide[c * 4 + d] = SIDE_LINKED;
                            }
                        }
                    }
                }
            }
        }
    }

    void SpacesPass::State::transform()
    {
        SK_NAV_ZONE("nav.spaces.transform");
        // Nearest-wall propagation: each cell carries the point on the
        // nearest side where floor stops, and hands it on while that is nearer.
        const size_t n = mCells.size();
        const F32 cap = params().mClearMaxM;
        // What a relaxation reads and writes of a cell, together: the propagation's order scatters it over the mesh.
        struct Near
        {
            LLVector2 mPos;
            LLVector2 mNearest;
            F32 mDist;
        };
        std::vector<Near> cells(n);
        for (U32 c = 0; c < n; ++c)
        {
            const TileInfo& ti = mTiles[mCells[c].mTile];
            cells[c].mPos.set(cellX(ti, mCells[c].mX), cellY(ti, mCells[c].mY));
            cells[c].mDist = std::numeric_limits<F32>::max();
        }
        // Dial's buckets at a sixteenth of a cell: the order is exact to
        // well under a cell, and a push is a push_back.
        const F32 step = mCell / 16.f;
        std::vector<std::vector<U32>> buckets(static_cast<size_t>(cap / step) + 2);
        auto bucket = [&](F32 d) { return std::min(buckets.size() - 1, static_cast<size_t>(d / step)); };
        for (U32 c = 0; c < n; ++c)
        {
            const Cell& cell = mCells[c];
            const TileInfo& ti = mTiles[cell.mTile];
            for (S32 d = 0; d < 4; ++d)
            {
                if (mSide[c * 4 + d] == SIDE_WALL)
                {
                    cells[c].mNearest.set(cellX(ti, cell.mX) + DX[d] * 0.5f * mCell, cellY(ti, cell.mY) + DY[d] * 0.5f * mCell);
                    cells[c].mDist = 0.5f * mCell;
                    buckets[bucket(cells[c].mDist)].push_back(c);
                    break;
                }
            }
        }
        for (size_t b = 0; b < buckets.size(); ++b)
        {
            for (size_t i = 0; i < buckets[b].size(); ++i)
            {
                const U32 c = buckets[b][i];
                const Near& from = cells[c];
                if (bucket(from.mDist) != b)
                {
                    continue;
                }
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = mLink[c * 4 + d];
                    if (o == NONE)
                    {
                        continue;
                    }
                    Near& to = cells[o];
                    const F32 dd = dist_vec(to.mPos, from.mNearest);
                    if (dd < to.mDist - 1e-6f && dd <= cap)
                    {
                        to.mDist = dd;
                        to.mNearest = from.mNearest;
                        buckets[std::max(b, bucket(dd))].push_back(o);
                    }
                }
            }
            std::vector<U32>().swap(buckets[b]);
        }
        mDist.resize(n);
        for (U32 c = 0; c < n; ++c)
        {
            mDist[c] = std::min(cells[c].mDist, cap);
        }
    }

    std::shared_ptr<FloorMemo::Tile> SpacesPass::State::tileFloors(U32 t, std::vector<U32>& cells) const
    {
        SK_NAV_ZONE("nav.spaces.floors.tile");
        // The tile's floor cells in buildCells' order, linked as it links them, components joined through links either way.
        auto out = std::make_shared<FloorMemo::Tile>();
        FloorMemo::Tile& f = *out;
        const TileInfo& ti = mTiles[t];
        const TileAir& a = *ti.mAir;
        const S32 w = a.mWidth, h = a.mHeight;
        f.mAir = ti.mAir;
        std::vector<U32> col_start(static_cast<size_t>(w) * h + 1, 0);
        std::vector<U32> gaps;
        std::vector<S32> floors, ceils;
        AirColumn col;
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const S32 c = ix + iy * w;
                col_start[c] = static_cast<U32>(gaps.size());
                airColumn(a, ti.mLeafMap[c], col);
                for (U32 i = 0; i < col.mCount; ++i)
                {
                    const AirGap& g = a.mGaps[col.mGap[i]];
                    if (floorGap(t, g))
                    {
                        gaps.push_back(col.mGap[i]);
                        floors.push_back(floorAt(t, col, i, ix, iy));
                        ceils.push_back(ceilOf(t, g));
                    }
                }
            }
        }
        const U32 n = static_cast<U32>(gaps.size());
        col_start.back() = n;
        UnionFind uf;
        uf.reset(n);
        std::vector<U32> open(n, 0), drops(n, 0);
        std::vector<U8> edge(n, 0);
        struct Pending
        {
            U32 mCell;
            U8 mDir;
            U16 mX, mY;
            U32 mGap;
        };
        std::vector<Pending> pending;
        std::vector<U32> o_gap;
        std::vector<S32> o_floor, o_ceil;
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const U32 c0 = col_start[ix + iy * w], c1 = col_start[ix + iy * w + 1];
                for (S32 d = 0; d < 4 && c0 < c1; ++d)
                {
                    U32 u = NONE;
                    S32 x = 0, y = 0;
                    const S32 r = resolve(t, ix + DX[d], iy + DY[d], u, x, y);
                    if (r == -2)
                    {
                        for (U32 c = c0; c < c1; ++c)
                        {
                            edge[c] = 1;
                        }
                        continue;
                    }
                    // The neighbour column's floor cells, in its tile's order.
                    U32 n0 = 0, n1 = 0;
                    const bool inside = r == 0 && u == t;
                    if (inside)
                    {
                        n0 = col_start[x + y * w];
                        n1 = col_start[x + y * w + 1];
                    }
                    else if (r == 0)
                    {
                        o_gap.clear();
                        o_floor.clear();
                        o_ceil.clear();
                        AirColumn oc;
                        for (U32 i = 0; i < column(u, x, y, oc); ++i)
                        {
                            const AirGap& g = air(u).mGaps[oc.mGap[i]];
                            if (floorGap(u, g))
                            {
                                o_gap.push_back(oc.mGap[i]);
                                o_floor.push_back(floorAt(u, oc, i, x, y));
                                o_ceil.push_back(ceilOf(u, g));
                            }
                        }
                        n1 = static_cast<U32>(o_gap.size());
                    }
                    AirColumn nc;
                    U32 count = 0;
                    bool counted = false;
                    for (U32 c = c0; c < c1; ++c)
                    {
                        U32 best = NONE;
                        S32 best_step = 0;
                        for (U32 o = n0; o < n1; ++o)
                        {
                            const S32 of = inside ? floors[o] : o_floor[o];
                            const S32 oc = inside ? ceils[o] : o_ceil[o];
                            const S32 step = std::abs(of - floors[c]);
                            const S32 overlap = std::min(oc, ceils[c]) - std::max(of, floors[c]);
                            if (step <= mClimb && overlap >= mWalkOverlap && (best == NONE || step < best_step))
                            {
                                best = o;
                                best_step = step;
                            }
                        }
                        if (best != NONE)
                        {
                            if (inside)
                            {
                                uf.unite(c, best);
                            }
                            else
                            {
                                pending.push_back({ c, static_cast<U8>(d), static_cast<U16>(x), static_cast<U16>(y), o_gap[best] });
                            }
                            continue;
                        }
                        // No floor to step to: open, and a drop when the floor beyond is well below.
                        ++open[c];
                        if (r != 0)
                        {
                            continue;   // the mesh's edge ends open ground as a wall or void does
                        }
                        if (!counted)
                        {
                            count = column(u, x, y, nc);
                            counted = true;
                        }
                        for (U32 i = 0; i < count; ++i)
                        {
                            const AirGap& gi = air(u).mGaps[nc.mGap[i]];
                            const S32 fl = floorAt(u, nc, i, x, y);
                            if (fl <= floors[c] - 1 && ceilOf(u, gi) >= floors[c] + 14)
                            {
                                drops[c] += fl < floors[c] - mClimb ? 1 : 0;
                                break;
                            }
                        }
                    }
                }
            }
        }
        cells.assign(n, NONE);
        for (U32 c = 0; c < n; ++c)
        {
            const U32 root = uf.find(c);
            if (cells[root] == NONE)
            {
                cells[root] = static_cast<U32>(f.mComps.size());
                f.mComps.emplace_back();
            }
            cells[c] = cells[root];
            FloorMemo::Comp& k = f.mComps[cells[c]];
            ++k.mCells;
            k.mOpen += open[c];
            k.mDrops += drops[c];
            k.mEdge = k.mEdge || edge[c];
        }
        for (const Pending& q : pending)
        {
            f.mCross.push_back({ cells[q.mCell], q.mDir, q.mX, q.mY, q.mGap });
        }
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                if (ix != 0 && iy != 0 && ix != w - 1 && iy != h - 1)
                {
                    continue;
                }
                for (U32 c = col_start[ix + iy * w]; c < col_start[ix + iy * w + 1]; ++c)
                {
                    f.mBorder.emplace_back(FloorMemo::cellKey(ix, iy, gaps[c]), cells[c]);
                }
            }
        }
        std::sort(f.mBorder.begin(), f.mBorder.end());
        return out;
    }

    void SpacesPass::State::floors()
    {
        SK_NAV_ZONE("nav.spaces.floors");
        const FloorMemo* old = mInput->mPrevious ? mInput->mPrevious->mFloors.get() : nullptr;
        if (old && (old->mClimb != mClimb || old->mWalkHead != mWalkHead || old->mWalkOverlap != mWalkOverlap))
        {
            old = nullptr;
        }
        mFloors = std::make_shared<FloorMemo>();
        mFloors->mClimb = mClimb;
        mFloors->mWalkHead = mWalkHead;
        mFloors->mWalkOverlap = mWalkOverlap;
        const size_t tiles = mTiles.size();
        mFloorTiles.assign(tiles, nullptr);
        mFloorCells.assign(tiles, std::vector<U32>());
        mFloorBase.assign(tiles + 1, 0);
        for (U32 t = 0; t < tiles; ++t)
        {
            const TileInfo& ti = mTiles[t];
            TileAirPtr near_air[4];
            S8 near_state[4];
            for (S32 d = 0; d < 4; ++d)
            {
                TileKey k = ti.mKey;
                k.mX += DX[d];
                k.mY += DY[d];
                const U32 u = tileAt(k);
                near_state[d] = u != NONE ? 0 : mEnc->tile(k) ? -2 : -1;
                if (u != NONE)
                {
                    near_air[d] = mTiles[u].mAir;
                }
            }
            std::shared_ptr<const FloorMemo::Tile> kept;
            if (old)
            {
                const auto it = old->mTiles.find(ti.mKey);
                if (it != old->mTiles.end() && it->second->mAir == ti.mAir)
                {
                    kept = it->second;
                    for (S32 d = 0; d < 4 && kept; ++d)
                    {
                        if (kept->mNearState[d] != near_state[d] || kept->mNear[d] != near_air[d])
                        {
                            kept = nullptr;
                        }
                    }
                }
            }
            if (!kept)
            {
                std::shared_ptr<FloorMemo::Tile> fresh = tileFloors(t, mFloorCells[t]);
                for (S32 d = 0; d < 4; ++d)
                {
                    fresh->mNear[d] = near_air[d];
                    fresh->mNearState[d] = near_state[d];
                }
                kept = fresh;
            }
            mFloorTiles[t] = kept;
            mFloors->mTiles.emplace(ti.mKey, kept);
            mFloorBase[t + 1] = mFloorBase[t] + static_cast<U32>(kept->mComps.size());
        }
        // Components joined across tile edges, by links either way.
        UnionFind uf;
        uf.reset(mFloorBase[tiles]);
        for (U32 t = 0; t < tiles; ++t)
        {
            for (const FloorMemo::Cross& q : mFloorTiles[t]->mCross)
            {
                TileKey k = mTiles[t].mKey;
                k.mX += DX[q.mDir];
                k.mY += DY[q.mDir];
                const U32 u = tileAt(k);
                const U32 other = u == NONE ? NONE : mFloorTiles[u]->borderComp(q.mX, q.mY, q.mGap);
                if (other != NONE)
                {
                    uf.unite(mFloorBase[t] + q.mComp, mFloorBase[u] + other);
                }
            }
        }
        const size_t comps = mFloorBase[tiles];
        std::vector<U64> cells(comps, 0), open(comps, 0), drop(comps, 0);
        std::vector<U8> edge(comps, 0);
        for (U32 t = 0; t < tiles; ++t)
        {
            for (U32 i = 0; i < mFloorTiles[t]->mComps.size(); ++i)
            {
                const FloorMemo::Comp& k = mFloorTiles[t]->mComps[i];
                const U32 root = uf.find(mFloorBase[t] + i);
                cells[root] += k.mCells;
                open[root] += k.mOpen;
                drop[root] += k.mDrops;
                edge[root] |= k.mEdge ? 1 : 0;
            }
        }
        const F64 cell_area = static_cast<F64>(mCell) * mCell;
        const F64 min_area = params().mAvatarAreaM2;
        mFloorAvatar.assign(comps, 0);
        for (U32 i = 0; i < comps; ++i)
        {
            const U32 root = uf.find(i);
            const F64 frac = static_cast<F64>(drop[root]) / std::max<F64>(1.0, static_cast<F64>(open[root]));
            mFloorAvatar[i] = frac < 0.5 && (cells[root] * cell_area >= min_area || edge[root]) ? 1 : 0;
        }
    }

    void SpacesPass::State::cutSeeds(size_t b)
    {
        SK_NAV_ZONE("nav.spaces.cut.seeds");
        const size_t n = mCells.size();

        // Floors an avatar stands on: walkable components of the whole mesh not ended mostly by drops.
        if (!mFloors)
        {
            floors();
        }
        std::vector<U32> stack;
        mAvatar.assign(n, 0);
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            if (!ti.mWork)
            {
                continue;
            }
            if (mFloorCells[t].empty() && ti.mColStart.back() > ti.mColStart.front())
            {
                std::vector<U32> cells;
                tileFloors(t, cells);
                mFloorCells[t] = std::move(cells);
            }
            const std::vector<U32>& cells = mFloorCells[t];
            const U32 first = ti.mColStart.front();
            llassert(cells.size() == ti.mColStart.back() - first);
            for (U32 c = first; c < ti.mColStart.back(); ++c)
            {
                mAvatar[c] = mFloorAvatar[mFloorBase[t] + cells[c - first]];
            }
        }

        wallEnds();
        apertureCuts();
        // The watershed over the clearance, off flights: descend its levels,
        // grow the regions there are into what each level exposes, then seed
        // new ones from what is left.
        // What the watershed reads of a neighbour, together: its growth scatters over the mesh.
        struct Shed
        {
            S32 mLabel;
            F32 mDist;
            U32 mStamp;
            U16 mLevel;
            U8 mGround;
        };
        std::vector<Shed> shed(n);
        for (U32 c = 0; c < n; ++c)
        {
            shed[c].mLabel = -1;
            shed[c].mDist = mDist[c];
            shed[c].mStamp = NONE;
            shed[c].mLevel = static_cast<U16>(static_cast<U32>(mDist[c] / CLEAR_UNIT_M));
            shed[c].mGround = mAvatar[c] && !mCells[c].mFlight;
        }
        std::vector<std::vector<U32>> levels;
        for (U32 c = 0; c < n; ++c)
        {
            if (!shed[c].mGround)
            {
                continue;
            }
            const size_t lv = shed[c].mLevel;
            if (levels.size() <= lv)
            {
                levels.resize(lv + 1);
            }
            levels[lv].push_back(c);
        }
        S32 regions = 0;
        std::vector<std::pair<U32, S32>> assign;
        std::vector<U32> frontier, next;
        for (size_t lv = levels.size(); lv-- > 0;)
        {
            const std::vector<U32>& fresh = levels[lv];
            frontier.clear();
            for (U32 c : fresh)
            {
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = mLink[c * 4 + d];
                    if (o != NONE && shed[o].mLabel >= 0 && shed[o].mGround)
                    {
                        frontier.push_back(c);
                        break;
                    }
                }
            }
            for (S32 step = 0; step < GROW_STEPS && !frontier.empty(); ++step)
            {
                // Every cell of a layer takes the label of its labelled
                // neighbour furthest from a wall, all at once.
                assign.clear();
                for (U32 c : frontier)
                {
                    if (shed[c].mLabel >= 0)
                    {
                        continue;
                    }
                    S32 best = -1;
                    F32 best_d = -1.f;
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = mLink[c * 4 + d];
                        if (o != NONE && shed[o].mLabel >= 0 && shed[o].mGround && shed[o].mDist > best_d)
                        {
                            best = shed[o].mLabel;
                            best_d = shed[o].mDist;
                        }
                    }
                    if (best >= 0)
                    {
                        assign.emplace_back(c, best);
                    }
                }
                for (const auto& a : assign)
                {
                    shed[a.first].mLabel = a.second;
                }
                next.clear();
                const U32 mark = static_cast<U32>(step + lv * GROW_STEPS);
                for (const auto& a : assign)
                {
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = mLink[a.first * 4 + d];
                        if (o == NONE)
                        {
                            continue;
                        }
                        Shed& so = shed[o];
                        if (so.mLabel < 0 && so.mGround && so.mLevel >= lv && so.mStamp != mark)
                        {
                            so.mStamp = mark;
                            next.push_back(o);
                        }
                    }
                }
                frontier.swap(next);
            }
            for (U32 c : fresh)
            {
                if (shed[c].mLabel >= 0)
                {
                    continue;
                }
                const S32 id = regions++;
                shed[c].mLabel = id;
                stack.assign(1, c);
                while (!stack.empty())
                {
                    const U32 k = stack.back();
                    stack.pop_back();
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = mLink[k * 4 + d];
                        if (o != NONE && shed[o].mLabel < 0 && shed[o].mGround && shed[o].mLevel >= lv)
                        {
                            shed[o].mLabel = id;
                            stack.push_back(o);
                        }
                    }
                }
            }
        }
        mLabel.resize(n);
        for (U32 c = 0; c < n; ++c)
        {
            mLabel[c] = shed[c].mLabel;
        }
        std::vector<Shed>().swap(shed);
        // Each flight is a region of its own.
        for (U32 c = 0; c < n; ++c)
        {
            if (!mAvatar[c] || !mCells[c].mFlight || mLabel[c] >= 0)
            {
                continue;
            }
            const S32 id = regions++;
            mLabel[c] = id;
            stack.assign(1, c);
            while (!stack.empty())
            {
                const U32 k = stack.back();
                stack.pop_back();
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = mLink[k * 4 + d];
                    if (o != NONE && mLabel[o] < 0 && mAvatar[o] && mCells[o].mFlight)
                    {
                        mLabel[o] = id;
                        stack.push_back(o);
                    }
                }
            }
        }

        // The block's tiles cut their seeds from the labels: a leaf gap takes
        // the label most of its cells have. Ring tiles note the labels along
        // their edges, for the seams.
        std::vector<std::pair<S32, U32>> votes;
        auto majority = [this, &votes](const TileInfo& ti, const AirLeaf& leaf, U32 gap) {
            const TileAir& a = *ti.mAir;
            const S32 s = 1 << leaf.mLevel;
            S32 best = -1;
            U32 best_votes = 0;
            votes.clear();
            for (S32 y = leaf.mY; y < std::min(a.mHeight, leaf.mY + s); ++y)
            {
                for (S32 x = leaf.mX; x < std::min(a.mWidth, leaf.mX + s); ++x)
                {
                    const S32 col = x + y * a.mWidth;
                    for (U32 c = ti.mColStart[col]; c < ti.mColStart[col + 1]; ++c)
                    {
                        if (mCells[c].mGap == gap && mLabel[c] >= 0)
                        {
                            auto it = std::find_if(votes.begin(), votes.end(), [&](const std::pair<S32, U32>& v) { return v.first == mLabel[c]; });
                            if (it == votes.end())
                            {
                                votes.emplace_back(mLabel[c], 0);
                                it = votes.end() - 1;
                            }
                            const U32 v = ++it->second;
                            if (v > best_votes || (v == best_votes && mLabel[c] < best))
                            {
                                best = mLabel[c];
                                best_votes = v;
                            }
                        }
                    }
                }
            }
            return best;
        };
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            TileInfo& ti = mTiles[t];
            if (!ti.mWork)
            {
                continue;
            }
            const TileAir& a = *ti.mAir;
            if (!ti.mInBlock)
            {
                // Only the edges it shares with the block make seams with it;
                // its far edges meet tiles cut when it was. Its edges with other
                // ring tiles only check that its kept seeds agree with the cut.
                // A basin can pass a tile corner alone: a tile diagonal to it shares that corner's leaves.
                U8 facing[4], corner[4];
                for (S32 d = 0; d < 4; ++d)
                {
                    TileKey k = ti.mKey;
                    k.mX += DX[d];
                    k.mY += DY[d];
                    U32 o = tileAt(k);
                    facing[d] = o == NONE ? 0 : mTiles[o].mInBlock ? 2 : mTiles[o].mWork ? 1 : 0;
                    k = ti.mKey;
                    k.mX += (d & 1) ? -1 : 1;
                    k.mY += (d & 2) ? -1 : 1;
                    o = tileAt(k);
                    corner[d] = o == NONE ? 0 : mTiles[o].mInBlock ? 2 : mTiles[o].mWork ? 1 : 0;
                }
                for (const AirLeaf& leaf : a.mLeaves)
                {
                    const S32 s = 1 << leaf.mLevel;
                    const bool hx = leaf.mX + s >= a.mWidth, lx = leaf.mX <= 0, hy = leaf.mY + s >= a.mHeight, ly = leaf.mY <= 0;
                    const U8 seam = std::max({ hx ? facing[0] : U8(0), lx ? facing[1] : U8(0), hy ? facing[2] : U8(0), ly ? facing[3] : U8(0),
                                               hx && hy ? corner[0] : U8(0), lx && hy ? corner[1] : U8(0), hx && ly ? corner[2] : U8(0),
                                               lx && ly ? corner[3] : U8(0) });
                    if (!seam)
                    {
                        continue;
                    }
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const S32 label = majority(ti, leaf, leaf.mFirstGap + i);
                        if (label >= 0)
                        {
                            (seam == 2 ? mShadow : mSeamShadow)[b][(static_cast<U64>(t) << 32) | (leaf.mFirstGap + i)] = mLabelBase + label;
                        }
                    }
                }
                continue;
            }
            RoomsTile& rt = ti.mRooms;
            rt = RoomsTile();
            rt.mKey = ti.mKey;
            rt.mAir = ti.mAir;
            rt.mSeeds.assign(a.mGaps.size(), 0);
            ti.mSeedLabels.clear();
            std::unordered_map<S32, U16> local;
            // Most gaps see one label: count that, and sort only the rest.
            std::vector<S32> first(a.mGaps.size(), -1);
            std::vector<U32> count(a.mGaps.size(), 0);
            std::vector<std::pair<U32, S32>> others;
            for (U32 c = ti.mColStart.front(); c < ti.mColStart.back(); ++c)
            {
                const S32 label = mLabel[c];
                const U32 g = mCells[c].mGap;
                if (label < 0)
                {
                    continue;
                }
                if (first[g] < 0 || first[g] == label)
                {
                    first[g] = label;
                    ++count[g];
                }
                else
                {
                    others.emplace_back(g, label);
                }
            }
            std::sort(others.begin(), others.end());
            size_t o = 0;
            for (U32 gap = 0; gap < a.mGaps.size(); ++gap)
            {
                if (first[gap] < 0)
                {
                    continue;
                }
                S32 best = first[gap];
                size_t best_n = count[gap];
                while (o < others.size() && others[o].first == gap)
                {
                    size_t j = o;
                    while (j < others.size() && others[j] == others[o])
                    {
                        ++j;
                    }
                    if (j - o > best_n || (j - o == best_n && others[o].second < best))
                    {
                        best = others[o].second;
                        best_n = j - o;
                    }
                    o = j;
                }
                {
                    auto it = local.find(best);
                    if (it == local.end())
                    {
                        if (local.size() >= 0xfffe)
                        {
                            continue;
                        }
                        it = local.emplace(best, static_cast<U16>(local.size() + 1)).first;
                        ti.mSeedLabels.push_back(mLabelBase + best);
                    }
                    rt.mSeeds[gap] = it->second;
                }
            }
            rt.mStats.assign(local.size(), SeedStats());
            for (SeedStats& st : rt.mStats)
            {
                st.mFloorMin = INT_MAX;
                st.mFloorMax = INT_MIN;
            }
            // What stays true of a seed while its air does: size, shape,
            // walls, flight cells and the raster its space is tracked by.
            for (U32 c = ti.mColStart.front(); c < ti.mColStart.back(); ++c)
            {
                const Cell& cell = mCells[c];
                const U16 seed = rt.mSeeds[cell.mGap];
                if (!seed || !mAvatar[c])
                {
                    continue;
                }
                SeedStats& st = rt.mStats[seed - 1];
                const F32 x = cellX(ti, cell.mX), y = cellY(ti, cell.mY);
                ++st.mCells;
                st.mFlightCells += cell.mFlight ? 1 : 0;
                st.mFloorMin = std::min(st.mFloorMin, cell.mFloor);
                st.mFloorMax = std::max(st.mFloorMax, cell.mFloor);
                st.mSumX += x;
                st.mSumY += y;
                st.mSumXX += static_cast<F64>(x) * x;
                st.mSumXY += static_cast<F64>(x) * y;
                st.mSumYY += static_cast<F64>(y) * y;
                bool edge = false;
                for (S32 d = 0; d < 4; ++d)
                {
                    edge |= mSide[c * 4 + d] == SIDE_WALL;
                }
                if (edge)
                {
                    ++st.mBoundary;
                    const S32 lo = cell.mFloor + mClimb, hi = cell.mFloor + static_cast<S32>(std::lround(2.f / mUnit));
                    bool wall = false;
                    for (S32 d = 0; d < 4 && !wall; ++d)
                    {
                        wall = blocked(cell.mTile, cell.mX + DX[d], cell.mY + DY[d], lo, hi);
                    }
                    st.mWalled += wall ? 1 : 0;
                }
            }
            // The raster a raster cell at a time, in key order, so each seed's comes out sorted.
            std::vector<U64> kx(a.mWidth), ky(a.mHeight);
            for (S32 ix = 0; ix < a.mWidth; ++ix)
            {
                kx[ix] = rasterKey(cellX(ti, ix), 0.f, 0.f) & ~RASTER_YZ;
            }
            for (S32 iy = 0; iy < a.mHeight; ++iy)
            {
                ky[iy] = rasterKey(0.f, cellY(ti, iy), 0.f) & RASTER_Y;
            }
            std::vector<U64> block;
            for (S32 x0 = 0, x1 = 0; x0 < a.mWidth; x0 = x1)
            {
                for (x1 = x0 + 1; x1 < a.mWidth && kx[x1] == kx[x0]; ++x1)
                {
                }
                for (S32 y0 = 0, y1 = 0; y0 < a.mHeight; y0 = y1)
                {
                    for (y1 = y0 + 1; y1 < a.mHeight && ky[y1] == ky[y0]; ++y1)
                    {
                    }
                    block.clear();
                    for (S32 iy = y0; iy < y1; ++iy)
                    {
                        for (S32 ix = x0; ix < x1; ++ix)
                        {
                            const S32 col = ix + iy * a.mWidth;
                            for (U32 c = ti.mColStart[col]; c < ti.mColStart[col + 1]; ++c)
                            {
                                const U16 seed = rt.mSeeds[mCells[c].mGap];
                                if (seed && mAvatar[c])
                                {
                                    block.push_back((static_cast<U64>(seed) << 21) | (rasterKey(0.f, 0.f, mCells[c].mFloor * mUnit) & RASTER_Z));
                                }
                            }
                        }
                    }
                    std::sort(block.begin(), block.end());
                    for (size_t i = 0; i < block.size();)
                    {
                        size_t j = i;
                        while (j < block.size() && block[j] == block[i])
                        {
                            ++j;
                        }
                        rt.mStats[(block[i] >> 21) - 1].mRaster.emplace_back(kx[x0] | ky[y0] | (block[i] & RASTER_Z), static_cast<U32>(j - i));
                        i = j;
                    }
                }
            }
        }
        mLabelBase += regions;
    }

    void SpacesPass::State::number()
    {
        SK_NAV_ZONE("nav.spaces.seeds");
        // Every tile's seeds, numbered across the mesh.
        U32 total = 0;
        for (TileInfo& ti : mTiles)
        {
            ti.mSeedBase = total;
            total += static_cast<U32>(ti.mRooms.mStats.size());
        }
        mSeeds.assign(total, SeedInfo());
        mSeedLabel.assign(total, -1);
        mProduct->mSeeds = total;
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            for (U32 s = 0; s < ti.mRooms.mStats.size(); ++s)
            {
                SeedInfo& si = mSeeds[ti.mSeedBase + s];
                si.mTile = t;
                si.mLocal = s;
                const SeedStats& st = ti.mRooms.mStats[s];
                si.mFlight = st.mFlightCells * 2 > st.mCells;
                if (ti.mBatch >= 0 && s < ti.mSeedLabels.size())
                {
                    mSeedLabel[ti.mSeedBase + s] = ti.mSeedLabels[s];
                }
            }
            // What changes with the enclosure pass: sky, indoorness, heights.
            const TileAir& a = *ti.mAir;
            const TileEnclosure& te = mEnc->mTiles[ti.mEnc];
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const F64 area = static_cast<F64>(1 << leaf.mLevel) * (1 << leaf.mLevel) * mCell * mCell;
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 g = leaf.mFirstGap + i;
                    const U16 seed = ti.mRooms.mSeeds[g];
                    if (!seed)
                    {
                        continue;
                    }
                    SeedInfo& si = mSeeds[ti.mSeedBase + seed - 1];
                    const AirGap& gap = a.mGaps[g];
                    const GapEnclosure& ge = te.mGaps[g];
                    si.mWeight += area;
                    si.mS += area * ge.mS / 255.0;
                    si.mCover += area * ge.mCover / 255.0;
                    si.mSideOpen += area * ge.mOpen / 255.0;
                    if (ge.mD != ENCLOSURE_D_UNKNOWN)
                    {
                        const F32 e = ge.mE / 255.f;
                        si.mKnown += area;
                        si.mE += area * e;
                        si.mEMin = std::min(si.mEMin, e);
                        si.mEMax = std::max(si.mEMax, e);
                    }
                    const S32 f = floorOf(t, gap), c = ceilOf(t, gap);
                    if (c >= OPEN_TOP)
                    {
                        si.mOpenArea += area;
                        si.mVolume += area * OPEN_HEIGHT_M;
                    }
                    else
                    {
                        const F32 cz = c * mUnit;
                        si.mVolume += area * (c - f) * mUnit;
                        si.mCeilSum += area * cz;
                        si.mCeilArea += area;
                        si.mCeilMin = std::min(si.mCeilMin, cz);
                        si.mCeilMax = std::max(si.mCeilMax, cz);
                    }
                }
            }
        }
        mRegions.reset(total);
        // Seeds cut from one watershed label are one region already: a tile
        // seam alone never parts them.
        std::unordered_map<S32, U32> first;
        for (U32 s = 0; s < total; ++s)
        {
            if (mSeedLabel[s] >= 0)
            {
                unite(first.emplace(mSeedLabel[s], s).first->second, s);
            }
        }
        // A kept seed of a ring tile is in the basin the cut's watershed labelled its gaps with:
        // seeds of ring tiles in one basin are one region, as a full pass would cut them.
        std::unordered_map<U32, S32> ring_label;
        for (size_t pass = 0; pass < 1 && !mShadow.empty(); ++pass)
        {
            for (const auto& [where, label] : mShadow[0])
            {
                const U32 t = static_cast<U32>(where >> 32);
                const TileInfo& ti = mTiles[t];
                const U32 gap = static_cast<U32>(where);
                if (ti.mBatch >= 0 || gap >= ti.mRooms.mSeeds.size() || !ti.mRooms.mSeeds[gap])
                {
                    continue;
                }
                const U32 s = ti.mSeedBase + ti.mRooms.mSeeds[gap] - 1;
                ring_label.emplace(s, label);
            }
        }
        std::vector<std::pair<S32, U32>> ring_seeds;
        for (const auto& [s, label] : ring_label)
        {
            ring_seeds.emplace_back(label, s);
        }
        std::sort(ring_seeds.begin(), ring_seeds.end());
        for (const auto& [label, s] : ring_seeds)
        {
            const auto it = first.emplace(label, s).first;
            if (it->second != s)
            {
                unite(it->second, s);
            }
        }
        // Nor does it part seeds an earlier pass joined, while both their tiles stand and no basin now parts them.
        if (mInput->mPrevious)
        {
            for (const SeedLink& l : mInput->mPrevious->mLinks)
            {
                const U32 a = tileAt(l.mA), b = tileAt(l.mB);
                if (a == NONE || b == NONE)
                {
                    continue;
                }
                const TileInfo& ta = mTiles[a];
                const TileInfo& tb = mTiles[b];
                if (ta.mDirty || tb.mDirty || l.mSeedA > ta.mRooms.mStats.size() || l.mSeedB > tb.mRooms.mStats.size())
                {
                    continue;
                }
                const U32 sa = ta.mSeedBase + l.mSeedA - 1, sb = tb.mSeedBase + l.mSeedB - 1;
                const auto la = ring_label.find(sa), lb = ring_label.find(sb);
                if (la != ring_label.end() && lb != ring_label.end() && la->second != lb->second)
                {
                    continue;
                }
                unite(sa, sb);
            }
        }
    }

    void SpacesPass::State::unite(U32 a, U32 b)
    {
        // Joined for want of any cut, not by cues: what the next pass keeps.
        if (mRegions.unite(a, b) && mSeeds[a].mTile != mSeeds[b].mTile)
        {
            SeedLink l;
            l.mA = mTiles[mSeeds[a].mTile].mKey;
            l.mSeedA = static_cast<U16>(mSeeds[a].mLocal + 1);
            l.mB = mTiles[mSeeds[b].mTile].mKey;
            l.mSeedB = static_cast<U16>(mSeeds[b].mLocal + 1);
            mProduct->mLinks.push_back(l);
        }
    }

    void SpacesPass::State::edges()
    {
        SK_NAV_ZONE("nav.spaces.edges");
        // Every level-0 step between floor gaps of two seeds, within and across tiles.
        mEdges.clear();
        AirColumn colb;
        S32 floor_a[0xff], floor_b[0xff];
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            const TileAir& a = *ti.mAir;
            for (const AirLeaf& leaf : a.mLeaves)
            {
                bool seeded = false;
                for (U32 i = 0; i < leaf.mGapCount && !seeded; ++i)
                {
                    seeded = ti.mRooms.mSeeds[leaf.mFirstGap + i] != 0;
                }
                if (!seeded)
                {
                    continue;
                }
                const S32 s = 1 << leaf.mLevel;
                for (S32 side = 0; side < 2; ++side)
                {
                    for (S32 k = 0; k < s; ++k)
                    {
                        const S32 ax = side == 0 ? leaf.mX + s - 1 : leaf.mX + k;
                        const S32 ay = side == 0 ? leaf.mY + k : leaf.mY + s - 1;
                        if (ax >= a.mWidth || ay >= a.mHeight)
                        {
                            break;
                        }
                        U32 u = t;
                        S32 bx = ax + (side == 0), by = ay + (side == 1);
                        if ((bx >= a.mWidth || by >= a.mHeight) && resolve(t, bx, by, u, bx, by) != 0)
                        {
                            continue;
                        }
                        const TileInfo& ui = mTiles[u];
                        // b's column, less the air a leaf over both cells holds: that is one gap, not a step.
                        colb.mCount = 0;
                        for (S32 lb = ui.mLeafMap[bx + by * ui.mAir->mWidth]; lb >= 0; lb = parentLeaf(*ui.mAir, lb))
                        {
                            const AirLeaf& o = ui.mAir->mLeaves[lb];
                            if (u == t && ax >= o.mX && ay >= o.mY && ax < o.mX + (1 << o.mLevel) && ay < o.mY + (1 << o.mLevel))
                            {
                                break;
                            }
                            for (U32 j = 0; j < o.mGapCount; ++j)
                            {
                                U32 at = colb.mCount++;
                                while (at > 0 && ui.mAir->mGaps[colb.mGap[at - 1]].mFloor > ui.mAir->mGaps[o.mFirstGap + j].mFloor)
                                {
                                    colb.mGap[at] = colb.mGap[at - 1];
                                    colb.mLeaf[at] = colb.mLeaf[at - 1];
                                    --at;
                                }
                                colb.mGap[at] = o.mFirstGap + j;
                                colb.mLeaf[at] = lb;
                            }
                        }
                        const U32 nb = colb.mCount;
                        if (!nb)
                        {
                            continue;
                        }
                        // Within one seed on both sides, as most steps are, there is no edge.
                        if (nb == 1 && leaf.mGapCount == 1 && u == t && ui.mRooms.mSeeds[colb.mGap[0]] == ti.mRooms.mSeeds[leaf.mFirstGap])
                        {
                            continue;
                        }
                        auto gb = [&](U32 j) -> const AirGap& { return ui.mAir->mGaps[colb.mGap[j]]; };
                        const LLVector2 ca(cellX(ti, ax), cellY(ti, ay)), cb(cellX(ui, bx), cellY(ui, by));
                        for (U32 j = 0; j < nb; ++j)
                        {
                            floor_b[j] = floorAt(u, colb, j, bx, by);
                        }
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            floor_a[i] = airFloorAt(a.mGaps[leaf.mFirstGap + i], leaf, ax, ay) + ti.mZOff;
                        }
                        // Where both columns' gaps rise with their floors, as air's do, only b's within a climb of a's are tried.
                        bool rising = true;
                        for (U32 j = 1; j < nb && rising; ++j)
                        {
                            rising = floor_b[j] > floor_b[j - 1];
                        }
                        for (U32 i = 1; i < leaf.mGapCount && rising; ++i)
                        {
                            rising = floor_a[i] > floor_a[i - 1];
                        }
                        U32 j0 = 0;
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            const U32 ga = leaf.mFirstGap + i;
                            const U16 sa = ti.mRooms.mSeeds[ga];
                            if (!sa)
                            {
                                continue;
                            }
                            const AirGap& gap_a = a.mGaps[ga];
                            const S32 fa = floor_a[i], ca_top = ceilOf(t, gap_a);
                            while (rising && j0 < nb && floor_b[j0] < fa - mClimb)
                            {
                                ++j0;
                            }
                            for (U32 j = rising ? j0 : 0; j < nb; ++j)
                            {
                                if (rising && floor_b[j] > fa + mClimb)
                                {
                                    break;
                                }
                                const U16 sb = ui.mRooms.mSeeds[colb.mGap[j]];
                                if (!sb)
                                {
                                    continue;
                                }
                                const U32 seed_a = ti.mSeedBase + sa - 1, seed_b = ui.mSeedBase + sb - 1;
                                if (seed_a == seed_b)
                                {
                                    continue;
                                }
                                const S32 fb = floor_b[j], cb_top = ceilOf(u, gb(j));
                                if (std::abs(fa - fb) > mClimb || std::min(ca_top, cb_top) - std::max(fa, fb) < mWalkOverlap)
                                {
                                    continue;
                                }
                                Edge e;
                                e.mA = seed_a;
                                e.mB = seed_b;
                                e.mCellA = ca;
                                e.mCellB = cb;
                                e.mMid = (ca + cb) * 0.5f;
                                e.mFloorA = fa;
                                e.mFloorB = fb;
                                e.mTileA = t;
                                e.mGapA = ga;
                                e.mTileB = u;
                                e.mGapB = colb.mGap[j];
                                e.mFlight = ((gap_a.mFlags | gb(j).mFlags) & AirGap::FLIGHT) != 0;
                                mEdges.push_back(e);
                            }
                        }
                    }
                }
            }
        }
    }

    std::vector<Run> SpacesPass::State::runs(const std::vector<U32>& edge_ids, U32 ru) const
    {
        // Edges closer than the link are one run: two doorways in one wall are two.
        const F32 link = params().mRunLinkM;
        const size_t k = edge_ids.size();
        UnionFind uf;
        uf.reset(k);
        std::unordered_map<U64, std::vector<U32>> grid;
        auto cellKey = [](S32 x, S32 y) { return (static_cast<U64>(static_cast<U32>(x)) << 32) | static_cast<U32>(y); };
        for (U32 i = 0; i < k; ++i)
        {
            const LLVector2& p = mEdges[edge_ids[i]].mMid;
            grid[cellKey(static_cast<S32>(std::floor(p.mV[VX] / link)), static_cast<S32>(std::floor(p.mV[VY] / link)))].push_back(i);
        }
        for (const auto& bucket : grid)
        {
            const S32 cx = static_cast<S32>(bucket.first >> 32), cy = static_cast<S32>(bucket.first & 0xffffffff);
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const auto it = grid.find(cellKey(cx + dx, cy + dy));
                    if (it == grid.end())
                    {
                        continue;
                    }
                    for (U32 i : bucket.second)
                    {
                        for (U32 j : it->second)
                        {
                            if (i < j && dist_vec(mEdges[edge_ids[i]].mMid, mEdges[edge_ids[j]].mMid) <= link)
                            {
                                uf.unite(i, j);
                            }
                        }
                    }
                }
            }
        }
        std::map<U32, std::vector<U32>> clusters;
        for (U32 i = 0; i < k; ++i)
        {
            clusters[uf.find(i)].push_back(edge_ids[i]);
        }
        std::vector<Run> out;
        std::vector<S32> floors;
        for (auto& cl : clusters)
        {
            std::vector<U32>& ids = cl.second;
            std::sort(ids.begin(), ids.end());
            Run r;
            r.mEdges = static_cast<U32>(ids.size());
            LLVector2 c, step;
            U64 key = SEED;
            floors.clear();
            for (U32 id : ids)
            {
                const Edge& e = mEdges[id];
                c += e.mMid;
                // From the first region's cell to the second's.
                const bool a_first = mRegions.find(e.mA) == ru;
                step += a_first ? e.mCellB - e.mCellA : e.mCellA - e.mCellB;
                floors.push_back(e.mFloorA);
                floors.push_back(e.mFloorB);
                r.mFlight |= e.mFlight;
                key = mix(key, id);
            }
            c /= static_cast<F32>(ids.size());
            F64 sxx = 0.0, sxy = 0.0, syy = 0.0;
            for (U32 id : ids)
            {
                const LLVector2 q = mEdges[id].mMid - c;
                sxx += q.mV[VX] * q.mV[VX];
                sxy += q.mV[VX] * q.mV[VY];
                syy += q.mV[VY] * q.mV[VY];
            }
            LLVector2 d;
            if (ids.size() > 1)
            {
                F64 l1, l2;
                d = principal(sxx, sxy, syy, l1, l2);
            }
            else
            {
                d.set(-step.mV[VY], step.mV[VX]);
                if (d.normVec() < 1e-6f)
                {
                    d.set(1.f, 0.f);
                }
            }
            F32 t0 = 1e30f, t1 = -1e30f;
            for (U32 id : ids)
            {
                const F32 t = (mEdges[id].mMid - c) * d;
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
            r.mC = c;
            r.mD = d;
            r.mA = c + d * (t0 - 0.5f * mCell);
            r.mB = c + d * (t1 + 0.5f * mCell);
            r.mN.set(-d.mV[VY], d.mV[VX]);
            if (r.mN * step < 0.f)
            {
                r.mN = -r.mN;
            }
            r.mWidth = dist_vec(r.mA, r.mB);
            std::nth_element(floors.begin(), floors.begin() + floors.size() / 2, floors.end());
            r.mFloor = floors[floors.size() / 2];
            r.mKey = key;
            r.mIds = ids;
            out.push_back(r);
        }
        return out;
    }

    bool SpacesPass::State::relaxedAt(const Run& run) const
    {
        // A portal stood here last time: hold it with thresholds relaxed.
        const F32 fz = run.mFloor * mUnit;
        const S32 gx = static_cast<S32>(std::floor(run.mC.mV[VX])), gy = static_cast<S32>(std::floor(run.mC.mV[VY]));
        for (S32 dy = -1; dy <= 1; ++dy)
        {
            for (S32 dx = -1; dx <= 1; ++dx)
            {
                const auto it = mOldGrid.find(gridKey(gx + dx, gy + dy));
                if (it == mOldGrid.end())
                {
                    continue;
                }
                for (U32 i : it->second)
                {
                    const LLVector3& q = mOldPortals[i];
                    if (std::fabs(q.mV[VZ] - fz) <= params().mClimbM && dist_vec(LLVector2(q.mV[VX], q.mV[VY]), run.mC) <= 0.5f)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    Scored SpacesPass::State::score(const Run& run)
    {
        const auto hit = mScored.find(run.mKey);
        if (hit != mScored.end())
        {
            return hit->second;
        }
        const bool relaxed = relaxedAt(run);
        // The cut, and every tile its probes can reach: all a score reads besides the doors.
        MemoKey key;
        key.feed(static_cast<U64>(static_cast<U32>(run.mFloor)) | (static_cast<U64>(run.mFlight) << 32) | (static_cast<U64>(relaxed) << 33));
        key.feed(run.mC);
        key.feed(run.mA);
        key.feed(run.mB);
        key.feed(run.mD);
        key.feed(run.mN);
        key.feed(run.mWidth);
        const F32 x0 = std::min(std::min(run.mA.mV[VX], run.mB.mV[VX]), run.mC.mV[VX]) - SCORE_REACH_M;
        const F32 x1 = std::max(std::max(run.mA.mV[VX], run.mB.mV[VX]), run.mC.mV[VX]) + SCORE_REACH_M;
        const F32 y0 = std::min(std::min(run.mA.mV[VY], run.mB.mV[VY]), run.mC.mV[VY]) - SCORE_REACH_M;
        const F32 y1 = std::max(std::max(run.mA.mV[VY], run.mB.mV[VY]), run.mC.mV[VY]) + SCORE_REACH_M;
        for (S32 ky = tileIndex(y0); ky <= tileIndex(y1); ++ky)
        {
            for (S32 kx = tileIndex(x0); kx <= tileIndex(x1); ++kx)
            {
                TileKey k;
                k.mX = kx;
                k.mY = ky;
                const U32 t = tileAt(k);
                key.feed(t == NONE ? 0 : mTileKey[t]);
            }
        }
        Scored out;
        const MergeMemo::Score* old = nullptr;
        if (mOldMemo)
        {
            const auto it = mOldMemo->mScores.find(key.mA);
            old = it != mOldMemo->mScores.end() && it->second.mCheck == key.mB ? &it->second : nullptr;
        }
        if (old)
        {
            out = old->mScored;
            ++mProduct->mReused;
        }
        else
        {
            out = scoreFresh(run, relaxed);
        }
        MergeMemo::Score& kept = mMemo->mScores[key.mA];
        kept.mCheck = key.mB;
        kept.mScored = out;
        mScored[run.mKey] = out;
        return out;
    }

    Scored SpacesPass::State::scoreFresh(const Run& run, bool relaxed)
    {
        const SpaceParams& p = params();
        const F32 fz = run.mFloor * mUnit;
        const F32 drop = relaxed ? p.mRelaxed : p.mLintelDropM;
        const F32 step = relaxed ? p.mRelaxed : p.mEStep;

        std::vector<LLVector2> inner;
        if (run.mWidth > 0.3f)
        {
            const LLVector2 a = run.mA + run.mD * 0.1f, b = run.mB - run.mD * 0.1f;
            const F32 len = dist_vec(a, b);
            for (F32 t = 0.f; t <= len + 1e-4f; t += mCell)
            {
                inner.push_back(a + run.mD * t);
            }
        }
        else
        {
            inner.push_back(run.mC);
        }
        const F32 inf = std::numeric_limits<F32>::infinity();
        auto ceiling = [&](const Probe& pr) { return pr.mCeil >= OPEN_TOP ? inf : pr.mCeil * mUnit; };
        std::vector<F32> zs;
        Probe pr;
        for (const LLVector2& q : inner)
        {
            if (probe(q.mV[VX], q.mV[VY], run.mFloor, mClimb, pr))
            {
                zs.push_back(ceiling(pr));
            }
        }
        const F32 head = zs.empty() ? inf : median(zs);
        F32 sides[2];
        for (S32 k = 0; k < 2; ++k)
        {
            zs.clear();
            const F32 sgn = k == 0 ? 1.f : -1.f;
            for (F32 off : { 0.75f, 1.25f })
            {
                for (const LLVector2& q : inner)
                {
                    const LLVector2 at = q + run.mN * (sgn * off);
                    if (probe(at.mV[VX], at.mV[VY], run.mFloor, mClimb, pr))
                    {
                        zs.push_back(ceiling(pr));
                    }
                }
            }
            sides[k] = median(zs);
        }
        // Not over a flight: the slab above a stair's top is no lintel.
        bool lintel = std::isfinite(head) && !run.mFlight && !(std::isnan(sides[0]) && std::isnan(sides[1]));
        for (F32 s : sides)
        {
            lintel = lintel && (std::isnan(s) || s >= head + drop);
        }
        // Walled both sides means solid jambs, not just floor that ends.
        LLVector2 jamb_a, jamb_b;
        const bool jambs = aperture(run, jamb_a, jamb_b) && dist_vec(jamb_a, jamb_b) >= 2.f * p.mAgentRadiusM;
        F32 es[2];
        for (S32 k = 0; k < 2; ++k)
        {
            const F32 sgn = k == 0 ? 1.f : -1.f;
            F64 sum = 0.0;
            U32 count = 0;
            for (F32 off : { 1.f, 1.5f, 2.f })
            {
                for (const LLVector2& q : inner)
                {
                    const LLVector2 at = q + run.mN * (sgn * off);
                    if (probe(at.mV[VX], at.mV[VY], run.mFloor, mClimb, pr))
                    {
                        const GapEnclosure& ge = mEnc->mTiles[mTiles[pr.mTile].mEnc].mGaps[pr.mGap];
                        if (ge.mD != ENCLOSURE_D_UNKNOWN)
                        {
                            sum += ge.mE / 255.0;
                            ++count;
                        }
                    }
                }
            }
            es[k] = count ? static_cast<F32>(sum / count) : std::numeric_limits<F32>::quiet_NaN();
        }
        const bool estep = !std::isnan(es[0]) && !std::isnan(es[1]) && std::fabs(es[0] - es[1]) >= step;
        // The first door on the cut, of those whose cells the cut crosses.
        std::vector<U32> near_doors;
        {
            const S32 x0 = static_cast<S32>(std::floor(std::min(run.mA.mV[VX], run.mB.mV[VX])));
            const S32 x1 = static_cast<S32>(std::floor(std::max(run.mA.mV[VX], run.mB.mV[VX])));
            const S32 y0 = static_cast<S32>(std::floor(std::min(run.mA.mV[VY], run.mB.mV[VY])));
            const S32 y1 = static_cast<S32>(std::floor(std::max(run.mA.mV[VY], run.mB.mV[VY])));
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    const auto it = mDoorGrid.find(gridKey(x, y));
                    if (it != mDoorGrid.end())
                    {
                        near_doors.insert(near_doors.end(), it->second.begin(), it->second.end());
                    }
                }
            }
            std::sort(near_doors.begin(), near_doors.end());
            near_doors.erase(std::unique(near_doors.begin(), near_doors.end()), near_doors.end());
        }
        S32 door = -1;
        for (U32 i : near_doors)
        {
            const DoorSpan& d = mDoors[i];
            if (d.mHi < fz || d.mLo > fz + 2.f)
            {
                continue;   // on another floor
            }
            if (segmentSegment(d.mA, d.mB, run.mA, run.mB) <= p.mDoorReachM)
            {
                door = static_cast<S32>(i);
                break;
            }
        }
        Scored out;
        out.mCues = (lintel ? CUE_LINTEL : 0) | (jambs ? CUE_JAMBS : 0) | (estep ? CUE_E_STEP : 0) | (door >= 0 ? CUE_DOOR : 0);
        out.mHead = lintel ? head : 0.f;
        out.mDoor = door;
        out.mAperture = jambs;
        out.mJambA = jamb_a;
        out.mJambB = jamb_b;
        if (door >= 0)
        {
            out.mPortal = true;
            out.mKind = PortalKind::DOOR;
        }
        else if (lintel || (jambs && estep))
        {
            out.mPortal = true;
            out.mKind = PortalKind::OPENING;
        }
        return out;
    }

    bool SpacesPass::State::demoteFlatFlights(F32 min_rise)
    {
        SK_NAV_ZONE("nav.spaces.demoteflights");
        // A staircase pairs its ends. Within one flight region the stair
        // portals join by stairPairs - each exit to the nearest end within
        // reach in plan - and a pair has to climb min_rise, so a real
        // staircase's foot meets its own exit and a multi-storey well's foot
        // serves each of its exits. What finds no end in reach, pairs across
        // level ground, or opens wider than any staircase is a deck edge or
        // a rail. The seeds under such a cut stop flying, so the next round
        // sees floor to floor there and joins what the cut was splitting
        // instead of cutting it again; nothing structural changes on an
        // unmappable cut, which is left alone rather than re-tried forever.
        std::unordered_set<U32> flying;
        for (U32 s = 0; s < mSeeds.size(); ++s)
        {
            if (mSeeds[s].mFlight)
            {
                flying.insert(mRegions.find(s));
            }
        }
        const auto clearUnder = [&](const Portal& q)
        {
            bool cleared = false;
            const S32 kf = static_cast<S32>(std::lround(q.mSill / mUnit));
            const LLVector2 n(q.mNormal.mV[VX], q.mNormal.mV[VY]);
            const LLVector2 at[3] = { LLVector2(q.mCentre.mV[VX], q.mCentre.mV[VY]),
                                      LLVector2(q.mA.mV[VX], q.mA.mV[VY]),
                                      LLVector2(q.mB.mV[VX], q.mB.mV[VY]) };
            for (const LLVector2& c : at)
            {
                for (const F32 side : { 0.5f, -0.5f })
                {
                    const LLVector2 p = c + n * side;
                    Probe hit;
                    if (!probe(p.mV[VX], p.mV[VY], kf, mClimb + 2, hit))
                    {
                        continue;
                    }
                    const U16 local = mTiles[hit.mTile].mRooms.mSeeds[hit.mGap];
                    if (!local)
                    {
                        continue;
                    }
                    const U32 s = mTiles[hit.mTile].mSeedBase + local - 1;
                    if (mSeeds[s].mFlight && flying.count(mRegions.find(s)))
                    {
                        mSeeds[s].mFlight = false;
                        cleared = true;
                    }
                }
            }
            return cleared;
        };
        std::unordered_map<U32, std::vector<U32>> ends;
        for (U32 i = 0; i < mPortals.size(); ++i)
        {
            const Portal& q = mPortals[i];
            if (q.mKind != PortalKind::STAIR)
            {
                continue;
            }
            for (const U32 r : q.mSpaces)
            {
                if (flying.count(r))
                {
                    ends[r].push_back(i);
                }
            }
        }
        bool changed = false;
        for (const auto& [r, idx] : ends)
        {
            std::vector<U32> narrow;
            for (const U32 i : idx)
            {
                if (mPortals[i].mWidth <= STAIR_MAX_WIDTH_M)
                {
                    narrow.push_back(i);
                }
            }
            const std::vector<StairLink> pairs = stairPairs(mPortals, narrow, STAIR_PAIR_REACH_M);
            std::vector<bool> real(mPortals.size(), false);
            for (const StairLink& l : pairs)
            {
                if (mPortals[l.mTo].mSill - mPortals[l.mFrom].mSill >= min_rise)
                {
                    real[l.mFrom] = true;
                    real[l.mTo] = true;
                }
            }
            bool kept_any = false;
            for (const U32 i : idx)
            {
                if (real[i] || !clearUnder(mPortals[i]))
                {
                    kept_any = true;    // unmappable: kept rather than re-tried forever
                }
            }
            if (!kept_any)
            {
                // Nothing real is left in the flight: its seeds stop flying,
                // and the next round joins the floors it touched.
                for (U32 s = 0; s < mSeeds.size(); ++s)
                {
                    if (mSeeds[s].mFlight && mRegions.find(s) == r)
                    {
                        mSeeds[s].mFlight = false;
                        changed = true;
                    }
                }
            }
        }
        return changed;
    }

    void SpacesPass::State::collectApart()
    {
        // An aperture cut keeps its sides apart: whatever else they meet
        // through, the regions either side never merge into one.
        mApart.clear();
        for (const Run& r : mForced)
        {
            U32 sides[2];
            S32 found = 0;
            // Each side's seed is the first past the lintel, out of the cut's own strip.
            for (const F32 sgn : { 1.f, -1.f })
            {
                Probe pr;
                if (pastLintel(r, sgn, pr) && mTiles[pr.mTile].mRooms.mSeeds[pr.mGap])
                {
                    sides[found++] = mTiles[pr.mTile].mSeedBase + mTiles[pr.mTile].mRooms.mSeeds[pr.mGap] - 1;
                }
            }
            if (found == 2 && mRegions.find(sides[0]) != mRegions.find(sides[1]))
            {
                mApart.emplace_back(sides[0], sides[1]);
            }
        }
        // A pass that cuts no tile again keeps the cuts an earlier one made there.
        if (mInput->mPrevious)
        {
            for (const SeedLink& l : mInput->mPrevious->mApart)
            {
                const U32 a = tileAt(l.mA), b = tileAt(l.mB);
                if (a == NONE || b == NONE || mTiles[a].mDirty || mTiles[b].mDirty || l.mSeedA > mTiles[a].mRooms.mStats.size() ||
                    l.mSeedB > mTiles[b].mRooms.mStats.size())
                {
                    continue;
                }
                const U32 sa = mTiles[a].mSeedBase + l.mSeedA - 1, sb = mTiles[b].mSeedBase + l.mSeedB - 1;
                if (mRegions.find(sa) != mRegions.find(sb))
                {
                    mApart.emplace_back(sa, sb);
                }
            }
        }
    }

    bool SpacesPass::State::keptApart(UnionFind& uf, U32 u, U32 v) const
    {
        u = uf.find(u);
        v = uf.find(v);
        for (const auto& pq : mApart)
        {
            const U32 a = uf.find(pq.first), b = uf.find(pq.second);
            if ((a == u && b == v) || (a == v && b == u))
            {
                return true;
            }
        }
        return false;
    }

    void SpacesPass::State::releaseApart(UnionFind& uf, U32 u, U32 v)
    {
        u = uf.find(u);
        v = uf.find(v);
        mApart.erase(std::remove_if(mApart.begin(), mApart.end(),
                                    [&](const std::pair<U32, U32>& pq) {
                                        const U32 a = uf.find(pq.first), b = uf.find(pq.second);
                                        return (a == u && b == v) || (a == v && b == u);
                                    }),
                     mApart.end());
    }

    void SpacesPass::State::absorbFragments()
    {
        SK_NAV_ZONE("nav.spaces.merge.absorb");
        // A region smaller than an avatar's floor is a fragment of a cut, not
        // a space: smallest first, it joins the neighbour it shares the most
        // boundary with before any cut is scored, flight or floor, and the
        // joined region flies when most of its floor does.
        const U32 total = static_cast<U32>(mSeeds.size());
        const F64 cell_area = static_cast<F64>(mCell) * mCell;
        const F64 min_area = params().mAvatarAreaM2;
        std::unordered_map<U32, F64> area, flying;
        std::unordered_map<U32, std::vector<U32>> members;
        for (U32 s = 0; s < total; ++s)
        {
            const U32 r = mRegions.find(s);
            const F64 a = mTiles[mSeeds[s].mTile].mRooms.mStats[mSeeds[s].mLocal].mCells * cell_area;
            area[r] += a;
            flying[r] += mSeeds[s].mFlight ? a : 0.0;
            members[r].push_back(s);
        }
        std::unordered_map<U32, std::map<U32, U32>> nb;
        for (const Edge& e : mEdges)
        {
            const U32 a = mRegions.find(e.mA), b = mRegions.find(e.mB);
            if (a != b)
            {
                ++nb[a][b];
                ++nb[b][a];
            }
        }
        typedef std::pair<F64, U32> Item;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        for (const auto& [r, a] : area)
        {
            if (a < min_area)
            {
                heap.emplace(a, r);
            }
        }
        while (!heap.empty())
        {
            const Item top = heap.top();
            heap.pop();
            const U32 r = top.second;
            if (mRegions.find(r) != r || top.first != area[r] || top.first >= min_area)
            {
                continue;
            }
            auto& mine = nb[r];
            U32 best = NONE, best_w = 0, held = NONE, held_w = 0;
            for (const auto& [o, w] : mine)
            {
                if (keptApart(mRegions, r, o))
                {
                    if (w > held_w)
                    {
                        held = o;
                        held_w = w;
                    }
                }
                else if (w > best_w)
                {
                    best = o;
                    best_w = w;
                }
            }
            if (best == NONE && held != NONE)
            {
                // A side too small to be a space of its own: the cut it was held by goes.
                releaseApart(mRegions, r, held);
                best = held;
            }
            if (best == NONE)
            {
                continue;
            }
            mRegions.unite(r, best);
            const U32 root = mRegions.find(r), gone = root == r ? best : r;
            for (const auto& [o, w] : nb[gone])
            {
                if (o == root)
                {
                    continue;
                }
                nb[root][o] += w;
                nb[o][root] += w;
                nb[o].erase(gone);
            }
            nb[root].erase(gone);
            nb.erase(gone);
            area[root] += area[gone];
            flying[root] += flying[gone];
            std::vector<U32>& kept = members[root];
            kept.insert(kept.end(), members[gone].begin(), members[gone].end());
            members.erase(gone);
            const bool flight = flying[root] * 2.0 > area[root];
            for (U32 s : kept)
            {
                mSeeds[s].mFlight = flight;
            }
            if (area[root] < min_area)
            {
                heap.emplace(area[root], root);
            }
        }
    }

    void SpacesPass::State::merge()
    {
        SK_NAV_ZONE("nav.spaces.merge");
        edges();
        mEdgeKey.resize(mEdges.size());
        for (size_t i = 0; i < mEdges.size(); ++i)
        {
            const Edge& e = mEdges[i];
            MemoKey& k = mEdgeKey[i];
            k = MemoKey();
            k.feed(e.mCellA);
            k.feed(e.mCellB);
            k.feed(static_cast<U64>(static_cast<U32>(e.mFloorA)) | (static_cast<U64>(static_cast<U32>(e.mFloorB)) << 32));
            k.feed(static_cast<U64>(e.mFlight));
        }
        // Across a seam between batches, one side's watershed saw the other:
        // where it ran on into it, the two are one basin.
        for (const Edge& e : mEdges)
        {
            const S32 xa = mTiles[e.mTileA].mBatch, xb = mTiles[e.mTileB].mBatch;
            if (xa == xb)
            {
                continue;
            }
            if (xa >= 0)
            {
                const auto it = mShadow[xa].find((static_cast<U64>(e.mTileB) << 32) | e.mGapB);
                if (it != mShadow[xa].end() && it->second == mSeedLabel[e.mA])
                {
                    unite(e.mA, e.mB);
                    continue;
                }
            }
            if (xb >= 0)
            {
                const auto it = mShadow[xb].find((static_cast<U64>(e.mTileA) << 32) | e.mGapA);
                if (it != mShadow[xb].end() && it->second == mSeedLabel[e.mB])
                {
                    unite(e.mA, e.mB);
                }
            }
        }
        // A ring gap the batch's watershed ran into is in that basin, as a full pass would cut it, seam or corner.
        {
            std::unordered_map<S32, U32> by_label;
            for (U32 sd = 0; sd < mSeeds.size(); ++sd)
            {
                if (mSeedLabel[sd] >= 0)
                {
                    by_label.emplace(mSeedLabel[sd], sd);
                }
            }
            for (const auto& shadow : mShadow)
            {
                for (const auto& [where, label] : shadow)
                {
                    const auto it = by_label.find(label);
                    const TileInfo& ti = mTiles[static_cast<U32>(where >> 32)];
                    const U32 gap = static_cast<U32>(where);
                    if (it == by_label.end() || ti.mBatch >= 0 || gap >= ti.mRooms.mSeeds.size() || !ti.mRooms.mSeeds[gap])
                    {
                        continue;
                    }
                    unite(it->second, ti.mSeedBase + ti.mRooms.mSeeds[gap] - 1);
                }
            }
        }
        std::vector<std::unordered_map<U64, S32>>().swap(mShadow);
        std::vector<std::unordered_map<U64, S32>>().swap(mSeamShadow);
        collectApart();
        absorbFragments();
        auto kept_apart = [this](UnionFind& uf, U32 u, U32 v) { return keptApart(uf, u, v); };
        const F32 passage = 2.f * params().mAgentRadiusM;
        const U32 total = static_cast<U32>(mSeeds.size());
        auto makePortal = [this](const Run& r, const Scored& s, PortalKind kind, U32 u, U32 v) {
            Portal q;
            q.mKind = kind;
            q.mSpaces[0] = u;
            q.mSpaces[1] = v;
            const F32 fz = r.mFloor * mUnit;
            q.mCentre.set(r.mC.mV[VX], r.mC.mV[VY], fz);
            q.mNormal.set(r.mN.mV[VX], r.mN.mV[VY], 0.f);
            q.mA.set(r.mA.mV[VX], r.mA.mV[VY], fz);
            q.mB.set(r.mB.mV[VX], r.mB.mV[VY], fz);
            q.mWidth = r.mWidth;
            q.mSill = fz;
            q.mHead = s.mHead;
            q.mCues = s.mCues;
            if (kind == PortalKind::OPENING && s.mAperture)
            {
                // An opening spans its aperture, jamb to jamb, not the boundary the watershed drew: between their true faces, where known.
                LLVector2 ja = s.mJambA, jb = s.mJambB;
                LLVector2 ta, tb;
                if (aperture(r, ta, tb, false, true))
                {
                    ja = ta;
                    jb = tb;
                }
                const LLVector2 c = (ja + jb) * 0.5f;
                LLVector2 n(ja.mV[VY] - jb.mV[VY], jb.mV[VX] - ja.mV[VX]);
                n.normVec();
                if (n * r.mN < 0.f)
                {
                    n = -n;
                }
                q.mCentre.set(c.mV[VX], c.mV[VY], fz);
                q.mNormal.set(n.mV[VX], n.mV[VY], 0.f);
                q.mA.set(ja.mV[VX], ja.mV[VY], fz);
                q.mB.set(jb.mV[VX], jb.mV[VY], fz);
                q.mWidth = dist_vec(ja, jb);
            }
            mPortals.push_back(q);
            mPortalDoor.push_back(kind == PortalKind::DOOR ? s.mDoor : -1);
        };
        std::vector<std::pair<U64, U32>> keyed;
        std::vector<U32> group;
        // Each stair portal's contact, by portal, for squaring to its flight once the rounds settle.
        std::vector<StairEnd> stair_ends;
        std::vector<size_t> stair_portals;
        auto stairEnd = [&](const Run& r, U32 st, U32 first) {
            StairEnd e;
            e.mA = r.mA;
            e.mB = r.mB;
            // Squared about the cut's middle, measured from the flight cell beside it nearest that.
            F32 nearest = FLT_MAX;
            for (U32 id : r.mIds)
            {
                const Edge& ed = mEdges[id];
                const bool a_flight = mRegions.find(ed.mA) == st;
                const F32 d = dist_vec_squared(ed.mMid, r.mC);
                if (d < nearest)
                {
                    nearest = d;
                    e.mSeed = a_flight ? ed.mCellA : ed.mCellB;
                    e.mZ = (a_flight ? ed.mFloorA : ed.mFloorB) * mUnit;
                }
            }
            e.mPivot = r.mC;
            e.mInto = st == first ? -r.mN : r.mN;
            e.mFlight = st;
            stair_ends.push_back(e);
            stair_portals.push_back(mPortals.size() - 1);
        };
        // A pair of regions whose edges a round left alone has the same runs next round.
        struct Memo
        {
            U32 mRu;
            std::vector<U32> mIds;
            std::vector<Run> mRuns;
        };
        std::unordered_map<U64, Memo> memo;
        auto groupRuns = [&](U32 ru) -> const std::vector<Run>& {
            U64 h = mix(SEED, ru);
            for (U32 id : group)
            {
                h = mix(h, id);
            }
            const auto it = memo.find(h);
            if (it != memo.end() && it->second.mRu == ru && it->second.mIds == group)
            {
                return it->second.mRuns;
            }
            Memo& m = memo[h];
            m.mRu = ru;
            m.mIds = group;
            // Runs read their edges and which side of each is ru's, nothing else.
            MemoKey key;
            key.feed(static_cast<U64>(group.size()));
            for (U32 id : group)
            {
                const U64 first = mRegions.find(mEdges[id].mA) == ru ? 1 : 0;
                key.feed(mEdgeKey[id].mA ^ first);
                key.feed(mEdgeKey[id].mB);
            }
            const MergeMemo::Runs* old = nullptr;
            if (mOldMemo)
            {
                const auto hit = mOldMemo->mRuns.find(key.mA);
                old = hit != mOldMemo->mRuns.end() && hit->second.mCheck == key.mB ? &hit->second : nullptr;
            }
            if (old)
            {
                m.mRuns = old->mRuns;
                for (Run& r : m.mRuns)
                {
                    r.mKey = SEED;
                    for (U32& id : r.mIds)
                    {
                        id = group[id];
                        r.mKey = mix(r.mKey, id);
                    }
                }
                ++mProduct->mReused;
            }
            else
            {
                m.mRuns = runs(group, ru);
            }
            MergeMemo::Runs& kept = mMemo->mRuns[key.mA];
            kept.mCheck = key.mB;
            kept.mRuns = m.mRuns;
            for (Run& r : kept.mRuns)
            {
                r.mKey = 0;
                for (U32& id : r.mIds)
                {
                    id = static_cast<U32>(std::lower_bound(group.begin(), group.end(), id) - group.begin());
                }
            }
            return m.mRuns;
        };
        for (S32 round = 0; round < MERGE_ROUNDS; ++round)
        {
            mPortals.clear();
            mPortalDoor.clear();
            stair_ends.clear();
            stair_portals.clear();
            std::vector<bool> flight(total, false);
            for (U32 s = 0; s < total; ++s)
            {
                const U32 r = mRegions.find(s);
                flight[r] = flight[r] || mSeeds[s].mFlight;
            }
            keyed.clear();
            for (U32 i = 0; i < mEdges.size(); ++i)
            {
                const U32 a = mRegions.find(mEdges[i].mA), b = mRegions.find(mEdges[i].mB);
                if (a != b)
                {
                    keyed.emplace_back((static_cast<U64>(std::min(a, b)) << 32) | std::max(a, b), i);
                }
            }
            std::sort(keyed.begin(), keyed.end());
            struct Group
            {
                U32 mU, mV;
                size_t mBegin, mEnd;
            };
            std::vector<Group> groups;
            for (size_t i = 0; i < keyed.size();)
            {
                size_t j = i;
                while (j < keyed.size() && keyed[j].first == keyed[i].first)
                {
                    ++j;
                }
                groups.push_back({ static_cast<U32>(keyed[i].first >> 32), static_cast<U32>(keyed[i].first & 0xffffffff), i, j });
                i = j;
            }
            std::stable_sort(groups.begin(), groups.end(), [](const Group& x, const Group& y) { return x.mEnd - x.mBegin > y.mEnd - y.mBegin; });
            UnionFind round_uf = mRegions;
            U32 merged = 0;
            for (const Group& g : groups)
            {
                group.clear();
                for (size_t i = g.mBegin; i < g.mEnd; ++i)
                {
                    group.push_back(keyed[i].second);
                }
                if (flight[g.mU] != flight[g.mV])
                {
                    // Each contact where a flight meets a floor at an end of its
                    // own, not along its sides: the floor lies at or past the
                    // flight's lowest or highest tread there. Switchbacks,
                    // landings and seams give a flight several.
                    const U32 st = flight[g.mU] ? g.mU : g.mV;
                    const std::vector<Run>& contacts = groupRuns(g.mU);
                    bool made = false;
                    const Run* best = nullptr;
                    for (const Run& r : contacts)
                    {
                        S32 fs_lo = INT_MAX, fs_hi = INT_MIN;
                        std::vector<S32> floor_side;
                        for (U32 id : r.mIds)
                        {
                            const Edge& e = mEdges[id];
                            const bool a_flight = mRegions.find(e.mA) == st;
                            const S32 fs = a_flight ? e.mFloorA : e.mFloorB;
                            fs_lo = std::min(fs_lo, fs);
                            fs_hi = std::max(fs_hi, fs);
                            floor_side.push_back(a_flight ? e.mFloorB : e.mFloorA);
                        }
                        std::nth_element(floor_side.begin(), floor_side.begin() + floor_side.size() / 2, floor_side.end());
                        const S32 ff = floor_side[floor_side.size() / 2];
                        // A staircase is never wider than this: a wider contact runs along a deck or round a corner.
                        if (r.mWidth > STAIR_MAX_WIDTH_M)
                        {
                            continue;
                        }
                        if (r.mWidth >= passage && (!best || r.mEdges > best->mEdges))
                        {
                            best = &r;
                        }
                        if (r.mEdges < STAIR_RUN_EDGES || r.mWidth < passage || (ff > fs_lo + 1 && ff < fs_hi - 1) || (fs_lo == fs_hi && std::abs(ff - fs_lo) > STAIR_END_UNITS))
                        {
                            continue;
                        }
                        Scored s = score(r);
                        s.mCues &= ~CUE_DOOR;
                        makePortal(r, s, PortalKind::STAIR, g.mU, g.mV);
                        stairEnd(r, st, g.mU);
                        made = true;
                    }
                    if (!made && best)
                    {
                        // No clear end: the likeliest contact still links it.
                        Scored s = score(*best);
                        s.mCues = static_cast<U8>((s.mCues & ~CUE_DOOR) | CUE_WEAK);
                        makePortal(*best, s, PortalKind::STAIR, g.mU, g.mV);
                        stairEnd(*best, st, g.mU);
                    }
                    continue;
                }
                // A run narrower than an avatar passes is no way through: no
                // portal, and two regions that meet only along such runs are
                // not one either.
                bool any = false, wide = false;
                for (const Run& r : groupRuns(g.mU))
                {
                    const Scored s = score(r);
                    const bool passes = s.mKind == PortalKind::DOOR || r.mWidth >= passage;
                    wide = wide || passes;
                    if (s.mPortal && passes)
                    {
                        makePortal(r, s, s.mKind, g.mU, g.mV);
                        any = true;
                    }
                }
                const bool held = !any && wide && kept_apart(round_uf, g.mU, g.mV);
                if (held)
                {
                    // Held apart by a cut elsewhere: where else they meet is an opening.
                    for (const Run& r : groupRuns(g.mU))
                    {
                        if (r.mWidth >= passage)
                        {
                            makePortal(r, score(r), PortalKind::OPENING, g.mU, g.mV);
                        }
                    }
                }
                if (!held && !any && wide && round_uf.unite(g.mU, g.mV))
                {
                    ++merged;
                }
            }
            if (!merged)
            {
                // The rounds settled. A staircase pairs its ends within
                // reach and climbs between them: what pairs with nothing,
                // pairs across level ground, or opens wider than any
                // staircase is a deck edge or a rail - the seeds under such
                // a cut stop flying, its flight loses the flag if nothing
                // real is left, and the rounds run again so the floors the
                // cut split join instead of staying split around it.
                if (!demoteFlatFlights(params().mStairMinRiseM))
                {
                    break;
                }
                round = 0;
            }
            mRegions = round_uf;
        }
        squareStairPortals(stair_ends, stair_portals);
        for (const auto& pq : mApart)
        {
            SeedLink l;
            l.mA = mTiles[mSeeds[pq.first].mTile].mKey;
            l.mSeedA = static_cast<U16>(mSeeds[pq.first].mLocal + 1);
            l.mB = mTiles[mSeeds[pq.second].mTile].mKey;
            l.mSeedB = static_cast<U16>(mSeeds[pq.second].mLocal + 1);
            mProduct->mApart.push_back(l);
        }
        // A flight that touches no floor is no stair: drops, low headroom or
        // steps past climb part it from every floor, so no contact can link it.
        std::unordered_set<U32> linked;
        for (const Portal& q : mPortals)
        {
            if (q.mKind == PortalKind::STAIR)
            {
                linked.insert(q.mSpaces[0]);
                linked.insert(q.mSpaces[1]);
            }
        }
        for (U32 s = 0; s < total; ++s)
        {
            if (mSeeds[s].mFlight && !linked.count(mRegions.find(s)))
            {
                mSeeds[s].mFlight = false;
            }
        }

        // Doors whose leaf splits the air on its own, with no cut on them.
        std::unordered_set<S32> matched(mPortalDoor.begin(), mPortalDoor.end());        for (size_t i = 0; i < mDoors.size(); ++i)
        {
            if (matched.count(static_cast<S32>(i)))
            {
                continue;
            }
            const DoorSpan& d = mDoors[i];
            const S32 kf = static_cast<S32>(std::lround(d.mLo / mUnit));
            Probe pa, pb;
            const LLVector2 qa = d.mC + d.mN * 0.6f, qb = d.mC - d.mN * 0.6f;
            if (!probe(qa.mV[VX], qa.mV[VY], kf, mClimb + 2, pa) || !probe(qb.mV[VX], qb.mV[VY], kf, mClimb + 2, pb))
            {
                continue;
            }
            const U16 sa = mTiles[pa.mTile].mRooms.mSeeds[pa.mGap], sb = mTiles[pb.mTile].mRooms.mSeeds[pb.mGap];
            if (!sa || !sb)
            {
                continue;
            }
            Run r;
            r.mC = d.mC;
            r.mA = d.mA;
            r.mB = d.mB;
            r.mD = d.mD;
            r.mN = -d.mN;   // from the first side into the second
            r.mWidth = d.mWidth;
            r.mFloor = pa.mFloor;
            Scored s;
            s.mCues = CUE_DOOR | CUE_SYNTHETIC;
            s.mDoor = static_cast<S32>(i);
            makePortal(r, s, PortalKind::DOOR, mRegions.find(mTiles[pa.mTile].mSeedBase + sa - 1), mRegions.find(mTiles[pb.mTile].mSeedBase + sb - 1));
        }
        // A door portal spans its door, once per door and pair of regions.
        std::set<std::tuple<S32, U32, U32>> seen;
        std::vector<Portal> kept;
        std::vector<S32> kept_door;
        for (size_t i = 0; i < mPortals.size(); ++i)
        {
            Portal q = mPortals[i];
            const S32 door = mPortalDoor[i];
            if (q.mKind == PortalKind::DOOR && door >= 0)
            {
                const auto key = std::make_tuple(door, std::min(q.mSpaces[0], q.mSpaces[1]), std::max(q.mSpaces[0], q.mSpaces[1]));
                if (!seen.insert(key).second)
                {
                    continue;
                }
                const DoorSpan& d = mDoors[door];
                q.mA.set(d.mA.mV[VX], d.mA.mV[VY], q.mSill);
                q.mB.set(d.mB.mV[VX], d.mB.mV[VY], q.mSill);
                q.mCentre.set(d.mC.mV[VX], d.mC.mV[VY], q.mSill);
                const F32 sgn = LLVector2(q.mNormal.mV[VX], q.mNormal.mV[VY]) * d.mN >= 0.f ? 1.f : -1.f;
                q.mNormal.set(d.mN.mV[VX] * sgn, d.mN.mV[VY] * sgn, 0.f);
                q.mWidth = d.mWidth;
                q.mDoor = d.mId;
            }
            kept.push_back(q);
            kept_door.push_back(door);
        }
        mPortals.swap(kept);
        mPortalDoor.swap(kept_door);
        std::unordered_map<U64, Scored>().swap(mScored);
    }

    void SpacesPass::State::joinUnderFlights(std::vector<Space>& out)
    {
        SK_NAV_ZONE("nav.spaces.joinunder");
        // A space's own avatar floor, not air it took over above a thin solid.
        auto ownFloor = [&](const Space& sp, U32 t, const AirGap& g) {
            const F32 z = floorOf(t, g) * mUnit;
            return floorGap(t, g) && z >= sp.mFloorMin - mClimb * mUnit && z <= sp.mFloorMax + mClimb * mUnit;
        };
        std::vector<U32> degree(out.size(), 0);
        for (const Portal& q : mPortals)
        {
            ++degree[q.mSpaces[0]];
            ++degree[q.mSpaces[1]];
        }
        // Each space's floor, and how much of it lies under an open flight's air.
        std::vector<F64> total(out.size(), 0.0), under(out.size(), 0.0);
        AirColumn col;
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            for (const AirLeaf& leaf : a.mLeaves)
            {
                if (!leaf.mGapCount)
                {
                    continue;
                }
                const F64 area = std::pow((1 << leaf.mLevel) * mCell, 2.0);
                column(t, leaf.mX, leaf.mY, col);
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 g = leaf.mFirstGap + i;
                    const U32 s = mGapSpace[t][g];
                    if (!s || !ownFloor(out[s - 1], t, a.mGaps[g]))
                    {
                        continue;
                    }
                    total[s - 1] += area;
                    const S32 f = floorOf(t, a.mGaps[g]);
                    for (U32 k = 0; k < col.mCount; ++k)
                    {
                        const AirGap& o = a.mGaps[col.mGap[k]];
                        if ((o.mFlags & AirGap::FLIGHT) && floorOf(t, o) > f + mWalkHead / 2 && ceilOf(t, o) >= OPEN_TOP)
                        {
                            under[s - 1] += area;
                            break;
                        }
                    }
                }
            }
        }
        std::vector<bool> lone(out.size(), false);
        for (U32 s = 0; s < out.size(); ++s)
        {
            Space& sp = out[s];
            lone[s] = !degree[s] && !sp.mLift && !sp.mFlight && total[s] > 0.0 && under[s] >= UNDER_FLIGHT_SHARE * total[s];
            sp.mUnderFlight = lone[s];
        }
        // From each lone space's floors, walk out across floor too low to stand on until ground of another space, or solid.
        struct Contact
        {
            LLVector2 mFrom, mTo;
            F32 mZ;
        };
        std::map<std::pair<U32, U32>, std::vector<Contact>> contacts;
        const S32 reach = static_cast<S32>(std::lround(UNDER_FLIGHT_REACH_M / mCell));
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const S32 sz = 1 << leaf.mLevel;
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 g = leaf.mFirstGap + i;
                    const U32 s = mGapSpace[t][g];
                    if (!s || !lone[s - 1] || !ownFloor(out[s - 1], t, a.mGaps[g]))
                    {
                        continue;
                    }
                    for (S32 d = 0; d < 4; ++d)
                    {
                        for (S32 e = 0; e < sz; ++e)
                        {
                            S32 x = DX[d] > 0 ? leaf.mX + sz - 1 : DX[d] < 0 ? leaf.mX : leaf.mX + e;
                            S32 y = DY[d] > 0 ? leaf.mY + sz - 1 : DY[d] < 0 ? leaf.mY : leaf.mY + e;
                            const LLVector2 from(a.mOrigin.mV[VX] + (x + 0.5f) * mCell, a.mOrigin.mV[VY] + (y + 0.5f) * mCell);
                            const S32 f = airFloorAt(a.mGaps[g], leaf, x, y) + mTiles[t].mZOff;
                            for (S32 k = 1; k <= reach; ++k)
                            {
                                x += DX[d];
                                y += DY[d];
                                U32 u;
                                S32 bx, by;
                                if (resolve(t, x, y, u, bx, by) != 0)
                                {
                                    break;
                                }
                                S32 hit = -1, hf = 0;
                                const U32 n = column(u, bx, by, col);
                                for (U32 j = 0; j < n; ++j)
                                {
                                    const S32 fo = floorAt(u, col, j, bx, by);
                                    if (std::abs(fo - f) <= mClimb && ceilOf(u, air(u).mGaps[col.mGap[j]]) > f + mClimb)
                                    {
                                        hit = static_cast<S32>(j);
                                        hf = fo;
                                        break;
                                    }
                                }
                                if (hit < 0)
                                {
                                    break;
                                }
                                const U32 og = col.mGap[hit];
                                const U32 o = mGapSpace[u][og];
                                if (!o || !floorGap(u, air(u).mGaps[og]))
                                {
                                    continue;
                                }
                                if (o != s)
                                {
                                    if (!out[o - 1].mFlight && !out[o - 1].mLift)
                                    {
                                        const TileAir& b = air(u);
                                        const LLVector2 to(b.mOrigin.mV[VX] + (bx + 0.5f) * mCell, b.mOrigin.mV[VY] + (by + 0.5f) * mCell);
                                        contacts[{ s - 1, o - 1 }].push_back({ from, to, 0.5f * (f + hf) * mUnit });
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Each lone space opens onto the ground it meets along the most cells.
        std::vector<std::pair<U32, const std::vector<Contact>*>> best(out.size(), { 0, nullptr });
        for (const auto& [key, list] : contacts)
        {
            auto& b = best[key.first];
            if (!b.second || list.size() > b.second->size())
            {
                b = { key.second, &list };
            }
        }
        for (U32 s = 0; s < out.size(); ++s)
        {
            const std::vector<Contact>* list = best[s].second;
            if (!list)
            {
                continue;
            }
            LLVector2 n(0.f, 0.f), mid(0.f, 0.f);
            F32 z = 0.f;
            for (const Contact& c : *list)
            {
                n += c.mTo - c.mFrom;
                mid += (c.mFrom + c.mTo) * 0.5f;
                z += c.mZ;
            }
            mid /= static_cast<F32>(list->size());
            z /= static_cast<F32>(list->size());
            n.normVec();
            const LLVector2 along(-n.mV[VY], n.mV[VX]);
            F32 lo = 1e30f, hi = -1e30f;
            for (const Contact& c : *list)
            {
                const F32 v = (((c.mFrom + c.mTo) * 0.5f) - mid) * along;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            // Narrower than an avatar passes is no way out.
            if (hi - lo + mCell < 2.f * params().mAgentRadiusM)
            {
                continue;
            }
            Portal q;
            q.mKind = PortalKind::OPENING;
            q.mSpaces[0] = s;
            q.mSpaces[1] = best[s].first;
            const LLVector2 pa = mid + along * (lo - 0.5f * mCell), pb = mid + along * (hi + 0.5f * mCell);
            q.mA.set(pa.mV[VX], pa.mV[VY], z);
            q.mB.set(pb.mV[VX], pb.mV[VY], z);
            q.mCentre.set(mid.mV[VX], mid.mV[VY], z);
            q.mNormal.set(n.mV[VX], n.mV[VY], 0.f);
            q.mWidth = hi - lo + mCell;
            q.mSill = z;
            mPortals.push_back(q);
        }
    }

    void SpacesPass::State::spaces()
    {
        SK_NAV_ZONE("nav.spaces.spaces");
        const SpaceParams& p = params();
        const U32 total = static_cast<U32>(mSeeds.size());
        const F32 cell_area = mCell * mCell;
        auto stats = [this](U32 s) -> const SeedStats& { return mTiles[mSeeds[s].mTile].mRooms.mStats[mSeeds[s].mLocal]; };

        // Regions, numbered compactly.
        std::vector<U32> region(total, NONE);
        std::vector<U32> roots;
        std::unordered_map<U32, U32> index;
        for (U32 s = 0; s < total; ++s)
        {
            const U32 r = mRegions.find(s);
            auto it = index.find(r);
            if (it == index.end())
            {
                it = index.emplace(r, static_cast<U32>(roots.size())).first;
                roots.push_back(r);
            }
            region[s] = it->second;
        }
        const U32 nreg = static_cast<U32>(roots.size());
        std::vector<F64> area(nreg, 0.0);
        std::vector<bool> flight(nreg, false);
        for (U32 s = 0; s < total; ++s)
        {
            area[region[s]] += stats(s).mCells * cell_area;
            flight[region[s]] = flight[region[s]] || mSeeds[s].mFlight;
        }
        for (Portal& q : mPortals)
        {
            q.mSpaces[0] = index.count(q.mSpaces[0]) ? index[q.mSpaces[0]] : NONE;
            q.mSpaces[1] = index.count(q.mSpaces[1]) ? index[q.mSpaces[1]] : NONE;
        }

        // Slivers join the neighbour they share the longest unportalled
        // boundary with, smallest first; never across a flight's edge.
        std::set<std::pair<U32, U32>> portalled;
        for (const Portal& q : mPortals)
        {
            if (q.mSpaces[0] != NONE && q.mSpaces[1] != NONE)
            {
                portalled.emplace(std::min(q.mSpaces[0], q.mSpaces[1]), std::max(q.mSpaces[0], q.mSpaces[1]));
            }
        }
        std::vector<std::unordered_map<U32, U32>> nb(nreg);
        for (const Edge& e : mEdges)
        {
            const U32 a = region[e.mA], b = region[e.mB];
            if (a == b || portalled.count(std::make_pair(std::min(a, b), std::max(a, b))) || flight[a] != flight[b])
            {
                continue;
            }
            ++nb[a][b];
            ++nb[b][a];
        }
        UnionFind sliver;
        sliver.reset(nreg);
        std::vector<bool> alive(nreg, true);
        typedef std::pair<F64, U32> Item;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        for (U32 r = 0; r < nreg; ++r)
        {
            heap.emplace(area[r], r);
        }
        while (!heap.empty())
        {
            const Item top = heap.top();
            heap.pop();
            const U32 r = top.second;
            if (!alive[r] || top.first != area[r] || flight[r] || nb[r].empty())
            {
                continue;
            }
            U32 best = NONE, best_w = 0;
            for (const auto& o : nb[r])
            {
                if (o.second > best_w || (o.second == best_w && o.first < best))
                {
                    best = o.first;
                    best_w = o.second;
                }
            }
            if (area[r] >= std::min<F64>(p.mSliverM2, p.mSliverShare * area[best]))
            {
                continue;
            }
            for (const auto& o : nb[r])
            {
                if (o.first == best)
                {
                    continue;
                }
                nb[best][o.first] += o.second;
                nb[o.first][best] += o.second;
                nb[o.first].erase(r);
            }
            nb[best].erase(r);
            nb[r].clear();
            sliver.mParent[r] = best;
            alive[r] = false;
            area[best] += area[r];
            heap.emplace(area[best], best);
        }
        // Clutter: under a square metre with nothing beside it.
        std::vector<bool> clutter(nreg, false);
        for (U32 r = 0; r < nreg; ++r)
        {
            clutter[r] = alive[r] && area[r] < 1.0 && nb[r].empty() && !flight[r];
        }
        for (const Portal& q : mPortals)
        {
            for (U32 s : q.mSpaces)
            {
                if (s != NONE)
                {
                    clutter[sliver.find(s)] = false;
                }
            }
        }

        // The outdoor rule: open-sky neighbours join across a portal unless
        // it is a door or a stair, or an opening with walls on a side, or
        // either hides a good share of its sky overhead.
        std::vector<F64> s_sum(nreg, 0.0), c_sum(nreg, 0.0), weight(nreg, 0.0), walled(nreg, 0.0), bound(nreg, 0.0);
        for (U32 s = 0; s < total; ++s)
        {
            const U32 r = sliver.find(region[s]);
            s_sum[r] += mSeeds[s].mS;
            c_sum[r] += mSeeds[s].mCover;
            weight[r] += mSeeds[s].mWeight;
            walled[r] += stats(s).mWalled;
            bound[r] += stats(s).mBoundary;
        }
        UnionFind outdoor = sliver;
        for (const Portal& q : mPortals)
        {
            if (q.mSpaces[0] == NONE || q.mSpaces[1] == NONE)
            {
                continue;
            }
            const U32 u = sliver.find(q.mSpaces[0]), v = sliver.find(q.mSpaces[1]);
            if (u == v || q.mKind != PortalKind::OPENING)
            {
                continue;
            }
            const bool open_u = weight[u] > 0.0 && s_sum[u] / weight[u] > p.mOutdoorS && c_sum[u] / weight[u] < p.mOutdoorCover;
            const bool open_v = weight[v] > 0.0 && s_sum[v] / weight[v] > p.mOutdoorS && c_sum[v] / weight[v] < p.mOutdoorCover;
            const bool walled_u = bound[u] > 0.0 && walled[u] / bound[u] >= p.mWalledL;
            const bool walled_v = bound[v] > 0.0 && walled[v] / bound[v] >= p.mWalledL;
            if (open_u && open_v && !walled_u && !walled_v)
            {
                outdoor.unite(u, v);
            }
        }

        // Spaces, in order of their first seed.
        std::unordered_map<U32, U32> space_of;
        mSeedSpace.assign(total, 0);
        std::vector<Space>& out = mProduct->mSpaces;
        for (U32 s = 0; s < total; ++s)
        {
            const U32 r = sliver.find(region[s]);
            if (clutter[r])
            {
                continue;
            }
            const U32 root = outdoor.find(r);
            auto it = space_of.find(root);
            if (it == space_of.end())
            {
                it = space_of.emplace(root, static_cast<U32>(out.size())).first;
                out.emplace_back();
            }
            mSeedSpace[s] = it->second + 1;
        }
        const U32 nsp = static_cast<U32>(out.size());
        struct Acc
        {
            F64 mCells = 0.0, mBoundary = 0.0, mWalled = 0.0, mFlightCells = 0.0;
            F64 mSumX = 0.0, mSumY = 0.0, mSumXX = 0.0, mSumXY = 0.0, mSumYY = 0.0;
            S32 mFloorMin = INT_MAX, mFloorMax = INT_MIN;
            F64 mWeight = 0.0, mS = 0.0, mCover = 0.0, mSideOpen = 0.0, mE = 0.0, mKnown = 0.0, mVolume = 0.0, mCeilSum = 0.0, mCeilArea = 0.0, mOpen = 0.0;
            F32 mEMin = 1.f, mEMax = 0.f, mCeilMin = 1e30f, mCeilMax = -1e30f;
        };
        std::vector<Acc> acc(nsp);
        for (U32 s = 0; s < total; ++s)
        {
            if (!mSeedSpace[s])
            {
                continue;
            }
            Acc& a = acc[mSeedSpace[s] - 1];
            const SeedStats& st = stats(s);
            const SeedInfo& si = mSeeds[s];
            a.mCells += st.mCells;
            a.mBoundary += st.mBoundary;
            a.mWalled += st.mWalled;
            a.mFlightCells += si.mFlight ? st.mFlightCells : 0;
            a.mSumX += st.mSumX;
            a.mSumY += st.mSumY;
            a.mSumXX += st.mSumXX;
            a.mSumXY += st.mSumXY;
            a.mSumYY += st.mSumYY;
            if (st.mCells)
            {
                a.mFloorMin = std::min(a.mFloorMin, st.mFloorMin);
                a.mFloorMax = std::max(a.mFloorMax, st.mFloorMax);
            }
            a.mWeight += si.mWeight;
            a.mS += si.mS;
            a.mCover += si.mCover;
            a.mSideOpen += si.mSideOpen;
            a.mE += si.mE;
            a.mKnown += si.mKnown;
            a.mEMin = std::min(a.mEMin, si.mEMin);
            a.mEMax = std::max(a.mEMax, si.mEMax);
            a.mVolume += si.mVolume;
            a.mCeilSum += si.mCeilSum;
            a.mCeilArea += si.mCeilArea;
            a.mCeilMin = std::min(a.mCeilMin, si.mCeilMin);
            a.mCeilMax = std::max(a.mCeilMax, si.mCeilMax);
            a.mOpen += si.mOpenArea;
            std::vector<std::pair<U64, U32>>& raster = out[mSeedSpace[s] - 1].mRaster;
            raster.insert(raster.end(), st.mRaster.begin(), st.mRaster.end());
        }
        // Each space's seed rasters summed at once: merging them one by one grows with the space's size.
        for (Space& sp : out)
        {
            sumRaster(sp.mRaster);
        }
        for (U32 i = 0; i < nsp; ++i)
        {
            const Acc& a = acc[i];
            Space& sp = out[i];
            const F64 cells = std::max(1.0, a.mCells);
            const F64 mx = a.mSumX / cells, my = a.mSumY / cells;
            F64 l1 = 0.0, l2 = 0.0;
            const LLVector2 axis = principal(a.mSumXX / cells - mx * mx, a.mSumXY / cells - mx * my, a.mSumYY / cells - my * my, l1, l2);
            const F32 floor_lo = a.mFloorMin == INT_MAX ? 0.f : a.mFloorMin * mUnit;
            const F32 floor_hi = a.mFloorMax == INT_MIN ? 0.f : a.mFloorMax * mUnit;
            sp.mFloorArea = static_cast<F32>(a.mCells * cell_area);
            sp.mFloorMin = floor_lo;
            sp.mFloorMax = floor_hi;
            sp.mCentre.set(static_cast<F32>(mx), static_cast<F32>(my), floor_lo);
            sp.mAxis = axis;
            sp.mHalf.set(static_cast<F32>(std::sqrt(3.0 * l1)) + 0.5f * mCell, static_cast<F32>(std::sqrt(3.0 * l2)) + 0.5f * mCell);
            sp.mElongation = static_cast<F32>(std::sqrt(l1 / std::max(l2, 1e-9)));
            sp.mVolume = static_cast<F32>(a.mVolume);
            if (a.mCeilArea > 0.0)
            {
                sp.mCeilingMin = a.mCeilMin;
                sp.mCeilingMax = a.mCeilMax;
                sp.mCeilingMean = static_cast<F32>(a.mCeilSum / a.mCeilArea);
            }
            sp.mOpenShare = a.mWeight > 0.0 ? static_cast<F32>(a.mOpen / a.mWeight) : 0.f;
            const F32 mean_height = sp.mFloorArea > 0.f ? sp.mVolume / std::max(static_cast<F32>(a.mWeight), 1e-3f) : 0.f;
            sp.mSurface = 2.f * sp.mFloorArea + static_cast<F32>(a.mBoundary) * mCell * mean_height;
            sp.mMeanFreePath = sp.mSurface > 0.f ? 4.f * sp.mVolume / sp.mSurface : 0.f;
            sp.mSMean = a.mWeight > 0.0 ? static_cast<F32>(a.mS / a.mWeight) : 0.f;
            sp.mCoverMean = a.mWeight > 0.0 ? static_cast<F32>(a.mCover / a.mWeight) : 0.f;
            sp.mSideOpenMean = a.mWeight > 0.0 ? static_cast<F32>(a.mSideOpen / a.mWeight) : 0.f;
            sp.mEMean = a.mKnown > 0.0 ? static_cast<F32>(a.mE / a.mKnown) : 0.f;
            sp.mEMin = a.mKnown > 0.0 ? a.mEMin : 0.f;
            sp.mEMax = a.mKnown > 0.0 ? a.mEMax : 0.f;
            sp.mUnknown = a.mWeight > 0.0 ? static_cast<F32>(1.0 - a.mKnown / a.mWeight) : 1.f;
            sp.mL = a.mBoundary > 0.0 ? static_cast<F32>(a.mWalled / a.mBoundary) : 0.f;
            sp.mFlight = a.mFlightCells * 2.0 > a.mCells;
            sp.mBucket = a.mKnown > 0.0 ? EnclosureBucket::SHELTERED : EnclosureBucket::UNKNOWN;
            sp.mAnchor.set(static_cast<F32>(std::floor(mx)), static_cast<F32>(std::floor(my)), std::floor(floor_lo));
        }
        // AABBs from the rasters.
        for (Space& sp : out)
        {
            sp.mMin.set(1e30f, 1e30f, sp.mFloorMin);
            sp.mMax.set(-1e30f, -1e30f, std::max(sp.mCeilingMax, sp.mFloorMax + p.mWalkHeadM));
            for (const auto& r : sp.mRaster)
            {
                const F32 x = (static_cast<S64>((r.first >> 42) & 0x1fffff) - (1ll << 20)) * RASTER_XY_M;
                const F32 y = (static_cast<S64>((r.first >> 21) & 0x1fffff) - (1ll << 20)) * RASTER_XY_M;
                sp.mMin.mV[VX] = std::min(sp.mMin.mV[VX], x);
                sp.mMin.mV[VY] = std::min(sp.mMin.mV[VY], y);
                sp.mMax.mV[VX] = std::max(sp.mMax.mV[VX], x + RASTER_XY_M);
                sp.mMax.mV[VY] = std::max(sp.mMax.mV[VY], y + RASTER_XY_M);
            }
        }

        // Gaps take their seed's space.
        mGapSpace.assign(mTiles.size(), std::vector<U32>());
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            std::vector<U32>& gs = mGapSpace[t];
            gs.assign(ti.mAir->mGaps.size(), 0);
            for (size_t g = 0; g < gs.size(); ++g)
            {
                const U16 seed = ti.mRooms.mSeeds[g];
                gs[g] = seed ? mSeedSpace[ti.mSeedBase + seed - 1] : 0;
            }
        }

        // Portals between two spaces; a region's space is any of its seeds'.
        std::vector<U32> region_space(nreg, 0);
        for (U32 s = 0; s < total; ++s)
        {
            region_space[region[s]] = mSeedSpace[s];
        }
        std::vector<Portal> portals;
        portals.swap(mPortals);
        for (Portal q : portals)
        {
            if (q.mSpaces[0] == NONE || q.mSpaces[1] == NONE)
            {
                continue;
            }
            const U32 a = region_space[q.mSpaces[0]], b = region_space[q.mSpaces[1]];
            if (!a || !b || a == b)
            {
                continue;
            }
            q.mSpaces[0] = a - 1;
            q.mSpaces[1] = b - 1;
            mPortals.push_back(q);
        }

        // Lifts: a census shaft, or a tall shaft behind a door, is a space of its own.
        std::vector<std::vector<std::pair<U32, U32>>> lifts;
        std::vector<std::pair<LLVector3d, LLVector3d>> shafts;
        const F64 leaf_min = 1.0;
        for (const auto& box : mInput->mLifts)
        {
            LLVector3d lo = box.first - mInput->mMeshOrigin, hi = box.second - mInput->mMeshOrigin;
            // A stack of doors stands in the shaft's wall: look either side of it for tall air only.
            const bool stack = std::min(hi.mdV[VX] - lo.mdV[VX], hi.mdV[VY] - lo.mdV[VY]) < LIFT_STACK_M;
            if (stack)
            {
                for (S32 k : { VX, VY })
                {
                    if (hi.mdV[k] - lo.mdV[k] < LIFT_STACK_M)
                    {
                        lo.mdV[k] -= LIFT_REACH_M;
                        hi.mdV[k] += LIFT_REACH_M;
                    }
                }
            }
            std::vector<std::pair<U32, U32>> gaps;
            F64 got = 0.0;
            for (U32 t = 0; t < mTiles.size(); ++t)
            {
                const TileAir& a = air(t);
                for (const AirLeaf& leaf : a.mLeaves)
                {
                    const F32 half = (1 << leaf.mLevel) * 0.5f * mCell;
                    const F32 x = a.mOrigin.mV[VX] + leaf.mX * mCell + half, y = a.mOrigin.mV[VY] + leaf.mY * mCell + half;
                    if (x < lo.mdV[VX] || x > hi.mdV[VX] || y < lo.mdV[VY] || y > hi.mdV[VY])
                    {
                        continue;
                    }
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const U32 g = leaf.mFirstGap + i;
                        const F32 z = floorOf(t, a.mGaps[g]) * mUnit;
                        const S32 c_top = ceilOf(t, a.mGaps[g]);
                        const bool tall = c_top < OPEN_TOP && (c_top - floorOf(t, a.mGaps[g])) * mUnit >= LIFT_SHAFT_M;
                        if (mGapSpace[t][g] && z >= lo.mdV[VZ] - 1.0 && z <= hi.mdV[VZ] + 1.0 && (!stack || tall))
                        {
                            gaps.emplace_back(t, g);
                            got += 4.0 * half * half;
                        }
                    }
                }
            }
            if (got >= leaf_min)
            {
                lifts.push_back(std::move(gaps));
                shafts.emplace_back(lo, hi);
            }
        }
        for (const DoorSpan& d : mDoors)
        {
            const S32 kf = static_cast<S32>(std::lround(d.mLo / mUnit));
            for (F32 sgn : { 1.f, -1.f })
            {
                const LLVector2 at = d.mC + d.mN * (0.6f * sgn);
                Probe pr;
                if (!probe(at.mV[VX], at.mV[VY], kf, mClimb + 2, pr) || !mGapSpace[pr.mTile][pr.mGap])
                {
                    continue;
                }
                // A shaft is roofed: open sky behind a door is the outdoors.
                if (pr.mCeil >= OPEN_TOP || (pr.mCeil - pr.mFloor) * mUnit < LIFT_TALL_M)
                {
                    continue;
                }
                const U32 home = mGapSpace[pr.mTile][pr.mGap];
                std::vector<std::pair<U32, U32>> gaps;
                F64 got = 0.0, around = 0.0, sky = 0.0;
                F32 tallest = 0.f;
                const F32 reach = LIFT_REACH_M + 2.f;
                for (U32 t = 0; t < mTiles.size(); ++t)
                {
                    const TileAir& a = air(t);
                    if (a.mOrigin.mV[VX] > d.mC.mV[VX] + reach || a.mOrigin.mV[VX] + a.mWidth * mCell < d.mC.mV[VX] - reach ||
                        a.mOrigin.mV[VY] > d.mC.mV[VY] + reach || a.mOrigin.mV[VY] + a.mHeight * mCell < d.mC.mV[VY] - reach)
                    {
                        continue;
                    }
                    for (const AirLeaf& leaf : a.mLeaves)
                    {
                        const F32 half = (1 << leaf.mLevel) * 0.5f * mCell;
                        const LLVector2 c(a.mOrigin.mV[VX] + leaf.mX * mCell + half, a.mOrigin.mV[VY] + leaf.mY * mCell + half);
                        const F32 dc = dist_vec(c, d.mC);
                        if (dc > reach || ((c - d.mC) * d.mN) * sgn <= 0.05f)
                        {
                            continue;
                        }
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            const U32 g = leaf.mFirstGap + i;
                            const AirGap& gap = a.mGaps[g];
                            const S32 f = floorOf(t, gap), c_top = ceilOf(t, gap);
                            const bool tall = c_top < OPEN_TOP && (c_top - f) * mUnit >= LIFT_TALL_M;
                            if (tall && mGapSpace[t][g] == home && std::abs(f - pr.mFloor) <= mClimb)
                            {
                                around += 4.0 * half * half;
                                if (dc <= LIFT_REACH_M)
                                {
                                    const GapEnclosure& ge = mEnc->mTiles[mTiles[t].mEnc].mGaps[g];
                                    gaps.emplace_back(t, g);
                                    got += 4.0 * half * half;
                                    sky += 4.0 * half * half * ge.mS / 255.0;
                                    tallest = std::max(tallest, (c_top - f) * mUnit);
                                }
                            }
                        }
                    }
                }
                // Small, running up through storeys, mostly blind to the sky, and nothing more of it just beyond: a shaft, not a hall or a doorstep.
                if (got < 1.0 || around > 16.0 || tallest < LIFT_SHAFT_M || sky >= OPEN_MIN_S * got)
                {
                    continue;
                }
                // A door in a census shaft's stack of doors opens onto that shaft.
                size_t into = shafts.size();
                F64 nearest = 1.0;
                for (size_t k = 0; k < shafts.size(); ++k)
                {
                    const LLVector3d& lo = shafts[k].first;
                    const LLVector3d& hi = shafts[k].second;
                    const F64 dx = std::max({ lo.mdV[VX] - d.mC.mV[VX], d.mC.mV[VX] - hi.mdV[VX], 0.0 });
                    const F64 dy = std::max({ lo.mdV[VY] - d.mC.mV[VY], d.mC.mV[VY] - hi.mdV[VY], 0.0 });
                    if (std::hypot(dx, dy) <= nearest)
                    {
                        nearest = std::hypot(dx, dy);
                        into = k;
                    }
                }
                if (into < shafts.size())
                {
                    lifts[into].insert(lifts[into].end(), gaps.begin(), gaps.end());
                }
                else
                {
                    lifts.push_back(std::move(gaps));
                }
            }
        }
        // Each gap's leaf.
        std::vector<std::vector<U32>> leaf_of(mTiles.size());
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            leaf_of[t].resize(a.mGaps.size());
            for (U32 l = 0; l < a.mLeaves.size(); ++l)
            {
                for (U32 i = 0; i < a.mLeaves[l].mGapCount; ++i)
                {
                    leaf_of[t][a.mLeaves[l].mFirstGap + i] = l;
                }
            }
        }
        // A new space of these gaps: its floor, volume, ceilings and raster; old gets the spaces they leave.
        const auto takeGaps = [&](U32 id, const std::vector<std::pair<U32, U32>>& gaps, std::set<U32>& old) {
            Space& sp = out[id];
            F64 w = 0.0, sx = 0.0, sy = 0.0, s = 0.0, e = 0.0, known = 0.0, vol = 0.0, cv = 0.0, ceil_sum = 0.0, ceil_area = 0.0;
            F32 lo = 1e30f, hi = -1e30f, ceil_lo = 1e30f, ceil_hi = -1e30f;
            std::map<U64, U32> raster;
            std::map<U32, F64> taken;
            for (const auto& tg : gaps)
            {
                const TileAir& a = air(tg.first);
                const S32 leaf = [&]() {
                    const U32 l = leaf_of[tg.first][tg.second];
                    if (l < a.mLeaves.size() && tg.second >= a.mLeaves[l].mFirstGap && tg.second < a.mLeaves[l].mFirstGap + a.mLeaves[l].mGapCount)
                    {
                        return static_cast<S32>(l);
                    }
                    return -1;
                }();
                if (leaf < 0)
                {
                    continue;
                }
                const AirLeaf& l = a.mLeaves[leaf];
                const F32 half = (1 << l.mLevel) * 0.5f * mCell;
                const F64 area = 4.0 * half * half;
                const F32 x = a.mOrigin.mV[VX] + l.mX * mCell + half, y = a.mOrigin.mV[VY] + l.mY * mCell + half;
                const AirGap& gap = a.mGaps[tg.second];
                const GapEnclosure& ge = mEnc->mTiles[mTiles[tg.first].mEnc].mGaps[tg.second];
                const F32 z = floorOf(tg.first, gap) * mUnit;
                old.insert(mGapSpace[tg.first][tg.second] - 1);
                taken[mGapSpace[tg.first][tg.second] - 1] += area;
                mGapSpace[tg.first][tg.second] = id + 1;
                w += area;
                sx += area * x;
                sy += area * y;
                s += area * ge.mS / 255.0;
                cv += area * ge.mCover / 255.0;
                if (ge.mD != ENCLOSURE_D_UNKNOWN)
                {
                    e += area * ge.mE / 255.0;
                    known += area;
                }
                const S32 c_top = ceilOf(tg.first, gap);
                vol += area * (c_top >= OPEN_TOP ? OPEN_HEIGHT_M : (c_top - floorOf(tg.first, gap)) * mUnit);
                if (c_top < OPEN_TOP)
                {
                    ceil_sum += area * c_top * mUnit;
                    ceil_area += area;
                    ceil_lo = std::min(ceil_lo, c_top * mUnit);
                    ceil_hi = std::max(ceil_hi, c_top * mUnit);
                }
                lo = std::min(lo, z);
                hi = std::max(hi, z);
                raster[rasterKey(x, y, z)] += static_cast<U32>(std::lround(area / cell_area));
            }
            sp.mFloorArea = static_cast<F32>(w);
            sp.mVolume = static_cast<F32>(vol);
            sp.mFloorMin = lo;
            sp.mFloorMax = hi;
            sp.mCentre.set(static_cast<F32>(sx / std::max(w, 1e-9)), static_cast<F32>(sy / std::max(w, 1e-9)), lo);
            sp.mSMean = static_cast<F32>(s / std::max(w, 1e-9));
            sp.mCoverMean = static_cast<F32>(cv / std::max(w, 1e-9));
            if (ceil_area > 0.0)
            {
                sp.mCeilingMin = ceil_lo;
                sp.mCeilingMax = ceil_hi;
                sp.mCeilingMean = static_cast<F32>(ceil_sum / ceil_area);
            }
            sp.mEMean = known > 0.0 ? static_cast<F32>(e / known) : 0.f;
            sp.mUnknown = static_cast<F32>(1.0 - known / std::max(w, 1e-9));
            sp.mAnchor.set(std::floor(sp.mCentre.mV[VX]), std::floor(sp.mCentre.mV[VY]), std::floor(lo));
            sp.mRaster.assign(raster.begin(), raster.end());
            sp.mMin.set(1e30f, 1e30f, lo);
            sp.mMax.set(-1e30f, -1e30f, hi + LIFT_TALL_M);
            for (const auto& r : sp.mRaster)
            {
                const F32 x = (static_cast<S64>((r.first >> 42) & 0x1fffff) - (1ll << 20)) * RASTER_XY_M;
                const F32 y = (static_cast<S64>((r.first >> 21) & 0x1fffff) - (1ll << 20)) * RASTER_XY_M;
                sp.mMin.mV[VX] = std::min(sp.mMin.mV[VX], x);
                sp.mMin.mV[VY] = std::min(sp.mMin.mV[VY], y);
                sp.mMax.mV[VX] = std::max(sp.mMax.mV[VX], x + RASTER_XY_M);
                sp.mMax.mV[VY] = std::max(sp.mMax.mV[VY], y + RASTER_XY_M);
            }
            // Each space it took from loses the floor it took there, not the whole of it.
            for (const auto& o : taken)
            {
                if (o.first < id && !out[o.first].mLift)
                {
                    out[o.first].mFloorArea = std::max(0.f, out[o.first].mFloorArea - static_cast<F32>(o.second));
                }
            }
        };
        for (auto& gaps : lifts)
        {
            std::sort(gaps.begin(), gaps.end());
            gaps.erase(std::unique(gaps.begin(), gaps.end()), gaps.end());
            // A shaft two doors found is one lift.
            gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [&](const std::pair<U32, U32>& tg) {
                           const U32 s = mGapSpace[tg.first][tg.second];
                           return !s || out[s - 1].mLift;
                       }),
                       gaps.end());
            if (gaps.empty())
            {
                continue;
            }
            const U32 id = static_cast<U32>(out.size());
            out.emplace_back();
            Space& sp = out.back();
            sp.mLift = true;
            sp.mBucket = EnclosureBucket::INDOORS;
            sp.mL = 1.f;
            std::set<U32> old;
            takeGaps(id, gaps, old);
            // The doors into it now open onto it.
            const LLVector2 centre(sp.mCentre.mV[VX], sp.mCentre.mV[VY]);
            for (Portal& q : mPortals)
            {
                if (q.mKind != PortalKind::DOOR || dist_vec(LLVector2(q.mCentre.mV[VX], q.mCentre.mV[VY]), centre) > LIFT_REACH_M)
                {
                    continue;
                }
                const U32 side = old.count(q.mSpaces[0]) ? 0 : (old.count(q.mSpaces[1]) ? 1 : 2);
                if (side < 2 && q.mSpaces[1 - side] != id)
                {
                    q.mSpaces[side] = id;
                }
            }
        }

        // Air over no avatar floor, furniture tops, seats and roofs, joins the
        // space it steps onto; never the one below across a solid, which no
        // one walks to from there.
        // Calls fn(tile, gap) for every floor gap a step from gap g of tile t.
        auto around = [&](U32 t, U32 g, const std::function<void(U32, U32)>& fn) {
            const TileAir& a = air(t);
            const AirLeaf& leaf = a.mLeaves[leaf_of[t][g]];
            const S32 c = ceilOf(t, a.mGaps[g]);
            const S32 sz = 1 << leaf.mLevel;
            for (S32 d = 0; d < 4; ++d)
            {
                for (S32 k = 0; k < sz; ++k)
                {
                    const S32 x = DX[d] > 0 ? leaf.mX + sz : DX[d] < 0 ? leaf.mX - 1 : leaf.mX + k;
                    const S32 y = DY[d] > 0 ? leaf.mY + sz : DY[d] < 0 ? leaf.mY - 1 : leaf.mY + k;
                    const S32 f = airFloorAt(a.mGaps[g], leaf, x - DX[d], y - DY[d]) + mTiles[t].mZOff;
                    U32 u;
                    S32 bx, by;
                    if (resolve(t, x, y, u, bx, by) != 0)
                    {
                        continue;
                    }
                    for (S32 lb = mTiles[u].mLeafMap[bx + by * air(u).mWidth]; lb >= 0; lb = parentLeaf(air(u), lb))
                    {
                        // A leaf over this gap's own cells holds air beside it in the same columns, not a step away.
                        const AirLeaf& ol = air(u).mLeaves[lb];
                        if (u == t && leaf.mX >= ol.mX && leaf.mY >= ol.mY && leaf.mX < ol.mX + (1 << ol.mLevel) && leaf.mY < ol.mY + (1 << ol.mLevel))
                        {
                            break;
                        }
                        for (U32 j = 0; j < ol.mGapCount; ++j)
                        {
                            const AirGap& o = air(u).mGaps[ol.mFirstGap + j];
                            const S32 fo = airFloorAt(o, ol, bx, by) + mTiles[u].mZOff;
                            if (floorGap(u, o) && std::abs(fo - f) <= mClimb && std::min(c, ceilOf(u, o)) - std::max(f, fo) >= mWalkOverlap)
                            {
                                fn(u, ol.mFirstGap + j);
                            }
                        }
                    }
                }
            }
        };
        // Floors with no space take the space of a floor they step onto, spreading out a ring at a time.
        auto spread = [&]() {
            std::vector<std::pair<U32, U32>> ring, next;
            for (U32 t = 0; t < mTiles.size(); ++t)
            {
                const TileAir& a = air(t);
                for (U32 g = 0; g < a.mGaps.size(); ++g)
                {
                    if (!mGapSpace[t][g] && floorGap(t, a.mGaps[g]))
                    {
                        ring.emplace_back(t, g);
                    }
                }
            }
            while (!ring.empty())
            {
                std::vector<std::pair<std::pair<U32, U32>, U32>> took;
                for (const auto& tg : ring)
                {
                    if (mGapSpace[tg.first][tg.second])
                    {
                        continue;
                    }
                    U32 best = 0;
                    around(tg.first, tg.second, [&](U32 u, U32 o) {
                        const U32 so = mGapSpace[u][o];
                        if (so && (!best || so < best))
                        {
                            best = so;
                        }
                    });
                    if (best)
                    {
                        took.emplace_back(tg, best);
                    }
                }
                next.clear();
                for (const auto& tk : took)
                {
                    mGapSpace[tk.first.first][tk.first.second] = tk.second;
                }
                for (const auto& tk : took)
                {
                    around(tk.first.first, tk.first.second, [&](U32 u, U32 o) {
                        if (!mGapSpace[u][o])
                        {
                            next.emplace_back(u, o);
                        }
                    });
                }
                std::sort(next.begin(), next.end());
                next.erase(std::unique(next.begin(), next.end()), next.end());
                ring.swap(next);
            }
        };
        spread();

        // Roofs: floors open to the sky over a building's roofed floors. A gap's
        // cells each look down their column for the first gap in a space; the
        // air's indoor or sheltered bucket there makes the cell cover that space.
        const F64 cell_m2 = static_cast<F64>(mCell) * mCell;
        AirColumn below;
        const auto coverOf = [&](U32 t, U32 g, F64& open, F64& covered, std::map<U32, F64>& covers) {
            const TileAir& a = air(t);
            if (ceilOf(t, a.mGaps[g]) < OPEN_TOP)
            {
                return;
            }
            const AirLeaf& leaf = a.mLeaves[leaf_of[t][g]];
            const S32 n = 1 << leaf.mLevel;
            const TileEnclosure& te = mEnc->mTiles[mTiles[t].mEnc];
            for (S32 iy = leaf.mY; iy < leaf.mY + n && iy < a.mHeight; ++iy)
            {
                for (S32 ix = leaf.mX; ix < leaf.mX + n && ix < a.mWidth; ++ix)
                {
                    open += cell_m2;
                    column(t, ix, iy, below);
                    U32 k = 0;
                    while (k < below.mCount && below.mGap[k] != g)
                    {
                        ++k;
                    }
                    while (k-- > 0 && k < below.mCount)
                    {
                        const U32 s = mGapSpace[t][below.mGap[k]];
                        if (s)
                        {
                            if (const EnclosureBucket bk = static_cast<EnclosureBucket>(te.mGaps[below.mGap[k]].mBucket); bk == EnclosureBucket::INDOORS || bk == EnclosureBucket::SHELTERED)
                            {
                                covered += cell_m2;
                                covers[s - 1] += cell_m2;
                            }
                            break;
                        }
                    }
                }
            }
        };
        const auto keepCover = [&](Space& sp, F64 covered, const std::map<U32, F64>& covers) {
            sp.mOverRoofed = sp.mFloorArea > 0.f ? static_cast<F32>(covered / sp.mFloorArea) : 0.f;
            sp.mCovers.clear();
            sp.mCoverM2.clear();
            for (const auto& c : covers)
            {
                sp.mCovers.push_back(c.first);
                sp.mCoverM2.push_back(static_cast<F32>(c.second));
            }
        };
        {
            // The spaces standing now, any of which may be a roof once its bucket is known.
            const size_t standing = out.size();
            std::vector<F64> open(standing, 0.0), covered(standing, 0.0);
            std::vector<std::map<U32, F64>> covers(standing);
            for (U32 t = 0; t < mTiles.size(); ++t)
            {
                const TileAir& a = air(t);
                for (U32 g = 0; g < a.mGaps.size(); ++g)
                {
                    const U32 s = mGapSpace[t][g];
                    if (s && s <= standing && floorGap(t, a.mGaps[g]))
                    {
                        coverOf(t, g, open[s - 1], covered[s - 1], covers[s - 1]);
                    }
                }
            }
            for (size_t s = 0; s < standing; ++s)
            {
                covers[s].erase(static_cast<U32>(s));
                keepCover(out[s], covered[s], covers[s]);
            }

            // Floors no space steps onto: each joined set of them is a roof when open and over indoor floors enough.
            std::vector<std::vector<U8>> seen(mTiles.size());
            for (U32 t = 0; t < mTiles.size(); ++t)
            {
                seen[t].assign(air(t).mGaps.size(), 0);
            }
            for (U32 t0 = 0; t0 < mTiles.size(); ++t0)
            {
                for (U32 g0 = 0; g0 < air(t0).mGaps.size(); ++g0)
                {
                    if (seen[t0][g0] || mGapSpace[t0][g0] || !floorGap(t0, air(t0).mGaps[g0]))
                    {
                        continue;
                    }
                    std::vector<std::pair<U32, U32>> gaps(1, { t0, g0 });
                    seen[t0][g0] = 1;
                    for (size_t head = 0; head < gaps.size(); ++head)
                    {
                        around(gaps[head].first, gaps[head].second, [&](U32 u, U32 o) {
                            if (!seen[u][o] && !mGapSpace[u][o])
                            {
                                seen[u][o] = 1;
                                gaps.emplace_back(u, o);
                            }
                        });
                    }
                    F64 area = 0.0, open_m2 = 0.0, covered_m2 = 0.0;
                    std::map<U32, F64> over;
                    for (const auto& tg : gaps)
                    {
                        const AirLeaf& leaf = air(tg.first).mLeaves[leaf_of[tg.first][tg.second]];
                        area += cell_m2 * (1 << leaf.mLevel) * (1 << leaf.mLevel);
                        coverOf(tg.first, tg.second, open_m2, covered_m2, over);
                    }
                    if (area < ROOF_MIN_M2 || open_m2 < ROOF_OPEN_SHARE * area || covered_m2 < ROOF_COVER_SHARE * area)
                    {
                        continue;
                    }
                    std::sort(gaps.begin(), gaps.end());
                    const U32 id = static_cast<U32>(out.size());
                    out.emplace_back();
                    out.back().mRoof = true;
                    out.back().mBucket = EnclosureBucket::OUTDOORS;
                    std::set<U32> old;
                    takeGaps(id, gaps, old);
                    keepCover(out.back(), covered_m2, over);
                }
            }
        }

        // Bounds over every gap a space holds, now that all have joined.
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const F32 x0 = a.mOrigin.mV[VX] + leaf.mX * mCell, y0 = a.mOrigin.mV[VY] + leaf.mY * mCell;
                const F32 side = (1 << leaf.mLevel) * mCell;
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 s = mGapSpace[t][leaf.mFirstGap + i];
                    if (!s)
                    {
                        continue;
                    }
                    Space& sp = out[s - 1];
                    const AirGap& g = a.mGaps[leaf.mFirstGap + i];
                    const S32 n = (1 << leaf.mLevel) - 1;
                    S32 f_lo = g.mFloor, f_hi = g.mFloor;
                    for (S32 k = 0; k < 4; ++k)
                    {
                        const S32 f = airFloorAt(g, leaf, leaf.mX + (k & 1) * n, leaf.mY + (k >> 1) * n);
                        f_lo = std::min(f_lo, f);
                        f_hi = std::max(f_hi, f);
                    }
                    const F32 z_lo = (f_lo + mTiles[t].mZOff) * mUnit, z_hi = (f_hi + mTiles[t].mZOff) * mUnit;
                    sp.mMin.set(std::min(sp.mMin.mV[VX], x0), std::min(sp.mMin.mV[VY], y0), std::min(sp.mMin.mV[VZ], z_lo));
                    sp.mMax.set(std::max(sp.mMax.mV[VX], x0 + side), std::max(sp.mMax.mV[VY], y0 + side), std::max(sp.mMax.mV[VZ], z_hi));
                }
            }
        }
        joinUnderFlights(out);
        for (U32 i = 0; i < mPortals.size(); ++i)
        {
            for (U32 s : mPortals[i].mSpaces)
            {
                std::vector<U32>& list = out[s].mPortals;
                if (list.empty() || list.back() != i)
                {
                    list.push_back(i);
                }
            }
        }
        mProduct->mPortals = mPortals;
    }

    void SpacesPass::State::ids()
    {
        SK_NAV_ZONE("nav.spaces.ids");
        const SpaceParams& p = params();
        std::vector<Space>& out = mProduct->mSpaces;
        const RoomsProductPtr& prev = mInput->mPrevious;
        std::unordered_set<U32> taken;
        std::vector<bool> named(out.size(), false);
        if (prev)
        {
            // Area-weighted overlap on the floor raster: an id stays with the
            // space it covers half of, and on a split with the larger part.
            // The old rasters in one open-addressed table, each key's entries
            // chained in space order: a map of vectors costs an allocation per key.
            struct OldEntry
            {
                U32 mSpace;
                U32 mCount;
                U32 mNext;
            };
            std::vector<OldEntry> entries;
            size_t n_old = 0;
            for (const Space& sp : prev->mSpaces)
            {
                n_old += sp.mRaster.size();
            }
            size_t cap = 16;
            while (cap < n_old * 2)
            {
                cap <<= 1;
            }
            std::vector<U64> slot_key(cap, 0);
            std::vector<U32> slot_head(cap, NONE);
            auto slotOf = [&](U64 key) {
                size_t h = static_cast<size_t>(spread(key)) & (cap - 1);
                while (slot_head[h] != NONE && slot_key[h] != key)
                {
                    h = (h + 1) & (cap - 1);
                }
                return h;
            };
            entries.reserve(n_old);
            std::vector<F64> old_area(prev->mSpaces.size(), 0.0);
            for (U32 i = 0; i < prev->mSpaces.size(); ++i)
            {
                taken.insert(prev->mSpaces[i].mId);
                for (const auto& r : prev->mSpaces[i].mRaster)
                {
                    old_area[i] += r.second;
                }
            }
            for (U32 i = static_cast<U32>(prev->mSpaces.size()); i-- > 0;)
            {
                for (const auto& r : prev->mSpaces[i].mRaster)
                {
                    const size_t h = slotOf(r.first);
                    slot_key[h] = r.first;
                    entries.push_back({ i, r.second, slot_head[h] });
                    slot_head[h] = static_cast<U32>(entries.size() - 1);
                }
            }
            struct Pair
            {
                U32 mNew, mOld;
                F64 mInter, mIoU;
            };
            std::vector<Pair> pairs;
            std::vector<F64> new_area(out.size(), 0.0);
            for (U32 i = 0; i < out.size(); ++i)
            {
                std::unordered_map<U32, F64> inter;
                for (const auto& r : out[i].mRaster)
                {
                    new_area[i] += r.second;
                    for (U32 e = slot_head[slotOf(r.first)]; e != NONE; e = entries[e].mNext)
                    {
                        inter[entries[e].mSpace] += std::min(r.second, entries[e].mCount);
                    }
                }
                for (const auto& o : inter)
                {
                    pairs.push_back({ i, o.first, o.second, 0.0 });
                }
            }
            for (Pair& q : pairs)
            {
                q.mIoU = q.mInter / std::max(1e-9, new_area[q.mNew] + old_area[q.mOld] - q.mInter);
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.mIoU > b.mIoU || (a.mIoU == b.mIoU && a.mInter > b.mInter); });
            std::vector<bool> used_old(prev->mSpaces.size(), false);
            for (const Pair& q : pairs)
            {
                if (q.mIoU < 0.5)
                {
                    break;
                }
                if (named[q.mNew] || used_old[q.mOld])
                {
                    continue;
                }
                named[q.mNew] = true;
                used_old[q.mOld] = true;
                out[q.mNew].mId = prev->mSpaces[q.mOld].mId;
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.mInter > b.mInter; });
            for (const Pair& q : pairs)
            {
                if (named[q.mNew] || used_old[q.mOld] || q.mInter < 0.5 * new_area[q.mNew] || q.mInter < 0.25 * old_area[q.mOld])
                {
                    continue;
                }
                named[q.mNew] = true;
                used_old[q.mOld] = true;
                out[q.mNew].mId = prev->mSpaces[q.mOld].mId;
            }
        }
        std::unordered_set<U32> used;
        for (U32 i = 0; i < out.size(); ++i)
        {
            if (named[i])
            {
                used.insert(out[i].mId);
            }
        }
        for (U32 i = 0; i < out.size(); ++i)
        {
            if (named[i])
            {
                continue;
            }
            U64 h = mix(mix(SEED, mInput->mMeshKey), static_cast<U64>(static_cast<S64>(out[i].mAnchor.mV[VX])));
            h = mix(mix(h, static_cast<U64>(static_cast<S64>(out[i].mAnchor.mV[VY]))), static_cast<U64>(static_cast<S64>(out[i].mAnchor.mV[VZ])));
            U32 id = static_cast<U32>(h ^ (h >> 32));
            while (id == 0 || used.count(id) || taken.count(id))
            {
                ++id;
            }
            out[i].mId = id;
            used.insert(id);
        }

        // Buckets, held against the space's last one; few walls cap one at SHELTERED.
        for (Space& sp : out)
        {
            if (sp.mUnknown >= 1.f)
            {
                sp.mBucket = EnclosureBucket::UNKNOWN;
                continue;
            }
            const Space* was = prev ? prev->byId(sp.mId) : nullptr;
            sp.mBucket = enclosureBucket(sp.mEMean, was ? was->mBucket : EnclosureBucket::UNKNOWN, p.mHysteresis);
            if (sp.mBucket == EnclosureBucket::INDOORS && sp.mL < p.mIndoorL)
            {
                sp.mBucket = EnclosureBucket::SHELTERED;
            }
            // A shaft is indoors whatever its depth reads.
            if (sp.mLift)
            {
                sp.mBucket = EnclosureBucket::INDOORS;
            }
            if (sp.mBucket == EnclosureBucket::INDOORS && sp.mUnderFlight)
            {
                sp.mBucket = EnclosureBucket::SHELTERED;
            }
            // Unroofed air under open sky is never indoors, lift or not.
            if (sp.mBucket == EnclosureBucket::INDOORS && sp.mCoverMean <= OPEN_MAX_COVER && sp.mSMean > OPEN_MIN_S)
            {
                sp.mBucket = EnclosureBucket::SHELTERED;
            }
            // A roof is outdoors; so is an outdoor floor over indoor ones, and that makes it a roof.
            sp.mRoof = sp.mRoof || (sp.mBucket == EnclosureBucket::OUTDOORS && sp.mOverRoofed >= ROOF_COVER_SHARE);
            if (sp.mRoof)
            {
                sp.mBucket = EnclosureBucket::OUTDOORS;
            }
        }

        // Buildings: indoor spaces joined by a portal, and every roofed space a roof covers with the roof.
        {
            UnionFind uf;
            uf.reset(out.size());
            const auto indoor = [&](U32 s) { return s < out.size() && out[s].mBucket == EnclosureBucket::INDOORS; };
            const auto roofed = [&](U32 s) { return indoor(s) || (s < out.size() && out[s].mBucket == EnclosureBucket::SHELTERED); };
            for (const Portal& q : mProduct->mPortals)
            {
                if (indoor(q.mSpaces[0]) && indoor(q.mSpaces[1]))
                {
                    uf.unite(q.mSpaces[0], q.mSpaces[1]);
                }
            }
            std::vector<U8> member(out.size(), 0);
            for (U32 s = 0; s < out.size(); ++s)
            {
                member[s] = indoor(s) ? 1 : 0;
                Space& sp = out[s];
                if (sp.mRoof)
                {
                    // A roof covers the roofed space it lies most over, and those it lies over a quarter of: an eave over a street joins nothing.
                    size_t keep = 0, most = sp.mCovers.size();
                    for (size_t k = 0; k < sp.mCovers.size() && k < sp.mCoverM2.size(); ++k)
                    {
                        if (roofed(sp.mCovers[k]) && (most == sp.mCovers.size() || sp.mCoverM2[k] > sp.mCoverM2[most]))
                        {
                            most = k;
                        }
                    }
                    for (size_t k = 0; k < sp.mCovers.size() && k < sp.mCoverM2.size(); ++k)
                    {
                        const U32 c = sp.mCovers[k];
                        if (roofed(c) && (k == most || sp.mCoverM2[k] >= ROOF_COVERS_SHARE * out[c].mFloorArea))
                        {
                            sp.mCovers[keep] = c;
                            sp.mCoverM2[keep++] = sp.mCoverM2[k];
                            uf.unite(s, c);
                        }
                    }
                    sp.mCovers.resize(keep);
                    sp.mCoverM2.resize(keep);
                }
            }
            for (U32 s = 0; s < out.size(); ++s)
            {
                for (const U32 c : out[s].mRoof ? out[s].mCovers : std::vector<U32>())
                {
                    member[c] = 1;
                }
            }
            std::unordered_map<U32, U32> least;
            for (U32 s = 0; s < out.size(); ++s)
            {
                if (member[s])
                {
                    auto it = least.emplace(uf.find(s), out[s].mId).first;
                    it->second = std::min(it->second, out[s].mId);
                }
            }
            for (U32 s = 0; s < out.size(); ++s)
            {
                const auto it = least.find(uf.find(s));
                out[s].mBuilding = (member[s] || out[s].mRoof) && it != least.end() ? it->second : 0;
            }
        }

        // Tiles whose detail was evicted keep the spaces they held, frozen.
        std::unordered_map<U32, U32> by_id;
        for (U32 i = 0; i < out.size(); ++i)
        {
            by_id[out[i].mId] = i;
        }
        std::unordered_map<U32, U32> remap;
        const size_t live = out.size();
        for (TileEnclosure& te : mEnc->mTiles)
        {
            if (te.mAir || !prev)
            {
                continue;
            }
            for (CoarseGap& g : te.mCoarse.mGaps)
            {
                if (!g.mSpace)
                {
                    continue;
                }
                const U32 old = g.mSpace - 1;
                auto it = remap.find(old);
                if (it == remap.end())
                {
                    U32 to = 0;
                    if (old < prev->mSpaces.size())
                    {
                        const auto hit = by_id.find(prev->mSpaces[old].mId);
                        if (hit != by_id.end())
                        {
                            to = hit->second + 1;
                        }
                        else
                        {
                            Space frozen = prev->mSpaces[old];
                            frozen.mFrozen = true;
                            frozen.mPortals.clear();
                            out.push_back(frozen);
                            by_id[frozen.mId] = static_cast<U32>(out.size() - 1);
                            to = static_cast<U32>(out.size());
                        }
                    }
                    it = remap.emplace(old, to).first;
                }
                g.mSpace = static_cast<U16>(std::min<U32>(it->second, 0xffff));
            }
        }
        // A frozen roof's covers name the last product's spaces: name them in this one, by id.
        for (size_t s = live; s < out.size(); ++s)
        {
            Space& sp = out[s];
            std::vector<std::pair<U32, F32>> covers;
            for (size_t k = 0; k < sp.mCovers.size() && k < sp.mCoverM2.size(); ++k)
            {
                const U32 c = sp.mCovers[k];
                const auto hit = c < prev->mSpaces.size() ? by_id.find(prev->mSpaces[c].mId) : by_id.end();
                if (hit != by_id.end())
                {
                    covers.emplace_back(hit->second, sp.mCoverM2[k]);
                }
            }
            std::sort(covers.begin(), covers.end());
            sp.mCovers.clear();
            sp.mCoverM2.clear();
            for (const auto& c : covers)
            {
                sp.mCovers.push_back(c.first);
                sp.mCoverM2.push_back(c.second);
            }
        }

        // Write the spaces into the enclosure product, fine and coarse.
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            TileEnclosure& te = mEnc->mTiles[ti.mEnc];
            const TileAir& a = *ti.mAir;
            const std::vector<U32>& gs = mGapSpace[t];
            for (size_t g = 0; g < gs.size(); ++g)
            {
                te.mGaps[g].mSpace = static_cast<U16>(std::min<U32>(gs[g], 0xffff));
            }
            CoarseAir& c = te.mCoarse;
            if (c.mWidth <= 0)
            {
                continue;
            }
            std::vector<std::vector<std::pair<U32, F32>>> votes(c.mGaps.size());
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const S32 s = 1 << leaf.mLevel;
                const S32 x1 = std::min(a.mWidth, leaf.mX + s), y1 = std::min(a.mHeight, leaf.mY + s);
                for (S32 cy = leaf.mY / c.mCells; cy * c.mCells < y1; ++cy)
                {
                    for (S32 cx = leaf.mX / c.mCells; cx * c.mCells < x1; ++cx)
                    {
                        const S32 ox = std::min(x1, (cx + 1) * c.mCells) - std::max<S32>(leaf.mX, cx * c.mCells);
                        const S32 oy = std::min(y1, (cy + 1) * c.mCells) - std::max<S32>(leaf.mY, cy * c.mCells);
                        if (ox <= 0 || oy <= 0 || cx >= c.mWidth || cy >= c.mHeight)
                        {
                            continue;
                        }
                        const S32 cell = cx + cy * c.mWidth;
                        const S32 mx = std::max<S32>(leaf.mX, cx * c.mCells) + (ox - 1) / 2, my = std::max<S32>(leaf.mY, cy * c.mCells) + (oy - 1) / 2;
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            const U32 g = leaf.mFirstGap + i;
                            if (!gs[g])
                            {
                                continue;
                            }
                            const U16 f = airFloorUnits(airFloorAt(a.mGaps[g], leaf, mx, my));
                            for (U32 k = c.mStart[cell]; k < c.mStart[cell + 1]; ++k)
                            {
                                if (c.mGaps[k].mFloor <= f && f <= c.mGaps[k].mFloor + mClimb)
                                {
                                    auto& v = votes[k];
                                    auto it = std::find_if(v.begin(), v.end(), [&](const std::pair<U32, F32>& q) { return q.first == gs[g]; });
                                    if (it == v.end())
                                    {
                                        v.emplace_back(gs[g], static_cast<F32>(ox * oy));
                                    }
                                    else
                                    {
                                        it->second += static_cast<F32>(ox * oy);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            for (size_t k = 0; k < c.mGaps.size(); ++k)
            {
                U32 best = 0;
                F32 best_w = 0.f;
                for (const auto& v : votes[k])
                {
                    if (v.second > best_w)
                    {
                        best = v.first;
                        best_w = v.second;
                    }
                }
                c.mGaps[k].mSpace = static_cast<U16>(std::min<U32>(best, 0xffff));
            }
        }
        mProduct->mMemo = mMemo;
        mProduct->mFloors = mFloors ? mFloors : mInput->mPrevious ? mInput->mPrevious->mFloors : nullptr;
        mEnc->mSpaceIds.resize(out.size());
        for (size_t i = 0; i < out.size(); ++i)
        {
            mEnc->mSpaceIds[i] = out[i].mId;
        }
        for (TileInfo& ti : mTiles)
        {
            mProduct->mTiles.push_back(std::move(ti.mRooms));
        }
        std::sort(mProduct->mTiles.begin(), mProduct->mTiles.end(), [](const RoomsTile& a, const RoomsTile& b) { return a.mKey < b.mKey; });
    }

    bool SpacesPass::State::runStage()
    {
        switch (mStage)
        {
            case STAGE_PREPARE:
                prepare();
                break;
            case STAGE_BATCHES:
                if (mBatch < mBatches.size())
                {
                    runBatch(mBatch++);
                }
                if (mBatch < mBatches.size() || recut())
                {
                    return false;
                }
                break;
            case STAGE_SEEDS:
                number();
                break;
            case STAGE_MERGE:
                merge();
                break;
            case STAGE_SPACES:
                spaces();
                break;
            case STAGE_IDS:
                ids();
                break;
            default:
                return true;
        }
        ++mStage;
        return mStage == STAGE_DONE;
    }

    SpacesPass::SpacesPass(std::shared_ptr<const RoomsInput> input, std::shared_ptr<EnclosureProduct> enclosure)
        : mState(std::make_unique<State>())
    {
        mState->mInput = std::move(input);
        mState->mEnc = std::move(enclosure);
    }

    SpacesPass::~SpacesPass() = default;

    bool SpacesPass::step(U32 budget)
    {
        SK_NAV_ZONE("nav.spaces.step");
        State& s = *mState;
        if (s.mStage != STAGE_DONE && budget > 0)
        {
            // A stage, or one block of the cut, is the unit of work.
            const auto t0 = std::chrono::steady_clock::now();
            const S32 stage = s.mStage;
            const bool done = s.runStage();
            s.mStageMs[stage] += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (done)
            {
                F64 total = 0.0;
                for (F64 ms : s.mStageMs)
                {
                    total += ms;
                }
                s.mProduct->mMillis = static_cast<F32>(total);
                // The overlay's draw, traced once with the pass and read by
                // every gather until the next one lands.
                s.mProduct->mDraw = buildSpacesDraw(*s.mEnc, *s.mProduct);
                // What each tile's stamp reads of the spaces, so a stamp is read again only where that moved.
                for (const TileEnclosure& te : s.mEnc->mTiles)
                {
                    s.mProduct->mStampKeys.emplace_back(te.mKey, stampRoomsKey(*s.mEnc, te, *s.mProduct));
                }
                std::sort(s.mProduct->mStampKeys.begin(), s.mProduct->mStampKeys.end(),
                          [](const std::pair<TileKey, U64>& a, const std::pair<TileKey, U64>& b) { return a.first < b.first; });
                // Nothing reads the cells again.
                s.mCells.clear();
                s.mCells.shrink_to_fit();
                s.mTiles.clear();
                s.mSolidMaps = {};
                s.mWindowCount = 0;
            }
        }
        return s.mStage == STAGE_DONE;
    }

    bool SpacesPass::done() const
    {
        return mState->mStage == STAGE_DONE;
    }

    F32 SpacesPass::stageMillis(S32 stage) const
    {
        return stage >= 0 && stage < NUM_STAGES ? static_cast<F32>(mState->mStageMs[stage]) : 0.f;
    }

    const char* SpacesPass::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "prepare", "cut", "seeds", "merge", "spaces", "ids" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    RoomsProductPtr SpacesPass::product() const
    {
        return done() ? mState->mProduct : nullptr;
    }

    RoomsProductPtr computeSpaces(std::shared_ptr<const RoomsInput> input, std::shared_ptr<EnclosureProduct> enclosure)
    {
        SpacesPass pass(std::move(input), std::move(enclosure));
        while (!pass.step(1u << 30))
        {
        }
        return pass.product();
    }

    std::vector<StairLink> stairPairs(const std::vector<Portal>& portals, const std::vector<U32>& indices, F32 reach)
    {
        // By ascending sill, each end joins the nearest still-unpaired end
        // within reach in plan. A foot stays available for further exits: a
        // multi-storey well's foot serves each of them. An end nothing in
        // reach wanted stays single.
        std::vector<U32> order(indices);
        std::sort(order.begin(), order.end(), [&portals](U32 a, U32 b) { return portals[a].mSill < portals[b].mSill; });
        std::vector<U32> waiting;
        std::vector<StairLink> pairs;
        for (const U32 pi : order)
        {
            const Portal& p = portals[pi];
            U32 best = 0;
            F32 best_d2 = reach * reach;
            bool found = false;
            for (const U32 q : waiting)
            {
                const F32 dx = p.mCentre.mV[VX] - portals[q].mCentre.mV[VX];
                const F32 dy = p.mCentre.mV[VY] - portals[q].mCentre.mV[VY];
                const F32 d2 = dx * dx + dy * dy;
                if (d2 <= best_d2)
                {
                    best_d2 = d2;
                    best = q;
                    found = true;
                }
            }
            if (found)
            {
                pairs.push_back({ best, pi });
            }
            else
            {
                waiting.push_back(pi);
            }
        }
        return pairs;
    }

    std::vector<StairLink> stairLinks(const RoomsProduct& product, F32 reach)
    {
        std::vector<StairLink> links;
        for (const Space& sp : product.mSpaces)
        {
            if (!sp.mFlight)
            {
                continue;
            }
            std::vector<U32> stairs;
            for (const U32 pi : sp.mPortals)
            {
                if (product.mPortals[pi].mKind == PortalKind::STAIR)
                {
                    stairs.push_back(pi);
                }
            }
            if (stairs.size() < 2)
            {
                continue;
            }
            for (const StairLink& l : stairPairs(product.mPortals, stairs, reach))
            {
                links.push_back(l);
            }
        }
        return links;
    }

    namespace
    {
        U64 cornerKey(S32 x, S32 y)
        {
            return (static_cast<U64>(static_cast<S64>(x) + (1ll << 20)) << 21) |
                   static_cast<U64>(static_cast<S64>(y) + (1ll << 20));
        }

        S32 cornerX(U64 key) { return static_cast<S32>(static_cast<S64>(key >> 21) - (1ll << 20)); }

        S32 cornerY(U64 key) { return static_cast<S32>(static_cast<S64>(key & 0x1fffff) - (1ll << 20)); }

        /// The four ways an edge runs, by the corner it steps to: +X, +Y, -X, -Y.
        U8 dirOf(U64 from, U64 to)
        {
            const S64 d = static_cast<S64>(to) - static_cast<S64>(from);
            return d == (1ll << 21) ? 0 : d == 1ll ? 1 : d == -(1ll << 21) ? 2 : 3;
        }

        struct TraceEdge
        {
            U64 mFrom = 0;
            U64 mTo = 0;
            F32 mZ = 0.f;
        };
    }

    std::vector<PlanEdge> tracePlan(const std::vector<PlanCell>& cells, F32 cell)
    {
        std::vector<PlanEdge> out;
        if (cells.empty() || cell <= 0.f)
        {
            return out;
        }
        // The plan on its lattice, a column's repeats keeping the lowest floor.
        struct Cell
        {
            S32 mX, mY;
            F32 mZ;
        };
        std::vector<Cell> plan;
        plan.reserve(cells.size());
        for (const PlanCell& c : cells)
        {
            plan.push_back({ static_cast<S32>(std::lround(c.mX / cell)), static_cast<S32>(std::lround(c.mY / cell)), c.mZ });
        }
        std::sort(plan.begin(), plan.end(), [](const Cell& a, const Cell& b)
                  {
                      return a.mY != b.mY ? a.mY < b.mY : (a.mX != b.mX ? a.mX < b.mX : a.mZ < b.mZ);
                  });
        plan.erase(std::unique(plan.begin(), plan.end(), [](const Cell& a, const Cell& b)
                   { return a.mX == b.mX && a.mY == b.mY; }), plan.end());
        const auto on_plan = [&plan](S32 x, S32 y)
        {
            size_t lo = 0, hi = plan.size();
            while (lo < hi)
            {
                const size_t mid = lo + (hi - lo) / 2;
                const Cell& c = plan[mid];
                if (c.mY < y || (c.mY == y && c.mX < x))
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
            return lo < plan.size() && plan[lo].mY == y && plan[lo].mX == x;
        };

        // Every side no cell of the plan stands across, as a directed edge
        // with the plan on its left, so chains close into loops.
        std::vector<TraceEdge> edges;
        edges.reserve(plan.size() * 2);
        for (const Cell& c : plan)
        {
            if (!on_plan(c.mX, c.mY - 1))
            {
                edges.push_back({ cornerKey(c.mX, c.mY), cornerKey(c.mX + 1, c.mY), c.mZ });
            }
            if (!on_plan(c.mX + 1, c.mY))
            {
                edges.push_back({ cornerKey(c.mX + 1, c.mY), cornerKey(c.mX + 1, c.mY + 1), c.mZ });
            }
            if (!on_plan(c.mX, c.mY + 1))
            {
                edges.push_back({ cornerKey(c.mX + 1, c.mY + 1), cornerKey(c.mX, c.mY + 1), c.mZ });
            }
            if (!on_plan(c.mX - 1, c.mY))
            {
                edges.push_back({ cornerKey(c.mX, c.mY + 1), cornerKey(c.mX, c.mY), c.mZ });
            }
        }
        std::sort(edges.begin(), edges.end(), [](const TraceEdge& a, const TraceEdge& b)
                  { return a.mFrom != b.mFrom ? a.mFrom < b.mFrom : a.mTo < b.mTo; });
        std::vector<U8> used(edges.size(), 0);
        const auto next_from = [&](U64 from, U32& next) -> bool
        {
            const auto begin = std::lower_bound(edges.begin(), edges.end(), from,
                                                [](const TraceEdge& e, U64 k) { return e.mFrom < k; });
            for (auto it = begin; it != edges.end() && it->mFrom == from; ++it)
            {
                const U32 i = static_cast<U32>(it - edges.begin());
                if (!used[i])
                {
                    next = i;
                    return true;
                }
            }
            return false;
        };

        std::vector<U32> loop;
        for (size_t s = 0; s < edges.size(); ++s)
        {
            if (used[s])
            {
                continue;
            }
            loop.clear();
            U32 cur = static_cast<U32>(s);
            const U64 start = edges[s].mFrom;
            while (true)
            {
                used[cur] = 1;
                loop.push_back(cur);
                const U64 to = edges[cur].mTo;
                if (to == start)
                {
                    break;
                }
                U32 next;
                if (!next_from(to, next))
                {
                    break;
                }
                cur = next;
            }
            // Start where the direction changes, so no run wraps the ends of
            // the loop, then merge the edges that run on straight at one floor.
            const size_t n = loop.size();
            size_t at = 0;
            for (size_t k = 0; k < n; ++k)
            {
                const TraceEdge& a = edges[loop[(k + n - 1) % n]];
                const TraceEdge& b = edges[loop[k]];
                if (a.mTo != b.mFrom || dirOf(a.mFrom, a.mTo) != dirOf(b.mFrom, b.mTo) || std::fabs(a.mZ - b.mZ) > 1e-4f)
                {
                    at = k;
                    break;
                }
            }
            for (size_t k = 0; k < n;)
            {
                const TraceEdge& e = edges[loop[(at + k) % n]];
                const U8 dir = dirOf(e.mFrom, e.mTo);
                F32 x0 = static_cast<F32>(cornerX(e.mFrom)) * cell, y0 = static_cast<F32>(cornerY(e.mFrom)) * cell;
                F32 x1 = static_cast<F32>(cornerX(e.mTo)) * cell, y1 = static_cast<F32>(cornerY(e.mTo)) * cell;
                const F32 z = e.mZ;
                size_t run = 1;
                while (run < n)
                {
                    const TraceEdge& f = edges[loop[(at + k + run) % n]];
                    if (dirOf(f.mFrom, f.mTo) != dir || std::fabs(f.mZ - z) > 1e-4f)
                    {
                        break;
                    }
                    x1 = static_cast<F32>(cornerX(f.mTo)) * cell;
                    y1 = static_cast<F32>(cornerY(f.mTo)) * cell;
                    ++run;
                }
                out.push_back({ x0, y0, x1, y1, z });
                k += run;
            }
        }
        return out;
    }

    size_t SpacesDraw::bytes() const
    {
        size_t total = sizeof(SpacesDraw) + mFills.capacity() * sizeof(Fill) + mSpaces.capacity() * sizeof(SpaceDraw);
        for (const SpaceDraw& s : mSpaces)
        {
            total += s.mOutline.capacity() * sizeof(PlanEdge);
        }
        return total;
    }

    SpacesDrawPtr buildSpacesDraw(const EnclosureProduct& enclosure, const RoomsProduct& rooms)
    {
        SK_NAV_ZONE("nav.spaces.draw");
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("spaces - build draw");
        auto draw = std::make_shared<SpacesDraw>();
        const std::vector<U32>& ids = enclosure.mSpaceIds;
        draw->mSpaces.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            // A pastel per space id, the same from pass to pass, and an
            // outline lift that differs from a neighbour's.
            const U64 h = mix(SEED, ids[i]);
            SpaceDraw& sd = draw->mSpaces[i];
            sd.mColor = LLColor4(0.35f + 0.6f * ((h & 255) / 255.f), 0.35f + 0.6f * (((h >> 8) & 255) / 255.f),
                                 0.35f + 0.6f * (((h >> 16) & 255) / 255.f), 1.f);
            sd.mLift = 0.09f + 0.01f * static_cast<F32>(h % 3);
        }

        // The space floor of every coarse cell of the mesh: which space,
        // where its floor lies, and the cell's corner. Tiles read past their
        // bounds to be exact at the seams, so the same cell arrives twice
        // and is taken once; a storey's floor, half a metre away or more,
        // is its own.
        struct Flat
        {
            U64 mKey;       ///< the cell on the mesh's global coarse lattice, packed
            U32 mSpace;     ///< the product's space index
            F32 mX, mY;     ///< the cell's min corner, mesh-local
            F32 mZ;         ///< its floor
            F32 mSide;      ///< the coarse cell's side
        };
        std::vector<Flat> flats;
        for (const TileEnclosure& t : enclosure.mTiles)
        {
            const CoarseAir& c = t.mCoarse;
            const F64 side = c.mCells * t.mCell;
            if (side <= 0.0)
            {
                continue;
            }
            const S32 gx0 = static_cast<S32>(std::lround(t.mOrigin.mV[VX] / side));
            const S32 gy0 = static_cast<S32>(std::lround(t.mOrigin.mV[VY] / side));
            for (S32 cy = 0; cy < c.mHeight; ++cy)
            {
                for (S32 cx = 0; cx < c.mWidth; ++cx)
                {
                    const S32 cell = cx + cy * c.mWidth;
                    for (U32 i = c.mStart[cell]; i < c.mStart[cell + 1]; ++i)
                    {
                        const CoarseGap& g = c.mGaps[i];
                        if (!g.mSpace || g.mSpace > ids.size())
                        {
                            continue;
                        }
                        Flat f;
                        f.mKey = (static_cast<U64>(gx0 + cx + (1 << 20)) << 21) | static_cast<U64>(gy0 + cy + (1 << 20));
                        f.mSpace = g.mSpace - 1;
                        f.mX = static_cast<F32>((gx0 + cx) * side);
                        f.mY = static_cast<F32>((gy0 + cy) * side);
                        f.mZ = static_cast<F32>(t.mOrigin.mV[VZ] + g.mFloor * t.mUnit);
                        f.mSide = static_cast<F32>(side);
                        flats.push_back(f);
                    }
                }
            }
        }
        std::sort(flats.begin(), flats.end(), [](const Flat& a, const Flat& b)
                  {
                      return a.mKey != b.mKey ? a.mKey < b.mKey : (a.mSpace != b.mSpace ? a.mSpace < b.mSpace : a.mZ < b.mZ);
                  });
        flats.erase(std::unique(flats.begin(), flats.end(), [](const Flat& a, const Flat& b)
                    { return a.mKey == b.mKey && a.mSpace == b.mSpace && std::fabs(a.mZ - b.mZ) < 0.5f; }), flats.end());

        draw->mFills.reserve(flats.size());
        std::vector<std::vector<PlanCell>> plans(ids.size());
        std::vector<F32> sides(ids.size(), 0.f);
        for (const Flat& f : flats)
        {
            draw->mFills.push_back({ f.mX, f.mY, f.mZ, f.mSide, f.mSpace });
            std::vector<PlanCell>& cells = plans[f.mSpace];
            if (cells.empty() || cells.back().mX != f.mX || cells.back().mY != f.mY)
            {
                // The sort put a column's floors together; the plan wants the
                // column once, at its lowest floor.
                cells.push_back({ f.mX, f.mY, f.mZ });
            }
            if (sides[f.mSpace] <= 0.f)
            {
                sides[f.mSpace] = f.mSide;
            }
        }
        for (size_t i = 0; i < plans.size(); ++i)
        {
            if (!plans[i].empty())
            {
                draw->mSpaces[i].mOutline = tracePlan(plans[i], sides[i]);
            }
        }
        return draw;
    }
}
