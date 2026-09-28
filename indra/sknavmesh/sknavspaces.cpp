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

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
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
    constexpr F32 RASTER_XY_M = 0.5f;
    constexpr F32 RASTER_Z_M = 1.f;
    constexpr U32 STAIR_RUN_EDGES = 3;
    constexpr F32 WALL_END_REACH = 2.5f;    ///< facing wall ends this many agent heights apart may hold a cut
    constexpr F32 WALL_END_RUN_M = 0.5f;    ///< a wall runs this far behind its end, or it is a post
    constexpr S32 STAIR_END_UNITS = 2;      ///< a floor this close to a tread it meets is at an end of the flight

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
    };

    struct Probe
    {
        U32 mTile = 0;
        U32 mGap = 0;
        S32 mFloor = 0;
        S32 mCeil = 0;
    };

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
}

namespace sknav
{
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

    size_t RoomsProduct::bytes() const
    {
        size_t n = sizeof(RoomsProduct) + mPortals.capacity() * sizeof(Portal);
        for (const Space& s : mSpaces)
        {
            n += sizeof(Space) + s.mPortals.capacity() * sizeof(U32) + s.mRaster.capacity() * sizeof(std::pair<U64, U32>);
        }
        for (const RoomsTile& t : mTiles)
        {
            n += sizeof(RoomsTile) + t.mSeeds.capacity() * sizeof(U16);
            for (const SeedStats& s : t.mStats)
            {
                n += sizeof(SeedStats) + s.mRaster.capacity() * sizeof(std::pair<U64, U32>);
            }
        }
        return n;
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

        std::vector<Cell> mCells;
        std::vector<U32> mLink;         ///< 4 per cell
        std::vector<U8> mSide;          ///< 4 per cell
        std::vector<F32> mDist;
        std::vector<LLVector2> mNearest;
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
        S32 ceilOf(U32 t, const AirGap& g) const
        {
            return (g.mFlags & AirGap::OPEN) || g.mCeiling == AIR_CEILING_OPEN ? OPEN_TOP : g.mCeiling + mTiles[t].mZOff;
        }
        bool floorGap(U32 t, const AirGap& g) const { return (g.mFlags & AirGap::WALKABLE) && ceilOf(t, g) - floorOf(t, g) >= mWalkHead; }

        /// The tile and fine cell a (possibly out of range) cell of tile t is,
        /// or -1 past the mesh's tiles and -2 in a tile without detail.
        S32 resolve(U32 t, S32 ix, S32 iy, U32& tile_out, S32& x_out, S32& y_out) const;
        /// The gaps over a fine cell: count 0 when it has none.
        const AirGap* column(U32 t, S32 ix, S32 iy, U32& count) const;
        bool probe(F32 x, F32 y, S32 floor, S32 tol, Probe& out) const;
        /// Whether solid stands between lo and hi (mesh units) over a fine cell.
        bool blocked(U32 t, S32 ix, S32 iy, S32 lo, S32 hi) const;

        void prepare();
        void runBatch(size_t b);
        bool recut();
        S32 mRecuts = 0;
        void buildCells();
        void transform();
        void cutSeeds(size_t b);
        void wallEnds();
        U32 mWallEndCuts = 0;
        void number();
        void unite(U32 a, U32 b);
        void edges();
        void merge();
        std::vector<Run> runs(const std::vector<U32>& edge_ids, U32 ru) const;
        Scored score(const Run& run);
        void spaces();
        void ids();
        bool runStage();
    };

