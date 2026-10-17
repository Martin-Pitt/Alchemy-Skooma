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
#include "sknavprofile.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>
#include <unordered_map>

namespace
{
    using namespace sknav;

    constexpr U32 INF = 0xffffffffu;
    constexpr U32 KEPT = INF - 1;           ///< a carry seed the repair left standing, read from the memo
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
        /// tie(from, to) hears of each that met a distance without lowering it.
        template<typename ADMIT, typename REACHED, typename TIE>
        bool step(std::vector<U32>& dist, const std::vector<U32>& start, const std::vector<GraphArc>& arcs, U8 mask, ADMIT admit, REACHED reached,
                  TIE tie, S64& budget)
        {
            SK_NAV_ZONE("nav.enclosure.flood");
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
                        else if (nd == dist[arc.mTo])
                        {
                            tie(n, arc.mTo);
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
            AirColumn column;
            airColumn(*t->mAir, leaf, column);
            for (U32 k = 0; k < column.mCount; ++k)
            {
                const AirGap& g = t->mAir->mGaps[column.mGap[k]];
                const GapEnclosure& e = t->mGaps[column.mGap[k]];
                const U16 floor = airFloorUnits(column.floorAt(*t->mAir, k, ix, iy));
                Enclosure out;
                fillEnclosure(*t, floor, g.mCeiling, e.mS, e.mE, e.mD, e.mOpen, e.mBucket, e.mSpace, e.mFlags, out);
                spaceId(p, out);
                fn(out, floor, g.mCeiling, *t);
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
        return findLeafByKey(air, ix, iy);
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


    struct SkyMemo
    {
        /// One gap's sky stage results, in the order the stage read them.
        struct Read
        {
            U32 mGap;
            U32 mSeed;          ///< the sky flood's start, NONE when the gap is not under sky
            U32 mWalkSeed;      ///< the depth flood's start from its sky, NONE for none
            U8 mCover;
            U8 mOpen;
        };
        struct Tile
        {
            TileAirPtr mAir;
            U64 mKey = 0;
            std::vector<std::shared_ptr<const void>> mKeep;
            std::vector<Read> mReads;
        };
        static constexpr U32 NONE = 0xffffffffu;
        std::map<TileKey, std::shared_ptr<const Tile>> mTiles;
    };

    struct EnclosureMemo
    {
        /// An edge between tile-local gaps; a seam's mB is in the neighbour.
        struct Edge
        {
            U32 mA;
            U32 mB;
            U8 mCost;
            U8 mFlags;
        };
        typedef std::shared_ptr<const std::vector<Edge>> Edges;
        /// A gap's seeds from neighbouring meshes, kept while they stand.
        struct NeighbourSeed
        {
            U32 mGap;
            U32 mSky;
            U32 mWalk;
        };
        enum Bits : U8
        {
            BIT_SEALED = 1 << 0,        ///< walkable with no way in: its depth is the cap
            BIT_BELOW = 1 << 1,         ///< its depth came from the walkable gap below it
            BIT_SEED_SEALED = 1 << 2,   ///< its depth came from a sealed floor
        };
        /// Per gap, what the floods left.
        struct Values
        {
            std::vector<U32> mDsky;
            std::vector<U32> mDepth;    ///< the final depth, carried and sealed
            std::vector<U32> mSkySeed;
            std::vector<U32> mWalkSeed;
            std::vector<U8> mBits;
            std::vector<NeighbourSeed> mNeighbourSeeds;
        };
        struct Tile
        {
            TileAirPtr mAir;
            Edges mEdges;
            TileAirPtr mSeamAir[2];     ///< the +x and +y neighbours' air its seams were stitched to
            Edges mSeams[2];
            std::shared_ptr<const Values> mValues;
        };
        U64 mKey = 0;                   ///< the params and origin the answers hold for
        U64 mNeighbours = 0;            ///< the neighbouring meshes' products read
        std::map<TileKey, Tile> mTiles;

        size_t bytes() const
        {
            size_t n = sizeof(EnclosureMemo);
            for (const auto& t : mTiles)
            {
                n += sizeof(t);
                n += t.second.mEdges ? t.second.mEdges->capacity() * sizeof(Edge) : 0;
                for (const Edges& s : t.second.mSeams)
                {
                    n += s ? s->capacity() * sizeof(Edge) : 0;
                }
                if (const Values* v = t.second.mValues.get())
                {
                    n += (v->mDsky.capacity() + v->mDepth.capacity() + v->mSkySeed.capacity() + v->mWalkSeed.capacity()) * sizeof(U32) +
                         v->mBits.capacity() + v->mNeighbourSeeds.capacity() * sizeof(NeighbourSeed);
                }
            }
            return n;
        }
    };

    size_t EnclosureProduct::bytes() const
    {
        size_t n = sizeof(EnclosureProduct) + (mMemo ? mMemo->bytes() : 0);
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
        std::vector<U8> mCover;         ///< share of sky roofed over, in 1/255; 255 under the gap's own low ceiling, 0 with no sky map
        std::vector<std::pair<U32, U32>> mSkyWalkSeeds;  ///< walkable gaps under some sky, at the depth their cover starts them
        F64 mSeamMs = 0.0;              ///< seam stitching, when the input times it
        std::vector<U8> mOpen;          ///< side openness, in 1/255; 0 unread

        // Per gap, what the floods start from and what the answers read.
        std::vector<U32> mSkySeed;      ///< the least sky seed, INF for none
        std::vector<U32> mWalkSeed;
        std::vector<U32> mNbSky;        ///< of which from neighbouring meshes
        std::vector<U32> mNbWalk;
        std::vector<U8> mSeedSealed;    ///< its depth came from a sealed floor
        std::vector<U8> mBelow;         ///< its depth came from the walkable gap below it
        bool mCarryAll = true;          ///< the carry flood may reach every gap, not just mInCarry's
        std::vector<U8> mInCarry;
        std::vector<U8> mBelowTiles;    ///< per tile, the step below runs again

        // The repair: the last pass's memo, and what of it still stands.
        U64 mMemoKey = 0;
        U64 mNeighboursKey = 0;
        const EnclosureMemo* mOldMemo = nullptr;
        bool mRepair = false;
        std::vector<const EnclosureMemo::Tile*> mOld;  ///< per tile, its last memo while its air stands, else null
        std::vector<U8> mSeamKept;      ///< per tile and side, its seam taken from the memo
        std::vector<EnclosureMemo::Tile> mNewTiles;     ///< per tile, the memo this pass leaves
        std::vector<U32> mOldDsky;      ///< per gap, INF in a tile whose air is new
        std::vector<U32> mOldDepth;
        std::vector<U32> mOldSkySeed;
        std::vector<U32> mOldWalkSeed;
        std::vector<U8> mOldBits;
        std::vector<U32> mSuspects;     ///< standing gaps a seam to changed air was cut from
        std::vector<U8> mAnswered;      ///< per tile, its answers worked out again
        U32 mAnsweredCount = 0;
        bool mLayoutMoved = true;

        const EnclosureParams& params() const { return mInput->mParams; }
        const TileAir& air(size_t t) const { return *mInput->mTiles[t].mAir; }
        bool isNew(U32 n) const { return !mOld[mNodeTile[n]]; }

        void prepare();
        void graphTile(size_t t);
        void seamSide(size_t t, S32 side);
        void pair(const U32* a, U32 na, const AirGap* ga, S32 oa, const U32* b, U32 nb, const AirGap* gb, S32 ob, U8 cost, std::vector<GraphEdge>& out) const;
        void finishGraph();
        void readOld();
        void neighbourSeeds(std::vector<std::pair<U32, U32>>& sky, std::vector<std::pair<U32, U32>>& walk, const std::vector<U8>& only) const;
        template<typename ADMIT>
        void repairSeeds(std::vector<U32>& dist, const std::vector<U32>& old, const std::vector<U32>& seed, const std::vector<U32>& old_seed, U8 mask,
                         ADMIT admit, std::vector<std::pair<U32, U32>>& seeds);
        void startCarry(std::vector<std::pair<U32, U32>>& seeds);
        void below();
        bool tileMoved(size_t t) const;
        void answerTile(size_t t);
        void coarseTile(size_t t);
        void finishAnswers();
        void leaveMemo();
        bool runStage(S64& budget);
    };

    void EnclosurePass::State::prepare()
    {
        SK_NAV_ZONE("nav.enclosure.prepare");
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
                    if ((g.mFlags & AirGap::TOP) && mCeil[n] - mFloor[n] >= mWalkHead)
                    {
                        mNodeFlags[n] |= NODE_WALK;
                    }
                }
            }
        }
        mProduct->mTiles.resize(in.mTiles.size());

        // What the answers hold for: a memo made under anything else is no use.
        U64 key = SEED;
        for (const F64 v : { static_cast<F64>(p.mSkyDecayM), static_cast<F64>(p.mDepthFullM), static_cast<F64>(p.mClimbM), static_cast<F64>(p.mWalkHeadM),
                             static_cast<F64>(p.mWalkOverlapM), static_cast<F64>(p.mCarryOverlapM), static_cast<F64>(p.mCarryRiseM),
                             static_cast<F64>(p.mSkyReachM), static_cast<F64>(p.mHysteresis), static_cast<F64>(p.mShelterCover),
                             static_cast<F64>(p.mShelterOpen), p.mSealedIndoors ? 1.0 : 0.0, static_cast<F64>(unit), static_cast<F64>(cell),
                             in.mMeshOrigin.mdV[VX], in.mMeshOrigin.mdV[VY], in.mMeshOrigin.mdV[VZ] })
        {
            key = mixF64(key, v);
        }
        mMemoKey = key;
        U64 neighbours = SEED;
        for (const EnclosureInput::Neighbour& nb : in.mNeighbours)
        {
            neighbours = mix(neighbours, reinterpret_cast<uintptr_t>(nb.mProduct.get()));
            neighbours = mix(neighbours, nb.mProduct ? nb.mProduct->mGeneration : 0);
            for (S32 k = 0; k < 3; ++k)
            {
                neighbours = mixF64(neighbours, nb.mMeshOrigin.mdV[k]);
            }
        }
        mNeighboursKey = neighbours;
        mOld.assign(in.mTiles.size(), nullptr);
        mNewTiles.resize(in.mTiles.size());
        mSeamKept.assign(in.mTiles.size() * 2, 0);
        mOldMemo = in.mPrevious && in.mPrevious->mMemo && in.mPrevious->mMemo->mKey == key ? in.mPrevious->mMemo.get() : nullptr;
        if (mOldMemo)
        {
            for (size_t t = 0; t < in.mTiles.size(); ++t)
            {
                const auto it = mOldMemo->mTiles.find(in.mTiles[t].mKey);
                const TileEnclosure* prev = in.mPrevious->tile(in.mTiles[t].mKey);
                // Only air whose last answers are still in the product: the hysteresis reads them.
                if (it != mOldMemo->mTiles.end() && it->second.mAir == in.mTiles[t].mAir && it->second.mValues && prev && prev->mAir == in.mTiles[t].mAir &&
                    prev->mGaps.size() == air(t).mGaps.size())
                {
                    mOld[t] = &it->second;
                    mRepair = true;
                }
            }
        }
    }

