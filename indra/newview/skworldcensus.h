/**
 * @file skworldcensus.h
 * @brief The census: every declared physics shape near the agent, as the navmesh sees it.
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

#ifndef SK_WORLDCENSUS_H
#define SK_WORLDCENSUS_H

#include "llsingleton.h"
#include "llviewerobject.h"
#include "sknavcluster.h"
#include "sknavfeatures.h"
#include "sknavtypes.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

class LLViewerObject;
class LLVOVolume;
class LLPathfindingObjectList;

/// A completed scan: immutable once published, shared by pointer with anyone
/// who needs a consistent view (the cluster builds, the overlay, the console).
struct SKCensusSnapshot
{
    U64 mStamp = 0;                             ///< moves once per completed scan
    LLVector3d mCenter;                         ///< the agent when the scan started
    LLVector3d mCamera;                         ///< the camera then: covered as well, and often elsewhere
    F64 mRange = 0.0;                           ///< metres from either centre the scan covered
    std::vector<sknav::RecordPtr> mRecords;     ///< static and dynamic alike; clusters skip the dynamic
    sknav::ClusterSet mClusters;
    /// Doors, ladders, shafts, vehicles and seats among this scan's parts.
    /// The records carry a part's kind from the scan after.
    std::vector<sknav::Feature> mFeatures;

    struct Stats
    {
        U32 mObjectsSeen = 0;
        U32 mRecords = 0;
        U32 mDynamic = 0;
        U32 mSurround = 0;
        U32 mPlaceholders = 0;      ///< provenance BOX: objects known only from the region's object cache
        U32 mCached = 0;            ///< parts reused from the previous scan
        U32 mFromObjectCache = 0;   ///< boxes from the region's object cache, never drawn this session
        U32 mTiny = 0;              ///< parts under SKNavMinShape in every axis: never a record
        U32 mTriangles = 0;
        U32 mRolesKnown = 0;
        U32 mFeatures = 0;
        F32 mFeatureMs = 0.f;       ///< the facts gathered and the rules run
        F32 mScanMs = 0.f;          ///< main-thread time the scan took, summed over its slices
        F32 mClusterMs = 0.f;
    } mStats;
};

typedef std::shared_ptr<const SKCensusSnapshot> SKCensusSnapshotPtr;

/// Runs a time-sliced scan of the object list under a per-frame budget and
/// swaps in a whole snapshot when one completes. Readers keep the previous
/// snapshot meanwhile. Main thread only.
class SKWorldCensus : public LLSingleton<SKWorldCensus>
{
    LLSINGLETON(SKWorldCensus);
    ~SKWorldCensus() override;

    /// Drops any LLPointer<LLViewerObject> a mid-flight scan is holding, so it can't
    /// outlive LLAvatarAppearance::cleanupClass() and tear the self avatar down late.
    void cleanupSingleton() override;

public:
    /// Once per frame after objects have moved. Does nothing unless the
    /// navmesh is enabled and something wants a census (a demand, or the overlay).
    void update();

    /// The last completed scan, or null before the first.
    SKCensusSnapshotPtr snapshot() const { return mSnapshot; }

    /// Consumers say they need the census; the scan runs while any is registered.
    void addDemand(const std::string& who, F32 range_m);
    void removeDemand(const std::string& who);

    /// Record and cluster boxes in world, from the pipeline's debug pass.
    void renderDebug();

    /// The sim's navmesh role for a linkset, when the region has told us.
    sknav::NavRole roleOf(const LLUUID& root) const;

    /// What the region's linkset reply says about a root besides its role.
    /// Only roots in the agent's region are covered; a missing entry or an
    /// empty name says nothing either way.
    struct LinksetFacts
    {
        std::string mName;
        std::string mDescription;
        bool mScriptedKnown = false;
        bool mScripted = false;
    };
    const LinksetFacts* linksetFactsOf(const LLUUID& root) const;

    /// What this session has seen a root do. Kept until logout, across
    /// region changes: crew get off and a vehicle stays one.
    struct RootHistory
    {
        bool mEverMoved = false;
        bool mEverCrewed = false;
        bool mWorkingElevator = false;  ///< it was the car of a working shaft
        sknav::FeatureKind mKind = sknav::FeatureKind::NONE;
        std::vector<LLUUID> mCrew;      ///< seated on it at the last scan
    };
    const RootHistory* historyOf(const LLUUID& root) const;

    std::string statsLine() const;

private:
    struct PartCache
    {
        U64 mSignature = 0;
        sknav::RecordPtr mRecord;
        sknav::RecordPtr mPhysics;  ///< a surround's physics shape, beside its rendered one
        F32 mLastSeen = 0.f;
    };

    struct Rest
    {
        LLVector3d mPosition;
        LLQuaternion mRotation;     ///< baseline together with mPosition
        F32 mMovedAt = -1.f;        ///< last time the root was seen somewhere else
        F32 mLastSeen = 0.f;
        bool mKnown = false;
    };

    /// What the region's object cache knows about something it has never
    /// instantiated: where it is and how big, and nothing else.
    struct CachedPart
    {
        LLUUID mId;                 ///< synthesised from the region and the local id
        LLVector3d mOrigin;         ///< the region's origin, global
        LLVector3 mCenter;          ///< region-local
        LLVector3 mHalf;
    };

    struct Scan
    {
        std::shared_ptr<SKCensusSnapshot> mPending;
        /// The object list as it stood when the scan began: the live list is
        /// compacted every frame and would move objects past the cursor.
        std::vector<LLPointer<LLViewerObject>> mObjects;
        S32 mIndex = 0;             ///< next entry of mObjects to look at
        std::vector<U64> mRegions;  ///< the region set the scan started with
        /// Objects the region has cached but not drawn: bounds only, gathered
        /// once when the scan begins and emitted as boxes after the live pass.
        std::vector<CachedPart> mCached;
        size_t mCachedIndex = 0;
        /// Every part recorded this scan, as the feature rules read it.
        std::vector<sknav::FeaturePart> mFeatureParts;
        F32 mElapsedMs = 0.f;
        bool mActive = false;
    };

    bool wanted() const;
    F64 range() const;
    void beginScan();
    void continueScan(F32 budget_ms);
    void finishScan();
    void abandonScan();
    bool sameRegions(const std::vector<U64>& regions) const;
    static std::vector<U64> regionHandles();

    /// One object into the pending snapshot: zero, one or many records.
    void visit(LLViewerObject* object);
    /// One cached object into the pending snapshot, as a box.
    void visitCached(const CachedPart& part);
    /// Every cached entry near either centre that has no object of its own.
    void gatherCached();
    bool classifyRest(LLViewerObject* root, const LLVector3d& pos, const LLQuaternion& rot);
    bool isSurround(LLViewerObject* root, const LLVector3d& mn, const LLVector3d& mx) const;
    /// One record from one part, at the sim-side transform the caller signed.
    sknav::RecordPtr record(LLVOVolume* volume, LLViewerObject* root, sknav::Role role, bool dynamic, U64 signature,
                            const LLVector3& pos, const LLQuaternion& rot, sknav::FeatureKind feature, U8 exclude);
    /// Who is sitting on what, into the session history.
    void gatherCrew();
    /// The feature rules over the finished scan's parts; the part kinds they
    /// give are what the next scan records with.
    void classify(SKCensusSnapshot& snap);
    /// A record into the pending snapshot, counted.
    void take(SKCensusSnapshot& snap, const sknav::RecordPtr& rec);

    void requestRoles();
    void onRoles(U32 request, S32 status, std::shared_ptr<LLPathfindingObjectList> list);

    SKCensusSnapshotPtr mSnapshot;
    Scan mScan;
    std::unordered_map<LLUUID, PartCache> mParts;
    std::unordered_map<LLUUID, Rest> mRest;
    std::unordered_map<LLUUID, sknav::NavRole> mRoles;
    std::unordered_map<LLUUID, LinksetFacts> mLinksets;
    std::unordered_map<LLUUID, RootHistory> mHistory;
    struct PartFeature
    {
        sknav::FeatureKind mKind = sknav::FeatureKind::NONE;
        U8 mExclude = 0;
    };
    std::unordered_map<LLUUID, PartFeature> mPartFeatures;
    std::map<std::string, F32> mDemands;
    U64 mStamp = 0;
    F32 mRolesRequestedAt = -1.f;
    U64 mRolesRegion = 0;
    bool mRolesWanted = false;
    U32 mRolesRequestId = 0;
    F32 mLastLog = 0.f;
};

#endif // SK_WORLDCENSUS_H
