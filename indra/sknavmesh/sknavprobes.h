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

#include "v3math.h"

#include <vector>

namespace sknav
{
    struct ProbeParams
    {
        F32 mCellM = 0.5f;          ///< the floor plan's cell, the spaces raster's
        F32 mMinPartM2 = 4.f;       ///< a part smaller than this folds into a neighbour
        F32 mMinPartWidthM = 1.f;   ///< and so does one narrower than this, a strip of ragged edge
        U32 mMaxRects = 64;         ///< rectangles cut per space before the rest floods into them
        F32 mMaxRadiusM = 13.86f;   ///< an octree probe's reach: its 16 m node's half-diagonal
        F32 mMinRadiusM = 2.f;      ///< a sphere shrunk below this is dropped
        F32 mOpenShare = 0.5f;      ///< a space with more of its floor under open sky has no ceiling
        F32 mMoveM = 1.f;           ///< a held probe moves only when its part moves further
        F32 mRadiusShare = 0.2f;    ///< or its radius changes by more than this share
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
        LLVector3 mOrigin;
        F32 mRadius = 0.f;
        F32 mArea = 0.f;
        F32 mDistance = 0.f;        ///< from the camera to the sphere's surface
    };

    /// The plan of a space from its raster, the lowest floor per column.
    FloorPlan floorPlanOf(const Space& space, const ProbeParams& params);

    /// Whether the space gets probes of its own: INDOORS or SHELTERED, under a ceiling.
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

    /// Keep at most cap slots, dropping the far and small first.
    void keepBudget(std::vector<ProbeSlot>& slots, U32 cap);

    /// Whether a held probe should move to a newly placed sphere.
    bool shouldReplace(const LLVector3& held_origin, F32 held_radius, const LLVector3& origin, F32 radius, const ProbeParams& params);
}

#endif // SK_NAVPROBES_H
