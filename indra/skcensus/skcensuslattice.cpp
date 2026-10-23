/**
 * @file skcensuslattice.cpp
 * @brief The global tile lattice and how a region's or a cluster's tiles are laid on it.
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


#include "linden_common.h"

#include "skcensuslattice.h"

namespace skworld
{
    LLVector3d latticeOrigin(const LLVector3d& p)
    {
        const TileKey k = tileOf(p);
        return LLVector3d(k.mX * TILE_M, k.mY * TILE_M, 0.0);
    }

    LLVector3d meshTileMin(const LLVector3d& origin, const TileKey& key)
    {
        return LLVector3d(origin.mdV[VX] + key.mX * TILE_M, origin.mdV[VY] + key.mY * TILE_M, 0.0);
    }

    bool overlapsXY(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        return !(amin.mdV[VX] > bmax.mdV[VX] || bmin.mdV[VX] > amax.mdV[VX] || amin.mdV[VY] > bmax.mdV[VY] || bmin.mdV[VY] > amax.mdV[VY]);
    }

    void RecordGrid::insert(const RecordPtr& r, F32 pad)
    {
        insert(r, pad, LLVector3d(-1e12, -1e12, 0), LLVector3d(1e12, 1e12, 0));
    }

    void RecordGrid::insert(const RecordPtr& r, F32 pad, const LLVector3d& clip_min, const LLVector3d& clip_max)
    {
        LLVector3d pmin = r->mMin - LLVector3d(pad, pad, 0);
        LLVector3d pmax = r->mMax + LLVector3d(pad, pad, 0);
        for (S32 a = VX; a <= VY; ++a)
        {
            pmin.mdV[a] = std::max(pmin.mdV[a], clip_min.mdV[a]);
            pmax.mdV[a] = std::min(pmax.mdV[a], clip_max.mdV[a]);
            if (pmin.mdV[a] > pmax.mdV[a])
            {
                return;
            }
        }
        const TileKey k0 = tileOf(pmin), k1 = tileOf(pmax);
        for (S32 y = k0.mY; y <= k1.mY; ++y)
        {
            for (S32 x = k0.mX; x <= k1.mX; ++x)
            {
                mCells[{ x, y }].push_back(r);
            }
        }
    }

    void RecordGrid::query(const TileKey& key, std::vector<RecordPtr>& out) const
    {
        const auto it = mCells.find(key);
        if (it == mCells.end())
        {
            out.clear();
            return;
        }
        out = it->second;
    }

    S32 voidReachTiles(F32 void_margin)
    {
        return std::clamp(static_cast<S32>(std::ceil(void_margin / TILE_M)), 0, 16);
    }

    ClusterWindow regionWindow(const std::vector<RegionSquare>& regions, F32 void_margin)
    {
        ClusterWindow w;
        if (regions.empty())
        {
            return w;
        }
        const F64 reach = voidReachTiles(void_margin) * TILE_M;
        w.mMin = regions.front().mOrigin;
        w.mMax = regions.front().mOrigin;
        for (const RegionSquare& r : regions)
        {
            for (S32 a = VX; a <= VY; ++a)
            {
                w.mMin.mdV[a] = std::min(w.mMin.mdV[a], r.mOrigin.mdV[a] - reach);
                w.mMax.mdV[a] = std::max(w.mMax.mdV[a], r.mOrigin.mdV[a] + r.mWidth + reach);
            }
        }
        // A millimetre short, so a box ending on the far edge stops in the window's last tile.
        w.mMax -= LLVector3d(0.001, 0.001, 0.0);
        w.mMin.mdV[VZ] = w.mMax.mdV[VZ] = 0.0;
        return w;
    }

    bool layAirborne(const Cluster& cluster, const LLVector3d& mesh_origin, F32 pad, std::vector<LaidTile>& out)
    {
        out.clear();
        if (cluster.mTiles.size() > static_cast<size_t>(MAX_TILES_PER_MESH))
        {
            return false;
        }
        const S32 ox = tileIndex(mesh_origin.mdV[VX]), oy = tileIndex(mesh_origin.mdV[VY]);
        // Only the cells the cluster tiles: a record far bigger than its
        // cluster's clipped box is not spread over cells nobody queries.
        const LLVector3d reach(TILE_M, TILE_M, 0.0);
        RecordGrid grid;
        for (const RecordPtr& r : cluster.mRecords)
        {
            grid.insert(r, pad, cluster.mMin - reach, cluster.mMax + reach);
        }
        for (const auto& entry : cluster.mTiles)
        {
            LaidTile t;
            t.mKey = { entry.first.mX - ox, entry.first.mY - oy };
            grid.query(entry.first, t.mRecords);
            out.push_back(std::move(t));
        }
        return true;
    }

    namespace
    {
        F64 distanceToSquare(const LLVector3d& p, const RegionSquare& r)
        {
            const F64 dx = llmax(r.mOrigin.mdV[VX] - p.mdV[VX], p.mdV[VX] - (r.mOrigin.mdV[VX] + r.mWidth), 0.0);
            const F64 dy = llmax(r.mOrigin.mdV[VY] - p.mdV[VY], p.mdV[VY] - (r.mOrigin.mdV[VY] + r.mWidth), 0.0);
            return std::sqrt(dx * dx + dy * dy);
        }

        // The region a patch of void belongs to: the nearest, and on a tie
        // the one with the lower origin, so every region agrees and none is
        // left out of an inside corner.
        const RegionSquare* voidOwner(const LLVector3d& p, const std::vector<RegionSquare>& regions)
        {
            const RegionSquare* best = nullptr;
            F64 best_d = 0.0;
            for (const RegionSquare& r : regions)
            {
                const F64 d = distanceToSquare(p, r);
                const bool lower = best && (r.mOrigin.mdV[VX] < best->mOrigin.mdV[VX] ||
                                            (r.mOrigin.mdV[VX] == best->mOrigin.mdV[VX] && r.mOrigin.mdV[VY] < best->mOrigin.mdV[VY]));
                if (!best || d < best_d || (d == best_d && lower))
                {
                    best = &r;
                    best_d = d;
                }
            }
            return best;
        }
    }

    bool layRegion(const LLVector3d& region_origin, F64 width, const LLVector3d& mesh_origin, const std::vector<RecordPtr>& ground_records, F32 pad,
                   F32 void_margin, const std::vector<RegionSquare>& regions, std::vector<LaidTile>& out)
    {
        out.clear();
        // Where a record stands on a region's land it is meshed with that
        // land, so a structure and the ground under it stay one mesh even when
        // the structure was rezzed across the border. The void is meshed by
        // the region nearest to it, with whatever hangs out there from any
        // region: an estate's corner void takes every neighbour's surrounds.
        const S32 reach = voidReachTiles(void_margin);
        const S32 per_edge = static_cast<S32>(std::ceil(width / TILE_M));
        const LLVector3d land_min = region_origin - LLVector3d(pad, pad, 0);
        const LLVector3d land_max = region_origin + LLVector3d(width + pad, width + pad, 0);
        const LLVector3d margin(reach * TILE_M + pad, reach * TILE_M + pad, 0);
        const LLVector3d void_min = region_origin - margin;
        const LLVector3d void_max = region_origin + LLVector3d(width, width, 0) + margin;
        const S32 ox = tileIndex(mesh_origin.mdV[VX]), oy = tileIndex(mesh_origin.mdV[VY]);
        RecordGrid land_grid;   // reaches over this region's land
        RecordGrid void_grid;   // reaches into the void about it
        S32 x0 = 0, x1 = per_edge - 1, y0 = 0, y1 = per_edge - 1;
        for (const RecordPtr& r : ground_records)
        {
            // A surround's rendered shape is for the void alone: over land a
            // phantom megasculpty would roof the region, and a solid one
            // already comes in as its physics record.
            if (r->mRole != Role::SURROUND && overlapsXY(r->mMin, r->mMax, land_min, land_max))
            {
                land_grid.insert(r, pad, land_min, land_max);
            }
            if (overlapsXY(r->mMin, r->mMax, void_min, void_max))
            {
                void_grid.insert(r, pad, void_min, void_max);
                x0 = std::min(x0, tileIndex(r->mMin.mdV[VX]) - ox);
                x1 = std::max(x1, tileIndex(r->mMax.mdV[VX]) - ox);
                y0 = std::min(y0, tileIndex(r->mMin.mdV[VY]) - oy);
                y1 = std::max(y1, tileIndex(r->mMax.mdV[VY]) - oy);
            }
        }
        x0 = std::max(x0, -reach);
        y0 = std::max(y0, -reach);
        x1 = std::min(x1, per_edge - 1 + reach);
        y1 = std::min(y1, per_edge - 1 + reach);

        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                LaidTile t;
                t.mKey = { x, y };
                const TileKey global{ ox + x, oy + y };
                t.mOverLand = x >= 0 && x < per_edge && y >= 0 && y < per_edge;
                if (t.mOverLand)
                {
                    land_grid.query(global, t.mRecords);
                }
                else
                {
                    // Land of another region, or void nearer another region,
                    // is that region's to mesh, so no patch is meshed twice;
                    // an empty void has nothing to mesh.
                    const LLVector3d centre = meshTileMin(mesh_origin, t.mKey) + LLVector3d(TILE_M * 0.5, TILE_M * 0.5, 0.0);
                    const RegionSquare* owner = voidOwner(centre, regions);
                    if (owner && owner->mOrigin != region_origin)
                    {
                        continue;
                    }
                    void_grid.query(global, t.mRecords);
                    if (t.mRecords.empty())
                    {
                        continue;
                    }
                }
                out.push_back(std::move(t));
            }
        }
        if (out.size() > static_cast<size_t>(MAX_TILES_PER_MESH))
        {
            out.clear();
            return false;
        }
        return true;
    }

}
