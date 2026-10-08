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

#include "sknavbuildstats.h"
#include "sknavcensusio.h"
#include "sknavcluster.h"
#include "sknavskymap.h"
#include "sknavspaceproducts.h"
#include "sknavspaces.h"
#include "sknavstamp.h"
#include "sknavwind.h"

#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class dtNavMesh;
struct dtMeshTile;

namespace sknav
{
    /// Which tiles build their air. RADIUS is the viewer's rule: a tile further
    /// than the census's air radius from its camera builds no air. EVERYWHERE
    /// builds air in every tile, for offline experiments; it no longer matches
    /// a navmesh export.
    enum class AirMode : U8 { RADIUS, EVERYWHERE };

    struct ReplayOptions
    {
        AirMode mAir = AirMode::RADIUS;
        S32 mThreads = 0;       ///< tile builds at once; 0 is one per core
        /// Enclosure passes per mesh. The first runs every mesh alone; each
        /// after runs the meshes that touch another with its neighbours'
        /// answers and its own last ones, as the viewer's re-runs do.
        S32 mRounds = 2;
        /// Rounds after the first rebuild this many tiles of every mesh, as
        /// though their air had moved, and run every mesh, neighboured or not.
        U32 mDirtyTiles = 0;
        /// Every tile build measures itself, and the rooms passes are timed per mesh.
        bool mStats = false;
        /// Polygons wait for the spaces and are stamped with them, as the viewer
        /// publishes them; off, each tile is polygonised as soon as it is rasterised.
        bool mStamp = true;
        /// A tile without air of its own reads coarse air, so it has spaces to be stamped with.
        bool mCoarseAir = true;
        /// Rasters stay held past the stamp, so edits can update and stamp them again.
        bool mKeepRasters = false;
    };

    struct ReplayTile
    {
        TileKey mKey;
        LLVector3d mMin;            ///< global, the box the viewer gives the tile
        LLVector3d mMax;
        bool mOverLand = true;
        std::shared_ptr<const TileBuildInput> mInput;
        std::shared_ptr<const TileBuildResult> mResult;
        std::shared_ptr<const TileBuildStats> mStats;   ///< when the replay measures
        TileRasterPtr mRaster;      ///< held from the raster stage until the stamp, when stamping
        /// The stamp the polygons were built with, and what binds it to the spaces.
        std::shared_ptr<const TileStampInput> mStampInput;
        std::shared_ptr<const TileBinding> mBinding;
        F64 mStampMs = 0.0;         ///< reading the stamp input and stamping the spans
        /// When the replay measures a stamped build: the same raster polygonised with no stamp.
        std::shared_ptr<const TileBuildResult> mUnstamped;
        std::shared_ptr<const TileBuildStats> mUnstampedStats;
    };

    /// One run of the rooms passes over a mesh, when the replay measures.
    struct ReplayRoomsRun
    {
        S32 mRound = 0;
        U32 mTiles = 0;
        F64 mEnclosureMs = 0.0;
        F64 mSeamMs = 0.0;
        F64 mSpacesMs = 0.0;
        F64 mEnclosureStageMs[EnclosurePass::NUM_STAGES] = {};
        F64 mSpacesStageMs[SpacesPass::NUM_STAGES] = {};
    };

    /// One mesh as SKNavMesh lays it out, with what its passes made.
    struct ReplayMesh
    {
        LLUUID mId;                 ///< the cluster's, or the region's for its own mesh
        bool mTerrain = false;
        LLVector3d mOrigin;
        std::map<TileKey, ReplayTile> mTiles;
        dtNavMesh* mNav[NUM_CLASSES] = { nullptr };
        /// With mStats and mStamp: the same tiles polygonised with no stamp, for the measurements to compare.
        dtNavMesh* mUnstampedNav = nullptr;
        LLVector3d mAirMin;         ///< the footprint of its tiles with air
        LLVector3d mAirMax;
        EnclosureProductPtr mEnclosure;
        RoomsProductPtr mRooms;
        WindProductPtr mWind;
        std::vector<ReplayRoomsRun> mRoomsRuns;
        /// Per stamped Detour tile, by the user id its header carries, its polys' space ids.
        std::unordered_map<U32, std::vector<U32>> mSlotSpaces;
        /// Per slot, its polys' flights in the tile's result; null for none.
        std::unordered_map<U32, std::vector<const Flight*>> mSlotFlights;
        StoreyProduct mStoreys;

        /// A poly's space id, as the viewer's space filter reads it; 0 for none.
        U32 polySpace(const dtMeshTile* tile, U32 poly) const;
        /// A poly's flight, or null.
        const Flight* polyFlight(const dtMeshTile* tile, U32 poly) const;

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
        /// Meshes left out for wanting more than MAX_TILES_PER_MESH tiles.
        U32 mRefused = 0;
        /// Wall-clock milliseconds per stage, in order.
        std::vector<std::pair<std::string, F64>> mStages;
    };

    /// clusters -> meshes and tiles -> terrain -> buildTile -> enclosure -> spaces -> wind.
    void replayCensus(const CensusFile& census, const ReplayOptions& options, ReplayResult& out);

    /// One mesh's enclosure and spaces run again over its last products, as
    /// the viewer reruns them after tiles' air moved, with that air in place
    /// of the replay's; the replay's own products are left as they were.
    struct ReplayRoomsAgain
    {
        std::shared_ptr<EnclosureProduct> mEnclosure;
        RoomsProductPtr mRooms;
        F64 mEnclosureMs = 0.0;
        F64 mSpacesMs = 0.0;
    };
    ReplayRoomsAgain replayRoomsAgain(const CensusFile& census, const ReplayResult& result, size_t mesh, const std::map<TileKey, TileAirPtr>& air);

    /// A mesh's rooms input as the replay gives it: its doors, lifts and last product.
    std::shared_ptr<RoomsInput> replayRoomsInput(const CensusFile& census, const ReplayMesh& mesh, U64 generation);

    /// The result in the viewer's navmesh export format, so its readers work unchanged.
    void writeReplayExport(std::ostream& file, const CensusFile& census, const ReplayResult& result);

    /// A line per stage, and the counts.
    std::string replaySummary(const ReplayResult& result);
}

#endif // SK_NAVREPLAY_H
