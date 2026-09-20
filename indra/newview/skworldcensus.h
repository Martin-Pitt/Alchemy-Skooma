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
    F64 mRange = 0.0;                           ///< metres from the centre the scan covered
    std::vector<sknav::RecordPtr> mRecords;     ///< static and dynamic alike; clusters skip the dynamic
    sknav::ClusterSet mClusters;

    struct Stats
    {
        U32 mObjectsSeen = 0;
        U32 mRecords = 0;
        U32 mDynamic = 0;
        U32 mSurround = 0;
        U32 mPlaceholders = 0;      ///< provenance BOX: an asset still fetching
        U32 mCached = 0;            ///< parts reused from the previous scan
        U32 mTriangles = 0;
        U32 mRolesKnown = 0;
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

    std::string statsLine() const;

private:
    struct PartCache
    {
        U64 mSignature = 0;
        sknav::RecordPtr mRecord;
        F32 mLastSeen = 0.f;
    };

    struct Rest
    {
        LLVector3d mPosition;
        F32 mMovedAt = -1.f;        ///< last time the root was seen somewhere else
        F32 mLastSeen = 0.f;
        bool mKnown = false;
    };

    struct Scan
    {
        std::shared_ptr<SKCensusSnapshot> mPending;
        /// The object list as it stood when the scan began: the live list is
        /// compacted every frame and would move objects past the cursor.
        std::vector<LLPointer<LLViewerObject>> mObjects;
        S32 mIndex = 0;             ///< next entry of mObjects to look at
        std::vector<U64> mRegions;  ///< the region set the scan started with
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
    bool classifyRest(LLViewerObject* root, const LLVector3d& pos);
    bool isSurround(LLViewerObject* root, const LLVector3d& mn, const LLVector3d& mx) const;
    sknav::RecordPtr record(LLVOVolume* volume, LLViewerObject* root, sknav::Role role, bool dynamic, U64 signature);

    void requestRoles();
    void onRoles(U32 request, S32 status, std::shared_ptr<LLPathfindingObjectList> list);

    SKCensusSnapshotPtr mSnapshot;
    Scan mScan;
    std::unordered_map<LLUUID, PartCache> mParts;
    std::unordered_map<LLUUID, Rest> mRest;
    std::unordered_map<LLUUID, sknav::NavRole> mRoles;
    std::map<std::string, F32> mDemands;
    U64 mStamp = 0;
    F32 mRolesRequestedAt = -1.f;
    U64 mRolesRegion = 0;
    bool mRolesWanted = false;
    U32 mRolesRequestId = 0;
    F32 mLastLog = 0.f;
};

#endif // SK_WORLDCENSUS_H
