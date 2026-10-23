/**
 * @file skclusterprobes.h
 * @brief Fill reflection probes over the census's clusters, between the default probe and the rest.
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

#ifndef SK_CLUSTERPROBES_H
#define SK_CLUSTERPROBES_H

#include "llpointer.h"
#include "v3dmath.h"

#include <memory>
#include <string>
#include <vector>

class LLReflectionMap;
class LLReflectionMapManager;
struct SKCensusSnapshot;
namespace skworld { class SolidLattice; }

/// Holds a fill probe per cell of each census cluster near the camera: the
/// cluster's footprint cut into a 1x1 up to 3x3 grid, one sphere per occupied
/// cell, high in the cell's occupied height and lifted clear of solids, and
/// large enough to cover the whole cell. The shader blends them over the
/// default probe and under every other probe (skProbeTier), so a surface
/// anywhere in a structure reflects the structure, not the sky probe above
/// the camera. Main thread only.
class SKClusterProbes
{
public:
    /// solids is the census scan's lattice, or null while it is not built yet.
    void update(LLReflectionMapManager& manager, const std::shared_ptr<const SKCensusSnapshot>& census,
                const skworld::SolidLattice* solids);
    /// Let every probe go.
    void clear();
    std::string statsLine() const;

private:
    struct Held
    {
        LLPointer<LLReflectionMap> mProbe;
        LLVector3d mOrigin;
        F32 mRadius = 0.f;
    };
    std::vector<Held> mHeld;
    F64 mNextUpdate = 0.0;

    // stats
    U32 mClusters = 0;      ///< clusters in reach and large enough
    U32 mCandidates = 0;
    U32 mCreated = 0;
    U32 mMoved = 0;
};

#endif // SK_CLUSTERPROBES_H
