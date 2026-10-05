/**
 * @file sknavlayout.h
 * @brief Which meshes and tiles a census makes, and what each tile is built from.
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

#ifndef SK_NAVLAYOUT_H
#define SK_NAVLAYOUT_H

#include "sknavbuild.h"
#include "sknavcluster.h"
#include "sknavtypes.h"

#include <functional>
#include <unordered_map>
#include <vector>

class dtNavMesh;

namespace sknav
{
    /// Detour packs tile, poly and salt bits into 32: 10 + 12 leaves the 10 salt
    /// bits init insists on. 1024 tiles is a square kilometre per structure.
    constexpr S32 MAX_TILES_PER_MESH = 1024;
    constexpr S32 MAX_POLYS_PER_TILE = 4096;

    /// How far past a tile geometry and terrain are taken: the build's border
    /// for the widest class, plus a metre of terrain grid.
    F32 tilePad(const TileBuildInput& params);

    /// The lattice origin a mesh gets: the tile corner at or below the point, z zero.
    LLVector3d latticeOrigin(const LLVector3d& p);

    /// A mesh tile's minimum corner, global, z zero.
    LLVector3d meshTileMin(const LLVector3d& origin, const TileKey& key);

    bool overlapsXY(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax);

    /// The records each lattice tile can see, bucketed by the cells their
    /// padded AABB touches. A tile is one cell, so a lookup is one hash: the
    /// per-tile scan over every record in the region is gone, and a record is
    /// kept only by the tiles it can actually reach.
    struct RecordGrid
    {
        std::unordered_map<TileKey, std::vector<RecordPtr>, TileKeyHash> mCells;

        void insert(const RecordPtr& r, F32 pad);
        void query(const TileKey& key, std::vector<RecordPtr>& out) const;
    };

    /// The key a mesh tile's air gives the sky map under.
    U64 skyContributor(const LLUUID& mesh, const TileKey& key);

    /// One tile a mesh wants, with what it rasterises.
    struct LaidTile
    {
        TileKey mKey;               ///< relative to the mesh origin
        bool mOverLand = true;      ///< false for the void past a region's edge
        std::vector<RecordPtr> mRecords;
    };

    /// A cluster clear of the ground: every lattice tile its box overlaps.
    void layAirborne(const Cluster& cluster, const LLVector3d& mesh_origin, F32 pad, std::vector<LaidTile>& out);

    /// A region's own mesh: its tiles over its land with every grounded record
    /// reaching over that land, plus the void past its edge within void_margin
    /// as far as the region's own records reach, where no other region's land
    /// is (land_at says where there is).
    void layRegion(const LLVector3d& region_origin, F64 width, const LLVector3d& mesh_origin, const std::vector<RecordPtr>& ground_records, F32 pad,
                   F32 void_margin, const std::function<bool(const LLVector3d&)>& land_at, std::vector<LaidTile>& out);

    /// Split a tile's records into shapes and cuts, and set the z range the
    /// tile rasterises from them and the terrain already in the input.
    void finishTileInput(const std::vector<RecordPtr>& records, const LLVector3d& mesh_origin, TileBuildInput& input);

    /// The tile's linkset roots that cue_of gives a NameCue, into mRootCues.
    void tileRootCues(const std::function<U8(const LLUUID&)>& cue_of, TileBuildInput& input);

    /// A mesh's Detour mesh for one class, mesh-local, on the 32 m lattice. Null on failure.
    dtNavMesh* allocMeshNav();
}

#endif // SK_NAVLAYOUT_H
