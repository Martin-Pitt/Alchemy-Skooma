/**
 * @file skoverviewcensus.h
 * @brief The census's physical shapes as vertex buffers, a range per map class, for the overview.
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

#ifndef SK_OVERVIEWCENSUS_H
#define SK_OVERVIEWCENSUS_H

#include "algpubuffer.h"
#include "llpointer.h"
#include "lluuid.h"
#include "skoverviewcuttable.h"
#include "skoverviewprobes.h"
#include "skoverviewslots.h"
#include "threadpool_fwd.h"
#include "v3dmath.h"
#include "v3math.h"

#include <future>
#include <memory>
#include <string>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLVertexBuffer;
class LLViewerRegion;

namespace sknav
{
    struct Record;
}

namespace skoverview
{
    /// The class code each overview layer writes into the capture's R channel; overviewResolveF.glsl reads the same.
    enum Code : U8
    {
        CODE_NONE = 0,
        CODE_TERRAIN = 255,     ///< the region's land: its shadow program writes white
        CODE_FLOOR = 16,        ///< census: walkable slope, facing up or down
        CODE_WALL = 17,         ///< census: steep faces of parts taller than cover
        CODE_COVER = 18,        ///< census: steep faces of parts no taller than COVER_TOP_M
        CODE_RAMP = 19,         ///< census: between floor and wall
        CODE_DOOR = 20,
        CODE_LADDER = 21,
        CODE_VEHICLE = 22,
        CODE_SECTION = 23,      ///< census: a solid the cut went through
        CODE_PLACEHOLDER = 24,  ///< census: a box the region's object cache knows of, not yet the object
        CODE_MOVER = 25,        ///< a physical or moving object at its live pose, drawn whole
        CODE_VEH_CIVIL = 26,    ///< a vehicle at its live pose, the mini-map's civilian hull
        CODE_VEH_ALLY = 27,     ///< an allied vehicle
        CODE_VEH_NEUTRAL = 28,  ///< a vehicle neither civilian nor allied
    };

    /// Whether anyone sees a part's faces; UNKNOWN while its object, a face or an image is not loaded.
    enum class Seen : U8 { VISIBLE, INVISIBLE, UNKNOWN };

    /// The census classes, in vertex buffer order.
    enum CensusClass : U8 { FLOOR, WALL, COVER, RAMP, DOOR, LADDER, VEHICLE, PLACEHOLDER, INVISIBLE, NUM_CENSUS_CLASSES };

    constexpr F32 FLOOR_NZ = 0.866f;    ///< steeper than 30 degrees is no longer floor
    constexpr F32 WALL_NZ = 0.5f;       ///< steeper than 60 degrees is wall
    constexpr F32 COVER_TOP_M = 1.6f;   ///< a part no taller than this is cover, not wall
    constexpr F32 INVISIBLE_WALL_M = 64.f; ///< a barrier, or an invisible part not yet judged, larger than this on any axis is no wall
    constexpr F32 GROUND_BASE_M = 4.f;  ///< a cluster whose base is no higher than this above the land stands on it
    constexpr F32 NO_CUT = 1.0e30f;     ///< the cut height of a linkset drawn whole
    constexpr F32 HIDDEN = -1.0e30f;    ///< the cut height of a linkset not drawn at all
    constexpr U8 TRI_PASS_THROUGH = 1;  ///< a triangle's flag: an invisible part's, which the cut's probes pass through
    constexpr U8 TRI_OPEN = 2;          ///< a triangle's flag: its part has open edges, so has no inside to section

    U8 codeOf(CensusClass cls);

    /// The default transparent texture or an old invisiprim texture: nobody sees a face drawn with one.
    bool transparentTexture(const LLUUID& id);

    /// Whether the overview draws a record at all, and the class of its parts that is not per triangle (door, ladder,
    /// vehicle); NUM_CENSUS_CLASSES when each triangle is classed by its slope.
    bool keepRecord(const sknav::Record& rec, CensusClass& fixed);

    /// A triangle's class from its unit normal's z and its part's height.
    CensusClass classifyTriangle(F32 nz, F32 part_height);
}

/// The census's shapes on the GPU: per region and class, pages of vertex runs, one run a linkset, uploaded only for
/// the linksets a snapshot changed. Main thread only.
class SKOverviewCensus
{
public:
    /// One linkset's triangles in one region, by class, as the worker emitted them: immutable once built, shared by the
    /// worker's cache and what draws it.
    struct LinksetGeom
    {
        U64 mKey = 0;           ///< what it was emitted from: an equal key emits the same triangles
        LLUUID mRoot;
        LLVector3d mOrigin;     ///< its region's origin, global; triangles are region-local
        std::vector<LLVector3> mClass[skoverview::NUM_CENSUS_CLASSES];
        std::vector<U8> mFlags[skoverview::NUM_CENSUS_CLASSES]; ///< per triangle: TRI_PASS_THROUGH, TRI_OPEN
        LLVector3 mMin{ 1e30f, 1e30f, 1e30f };  ///< region-local bounds of its triangles
        LLVector3 mMax{ -1e30f, -1e30f, -1e30f };
        /// Its triangles by where they came from, the hatch copies in the invisible class.
        U64 mRecordTris = 0;
        U64 mSculptTris = 0;
        U64 mSurroundTris = 0;
        U64 mHatchTris = 0;
        /// What the cover test made of its invisible parts.
        struct Cover
        {
            U32 mParts = 0;
            U32 mProxies = 0;
            U32 mExtra = 0;
            U32 mBarriers = 0;
            U32 mUnknown = 0;
            U32 mTooFine = 0;
            U32 mDropped = 0;
            F32 mHatchedM2 = 0.f;
        };
        Cover mCover;
        std::vector<std::pair<LLUUID, U64>> mJudged; ///< its invisible parts and the verdict keys it was emitted with
        U64 vertices() const;
    };

    /// A linkset within a region: its runs in the region's pages, and the heights its cluster spans.
    struct Group
    {
        static constexpr U32 NO_PAGE = 0xFFFFFFFFu;
        Group();
        F32 mMinZ = 0.f;        ///< global z, which is agent z too: its cluster's, so a level keeps whole clusters
        F32 mMaxZ = 0.f;
        bool mGround = false;   ///< its cluster's base is on the land under it
        U32 mId = 0;            ///< its linkset's id, stable while its root stays and the same in every region it spans
        LLUUID mRoot;           ///< the linkset's root
        LLVector3d mAnchor;     ///< global; the linkset root's position, which says whose region it is in
        bool mNeverCut = false; ///< a surround in the void: always whole
        LLVector3d mMin;        ///< global bounds of its own triangles in this region
        LLVector3d mMax;
        U32 mPage[skoverview::NUM_CENSUS_CLASSES]; ///< per class, its page in the region's, NO_PAGE when it has none
        U32 mStart[skoverview::NUM_CENSUS_CLASSES] = {}; ///< per class, its run in that page
        U32 mCount[skoverview::NUM_CENSUS_CLASSES] = {};
        /// Its triangles and their flags, region-local: what its runs are uploaded from when they are placed again.
        std::shared_ptr<const LinksetGeom> mGeom;
    };

    /// One class's vertices in one region: a buffer of fixed capacity holding a run per linkset.
    struct Page
    {
        U8 mClass = 0;
        LLPointer<LLVertexBuffer> mBuffer;
        skoverview::SlotAllocator mRuns;
        /// Its runs in vertex order, as the command builders read them; refreshed with the runs and the heights.
        std::vector<skoverview::SlotCull> mSlots;
        /// The compute command list's slot table and commands, uploaded when mSlots moves.
        mutable ALGPUBuffer mSlotTable;
        mutable ALGPUBuffer mCommands;
        mutable bool mTableDirty = true;
    };

    /// A region's pages, and the linksets in it.
    struct Region
    {
        LLVector3d mOrigin;     ///< global; vertices are region-local
        std::vector<Page> mPages;   ///< by class
        std::vector<Group> mGroups;
    };

    /// Which groups a shot draws: all of them, or the ground's (mGround) and those overlapping [mLo, mHi], as the
    /// command builders keep their slots.
    struct Level : skoverview::LevelCull
    {
        using LevelCull::keeps;
        bool keeps(const Group& group) const { return keeps(group.mMinZ, group.mMaxZ, group.mGround); }
    };
    /// The top of what a level keeps, at least floor; for a shot's near plane.
    F32 levelTop(const Level& level, F32 floor) const;

    SKOverviewCensus();
    ~SKOverviewCensus();

    /// Rebuilds the buffers when the census's geometry changed; true when there is anything to draw.
    bool update();

    /// The air's room someone is in, its mask grown by a cell every way so a wall just past its last cell touches it.
    struct RoomFootprint
    {
        LLUUID mMesh;           ///< the room's mesh and its id there, as SKNavWorldView::RoomId holds them
        U32 mId = 0;
        U64 mHash = 0;          ///< the room's: moves only where its air moved
        LLVector3d mOrigin;     ///< global, the minimum corner of cell (0, 0)
        S32 mW = 0;
        S32 mH = 0;
        std::vector<U8> mMask;  ///< row-major mW x mH; empty for a room too large to keep one, whose box is covered
        bool covers(S32 x, S32 y) const;
    };

    /// Someone the tactical cut is taken for: the building whose parts' bounds hold their floor is cut at mCut.
    struct Occupant
    {
        LLUUID mId;             ///< who, for the log
        bool mSelf = false;     ///< the agent: what is cut for them is cut at their level, whoever else needs it
        LLVector3d mFloor;      ///< global, their feet on their floor
        LLVector3d mFeet;       ///< global, their feet as the avatar stands, for the surface they stand on
        F32 mCut = 0.f;         ///< agent z
        /// The room they are in, on mRoomCellM cells; null outside any, where their column is cut instead.
        std::shared_ptr<const RoomFootprint> mRoom;
        F32 mRoomCellM = 0.5f;
        bool mOutdoors = false; ///< in outdoor air by the enclosure: no room or column is cut for them
        F32 mPriority = 0.f;    ///< among others sharing a building, the highest priority's storey is cut for
    };
    /// Writes each linkset's cut height into the cut table for the tactical view, for the linksets in the given region:
    /// - room: every linkset bounding an occupant's enclosed room (walls round its floor, floors above, the roof);
    /// - none for an occupant in outdoor air (on a roof, in the street);
    /// - column: with no room known, every linkset whose bounds hold their column and rise above it;
    /// - on a roof: a linkset someone stands on with none of it over them is cut no lower than their level, nor hidden,
    ///   unless the agent is under it; held a second after they leave it.
    /// What hides someone from the tactical eye is not cut here: its shader dissolves it round them.
    /// The agent's are cut at the agent's level, whoever else needs them; another's at the level of its
    /// occupant of highest mPriority. Every other part, there and elsewhere, is drawn whole. key says when the eye or the people moved:
    /// the same key and buffers do nothing. Publishes the table only when the result changed, and logs it then;
    /// cutStamp() moves when it does.
    /// The GPU answers which linksets stand round, over or under each person (probes, which may be null): an answer is
    /// read a frame or two after it is asked for, and what stood before is kept until it lands.
    void applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, U64 key, SKOverviewProbes* probes);
    U64 cutStamp() const { return mCutStamp; }
    /// A linkset's cut at the last applyCut, by its root: its height (HIDDEN when hidden) and why; false when whole.
    bool cutOf(const LLUUID& root, F32& height, std::string& why) const;
    const std::vector<Region>& regions() const { return mRegions; }
    U32 triangles() const { return mTriangles; }
    /// Parts that keep changing (a scripted door, a target on a track), left out of the static buffers for the movers
    /// to draw live: each change would otherwise rebuild every region's buffers.
    std::vector<LLUUID> restlessParts() const;
    /// Moves whenever the buffers are rebuilt.
    U64 signature() const { return mSignature; }
    /// Every linkset's cut by its id, which each vertex carries in its weight channel; for the tactical view's draws.
    SKOverviewCutTable& cutTable() { return mCutTable; }
    void release();

private:
    /// A part drawn from its rendered geometry rather than a census record.
    struct RenderedPart
    {
        LLUUID mPart;
        LLVector3d mOrigin;             ///< its region's origin, global; triangles are region-local
        LLVector3d mAnchor;             ///< its linkset root's position, global
        LLUUID mRoot;
        std::shared_ptr<const std::vector<LLVector3>> mTris; ///< shared with the cache it came from; never null
        F32 mHeight = 0.f;
        skoverview::CensusClass mFixed = skoverview::NUM_CENSUS_CLASSES;
        bool mSurround = false;         ///< a surround (gatherSurrounds), not a near sculpt
        U64 mKey = 0;                   ///< what its triangles were worked from: its volume, map and placement
    };
    void gatherSurrounds(const struct SKCensusSnapshot& snap);
    /// What decides an invisible part's verdict: the census records near it whose look is visible.
    struct CoverInput
    {
        bool mTest = false;     ///< an invisible part, judged by the cover test
        bool mPending = false;  ///< a neighbour's look is not known yet: no hatch yet
        U64 mKey = 0;           ///< its candidates' signatures, kinds and looks
        std::vector<const sknav::Record*> mCandidates; ///< owned by the snapshot the job holds
    };
    /// The pass's records near an invisible part's bounds (global) that it may stand in for or add to.
    CoverInput coverInputFor(const LLUUID& part, const LLVector3d& lo, const LLVector3d& hi, const struct SKCensusSnapshot& snap);
    /// The pass's cover candidates (sknav::coverPoolRecord, not restless) by index into the snapshot's records, filed
    /// by COVER_CELL_M cell in plan, with their looks.
    std::unordered_map<U64, std::vector<U32>> mCoverGrid;
    std::vector<U32> mCoverLarge;                   ///< those spanning too many cells to file
    std::vector<skoverview::Seen> mCoverSeen;      ///< per snapshot record; UNKNOWN when not a candidate
    static constexpr F64 COVER_CELL_M = 16.0;
    /// The worker's verdicts kept across builds by part; each build reads the last landed map and makes the next.
    struct Verdict
    {
        U64 mKey = 0;           ///< the part's geometry and its CoverInput::mKey
        U8 mVerdict = 0;        ///< sknav::CoverVerdict
        bool mTooFine = false;
        F32 mOutsideM2 = 0.f;   ///< what sticks out past the colliding neighbours
        F32 mPartM2 = 0.f;
        std::shared_ptr<const std::vector<LLVector3>> mProtruding; ///< EXTRA: what sticks out, region-local triangles
    };
    struct Verdicts
    {
        std::unordered_map<LLUUID, Verdict> mByPart;
    };
    std::shared_ptr<const Verdicts> mVerdicts = std::make_shared<const Verdicts>();
    /// The worker's linksets kept across builds by root and region, as the verdicts are: each build reads the last
    /// landed map and makes the next, re-emitting only the linksets whose key moved.
    struct Cache
    {
        std::map<std::tuple<LLUUID, S64, S64>, std::shared_ptr<const LinksetGeom>> mByLinkset;
    };
    std::shared_ptr<const Cache> mCache = std::make_shared<const Cache>();
    /// What a build needs, as values: the worker never touches viewer objects, the world, settings or GL.
    struct Job
    {
        std::shared_ptr<const struct SKCensusSnapshot> mSnapshot; ///< owns the records and clusters below
        U64 mSignature = 0;
        U32 mGeneration = 0;
        std::vector<const sknav::Record*> mKept;
        std::vector<U8> mFixed;
        std::vector<bool> mVoidOnly;
        std::vector<RenderedPart> mSculpts;
        std::vector<CoverInput> mCover; ///< per kept record then sculpt
        std::shared_ptr<const Verdicts> mVerdicts;
        std::shared_ptr<const Cache> mCache;
        std::vector<U32> mGroupOf;      ///< per kept record then sculpt, its linkset's group
        std::vector<LLUUID> mRoots;     ///< per group
        std::vector<U32> mIds;          ///< per group, its linkset's stable id
        std::map<U32, U32> mGroupCluster;
        std::vector<std::pair<LLVector3d, bool>> mAnchors; ///< per group: its root's position, and void-only
        std::vector<std::pair<LLVector3d, F64>> mRegions;  ///< connected regions' origins and widths
    };
    /// A build's linksets per region, short of their runs and of whether each stands on the land.
    struct Built
    {
        struct Linkset
        {
            Group mGroup;       ///< all but its runs; mGeom set
            std::pair<LLVector3d, LLVector3d> mFootprint; ///< the box the land under it is read over
        };
        /// What the cover test made of the invisible parts, for the log.
        struct CoverStats
        {
            U32 mParts = 0;
            U32 mProxies = 0;
            U32 mExtra = 0;
            U32 mBarriers = 0;
            U32 mUnknown = 0;   ///< a neighbour's type or look not known yet, or too fine
            U32 mTooFine = 0;
            U32 mDropped = 0;   ///< parts other than proxies over INVISIBLE_WALL_M
            U32 mJudged = 0;    ///< worked this build, the rest from the kept verdicts
            F32 mHatchedM2 = 0.f; ///< what sticks out of the extra parts, and the barriers whole
            F64 mVerdictSeconds = 0.0; ///< in the verdicts of the judged parts
            F64 mBuildSeconds = 0.0;   ///< in the whole per-part loop
        };
        CoverStats mCover;
        /// Where the build's triangles came from, for the upload log.
        struct Sources
        {
            U64 mRecords = 0;
            U64 mSculpts = 0;
            U64 mSurrounds = 0;
            U64 mHatch = 0;     ///< the hatch copies in the invisible range
            std::vector<std::pair<LLUUID, U64>> mLargest; ///< the ten linksets of most triangles, by root
        };
        Sources mSources;
        std::shared_ptr<const Verdicts> mVerdicts; ///< the verdicts this build asked for, adopted when it lands
        std::shared_ptr<const Cache> mCache;       ///< the linksets this build drew, adopted when it lands
        U32 mLinksetsEmitted = 0;                  ///< of its linksets, those emitted again rather than kept
        U64 mSignature = 0;
        U32 mGeneration = 0;
        std::vector<Linkset> mLinksets;
        std::vector<LLUUID> mRoots;     ///< every root the build gave an id
        /// Linksets left out for a run of one class over MAX_PAGE_VERTICES: their roots and vertices.
        std::vector<std::pair<LLUUID, U64>> mOverflow;
    };
    static std::shared_ptr<Built> buildLinksets(const Job& job);
    /// Lands a build in this frame: frees the runs of linksets gone or changed, and uploads the changed ones' runs.
    void swapIn(Built& built);
    /// A page of the region's class with room for count vertices, made when none has, sized for the want vertices the
    /// build still has to place there; its index, and the run's first vertex.
    U32 pageFor(Region& region, U8 cls, U32 count, U32 want, U32& first);
    static constexpr U32 MIN_PAGE_VERTICES = 3u * 8192u;   ///< a new page's least, so a class's spills stay few
    /// Rebuilds the region's pages' slot lists from its groups.
    void refreshSlots(Region& region);
    std::unordered_map<LLUUID, U32> mLinksetIds;    ///< each linkset root's id while it is in a landed or pending build
    skoverview::IdPool mIds;
    std::unique_ptr<LL::ThreadPool> mWorkers;
    std::future<std::shared_ptr<Built>> mJob;
    U32 mGeneration = 0;    ///< moves at release(), so a build begun before it never lands
    bool mBuilt = false;    ///< a build has landed since the last release()
    /// A part's look, kept per part while its object and faces' inputs stay the same.
    skoverview::Seen seenOf(const LLUUID& part);
public:
    /// A part's look as the last pass found it: 0 visible, 1 invisible, 2 not known yet, 255 not asked; for exports.
    U8 lastSeen(const LLUUID& part) const;
private:
    /// Drops the verdicts of parts no pass asked about since the last prune.
    void pruneSeen();
    struct SeenEntry
    {
        U64 mKey = 0;
        U32 mPass = 0;
        skoverview::Seen mSeen = skoverview::Seen::UNKNOWN;
    };
    static constexpr U32 SEEN_REVERIFY_PASSES = 32; ///< each kept verdict is worked again once in this many passes
    std::unordered_map<LLUUID, SeenEntry> mSeen;
    std::unordered_map<LLUUID, U32> mSeenUnknown; ///< the parts whose look was not known yet, by the pass that asked
    U32 mSeenPass = 0;
    /// A sculpt's region-local triangles, kept while its volume, map and placement stay the same.
    struct SculptEntry
    {
        U64 mKey = 0;
        U32 mPass = 0;
        bool mValid = false;    ///< it had geometry
        std::shared_ptr<const std::vector<LLVector3>> mTris;
        F32 mHeight = 0.f;
    };
    std::unordered_map<LLUUID, SculptEntry> mSculptGeom;
    std::vector<LLUUID> mSculptCandidates;  ///< the sculpted volumes the last full object list walk found
    /// Logs which signature entries came and went since the last rebuild; sorts entries and keeps them.
    /// and turns parts that moved twice within the window restless, from their shape and place alone (moves).
    /// Returns how many record and linkset entries (R, C) of settled parts moved.
    U32 logChurn(std::vector<std::pair<U64, std::pair<char, LLUUID>>>& entries,
                  std::vector<std::pair<U64, std::pair<char, LLUUID>>>& moves);
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> mLastEntries;
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> mLastMoves;
    static constexpr F64 RESTLESS_WINDOW_S = 20.0;  ///< a second change within this turns a part restless
    static constexpr F64 RESTLESS_SETTLE_S = 30.0;  ///< a restless part unchanged this long rejoins the census
    struct Restless
    {
        F64 mLast = 0.0;        ///< when it last changed
        U32 mChanges = 0;
        bool mOn = false;
        bool mSurround = false; ///< a void surround: kept in the buffers, only its changes ignored
        U64 mHash = 0;          ///< its shape and place when last seen, to tell when it holds still
    };
    std::map<LLUUID, Restless> mRestless;
    bool isRestless(const LLUUID& part) const;

    std::vector<Region> mRegions;
    std::vector<RenderedPart> mSurrounds;
    U64 mSurroundsSig = 0;
    F64 mSurroundsAt = 0.0;     ///< when the surrounds are looked at again
    U64 mStamp = 0;
    U64 mSignature = 0;
    bool mUnknownLook = false;  ///< some part's faces or images were not loaded at the last build
    std::unordered_map<LLUUID, skoverview::Seen> mLargeLogged; ///< the large parts of the last pass and how each was logged
    std::string mCoverLogged;   ///< the cover counts last logged
    std::string mUploadLogged;  ///< the upload's source totals last logged
    std::vector<std::pair<LLUUID, U64>> mOverflowLogged; ///< the overflowing linksets last warned about
    U32 mPagesMade = 0;         ///< pages made and freed at the last swapIn, for the log
    U32 mPagesFreed = 0;
    F64 mRecheckAt = 0.0;
    U64 mRecordsKey = 0;        ///< the census records' identities at the last look
    bool mRecordsChanged = true;
    std::unordered_set<LLUUID> mClusterRoots; ///< the linksets the last build bounds by their cluster's heights
    F64 mSculptsAt = 0.0;       ///< when the live sculpts are looked at again
    std::shared_ptr<const struct SKCensusSnapshot> mSnapshot;
    U32 mTriangles = 0;
    std::vector<std::pair<U32, F32>> mCuts; ///< the clusters and parts cut at the last applyCut, and their heights
    U64 mCutSignature = 0;      ///< the buffers' signature the cut attribute was written for
    U64 mCutStamp = 0;
    U64 mCutKey = 0;            ///< the inputs the last applyCut was worked from
    std::string mCutLogged;     ///< the cut linksets and reasons last logged
    std::map<LLUUID, std::pair<F32, std::string>> mCutRoots; ///< the last applyCut's cuts by linkset root
    SKOverviewCutTable mCutTable; ///< the last applyCut's cuts by linkset id, on the GPU
    struct Standing
    {
        LLUUID mRoot;           ///< the linkset stood on
        F64 mZ = 0.0;           ///< the surface's height under the feet
        F64 mAt = 0.0;          ///< when it was last seen under them
    };
    std::map<LLUUID, Standing> mStanding;   ///< who stands on a roof, held through a hop
    /// Why a linkset is cut, put into words only when logged or asked for.
    struct Reason
    {
        const char* mFixed = nullptr;   ///< a rule of no one's, or null
        LLUUID mWho;
        bool mSelf = false;
        std::string mRule;
        bool mReleasing = false;
        std::string text() const;
    };
    struct Release
    {
        F32 mCut = 0.f;         ///< the cut applied
        Reason mWhy;
        F64 mSince = -1.0;      ///< when a relaxing decision first came, or -1
    };
    /// A linkset over every region it spans: its bounds and where its triangles are. Built with the pages.
    struct Extent
    {
        LLVector3d mMin{ 1e30, 1e30, 1e30 };
        LLVector3d mMax{ -1e30, -1e30, -1e30 };
        LLVector3d mAnchor;
        bool mNeverCut = false;
        bool mCuttable = false; ///< its anchor is in the cut's region
        LLUUID mRoot;
        bool mGround = false;   ///< in a cluster standing on the land
        F64 mBottom = 1e30;     ///< its cluster's base
        std::vector<std::pair<const Region*, const Group*>> mParts;
    };
    std::map<U32, Extent> mExtents;
    std::map<LLUUID, U32> mRootExtent;  ///< each linkset root's extent
    std::vector<U32> mFloating;         ///< the extents not standing on the land
    U64 mExtentsSignature = 0;  ///< the buffers' signature mExtents was built for
    bool mExtentsBuilt = false;
    bool mExtentHeightsMoved = false;   ///< clusters moved in height since mExtents' bases were taken
    U64 mExtentsRegion = ~0ull; ///< the region handle mCuttable was worked for
    /// One person's own share of the cut, kept while they stay put: the surface under them and what their room or
    /// column takes. Most people are still in any one frame.
    struct OccupantCut
    {
        U32 mBest = 0;
        F64 mBestZ = -1e30;
        bool mCovered = false;
        std::vector<std::pair<U32, std::string>> mMarks; ///< linkset and rule
        bool mSeen = false;
        /// The membership probe in flight: its candidates (extent id, root), one query each, and the rule they mark.
        skoverview::ProbeSlot mMarksProbe;
        std::vector<std::pair<U32, LLUUID>> mMarkCandidates;
        std::vector<ALQueryTicket> mMarkTickets;
        const char* mMarkRule = nullptr;
        /// The roof probe in flight: the feet texel's slot and the feet's height then, and a query per linkset that
        /// may cover the feet.
        skoverview::ProbeSlot mRoofProbe;
        S32 mFeetSlot = -1;
        F64 mFeetZ = 0.0;
        std::vector<std::pair<U32, LLUUID>> mCoverCandidates;
        std::vector<ALQueryTicket> mCoverTickets;
    };
    /// The probes' counts over a second, for the log.
    struct ProbeStats
    {
        F64 mSince = 0.0;
        U32 mIssued = 0;        ///< people probed
        U32 mQueries = 0;
        U32 mApplied = 0;
        U32 mLatencyFrames = 0; ///< summed over the applied ones
    };
    ProbeStats mProbeStats;
    std::map<LLUUID, OccupantCut> mOccupantCuts;
    std::map<LLUUID, Release> mRelease;     ///< applied cuts by linkset root, relaxed only once the relaxing holds
};

#endif // SK_OVERVIEWCENSUS_H