    S32 SpacesPass::State::resolve(U32 t, S32 ix, S32 iy, U32& tile_out, S32& x_out, S32& y_out) const
    {
        const TileAir& a = air(t);
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

    const AirGap* SpacesPass::State::column(U32 t, S32 ix, S32 iy, U32& count) const
    {
        const TileInfo& ti = mTiles[t];
        const S32 leaf = ti.mLeafMap[ix + iy * ti.mAir->mWidth];
        if (leaf < 0)
        {
            count = 0;
            return nullptr;
        }
        const AirLeaf& l = ti.mAir->mLeaves[leaf];
        count = l.mGapCount;
        return &ti.mAir->mGaps[l.mFirstGap];
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
        const S32 leaf = mTiles[t].mLeafMap[ix + iy * a.mWidth];
        if (leaf < 0)
        {
            return false;
        }
        const AirLeaf& l = a.mLeaves[leaf];
        for (U32 i = 0; i < l.mGapCount; ++i)
        {
            const AirGap& g = a.mGaps[l.mFirstGap + i];
            const S32 f = floorOf(t, g);
            if (f >= floor - tol && f <= floor + tol && floorGap(t, g))
            {
                out.mTile = t;
                out.mGap = l.mFirstGap + i;
                out.mFloor = f;
                out.mCeil = ceilOf(t, g);
                return true;
            }
        }
        return false;
    }

    bool SpacesPass::State::blocked(U32 t, S32 ix, S32 iy, S32 lo, S32 hi) const
    {
        U32 tile;
        S32 x, y;
        if (resolve(t, ix, iy, tile, x, y) != 0)
        {
            return false;
        }
        U32 count;
        const AirGap* g = column(tile, x, y, count);
        if (!count)
        {
            return false;   // nothing stands there at all
        }
        for (U32 i = 0; i < count; ++i)
        {
            if (floorOf(tile, g[i]) <= lo && ceilOf(tile, g[i]) >= hi)
            {
                return false;
            }
        }
        return true;
    }

    void SpacesPass::State::prepare()
    {
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
    }

    void SpacesPass::State::runBatch(size_t b)
    {
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
        std::vector<LLVector2>().swap(mNearest);
        std::vector<U8>().swap(mAvatar);
        std::vector<S32>().swap(mLabel);
        for (TileInfo& ti : mTiles)
        {
            std::vector<U32>().swap(ti.mColStart);
        }
    }

    void SpacesPass::State::wallEnds()
    {
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
                        mLink[c * 4 + d] = NONE;
                        mLink[o * 4 + (d ^ 1)] = NONE;
                    }
                }
            }
        }
    }

    bool SpacesPass::State::recut()
    {
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
        mLabelBase = 0;
        mBatch = 0;
        return true;
    }

    void SpacesPass::State::buildCells()
    {
        size_t gaps = 0;
        for (const TileInfo& ti : mTiles)
        {
            for (const AirLeaf& leaf : ti.mWork ? ti.mAir->mLeaves : std::vector<AirLeaf>())
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
            ti.mColStart.assign(static_cast<size_t>(a.mWidth) * a.mHeight + 1, 0);
            for (S32 iy = 0; iy < a.mHeight; ++iy)
            {
                for (S32 ix = 0; ix < a.mWidth; ++ix)
                {
                    const S32 c = ix + iy * a.mWidth;
                    ti.mColStart[c] = static_cast<U32>(mCells.size());
                    const S32 leaf = ti.mLeafMap[c];
                    if (leaf < 0)
                    {
                        continue;
                    }
                    const AirLeaf& l = a.mLeaves[leaf];
                    for (U32 i = 0; i < l.mGapCount; ++i)
                    {
                        const AirGap& g = a.mGaps[l.mFirstGap + i];
                        if (floorGap(t, g))
                        {
                            mCells.push_back({ t, static_cast<U16>(ix), static_cast<U16>(iy), l.mFirstGap + i, floorOf(t, g), ceilOf(t, g),
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
        for (U32 c = 0; c < n; ++c)
        {
            const Cell& cell = mCells[c];
            for (S32 d = 0; d < 4; ++d)
            {
                U32 t;
                S32 x, y;
                const S32 r = resolve(cell.mTile, cell.mX + DX[d], cell.mY + DY[d], t, x, y);
                if (r == -2 || (r == 0 && !mTiles[t].mWork))
                {
                    mSide[c * 4 + d] = SIDE_UNKNOWN;
                    continue;
                }
                if (r != 0)
                {
                    continue;
                }
                const TileInfo& ti = mTiles[t];
                const S32 col = x + y * ti.mAir->mWidth;
                U32 best = NONE;
                S32 best_step = 0;
                for (U32 o = ti.mColStart[col]; o < ti.mColStart[col + 1]; ++o)
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

    void SpacesPass::State::transform()
    {
        // Nearest-wall propagation: each cell carries the point on the
        // nearest side where floor stops, and hands it on while that is nearer.
        const size_t n = mCells.size();
        const F32 cap = params().mClearMaxM;
        mDist.assign(n, std::numeric_limits<F32>::max());
        mNearest.assign(n, LLVector2());
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
                    mNearest[c].set(cellX(ti, cell.mX) + DX[d] * 0.5f * mCell, cellY(ti, cell.mY) + DY[d] * 0.5f * mCell);
                    mDist[c] = 0.5f * mCell;
                    buckets[bucket(mDist[c])].push_back(c);
                    break;
                }
            }
        }
        for (size_t b = 0; b < buckets.size(); ++b)
        {
            for (size_t i = 0; i < buckets[b].size(); ++i)
            {
            const U32 c = buckets[b][i];
            if (bucket(mDist[c]) != b)
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
                const Cell& other = mCells[o];
                const TileInfo& ot = mTiles[other.mTile];
                const F32 dd = dist_vec(LLVector2(cellX(ot, other.mX), cellY(ot, other.mY)), mNearest[c]);
                if (dd < mDist[o] - 1e-6f && dd <= cap)
                {
                    mDist[o] = dd;
                    mNearest[o] = mNearest[c];
                    buckets[std::max(b, bucket(dd))].push_back(o);
                }
            }
            }
            std::vector<U32>().swap(buckets[b]);
        }
        for (F32& d : mDist)
        {
            d = std::min(d, cap);
        }
    }

    void SpacesPass::State::cutSeeds(size_t b)
    {
        const SpaceParams& p = params();
        const size_t n = mCells.size();
        const F32 cell_area = mCell * mCell;

        // Floors an avatar stands on: walkable components not ended mostly by drops.
        std::vector<U32> comp(n, NONE);
        std::vector<F64> comp_area, comp_open, comp_drops;
        std::vector<bool> comp_edge;
        std::vector<U32> stack;
        for (U32 c = 0; c < n; ++c)
        {
            if (comp[c] != NONE)
            {
                continue;
            }
            const U32 id = static_cast<U32>(comp_area.size());
            comp_area.push_back(0.0);
            comp_open.push_back(0.0);
            comp_drops.push_back(0.0);
            comp_edge.push_back(false);
            comp[c] = id;
            stack.assign(1, c);
            while (!stack.empty())
            {
                const U32 k = stack.back();
                stack.pop_back();
                comp_area[id] += cell_area;
                const Cell& cell = mCells[k];
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = mLink[k * 4 + d];
                    if (o != NONE)
                    {
                        if (comp[o] == NONE)
                        {
                            comp[o] = id;
                            stack.push_back(o);
                        }
                        continue;
                    }
                    if (mSide[k * 4 + d] == SIDE_UNKNOWN)
                    {
                        comp_edge[id] = true;
                        continue;
                    }
                    comp_open[id] += 1.0;
                    U32 t;
                    S32 x, y;
                    if (resolve(cell.mTile, cell.mX + DX[d], cell.mY + DY[d], t, x, y) != 0)
                    {
                        continue;   // the mesh's edge ends open ground as a wall or void does
                    }
                    U32 count;
                    const AirGap* g = column(t, x, y, count);
                    for (U32 i = 0; i < count; ++i)
                    {
                        const S32 f = floorOf(t, g[i]);
                        if (f <= cell.mFloor - 1 && ceilOf(t, g[i]) >= cell.mFloor + 14)
                        {
                            comp_drops[id] += f < cell.mFloor - mClimb ? 1.0 : 0.0;
                            break;
                        }
                    }
                }
            }
        }
        mAvatar.assign(n, 0);
        for (U32 c = 0; c < n; ++c)
        {
            const U32 id = comp[c];
            const F64 frac = comp_drops[id] / std::max(1.0, comp_open[id]);
            mAvatar[c] = frac < 0.5 && (comp_area[id] >= p.mAvatarAreaM2 || comp_edge[id]) ? 1 : 0;
        }

        wallEnds();

        // The watershed over the clearance, off flights: descend its levels,
        // grow the regions there are into what each level exposes, then seed
        // new ones from what is left.
        mLabel.assign(n, -1);
        auto ground = [this](U32 c) { return mAvatar[c] && !mCells[c].mFlight; };
        std::vector<std::vector<U32>> levels;
        for (U32 c = 0; c < n; ++c)
        {
            if (!ground(c))
            {
                continue;
            }
            const size_t lv = static_cast<size_t>(mDist[c] / CLEAR_UNIT_M);
            if (levels.size() <= lv)
            {
                levels.resize(lv + 1);
            }
            levels[lv].push_back(c);
        }
        S32 regions = 0;
        std::vector<U32> stamp(n, NONE);
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
                    if (o != NONE && mLabel[o] >= 0 && ground(o))
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
                    if (mLabel[c] >= 0)
                    {
                        continue;
                    }
                    S32 best = -1;
                    F32 best_d = -1.f;
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = mLink[c * 4 + d];
                        if (o != NONE && mLabel[o] >= 0 && ground(o) && mDist[o] > best_d)
                        {
                            best = mLabel[o];
                            best_d = mDist[o];
                        }
                    }
                    if (best >= 0)
                    {
                        assign.emplace_back(c, best);
                    }
                }
                for (const auto& a : assign)
                {
                    mLabel[a.first] = a.second;
                }
                next.clear();
                for (const auto& a : assign)
                {
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = mLink[a.first * 4 + d];
                        if (o != NONE && mLabel[o] < 0 && ground(o) && static_cast<size_t>(mDist[o] / CLEAR_UNIT_M) >= lv &&
                            stamp[o] != static_cast<U32>(step + lv * GROW_STEPS))
                        {
                            stamp[o] = static_cast<U32>(step + lv * GROW_STEPS);
                            next.push_back(o);
                        }
                    }
                }
                frontier.swap(next);
            }
            for (U32 c : fresh)
            {
                if (mLabel[c] >= 0)
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
                        if (o != NONE && mLabel[o] < 0 && ground(o) && static_cast<size_t>(mDist[o] / CLEAR_UNIT_M) >= lv)
                        {
                            mLabel[o] = id;
                            stack.push_back(o);
                        }
                    }
                }
            }
        }
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
                for (const AirLeaf& leaf : a.mLeaves)
                {
                    const S32 s = 1 << leaf.mLevel;
                    if (leaf.mX > 0 && leaf.mY > 0 && leaf.mX + s < a.mWidth && leaf.mY + s < a.mHeight)
                    {
                        continue;
                    }
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const S32 label = majority(ti, leaf, leaf.mFirstGap + i);
                        if (label >= 0)
                        {
                            mShadow[b][(static_cast<U64>(t) << 32) | (leaf.mFirstGap + i)] = mLabelBase + label;
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
            std::vector<std::pair<U32, U64>> raster;
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
                raster.emplace_back(static_cast<U32>(seed), rasterKey(x, y, cell.mFloor * mUnit));
            }
            std::sort(raster.begin(), raster.end());
            for (size_t i = 0; i < raster.size();)
            {
                size_t j = i;
                while (j < raster.size() && raster[j] == raster[i])
                {
                    ++j;
                }
                rt.mStats[raster[i].first - 1].mRaster.emplace_back(raster[i].second, static_cast<U32>(j - i));
                i = j;
            }
        }
        mLabelBase += regions;
    }

    void SpacesPass::State::number()
    {
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
        // Nor does it part seeds an earlier pass joined, while both their tiles stand.
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
                unite(ta.mSeedBase + l.mSeedA - 1, tb.mSeedBase + l.mSeedB - 1);
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
        // Every level-0 step between floor gaps of two seeds, within and across tiles.
        mEdges.clear();
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileInfo& ti = mTiles[t];
            const TileAir& a = *ti.mAir;
            for (const AirLeaf& leaf : a.mLeaves)
            {
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
                        U32 u;
                        S32 bx, by;
                        if (resolve(t, ax + (side == 0), ay + (side == 1), u, bx, by) != 0)
                        {
                            continue;
                        }
                        U32 nb;
                        const AirGap* gb = column(u, bx, by, nb);
                        if (!nb)
                        {
                            continue;
                        }
                        const TileInfo& ui = mTiles[u];
                        const S32 lb = ui.mLeafMap[bx + by * ui.mAir->mWidth];
                        const U32 first_b = ui.mAir->mLeaves[lb].mFirstGap;
                        const LLVector2 ca(cellX(ti, ax), cellY(ti, ay)), cb(cellX(ui, bx), cellY(ui, by));
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            const U32 ga = leaf.mFirstGap + i;
                            const U16 sa = ti.mRooms.mSeeds[ga];
                            if (!sa)
                            {
                                continue;
                            }
                            const AirGap& gap_a = a.mGaps[ga];
                            const S32 fa = floorOf(t, gap_a), ca_top = ceilOf(t, gap_a);
                            for (U32 j = 0; j < nb; ++j)
                            {
                                const U16 sb = ui.mRooms.mSeeds[first_b + j];
                                if (!sb)
                                {
                                    continue;
                                }
                                const U32 seed_a = ti.mSeedBase + sa - 1, seed_b = ui.mSeedBase + sb - 1;
                                if (seed_a == seed_b)
                                {
                                    continue;
                                }
                                const S32 fb = floorOf(u, gb[j]), cb_top = ceilOf(u, gb[j]);
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
                                e.mGapB = first_b + j;
                                e.mFlight = ((gap_a.mFlags | gb[j].mFlags) & AirGap::FLIGHT) != 0;
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

    Scored SpacesPass::State::score(const Run& run)
    {
        const auto hit = mScored.find(run.mKey);
        if (hit != mScored.end())
        {
            return hit->second;
        }
        const SpaceParams& p = params();
        const F32 fz = run.mFloor * mUnit;
        // A portal stood here last time: hold it with thresholds relaxed.
        bool relaxed = false;
        const S32 gx = static_cast<S32>(std::floor(run.mC.mV[VX])), gy = static_cast<S32>(std::floor(run.mC.mV[VY]));
        for (S32 dy = -1; dy <= 1 && !relaxed; ++dy)
        {
            for (S32 dx = -1; dx <= 1 && !relaxed; ++dx)
            {
                const auto it = mOldGrid.find(gridKey(gx + dx, gy + dy));
                if (it == mOldGrid.end())
                {
                    continue;
                }
                for (U32 i : it->second)
                {
                    const LLVector3& q = mOldPortals[i];
                    if (std::fabs(q.mV[VZ] - fz) <= p.mClimbM && dist_vec(LLVector2(q.mV[VX], q.mV[VY]), run.mC) <= 0.5f)
                    {
                        relaxed = true;
                        break;
                    }
                }
            }
        }
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
        bool jambs = true;
        for (S32 k = 0; k < 2 && jambs; ++k)
        {
            const LLVector2 end = k == 0 ? run.mA : run.mB;
            const F32 sgn = k == 0 ? -1.f : 1.f;
            for (F32 t : { 0.3f, 0.8f })
            {
                const LLVector2 at = end + run.mD * (sgn * t);
                jambs = jambs && !probe(at.mV[VX], at.mV[VY], run.mFloor, mClimb, pr);
            }
        }
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
        mScored[run.mKey] = out;
        return out;
    }

    void SpacesPass::State::merge()
    {
        edges();
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
        std::vector<std::unordered_map<U64, S32>>().swap(mShadow);
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
            mPortals.push_back(q);
            mPortalDoor.push_back(kind == PortalKind::DOOR ? s.mDoor : -1);
        };
        std::vector<std::pair<U64, U32>> keyed;
        std::vector<U32> group;
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
            m.mRuns = runs(group, ru);
            return m.mRuns;
        };
        for (S32 round = 0; round < MERGE_ROUNDS; ++round)
        {
            mPortals.clear();
            mPortalDoor.clear();
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
                        if (!best || r.mEdges > best->mEdges)
                        {
                            best = &r;
                        }
                        if (r.mEdges < STAIR_RUN_EDGES || (ff > fs_lo + 1 && ff < fs_hi - 1) || (fs_lo == fs_hi && std::abs(ff - fs_lo) > STAIR_END_UNITS))
                        {
                            continue;
                        }
                        Scored s = score(r);
                        s.mCues &= ~CUE_DOOR;
                        makePortal(r, s, PortalKind::STAIR, g.mU, g.mV);
                        made = true;
                    }
                    if (!made && best)
                    {
                        // No clear end: the likeliest contact still links it.
                        Scored s = score(*best);
                        s.mCues = static_cast<U8>((s.mCues & ~CUE_DOOR) | CUE_WEAK);
                        makePortal(*best, s, PortalKind::STAIR, g.mU, g.mV);
                    }
                    continue;
                }
                bool any = false;
                for (const Run& r : groupRuns(g.mU))
                {
                    const Scored s = score(r);
                    if (s.mPortal)
                    {
                        makePortal(r, s, s.mKind, g.mU, g.mV);
                        any = true;
                    }
                }
                if (!any && round_uf.unite(g.mU, g.mV))
                {
                    ++merged;
                }
            }
            if (!merged)
            {
                break;
            }
            mRegions = round_uf;
        }

        // Doors whose leaf splits the air on its own, with no cut on them.
        std::unordered_set<S32> matched(mPortalDoor.begin(), mPortalDoor.end());
        for (size_t i = 0; i < mDoors.size(); ++i)
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

    void SpacesPass::State::spaces()
    {
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
            F64 mWeight = 0.0, mS = 0.0, mCover = 0.0, mE = 0.0, mKnown = 0.0, mVolume = 0.0, mCeilSum = 0.0, mCeilArea = 0.0, mOpen = 0.0;
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
            a.mFlightCells += st.mFlightCells;
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
        const F64 leaf_min = 1.0;
        for (const auto& box : mInput->mLifts)
        {
            const LLVector3d lo = box.first - mInput->mMeshOrigin, hi = box.second - mInput->mMeshOrigin;
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
                        if (mGapSpace[t][g] && z >= lo.mdV[VZ] - 1.0 && z <= hi.mdV[VZ] + 1.0)
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
                F64 got = 0.0, around = 0.0;
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
                                    gaps.emplace_back(t, g);
                                    got += 4.0 * half * half;
                                }
                            }
                        }
                    }
                }
                // Small, and nothing more of it just beyond: a shaft, not a hall.
                if (got >= 1.0 && around <= 16.0)
                {
                    lifts.push_back(std::move(gaps));
                }
            }
        }
        for (auto& gaps : lifts)
        {
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
            F64 w = 0.0, sx = 0.0, sy = 0.0, s = 0.0, e = 0.0, known = 0.0, vol = 0.0;
            F32 lo = 1e30f, hi = -1e30f;
            std::map<U64, U32> raster;
            std::set<U32> old;
            for (const auto& tg : gaps)
            {
                const TileAir& a = air(tg.first);
                const S32 leaf = [&]() {
                    for (size_t l = 0; l < a.mLeaves.size(); ++l)
                    {
                        if (tg.second >= a.mLeaves[l].mFirstGap && tg.second < a.mLeaves[l].mFirstGap + a.mLeaves[l].mGapCount)
                        {
                            return static_cast<S32>(l);
                        }
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
                mGapSpace[tg.first][tg.second] = id + 1;
                w += area;
                sx += area * x;
                sy += area * y;
                s += area * ge.mS / 255.0;
                if (ge.mD != ENCLOSURE_D_UNKNOWN)
                {
                    e += area * ge.mE / 255.0;
                    known += area;
                }
                const S32 c_top = ceilOf(tg.first, gap);
                vol += area * (c_top >= OPEN_TOP ? OPEN_HEIGHT_M : (c_top - floorOf(tg.first, gap)) * mUnit);
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
            for (U32 o : old)
            {
                if (o < id && !out[o].mLift)
                {
                    out[o].mFloorArea = std::max(0.f, out[o].mFloorArea - sp.mFloorArea);
                }
            }
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

        // Air over no avatar floor joins the space of the floor below it.
        for (U32 t = 0; t < mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            std::vector<U32>& gs = mGapSpace[t];
            for (const AirLeaf& leaf : a.mLeaves)
            {
                U32 below = 0;
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    U32& s = gs[leaf.mFirstGap + i];
                    if (s)
                    {
                        below = s;
                    }
                    else
                    {
                        s = below;
                    }
                }
            }
        }
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
            if (sp.mLift)
            {
                continue;
            }
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
        }

        // Tiles whose detail was evicted keep the spaces they held, frozen.
        std::unordered_map<U32, U32> by_id;
        for (U32 i = 0; i < out.size(); ++i)
        {
            by_id[out[i].mId] = i;
        }
        std::unordered_map<U32, U32> remap;
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
                        for (U32 i = 0; i < leaf.mGapCount; ++i)
                        {
                            const U32 g = leaf.mFirstGap + i;
                            if (!gs[g])
                            {
                                continue;
                            }
                            const U16 f = a.mGaps[g].mFloor;
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
                // Nothing reads the cells again.
                s.mCells.clear();
                s.mCells.shrink_to_fit();
                s.mTiles.clear();
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
}
