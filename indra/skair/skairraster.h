/**
 * @file skairraster.h
 * @brief One tile's solid raster from census records and terrain, and the air products read off it; pure and thread-free.
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

#ifndef SK_AIRRASTER_H
#define SK_AIRRASTER_H

#include "skairdistil.h"
#include "skairwalls.h"
#include "skcensustypes.h"

#include <string>
#include <utility>
#include <vector>

namespace skworld
{
    /// The raster's cell, metres, both horizontal and vertical.
    constexpr F32 CELL_M = 0.125f;

    /// What a floor stands on. 1-3 are the areas an air gap carries.
    enum Area : U8 { AREA_NONE = 0, AREA_GROUND = 1, AREA_OBJECT = 2, AREA_SUBMERGED = 3 };

    /// The solid raster's area byte. 1-3 are surfaces only the air's slope
    /// walks, each its own Area; from AREA_CODE_BASE up are AVATAR's, the
    /// area moved past them, so a merge that keeps the higher code keeps
    /// AVATAR's floor over a TOP-only one.
    constexpr U8 AREA_TOP_ONLY = 3;
    constexpr U8 AREA_CODE_BASE = AREA_TOP_ONLY + 1;
    constexpr U8 AREA_CODE_MAX = 63;

    /// What a linkset's name says it is: a hint the raster carries per triangle.
    enum NameCue : U8 { CUE_NONE = 0, CUE_STAIR = 1, CUE_RAMP = 2 };

    /// The NameCue an object's name and description give: stair words, else ramp.
    U8 nameCue(const std::string& name, const std::string& description);

    struct ClassParams
    {
        F32 mHeight = 2.f;      ///< metres of headroom a floor needs
        F32 mRadius = 0.3f;     ///< metres; sets the border AVATAR's view reads
        F32 mClimb = 0.75f;     ///< metres of step that still connects
        F32 mSlope = 45.f;      ///< degrees, steeper is a wall
    };

    /// What the air stands on: nearly everything, since weather runs down
    /// whatever it lands on, under half a metre of headroom. The raster's
    /// TOP surfaces; a truly vertical face has no top and is a wall face.
    constexpr F32 TOP_HEIGHT_M = 0.5f;
    constexpr F32 TOP_SLOPE_DEG = 89.f;

    /// Metres of padding the air reads past a tile, so its clearance there is exact.
    constexpr F32 AIR_PAD_M = 2.f;

    /// The cell a tile without its own air reads its coarse air at.
    constexpr F32 COARSE_AIR_M = 1.f;

    /// Everything one tile's raster reads. Immutable once handed to a worker:
    /// records are shared const, the rest is copied in.
    struct TileInput
    {
        LLVector3d mMeshOrigin;             ///< global; the mesh's origin is this point
        S32 mTileX = 0;                     ///< tile index within the mesh, along SL x
        S32 mTileY = 0;                     ///< along SL y
        F32 mTileSize = 32.f;
        F32 mZMin = 0.f;                    ///< mesh-local z range the tile rasterises
        F32 mZMax = 0.f;
        F32 mCell = CELL_M;
        F32 mWallMinHeight = 0.5f;          ///< shorter faces are a slope's terracing, not a wall
        std::vector<RecordPtr> mRecords;    ///< everything intersecting the padded tile
        /// Linkset roots whose name says stair or ramp (NameCue), sorted by id.
        std::vector<std::pair<LLUUID, U8>> mRootCues;
        /// The sim's exclusion volumes: their inside is cut out of the raster
        /// rather than built, so they never become a surface.
        std::vector<RecordPtr> mCuts;
        TerrainWindow mTerrain;
        /// The avatar: what AVATAR_FLOOR marks.
        ClassParams mAvatar;
        /// What the raster's TOP surfaces are; its climb is AVATAR's, its radius unused.
        ClassParams mTopParams = { TOP_HEIGHT_M, 0.f, 0.75f, TOP_SLOPE_DEG };
        /// The exposed faces of every solid (skairwalls.h).
        bool mWallFaces = false;
        /// Where the raster's TOP floors end over a drop, and where weather
        /// runs off them, down to mRunoffMax metres.
        bool mRunoff = false;
        F32 mRunoffMax = 24.f;
        /// The air grid (skair.h), off the solid raster, which keeps the door leaves that split rooms.
        bool mAir = false;
        /// Cells the raster reads past the tile when it keeps the air, so the clearance on the tile is exact.
        S32 mAirPad = 16;
        /// Closed gaps lower than this are folded into the solid around them.
        F32 mAirMinGap = 0.5f;
        /// Where walls' true faces sit inside their solid cells, kept with the air (skairfaceoffsets.h).
        bool mFaceOffsets = true;
        /// Without mAir, the air at COARSE_AIR_M in this tile's cells.
        bool mCoarseAir = false;
    };

    struct TileAirResult
    {
        enum Status { SUCCESS, EMPTY, FAILED } mStatus = FAILED;
        std::string mError;
        S32 mTriangles = 0;                 ///< rasterised
        F32 mMillis = 0.f;
        F32 mExcludeMillis = 0.f;           ///< of mMillis, building the soups a view leaves records out of
        /// Exposed vertical surfaces of the tile's solids, mesh-local SL metres.
        std::vector<WallFace> mFaces;
        /// The raster's TOP floor edges over a drop, and the run-off down them.
        std::vector<WallSegment> mLedges;
        std::vector<RunoffSegment> mRunoff;
        /// The distilled air, null when there is none; mAirBuilt says it was asked for.
        TileAirPtr mAir;
        bool mAirBuilt = false;
        bool mCoarseAir = false;            ///< mAir is the coarse air, not the tile's own
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

    /// The padding AVATAR's view reads past a tile, in cells: Recast's own
    /// three, and the radius twice over, so both sides of a seam read the same cells.
    S32 borderCells(F32 radius, F32 cell);

    /// The air's pad in cells: AIR_PAD_M, whole cells.
    S32 airPadCells(F32 cell);

    struct TileBuildStats;

    /// A raster's columns as dumped: Recast's grid (x along SL y, z along SL x),
    /// every column's spans lowest first, packed smin | smax << 13 | area << 26.
    struct RasterColumns
    {
        S32 mWidth = 0;                     ///< Recast x cells
        S32 mHeight = 0;                    ///< Recast z cells
        std::vector<U32> mStart;            ///< columns + 1
        std::vector<U32> mSpans;

        static U32 pack(U32 smin, U32 smax, U32 area) { return smin | smax << 13 | area << 26; }
        bool empty() const { return mStart.empty(); }
    };

    /// The raster's states between its passes, for fixtures a later raster is
    /// checked against. Every state is the full padded grid unless it says otherwise.
    struct RasterCapture
    {
        F32 mBmin[3] = { 0.f, 0.f, 0.f };   ///< the grid's Recast corner
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        S32 mBorder = 0;                    ///< the grid's cells past the tile
        S32 mAvatarBorder = 0;              ///< AVATAR's view's; it is inset by mBorder - mAvatarBorder
        std::vector<U8> mCodes;             ///< per soup triangle, its area byte
        std::vector<U8> mReal;              ///< area byte -> area
        /// The soup the raster was made from, so a raster of another kind can be made from the same: nine
        /// Recast-space floats per triangle, its closed bodies' triangle ranges, the cuts' own soup and
        /// ranges, the triangles AVATAR leaves out (empty for none), each triangle's NameCue, and the land.
        std::vector<F32> mSoupVerts;
        std::vector<S32> mSolids;           ///< begin, end pairs
        std::vector<F32> mCutVerts;
        std::vector<S32> mCuts;
        std::vector<U8> mLeafTri;
        std::vector<U8> mCues;
        TerrainWindow mTerrain;
        S32 mClimb = 0;                     ///< the merge's and bridges' climb, units
        RasterColumns mRasterised;          ///< triangles, filled bodies and cuts
        RasterColumns mBridged;             ///< with the air's crevice bridges
        RasterColumns mCulled;              ///< underground spans removed
        RasterColumns mTopPreLedge;         ///< the air's filters but the ledge filter
        RasterColumns mTopFiltered;         ///< the air's filters
        RasterColumns mAvatarFiltered;      ///< AVATAR's view after its filters, its own grid
        std::vector<U8> mOwnColumn;         ///< per column, 1 where AVATAR's view reads a copy of its own
        std::vector<U8> mInWindow;          ///< per column, 1 under a record AVATAR leaves out
        std::vector<U32> mAvatarFloorStart; ///< per column of the raster as culled, its spans' AVATAR_FLOOR bits
        std::vector<U8> mAvatarFloor;
        AirGrid mPadded;                    ///< the air's padded gaps, folded, before clearance
        std::vector<FaceOffset> mPaddedFaceOffsets;
    };

    /// The raster and the air products off it: faces, ledges, run-off and air.
    /// False, the result saying why, when the tile is empty or a stage failed.
    /// Safe on any thread; touches nothing but its arguments.
    bool rasteriseTile(const TileInput& input, TileAirResult& result, TileBuildStats* stats = nullptr, RasterCapture* capture = nullptr);
}

#endif // SK_AIRRASTER_H
