/**
 * @file sknavbuild.h
 * @brief One navmesh tile from census records and terrain: the Recast pipeline, pure and thread-free.
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

#ifndef SK_NAVBUILD_H
#define SK_NAVBUILD_H

#include "sknavairdistil.h"
#include "sknavflights.h"
#include "sknavtypes.h"
#include "sknavwalls.h"

#include <memory>
#include <string>
#include <vector>

namespace sknav
{
    /// The agent classes a tile is built for. Order is the index everywhere.
    enum class AgentClass : U8 { AVATAR = 0, WEATHER = 1 };
    constexpr S32 NUM_CLASSES = 2;

    /// Recast area ids: what a walkable polygon stands on. Detour poly flags
    /// are 1 << area, so a query filter can pick them.
    enum Area : U8 { AREA_NONE = 0, AREA_GROUND = 1, AREA_OBJECT = 2, AREA_SUBMERGED = 3 };

    struct ClassParams
    {
        F32 mHeight = 2.f;      ///< metres of headroom a floor needs
        F32 mRadius = 0.3f;     ///< metres the floor is eroded by
        F32 mClimb = 0.75f;     ///< metres of step that still connects
        F32 mSlope = 45.f;      ///< degrees, steeper is a wall
    };

    /// A terrain height window: samples at mSpacing from mOrigin (mesh-local
    /// x, y), mWidth by mHeight, row-major. Empty when the tile has no terrain.
    struct TerrainWindow
    {
        LLVector3 mOrigin;
        F32 mSpacing = 1.f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        std::vector<F32> mZ;
        F32 mWater = 0.f;       ///< region water height, mesh-local z
    };

    /// Everything one tile build reads. Immutable once handed to a worker:
    /// records are shared const, the rest is copied in.
    /// How Recast divides the walkable area into regions before it builds
    /// contours. Watershed handles open ground well but can leave holes;
    /// monotone never does but favours long thin polygons; layers is Recast's
    /// own suggestion for a tiled mesh. Which reads best on a flat floor is a
    /// question for a real region, so it is a setting rather than a choice.
    /// Slope is watershed within slope classes: a flight, its landing and a
    /// ramp's turns become separate regions, so their polygons meet on the
    /// lines where the slope changes instead of fanning across them.
    enum class Partition : U8 { WATERSHED, MONOTONE, LAYERS, SLOPE };

    struct TileBuildInput
    {
        LLVector3d mMeshOrigin;             ///< global; the mesh's Recast origin is this point
        S32 mTileX = 0;                     ///< tile index within the mesh, along SL x
        S32 mTileY = 0;                     ///< along SL y
        F32 mTileSize = 32.f;
        F32 mZMin = 0.f;                    ///< mesh-local z range the tile rasterises
        F32 mZMax = 0.f;
        F32 mCell = CELL_M;
        Partition mPartition = Partition::SLOPE;
        /// The walkable limit the slope partition is taken at, degrees; 0 takes
        /// the air's slope on a tile that keeps air and AVATAR's elsewhere, so
        /// the partition depends on where the tile is, not on which classes run.
        F32 mPartitionSlope = 0.f;
        F32 mDetailSampleCells = 6.f;       ///< 0 leaves the polygons untouched
        F32 mDetailMaxErrorCells = 2.f;
        F32 mWallMinHeight = 0.5f;          ///< shorter faces are a slope's terracing, not a wall
        std::vector<RecordPtr> mRecords;    ///< everything intersecting the padded tile
        /// Linkset roots whose name says stair or ramp (NameCue), sorted by id.
        std::vector<std::pair<LLUUID, U8>> mRootCues;
        /// The sim's exclusion volumes: their inside is cut out of the
        /// navmesh rather than built, so they never become a surface.
        std::vector<RecordPtr> mCuts;
        TerrainWindow mTerrain;
        bool mClasses[NUM_CLASSES] = { true, false };
        ClassParams mParams[NUM_CLASSES];
        /// Walls (sknavwalls.h). Faces are class-independent and cost a pass
        /// over the raw heightfield; edges and drops are per class.
        bool mWallFaces = false;
        bool mEdges[NUM_CLASSES] = { false, false };
        F32 mDropMax[NUM_CLASSES] = { 10.f, 24.f };
        F32 mDropSpacing = 2.f;
        /// The air grid (sknavair.h), off the WEATHER heightfield, which keeps
        /// the door leaves that split rooms. Nothing when WEATHER is not built.
        bool mAir = false;
        /// Cells WEATHER reads past the tile when it keeps the air, so the
        /// clearance on the tile is exact; AVATAR's heightfield never grows.
        S32 mAirPad = 16;
        /// Closed gaps lower than this are folded into the solid around them.
        F32 mAirMinGap = 0.5f;
    };

    struct TileBuildResult
    {
        enum Status { SUCCESS, EMPTY, FAILED } mStatus = FAILED;
        std::string mError;
        /// Detour tile data per class; empty for a class not built or with no polys.
        std::vector<U8> mData[NUM_CLASSES];
        S32 mPolys[NUM_CLASSES] = { 0, 0 };
        S32 mTriangles = 0;                 ///< rasterised
        F32 mMillis = 0.f;
        F32 mExcludeMillis = 0.f;           ///< of mMillis, building the soups a class leaves records out of
        /// Exposed vertical surfaces of the tile's solids, mesh-local SL metres.
        std::vector<WallFace> mFaces;
        /// Floor edges with what lies beyond them, and where a floor can be left.
        std::vector<WallSegment> mSegments[NUM_CLASSES];
        std::vector<DropLink> mLinks[NUM_CLASSES];
        /// The distilled air, null when there is none; mAirBuilt says it was asked for.
        TileAirPtr mAir;
        bool mAirBuilt = false;
        /// Stair flights and the polys they produced, when the partition is SLOPE.
        std::vector<Flight> mFlights[NUM_CLASSES];
    };

    /// SL is z-up, Recast is y-up: (x, y, z) -> (y, z, x), a cyclic permutation,
    /// so winding and up-facing are preserved and nothing goes negative inside a
    /// mesh whose origin is at its minimum corner.
    inline void toRecast(const LLVector3& sl, F32* out)
    {
        out[0] = sl.mV[VY];
        out[1] = sl.mV[VZ];
        out[2] = sl.mV[VX];
    }
    inline LLVector3 fromRecast(const F32* rc)
    {
        return LLVector3(rc[2], rc[0], rc[1]);
    }

    /// The padding a tile builds past its edge, in cells. Every pass that
    /// reads a span's neighbours must see the same cells from both sides of
    /// a seam, or the two tiles disagree about the floor there: Recast's own
    /// three, the erosion radius that eats the padding's outer edge, and the
    /// narrow-step walk of twice the radius that must still fit inside it.
    S32 borderCells(F32 radius, F32 cell);

    /// The slope the tile's shared partition is taken at.
    F32 partitionSlope(const TileBuildInput& input);

    struct TileBuildStats;

    /// Run the pipeline. Safe on any thread; touches nothing but the input
    /// and, when given, the stats it measures itself into.
    TileBuildResult buildTile(const TileBuildInput& input, TileBuildStats* stats = nullptr);

    /// Tessellate an analytic record into triangles (mesh-local, given the
    /// mesh origin), or copy its soup. Exposed for tests.
    void emitRecord(const Record& record, const LLVector3d& mesh_origin, std::vector<LLVector3>& out);
}

#endif // SK_NAVBUILD_H
