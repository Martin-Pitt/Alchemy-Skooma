/**
 * @file skaircensus.cpp
 * @brief The air's section of a census file, and a census's tiles laid out as the viewer lays them, offline.
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

#include "skaircensus.h"

#include "skairlayout.h"
#include "skcensuscluster.h"
#include "skcensuslattice.h"

#include "llsd.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <ostream>

namespace skworld
{
    namespace
    {
        const char* boolText(bool b)
        {
            return b ? "true" : "false";
        }

        F32 real(const LLSD& map, const char* key, F32 fallback)
        {
            return map.has(key) ? static_cast<F32>(map[key].asReal()) : fallback;
        }

        bool flag(const LLSD& map, const char* key, bool fallback)
        {
            return map.has(key) ? map[key].asBoolean() : fallback;
        }

        /// The land under a point, from the nearest sample: good enough to
        /// ground a cluster the census did not say is grounded.
        bool landUnder(const CensusFile& census, F64 x, F64 y, F32& z)
        {
            const CensusRegion* r = census.regionAt(x, y);
            if (!r || !r->mSamples)
            {
                return false;
            }
            const S32 ix = std::clamp(static_cast<S32>(std::lround(x - r->mOrigin.mdV[VX] - r->mPhase)), 0, r->mSamples - 1);
            const S32 iy = std::clamp(static_cast<S32>(std::lround(y - r->mOrigin.mdV[VY] - r->mPhase)), 0, r->mSamples - 1);
            z = r->mZ[static_cast<size_t>(iy) * r->mSamples + ix];
            return true;
        }

        bool groundedFallback(const CensusFile& census, const Cluster& c, F32 margin)
        {
            const LLVector3d corners[5] = {
                c.mMin,
                LLVector3d(c.mMax.mdV[VX], c.mMin.mdV[VY], 0.0),
                LLVector3d(c.mMin.mdV[VX], c.mMax.mdV[VY], 0.0),
                c.mMax,
                (c.mMin + c.mMax) * 0.5,
            };
            F32 land = -1e9f;
            for (const LLVector3d& p : corners)
            {
                F32 z;
                if (landUnder(census, p.mdV[VX], p.mdV[VY], z))
                {
                    land = std::max(land, z);
                }
            }
            return static_cast<F32>(c.mMin.mdV[VZ]) - land <= margin;
        }

        bool inAirRadius(const CensusFile& census, F32 radius, const LLVector3d& tmin, const LLVector3d& tmax)
        {
            const LLVector3d& camera = census.mCamera;
            const F64 dx = std::max({ tmin.mdV[VX] - camera.mdV[VX], 0.0, camera.mdV[VX] - tmax.mdV[VX] });
            const F64 dy = std::max({ tmin.mdV[VY] - camera.mdV[VY], 0.0, camera.mdV[VY] - tmax.mdV[VY] });
            return dx * dx + dy * dy <= static_cast<F64>(radius) * radius;
        }
    }

    void writeAirParams(std::ostream& out, const AirCensusParams& params)
    {
        const TileInput& p = params.mParams;
        const ClassParams& a = p.mAvatar;
        out << "{\"tile_size\":" << p.mTileSize << ",\"cell\":" << p.mCell << ",\"wall_min_height\":" << p.mWallMinHeight
            << ",\"wall_faces\":" << boolText(p.mWallFaces) << ",\"air\":" << boolText(p.mAir)
            << ",\"air_pad\":" << p.mAirPad << ",\"air_min_gap\":" << p.mAirMinGap << ",\"face_offsets\":" << boolText(p.mFaceOffsets)
            << ",\"classes\":[{\"name\":\"avatar\",\"height\":" << a.mHeight << ",\"radius\":" << a.mRadius << ",\"climb\":" << a.mClimb
            << ",\"slope\":" << a.mSlope << "}],\"top\":{\"height\":" << p.mTopParams.mHeight << ",\"slope\":" << p.mTopParams.mSlope
            << ",\"runoff\":" << boolText(p.mRunoff) << ",\"runoff_max\":" << p.mRunoffMax << "},\"air_radius\":" << params.mAirRadius
            << ",\"grounded_margin\":" << params.mGroundedMargin << '}';
    }

    bool readAirParams(const LLSD& v, U32 version, AirCensusParams& params)
    {
        if (!v.isMap())
        {
            return false;
        }
        TileInput& p = params.mParams;
        p.mTileSize = real(v, "tile_size", p.mTileSize);
        p.mCell = real(v, "cell", p.mCell);
        p.mWallMinHeight = real(v, "wall_min_height", p.mWallMinHeight);
        p.mWallFaces = flag(v, "wall_faces", p.mWallFaces);
        p.mAir = flag(v, "air", p.mAir);
        p.mAirPad = v.has("air_pad") ? v["air_pad"].asInteger() : p.mAirPad;
        p.mAirMinGap = real(v, "air_min_gap", p.mAirMinGap);
        p.mFaceOffsets = flag(v, "face_offsets", p.mFaceOffsets);
        const LLSD& classes = v["classes"];
        for (S32 k = 0; k < classes.size(); ++k)
        {
            const LLSD& c = classes[k];
            if (k == 0)
            {
                p.mAvatar.mHeight = real(c, "height", p.mAvatar.mHeight);
                p.mAvatar.mRadius = real(c, "radius", p.mAvatar.mRadius);
                p.mAvatar.mClimb = real(c, "climb", p.mAvatar.mClimb);
                p.mAvatar.mSlope = real(c, "slope", p.mAvatar.mSlope);
            }
            else if (version < 4)
            {
                // The WEATHER class of an older census: what the air stood on, and its drainage.
                p.mTopParams.mHeight = real(c, "height", p.mTopParams.mHeight);
                p.mTopParams.mSlope = real(c, "slope", p.mTopParams.mSlope);
                p.mRunoff = flag(c, "edges", p.mRunoff);
                p.mRunoffMax = real(c, "drop_max", p.mRunoffMax);
            }
        }
        if (v.has("top"))
        {
            const LLSD& t = v["top"];
            p.mTopParams.mHeight = real(t, "height", p.mTopParams.mHeight);
            p.mTopParams.mSlope = real(t, "slope", p.mTopParams.mSlope);
            p.mRunoff = flag(t, "runoff", p.mRunoff);
            p.mRunoffMax = real(t, "runoff_max", p.mRunoffMax);
        }
        params.mAirRadius = real(v, "air_radius", params.mAirRadius);
        params.mGroundedMargin = real(v, "grounded_margin", params.mGroundedMargin);
        return p.mCell > 0.f && p.mTileSize > 0.f;
    }

    CensusSection airParamsSection(AirCensusParams& params)
    {
        return { "air.params", [&params](std::ostream& out) { writeAirParams(out, params); },
                 [&params](const LLSD& v, U32 version) { return readAirParams(v, version, params); } };
    }

    U32 layCensusTiles(const CensusFile& census, const AirCensusParams& params, std::vector<CensusTile>& out)
    {
        out.clear();
        std::map<LLUUID, U8> cues;
        for (const CensusRoot& root : census.mRoots)
        {
            cues[root.mId] = nameCue(root.mName, root.mDescription);
        }
        const auto cue_of = [&cues](const LLUUID& id) {
            const auto it = cues.find(id);
            return it == cues.end() ? static_cast<U8>(CUE_NONE) : it->second;
        };
        std::vector<RegionSquare> squares;
        for (const CensusRegion& region : census.mRegions)
        {
            squares.push_back({ region.mOrigin, region.mWidth });
        }
        const ClusterSet clusters = cluster(census.mRecords, census.mClusterGap, TILE_M, regionWindow(squares, census.mVoidMargin));
        std::map<LLUUID, bool> grounded_by_id;
        for (const CensusCluster& c : census.mClusters)
        {
            grounded_by_id[c.mId] = c.mGrounded;
        }

        TileInput base = params.mParams;
        base.mRecords.clear();
        base.mCuts.clear();
        base.mTerrain = TerrainWindow();
        base.mAir = true;
        base.mCoarseAir = false;
        base.mTileSize = static_cast<F32>(TILE_M);
        const F32 pad = tilePad(base);

        const auto make = [&](const LLUUID& mesh, bool terrain, const LLVector3d& origin, const LaidTile& lt, F64 zmin, F64 zmax) {
            CensusTile tile;
            tile.mMesh = mesh;
            tile.mTerrain = terrain;
            tile.mKey = lt.mKey;
            tile.mOverLand = lt.mOverLand;
            tile.mMin = meshTileMin(origin, lt.mKey);
            tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
            tile.mMin.mdV[VZ] = zmin;
            tile.mMax.mdV[VZ] = zmax;
            tile.mInAirRadius = inAirRadius(census, params.mAirRadius, tile.mMin, tile.mMax);
            tile.mInput = std::make_shared<TileInput>(base);
            tile.mInput->mMeshOrigin = origin;
            tile.mInput->mTileX = lt.mKey.mX;
            tile.mInput->mTileY = lt.mKey.mY;
            // Only a region's own land is in its mesh; the void past it has none.
            if (terrain && lt.mOverLand)
            {
                censusTerrain(census, origin, tile.mMin, tile.mMax, pad, tile.mInput->mTerrain);
            }
            finishTileInput(lt.mRecords, origin, *tile.mInput);
            tileRootCues(cue_of, *tile.mInput);
            out.push_back(std::move(tile));
        };

        U32 refused = 0;
        std::vector<RecordPtr> ground_records;
        std::vector<LaidTile> laid;
        for (const ClusterPtr& c : clusters.mClusters)
        {
            const auto it = grounded_by_id.find(c->mId);
            const bool grounded = it != grounded_by_id.end() ? it->second : groundedFallback(census, *c, params.mGroundedMargin);
            if (grounded)
            {
                ground_records.insert(ground_records.end(), c->mRecords.begin(), c->mRecords.end());
                continue;
            }
            const LLVector3d origin = latticeOrigin(c->mMin);
            if (!layAirborne(*c, origin, pad, laid))
            {
                ++refused;
            }
            for (const LaidTile& lt : laid)
            {
                make(c->mId, false, origin, lt, c->mMin.mdV[VZ], c->mMax.mdV[VZ]);
            }
        }
        for (const CensusRegion& region : census.mRegions)
        {
            const LLVector3d origin = latticeOrigin(region.mOrigin);
            if (!layRegion(region.mOrigin, region.mWidth, origin, ground_records, pad, census.mVoidMargin, squares, laid))
            {
                ++refused;
            }
            for (const LaidTile& lt : laid)
            {
                make(region.mId, true, origin, lt, region.mLandMin, region.mLandMax);
            }
        }
        return refused;
    }
}
