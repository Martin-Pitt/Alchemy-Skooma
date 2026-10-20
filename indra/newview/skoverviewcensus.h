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

#include "llpointer.h"
#include "lluuid.h"
#include "threadpool_fwd.h"
#include "v3dmath.h"
#include "v3math.h"

#include <future>
#include <memory>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
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
    constexpr U32 COVER_RETRIES = 8;     ///< a candidate not settled after this many attempts stops forcing rechecks
    constexpr U32 COVER_EXTRACTS_PER_BUILD = 64; ///< visible parts whose top LOD is copied for the overlap test, at most, a build
    constexpr F32 GROUND_BASE_M = 4.f;  ///< a cluster whose base is no higher than this above the land stands on it
    constexpr F32 NO_CUT = 1.0e30f;     ///< the per-vertex cut height of a part drawn whole
    constexpr F32 HIDDEN = -1.0e30f;    ///< the per-vertex cut height of a part not drawn at all

    U8 codeOf(CensusClass cls);

    /// The default transparent texture or an old invisiprim texture: nobody sees a face drawn with one.
    bool transparentTexture(const LLUUID& id);

    /// Whether the overview draws a record at all, and the class of its parts that is not per triangle (door, ladder,
    /// vehicle); NUM_CENSUS_CLASSES when each triangle is classed by its slope.
    bool keepRecord(const sknav::Record& rec, CensusClass& fixed);

    /// A triangle's class from its unit normal's z and its part's height.
    CensusClass classifyTriangle(F32 nz, F32 part_height);
}

/// The census's shapes uploaded once per snapshot that changes them: one vertex buffer per region, its triangles
/// grouped by class. Main thread only.
class SKOverviewCensus
{
public:
    /// A linkset's triangles within a batch, and the heights its cluster spans.
    struct Group
    {
        F32 mMinZ = 0.f;        ///< global z, which is agent z too: its cluster's, so a level keeps whole clusters
        F32 mMaxZ = 0.f;
        bool mGround = false;   ///< its cluster's base is on the land under it
        U32 mId = 0;            ///< its linkset, the same in every batch it spans
        LLUUID mRoot;           ///< the linkset's root
        LLVector3d mAnchor;     ///< global; the linkset root's position, which says whose region it is in
        bool mNeverCut = false; ///< a surround in the void: always whole
        LLVector3d mMin;        ///< global bounds of its own triangles in this batch
        LLVector3d mMax;
        U32 mStart[skoverview::NUM_CENSUS_CLASSES] = {}; ///< its vertex range within each class's
        U32 mCount[skoverview::NUM_CENSUS_CLASSES] = {};
    };

    struct Batch
    {
        LLVector3d mOrigin;     ///< region origin, global; vertices are region-local
        LLPointer<LLVertexBuffer> mBuffer;
        U32 mStart[skoverview::NUM_CENSUS_CLASSES + 1] = {}; ///< vertex ranges per class
        std::vector<Group> mGroups;
        std::vector<LLVector3> mPositions; ///< the buffer's positions, region-local, for the cut's sight lines
        /// Per triangle: an invisible part's that does not stand in for visible geometry, passed through by the cut
        /// and the raycast as invisible parts always were.
        std::vector<U8> mInvisible;
        /// The cut heights the buffer holds, per vertex; empty while every one is NO_CUT.
        std::vector<F32> mCutValues;
    };

    /// Which groups a shot draws: all of them, or the ground's and those at the agent's level.
    struct Level
    {
        bool mAll = true;
        bool mGround = false;   ///< the agent is at ground level: groups standing on the land are kept
        F32 mLo = 0.f;          ///< groups overlapping these heights are kept
        F32 mHi = 0.f;

        bool keeps(const Group& group) const
        {
            return mAll || (mGround && group.mGround) || (group.mMaxZ >= mLo && group.mMinZ <= mHi);
        }
    };
    /// The top of what a level keeps, at least floor; for a shot's near plane.
    F32 levelTop(const Level& level, F32 floor) const;

