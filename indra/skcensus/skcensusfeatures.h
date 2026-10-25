/**
 * @file skcensusfeatures.h
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

#ifndef SK_CENSUSFEATURES_H
#define SK_CENSUSFEATURES_H

#include "skcensustypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace skworld
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
        F32 mSeeThrough = 0.f;      ///< share of its faces that can be seen through
        F32 mFaceCover = -1.f;      ///< share of its more covered face something lies against; < 0 when not measured
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
        bool mEverMoved = false;    ///< this session, turning included
        F32 mTravel = 0.f;          ///< how far it has gone this session, turning not included
        bool mEverCrewed = false;   ///< this session
        bool mEverVehicle = false;  ///< crewed while physical or travelling earlier this session
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
        F32 mDoorWidthMin = 0.5f;   ///< a door leaf is this wide or wider
        F32 mDoorWidthMax = 6.f;    ///< and no wider than a gate
        F32 mDoorConfidence = 0.5f; ///< evidence a leaf needs to be a door
        F32 mFaceReach = 0.3f;      ///< how far off a leaf's face something counts as lying against it
        F32 mFaceBlocked = 0.5f;    ///< face cover from which a panel is mounted on something, not a leaf
        F32 mFaceClear = 0.2f;      ///< face cover up to which both faces are open, as a leaf's are
        F32 mVehicleTravel = 1.f;   ///< how far a crewed linkset must go to be a vehicle
        F32 mLadderFootprint = 1.f; ///< a lone ladder piece is no wider than this on either plan axis
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

    /// A door-sized thin upright panel: the shape a leaf must have.
    bool doorShaped(const FeaturePart& part, const FeatureParams& params);

    /// The share of the leaf's more covered face that other records' geometry
    /// lies against, within the reach, by a grid of short rays off each face
    /// that leaves out the edges and the sill. A wall behind a painting or a
    /// mattress against a headboard covers a face; a doorway's frame does not.
    /// Negative for a part that is not door-shaped.
    F32 faceCover(const FeaturePart& leaf, const std::vector<RecordPtr>& records, const FeatureParams& params);

    /// Records bucketed on a plan grid, so a part's neighbours are found
    /// without walking the whole census. Holds pointers into the vector it
    /// was built from, which must outlive it unchanged.
    class RecordIndex
    {
    public:
        explicit RecordIndex(const std::vector<RecordPtr>& records, F64 cell = 8.0);

        /// Every record whose box may meet the plan of mn..mx, in record order.
        void query(const LLVector3d& mn, const LLVector3d& mx, std::vector<const RecordPtr*>& out) const;

    private:
        const std::vector<RecordPtr>& mRecords;
        F64 mCell;
        /// The cells that hold records, as words sorting by x then y; a cell's
        /// records are mItems[mStart[k]] up to mItems[mStart[k + 1]], in record order.
        std::vector<U64> mKeys;
        std::vector<U32> mStart;
        std::vector<U32> mItems;
        std::vector<U32> mLarge;    ///< records spanning too many cells to bucket
    };

    /// faceCover over an index: the same answer, with the neighbours found
    /// by the grid and only the triangles near the leaf cast against.
    F32 faceCover(const FeaturePart& leaf, const RecordIndex& index, const FeatureParams& params);

    /// faceCover kept per part from one scan to the next: a leaf whose pose,
    /// parameters and neighbouring records are all as they were is not cast again.
    class FaceCoverCache
    {
    public:
        F32 measure(const FeaturePart& leaf, const RecordIndex& index, const FeatureParams& params);

        /// Forgets the parts not measured since the last prune.
        void prune();

        size_t size() const { return mEntries.size(); }
        U32 mHits = 0;
        U32 mMisses = 0;

    private:
        struct Entry
        {
            LLVector3d mMin;
            LLVector3d mMax;
            LLVector3d mCenter;
            LLVector3 mAxes[3];
            LLVector3 mHalf;
            FeatureParams mParams;
            std::vector<RecordPtr> mNearby;     ///< held, so a pointer is never reused while it keys an entry
            F32 mCover = -1.f;
            bool mUsed = false;
        };
        std::unordered_map<LLUUID, Entry> mEntries;
        std::vector<const RecordPtr*> mScratch;
    };

    /// One doorway as the rooms and the map see it: plain values, no viewer
    /// types. Positions are global.
    struct DoorEntity
    {
        LLUUID mId;                     ///< the smallest leaf part id, stable while the leaves are
        U64 mRegion = 0;                ///< region handle; set by whoever keys the table by region
        LLVector3d mMin;                ///< AABB of the leaves where they rest shut, or as they are when that is not known
        LLVector3d mMax;
        LLVector3d mPlanePoint;         ///< on the leaves' plane, at the sill
        bool mPlaneKnown = true;        ///< false while a leaf moves or has no rest pose: use the AABB only
        LLVector3 mNormal;              ///< the leaf's thin axis, horizontal, x (then y) positive
        F32 mWidth = 0.f;               ///< across the opening, in the plane
        F32 mHeight = 0.f;
        F64 mSill = 0.0;                ///< bottom of the leaves
        bool mDouble = false;
        std::vector<LLUUID> mLeaves;    ///< sorted
        F32 mConfidence = 0.f;
        bool mDamageable = false;       ///< an LBA description or health in the hover text
        std::string mName;              ///< the leaf's own name, else its linkset's
    };

    /// Where one door leaf has come to rest this session. A door rests shut
    /// most of the time, so the pose it has spent longest at is its doorway.
    struct LeafHistory
    {
        std::vector<FeaturePart> mRests;    ///< distinct still poses, first seen first
        std::vector<F32> mDwell;            ///< seconds spent at each
        FeaturePart mLast;                  ///< the pose at the last look
        bool mSeen = false;
        bool mMoving = false;               ///< it had moved since the look before
        bool mEverMoved = false;
        F32 mLastSeen = 0.f;
        LLUUID mDoor;                       ///< the door id it was last given, kept for the session
    };

    /// One look at a leaf at time now. Poses within the tolerance, in metres
    /// at the leaf's corners, are one pose.
    void observeLeaf(LeafHistory& history, const FeaturePart& pose, F32 now, F32 tolerance = 0.02f);

    /// The pose that stands for the doorway: the current one for a leaf that
    /// has never moved, else the rest it has spent longest at. False while it
    /// moves or before any rest is known, unless the doorway is established.
    bool doorwayPose(const LeafHistory& history, FeaturePart& out);

    /// Seconds a leaf must have rested at one pose before that pose is its
    /// doorway for good: a swing then changes nothing built over it.
    constexpr F32 DOORWAY_ESTABLISHED_SECONDS = 10.f;

    /// The rest the leaf has spent longest at, once it has spent at least
    /// DOORWAY_ESTABLISHED_SECONDS there; false before, moving or not.
    bool establishedDoorway(const LeafHistory& history, FeaturePart& out);

    /// Whether the leaf's last look found it still at its established doorway.
    bool atEstablishedDoorway(const LeafHistory& history, F32 tolerance = 0.02f);

    typedef std::unordered_map<LLUUID, LeafHistory> LeafHistories;

    /// A DoorEntity per DOOR feature, sorted by id, whatever order the parts
    /// came in. Leaves missing from the parts are skipped. A leaf with no
    /// history is taken as never having moved; the ids come from the
    /// histories' mDoor where a leaf has one, which this writes back.
    std::vector<DoorEntity> buildDoorEntities(const std::vector<Feature>& features, const std::vector<FeaturePart>& parts,
                                              const std::vector<FeatureRoot>& roots, LeafHistories* histories = nullptr);

    /// An LBA description or health shown in the hover text.
    bool isDamageable(const FeatureRoot& root);

    /// The classes a part of this kind is left out of, as Record::mExclude.
    U8 excludeFor(FeatureKind kind);

    /// Hover text holding two numbers around a slash: 47/50, [47/50], HP 47 / 50.
    bool hoverShowsHealth(const std::string& hover);

    /// Case-insensitive substring test.
    bool containsNoCase(const std::string& haystack, const char* needle);

    /// Case-insensitive word test: the needle neither starts nor ends inside
    /// a word, a plural s and, with humps, a camel-case hump counting as its edges.
    bool containsWordNoCase(const std::string& haystack, const char* needle, bool humps = true);

    const char* featureName(FeatureKind kind);
}

#endif // SK_CENSUSFEATURES_H
