/**
 * @file skcensusclusterspots.h
 * @brief Where a cluster's fill probes go: one spot per occupied cell, per region, on its largest clump.
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

#ifndef SK_CENSUSCLUSTERSPOTS_H
#define SK_CENSUSCLUSTERSPOTS_H

#include "skcensuscluster.h"

#include <vector>

namespace skworld
{
    /// Footprints this close in xy belong to one clump.
    constexpr F64 CLUMP_TOUCH_M = 0.5;
    /// A cluster part is gridded into at most this many cells along x and y.
    constexpr U32 MAX_SPOT_CELLS = 3;

    /// One clump of one cell: its members' bounds clipped to the cell, and its
    /// footprint's area-weighted centre in xy.
    struct ClusterSpot
    {
        LLVector3d mMin;
        LLVector3d mMax;
        F64 mX = 0.0;
        F64 mY = 0.0;
    };

    /// Split the cluster's records by region, grid each part's own box in cells
    /// near cell_m, and append the largest footprint clump of every occupied cell.
    void clusterSpots(const Cluster& cluster, F64 cell_m, std::vector<ClusterSpot>& out);
}

#endif // SK_CENSUSCLUSTERSPOTS_H
