/**
 * @file skworldmapmath.h
 * @brief The 3D world map's pure geometry: panning across the ground, which map tile level to draw where,
 *        and how agent clusters stack into columns of dots.
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

#pragma once

#include "stdtypes.h"
#include "v2math.h"
#include "v3dmath.h"

#include <vector>

/// Global frame throughout: x east, y north, z up, metres. A region is 256 m on a side. A map server
/// tile of level L (1 to 8) covers 2^(L-1) regions on a side, its origin aligned to that many regions.
namespace skworldmap
{
    constexpr F64 REGION_METRES = 256.0;
    constexpr S32 TILE_LEVELS = 8;
    constexpr F64 TILE_PIXELS = 256.0;
    constexpr F64 GROUND_Z = 20.0;          ///< the tile plane, at the default water height
    constexpr S32 GRID_REGIONS = 2048;      ///< tile roots cover region coordinates [0, GRID_REGIONS)

    /// Unit look direction from a heading (radians from east, counter-clockwise) and a pitch (negative looks down).
    LLVector3d lookDirection(F64 yaw, F64 pitch);

    /// Where a ray from `origin` along `dir` meets the ground plane; false if it never does.
    bool groundHit(const LLVector3d& origin, const LLVector3d& dir, LLVector3d& hit, F64 ground_z = GROUND_Z);

    /// How far to move the camera and its focus across the ground, keeping height and angle, so that
    /// `grabbed` (a ground point) lies under a cursor whose ray from `eye` runs along `dir`.
    /// False when that ray does not meet the ground.
    bool panOffset(const LLVector3d& eye, const LLVector3d& dir, const LLVector3d& grabbed, LLVector3d& offset, F64 ground_z = GROUND_Z);

    /// The camera as a drag found it: a cursor ray is cast through this, never the live camera, so a pan
    /// cannot feed back into itself.
    struct CursorBasis
    {
        LLVector3d mAt, mLeft, mUp;
        F64 mFocal = 1.0;   ///< pixels from the eye to the screen, half the view height over tan(fov / 2)
        F64 mCenterX = 0.0; ///< the world view centre, in the same pixels as the cursor
        F64 mCenterY = 0.0;
    };

    /// The unit ray under cursor pixel (x, y), as LLViewerWindow::mouseDirectionGlobal casts it.
    LLVector3d cursorRay(const CursorBasis& basis, F64 x, F64 y);

    /// The shallowest a pan ray may dip below the horizon (sine of about one degree): nearer the horizon a
    /// pixel of cursor is kilometres of ground.
    constexpr F64 PAN_MIN_DIP = 0.02;

    /// `dir` tilted down to at least `min_dip`, keeping its heading.
    LLVector3d clampDip(const LLVector3d& dir, F64 min_dip = PAN_MIN_DIP);

    /// The height the flycam's own speeds are tuned for: a person's scale.
    constexpr F64 FLYCAM_REFERENCE_HEIGHT = 10.0;

    /// The flycam's speed multiplier at height `eye_z`: its height above the ground over the reference,
    /// never below 1.
    F64 flycamHeightScale(F64 eye_z, F64 ground_z = GROUND_Z);

    /// A view for tile selection: the eye, its heading and pitch, the vertical field of view and the screen.
    struct View
    {
        LLVector3d mEye;
        F64 mYaw = 0.0;
        F64 mPitch = 0.0;
        F64 mFovY = 0.785398;
        F64 mAspect = 16.0 / 9.0;
        F64 mScreenHeight = 1080.0;
    };

    /// The view's right, up and forward axes.
    void viewBasis(F64 yaw, F64 pitch, LLVector3d& forward, LLVector3d& right, LLVector3d& up);

    struct TileKey
    {
        S32 mLevel = 1;
        S32 mGridX = 0;     ///< region coordinates of the tile's south-west corner
        S32 mGridY = 0;

        bool operator==(const TileKey& o) const { return mLevel == o.mLevel && mGridX == o.mGridX && mGridY == o.mGridY; }
        bool operator<(const TileKey& o) const
        {
            return mLevel != o.mLevel ? mLevel < o.mLevel : (mGridX != o.mGridX ? mGridX < o.mGridX : mGridY < o.mGridY);
        }
        S32 regionsPerSide() const { return 1 << (mLevel - 1); }
        F64 sideMetres() const { return regionsPerSide() * REGION_METRES; }
    };

    struct SelectedTile
    {
        TileKey mKey;
        F64 mProjectedPixels = 0.0;     ///< the tile's side on screen, measured at its nearest point
        F64 mDistance = 0.0;            ///< from the eye to the tile's nearest point
    };

    struct TileSelectParams
    {
        F64 mDetail = 1.0;              ///< a tile splits while its side covers more than this many of its own texel widths
        S32 mCap = 768;                 ///< at most this many tiles; the largest on screen split first
        F64 mGroundZ = GROUND_Z;
        S32 mGridRegions = GRID_REGIONS;
    };

    /// Disjoint tiles covering the ground in view. Each tile's level follows its projected size, so tiles
    /// coarsen with distance as well as with height. Splitting stops at level 1, at the detail threshold,
    /// or where a split would pass the cap.
    std::vector<SelectedTile> selectTiles(const View& view, const TileSelectParams& params = TileSelectParams());

    /// The tile `up` levels coarser that contains `key`, and where `key` lies in its texture (u0, v0, u1, v1).
    TileKey ancestor(const TileKey& key, S32 up, F32 uv[4]);

    /// The screen direction a stack of dots grows in, from world up in view space (x right, y up,
    /// z toward the viewer). Seen from straight above world up has no screen direction, so it blends to screen up.
    LLVector2 stackDirection(const LLVector3d& up_in_view);

    /// How many dots each cluster draws, nearest first: each at most `per_stack_cap`, all within `budget`.
    std::vector<S32> stackBudget(const std::vector<F64>& distances, const std::vector<S32>& counts, S32 per_stack_cap, S32 budget);
} // namespace skworldmap
