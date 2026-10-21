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

#include <algorithm>
#include <cmath>
#include <limits>
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

    /// What a tile's records are signed by, in whatever order they come: a sum, so two equal parts do not cancel.
    U64 recordSignature(const std::vector<RecordPtr>& records);

    /// Query a TileKey-ordered map without visiting tiles outside the rectangle.
    /// Bounds are inclusive, like overlapsXY: a query on an edge sees both tiles.
    /// The callback must not change the map. Empty coordinate columns are skipped.
    template<class TileMap, class Fn>
    void visitTilesIn(const TileMap& tiles, const LLVector3d& origin,
                      const LLVector3d& min, const LLVector3d& max, Fn&& fn)
    {
        if (!(min.mdV[VX] <= max.mdV[VX] && min.mdV[VY] <= max.mdV[VY])) return;
        constexpr S32 low = std::numeric_limits<S32>::min();
        constexpr S32 high = std::numeric_limits<S32>::max();
        const auto index = [](F64 value)
        {
            return static_cast<S32>(std::clamp(value, static_cast<F64>(low), static_cast<F64>(high)));
        };
        const S32 x0 = index(std::ceil((min.mdV[VX] - origin.mdV[VX]) / TILE_M) - 1.);
        const S32 y0 = index(std::ceil((min.mdV[VY] - origin.mdV[VY]) / TILE_M) - 1.);
        const S32 x1 = index(std::floor((max.mdV[VX] - origin.mdV[VX]) / TILE_M));
        const S32 y1 = index(std::floor((max.mdV[VY] - origin.mdV[VY]) / TILE_M));
        auto column = tiles.lower_bound(TileKey { x0, low });
        while (column != tiles.end() && column->first.mX <= x1)
        {
            const S32 x = column->first.mX;
            for (auto it = tiles.lower_bound(TileKey { x, y0 });
                 it != tiles.end() && it->first.mX == x && it->first.mY <= y1; ++it)
            {
                fn(it->first, it->second);
            }
            column = tiles.upper_bound(TileKey { x, high });
        }
    }

    bool overlapsXY(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax);

    /// The records each lattice tile can see, bucketed by the cells their
    /// padded AABB touches. A tile is one cell, so a lookup is one hash: the
    /// per-tile scan over every record in the region is gone, and a record is
    /// kept only by the tiles it can actually reach.
    struct RecordGrid
    {
        std::unordered_map<TileKey, std::vector<RecordPtr>, TileKeyHash> mCells;

        void insert(const RecordPtr& r, F32 pad);
        /// Only into the cells the XY box [clip_min, clip_max] touches.
        void insert(const RecordPtr& r, F32 pad, const LLVector3d& clip_min, const LLVector3d& clip_max);
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
    /// False, with out empty, when that is more than a mesh can hold.
    bool layAirborne(const Cluster& cluster, const LLVector3d& mesh_origin, F32 pad, std::vector<LaidTile>& out);

    /// A connected region's land.
    struct RegionSquare
    {
        LLVector3d mOrigin;
        F64 mWidth = 256.0;
    };

    /// How far past its edge a region meshes the void, whole tiles.
    S32 voidReachTiles(F32 void_margin);

    /// The regions' bounding box grown by the void a region meshes: no mesh
    /// has a tile past it, so clusters are clipped to it.
    ClusterWindow regionWindow(const std::vector<RegionSquare>& regions, F32 void_margin);

    /// A region's own mesh: its tiles over its land with every grounded record
    /// reaching over that land but the surrounds, plus the void within
    /// void_margin of its edge that no region's land covers and no other
    /// region is nearer to, with every grounded record reaching there
    /// whichever region it was rezzed in. False, with out empty, when that is
    /// more than a mesh can hold.
    bool layRegion(const LLVector3d& region_origin, F64 width, const LLVector3d& mesh_origin, const std::vector<RecordPtr>& ground_records, F32 pad,
                   F32 void_margin, const std::vector<RegionSquare>& regions, std::vector<LaidTile>& out);

    /// Split a tile's records into shapes and cuts, and set the z range the
    /// tile rasterises from them and the terrain already in the input.
    void finishTileInput(const std::vector<RecordPtr>& records, const LLVector3d& mesh_origin, TileBuildInput& input);

    /// The tile's linkset roots that cue_of gives a NameCue, into mRootCues.
    void tileRootCues(const std::function<U8(const LLUUID&)>& cue_of, TileBuildInput& input);

    /// A mesh's Detour mesh for one class, mesh-local, on the 32 m lattice. Null on failure.
    dtNavMesh* allocMeshNav();

    /// Put next in Detour tile (tx, ty)'s place, the caller owning the bytes of
    /// both: Detour gets them without DT_TILE_FREE_DATA. held is the tile's
    /// bytes now in Detour, or empty. On success held takes next and the old
    /// bytes go; when Detour refuses next, the old tile goes back under its old
    /// ref and next is left as it was. Empty next takes the tile out.
    bool swapDetourTile(dtNavMesh& nav, S32 tx, S32 ty, std::vector<U8>& held, std::vector<U8>& next);
}

#endif // SK_NAVLAYOUT_H