    SKOverviewCensus();
    ~SKOverviewCensus();

    /// Rebuilds the buffers when the census's geometry changed; true when there is anything to draw.
    bool update();

    /// Someone the tactical cut is taken for: the building whose parts' bounds hold their floor is cut at mCut.
    struct Occupant
    {
        LLUUID mId;             ///< who, for the log
        bool mSelf = false;     ///< the agent: what is cut for them is cut at their level, whoever else needs it
        LLVector3d mFloor;      ///< global, their feet on their floor
        LLVector3d mFeet;       ///< global, their feet as the avatar stands, for the surface they stand on
        F32 mCut = 0.f;         ///< agent z
        /// The enclosed room they are in, flooded through the indoor air: its cells on the mRoomCellM lattice
        /// (global x and y over the cell size, floored); null outdoors.
        std::shared_ptr<const std::set<std::pair<S32, S32>>> mRoomCells;
        F32 mRoomCellM = 0.5f;
        bool mOutdoors = false; ///< in outdoor air by the enclosure: no room or column is cut for them
        F32 mPriority = 0.f;    ///< among others sharing a building, the highest priority's storey is cut for
    };
    /// Writes each vertex's cut height for the tactical view, for the linksets in the given region:
    /// - room: every linkset bounding an occupant's enclosed room (walls round its floor, floors above, the roof);
    /// - none for an occupant in outdoor air (on a roof, in the street);
    /// - column: with no room known, every linkset whose bounds hold their column and rise above it;
    /// - on a roof: a linkset someone stands on with none of it over them is cut no lower than their level, nor hidden,
    ///   unless the agent is under it; held a second after they leave it.
    /// What hides someone from the tactical eye is not cut here: its shader dissolves it round them.
    /// The agent's are cut at the agent's level, whoever else needs them; another's at the level of its
    /// occupant of highest mPriority. Every other part, there and elsewhere, is drawn whole. key says when the eye or the people moved:
    /// the same key and buffers do nothing. Rewrites the attribute only when the result changed, and logs it then;
    /// cutStamp() moves when it does.
    void applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, U64 key);
    U64 cutStamp() const { return mCutStamp; }
    /// A linkset's cut at the last applyCut, by its root: its height (HIDDEN when hidden) and why; false when whole.
    bool cutOf(const LLUUID& root, F32& height, std::string& why) const;
    /// The nearest census triangle along a ray as the tactical view draws it, global: invisible parts, hidden linksets
    /// and whatever lies above its linkset's last cut are passed through. False when nothing is hit within max_m.
    bool raycast(const LLVector3d& origin, const LLVector3& dir, F32 max_m, LLVector3d& hit) const;
    const std::vector<Batch>& batches() const { return mBatches; }
    U32 triangles() const { return mTriangles; }
    /// Parts that keep changing (a scripted door, a target on a track), left out of the static buffers for the movers
    /// to draw live: each change would otherwise rebuild every region's buffers.
    std::vector<LLUUID> restlessParts() const;
    /// Moves whenever the buffers are rebuilt.
    U64 signature() const { return mSignature; }
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
    };
    void gatherSurrounds(const struct SKCensusSnapshot& snap);
    /// A visible part's visible faces at its top LOD, region-local: what an invisible part may lie on.
    struct CoverGeom
    {
        LLVector3d mOrigin;
        std::vector<LLVector3> mTris;
    };
    /// What decides an invisible part's verdict: the visible geometry near it, and whether some is still on its way.
    struct CoverInput
    {
        bool mTest = false;     ///< an invisible part, judged by the overlap test
        bool mPending = false;  ///< a candidate's top LOD or a face's look is not settled: no barrier yet
        bool mRetry = false;    ///< and is worth asking again at the recheck
        U64 mKey = 0;           ///< its candidates' geometry and looks
        std::vector<std::shared_ptr<const CoverGeom>> mGeoms;
    };
    /// The candidates near an invisible part's bounds (global), copying at most budget new ones' geometry.
    CoverInput coverInputFor(const LLUUID& part, const LLVector3d& lo, const LLVector3d& hi,
                             const struct SKCensusSnapshot& snap, U32& budget);
    struct CoverEntry
    {
        U64 mKey = 0;           ///< its geometry and look, as the verdicts are keyed
        U64 mGeomKey = 0;       ///< its shape and placement
        U64 mLookKey = 0;       ///< its faces' inputs
        U32 mPass = ~0u;        ///< the pass it was last worked in
        U32 mTries = 0;         ///< attempts since it last settled or changed
        bool mGeomPending = true; ///< its top LOD is not copied yet
        bool mLookPending = false; ///< a face's look is not known yet
        bool mInRange = false;  ///< near enough the scan to copy its top LOD
        LLVector3d mOrigin;
        std::shared_ptr<const std::vector<LLVector3>> mRaw; ///< every face's triangles, region-local
        std::vector<U32> mStarts;   ///< each face's first vertex in mRaw
        std::shared_ptr<const CoverGeom> mGeom; ///< the visible faces' triangles; null when it shows nothing
    };
    std::unordered_map<LLUUID, CoverEntry> mCoverGeom;
    struct CoverCandidate
    {
        LLUUID mId;
        LLVector3d mCentre;
        F32 mRadius = 0.f;
    };
    std::vector<CoverCandidate> mCoverCandidates;   ///< every static volume the last full object list walk found
    std::unordered_map<U64, std::vector<U32>> mCoverGrid; ///< mCoverCandidates by COVER_CELL_M cell in plan
    std::vector<U32> mCoverLarge;                   ///< those spanning too many cells to file
    static constexpr F64 COVER_CELL_M = 16.0;
    /// The worker's verdicts kept across builds by part; each build reads the last landed map and makes the next.
    struct Verdict
    {
        U64 mKey = 0;           ///< the part's geometry and its CoverInput::mKey
        U8 mVerdict = 0;        ///< sknav::InvisibleVerdict
        U16 mCovered = 0;       ///< faces covered and bare, for the log
        U16 mBare = 0;
        F32 mSteepM2 = 0.f;
    };
    struct Verdicts
    {
        std::unordered_map<LLUUID, Verdict> mByPart;
    };
    std::shared_ptr<const Verdicts> mVerdicts = std::make_shared<const Verdicts>();
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
        std::vector<U32> mGroupOf;      ///< per kept record then sculpt, its linkset's group
        std::vector<LLUUID> mRoots;     ///< per group
        std::map<U32, U32> mGroupCluster;
        std::vector<std::pair<LLVector3d, bool>> mAnchors; ///< per group: its root's position, and void-only
        std::vector<std::pair<LLVector3d, F64>> mRegions;  ///< connected regions' origins and widths
    };
    /// A build's batches, short of their buffers and of whether each group stands on the land.
    struct Built
    {
        struct Region
        {
            LLVector3d mOrigin;
            std::vector<LLVector3> mPositions;  ///< by class, then group
            U32 mStart[skoverview::NUM_CENSUS_CLASSES + 1] = {};
            std::vector<Group> mGroups;
            std::vector<std::pair<LLVector3d, LLVector3d>> mFootprints;  ///< per group, the box the land under it is read over
            std::vector<U8> mInvisible;         ///< per triangle, as Batch's
        };
        /// What the overlap test made of the invisible parts, for the log.
        struct CoverStats
        {
            U32 mParts = 0;
            U32 mStandIns = 0;
            U32 mBarriers = 0;
            U32 mUnknown = 0;
            U32 mDropped = 0;   ///< barriers or unknown parts over INVISIBLE_WALL_M
            U32 mJudged = 0;    ///< worked this build, the rest from the kept verdicts
            U32 mFacesCovered = 0;
            U32 mFacesBare = 0;
            F32 mHatchedSteepM2 = 0.f;
            F64 mVerdictSeconds = 0.0; ///< in the verdicts of the judged parts
            F64 mBuildSeconds = 0.0;   ///< in the whole per-part loop
        };
        CoverStats mCover;
        std::shared_ptr<const Verdicts> mVerdicts; ///< the verdicts this build asked for, adopted when it lands
        U64 mSignature = 0;
        U32 mGeneration = 0;
        std::vector<Region> mRegions;
        std::vector<std::pair<size_t, size_t>> mSplit; ///< regions over one batch: their vertices and batch count
    };
    static std::shared_ptr<Built> buildBatches(const Job& job);
    /// Uploads a build and replaces every batch with it, in this frame.
    void swapIn(Built& built);
    std::unique_ptr<LL::ThreadPool> mWorkers;
    std::future<std::shared_ptr<Built>> mJob;
    U32 mGeneration = 0;    ///< moves at release(), so a build begun before it never lands
    bool mBuilt = false;    ///< a build has landed since the last release()
    /// A part's look, kept per part while its object and faces' inputs stay the same.
    skoverview::Seen seenOf(const LLUUID& part);
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
    void logChurn(std::vector<std::pair<U64, std::pair<char, LLUUID>>>& entries,
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

    std::vector<Batch> mBatches;
    std::vector<RenderedPart> mSurrounds;
    U64 mSurroundsSig = 0;
    F64 mSurroundsAt = 0.0;     ///< when the surrounds are looked at again
    U64 mStamp = 0;
    U64 mSignature = 0;
    bool mUnknownLook = false;  ///< some part's faces or images were not loaded at the last build
    std::unordered_map<LLUUID, skoverview::Seen> mLargeLogged; ///< the large parts of the last pass and how each was logged
    std::string mCoverLogged;   ///< the cover counts last logged
    F64 mRecheckAt = 0.0;
    U64 mRecordsKey = 0;        ///< the census records' identities at the last look
    bool mRecordsChanged = true;
    F64 mSculptsAt = 0.0;       ///< when the live sculpts are looked at again
    std::shared_ptr<const struct SKCensusSnapshot> mSnapshot;
    U32 mTriangles = 0;
    std::vector<std::pair<U32, F32>> mCuts; ///< the clusters and parts cut at the last applyCut, and their heights
    U64 mCutSignature = 0;      ///< the buffers' signature the cut attribute was written for
    U64 mCutStamp = 0;
    U64 mCutKey = 0;            ///< the inputs the last applyCut was worked from
    std::string mCutLogged;     ///< the cut linksets and reasons last logged
    std::map<LLUUID, std::pair<F32, std::string>> mCutRoots; ///< the last applyCut's cuts by linkset root
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
    /// A linkset over every batch it spans: its bounds and where its triangles are. Built with the buffers.
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
        std::vector<std::pair<const Batch*, const Group*>> mParts;
    };
    std::map<U32, Extent> mExtents;
    std::map<LLUUID, U32> mRootExtent;  ///< each linkset root's extent
    std::vector<U32> mFloating;         ///< the extents not standing on the land
    U64 mExtentsSignature = 0;  ///< the buffers' signature mExtents was built for
    bool mExtentsBuilt = false;
    U64 mExtentsRegion = ~0ull; ///< the region handle mCuttable was worked for
    /// One person's own share of the cut, kept while they stay put: the surface under them and what their room or
    /// column takes. Most people are still in any one frame.
    struct OccupantCut
    {
        bool mRoofValid = false;
        U64 mRoofKey = 0;
        U32 mBest = 0;
        F64 mBestZ = -1e30;
        bool mCovered = false;
        bool mMarksValid = false;
        U64 mMarksKey = 0;
        std::vector<std::pair<U32, std::string>> mMarks; ///< linkset and rule
        bool mSeen = false;
    };
    std::map<LLUUID, OccupantCut> mOccupantCuts;
    std::map<LLUUID, Release> mRelease;     ///< applied cuts by linkset root, relaxed only once the relaxing holds
};

#endif // SK_OVERVIEWCENSUS_H
