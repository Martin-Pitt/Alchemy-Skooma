/**
 * @file sknavroomprobes.h
 * @brief Automatic reflection probe spheres for the navmesh's rooms, in place of the octree's.
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

#ifndef SK_NAVROOMPROBES_H
#define SK_NAVROOMPROBES_H

#include "sknavprobes.h"
#include "sknavrooms.h"

#include "llpointer.h"

#include <map>
#include <string>
#include <vector>

class LLReflectionMap;
struct SKCensusSnapshot;

/// Holds a full probe per walkable space near the camera — its extra parts
/// only while the probe budget has room for twice the candidates, and never
/// two spheres closer than the budget's spacing — matched to their parts by
/// nearest origin, before the residual grid pass. Placed probes
/// always win: a sphere shrinks clear of their influence or is dropped.
/// Only at probe level 3, where octree probes exist. Main thread only.
class SKNavRoomProbes
{
public:
    SKNavRoomProbes();
    ~SKNavRoomProbes();

    void update(const SKNavWorldViewPtr& view);
    /// Let every full probe go and release its claim on residual coverage.
    void clear();
    std::string statsLine() const;

private:
    struct Held
    {
        LLPointer<LLReflectionMap> mProbe;
        LLVector3d mOrigin;
        F32 mRadius = 0.f;
    };
    struct Parts
    {
        U64 mSignature = 0;
        std::vector<sknav::ProbePart> mParts;
        bool mSeen = false;
        S32 mIndex = -1;        ///< this space's number within an update, for the budget's per-space rule
    };

    std::vector<Held> mHeld;
    std::map<std::pair<LLUUID, U32>, Parts> mParts;
    F64 mNextUpdate = 0.0;
    // The census's solid bodies as a tile lattice for the candidates' point
    // tests, rebuilt only when the census publishes a new scan;
    // mSolidsCensus is the scan it was built from.
    sknav::SolidLattice mSolids;
    std::shared_ptr<const SKCensusSnapshot> mSolidsCensus;

    // stats
    U32 mCandidates = 0;
    U32 mClearedOut = 0;    ///< dropped to clear a placed probe
    U32 mOverBudget = 0;    ///< candidates the budget dropped: crowded, past one per space, or past the cap
    U32 mMoves = 0;
    U32 mCreated = 0;
    U32 mPlaced = 0;
    F32 mLastMs = 0.f;
};

#endif // SK_NAVROOMPROBES_H
