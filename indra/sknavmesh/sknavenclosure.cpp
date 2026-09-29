/**
 * @file sknavenclosure.cpp
 * @brief How enclosed each air gap of a mesh is: sky openness, depth from outside and indoorness.
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

#include "sknavenclosure.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <map>
#include <unordered_map>

namespace
{
    using namespace sknav;

    constexpr U32 INF = 0xffffffffu;
    constexpr S32 OPEN_TOP = INT_MAX / 4;    ///< an open ceiling, in global units
    constexpr F32 SKY_COVER_HEADROOM_M = 4.5f;  ///< under a lower ceiling a gap's own roof covers it without asking
    constexpr F32 SKY_COVER_EYE_M = 1.5f;       ///< the sky is judged from this far over the floor
    constexpr F32 SKY_COVER_OPEN_MIN = 0.05f;   ///< under this share of open sky a gap is not seeded
    constexpr F32 TILE_TOP_PAD_M = 3.f;         ///< a tile's air runs this far over its highest solid
    constexpr S32 FLOOR_SLACK = 2;           ///< a read may sit this many units under a floor, which reads high

    enum EdgeFlags : U8
    {
        EDGE_SKY = 1 << 0,      ///< any shared air
        EDGE_CARRY = 1 << 1,    ///< carry overlap, at any floor step
        EDGE_WALK = 1 << 2,     ///< both walkable, floors within climb, walk overlap
    };

    enum NodeFlags : U8
    {
        NODE_WALK = 1 << 0,
        NODE_SKY = 1 << 1,
        NODE_SEALED = 1 << 2,   ///< walkable, but no walkable path from outside reached it
    };

    enum Stage : S32
    {
        STAGE_GRAPH = 0,
        STAGE_SKY,
        STAGE_DEPTH,
        STAGE_CARRY,
        STAGE_ANSWER,
        STAGE_COARSE,
        STAGE_DONE,
    };

    struct GraphEdge
    {
        U32 mA;
        U32 mB;
        U8 mCost;       ///< half cells between leaf centres
        U8 mFlags;
    };

    struct GraphArc
    {
        U32 mTo;
        U8 mCost;
        U8 mFlags;
    };

    U8 quantise(F32 v)
    {
        return static_cast<U8>(std::lround(llclamp(v, 0.f, 1.f) * 255.f));
    }

    S32 ceilingUnits(U16 ceiling, S32 offset)
    {
        return ceiling == AIR_CEILING_OPEN ? OPEN_TOP : ceiling + offset;
    }

    /// Dial's algorithm over the arcs whose flags meet the mask, seeded at any
    /// distance, resumable between steps. Costs are at most 32 half cells.
    class Flood
    {
    public:
        void begin(std::vector<std::pair<U32, U32>>&& seeds)
        {
            // A counting sort by distance: seeds are many and their distances few.
            U32 top = 0;
            for (const auto& s : seeds)
            {
                top = std::max(top, s.first);
            }
            std::vector<U32> at(static_cast<size_t>(top) + 2, 0);
            for (const auto& s : seeds)
            {
                ++at[s.first + 1];
            }
            for (size_t i = 1; i < at.size(); ++i)
            {
                at[i] += at[i - 1];
            }
            mSeeds.resize(seeds.size());
            for (const auto& s : seeds)
            {
                mSeeds[at[s.first]++] = s;
            }
            mSeedAt = 0;
            mCur = 0;
            mAt = 0;
            mQueued = 0;
            for (auto& b : mBuckets)
            {
                b.clear();
            }
        }

        /// True when the flood is done.
        /// admit(from, to) filters a step; reached(from, to) hears of each that lowered a distance.
        template<typename ADMIT, typename REACHED>
        bool step(std::vector<U32>& dist, const std::vector<U32>& start, const std::vector<GraphArc>& arcs, U8 mask, ADMIT admit, REACHED reached,
                  S64& budget)
        {
            while (budget > 0)
            {
                if (mQueued == 0 && mAt == 0)
                {
                    if (mSeedAt >= mSeeds.size())
                    {
                        return true;
                    }
                    mCur = std::max(mCur, mSeeds[mSeedAt].first);
                }
                if (mAt == 0)
                {
                    while (mSeedAt < mSeeds.size() && mSeeds[mSeedAt].first <= mCur)
                    {
                        const auto& s = mSeeds[mSeedAt++];
                        // At or under what the node holds: a seed may restate a settled distance.
                        if (s.first <= dist[s.second])
                        {
                            dist[s.second] = s.first;
                            mBuckets[mCur % NUM_BUCKETS].push_back(s.second);
                            ++mQueued;
                        }
                    }
                }
                std::vector<U32>& bucket = mBuckets[mCur % NUM_BUCKETS];
                while (mAt < bucket.size() && budget > 0)
                {
                    const U32 n = bucket[mAt++];
                    --mQueued;
                    --budget;
                    if (dist[n] != mCur)
                    {
                        continue;
                    }
                    for (U32 a = start[n]; a < start[n + 1]; ++a)
                    {
                        const GraphArc& arc = arcs[a];
                        if (!(arc.mFlags & mask) || !admit(n, arc.mTo))
                        {
                            continue;
                        }
                        const U32 nd = mCur + arc.mCost;
                        if (nd < dist[arc.mTo])
                        {
                            dist[arc.mTo] = nd;
                            reached(n, arc.mTo);
                            mBuckets[nd % NUM_BUCKETS].push_back(arc.mTo);
                            ++mQueued;
                        }
                    }
                    budget -= static_cast<S64>(start[n + 1] - start[n]) / 4;
                }
                if (mAt < bucket.size())
                {
                    return false;
                }
                bucket.clear();
                mAt = 0;
                ++mCur;
            }
            return false;
        }

    private:
        static constexpr U32 NUM_BUCKETS = 64;
        std::vector<U32> mBuckets[NUM_BUCKETS];
        std::vector<std::pair<U32, U32>> mSeeds;
        size_t mSeedAt = 0;
        U32 mCur = 0;
        size_t mAt = 0;
        size_t mQueued = 0;
    };

    const GapEnclosure* previousGaps(const EnclosureProductPtr& previous, const EnclosureInput::Tile& tile)
    {
        if (!previous)
        {
            return nullptr;
        }
        const TileEnclosure* t = previous->tile(tile.mKey);
        return t && t->mAir == tile.mAir && t->mGaps.size() == tile.mAir->mGaps.size() ? t->mGaps.data() : nullptr;
    }

    void fillEnclosure(const TileEnclosure& t, U16 floor, U16 ceiling, U8 s, U8 e, U8 d, U8 o, U8 bucket, U16 space, U8 flags, Enclosure& out)
    {
        out.mSealed = (flags & ENCLOSURE_SEALED) != 0;
        out.mCanopy = (flags & ENCLOSURE_CANOPY) != 0;
        out.mS = s / 255.f;
        out.mE = e / 255.f;
        out.mO = o / 255.f;
        out.mD = d == ENCLOSURE_D_UNKNOWN ? 0.f : d * ENCLOSURE_D_UNIT_M;
        out.mBucket = static_cast<EnclosureBucket>(bucket);
        out.mSpace = space;
        out.mFloor = t.mOrigin.mV[VZ] + floor * t.mUnit;
        out.mCeiling = ceiling == AIR_CEILING_OPEN ? 1e30f : t.mOrigin.mV[VZ] + ceiling * t.mUnit;
    }

    /// The bucket a previous answer gave the coarse gap at a mesh-local point
    /// whose floor is within tol of z: what hysteresis holds to once the air
    /// under it has been rebuilt.
    EnclosureBucket previousBucket(const TileEnclosure* t, F32 x, F32 y, F32 z, F32 tol)
    {
        if (!t || t->mCell <= 0.f)
        {
            return EnclosureBucket::UNKNOWN;
        }
        const CoarseAir& c = t->mCoarse;
        const S32 cx = static_cast<S32>(std::floor((x - t->mOrigin.mV[VX]) / t->mCell)) / c.mCells;
        const S32 cy = static_cast<S32>(std::floor((y - t->mOrigin.mV[VY]) / t->mCell)) / c.mCells;
        if (cx < 0 || cy < 0 || cx >= c.mWidth || cy >= c.mHeight)
        {
            return EnclosureBucket::UNKNOWN;
        }
        const S32 cell = cx + cy * c.mWidth;
        for (U32 i = c.mStart[cell]; i < c.mStart[cell + 1]; ++i)
        {
            if (std::fabs(t->mOrigin.mV[VZ] + c.mGaps[i].mFloor * t->mUnit - z) <= tol)
            {
                return static_cast<EnclosureBucket>(c.mGaps[i].mBucket);
            }
        }
        return EnclosureBucket::UNKNOWN;
    }

    /// Visit the gaps over a mesh-local (x, y), detail first, coarse when the tile has no detail.
    void spaceId(const EnclosureProduct& p, Enclosure& out)
    {
        out.mSpaceId = out.mSpace && out.mSpace <= p.mSpaceIds.size() ? p.mSpaceIds[out.mSpace - 1] : 0;
    }

    template<typename FN>
    void visitGaps(const EnclosureProduct& p, F32 x, F32 y, FN fn)
    {
        TileKey key;
        key.mX = tileIndex(x);
        key.mY = tileIndex(y);
        const TileEnclosure* t = p.tile(key);
        if (!t || t->mCell <= 0.f)
        {
            return;
        }
        const S32 ix = static_cast<S32>(std::floor((x - t->mOrigin.mV[VX]) / t->mCell));
        const S32 iy = static_cast<S32>(std::floor((y - t->mOrigin.mV[VY]) / t->mCell));
        if (ix < 0 || iy < 0 || ix >= t->mWidth || iy >= t->mHeight)
        {
            return;
        }
        if (t->mAir)
        {
            const S32 leaf = findAirLeaf(*t->mAir, ix, iy);
            if (leaf < 0)
            {
                return;
            }
            const AirLeaf& l = t->mAir->mLeaves[leaf];
            for (U32 i = 0; i < l.mGapCount; ++i)
            {
                const AirGap& g = t->mAir->mGaps[l.mFirstGap + i];
                const GapEnclosure& e = t->mGaps[l.mFirstGap + i];
                Enclosure out;
                fillEnclosure(*t, g.mFloor, g.mCeiling, e.mS, e.mE, e.mD, e.mOpen, e.mBucket, e.mSpace, e.mFlags, out);
                spaceId(p, out);
                fn(out, g.mFloor, g.mCeiling, *t);
            }
            return;
        }
        const CoarseAir& c = t->mCoarse;
        const S32 cx = ix / c.mCells, cy = iy / c.mCells;
        if (cx >= c.mWidth || cy >= c.mHeight)
        {
            return;
        }
        const S32 cell = cx + cy * c.mWidth;
        for (U32 i = c.mStart[cell]; i < c.mStart[cell + 1]; ++i)
        {
            const CoarseGap& g = c.mGaps[i];
            Enclosure out;
            fillEnclosure(*t, g.mFloor, g.mCeiling, g.mS, g.mE, g.mD, g.mOpen, g.mBucket, g.mSpace, g.mFlags, out);
            spaceId(p, out);
            fn(out, g.mFloor, g.mCeiling, *t);
        }
    }
}

namespace sknav
{
    F32 enclosureE(F32 s, F32 d, F32 d_full)
    {
        const F32 t = llclamp(d / std::max(d_full, 1e-3f), 0.f, 1.f);
        return (1.f - s) * t * t * (3.f - 2.f * t);
    }

    EnclosureBucket enclosureBucket(F32 e, EnclosureBucket previous, F32 hysteresis)
    {
        const F32 lo = 0.25f, hi = 0.75f;
        switch (previous)
        {
            case EnclosureBucket::OUTDOORS:
                if (e < lo + hysteresis)
                {
                    return EnclosureBucket::OUTDOORS;
                }
                break;
            case EnclosureBucket::SHELTERED:
                if (e >= lo - hysteresis && e <= hi + hysteresis)
                {
                    return EnclosureBucket::SHELTERED;
                }
                break;
            case EnclosureBucket::INDOORS:
                if (e > hi - hysteresis)
                {
                    return EnclosureBucket::INDOORS;
                }
                break;
            default:
                break;
        }
        return e < lo ? EnclosureBucket::OUTDOORS : (e > hi ? EnclosureBucket::INDOORS : EnclosureBucket::SHELTERED);
    }

    S32 findAirLeaf(const TileAir& air, S32 ix, S32 iy)
    {
        if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
        {
            return -1;
        }
        for (S32 lv = 0; lv <= AIR_MAX_LEVEL; ++lv)
        {
            const S32 mask = ~((1 << lv) - 1);
            const U16 x = static_cast<U16>(ix & mask), y = static_cast<U16>(iy & mask);
            const auto it = std::lower_bound(air.mLeaves.begin(), air.mLeaves.end(), std::make_pair(y, x),
                                             [](const AirLeaf& l, const std::pair<U16, U16>& k) {
                                                 return l.mY < k.first || (l.mY == k.first && l.mX < k.second);
                                             });
            // A leaf starting here larger than this level covers the cell too,
            // and is found again at its own level.
            if (it != air.mLeaves.end() && it->mY == y && it->mX == x && it->mLevel <= lv)
            {
                return it->mLevel == lv ? static_cast<S32>(it - air.mLeaves.begin()) : -1;
            }
        }
        return -1;
    }

    size_t TileEnclosure::bytes() const
    {
        return sizeof(TileEnclosure) + mGaps.capacity() * sizeof(GapEnclosure) + mCoarse.mStart.capacity() * sizeof(U32) +
               mCoarse.mGaps.capacity() * sizeof(CoarseGap);
    }

    const TileEnclosure* EnclosureProduct::tile(const TileKey& key) const
    {
        const auto it = std::lower_bound(mTiles.begin(), mTiles.end(), key, [](const TileEnclosure& t, const TileKey& k) { return t.mKey < k; });
        return it != mTiles.end() && it->mKey == key ? &*it : nullptr;
    }

    bool EnclosureProduct::enclosureAt(const LLVector3& pos, Enclosure& out) const
    {
        bool found = false;
        visitGaps(*this, pos.mV[VX], pos.mV[VY], [&](const Enclosure& e, U16 floor, U16 ceiling, const TileEnclosure& t) {
            if (found)
            {
                return;
            }
            const F32 z = (pos.mV[VZ] - t.mOrigin.mV[VZ]) / t.mUnit;
            if (z >= floor - FLOOR_SLACK && (ceiling == AIR_CEILING_OPEN || z < ceiling))
            {
                out = e;
                found = true;
            }
        });
        return found;
    }

    void EnclosureProduct::gapsAt(F32 x, F32 y, const std::function<void(const Enclosure&)>& fn) const
    {
        visitGaps(*this, x, y, [&](const Enclosure& e, U16, U16, const TileEnclosure&) { fn(e); });
    }

    size_t EnclosureProduct::bytes() const
    {
        size_t n = sizeof(EnclosureProduct);
        for (const TileEnclosure& t : mTiles)
        {
            n += t.bytes();
        }
        return n;
    }

    struct EnclosurePass::State
    {
        std::shared_ptr<const EnclosureInput> mInput;
        std::shared_ptr<EnclosureProduct> mProduct;
        S32 mStage = STAGE_GRAPH;
        size_t mCursor = 0;         ///< tile within a per-tile stage
        bool mStarted = false;      ///< a flood stage has been seeded
        F64 mStageMs[NUM_STAGES] = {};

        // units
        S32 mClimb = 0;
        S32 mWalkHead = 0;
        S32 mWalkOverlap = 0;
        S32 mCarryOverlap = 0;
        F32 mHalfCellM = 0.f;

        std::map<TileKey, U32> mTileIndex;
        std::vector<S32> mZOffset;      ///< per tile, its units from the mesh's z
        std::vector<U32> mBase;         ///< per tile, its first node; one past the last at the end
        std::vector<U32> mNodeLeaf;
        std::vector<U32> mNodeTile;
        std::vector<S32> mFloor;        ///< global units
        std::vector<S32> mCeil;
        std::vector<U8> mNodeFlags;
        std::vector<GraphEdge> mEdges;
        std::vector<U32> mStart;
        std::vector<GraphArc> mArcs;
        std::vector<U32> mDsky;
        std::vector<U32> mDwalk;
        std::vector<S32> mLeafMap;      ///< scratch: fine cell to leaf, one tile at a time
        Flood mFlood;
        std::vector<U32> mCarrySeed;    ///< the walkable gap a carried depth came from
        S32 mCarryRise = 0;
        std::vector<std::pair<U32, U32>> mWalkSeeds;     ///< from neighbouring meshes, kept for the depth flood
        std::vector<U8> mCover;         ///< share of sky roofed over, in 1/255; 255 under the gap's own low ceiling, 0 with no sky map
        std::vector<std::pair<U32, U32>> mSkyWalkSeeds;  ///< walkable gaps under some sky, at the depth their cover starts them
        std::vector<U8> mOpen;          ///< side openness, in 1/255; 0 unread

        const EnclosureParams& params() const { return mInput->mParams; }
        const TileAir& air(size_t t) const { return *mInput->mTiles[t].mAir; }

        void prepare();
        void graphTile(size_t t);
        void seamTile(size_t t);
        void pair(U32 a0, U32 na, const AirGap* ga, S32 oa, U32 b0, U32 nb, const AirGap* gb, S32 ob, U8 cost, std::vector<GraphEdge>& out) const;
        void finishGraph();
        void neighbourSeeds(std::vector<std::pair<U32, U32>>& sky, std::vector<std::pair<U32, U32>>& walk) const;
        void answerTile(size_t t);
        void coarseTile(size_t t);
        bool runStage(S64& budget);
    };

    void EnclosurePass::State::prepare()
    {
        const EnclosureParams& p = params();
        const EnclosureInput& in = *mInput;
        mProduct = std::make_shared<EnclosureProduct>();
        mProduct->mGeneration = in.mGeneration;
        F32 unit = 0.125f, cell = 0.125f;
        U32 nodes = 0;
        mBase.reserve(in.mTiles.size() + 1);
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            unit = a.mUnit;
            cell = a.mCell;
            mTileIndex[in.mTiles[t].mKey] = static_cast<U32>(t);
            mZOffset.push_back(static_cast<S32>(std::lround(a.mOrigin.mV[VZ] / a.mUnit)));
            mBase.push_back(nodes);
            nodes += static_cast<U32>(a.mGaps.size());
        }
        mBase.push_back(nodes);
        mClimb = static_cast<S32>(std::lround(p.mClimbM / unit));
        mWalkHead = static_cast<S32>(std::lround(p.mWalkHeadM / unit));
        mWalkOverlap = static_cast<S32>(std::lround(p.mWalkOverlapM / unit));
        mCarryOverlap = static_cast<S32>(std::lround(p.mCarryOverlapM / unit));
        mCarryRise = static_cast<S32>(std::lround(p.mCarryRiseM / unit));
        mHalfCellM = cell * 0.5f;
        mNodeLeaf.resize(nodes);
        mNodeTile.resize(nodes);
        mFloor.resize(nodes);
        mCeil.resize(nodes);
        mNodeFlags.assign(nodes, 0);
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            for (size_t l = 0; l < a.mLeaves.size(); ++l)
            {
                const AirLeaf& leaf = a.mLeaves[l];
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 n = mBase[t] + leaf.mFirstGap + i;
                    const AirGap& g = a.mGaps[leaf.mFirstGap + i];
                    mNodeLeaf[n] = static_cast<U32>(l);
                    mNodeTile[n] = static_cast<U32>(t);
                    mFloor[n] = g.mFloor + mZOffset[t];
                    mCeil[n] = ceilingUnits(g.mCeiling, mZOffset[t]);
                    if ((g.mFlags & AirGap::WALKABLE) && mCeil[n] - mFloor[n] >= mWalkHead)
                    {
                        mNodeFlags[n] |= NODE_WALK;
                    }
                }
            }
        }
        mProduct->mTiles.resize(in.mTiles.size());
    }

    void EnclosurePass::State::pair(U32 a0, U32 na, const AirGap* ga, S32 oa, U32 b0, U32 nb, const AirGap* gb, S32 ob, U8 cost,
                                    std::vector<GraphEdge>& out) const
    {
        // Both columns run lowest first and their gaps are disjoint, so the
        // overlapping pairs fall out of one sweep.
        U32 i = 0, j = 0;
        while (i < na && j < nb)
        {
            const S32 fa = ga[i].mFloor + oa, ca = ceilingUnits(ga[i].mCeiling, oa);
            const S32 fb = gb[j].mFloor + ob, cb = ceilingUnits(gb[j].mCeiling, ob);
            const S32 overlap = std::min(ca, cb) - std::max(fa, fb);
            if (overlap >= 1)
            {
                const U32 a = a0 + i, b = b0 + j;
                const bool within = std::abs(fa - fb) <= mClimb;
                U8 flags = EDGE_SKY;
                if (overlap >= mCarryOverlap)
                {
                    flags |= EDGE_CARRY;
                }
                if (within && overlap >= mWalkOverlap && (mNodeFlags[a] & NODE_WALK) && (mNodeFlags[b] & NODE_WALK))
                {
                    flags |= EDGE_WALK;
                }
                out.push_back({ a, b, cost, flags });
            }
            if (ca < cb)
            {
                ++i;
            }
            else if (cb < ca)
            {
                ++j;
            }
            else
            {
                ++i;
                ++j;
            }
        }
    }

    void EnclosurePass::State::graphTile(size_t t)
    {
        const TileAir& a = air(t);
        const S32 w = a.mWidth, h = a.mHeight;
        mLeafMap.assign(static_cast<size_t>(w) * h, -1);
        for (size_t l = 0; l < a.mLeaves.size(); ++l)
        {
            const AirLeaf& leaf = a.mLeaves[l];
            const S32 s = 1 << leaf.mLevel;
            for (S32 y = leaf.mY; y < std::min(h, leaf.mY + s); ++y)
            {
                for (S32 x = leaf.mX; x < std::min(w, leaf.mX + s); ++x)
                {
                    mLeafMap[x + y * w] = static_cast<S32>(l);
                }
            }
        }
        const U32 base = mBase[t];
        const S32 off = mZOffset[t];
        for (size_t l = 0; l < a.mLeaves.size(); ++l)
        {
            const AirLeaf& leaf = a.mLeaves[l];
            const S32 s = 1 << leaf.mLevel;
            // The +x and +y sides only: every touching pair is met once.
            for (S32 side = 0; side < 2; ++side)
            {
                S32 last = -1;
                for (S32 k = 0; k < s; ++k)
                {
                    const S32 x = side == 0 ? leaf.mX + s : leaf.mX + k;
                    const S32 y = side == 0 ? leaf.mY + k : leaf.mY + s;
                    if (x >= w || y >= h)
                    {
                        break;
                    }
                    const S32 n = mLeafMap[x + y * w];
                    if (n < 0 || n == last)
                    {
                        continue;
                    }
                    last = n;
                    const AirLeaf& other = a.mLeaves[n];
                    const U8 cost = static_cast<U8>(s + (1 << other.mLevel));
                    pair(base + leaf.mFirstGap, leaf.mGapCount, &a.mGaps[leaf.mFirstGap], off, base + other.mFirstGap, other.mGapCount,
                         &a.mGaps[other.mFirstGap], off, cost, mEdges);
                }
            }
        }
    }

    void EnclosurePass::State::seamTile(size_t t)
    {
        // Across the +x and +y seams, gap by gap along the neighbours' level-0
        // strips: the strip's k-th gap is the covering leaf's k-th.
        const EnclosureInput& in = *mInput;
        const TileAir& a = air(t);
        std::vector<GraphEdge> edges;
        for (S32 side = 0; side < 2; ++side)
        {
            TileKey nk = in.mTiles[t].mKey;
            (side == 0 ? nk.mX : nk.mY) += 1;
            const auto it = mTileIndex.find(nk);
            if (it == mTileIndex.end())
            {
                continue;
            }
            const size_t u = it->second;
            const TileAir& b = air(u);
            const SeamStrip& sa = a.mSeams[side == 0 ? TileAir::SIDE_POS_X : TileAir::SIDE_POS_Y];
            const SeamStrip& sb = b.mSeams[side == 0 ? TileAir::SIDE_NEG_X : TileAir::SIDE_NEG_Y];
            const S32 along = side == 0 ? VY : VX;
            const S32 shift = static_cast<S32>(std::lround((a.mOrigin.mV[along] - b.mOrigin.mV[along]) / a.mCell));
            const S32 na = static_cast<S32>(sa.mStart.size()) - 1, nb = static_cast<S32>(sb.mStart.size()) - 1;
            edges.clear();
            for (S32 i = 0; i < na; ++i)
            {
                const S32 j = i + shift;
                if (j < 0 || j >= nb || !sa.count(i) || !sb.count(j))
                {
                    continue;
                }
                const S32 la = side == 0 ? findAirLeaf(a, a.mWidth - 1, i) : findAirLeaf(a, i, a.mHeight - 1);
                const S32 lb = side == 0 ? findAirLeaf(b, 0, j) : findAirLeaf(b, j, 0);
                if (la < 0 || lb < 0 || a.mLeaves[la].mGapCount != sa.count(i) || b.mLeaves[lb].mGapCount != sb.count(j))
                {
                    continue;
                }
                const AirLeaf& leaf_a = a.mLeaves[la];
                const AirLeaf& leaf_b = b.mLeaves[lb];
                const U8 cost = static_cast<U8>((1 << leaf_a.mLevel) + (1 << leaf_b.mLevel));
                pair(mBase[t] + leaf_a.mFirstGap, sa.count(i), &sa.mGaps[sa.mStart[i]], mZOffset[t], mBase[u] + leaf_b.mFirstGap, sb.count(j),
                     &sb.mGaps[sb.mStart[j]], mZOffset[u], cost, edges);
            }
            // Cells along a side share leaves: one edge per pair, with every
            // cell's link kinds.
            std::sort(edges.begin(), edges.end(), [](const GraphEdge& x, const GraphEdge& y) { return x.mA < y.mA || (x.mA == y.mA && x.mB < y.mB); });
            for (size_t k = 0; k < edges.size(); ++k)
            {
                if (!mEdges.empty() && k > 0 && edges[k - 1].mA == edges[k].mA && edges[k - 1].mB == edges[k].mB)
                {
                    mEdges.back().mFlags |= edges[k].mFlags;
                    continue;
                }
                mEdges.push_back(edges[k]);
            }
        }
    }

    void EnclosurePass::State::finishGraph()
    {
        const size_t n = mNodeFlags.size();
        mStart.assign(n + 1, 0);
        for (const GraphEdge& e : mEdges)
        {
            ++mStart[e.mA + 1];
            ++mStart[e.mB + 1];
        }
        for (size_t i = 0; i < n; ++i)
        {
            mStart[i + 1] += mStart[i];
        }
        mArcs.resize(mStart[n]);
        std::vector<U32> fill(mStart.begin(), mStart.end() - 1);
        for (const GraphEdge& e : mEdges)
        {
            mArcs[fill[e.mA]++] = { e.mB, e.mCost, e.mFlags };
            mArcs[fill[e.mB]++] = { e.mA, e.mCost, e.mFlags };
        }
        std::vector<GraphEdge>().swap(mEdges);
        std::vector<S32>().swap(mLeafMap);
        mDsky.assign(n, INF);
        mDwalk.assign(n, INF);
    }

    void EnclosurePass::State::neighbourSeeds(std::vector<std::pair<U32, U32>>& sky, std::vector<std::pair<U32, U32>>& walk) const
    {
        // Another mesh's air over the same ground, or just past this mesh's
        // edge, hands over its last distances at the cost of the step there.
        const EnclosureInput& in = *mInput;
        if (in.mNeighbours.empty())
        {
            return;
        }
        const EnclosureParams& p = params();
        const F32 cell = mHalfCellM * 2.f;
        // A tile's height range ends TILE_TOP_PAD_M over its highest solid (finishTileInput), where its open air leaves it.
        std::unordered_map<const TileEnclosure*, F32> tops;
        auto openTop = [&tops](const TileEnclosure& nt) {
            auto it = tops.find(&nt);
            if (it == tops.end())
            {
                F32 top = -1e30f;
                for (const TopInterval& i : nt.mAir ? nt.mAir->mTop : std::vector<TopInterval>())
                {
                    top = std::max(top, nt.mOrigin.mV[VZ] + i.mTop * nt.mUnit);
                }
                it = tops.emplace(&nt, top > -1e29f ? top + TILE_TOP_PAD_M : 1e30f).first;
            }
            return it->second;
        };
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            const TileKey key = in.mTiles[t].mKey;
            bool open_side[4];
            for (S32 side = 0; side < 4; ++side)
            {
                TileKey nk = key;
                (side < 2 ? nk.mX : nk.mY) += (side & 1) ? 1 : -1;
                open_side[side] = mTileIndex.find(nk) == mTileIndex.end();
            }
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const S32 s = 1 << leaf.mLevel;
                const F64 cx = in.mMeshOrigin.mdV[VX] + a.mOrigin.mV[VX] + (leaf.mX + s * 0.5) * cell;
                const F64 cy = in.mMeshOrigin.mdV[VY] + a.mOrigin.mV[VY] + (leaf.mY + s * 0.5) * cell;
                struct Probe
                {
                    F64 mX, mY;
                    U32 mCost;
                };
                Probe probes[5];
                S32 count = 0;
                probes[count++] = { cx, cy, 0 };
                const F64 reach = (s * 0.5 + 0.5) * cell;
                if (open_side[0] && leaf.mX == 0) probes[count++] = { cx - reach, cy, static_cast<U32>(s + 1) };
                if (open_side[1] && leaf.mX + s >= a.mWidth) probes[count++] = { cx + reach, cy, static_cast<U32>(s + 1) };
                if (open_side[2] && leaf.mY == 0) probes[count++] = { cx, cy - reach, static_cast<U32>(s + 1) };
                if (open_side[3] && leaf.mY + s >= a.mHeight) probes[count++] = { cx, cy + reach, static_cast<U32>(s + 1) };
                for (const EnclosureInput::Neighbour& nb : in.mNeighbours)
                {
                    if (!nb.mProduct)
                    {
                        continue;
                    }
                    for (S32 k = 0; k < count; ++k)
                    {
                        const F32 lx = static_cast<F32>(probes[k].mX - nb.mMeshOrigin.mdV[VX]);
                        const F32 ly = static_cast<F32>(probes[k].mY - nb.mMeshOrigin.mdV[VY]);
                        visitGaps(*nb.mProduct, lx, ly, [&](const Enclosure& e, U16, U16, const TileEnclosure& nt) {
                            const F64 base_z = in.mMeshOrigin.mdV[VZ] + a.mOrigin.mV[VZ];
                            const F64 nb_floor = nb.mMeshOrigin.mdV[VZ] + e.mFloor;
                            // Open air is known only to the top of its own tile: above that another mesh's solids may stand.
                            const F64 nb_ceil = nb.mMeshOrigin.mdV[VZ] + (e.mCeiling > 1e29f ? openTop(nt) : e.mCeiling);
                            for (U32 i = 0; i < leaf.mGapCount; ++i)
                            {
                                const U32 n = mBase[t] + leaf.mFirstGap + i;
                                const F64 floor = base_z + (mFloor[n] - mZOffset[t]) * a.mUnit;
                                const F64 ceil = mCeil[n] >= OPEN_TOP ? 1e30 : base_z + (mCeil[n] - mZOffset[t]) * a.mUnit;
                                if (std::min(ceil, nb_ceil) - std::max(floor, nb_floor) < a.mUnit)
                                {
                                    continue;
                                }
                                if (e.mS > 0.f)
                                {
                                    const F32 d = -p.mSkyDecayM * std::log(e.mS);
                                    sky.emplace_back(static_cast<U32>(std::lround(d / mHalfCellM)) + probes[k].mCost, n);
                                }
                                if ((mNodeFlags[n] & NODE_WALK) && e.mBucket != EnclosureBucket::UNKNOWN && !e.mSealed &&
                                    std::abs(floor - nb_floor) <= p.mClimbM)
                                {
                                    walk.emplace_back(static_cast<U32>(std::lround(e.mD / mHalfCellM)) + probes[k].mCost, n);
                                }
                            }
                        });
                    }
                }
            }
        }
    }

    void EnclosurePass::State::answerTile(size_t t)
    {
        const EnclosureInput& in = *mInput;
        const EnclosureParams& p = params();
        const TileAir& a = air(t);
        TileEnclosure& out = mProduct->mTiles[t];
        out.mKey = in.mTiles[t].mKey;
        out.mOrigin = a.mOrigin;
        out.mCell = a.mCell;
        out.mUnit = a.mUnit;
        out.mWidth = a.mWidth;
        out.mHeight = a.mHeight;
        out.mAir = in.mTiles[t].mAir;
        out.mGaps.resize(a.mGaps.size());
        const GapEnclosure* previous = previousGaps(in.mPrevious, in.mTiles[t]);
        const TileEnclosure* previous_tile = in.mPrevious ? in.mPrevious->tile(out.mKey) : nullptr;
        for (const AirLeaf& leaf : a.mLeaves)
        {
            const F64 area = static_cast<F64>(1 << leaf.mLevel) * (1 << leaf.mLevel) * a.mCell * a.mCell;
            for (U32 i = 0; i < leaf.mGapCount; ++i)
            {
                const U32 g = leaf.mFirstGap + i;
                const U32 n = mBase[t] + g;
                const F32 s = mDsky[n] == INF ? 0.f : std::exp(-(mDsky[n] * mHalfCellM) / p.mSkyDecayM);
                GapEnclosure& e = out.mGaps[g];
                e.mS = quantise(s);
                e.mCover = mCover[n];
                e.mOpen = mOpen[n];
                ++mProduct->mGapCount;
                mProduct->mArea += area;
                if (mDwalk[n] == INF)
                {
                    e.mD = ENCLOSURE_D_UNKNOWN;
                    e.mE = 0;
                    e.mBucket = static_cast<U8>(EnclosureBucket::UNKNOWN);
                    ++mProduct->mUnknownGaps;
                    mProduct->mUnknownArea += area;
                    continue;
                }
                const F32 d = mDwalk[n] * mHalfCellM;
                const F32 ev = enclosureE(s, d, p.mDepthFullM);
                e.mD = static_cast<U8>(std::min<S32>(ENCLOSURE_D_DEEP, std::lround(d / ENCLOSURE_D_UNIT_M)));
                e.mE = quantise(ev);
                if (mCarrySeed[n] != INF && (mNodeFlags[mCarrySeed[n]] & NODE_SEALED))
                {
                    e.mD = ENCLOSURE_D_DEEP;
                    e.mFlags = ENCLOSURE_SEALED;
                    e.mBucket = static_cast<U8>(EnclosureBucket::INDOORS);
                    ++mProduct->mSealedGaps;
                    continue;
                }
                // Roofed but open at the sides: however deep, a canopy shelters rather than encloses.
                const bool canopy = mCover[n] >= p.mShelterCover * 255.f && mOpen[n] >= p.mShelterOpen * 255.f;
                e.mFlags = canopy ? ENCLOSURE_CANOPY : 0;
                // The same air: its own last bucket. Rebuilt air: the last one of the metre it stands in.
                const F32 half = (1 << leaf.mLevel) * 0.5f * a.mCell;
                const EnclosureBucket prev = previous ? static_cast<EnclosureBucket>(previous[g].mBucket)
                                                      : previousBucket(previous_tile, a.mOrigin.mV[VX] + leaf.mX * a.mCell + half,
                                                                       a.mOrigin.mV[VY] + leaf.mY * a.mCell + half,
                                                                       a.mOrigin.mV[VZ] + a.mGaps[g].mFloor * a.mUnit, p.mClimbM);
                const EnclosureBucket b = enclosureBucket(ev, prev, p.mHysteresis);
                e.mBucket = static_cast<U8>(canopy && b == EnclosureBucket::INDOORS ? EnclosureBucket::SHELTERED : b);
            }
        }
    }

    void EnclosurePass::State::coarseTile(size_t t)
    {
        const TileAir& a = air(t);
        TileEnclosure& out = mProduct->mTiles[t];
        CoarseAir& c = out.mCoarse;
        const TileEnclosure* previous_tile = mInput->mPrevious ? mInput->mPrevious->tile(out.mKey) : nullptr;
        c.mCells = std::max(1, static_cast<S32>(std::lround(1.f / a.mCell)));
        c.mWidth = (a.mWidth + c.mCells - 1) / c.mCells;
        c.mHeight = (a.mHeight + c.mCells - 1) / c.mCells;
        struct Part
        {
            U32 mCell;
            U16 mFloor;
            U32 mGap;
            F32 mArea;
        };
        std::vector<Part> parts;
        parts.reserve(a.mGaps.size() * 2);
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
                    if (ox <= 0 || oy <= 0)
                    {
                        continue;
                    }
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const U32 g = leaf.mFirstGap + i;
                        parts.push_back({ static_cast<U32>(cx + cy * c.mWidth), a.mGaps[g].mFloor, g, static_cast<F32>(ox * oy) });
                    }
                }
            }
        }
        std::sort(parts.begin(), parts.end(), [](const Part& x, const Part& y) { return x.mCell < y.mCell || (x.mCell == y.mCell && x.mFloor < y.mFloor); });
        c.mStart.assign(static_cast<size_t>(c.mWidth) * c.mHeight + 1, 0);
        c.mGaps.clear();
        size_t i = 0;
        U32 cell = 0;
        while (i < parts.size())
        {
            // Gaps whose floors sit within a climb of the lowest are one coarse gap.
            size_t j = i;
            F32 area = 0.f, s = 0.f, e = 0.f, d = 0.f, o = 0.f, known = 0.f, sealed = 0.f, canopy = 0.f;
            U16 floor = parts[i].mFloor, ceiling = 0;
            while (j < parts.size() && parts[j].mCell == parts[i].mCell && parts[j].mFloor <= parts[i].mFloor + mClimb)
            {
                const AirGap& g = a.mGaps[parts[j].mGap];
                const GapEnclosure& ge = out.mGaps[parts[j].mGap];
                const F32 w = parts[j].mArea;
                floor = std::min(floor, g.mFloor);
                ceiling = (ceiling == AIR_CEILING_OPEN || g.mCeiling == AIR_CEILING_OPEN) ? AIR_CEILING_OPEN : std::max(ceiling, g.mCeiling);
                area += w;
                s += w * ge.mS;
                o += w * ge.mOpen;
                if (ge.mD != ENCLOSURE_D_UNKNOWN)
                {
                    known += w;
                    sealed += (ge.mFlags & ENCLOSURE_SEALED) ? w : 0.f;
                    canopy += (ge.mFlags & ENCLOSURE_CANOPY) ? w : 0.f;
                    e += w * ge.mE;
                    d += w * ge.mD;
                }
                ++j;
            }
            CoarseGap g;
            g.mFloor = floor;
            g.mCeiling = ceiling;
            g.mS = static_cast<U8>(std::lround(s / area));
            g.mOpen = static_cast<U8>(std::lround(o / area));
            if (known * 2.f >= area)
            {
                g.mE = static_cast<U8>(std::lround(e / known));
                g.mD = static_cast<U8>(std::lround(d / known));
                g.mFlags = sealed * 2.f >= known ? ENCLOSURE_SEALED : canopy * 2.f >= known ? ENCLOSURE_CANOPY : 0;
                const S32 cell_index = static_cast<S32>(parts[i].mCell);
                const F32 x = a.mOrigin.mV[VX] + ((cell_index % c.mWidth) + 0.5f) * c.mCells * a.mCell;
                const F32 y = a.mOrigin.mV[VY] + ((cell_index / c.mWidth) + 0.5f) * c.mCells * a.mCell;
                const EnclosureBucket prev = previousBucket(previous_tile, x, y, a.mOrigin.mV[VZ] + floor * a.mUnit, params().mClimbM);
                const EnclosureBucket b = enclosureBucket(g.mE / 255.f, prev, params().mHysteresis);
                g.mBucket = static_cast<U8>((g.mFlags & ENCLOSURE_SEALED)                                    ? EnclosureBucket::INDOORS
                                            : (g.mFlags & ENCLOSURE_CANOPY) && b == EnclosureBucket::INDOORS ? EnclosureBucket::SHELTERED
                                                                                                             : b);
            }
            while (cell < parts[i].mCell)
            {
                c.mStart[++cell] = static_cast<U32>(c.mGaps.size());
            }
            c.mGaps.push_back(g);
            i = j;
        }
        while (cell < c.mStart.size() - 1)
        {
            c.mStart[++cell] = static_cast<U32>(c.mGaps.size());
        }
        c.mGaps.shrink_to_fit();
        U64 h = mProduct->mSummary ? mProduct->mSummary : SEED;
        h = mix(h, static_cast<U64>(static_cast<U32>(out.mKey.mX)) << 32 | static_cast<U32>(out.mKey.mY));
        for (const CoarseGap& g : c.mGaps)
        {
            h = mix(h, (g.mS >> 4) | ((g.mD == ENCLOSURE_D_UNKNOWN ? 0xff : g.mD >> 3) << 8));
        }
        mProduct->mSummary = h;
    }

    bool EnclosurePass::State::runStage(S64& budget)
    {
        const EnclosureInput& in = *mInput;
        const size_t tiles = in.mTiles.size();
        switch (mStage)
        {
            case STAGE_GRAPH:
                if (!mStarted)
                {
                    prepare();
                    mStarted = true;
                    budget -= static_cast<S64>(mNodeFlags.size() / 8);
                }
                while (mCursor < tiles && budget > 0)
                {
                    graphTile(mCursor);
                    seamTile(mCursor);
                    budget -= static_cast<S64>(air(mCursor).mGaps.size()) * 2;
                    ++mCursor;
                }
                if (mCursor < tiles)
                {
                    return false;
                }
                finishGraph();
                break;
            case STAGE_SKY:
                if (!mStarted)
                {
                    const EnclosureParams& p = params();
                    std::vector<std::pair<U32, U32>> seeds;
                    // Per 2 m square of the tile, the covers read there, by whole metre of height.
                    struct Read
                    {
                        S32 mZ;
                        F32 mCover, mRoof, mOpen;
                    };
                    std::vector<std::vector<Read>> covers;
                    mCover.assign(mNodeFlags.size(), in.mSky ? 255 : 0);
                    mOpen.assign(mNodeFlags.size(), 0);
                    // One dense window over the mesh and what stands within reach of it.
                    std::unique_ptr<SkyView> view;
                    if (in.mSky && tiles)
                    {
                        F64 lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
                        for (size_t t = 0; t < tiles; ++t)
                        {
                            const TileAir& a = air(t);
                            for (S32 k = 0; k < 2; ++k)
                            {
                                const F64 o = in.mMeshOrigin.mdV[k] + a.mOrigin.mV[k];
                                lo[k] = std::min(lo[k], o);
                                hi[k] = std::max(hi[k], o + (k == 0 ? a.mWidth : a.mHeight) * a.mCell);
                            }
                        }
                        const F64 m = p.mSkyReachM;
                        view = std::make_unique<SkyView>(*in.mSky, lo[0] - m, lo[1] - m, hi[0] + m, hi[1] + m);
                        budget -= static_cast<S64>((hi[0] - lo[0] + 2 * m) * (hi[1] - lo[1] + 2 * m));
                    }
                    for (size_t t = 0; t < tiles; ++t)
                    {
                        const TileAir& a = air(t);
                        const S32 side = static_cast<S32>(std::ceil(a.mWidth * a.mCell * 0.5)) + 1;
                        covers.assign(static_cast<size_t>(side) * (static_cast<S32>(std::ceil(a.mHeight * a.mCell * 0.5)) + 1), {});
                        for (const AirLeaf& leaf : a.mLeaves)
                        {
                            const F64 s = (1 << leaf.mLevel) * 0.5 * a.mCell;
                            const F64 x = in.mMeshOrigin.mdV[VX] + a.mOrigin.mV[VX] + leaf.mX * a.mCell + s;
                            const F64 y = in.mMeshOrigin.mdV[VY] + a.mOrigin.mV[VY] + leaf.mY * a.mCell + s;
                            for (U32 i = 0; i < leaf.mGapCount; ++i)
                            {
                                const AirGap& g = a.mGaps[leaf.mFirstGap + i];
                                const bool own_sky = (g.mFlags & AirGap::OWN_SKY) != 0;
                                const F32 headroom = g.mCeiling == AIR_CEILING_OPEN ? 1e6f : (g.mCeiling - g.mFloor) * a.mUnit;
                                const U32 n = mBase[t] + leaf.mFirstGap + i;
                                if (!own_sky && (!in.mSky || headroom < SKY_COVER_HEADROOM_M))
                                {
                                    // Under its own low ceiling only the sides are asked, at eye height and so under that ceiling.
                                    if (view && (mNodeFlags[n] & NODE_WALK))
                                    {
                                        const F64 z = std::floor(4.0 * (in.mMeshOrigin.mdV[VZ] + a.mOrigin.mV[VZ] + g.mFloor * a.mUnit + std::min(SKY_COVER_EYE_M, headroom * 0.5f)));
                                        const S32 key = -1 - static_cast<S32>(z);
                                        const S32 cx = static_cast<S32>((leaf.mX * a.mCell + s) * 0.5), cy = static_cast<S32>((leaf.mY * a.mCell + s) * 0.5);
                                        std::vector<Read>& at = covers[cx + cy * side];
                                        const auto hit = std::find_if(at.begin(), at.end(), [key](const Read& r) { return r.mZ == key; });
                                        F32 open = 0.f;
                                        if (hit != at.end())
                                        {
                                            open = hit->mOpen;
                                        }
                                        else
                                        {
                                            open = view->sideOpenAt(LLVector3d(std::floor(x) + 0.5, std::floor(y) + 0.5, 0.25 * z + 0.125));
                                            at.push_back({ key, 0.f, 0.f, open });
                                            budget -= 16;
                                        }
                                        mOpen[n] = quantise(open);
                                    }
                                    continue;
                                }
                                // The sky a standing head sees, less what the solids over it hide.
                                F32 cover = 0.f, roof = 0.f, open = 0.f;
                                if (view)
                                {
                                    // Read at the even metre over the head, once per 2 m square and 2 m of height: the first gap there speaks for the rest.
                                    const F32 head = std::min(SKY_COVER_EYE_M, headroom * 0.5f);
                                    const F64 z = 2.0 * std::ceil(0.5 * (in.mMeshOrigin.mdV[VZ] + a.mOrigin.mV[VZ] + g.mFloor * a.mUnit + head));
                                    const S32 cx = static_cast<S32>((leaf.mX * a.mCell + s) * 0.5), cy = static_cast<S32>((leaf.mY * a.mCell + s) * 0.5);
                                    std::vector<Read>& at = covers[cx + cy * side];
                                    const auto hit = std::find_if(at.begin(), at.end(), [z](const Read& r) { return r.mZ == static_cast<S32>(z); });
                                    if (hit != at.end())
                                    {
                                        cover = hit->mCover;
                                        roof = hit->mRoof;
                                        open = hit->mOpen;
                                    }
                                    else
                                    {
                                        cover = view->coverAbove(LLVector3d(std::floor(x) + 0.5, std::floor(y) + 0.5, z), p.mSkyReachM, &roof, &open);
                                        at.push_back({ static_cast<S32>(z), cover, roof, open });
                                        budget -= 16;
                                    }
                                }
                                mCover[n] = quantise(roof);
                                mOpen[n] = quantise(open);
                                if (cover > 1.f - SKY_COVER_OPEN_MIN)
                                {
                                    continue;
                                }
                                // Seeded as far out as its hidden sky: S starts at the open share, and D at the share of full depth
                                // its roof gives it, walls counting only as far as it is roofed: a street stays shallow, a half-roofed hall does not.
                                mNodeFlags[n] |= NODE_SKY;
                                seeds.emplace_back(static_cast<U32>(std::lround(-p.mSkyDecayM * std::log(1.f - cover) / mHalfCellM)), n);
                                if (mNodeFlags[n] & NODE_WALK)
                                {
                                    mSkyWalkSeeds.emplace_back(static_cast<U32>(std::lround((roof + (cover - roof) * roof) * p.mDepthFullM / mHalfCellM)), n);
                                }
                            }
                        }
                    }
                    neighbourSeeds(seeds, mWalkSeeds);
                    mFlood.begin(std::move(seeds));
                    mStarted = true;
                }
                if (!mFlood.step(mDsky, mStart, mArcs, EDGE_SKY, [](U32, U32) { return true; }, [](U32, U32) {}, budget))
                {
                    return false;
                }
                break;
            case STAGE_DEPTH:
                if (!mStarted)
                {
                    std::vector<std::pair<U32, U32>> seeds;
                    seeds.swap(mWalkSeeds);
                    seeds.insert(seeds.end(), mSkyWalkSeeds.begin(), mSkyWalkSeeds.end());
                    std::vector<std::pair<U32, U32>>().swap(mSkyWalkSeeds);
                    mFlood.begin(std::move(seeds));
                    mStarted = true;
                }
                if (!mFlood.step(mDwalk, mStart, mArcs, EDGE_WALK, [this](U32, U32 n) { return (mNodeFlags[n] & NODE_WALK) != 0; }, [](U32, U32) {},
                                 budget))
                {
                    return false;
                }
                break;
            case STAGE_CARRY:
                if (!mStarted)
                {
                    // Unwalkable air takes the walkable depth beside it plus the way there.
                    std::vector<std::pair<U32, U32>> seeds;
                    mCarrySeed.assign(mNodeFlags.size(), INF);
                    // A floor with no walkable way from outside is as deep as depth goes.
                    const U32 deep = static_cast<U32>(std::lround(ENCLOSURE_D_DEEP * ENCLOSURE_D_UNIT_M / mHalfCellM));
                    for (U32 n = 0; n < mNodeFlags.size(); ++n)
                    {
                        if (params().mSealedIndoors && (mNodeFlags[n] & NODE_WALK) && mDwalk[n] == INF)
                        {
                            mDwalk[n] = deep;
                            mNodeFlags[n] |= NODE_SEALED;
                        }
                        if ((mNodeFlags[n] & NODE_WALK) && mDwalk[n] != INF)
                        {
                            seeds.emplace_back(mDwalk[n], n);
                            mCarrySeed[n] = n;
                        }
                    }
                    mFlood.begin(std::move(seeds));
                    mStarted = true;
                }
                if (!mFlood.step(mDwalk, mStart, mArcs, EDGE_CARRY,
                                 [this](U32 from, U32 to) {
                                     // Not up a facade or a shaft: only so far from the floor it started on.
                                     return !(mNodeFlags[to] & NODE_WALK) && std::abs(mFloor[to] - mFloor[mCarrySeed[from]]) <= mCarryRise;
                                 },
                                 [this](U32 from, U32 to) { mCarrySeed[to] = mCarrySeed[from]; }, budget))
                {
                    return false;
                }
                // Else the walkable gap straight below it, through no more than a climb of solid.
                for (size_t t = 0; t < tiles; ++t)
                {
                    for (const AirLeaf& leaf : air(t).mLeaves)
                    {
                        for (U32 i = 1; i < leaf.mGapCount; ++i)
                        {
                            const U32 n = mBase[t] + leaf.mFirstGap + i, below = n - 1;
                            if (!(mNodeFlags[n] & NODE_WALK) && mDwalk[n] == INF && (mNodeFlags[below] & NODE_WALK) && mDwalk[below] != INF &&
                                mFloor[n] - mCeil[below] <= mClimb)
                            {
                                mDwalk[n] = mDwalk[below];
                                mCarrySeed[n] = mCarrySeed[below];
                            }
                        }
                    }
                }
                break;
            case STAGE_ANSWER:
                while (mCursor < tiles && budget > 0)
                {
                    answerTile(mCursor);
                    budget -= static_cast<S64>(air(mCursor).mGaps.size());
                    ++mCursor;
                }
                if (mCursor < tiles)
                {
                    return false;
                }
                break;
            case STAGE_COARSE:
                while (mCursor < tiles && budget > 0)
                {
                    coarseTile(mCursor);
                    budget -= static_cast<S64>(air(mCursor).mGaps.size());
                    ++mCursor;
                }
                if (mCursor < tiles)
                {
                    return false;
                }
                break;
            default:
                return true;
        }
        ++mStage;
        mCursor = 0;
        mStarted = false;
        return mStage == STAGE_DONE;
    }

    EnclosurePass::EnclosurePass(std::shared_ptr<const EnclosureInput> input)
        : mState(std::make_unique<State>())
    {
        mState->mInput = std::move(input);
    }

    EnclosurePass::~EnclosurePass() = default;

    bool EnclosurePass::step(U32 budget)
    {
        State& s = *mState;
        S64 left = budget;
        while (s.mStage != STAGE_DONE && left > 0)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const S32 stage = s.mStage;
            const bool done = s.runStage(left);
            s.mStageMs[stage] += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (done)
            {
                F64 total = 0.0;
                for (F64 ms : s.mStageMs)
                {
                    total += ms;
                }
                s.mProduct->mMillis = static_cast<F32>(total);
                for (const TileEnclosure& kept : s.mInput->mKept)
                {
                    if (!s.mProduct->tile(kept.mKey))
                    {
                        s.mProduct->mTiles.push_back(kept);
                        s.mProduct->mTiles.back().mAir.reset();
                        s.mProduct->mTiles.back().mGaps.clear();
                    }
                }
                std::sort(s.mProduct->mTiles.begin(), s.mProduct->mTiles.end(),
                          [](const TileEnclosure& a, const TileEnclosure& b) { return a.mKey < b.mKey; });
                // Nothing reads the graph again.
                std::vector<GraphArc>().swap(s.mArcs);
                std::vector<U32>().swap(s.mStart);
            }
        }
        return s.mStage == STAGE_DONE;
    }

    bool EnclosurePass::done() const
    {
        return mState->mStage == STAGE_DONE;
    }

    F32 EnclosurePass::stageMillis(S32 stage) const
    {
        return stage >= 0 && stage < NUM_STAGES ? static_cast<F32>(mState->mStageMs[stage]) : 0.f;
    }

    const char* EnclosurePass::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "graph", "sky", "depth", "carry", "answer", "coarse" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    EnclosureProductPtr EnclosurePass::product() const
    {
        return done() ? mState->mProduct : nullptr;
    }

    std::shared_ptr<EnclosureProduct> EnclosurePass::takeProduct()
    {
        return done() ? mState->mProduct : nullptr;
    }

    EnclosureProductPtr computeEnclosure(std::shared_ptr<const EnclosureInput> input)
    {
        EnclosurePass pass(std::move(input));
        while (!pass.step(1u << 30))
        {
        }
        return pass.product();
    }
}
