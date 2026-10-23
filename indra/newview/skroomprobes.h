/**
 * @file skroomprobes.h
 * @brief Automatic reflection probe spheres for the rooms layer's spaces, in place of the octree's.
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

#ifndef SK_ROOMPROBES_H
#define SK_ROOMPROBES_H

#include "skrooms.h"
#include "skroomsprobecache.h"
#include "skroomsprobes.h"

#include "llpointer.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class LLReflectionMap;
struct SKCensusSnapshot;

/// Holds a full probe per walkable space near the camera — its extra parts
/// only while the probe budget has room for twice the candidates, and never
/// two spheres closer than the budget's spacing — matched to their parts by
/// nearest origin, before the residual grid pass. Placed probes
/// always win: a sphere shrinks clear of their influence or is dropped.
/// Only at probe level 3, where octree probes exist. A space's parts are
/// cached in memory under its tiles' content keys, so a space over
/// unchanged tiles places exactly as it did, however far the camera went.
/// Keeps the air cells building while it places. Main thread only.
class SKRoomProbes
{
public:
    SKRoomProbes();
    ~SKRoomProbes();

    void update(const SKWorldViewPtr& view);
    /// Let every full probe go, release its claim on residual coverage, and drop the air cells' demand.
    void clear();
    std::string statsLine() const;

private:
    struct Held
    {
        LLPointer<LLReflectionMap> mProbe;
        LLVector3d mOrigin;
        F32 mRadius = 0.f;
        /// Kept only until what replaces it is captured: mWaitFor when it
        /// moved there, else whatever covers it once its area has settled.
        U64 mSource = 0;        ///< what its space was placed from: tiles' content, else the space's signature
        bool mRetiring = false;
        LLPointer<LLReflectionMap> mWaitFor;
    };
    struct Parts
    {
        U64 mSignature = 0;
        std::vector<skworld::ProbePart> mParts;
        bool mPlaced = false;   ///< placed under mSignature, even to nothing
        bool mSeen = false;
        S32 mIndex = -1;        ///< this space's number within an update, for the budget's per-space rule
    };

    /// A space's parts as a worker placed them.
    struct Placed
    {
        U64 mKey = 0;           ///< the tiles' content key, 0 when they have none
        LLUUID mMesh;
        U32 mSpace = 0;
        LLVector3 mAnchor;
        U64 mSignature = 0;
        std::vector<skworld::ProbePart> mParts;
    };
    /// What the workers finished, taken on the main thread.
    struct WorkerResults
    {
        std::mutex mMutex;
        std::vector<Placed> mPlaced;
    };
    void takeWorkerResults();

    std::vector<Held> mHeld;
    skworld::ProbePartsCache mCache;
    std::shared_ptr<WorkerResults> mResults;
    std::set<std::pair<LLUUID, U32>> mPlacing;  ///< spaces a worker is placing; their old parts stand
    std::map<std::pair<LLUUID, U32>, Parts> mParts;
    F64 mNextUpdate = 0.0;
    // The census's solid bodies as a tile lattice for the candidates' point
    // tests, rebuilt only when the census publishes a new scan;
    // mSolidsCensus is the scan it was built from.
    skworld::SolidLattice mSolids;
    std::shared_ptr<const SKCensusSnapshot> mSolidsCensus;

    // stats
    U32 mCandidates = 0;
    U32 mClearedOut = 0;    ///< dropped to clear a placed probe
    U32 mOverBudget = 0;    ///< candidates the budget dropped: crowded, past one per space, or past the cap
    U32 mMoves = 0;
    U32 mResizes = 0;       ///< held probes given a new radius, capture kept
    U32 mStaled = 0;        ///< held probes kept over changed content, queued for a lazy recapture
    U32 mCreated = 0;
    U32 mCandidatesAdded = 0;   ///< threshold bubbles the portal pass stood up
    U32 mPlaced = 0;
    U32 mCacheHits = 0;     ///< spaces given their cached parts, last update
    U32 mCachePlaced = 0;   ///< spaces placed anew, last update
    bool mDemanding = false;    ///< the air cells' demand is held
    F32 mLastMs = 0.f;
};

#endif // SK_ROOMPROBES_H
