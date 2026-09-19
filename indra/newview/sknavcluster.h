/**
 * @file sknavcluster.h
 * @brief Clusters: connected components over census records, tiled on the global lattice.
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

#ifndef SK_NAVCLUSTER_H
#define SK_NAVCLUSTER_H

#include "sknavtypes.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace sknav
{
    /// A structure: every record whose inflated AABB touches another's, linksets
    /// never split. The id is the smallest root UUID in the component, so it is
    /// stable across rebuilds and sessions.
    struct Cluster
    {
        LLUUID mId;
        std::vector<RecordPtr> mRecords;
        LLVector3d mMin;
        LLVector3d mMax;
        /// The lattice tiles the AABB overlaps, with the signature of what each
        /// one sees: the member records intersecting it.
        std::map<TileKey, U64> mTiles;
    };

    typedef std::shared_ptr<const Cluster> ClusterPtr;

    struct ClusterSet
    {
        std::vector<ClusterPtr> mClusters;
        /// Clusters whose AABB overlaps each lattice tile, for lookups by position.
        std::unordered_map<TileKey, std::vector<ClusterPtr>, TileKeyHash> mByTile;
        F64 mTile = TILE_M;     ///< the pitch mByTile was built with

        /// Clusters whose AABB contains the point.
        void at(const LLVector3d& p, std::vector<ClusterPtr>& out) const;
        const Cluster* find(const LLUUID& id) const;
    };

    /// Build the clusters of a census snapshot. DYNAMIC records are left out.
    /// gap is the AABB inflation in metres; tile is the lattice pitch (TILE_M in
    /// the viewer, smaller in tests).
    ClusterSet cluster(const std::vector<RecordPtr>& records, F64 gap, F64 tile = TILE_M);
}

#endif // SK_NAVCLUSTER_H
