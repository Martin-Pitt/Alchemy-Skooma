/**
 * @file sknavwallruns.h
 * @brief Wall runs: what solid stands beside each walkable floor's boundary, measured from that floor.
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

#ifndef SK_NAVWALLRUNS_H
#define SK_NAVWALLRUNS_H

#include "sknavairdistil.h"
#include "sknavwalls.h"

#include <functional>
#include <vector>

struct dtMeshTile;

namespace sknav
{
    struct TileEnclosure;

    /// The planes a wall is read at over its floor: the map's section cuts, half a cell off SL's 0.5 m stacking.
    constexpr F32 WALL_LOW_PLANE_M = 0.4375f;
    constexpr F32 WALL_HIGH_PLANE_M = 1.4375f;
    constexpr F32 WALL_RUN_STRIDE_M = 0.125f;   ///< samples along a boundary edge at most this far apart

    /// What a run shows: solid at the high plane, else at the low plane, else nothing.
    enum class WallClass : U8 { NONE, HATCHED, SOLID };

    /// One stretch of a walkable floor's boundary with one answer. The line
    /// is the boundary itself, at floor height, mesh-local, with the floor on
    /// its left; the solid lies to the right.
    struct WallRun
    {
        std::vector<LLVector3> mLine;
        WallClass mClass = WallClass::NONE;
        bool mLow = false;          ///< solid at floor + WALL_LOW_PLANE_M
        bool mHigh = false;         ///< solid at floor + WALL_HIGH_PLANE_M
        F32 mTopMin = 0.f;          ///< least and greatest top of the solid over the floor along the run, metres; 0 for none
        F32 mTopMax = 0.f;
        F32 mFace = -1.f;           ///< mean distance from the line to the solid's true face, metres; < 0 when no face was met
        F32 mFloorZ = 0.f;          ///< mean floor height the run is measured from, mesh-local
        std::vector<U16> mPolys;    ///< the tile's floor polys it bounds, by index, ascending
        U32 mSpace = 0;             ///< the floor's space id, 0 for none
        U32 mAcross = 0;            ///< the space id most samples found beyond the solid or the edge, 0 for none
        Cover mCover = Cover::NONE; ///< the greatest top read against the class's eye height
    };

    /// One column's gap: mesh-local metres.
    struct WallColumnGap
    {
        F32 mFloor = 0.f;
        F32 mCeiling = 0.f;         ///< a very large number when open
        U32 mSpace = 0;             ///< space id of the gap, 0 for none
        bool mWalkable = false;     ///< AVATAR walks the solid under it
    };

    /// A tile's air, with the enclosure that answers for exactly it when there is one.
    struct WallAir
    {
        TileAirPtr mAir;
        const TileEnclosure* mEnclosure = nullptr;
    };

    /// The air of a set of tiles of one mesh, read by mesh-local point.
    class WallAirReader
    {
    public:
        /// space_id turns an enclosure's space index + 1 into the space's id.
        WallAirReader(std::vector<WallAir> tiles, std::function<U32(U16)> space_id);

        /// The gaps of the column holding (x, y), lowest first; false where no tile has air there.
        bool column(F32 x, F32 y, std::vector<WallColumnGap>& out) const;
        /// Where the solid column holding (x, y) shows its true face to air along
        /// dir (0 +x, 1 -x, 2 +y, 3 -y, the way one walks into it): the axis
        /// coordinate, mesh-local. NaN off every tile.
        F32 faceAt(F32 x, F32 y, S32 dir) const;
        F32 cell() const { return mCell; }

    private:
        const WallAir* tileAt(F32 x, F32 y, S32& ix, S32& iy) const;

        std::vector<WallAir> mTiles;
        std::function<U32(U16)> mSpaceId;
        F32 mCell = 0.125f;
    };

    /// Totals over runs: count and plan metres by class, and boundary edges whose samples took more than one class.
    struct WallRunStats
    {
        U32 mRuns[3] = { 0, 0, 0 };
        F64 mMetres[3] = { 0.0, 0.0, 0.0 };
        U32 mMixedEdges = 0;
        U32 mEdges = 0;
    };

    struct WallRunParams
    {
        F32 mRadius = 0.3f;         ///< the class's erosion: the solid is about this far beyond the boundary
        F32 mClimb = 0.75f;         ///< a floor gap within this of the poly is the floor measured from
        F32 mHeight = 2.f;          ///< solids over gaps shorter than this stack into one wall
        F32 mEyeHeight = 1.6f;      ///< for the cover class
        F32 mReach = 0.75f;         ///< how far past the boundary solid is looked for
        F32 mAcrossReach = 1.5f;    ///< how far past the face the far side's space is looked for
    };

    /// The runs of every boundary edge of a Detour tile's ground polys: edges
    /// with no neighbour and not on the tile's border. Each edge is sampled
    /// every WALL_RUN_STRIDE_M at most; a sample reads the floor gap just inside,
    /// then steps outward for the first column with solid at either plane.
    /// Runs chain along the boundary and split where the class or the floor's
    /// space changes. poly_space gives each poly's
    /// space id, by index; empty for none. Understructure below the floor never counts.
    void buildWallRuns(const dtMeshTile& tile, const std::vector<U32>& poly_space, const WallAirReader& air, const WallRunParams& params,
                       std::vector<WallRun>& out, WallRunStats* stats = nullptr);

    const char* wallClassName(WallClass c);
}

#endif // SK_NAVWALLRUNS_H
