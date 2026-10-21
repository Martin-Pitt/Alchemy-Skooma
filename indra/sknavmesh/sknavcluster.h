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
        U64 mHash = 0;      ///< the sum of its members' recordHash: moves when any member's content does
        /// The lattice tiles the AABB overlaps, with the signature of what each
        /// one sees: the member records intersecting it.
        std::map<TileKey, U64> mTiles;
    };

    typedef std::shared_ptr<const Cluster> ClusterPtr;

    /// The XY box clusters are joined and tiled within; the default clips nothing.
    struct ClusterWindow
    {
        LLVector3d mMin{ 1.0, 1.0, 0.0 };
        LLVector3d mMax{ 0.0, 0.0, 0.0 };

        bool clips() const { return mMin.mdV[VX] <= mMax.mdV[VX] && mMin.mdV[VY] <= mMax.mdV[VY]; }
    };

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

    /// Build the clusters of a census snapshot. DYNAMIC records, and records
    /// every class leaves out, are left out.
    /// gap is the AABB inflation in metres; tile is the lattice pitch (TILE_M in
    /// the viewer, smaller in tests). A record's box is cut to the window before
    /// it joins or tiles anything, so a kilometre-scale megaprim neither makes
    /// tiles past it nor links things the window never sees.
    ClusterSet cluster(const std::vector<RecordPtr>& records, F64 gap, F64 tile = TILE_M, const ClusterWindow& window = ClusterWindow());
}

#endif // SK_NAVCLUSTER_H
