/**
 * @file sknavfeatures.h
 * @brief Doors, ladders, elevators, vehicles and seats, recognised from census parts and what the viewer knows of them.
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

#ifndef SK_NAVFEATURES_H
#define SK_NAVFEATURES_H

#include "sknavtypes.h"

#include <string>
#include <vector>

namespace sknav
{
    /// Something the viewer may or may not have been told.
    enum class TriState : U8 { UNKNOWN, NO, YES };

    /// One part as the rules see it: its oriented box and the flags the
    /// viewer holds for it. Positions are global.
    struct FeaturePart
    {
        LLUUID mPart;
        LLUUID mRoot;
        LLVector3d mMin;            ///< AABB
        LLVector3d mMax;
        LLVector3d mCenter;
        LLVector3 mAxes[3];         ///< unit axes of the prim's own box
        LLVector3 mHalf;            ///< half extents along mAxes
        std::string mName;          ///< the prim's own name, empty when not known
        bool mScripted = false;     ///< the leaf's own flag
        bool mTouch = false;
    };

    /// What is known about one linkset. An empty name or description is not
    /// evidence either way: most roots are never named to the viewer.
    struct FeatureRoot
    {
        LLUUID mRoot;
        std::string mName;
        std::string mDescription;
        std::string mHover;
        TriState mScripted = TriState::UNKNOWN;
        bool mPhysical = false;
        bool mEverMoved = false;    ///< this session
        bool mEverCrewed = false;   ///< this session
        bool mWorkingElevator = false;  ///< confirmed a working car earlier this session
        bool mHasBounds = false;    ///< the whole linkset's box, for roots whose parts the census left out
        LLVector3d mMin;
        LLVector3d mMax;
        F32 mClearance = -1.f;      ///< its bottom above the floor under it; < 0 when not known
    };

    struct FeatureParams
    {
        F32 mClassHeight = 2.f;     ///< a door leaf is at least this tall
        F32 mThinMax = 0.35f;       ///< a door leaf is at most this thick
        F32 mAirClearance = 30.f;   ///< a vehicle this far above its floor is flying
        F32 mStackTolerance = 0.1f; ///< z gap that still counts as touching in a ladder stack
        F32 mFootprintOverlap = 0.5f;   ///< share of each plan a ladder piece must have in common
        F32 mDoorLine = 0.5f;       ///< how far apart stacked doors may sit in plan
        F32 mCarReach = 3.f;        ///< how far in front of a door stack a car is looked for
    };

    struct Feature
    {
        FeatureKind mKind = FeatureKind::NONE;
        F32 mConfidence = 0.f;
        std::vector<LLUUID> mParts;     ///< what it owns: a door's leaves, a ladder's pieces, a car, a vehicle
        std::vector<LLUUID> mRoots;
        LLVector3d mMin;                ///< what it covers; a shaft's column, a ladder's span
        LLVector3d mMax;
        F64 mFoot = 0.0;                ///< ladder and shaft: bottom and top of the span
        F64 mTop = 0.0;
        LLVector3 mNormal;              ///< door: the leaf's thin axis, horizontal
        std::vector<S32> mDoors;        ///< shaft: the door features stacked on it
        bool mWorking = false;          ///< shaft: a car is in its column
        bool mCombat = false;           ///< vehicle: damageable
        bool mAir = false;              ///< vehicle: flying
        std::string mEvidence;          ///< why, for the mark dump
    };

    /// Every feature among the parts. Vehicles and seats first, then ladders,
    /// doors and the shafts their stacks make; a part belongs to one feature.
    std::vector<Feature> classifyFeatures(const std::vector<FeaturePart>& parts, const std::vector<FeatureRoot>& roots,
                                          const FeatureParams& params);

    /// The classes a part of this kind is left out of, as Record::mExclude.
    U8 excludeFor(FeatureKind kind);

    /// Hover text holding two numbers around a slash: 47/50, [47/50], HP 47 / 50.
    bool hoverShowsHealth(const std::string& hover);

    /// Case-insensitive substring test.
    bool containsNoCase(const std::string& haystack, const char* needle);

    const char* featureName(FeatureKind kind);
}

#endif // SK_NAVFEATURES_H
