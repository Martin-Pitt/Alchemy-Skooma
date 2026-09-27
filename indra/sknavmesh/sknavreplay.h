/**
 * @file sknavreplay.h
 * @brief A census replayed headless: clusters, meshes and tiles, the tile builds, then enclosure, spaces and wind per mesh.
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

#ifndef SK_NAVREPLAY_H
#define SK_NAVREPLAY_H

#include "sknavcensusio.h"
#include "sknavcluster.h"
#include "sknavskymap.h"
#include "sknavspaces.h"
#include "sknavwind.h"

#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class dtNavMesh;

namespace sknav
{
    struct ReplayOptions
    {
        S32 mThreads = 0;       ///< tile builds at once; 0 is one per core
        /// Enclosure passes per mesh. The first runs every mesh alone; each
        /// after runs the meshes that touch another with its neighbours'
        /// answers and its own last ones, as the viewer's re-runs do.
        S32 mRounds = 2;
    };

    struct ReplayTile
    {
        TileKey mKey;
        LLVector3d mMin;            ///< global, the box the viewer gives the tile
        LLVector3d mMax;
        bool mOverLand = true;
        std::shared_ptr<const TileBuildInput> mInput;
        std::shared_ptr<const TileBuildResult> mResult;
    };

    /// One mesh as SKNavMesh lays it out, with what its passes made.
    struct ReplayMesh
    {
        LLUUID mId;                 ///< the cluster's, or the region's for its own mesh
        bool mTerrain = false;
        LLVector3d mOrigin;
        std::map<TileKey, ReplayTile> mTiles;
        dtNavMesh* mNav[NUM_CLASSES] = { nullptr, nullptr };
        LLVector3d mAirMin;         ///< the footprint of its tiles with air
        LLVector3d mAirMax;
        EnclosureProductPtr mEnclosure;
        RoomsProductPtr mRooms;
        WindProductPtr mWind;

        ReplayMesh() = default;
        ReplayMesh(const ReplayMesh&) = delete;
        ReplayMesh& operator=(const ReplayMesh&) = delete;
        ~ReplayMesh();
    };

    struct ReplayResult
    {
        ClusterSet mClusters;
        /// The replay's clusters against the viewer's: the same members, other
        /// members under the same id, or an id only one side has.
        U32 mClustersSame = 0;
        U32 mClustersDiffer = 0;
        U32 mClustersOnlyViewer = 0;
        U32 mClustersOnlyReplay = 0;
        std::vector<std::string> mClusterNotes;
        std::vector<std::unique_ptr<ReplayMesh>> mMeshes;   ///< sorted by id, as the viewer keeps them
        std::shared_ptr<const SkyMap> mSky;
        U32 mTiles = 0;
        U32 mEmpty = 0;
        U32 mFailed = 0;
        /// Wall-clock milliseconds per stage, in order.
        std::vector<std::pair<std::string, F64>> mStages;
    };

    /// clusters -> meshes and tiles -> terrain -> buildTile -> enclosure -> spaces -> wind.
    void replayCensus(const CensusFile& census, const ReplayOptions& options, ReplayResult& out);

    /// The result in the viewer's navmesh export format, so its readers work unchanged.
    void writeReplayExport(std::ostream& file, const CensusFile& census, const ReplayResult& result);

    /// A line per stage, and the counts.
    std::string replaySummary(const ReplayResult& result);
}

#endif // SK_NAVREPLAY_H
