/**
 * @file skairlayout.cpp
 * @brief What a mesh tile of air is built from, and the key its air gives the sky map.
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

#include "skairlayout.h"

#include "skcensusrecord.h"

#include <algorithm>
#include <cmath>

namespace skworld
{
    F32 tilePad(const TileInput& params)
    {
        F32 border = borderCells(params.mAvatar.mRadius, params.mCell) * params.mCell;
        // The raster reads its air past the tile, and a seam column there needs its ground.
        if (params.mAir)
        {
            border = std::max(border, params.mAirPad * params.mCell);
        }
        return border + 1.f;
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

    void tileRootCues(const std::function<U8(const LLUUID&)>& cue_of, TileInput& input)
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

    void finishTileInput(const std::vector<RecordPtr>& records, const LLVector3d& mesh_origin, TileInput& input)
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

    U64 tileContentKey(const TileInput& input)
    {
        U64 h = mix(SEED, AIR_VERSION);
        h = mixF64(h, input.mMeshOrigin.mdV[VX]);
        h = mixF64(h, input.mMeshOrigin.mdV[VY]);
        h = mixF64(h, input.mMeshOrigin.mdV[VZ]);
        h = mix(h, static_cast<U32>(input.mTileX));
        h = mix(h, static_cast<U32>(input.mTileY));
        h = mixF32(h, input.mTileSize);
        h = mixF32(h, input.mZMin);
        h = mixF32(h, input.mZMax);
        h = mixF32(h, input.mCell);
        h = mixF32(h, input.mWallMinHeight);
        for (const ClassParams* c : { &input.mAvatar, &input.mTopParams })
        {
            h = mixF32(h, c->mHeight);
            h = mixF32(h, c->mRadius);
            h = mixF32(h, c->mClimb);
            h = mixF32(h, c->mSlope);
        }
        h = mix(h, (input.mWallFaces ? 1u : 0u) | (input.mRunoff ? 2u : 0u) | (input.mAir ? 4u : 0u) | (input.mFaceOffsets ? 8u : 0u)
                       | (input.mCoarseAir ? 16u : 0u));
        h = mixF32(h, input.mRunoffMax);
        h = mix(h, static_cast<U32>(input.mAirPad));
        h = mixF32(h, input.mAirMinGap);
        for (const auto& [root, cue] : input.mRootCues)
        {
            for (U8 b : root.mData)
            {
                h = mix(h, b);
            }
            h = mix(h, cue);
        }
        const TerrainWindow& t = input.mTerrain;
        h = mixF32(h, t.mOrigin.mV[VX]);
        h = mixF32(h, t.mOrigin.mV[VY]);
        h = mixF32(h, t.mSpacing);
        h = mix(h, static_cast<U32>(t.mWidth));
        h = mix(h, static_cast<U32>(t.mHeight));
        h = mixF32(h, t.mWater);
        for (F32 z : t.mZ)
        {
            h = mixF32(h, z);
        }
        // Order-free over the records, as the tile's own signature is.
        for (const std::vector<RecordPtr>* set : { &input.mRecords, &input.mCuts })
        {
            U64 sum = 0;
            for (const RecordPtr& r : *set)
            {
                sum += r ? finalise(recordContentKey(*r)) : 0;
            }
            h = mix(h, sum);
        }
        return finalise(h);
    }
}
