/**
 * @file sknavlayout.cpp
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

#include "linden_common.h"

#include "sknavlayout.h"

#include "DetourAlloc.h"
#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>

namespace sknav
{
    F32 tilePad(const TileBuildInput& params)
    {
        F32 border = 0.f;
        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            if (params.mClasses[c])
            {
                border = std::max(border, borderCells(params.mParams[c].mRadius, params.mCell) * params.mCell);
            }
        }
        // WEATHER reads its air past the tile, and a seam column there needs its ground.
        if (params.mAir)
        {
            border = std::max(border, params.mAirPad * params.mCell);
        }
        return border + 1.f;
    }

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
        const LLVector3d pmin = r->mMin - LLVector3d(pad, pad, 0);
        const LLVector3d pmax = r->mMax + LLVector3d(pad, pad, 0);
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

    U64 skyContributor(const LLUUID& mesh, const TileKey& key)
    {
        U64 id = SEED;
        for (U8 b : mesh.mData)
        {
            id = mix(id, b);
        }
        return mix(mix(id, static_cast<U32>(key.mX)), static_cast<U32>(key.mY));
    }

    void layAirborne(const Cluster& cluster, const LLVector3d& mesh_origin, F32 pad, std::vector<LaidTile>& out)
    {
        out.clear();
        const S32 ox = tileIndex(mesh_origin.mdV[VX]), oy = tileIndex(mesh_origin.mdV[VY]);
        RecordGrid grid;
        for (const RecordPtr& r : cluster.mRecords)
        {
            grid.insert(r, pad);
        }
        for (const auto& entry : cluster.mTiles)
        {
            LaidTile t;
            t.mKey = { entry.first.mX - ox, entry.first.mY - oy };
            grid.query(entry.first, t.mRecords);
            out.push_back(std::move(t));
        }
    }

    void layRegion(const LLVector3d& region_origin, F64 width, const LLVector3d& mesh_origin, const std::vector<RecordPtr>& ground_records, F32 pad,
                   F32 void_margin, const std::function<bool(const LLVector3d&)>& land_at, std::vector<LaidTile>& out)
    {
        out.clear();
        // A record's own region names it for the void past every edge, but
        // where it stands on another region's land it is meshed with that
        // land, so a structure and the ground under it stay one mesh even when
        // the structure was rezzed across the border.
        const LLVector3d land_min = region_origin - LLVector3d(pad, pad, 0);
        const LLVector3d land_max = region_origin + LLVector3d(width + pad, width + pad, 0);
        const S32 ox = tileIndex(mesh_origin.mdV[VX]), oy = tileIndex(mesh_origin.mdV[VY]);
        std::vector<RecordPtr> land_records;    // reaches over this region's land
        std::vector<RecordPtr> own_records;     // this region's own, for the void
        for (const RecordPtr& r : ground_records)
        {
            if (overlapsXY(r->mMin, r->mMax, land_min, land_max))
            {
                land_records.push_back(r);
            }
            if (r->mOrigin == region_origin)
            {
                own_records.push_back(r);
            }
        }
        const S32 per_edge = static_cast<S32>(std::ceil(width / TILE_M));
        RecordGrid land_grid;
        for (const RecordPtr& r : land_records)
        {
            land_grid.insert(r, pad);
        }
        RecordGrid void_grid;
        for (const RecordPtr& r : own_records)
        {
            void_grid.insert(r, pad);
        }

        // The region's own tiles, plus the void past its edge as far as its
        // own records reach, within the margin: the surrounds and the things
        // hanging off the sim are out there, and the void is exactly where they are.
        const S32 reach = std::clamp(static_cast<S32>(std::ceil(void_margin / TILE_M)), 0, 16);
        S32 x0 = 0, x1 = per_edge - 1, y0 = 0, y1 = per_edge - 1;
        for (const RecordPtr& r : own_records)
        {
            x0 = std::min(x0, tileIndex(r->mMin.mdV[VX]) - ox);
            x1 = std::max(x1, tileIndex(r->mMax.mdV[VX]) - ox);
            y0 = std::min(y0, tileIndex(r->mMin.mdV[VY]) - oy);
            y1 = std::max(y1, tileIndex(r->mMax.mdV[VY]) - oy);
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
                    // A tile over another region's land belongs to that
                    // region's mesh, records and all, so the same patch is
                    // not meshed twice; an empty void has nothing to mesh.
                    const LLVector3d tmin = meshTileMin(mesh_origin, t.mKey);
                    if (land_at(tmin + LLVector3d(TILE_M * 0.5, TILE_M * 0.5, 0.0)))
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
    }

    void tileRootCues(const std::function<U8(const LLUUID&)>& cue_of, TileBuildInput& input)
    {
        input.mRootCues.clear();
        for (const RecordPtr& r : input.mRecords)
        {
            if (r)
            {
                input.mRootCues.emplace_back(r->mRoot, U8(0));
            }
        }
        std::sort(input.mRootCues.begin(), input.mRootCues.end());
        input.mRootCues.erase(std::unique(input.mRootCues.begin(), input.mRootCues.end()), input.mRootCues.end());
        for (auto& [id, cue] : input.mRootCues)
        {
            cue = cue_of(id);
        }
        input.mRootCues.erase(std::remove_if(input.mRootCues.begin(), input.mRootCues.end(), [](const auto& c) { return c.second == CUE_NONE; }),
                              input.mRootCues.end());
    }

    void finishTileInput(const std::vector<RecordPtr>& records, const LLVector3d& mesh_origin, TileBuildInput& input)
    {
        F32 zmin = 1e9f, zmax = -1e9f;
        for (F32 z : input.mTerrain.mZ)
        {
            if (z > -900.f)
            {
                zmin = std::min(zmin, z);
                zmax = std::max(zmax, z);
            }
        }
        const F32 land_min = zmin;
        for (const RecordPtr& r : records)
        {
            // An exclusion volume is not a shape: the sim takes the navmesh out
            // of its inside. Build it as a cut, not as something to stand on.
            if (r && r->mNavRole == NavRole::EXCLUSION_VOLUME)
            {
                input.mCuts.push_back(r);
            }
            else
            {
                input.mRecords.push_back(r);
            }
        }
        for (const RecordPtr& r : records)
        {
            zmin = std::min(zmin, static_cast<F32>(r->mMin.mdV[VZ] - mesh_origin.mdV[VZ]));
            zmax = std::max(zmax, static_cast<F32>(r->mMax.mdV[VZ] - mesh_origin.mdV[VZ]));
        }
        // The land's lowest sample is the deepest ground there is: a record
        // buried below it never becomes a floor, so the heightfield does not
        // reach down for it. Whatever still falls under the surface, the tile
        // build takes out of the spans.
        if (land_min < 1e9f)
        {
            zmin = std::max(zmin, land_min);
        }
        if (zmin > zmax)
        {
            zmin = zmax = 0.f;
        }
        input.mZMin = zmin - 1.f;
        input.mZMax = zmax + 3.f;
    }

    dtNavMesh* allocMeshNav()
    {
        dtNavMesh* nav = dtAllocNavMesh();
        if (!nav)
        {
            return nullptr;
        }
        dtNavMeshParams p;
        p.orig[0] = p.orig[1] = p.orig[2] = 0.f;    // coordinates are mesh-local already
        p.tileWidth = static_cast<F32>(TILE_M);
        p.tileHeight = static_cast<F32>(TILE_M);
        p.maxTiles = MAX_TILES_PER_MESH;
        p.maxPolys = MAX_POLYS_PER_TILE;
        if (!dtStatusSucceed(nav->init(&p)))
        {
            dtFreeNavMesh(nav);
            return nullptr;
        }
        return nav;
    }
}