    void EnclosurePass::State::pair(const U32* node_a, U32 na, const AirGap* ga, S32 oa, const U32* node_b, U32 nb, const AirGap* gb, S32 ob,
                                    U8 cost, std::vector<GraphEdge>& out) const
    {
        static_assert((2 << AIR_MAX_LEVEL) <= 0xff, "two leaves' widths fit an edge's cost");
        // Both lists run lowest first and their gaps are disjoint, so the
        // overlapping pairs fall out of one sweep.
        U32 i = 0, j = 0;
        while (i < na && j < nb)
        {
            const S32 fa = ga[i].mFloor + oa, ca = ceilingUnits(ga[i].mCeiling, oa);
            const S32 fb = gb[j].mFloor + ob, cb = ceilingUnits(gb[j].mCeiling, ob);
            const S32 overlap = std::min(ca, cb) - std::max(fa, fb);
            if (overlap >= 1)
            {
                const U32 a = node_a[i], b = node_b[j];
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
                // No cost given: half the two leaves' widths each, from the leaves that hold the gaps.
                const U8 c = cost ? cost
                                  : static_cast<U8>((1 << air(mNodeTile[a]).mLeaves[mNodeLeaf[a]].mLevel) +
                                                    (1 << air(mNodeTile[b]).mLeaves[mNodeLeaf[b]].mLevel));
                out.push_back({ a, b, c, flags });
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
        SK_NAV_ZONE("nav.enclosure.graph");
        const TileAir& a = air(t);
        const S32 w = a.mWidth, h = a.mHeight;
        // Leaves are stored coarsest first over a cell, so each cell ends up with its finest.
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
        U32 ids_a[0xff], ids_b[0xff];
        AirGap gaps_a[0xff], gaps_b[0xff];
        std::vector<S32> met;
        for (size_t l = 0; l < a.mLeaves.size(); ++l)
        {
            const AirLeaf& leaf = a.mLeaves[l];
            const S32 s = 1 << leaf.mLevel;
            for (U32 i = 0; i < leaf.mGapCount; ++i)
            {
                ids_a[i] = base + leaf.mFirstGap + i;
            }
            // The +x and +y sides only: every touching pair is met once. A leaf over both cells is air they share, not a neighbour.
            for (S32 side = 0; side < 2; ++side)
            {
                met.clear();
                for (S32 k = 0; k < s; ++k)
                {
                    const S32 x = side == 0 ? leaf.mX + s : leaf.mX + k;
                    const S32 y = side == 0 ? leaf.mY + k : leaf.mY + s;
                    if (x >= w || y >= h)
                    {
                        break;
                    }
                    for (S32 n = mLeafMap[x + y * w]; n >= 0; n = parentLeaf(a, n))
                    {
                        const AirLeaf& other = a.mLeaves[n];
                        const S32 os = 1 << other.mLevel;
                        if (leaf.mX >= other.mX && leaf.mY >= other.mY && leaf.mX < other.mX + os && leaf.mY < other.mY + os)
                        {
                            break;
                        }
                        if (std::find(met.begin(), met.end(), n) != met.end())
                        {
                            continue;
                        }
                        met.push_back(n);
                        // Tilted floors are compared where the leaves meet: the middle of their shared run.
                        const S32 lo = side == 0 ? std::max(leaf.mY, other.mY) : std::max(leaf.mX, other.mX);
                        const S32 hi = side == 0 ? std::min(leaf.mY + s, other.mY + os) : std::min(leaf.mX + s, other.mX + os);
                        const S32 mid = (lo + hi - 1) / 2;
                        const S32 ax = side == 0 ? x - 1 : mid, ay = side == 0 ? mid : y - 1;
                        const S32 bx = side == 0 ? x : mid, by = side == 0 ? mid : y;
                        for (U32 j = 0; j < leaf.mGapCount; ++j)
                        {
                            gaps_a[j] = a.mGaps[leaf.mFirstGap + j];
                            gaps_a[j].mFloor = airFloorUnits(airFloorAt(gaps_a[j], leaf, ax, ay));
                        }
                        for (U32 j = 0; j < other.mGapCount; ++j)
                        {
                            ids_b[j] = base + other.mFirstGap + j;
                            gaps_b[j] = a.mGaps[other.mFirstGap + j];
                            gaps_b[j].mFloor = airFloorUnits(airFloorAt(gaps_b[j], other, bx, by));
                        }
                        const U8 cost = static_cast<U8>(s + os);
                        pair(ids_a, leaf.mGapCount, gaps_a, off, ids_b, other.mGapCount, gaps_b, off, cost, mEdges);
                    }
                }
            }
        }
    }

    void EnclosurePass::State::seamSide(size_t t, S32 side)
    {
        SK_NAV_ZONE("nav.enclosure.seam");
        // Across the +x or +y seam, gap by gap along the neighbours' level-0
        // strips: the strip's k-th gap is the column's k-th. The key only
        // finds the leaves; a cell whose counts disagree is skipped.
        const EnclosureInput& in = *mInput;
        const TileAir& a = air(t);
        std::vector<GraphEdge> edges;
        AirColumn ca, cb;
        U32 ids_a[0xff], ids_b[0xff];
        {
            TileKey nk = in.mTiles[t].mKey;
            (side == 0 ? nk.mX : nk.mY) += 1;
            const auto it = mTileIndex.find(nk);
            if (it == mTileIndex.end())
            {
                return;
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
                const S32 la = side == 0 ? findLeafByKey(a, a.mWidth - 1, i) : findLeafByKey(a, i, a.mHeight - 1);
                const S32 lb = side == 0 ? findLeafByKey(b, 0, j) : findLeafByKey(b, j, 0);
                if (la < 0 || lb < 0)
                {
                    continue;
                }
                airColumn(a, la, ca);
                airColumn(b, lb, cb);
                if (ca.mCount != sa.count(i) || cb.mCount != sb.count(j))
                {
                    continue;
                }
                for (U32 k = 0; k < ca.mCount; ++k)
                {
                    ids_a[k] = mBase[t] + ca.mGap[k];
                }
                for (U32 k = 0; k < cb.mCount; ++k)
                {
                    ids_b[k] = mBase[u] + cb.mGap[k];
                }
                pair(ids_a, sa.count(i), &sa.mGaps[sa.mStart[i]], mZOffset[t], ids_b, sb.count(j), &sb.mGaps[sb.mStart[j]], mZOffset[u], 0, edges);
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
        SK_NAV_ZONE("nav.enclosure.finish");
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

    void EnclosurePass::State::readOld()
    {
        SK_NAV_ZONE("nav.enclosure.old");
        const EnclosureInput& in = *mInput;
        const size_t n = mNodeFlags.size();
        mOldDsky.assign(n, INF);
        mOldDepth.assign(n, INF);
        mOldSkySeed.assign(n, INF);
        mOldWalkSeed.assign(n, INF);
        mOldBits.assign(n, 0);
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            if (!mOld[t])
            {
                continue;
            }
            const EnclosureMemo::Values& v = *mOld[t]->mValues;
            std::copy(v.mDsky.begin(), v.mDsky.end(), mOldDsky.begin() + mBase[t]);
            std::copy(v.mDepth.begin(), v.mDepth.end(), mOldDepth.begin() + mBase[t]);
            std::copy(v.mSkySeed.begin(), v.mSkySeed.end(), mOldSkySeed.begin() + mBase[t]);
            std::copy(v.mWalkSeed.begin(), v.mWalkSeed.end(), mOldWalkSeed.begin() + mBase[t]);
            std::copy(v.mBits.begin(), v.mBits.end(), mOldBits.begin() + mBase[t]);
        }
        // A standing gap at the end of a seam the last pass stitched to air that has since changed or gone lost that edge.
        auto standing = [this](const TileKey& key, size_t& t) {
            const auto it = mTileIndex.find(key);
            if (it == mTileIndex.end() || !mOld[it->second])
            {
                return false;
            }
            t = it->second;
            return true;
        };
        for (const auto& entry : mOldMemo->mTiles)
        {
            for (S32 side = 0; side < 2; ++side)
            {
                const EnclosureMemo::Edges& edges = entry.second.mSeams[side];
                if (!edges || edges->empty())
                {
                    continue;
                }
                TileKey nk = entry.first;
                (side == 0 ? nk.mX : nk.mY) += 1;
                size_t a = 0, b = 0;
                const bool keep_a = standing(entry.first, a), keep_b = standing(nk, b);
                if (keep_a && keep_b && mSeamKept[a * 2 + side])
                {
                    continue;
                }
                for (const EnclosureMemo::Edge& e : *edges)
                {
                    if (keep_a)
                    {
                        mSuspects.push_back(mBase[a] + e.mA);
                    }
                    if (keep_b)
                    {
                        mSuspects.push_back(mBase[b] + e.mB);
                    }
                }
            }
        }
        std::sort(mSuspects.begin(), mSuspects.end());
        mSuspects.erase(std::unique(mSuspects.begin(), mSuspects.end()), mSuspects.end());
    }

    template<typename ADMIT>
    void EnclosurePass::State::repairSeeds(std::vector<U32>& dist, const std::vector<U32>& old, const std::vector<U32>& seed, const std::vector<U32>& old_seed,
                                           U8 mask, ADMIT admit, std::vector<std::pair<U32, U32>>& seeds)
    {
        SK_NAV_ZONE("nav.enclosure.repair");
        // Distances still held by a standing path stand; the rest are lost, in
        // order of distance so every gap is judged after all that could hold it.
        const U32 n_nodes = static_cast<U32>(dist.size());
        dist = old;
        typedef std::pair<U32, U32> Item;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        for (const U32 n : mSuspects)
        {
            if (dist[n] != INF)
            {
                queue.emplace(dist[n], n);
            }
        }
        for (U32 n = 0; n < n_nodes; ++n)
        {
            if (dist[n] != INF && seed[n] > old_seed[n])
            {
                queue.emplace(dist[n], n);
            }
        }
        std::vector<U32> lost;
        while (!queue.empty())
        {
            const Item top = queue.top();
            queue.pop();
            const U32 d = top.first, n = top.second;
            if (dist[n] != d || seed[n] <= d)
            {
                continue;
            }
            bool held = false;
            for (U32 a = mStart[n]; a < mStart[n + 1] && !held; ++a)
            {
                const GraphArc& arc = mArcs[a];
                held = (arc.mFlags & mask) && dist[arc.mTo] != INF && dist[arc.mTo] + arc.mCost == d && admit(arc.mTo, n);
            }
            if (held)
            {
                continue;
            }
            dist[n] = INF;
            lost.push_back(n);
            for (U32 a = mStart[n]; a < mStart[n + 1]; ++a)
            {
                const GraphArc& arc = mArcs[a];
                if ((arc.mFlags & mask) && dist[arc.mTo] != INF && dist[arc.mTo] == d + arc.mCost && admit(n, arc.mTo))
                {
                    queue.emplace(dist[arc.mTo], arc.mTo);
                }
            }
        }
        // Then the flood runs on from what stands into what was lost or is new, and from every seed that came closer.
        std::vector<U8> queued(n_nodes, 0);
        auto open = [&](U32 v) {
            if (seed[v] != INF)
            {
                seeds.emplace_back(seed[v], v);
            }
            for (U32 a = mStart[v]; a < mStart[v + 1]; ++a)
            {
                const GraphArc& arc = mArcs[a];
                const U32 u = arc.mTo;
                if ((arc.mFlags & mask) && dist[u] != INF && !queued[u] && admit(u, v))
                {
                    queued[u] = 1;
                    seeds.emplace_back(dist[u], u);
                }
            }
        };
        for (const U32 v : lost)
        {
            open(v);
        }
        for (U32 v = 0; v < n_nodes; ++v)
        {
            if (isNew(v))
            {
                open(v);
            }
            else if (seed[v] < dist[v])
            {
                seeds.emplace_back(seed[v], v);
            }
        }
    }

    void EnclosurePass::State::neighbourSeeds(std::vector<std::pair<U32, U32>>& sky, std::vector<std::pair<U32, U32>>& walk, const std::vector<U8>& only) const
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
            if (!only.empty() && !only[t])
            {
                continue;
            }
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
                    S32 mIx, mIy;   ///< the leaf's cell the probe leaves from, where a tilted floor is read
                };
                Probe probes[5];
                S32 count = 0;
                const S32 mx = leaf.mX + (s - 1) / 2, my = leaf.mY + (s - 1) / 2;
                probes[count++] = { cx, cy, 0, -1, -1 };
                const F64 reach = (s * 0.5 + 0.5) * cell;
                if (open_side[0] && leaf.mX == 0) probes[count++] = { cx - reach, cy, static_cast<U32>(s + 1), leaf.mX, my };
                if (open_side[1] && leaf.mX + s >= a.mWidth) probes[count++] = { cx + reach, cy, static_cast<U32>(s + 1), leaf.mX + s - 1, my };
                if (open_side[2] && leaf.mY == 0) probes[count++] = { cx, cy - reach, static_cast<U32>(s + 1), mx, leaf.mY };
                if (open_side[3] && leaf.mY + s >= a.mHeight) probes[count++] = { cx, cy + reach, static_cast<U32>(s + 1), mx, leaf.mY + s - 1 };
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
                                const S32 at = probes[k].mIx < 0 ? mFloor[n] - mZOffset[t]
                                                                 : airFloorAt(a.mGaps[leaf.mFirstGap + i], leaf, probes[k].mIx, probes[k].mIy);
                                const F64 floor = base_z + at * a.mUnit;
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

    void EnclosurePass::State::startCarry(std::vector<std::pair<U32, U32>>& seeds)
    {
        SK_NAV_ZONE("nav.enclosure.carry");
        // Unwalkable air takes the walkable depth beside it plus the way there.
        const U32 n_nodes = static_cast<U32>(mNodeFlags.size());
        mCarrySeed.assign(n_nodes, INF);
        mSeedSealed.assign(n_nodes, 0);
        mBelow.assign(n_nodes, 0);
        // A floor with no walkable way from outside is as deep as depth goes.
        const U32 deep = static_cast<U32>(std::lround(ENCLOSURE_D_DEEP * ENCLOSURE_D_UNIT_M / mHalfCellM));
        for (U32 n = 0; n < n_nodes; ++n)
        {
            if (params().mSealedIndoors && (mNodeFlags[n] & NODE_WALK) && mDwalk[n] == INF)
            {
                mDwalk[n] = deep;
                mNodeFlags[n] |= NODE_SEALED;
            }
            if ((mNodeFlags[n] & NODE_WALK) && mDwalk[n] != INF)
            {
                mCarrySeed[n] = n;
            }
        }
        if (!mRepair)
        {
            mCarryAll = true;
            for (U32 n = 0; n < n_nodes; ++n)
            {
                if (mCarrySeed[n] == n)
                {
                    seeds.emplace_back(mDwalk[n], n);
                }
            }
            return;
        }
        // Carried depth moves only in the unwalkable air joined to what changed:
        // new air, air cut from a seam, or air beside a floor whose depth moved.
        mCarryAll = false;
        mInCarry.assign(n_nodes, 0);
        std::vector<U32> stack;
        auto add = [&](U32 v) {
            if (!(mNodeFlags[v] & NODE_WALK) && !mInCarry[v])
            {
                mInCarry[v] = 1;
                stack.push_back(v);
            }
        };
        auto addBeside = [&](U32 v) {
            for (U32 a = mStart[v]; a < mStart[v + 1]; ++a)
            {
                if (mArcs[a].mFlags & EDGE_CARRY)
                {
                    add(mArcs[a].mTo);
                }
            }
        };
        mBelowTiles.assign(mInput->mTiles.size(), 0);
        for (U32 n = 0; n < n_nodes; ++n)
        {
            const bool walk = (mNodeFlags[n] & NODE_WALK) != 0;
            if (isNew(n))
            {
                walk ? addBeside(n) : add(n);
            }
            else if (walk && (mDwalk[n] != mOldDepth[n] || ((mNodeFlags[n] & NODE_SEALED) != 0) != ((mOldBits[n] & EnclosureMemo::BIT_SEALED) != 0)))
            {
                addBeside(n);
                mBelowTiles[mNodeTile[n]] = 1;
            }
        }
        for (const U32 n : mSuspects)
        {
            add(n);
        }
        while (!stack.empty())
        {
            const U32 v = stack.back();
            stack.pop_back();
            mBelowTiles[mNodeTile[v]] = 1;
            addBeside(v);
        }
        std::vector<U8> queued(n_nodes, 0);
        for (U32 n = 0; n < n_nodes; ++n)
        {
            if (mNodeFlags[n] & NODE_WALK)
            {
                continue;
            }
            if (mInCarry[n])
            {
                // Every floor beside it floods again, into it alone.
                for (U32 a = mStart[n]; a < mStart[n + 1]; ++a)
                {
                    const U32 u = mArcs[a].mTo;
                    if ((mArcs[a].mFlags & EDGE_CARRY) && mCarrySeed[u] == u && !queued[u])
                    {
                        queued[u] = 1;
                        seeds.emplace_back(mDwalk[u], u);
                    }
                }
                continue;
            }
            // The rest stand as they were; what came from below is the step below's to say.
            if (!(mOldBits[n] & EnclosureMemo::BIT_BELOW))
            {
                mDwalk[n] = mOldDepth[n];
                mCarrySeed[n] = KEPT;
                mSeedSealed[n] = (mOldBits[n] & EnclosureMemo::BIT_SEED_SEALED) ? 1 : 0;
            }
        }
    }

    void EnclosurePass::State::below()
    {
        // Else the walkable gap straight below it, through no more than a climb of solid, in any column under it.
        const EnclosureInput& in = *mInput;
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            if (mRepair && mOld[t] && !mBelowTiles[t])
            {
                // Nothing under it moved: what it took from below stands.
                for (U32 n = mBase[t]; n < mBase[t + 1]; ++n)
                {
                    if (!(mNodeFlags[n] & NODE_WALK) && (mOldBits[n] & EnclosureMemo::BIT_BELOW))
                    {
                        mDwalk[n] = mOldDepth[n];
                        mCarrySeed[n] = KEPT;
                        mSeedSealed[n] = (mOldBits[n] & EnclosureMemo::BIT_SEED_SEALED) ? 1 : 0;
                        mBelow[n] = 1;
                    }
                }
                continue;
            }
            SK_NAV_ZONE("nav.enclosure.below");
            const TileAir& a = air(t);
            AirColumn column;
            for (const AirLeaf& leaf : a.mLeaves)
            {
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const U32 g = leaf.mFirstGap + i, n = mBase[t] + g;
                    if ((mNodeFlags[n] & NODE_WALK) || mDwalk[n] != INF)
                    {
                        continue;
                    }
                    const S32 s = 1 << leaf.mLevel;
                    for (S32 y = leaf.mY; y < std::min(a.mHeight, leaf.mY + s); ++y)
                    {
                        for (S32 x = leaf.mX; x < std::min(a.mWidth, leaf.mX + s); ++x)
                        {
                            airColumn(a, findLeafByKey(a, x, y), column);
                            for (U32 k = 1; k < column.mCount; ++k)
                            {
                                if (column.mGap[k] != g)
                                {
                                    continue;
                                }
                                const U32 under = mBase[t] + column.mGap[k - 1];
                                if ((mNodeFlags[under] & NODE_WALK) && mDwalk[under] < mDwalk[n] && mFloor[n] - mCeil[under] <= mClimb)
                                {
                                    mDwalk[n] = mDwalk[under];
                                    mCarrySeed[n] = mCarrySeed[under];
                                    mBelow[n] = 1;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        for (U32 n = 0; n < mNodeFlags.size(); ++n)
        {
            const U32 seed = mCarrySeed[n];
            if (seed != KEPT)
            {
                mSeedSealed[n] = seed != INF && (mNodeFlags[seed] & NODE_SEALED) ? 1 : 0;
            }
        }
    }

    void EnclosurePass::State::answerTile(size_t t)
    {
        SK_NAV_ZONE("nav.enclosure.answer");
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
            for (U32 i = 0; i < leaf.mGapCount; ++i)
            {
                const U32 g = leaf.mFirstGap + i;
                const U32 n = mBase[t] + g;
                const F32 s = mDsky[n] == INF ? 0.f : std::exp(-(mDsky[n] * mHalfCellM) / p.mSkyDecayM);
                GapEnclosure& e = out.mGaps[g];
                e.mS = quantise(s);
                e.mCover = mCover[n];
                e.mOpen = mOpen[n];
                if (mDwalk[n] == INF)
                {
                    e.mD = ENCLOSURE_D_UNKNOWN;
                    e.mE = 0;
                    e.mBucket = static_cast<U8>(EnclosureBucket::UNKNOWN);
                    continue;
                }
                const F32 d = mDwalk[n] * mHalfCellM;
                const F32 ev = enclosureE(s, d, p.mDepthFullM);
                e.mD = static_cast<U8>(std::min<S32>(ENCLOSURE_D_DEEP, std::lround(d / ENCLOSURE_D_UNIT_M)));
                e.mE = quantise(ev);
                if (mSeedSealed[n])
                {
                    e.mD = ENCLOSURE_D_DEEP;
                    e.mFlags = ENCLOSURE_SEALED;
                    e.mBucket = static_cast<U8>(EnclosureBucket::INDOORS);
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
        SK_NAV_ZONE("nav.enclosure.coarse");
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
                    const S32 mx = std::max<S32>(leaf.mX, cx * c.mCells) + (ox - 1) / 2, my = std::max<S32>(leaf.mY, cy * c.mCells) + (oy - 1) / 2;
                    for (U32 i = 0; i < leaf.mGapCount; ++i)
                    {
                        const U32 g = leaf.mFirstGap + i;
                        parts.push_back({ static_cast<U32>(cx + cy * c.mWidth), airFloorUnits(airFloorAt(a.mGaps[g], leaf, mx, my)), g, static_cast<F32>(ox * oy) });
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
                floor = std::min(floor, parts[j].mFloor);
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
    }

    bool EnclosurePass::State::tileMoved(size_t t) const
    {
        // Answers are a function of these and the last bucket, which holds where they do.
        const TileEnclosure* prev = mInput->mPrevious->tile(mInput->mTiles[t].mKey);
        const U32 base = mBase[t], count = mBase[t + 1] - mBase[t];
        for (U32 g = 0; g < count; ++g)
        {
            const U32 n = base + g;
            if (mDsky[n] != mOldDsky[n] || mDwalk[n] != mOldDepth[n] || mSeedSealed[n] != ((mOldBits[n] & EnclosureMemo::BIT_SEED_SEALED) != 0) ||
                mCover[n] != prev->mGaps[g].mCover || mOpen[n] != prev->mGaps[g].mOpen)
            {
                return true;
            }
        }
        return false;
    }

    void EnclosurePass::State::leaveMemo()
    {
        SK_NAV_ZONE("nav.enclosure.memo");
        const EnclosureInput& in = *mInput;
        auto memo = std::make_shared<EnclosureMemo>();
        memo->mKey = mMemoKey;
        memo->mNeighbours = mNeighboursKey;
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            EnclosureMemo::Tile& tile = mNewTiles[t];
            const U32 base = mBase[t], count = mBase[t + 1] - mBase[t];
            auto bits = [this](U32 n) {
                return static_cast<U8>(((mNodeFlags[n] & NODE_SEALED) ? EnclosureMemo::BIT_SEALED : 0) | (mBelow[n] ? EnclosureMemo::BIT_BELOW : 0) |
                                       (mSeedSealed[n] ? EnclosureMemo::BIT_SEED_SEALED : 0));
            };
            std::vector<EnclosureMemo::NeighbourSeed> nb;
            for (U32 g = 0; g < count; ++g)
            {
                if (mNbSky[base + g] != INF || mNbWalk[base + g] != INF)
                {
                    nb.push_back({ g, mNbSky[base + g], mNbWalk[base + g] });
                }
            }
            // Values that all stand are shared with the last memo rather than copied.
            bool same = mOld[t] != nullptr;
            if (same)
            {
                const EnclosureMemo::Values& v = *mOld[t]->mValues;
                same = v.mNeighbourSeeds.size() == nb.size() &&
                       std::equal(nb.begin(), nb.end(), v.mNeighbourSeeds.begin(), [](const EnclosureMemo::NeighbourSeed& x, const EnclosureMemo::NeighbourSeed& y) {
                           return x.mGap == y.mGap && x.mSky == y.mSky && x.mWalk == y.mWalk;
                       });
                for (U32 g = 0; g < count && same; ++g)
                {
                    const U32 n = base + g;
                    same = mDsky[n] == v.mDsky[g] && mDwalk[n] == v.mDepth[g] && mSkySeed[n] == v.mSkySeed[g] && mWalkSeed[n] == v.mWalkSeed[g] &&
                           bits(n) == v.mBits[g];
                }
            }
            if (same)
            {
                tile.mValues = mOld[t]->mValues;
            }
            else
            {
                auto v = std::make_shared<EnclosureMemo::Values>();
                v->mDsky.assign(mDsky.begin() + base, mDsky.begin() + base + count);
                v->mDepth.assign(mDwalk.begin() + base, mDwalk.begin() + base + count);
                v->mSkySeed.assign(mSkySeed.begin() + base, mSkySeed.begin() + base + count);
                v->mWalkSeed.assign(mWalkSeed.begin() + base, mWalkSeed.begin() + base + count);
                v->mBits.resize(count);
                for (U32 g = 0; g < count; ++g)
                {
                    v->mBits[g] = bits(base + g);
                }
                v->mNeighbourSeeds = std::move(nb);
                tile.mValues = std::move(v);
            }
            memo->mTiles.emplace(in.mTiles[t].mKey, std::move(tile));
        }
        std::vector<EnclosureMemo::Tile>().swap(mNewTiles);
        mProduct->mMemo = memo;

        // Whether the spaces cut from the last answers still stand: the same air, and every gap's bucket and flags.
        mLayoutMoved = !mRepair;
        if (mRepair)
        {
            size_t bare = 0;
            for (const TileEnclosure& prev : in.mPrevious->mTiles)
            {
                bare += prev.mAir ? 0 : 1;
            }
            mLayoutMoved = in.mPrevious->mTiles.size() - bare != in.mTiles.size() || bare != in.mKept.size();
            for (size_t t = 0; t < in.mTiles.size() && !mLayoutMoved; ++t)
            {
                if (!mOld[t])
                {
                    mLayoutMoved = true;
                    break;
                }
                if (!mAnswered[t])
                {
                    continue;
                }
                const TileEnclosure& now = mProduct->mTiles[t];
                const TileEnclosure& prev = *in.mPrevious->tile(now.mKey);
                for (size_t g = 0; g < now.mGaps.size() && !mLayoutMoved; ++g)
                {
                    mLayoutMoved = now.mGaps[g].mBucket != prev.mGaps[g].mBucket || now.mGaps[g].mFlags != prev.mGaps[g].mFlags;
                }
                mLayoutMoved = mLayoutMoved || now.mCoarse.mGaps.size() != prev.mCoarse.mGaps.size();
                for (size_t g = 0; g < now.mCoarse.mGaps.size() && !mLayoutMoved; ++g)
                {
                    mLayoutMoved = now.mCoarse.mGaps[g].mBucket != prev.mCoarse.mGaps[g].mBucket || now.mCoarse.mGaps[g].mFlags != prev.mCoarse.mGaps[g].mFlags;
                }
            }
        }
    }

    void EnclosurePass::State::finishAnswers()
    {
        // In tile and gap order, as the sums always ran.
        const EnclosureInput& in = *mInput;
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            const TileAir& a = air(t);
            const TileEnclosure& out = mProduct->mTiles[t];
            for (const AirLeaf& leaf : a.mLeaves)
            {
                const F64 area = static_cast<F64>(1 << leaf.mLevel) * (1 << leaf.mLevel) * a.mCell * a.mCell;
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    const GapEnclosure& e = out.mGaps[leaf.mFirstGap + i];
                    ++mProduct->mGapCount;
                    mProduct->mArea += area;
                    if (e.mD == ENCLOSURE_D_UNKNOWN)
                    {
                        ++mProduct->mUnknownGaps;
                        mProduct->mUnknownArea += area;
                    }
                    else if (e.mFlags & ENCLOSURE_SEALED)
                    {
                        ++mProduct->mSealedGaps;
                    }
                }
            }
        }
        U64 h = 0;
        for (size_t t = 0; t < in.mTiles.size(); ++t)
        {
            const TileEnclosure& out = mProduct->mTiles[t];
            h = mix(h ? h : SEED, static_cast<U64>(static_cast<U32>(out.mKey.mX)) << 32 | static_cast<U32>(out.mKey.mY));
            for (const CoarseGap& g : out.mCoarse.mGaps)
            {
                h = mix(h, (g.mS >> 4) | ((g.mD == ENCLOSURE_D_UNKNOWN ? 0xff : g.mD >> 3) << 8));
            }
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
                    const size_t t = mCursor;
                    EnclosureMemo::Tile& memo = mNewTiles[t];
                    memo.mAir = in.mTiles[t].mAir;
                    // Edges in tile-local gaps, from the memo or made here and kept for the next pass.
                    auto reuse = [this](const EnclosureMemo::Edges& edges, U32 base_a, U32 base_b) {
                        for (const EnclosureMemo::Edge& e : *edges)
                        {
                            mEdges.push_back({ base_a + e.mA, base_b + e.mB, e.mCost, e.mFlags });
                        }
                    };
                    auto keep = [this](size_t from, U32 base_a, U32 base_b) {
                        auto edges = std::make_shared<std::vector<EnclosureMemo::Edge>>();
                        edges->reserve(mEdges.size() - from);
                        for (size_t k = from; k < mEdges.size(); ++k)
                        {
                            edges->push_back({ mEdges[k].mA - base_a, mEdges[k].mB - base_b, mEdges[k].mCost, mEdges[k].mFlags });
                        }
                        return EnclosureMemo::Edges(std::move(edges));
                    };
                    if (mOld[t] && mOld[t]->mEdges)
                    {
                        memo.mEdges = mOld[t]->mEdges;
                        reuse(memo.mEdges, mBase[t], mBase[t]);
                        budget -= static_cast<S64>(memo.mEdges->size() / 8);
                    }
                    else
                    {
                        const size_t from = mEdges.size();
                        graphTile(t);
                        memo.mEdges = keep(from, mBase[t], mBase[t]);
                        budget -= static_cast<S64>(air(t).mGaps.size()) * 2;
                    }
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
                        memo.mSeamAir[side] = in.mTiles[u].mAir;
                        if (mOld[t] && mOld[u] && mOld[t]->mSeams[side] && mOld[t]->mSeamAir[side] == in.mTiles[u].mAir)
                        {
                            memo.mSeams[side] = mOld[t]->mSeams[side];
                            reuse(memo.mSeams[side], mBase[t], mBase[u]);
                            mSeamKept[t * 2 + side] = 1;
                            continue;
                        }
                        const size_t from = mEdges.size();
                        if (in.mTimeSeams)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            seamSide(t, side);
                            mSeamMs += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        }
                        else
                        {
                            seamSide(t, side);
                        }
                        memo.mSeams[side] = keep(from, mBase[t], mBase[u]);
                    }
                    ++mCursor;
                }
                if (mCursor < tiles)
                {
                    return false;
                }
                finishGraph();
                if (mRepair)
                {
                    readOld();
                }
                break;
            case STAGE_SKY:
                if (!mStarted)
                {
                    SK_NAV_ZONE("nav.enclosure.sky");
                    const EnclosureParams& p = params();
                    std::vector<std::pair<U32, U32>> seeds;
                    // Per 2 m square of the tile, the covers read there, by whole metre of height.
                    struct Read
                    {
                        S32 mZ;
                        F32 mCover, mRoof, mOpen;
                    };
                    std::vector<std::vector<Read>> covers;
                    std::vector<U32> rows;
                    mCover.assign(mNodeFlags.size(), in.mSky ? 255 : 0);
                    mOpen.assign(mNodeFlags.size(), 0);
                    // One dense window over the tiles that read the sky and what stands within reach of them, made once one has to.
                    const bool has_sky = in.mSky && tiles;
                    std::unique_ptr<SkyView> view;
                    // Outside it the view reads the map itself, so any window gives the same reads.
                    auto openView = [&](const std::vector<std::shared_ptr<const SkyMemo::Tile>>& kept) {
                        F64 lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
                        for (size_t t = 0; t < tiles; ++t)
                        {
                            if (kept[t])
                            {
                                continue;
                            }
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
                    };
                    // A tile reads the sky within this of its plan: the cover's radius or the side openness's reach, and the cell a read lands in.
                    const F64 sky_reach = std::max<F64>(p.mSkyReachM, SIDE_OPEN_REACH_M) + 1.0;
                    const SkyMemo* old_memo = in.mPrevious ? in.mPrevious->mSkyMemo.get() : nullptr;
                    auto memo = std::make_shared<SkyMemo>();
                    // Which tiles' reads stand, first, so the window need only cover the rest.
                    std::vector<std::shared_ptr<SkyMemo::Tile>> fresh_tiles(tiles);
                    std::vector<std::shared_ptr<const SkyMemo::Tile>> kept_tiles(tiles);
                    for (size_t t = 0; t < tiles; ++t)
                    {
                        const TileAir& a = air(t);
                        auto fresh = std::make_shared<SkyMemo::Tile>();
                        fresh->mAir = in.mTiles[t].mAir;
                        if (has_sky)
                        {
                            const F64 x0 = in.mMeshOrigin.mdV[VX] + a.mOrigin.mV[VX], y0 = in.mMeshOrigin.mdV[VY] + a.mOrigin.mV[VY];
                            fresh->mKey = in.mSky->windowKey(x0 - sky_reach, y0 - sky_reach, x0 + a.mWidth * a.mCell + sky_reach,
                                                             y0 + a.mHeight * a.mCell + sky_reach, fresh->mKeep);
                            for (const F64 v : { x0, y0, in.mMeshOrigin.mdV[VZ], static_cast<F64>(p.mSkyReachM), static_cast<F64>(p.mSkyDecayM),
                                                 static_cast<F64>(p.mDepthFullM), static_cast<F64>(mHalfCellM), static_cast<F64>(mWalkHead) })
                            {
                                U64 bits;
                                std::memcpy(&bits, &v, sizeof(bits));
                                fresh->mKey = (fresh->mKey ^ bits) * 0x100000001b3ull;
                            }
                        }
                        if (old_memo && has_sky)
                        {
                            const auto it = old_memo->mTiles.find(in.mTiles[t].mKey);
                            if (it != old_memo->mTiles.end() && it->second->mAir == fresh->mAir && it->second->mKey == fresh->mKey)
                            {
                                kept_tiles[t] = it->second;
                            }
                        }
                        fresh_tiles[t] = std::move(fresh);
                    }
                    for (size_t t = 0; t < tiles; ++t)
                    {
                        const TileAir& a = air(t);
                        const std::shared_ptr<SkyMemo::Tile>& fresh = fresh_tiles[t];
                        const std::shared_ptr<const SkyMemo::Tile>& kept = kept_tiles[t];
                        if (kept)
                        {
                            // Its air and every block of sky it reads stand: the same reads, without reading.
                            for (const SkyMemo::Read& r : kept->mReads)
                            {
                                const U32 n = mBase[t] + r.mGap;
                                mCover[n] = r.mCover;
                                mOpen[n] = r.mOpen;
                                if (r.mSeed != SkyMemo::NONE)
                                {
                                    mNodeFlags[n] |= NODE_SKY;
                                    seeds.emplace_back(r.mSeed, n);
                                }
                                if (r.mWalkSeed != SkyMemo::NONE)
                                {
                                    mSkyWalkSeeds.emplace_back(r.mWalkSeed, n);
                                }
                            }
                            memo->mTiles.emplace(in.mTiles[t].mKey, kept);
                            continue;
                        }
                        if (has_sky && !view)
                        {
                            openView(kept_tiles);
                        }
                        const auto keep = [&](U32 n) {
                            const U32 g = n - mBase[t];
                            const bool sky = (mNodeFlags[n] & NODE_SKY) != 0;
                            const bool walk = sky && !mSkyWalkSeeds.empty() && mSkyWalkSeeds.back().second == n;
                            fresh->mReads.push_back({ g, sky ? seeds.back().first : SkyMemo::NONE, walk ? mSkyWalkSeeds.back().first : SkyMemo::NONE,
                                                      mCover[n], mOpen[n] });
                        };
                        const S32 side = static_cast<S32>(std::ceil(a.mWidth * a.mCell * 0.5)) + 1;
                        covers.assign(static_cast<size_t>(side) * (static_cast<S32>(std::ceil(a.mHeight * a.mCell * 0.5)) + 1), {});
                        // In row order, so the leaf whose read speaks for its square does not depend on how leaves are stored.
                        rows.resize(a.mLeaves.size());
                        for (U32 l = 0; l < rows.size(); ++l)
                        {
                            rows[l] = l;
                        }
                        std::sort(rows.begin(), rows.end(), [&a](U32 i, U32 j) {
                            const AirLeaf &li = a.mLeaves[i], &lj = a.mLeaves[j];
                            return li.mY < lj.mY || (li.mY == lj.mY && (li.mX < lj.mX || (li.mX == lj.mX && li.mLevel > lj.mLevel)));
                        });
                        for (const U32 l : rows)
                        {
                            const AirLeaf& leaf = a.mLeaves[l];
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
                                    keep(n);
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
                                    keep(n);
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
                                keep(n);
                            }
                        }
                        if (has_sky)
                        {
                            memo->mTiles.emplace(in.mTiles[t].mKey, std::shared_ptr<const SkyMemo::Tile>(fresh));
                        }
                    }
                    if (has_sky)
                    {
                        mProduct->mSkyMemo = memo;
                    }
                    // Each gap's least seed: the floods read nothing else of them.
                    const size_t n_nodes = mNodeFlags.size();
                    mSkySeed.assign(n_nodes, INF);
                    mWalkSeed.assign(n_nodes, INF);
                    for (const auto& s : seeds)
                    {
                        mSkySeed[s.second] = std::min(mSkySeed[s.second], s.first);
                    }
                    for (const auto& s : mSkyWalkSeeds)
                    {
                        mWalkSeed[s.second] = std::min(mWalkSeed[s.second], s.first);
                    }
                    std::vector<std::pair<U32, U32>>().swap(seeds);
                    std::vector<std::pair<U32, U32>>().swap(mSkyWalkSeeds);
                    // Neighbouring meshes' seeds stand for a tile whose air, sides and neighbours all do.
                    std::vector<U8> fresh(tiles, 1);
                    if (mRepair && mOldMemo->mNeighbours == mNeighboursKey)
                    {
                        for (size_t t = 0; t < tiles; ++t)
                        {
                            bool stands = mOld[t] != nullptr;
                            for (S32 side = 0; side < 4 && stands; ++side)
                            {
                                TileKey nk = in.mTiles[t].mKey;
                                (side < 2 ? nk.mX : nk.mY) += (side & 1) ? 1 : -1;
                                stands = (mTileIndex.find(nk) != mTileIndex.end()) == (mOldMemo->mTiles.find(nk) != mOldMemo->mTiles.end());
                            }
                            fresh[t] = stands ? 0 : 1;
                        }
                    }
                    std::vector<std::pair<U32, U32>> nb_sky, nb_walk;
                    neighbourSeeds(nb_sky, nb_walk, fresh);
                    mNbSky.assign(n_nodes, INF);
                    mNbWalk.assign(n_nodes, INF);
                    for (const auto& s : nb_sky)
                    {
                        mNbSky[s.second] = std::min(mNbSky[s.second], s.first);
                    }
                    for (const auto& s : nb_walk)
                    {
                        mNbWalk[s.second] = std::min(mNbWalk[s.second], s.first);
                    }
                    for (size_t t = 0; t < tiles; ++t)
                    {
                        if (!fresh[t])
                        {
                            for (const EnclosureMemo::NeighbourSeed& ns : mOld[t]->mValues->mNeighbourSeeds)
                            {
                                mNbSky[mBase[t] + ns.mGap] = ns.mSky;
                                mNbWalk[mBase[t] + ns.mGap] = ns.mWalk;
                            }
                        }
                    }
                    for (size_t n = 0; n < n_nodes; ++n)
                    {
                        mSkySeed[n] = std::min(mSkySeed[n], mNbSky[n]);
                        mWalkSeed[n] = std::min(mWalkSeed[n], mNbWalk[n]);
                    }
                    std::vector<std::pair<U32, U32>> start;
                    if (mRepair)
                    {
                        repairSeeds(mDsky, mOldDsky, mSkySeed, mOldSkySeed, EDGE_SKY, [](U32, U32) { return true; }, start);
                    }
                    else
                    {
                        for (U32 n = 0; n < n_nodes; ++n)
                        {
                            if (mSkySeed[n] != INF)
                            {
                                start.emplace_back(mSkySeed[n], n);
                            }
                        }
                    }
                    mFlood.begin(std::move(start));
                    mStarted = true;
                }
                if (!mFlood.step(mDsky, mStart, mArcs, EDGE_SKY, [](U32, U32) { return true; }, [](U32, U32) {}, [](U32, U32) {}, budget))
                {
                    return false;
                }
                break;
            case STAGE_DEPTH:
            {
                const auto walkable = [this](U32, U32 n) { return (mNodeFlags[n] & NODE_WALK) != 0; };
                if (!mStarted)
                {
                    const U32 n_nodes = static_cast<U32>(mNodeFlags.size());
                    std::vector<std::pair<U32, U32>> start;
                    if (mRepair)
                    {
                        // The flood's own depths, before sealing: a sealed floor had none.
                        std::vector<U32> old(n_nodes, INF);
                        for (U32 n = 0; n < n_nodes; ++n)
                        {
                            if ((mNodeFlags[n] & NODE_WALK) && !(mOldBits[n] & EnclosureMemo::BIT_SEALED))
                            {
                                old[n] = mOldDepth[n];
                            }
                        }
                        repairSeeds(mDwalk, old, mWalkSeed, mOldWalkSeed, EDGE_WALK, walkable, start);
                    }
                    else
                    {
                        for (U32 n = 0; n < n_nodes; ++n)
                        {
                            if (mWalkSeed[n] != INF)
                            {
                                start.emplace_back(mWalkSeed[n], n);
                            }
                        }
                    }
                    mFlood.begin(std::move(start));
                    mStarted = true;
                }
                if (!mFlood.step(mDwalk, mStart, mArcs, EDGE_WALK, walkable, [](U32, U32) {}, [](U32, U32) {}, budget))
                {
                    return false;
                }
                break;
            }
            case STAGE_CARRY:
                if (!mStarted)
                {
                    std::vector<std::pair<U32, U32>> seeds;
                    startCarry(seeds);
                    mFlood.begin(std::move(seeds));
                    mStarted = true;
                }
                if (!mFlood.step(mDwalk, mStart, mArcs, EDGE_CARRY,
                                 [this](U32 from, U32 to) {
                                     // Not up a facade or a shaft: only so far from the floor it started on.
                                     return !(mNodeFlags[to] & NODE_WALK) && (mCarryAll || mInCarry[to]) &&
                                            std::abs(mFloor[to] - mFloor[mCarrySeed[from]]) <= mCarryRise;
                                 },
                                 [this](U32 from, U32 to) { mCarrySeed[to] = mCarrySeed[from]; },
                                 // Of equal depths the lowest floor's id, so the answer is the same whatever order the flood met them in.
                                 [this](U32 from, U32 to) { mCarrySeed[to] = std::min(mCarrySeed[to], mCarrySeed[from]); }, budget))
                {
                    return false;
                }
                below();
                break;
            case STAGE_ANSWER:
                if (!mStarted)
                {
                    // A standing tile none of whose gaps' inputs moved keeps its last answers, coarse and all.
                    mAnswered.assign(tiles, 1);
                    for (size_t t = 0; t < tiles && mRepair; ++t)
                    {
                        mAnswered[t] = !mOld[t] || tileMoved(t);
                    }
                    budget -= static_cast<S64>(mNodeFlags.size() / 8);
                    mStarted = true;
                }
                while (mCursor < tiles && budget > 0)
                {
                    const size_t t = mCursor;
                    if (mAnswered[t])
                    {
                        answerTile(t);
                        ++mAnsweredCount;
                        budget -= static_cast<S64>(air(t).mGaps.size());
                    }
                    else
                    {
                        mProduct->mTiles[t] = *in.mPrevious->tile(in.mTiles[t].mKey);
                        budget -= static_cast<S64>(air(t).mGaps.size() / 16);
                    }
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
                    if (mAnswered[mCursor])
                    {
                        coarseTile(mCursor);
                        budget -= static_cast<S64>(air(mCursor).mGaps.size());
                    }
                    ++mCursor;
                }
                if (mCursor < tiles)
                {
                    return false;
                }
                finishAnswers();
                leaveMemo();
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
        SK_NAV_ZONE("nav.enclosure.step");
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

    F32 EnclosurePass::seamMillis() const
    {
        return static_cast<F32>(mState->mSeamMs);
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

    bool EnclosurePass::repaired() const
    {
        return mState->mRepair;
    }

    U32 EnclosurePass::tilesAnswered() const
    {
        return mState->mAnsweredCount;
    }

    bool EnclosurePass::layoutMoved() const
    {
        return mState->mLayoutMoved;
    }

    void carrySpaces(EnclosureProduct& product, const EnclosureProduct& previous)
    {
        for (TileEnclosure& t : product.mTiles)
        {
            const TileEnclosure* prev = previous.tile(t.mKey);
            if (!prev || prev->mAir != t.mAir)
            {
                continue;
            }
            if (prev->mGaps.size() == t.mGaps.size())
            {
                for (size_t g = 0; g < t.mGaps.size(); ++g)
                {
                    t.mGaps[g].mSpace = prev->mGaps[g].mSpace;
                }
            }
            if (prev->mCoarse.mGaps.size() == t.mCoarse.mGaps.size())
            {
                for (size_t g = 0; g < t.mCoarse.mGaps.size(); ++g)
                {
                    t.mCoarse.mGaps[g].mSpace = prev->mCoarse.mGaps[g].mSpace;
                }
            }
        }
        product.mSpaceIds = previous.mSpaceIds;
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
