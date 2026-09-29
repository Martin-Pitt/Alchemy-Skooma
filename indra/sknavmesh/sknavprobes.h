/**
 * @file sknavprobes.h
 * @brief Automatic reflection probe spheres for rooms: parts, placement, clearing placed probes, budget and churn.
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

#ifndef SK_NAVPROBES_H
#define SK_NAVPROBES_H

#include "sknavspaces.h"
#include "sknavtypes.h"

#include "v3math.h"

#include <unordered_map>
#include <vector>

namespace sknav
{
    /// True only inside a known closed collision body, never a placeholder AABB.
    bool probeInsideSolid(const Record& record, const LLVector3d& position);

    /// The solid records of one census scan, bucketed by the lattice tiles
    /// their global AABBs cover, with the oversized few -- spanning more than
    /// a screen of tiles -- kept separate, where every point test reaches
    /// them. A point test is one hash and the records of a single tile.
    /// Holds its records by counted handle: rebuilding from a new scan lets
    /// the old scan's records go.
    class SolidLattice
    {
    public:
        void clear();
        /// Re-derive from the given records, dropping whatever was held.
        void rebuild(const std::vector<RecordPtr>& records);
        /// Whether the point lies inside any solid record's volume.
        bool contains(const LLVector3d& p) const;

    private:
        std::unordered_map<TileKey, std::vector<RecordPtr>, TileKeyHash> mTiles;
        std::vector<RecordPtr> mLarge;
    };

    struct ProbeParams
    {
        F32 mCellM = 0.5f;          ///< the floor plan's cell, the spaces raster's
        F32 mMinPartM2 = 4.f;       ///< a part smaller than this folds into a neighbour
        F32 mMinPartWidthM = 1.f;   ///< and so does one narrower than this, a strip of ragged edge
        U32 mMaxRects = 64;         ///< rectangles cut per space before the rest floods into them
        F32 mMaxRadiusM = 13.86f;   ///< an octree probe's reach: its 16 m node's half-diagonal
        F32 mMinRadiusM = 2.f;      ///< a sphere shrunk below this is dropped
        F32 mCoverageShare = 1.f;   ///< a part's sphere reaches this far past its farthest cell
        F32 mSpaceRadiusM = 0.f;    ///< a sphere also reaches across its space's plan up to this
        F32 mBlendZoneM = 2.f;      ///< the overlap two adjacent parts' spheres must share, so the
                                    ///< blend between them has room; solved iteratively per part
        F32 mBlendShare = 0.35f;    ///< or this share of the smaller sphere's radius, whichever is more
        F32 mUnknownRadiusM = 8.f;  ///< a probe of an UNKNOWN-bucket space reaches this far at most
        U32 mUnknownMinCells = 2;   ///< raster cells an UNKNOWN space needs before it probes at all
        F32 mOpenShare = 0.5f;      ///< a space with more of its floor under open sky has no ceiling
        F32 mMoveM = 1.f;           ///< a held probe moves only when its part moves further
        F32 mRadiusShare = 0.2f;    ///< or its radius changes by more than this share
        F32 mMatchM = 4.f;          ///< a new sphere this close to a held one is that one, moved
        F32 mBudgetHoldM = 8.f;     ///< a held sphere ranks this much nearer in the budget
        F32 mSpacingM = 8.f;        ///< kept spheres stand this far apart at least, while the budget is tight
        U32 mMaxPerSpace = 1;       ///< spheres per space at most, while the budget is tight
    };

    /// A space's floor seen from above: per cell, its floor height, NaN
    /// outside the space. Mesh-local.
    struct FloorPlan
    {
        S32 mX0 = 0;                ///< in cells
        S32 mY0 = 0;
        S32 mW = 0;
        S32 mH = 0;
        F32 mCell = 0.5f;
        F32 mCeiling = 0.f;         ///< the space's mean ceiling height
        std::vector<F32> mFloor;

        bool in(S32 x, S32 y) const;
        F32 floorAt(S32 x, S32 y) const { return mFloor[y * mW + x]; }
    };

    /// One sphere for one part of a space.
    struct ProbePart
    {
        U32 mPart = 0;              ///< largest first
        LLVector3 mOrigin;
        F32 mRadius = 0.f;
        F32 mSpaceRadius = 0.f;     ///< the reach from this origin across the whole space's plan,
                                    ///< so spheres of neighbouring spaces intersect; 0 when off
        bool mBridged = false;      ///< grown past its part only to carry a blend across a
                                    ///< boundary the cap left weak; the first to shed
        F32 mArea = 0.f;
        LLVector3 mMin;             ///< the part's floor plan, floor to ceiling
        LLVector3 mMax;
    };

    /// A placed probe's influence: a sphere, or a box on three unit axes.
    struct ProbeInfluence
    {
        LLVector3 mCentre;
        F32 mRadius = 0.f;
        bool mBox = false;
        LLVector3 mAxes[3];
        LLVector3 mHalf;
    };

    /// A candidate sphere with where it sits, for the budget.
    struct ProbeSlot
    {
        U64 mKey = 0;
        U32 mSpace = 0;             ///< the space it belongs to, as the caller numbers them
        U32 mPart = 0;              ///< the rank of its part in the space, the largest first
        LLVector3 mOrigin;
        F32 mRadius = 0.f;
        F32 mArea = 0.f;
        F32 mDistance = 0.f;        ///< from the camera to the sphere's surface
        bool mHeld = false;         ///< a probe stands for it already
    };

    /// The plan of a space from its raster, the lowest floor per column.
    FloorPlan floorPlanOf(const Space& space, const ProbeParams& params);

    /// Whether a known space has walkable raster cells for a full probe.
    bool wantsProbes(const Space& space, const ProbeParams& params);

    /// Split the plan into rectangles, greedily at its reflex corners, fold
    /// small parts into a neighbour, and place one sphere per part: at its
    /// most clear cell, half way up to the ceiling, reaching its farthest cell.
    std::vector<ProbePart> placeParts(const FloorPlan& plan, const ProbeParams& params);

    /// Both of the above; empty for a space that wants none.
    std::vector<ProbePart> spaceProbes(const Space& space, const ProbeParams& params);

    /// How far p lies outside the influence; zero inside it.
    F32 distanceTo(const ProbeInfluence& influence, const LLVector3& p);

    /// The radius a sphere keeps when it must not reach into any placed
    /// probe's influence; zero when that leaves it under the minimum.
    F32 clearRadius(const LLVector3& origin, F32 radius, const std::vector<ProbeInfluence>& placed, const ProbeParams& params);

    /// Keep at most cap slots, dropping the far and small first; held ones
    /// rank hold_m nearer, so the kept set does not flicker at the edge.
    void keepBudget(std::vector<ProbeSlot>& slots, U32 cap, F32 hold_m = 0.f);

    /// The budget a room probe stands in: while the cap holds fewer than twice
    /// the candidates, one sphere per space at most, each standing mSpacingM
    /// clear of the last, the far and small dropping first; with room to spare,
    /// every part places however close. Held ones rank mBudgetHoldM nearer
    /// throughout, so the kept set does not flicker at either edge.
    void budgetProbes(std::vector<ProbeSlot>& slots, U32 cap, const ProbeParams& params);

    /// Pair each new origin with the nearest held one within tolerance, the
    /// closest pairs first and each held one at most once: per origin, the
    /// held index or -1. Parts renumbered by a rebuild keep their probes.
    std::vector<S32> matchNearest(const std::vector<LLVector3>& held, const std::vector<LLVector3>& origins, F32 tolerance);

    /// Whether a held probe should move to a newly placed sphere.
    bool shouldReplace(const LLVector3& held_origin, F32 held_radius, const LLVector3& origin, F32 radius, const ProbeParams& params);
}

#endif // SK_NAVPROBES_H
