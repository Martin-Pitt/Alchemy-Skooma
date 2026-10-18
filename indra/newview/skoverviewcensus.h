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
#include "v3dmath.h"
#include "v3math.h"

#include <memory>
#include <string>
#include <map>
#include <set>
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

    /// The census classes, in vertex buffer order.
    enum CensusClass : U8 { FLOOR, WALL, COVER, RAMP, DOOR, LADDER, VEHICLE, PLACEHOLDER, INVISIBLE, NUM_CENSUS_CLASSES };

    constexpr F32 FLOOR_NZ = 0.866f;    ///< steeper than 30 degrees is no longer floor
    constexpr F32 WALL_NZ = 0.5f;       ///< steeper than 60 degrees is wall
    constexpr F32 COVER_TOP_M = 1.6f;   ///< a part no taller than this is cover, not wall
    constexpr F32 INVISIBLE_WALL_M = 64.f; ///< a fully transparent part larger than this on any axis is no wall
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
        F32 mCut = 0.f;         ///< agent z
        std::vector<LLVector3d> mSight;     ///< global points on them the tactical eye should see: feet, chest, head
        /// The enclosed room they are in, flooded through the indoor air: its cells on the mRoomCellM lattice
        /// (global x and y over the cell size, floored); null outdoors.
        std::shared_ptr<const std::set<std::pair<S32, S32>>> mRoomCells;
        F32 mRoomCellM = 0.5f;
    };
    /// Writes each vertex's cut height for the tactical view, for the linksets in the given region:
    /// - room: every linkset bounding an occupant's enclosed room (walls round its floor, floors above, the roof);
    /// - column: outdoors or with no room known, every linkset whose bounds hold their column and rise above it;
    /// - line of sight: every linkset whose triangles block the eye's sight of their feet, chest or head.
    /// The agent's are cut at the agent's level, whoever else needs them; another's at the highest of its
    /// occupants'. Every other part, there and elsewhere, is drawn whole. key says when the eye or the people moved:
    /// the same key and buffers do nothing. Rewrites the attribute only when the result changed, and logs it then;
    /// cutStamp() moves when it does.
    void applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, const LLVector3d& eye, U64 key);
    U64 cutStamp() const { return mCutStamp; }
    /// A linkset's cut at the last applyCut, by its root: its height (HIDDEN when hidden) and why; false when whole.
    bool cutOf(const LLUUID& root, F32& height, std::string& why) const;
    /// The nearest census triangle along a ray as the tactical view draws it, global: invisible parts, hidden linksets
    /// and whatever lies above its linkset's last cut are passed through. False when nothing is hit within max_m.
    bool raycast(const LLVector3d& origin, const LLVector3& dir, F32 max_m, LLVector3d& hit) const;
    const std::vector<Batch>& batches() const { return mBatches; }
    U32 triangles() const { return mTriangles; }
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
        std::vector<LLVector3> mTris;
        F32 mHeight = 0.f;
        skoverview::CensusClass mFixed = skoverview::NUM_CENSUS_CLASSES;
    };
    void gatherSurrounds(const struct SKCensusSnapshot& snap);

    std::vector<Batch> mBatches;
    std::vector<RenderedPart> mSurrounds;
    U64 mSurroundsSig = 0;
    F64 mSurroundsAt = 0.0;     ///< when the surrounds are looked at again
    U64 mStamp = 0;
    U64 mSignature = 0;
    bool mUnknownLook = false;  ///< some part's faces or images were not loaded at the last build
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
};

#endif // SK_OVERVIEWCENSUS_H
