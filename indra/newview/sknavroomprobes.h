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
#include <tuple>

class LLReflectionMap;

/// Holds one automatic sphere per part of each indoor space near the
/// camera, keyed (mesh, space id, part), and tells the probe manager which
/// octree probes those parts replace. Placed probes always win: a sphere
/// shrinks clear of their influence or is dropped. Main thread only.
class SKNavRoomProbes
{
public:
    SKNavRoomProbes();
    ~SKNavRoomProbes();

    void update(const SKNavWorldViewPtr& view);
    /// Let every probe go and replace no octree probe.
    void clear();
    std::string statsLine() const;

private:
    typedef std::tuple<LLUUID, U32, U32> Key;
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
    };

    std::map<Key, Held> mHeld;
    std::map<std::pair<LLUUID, U32>, Parts> mParts;
    F64 mNextUpdate = 0.0;

    // stats
    U32 mCandidates = 0;
    U32 mClearedOut = 0;    ///< dropped to clear a placed probe
    U32 mOverBudget = 0;
    U32 mMoves = 0;
    U32 mPlaced = 0;
    F32 mLastMs = 0.f;
};

#endif // SK_NAVROOMPROBES_H
