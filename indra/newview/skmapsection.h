/**
 * @file skmapsection.h
 * @brief Census records cut by horizontal planes: the mini-map's walls as a section through the geometry.
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

#ifndef SK_MAPSECTION_H
#define SK_MAPSECTION_H

#include "sknavtypes.h"
#include "v2math.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skmap
{
    constexpr F32 SECTION_CELL_M = 0.125f;          ///< Recast's lattice (sknav::CELL_M)
    constexpr S32 SECTION_LOW_Q = 3, SECTION_HIGH_Q = 11;   ///< planes k+3 and k+11: f+0.4375, f+1.4375
    /// A plane's height: planes sit half a cell off the floor lattice, so SL's 0.5 m stacking never lands on one.
    inline F32 sectionPlaneZ(S32 q) { return (q + 0.5f) * SECTION_CELL_M; }
    inline S32 sectionLevel(F32 floor_z) { return static_cast<S32>(std::lround(floor_z / SECTION_CELL_M)); }
    constexpr F32 SECTION_EPS_CHORD_M = 0.01f;      ///< tessellation chord error
    constexpr F32 SECTION_SNAP_M = 0.001f;          ///< chaining: second pass
    constexpr F32 SECTION_CLOSE_M = 0.02f;          ///< chaining: third pass, nearest first
    constexpr F32 SECTION_MERGE_M = 0.001f;         ///< collinear merge distance
    constexpr F32 SECTION_MERGE_DEG = 0.5f;
    constexpr U32 SECTION_DP_MIN_POINTS = 4096;     ///< Douglas-Peucker only above this
    constexpr F32 SECTION_DP_M = 0.005f;
    constexpr F32 SECTION_ZBIN_M = 0.25f;
    constexpr U32 SECTION_ZBIN_MIN_TRIS = 256;
    constexpr F32 SECTION_HEADROOM_M = 0.25f;       ///< a floor this far under a plane has headroom through it
    constexpr F32 ISLAND_MAX_THICKNESS_M = 1.5f;
    constexpr F32 ISLAND_STEP_M = 0.375f;
    constexpr F32 ISLAND_SHARE = 0.6f;
    constexpr F32 ISLAND_OUT_M = 0.5f;
    constexpr F32 ISLAND_SAMPLE_M = 0.25f;
    constexpr F32 ISLAND_HIGHER_M = 0.125f;         ///< no floor beside an island may stand higher than its top by this
    constexpr F32 ISLAND_EDGE_M = 0.125f;           ///< an island's top must step down across its outline, probed this far either side
    constexpr F32 CLUTTER_AREA_M2 = 0.05f;
    constexpr F32 CLUTTER_EXTENT_M = 0.3f;
    constexpr F32 LEVEL_CELL_M = 1.f;
    constexpr S32 SECTION_REACH_CELLS = 6;          ///< SECTION_REACH_M = 6 m
    constexpr S32 LEVEL_MAX = 4;
    constexpr S32 LEVEL_MERGE_CELLS = 4;            ///< levels within 0.5 m in one cell merge
    constexpr F32 RECORD_BUCKET_M = 8.f;
    constexpr S32 RECORD_LARGE_BUCKETS = 64;        ///< more than 8 x 8 buckets: the large list
    constexpr F32 ORPHAN_STEP_M = 0.5f, ORPHAN_PROBE_M = 0.4f, ORPHAN_REACH_M = 0.45f;
    constexpr F32 DOOR_SLICE_M = 1.f;               ///< a leaf is sliced 1 m over its sill
    constexpr F32 DOOR_SHUT_M = 0.1f, DOOR_HINGE_M = 0.15f;
    constexpr size_t SECTION_CACHE_BYTES = 32u << 20;

    /// A run of points in SliceResult::mPts; closed loops are counter-clockwise.
    struct SliceLoop
    {
        U32 mFirst = 0, mCount = 0;
        S32 mParent = -1;       ///< the smallest loop wholly containing this one
        S8 mWind = 1;           ///< +1 at an even containment depth, -1 at an odd one, 0 when left out
    };

    /// An outer loop and its direct holes: the unit pieces are made of.
    struct SliceGroup
    {
        U16 mOuter = 0;
        std::vector<U16> mHoles;
        F32 mArea = 0.f, mPerimeter = 0.f, mThickness = 0.f;   ///< thickness 2 area / perimeter
        LLVector2 mMin, mMax;
    };

    /// One record cut by one plane, in the record's frame. Immutable once cached.
    struct SliceResult
    {
        std::vector<LLVector2> mPts;
        std::vector<SliceLoop> mLoops;      ///< closed, wound by containment depth
        std::vector<SliceLoop> mLines;      ///< open polylines (mParent, mWind unused)
        std::vector<SliceGroup> mGroups;    ///< per +1 loop: it and its direct -1 children
        bool mSheet = false;                ///< a non-solid soup: its loops may be sheet rooms
        size_t bytes() const;
    };
    typedef std::shared_ptr<const SliceResult> SlicePtr;

    /// Triangle ids per 0.25 m z bin, ascending in each bin.
    struct ZBins
    {
        F32 mZ0 = 0.f;
        std::vector<U32> mStart;
        std::vector<U32> mTris;
    };

    /// Cut a record at height c in its own frame (region-local metres). Half-open: a point with z >= c is above.
    SliceResult sliceRecord(const sknav::Record& rec, F32 c, const ZBins* bins = nullptr);
    ZBins buildZBins(const std::vector<LLVector3>& tris);
    /// Segments for a curve of this semi-major axis to stay within the chord error.
    S32 tessellationCount(F32 semi_major);
    /// centre + cos t v1 + sin t v2, parametrised on its principal axes so the chord bound holds; counter-clockwise.
    void ellipsePolygon(const LLVector2& centre, const LLVector2& v1, const LLVector2& v2, std::vector<LLVector2>& out);
    /// Signed shoelace area, counter-clockwise positive.
    F32 loopArea(const LLVector2* pts, U32 count);
    bool loopContains(const LLVector2* pts, U32 count, const LLVector2& p);
    /// Parents, winds and groups from containment alone; loops not active are left out (wind 0).
    void windByNesting(SliceResult& s, const std::vector<bool>* active = nullptr);

    /// Winding fill of the given loops (their winds as given) and conservative stamping of every edge and line, into a
    /// subsample mask of w x h starting at (x0, y0) in the slice's frame after shift, sub metres a side. Sets bytes to 1.
    void fillCoverage(const SliceResult& s, const std::vector<U16>& loops, const std::vector<S8>& winds, bool lines,
                      const LLVector2& shift, F32 x0, F32 y0, F32 sub, S32 w, S32 h, std::vector<U8>& mask);

    /// Up to LEVEL_MAX levels for one cell from (level, samples) pairs: sort by level; a level joins the current run when
    /// it is at most LEVEL_MERGE_CELLS above the run's lowest level; each run keeps its most sampled level (ties: the
    /// lower) with the run's summed samples; then the LEVEL_MAX runs with the most samples (ties: the lower level)
    /// survive, returned ascending.
    void mergeLevels(std::vector<std::pair<S32, U32>>& samples, std::vector<S32>& out);

    /// Whether a narrow piece stands on its own raised walkable top: thinner than 1.5 m, its top over the floor found
    /// just outside it on most of its perimeter, no floor beside it higher than that top, and no floor running on across
    /// its outline. floor_at gives the topmost floor at a point, false where there is none.
    bool isIsland(const LLVector2* outer, U32 count, F32 thickness, const std::function<bool(const LLVector2&, F32&)>& floor_at);

    /// What the scene builder keeps of its last build for the next; it defines the type.
    struct SectionMemo;

    /// Slices by (signature, shape, plane), with each large soup's z-bins; an upright box or cylinder keeps one section
    /// for every plane well inside its height. Confined to one worker thread, no lock; it holds no record but the last
    /// scene's door leaves, which the map holds as long, so no soup outlives its snapshot by more than a build. Least
    /// recently used out first, by bytes.
    class SectionCache
    {
    public:
        explicit SectionCache(size_t budget = SECTION_CACHE_BYTES);
        SectionCache(const SectionCache&) = delete;
        SectionCache& operator=(const SectionCache&) = delete;
        /// The record's slice at plane q, sliced on a miss.
        SlicePtr get(const sknav::Record& rec, S32 q, bool* hit = nullptr);
        size_t bytes() const { return mBytes; }
        /// Slices held.
        size_t entries() const { return mSlices; }
        /// The last scene build's memo, kept on the same worker; outside the byte budget, as it is one scene's worth.
        std::shared_ptr<SectionMemo>& memo() { return mMemo; }

    private:
        struct Key
        {
            U64 mSignature = 0;
            U8 mShape = 0;
            S32 mPlane = 0;         ///< INT_MIN: the record's z-bins; INT_MIN + 1: an upright prism's section
            bool operator<(const Key& o) const
            {
                return mSignature != o.mSignature ? mSignature < o.mSignature : mShape != o.mShape ? mShape < o.mShape : mPlane < o.mPlane;
            }
            bool operator==(const Key& o) const { return mSignature == o.mSignature && mShape == o.mShape && mPlane == o.mPlane; }
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const
            {
                return static_cast<size_t>(sknav::mix(sknav::mix(k.mSignature, k.mShape), static_cast<U32>(k.mPlane)));
            }
        };
        /// Linked newest to oldest through the entries themselves: the map's nodes never move.
        struct Entry
        {
            SlicePtr mSlice;
            std::shared_ptr<const ZBins> mBins;
            size_t mBytes = 0;
            Key mKey;
            Entry* mNewer = nullptr;
            Entry* mOlder = nullptr;
        };
        void unlink(Entry& e);
        void linkNewest(Entry& e);
        void touch(Entry& e);
        void insert(const Key& key, Entry e);
        void evict(const Key& keep_a, const Key& keep_b);

        size_t mBudget;
        size_t mBytes = 0;
        size_t mSlices = 0;
        std::unordered_map<Key, Entry, KeyHash> mEntries;
        Entry* mNewest = nullptr;
        Entry* mOldest = nullptr;
        std::shared_ptr<SectionMemo> mMemo;
    };

#if AL_BENCH
    /// Benchmark builds only: wall time per stage of a section build, kept while mOn is set.
    enum class SectionStage : U8
    {
        FILTER, LAYER_INDEX, MEMO, MEMO_MATCH, LEVELS, LEVEL_SAMPLES, LEVEL_SPREAD, LEVEL_PACK, PAIRS, PAIR_SORT, PIECES, PIECE_REUSE, SLICE,
        ZBINS, SHAPE, SEGMENTS,
        CHAIN, CHAIN_NODES, CHAIN_SNAP, CHAIN_WALK, CLOSE, SIMPLIFY, NEST, SHEET, ISLAND, HASH, COUNT
    };
    struct SectionProfile
    {
        bool mOn = false;
        F64 mMs[static_cast<size_t>(SectionStage::COUNT)] = {};
        U64 mCalls[static_cast<size_t>(SectionStage::COUNT)] = {};
    };
    inline SectionProfile& sectionProfile()
    {
        static SectionProfile profile;
        return profile;
    }
    class SectionStageTimer
    {
    public:
        explicit SectionStageTimer(SectionStage stage)
            : mStage(stage), mOn(sectionProfile().mOn)
        {
            if (mOn)
            {
                mStart = std::chrono::steady_clock::now();
            }
        }
        ~SectionStageTimer()
        {
            if (mOn)
            {
                SectionProfile& p = sectionProfile();
                p.mMs[static_cast<size_t>(mStage)] += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - mStart).count();
                p.mCalls[static_cast<size_t>(mStage)]++;
            }
        }

    private:
        SectionStage mStage;
        bool mOn;
        std::chrono::steady_clock::time_point mStart;
    };
#define SK_SECTION_STAGE_JOIN(a, b) a##b
#define SK_SECTION_STAGE_NAME(line) SK_SECTION_STAGE_JOIN(sk_section_stage_timer_, line)
#define SK_SECTION_STAGE(stage) skmap::SectionStageTimer SK_SECTION_STAGE_NAME(__LINE__)(skmap::SectionStage::stage)
/// A stage that ends before its scope does.
#define SK_SECTION_STAGE_START(var, stage) std::optional<skmap::SectionStageTimer> var(std::in_place, skmap::SectionStage::stage)
#define SK_SECTION_STAGE_STOP(var) var.reset()
#else
#define SK_SECTION_STAGE(stage) ((void)0)
#define SK_SECTION_STAGE_START(var, stage) ((void)0)
#define SK_SECTION_STAGE_STOP(var) ((void)0)
#endif
}

#endif // SK_MAPSECTION_H
