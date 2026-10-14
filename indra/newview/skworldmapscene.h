/**
 * @file skworldmapscene.h
 * @brief The 3D world map's own pass: the ground of map tiles, a light tilt-shift, and the agent dots.
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

#include "llrect.h"
#include "llrendertarget.h"
#include "skworldmapmath.h"
#include "skworldmaptiles.h"

#include <vector>

/// One frame's map camera: where it is, where it looks, and the orbit focus when it has one.
struct SKWorldMapFrame
{
    LLVector3d mEye;
    F64 mYaw = 0.0;
    F64 mPitch = -0.9;
    F64 mFovY = 0.785398;
    LLVector3d mFocus;
    bool mHasFocus = false;
};

/// Draws the map in place of the world: the tiles into a target of its own, a tilt-shift from that
/// target onto the back buffer, then the agent dots on top, sharp. The UI is drawn over it as usual.
class SKWorldMapScene
{
public:
    static constexpr S32 DOTS_PER_STACK = 8;        ///< a cluster shows at most this many dots, the top one ringed past it
    static constexpr S32 DOT_BUDGET = 1200;         ///< dots across the whole view, nearest clusters first
    static constexpr S32 POLLED_REGIONS = 64;       ///< nearest regions whose agents are asked for, each once a minute
    static constexpr S32 POLL_MAX_LEVEL = 3;        ///< regions are polled only under tiles this fine, as the 2D map does

    /// Draws the map into the back buffer over the world view's rectangle, in raw window pixels.
    void render(const SKWorldMapFrame& frame, const LLRect& world_rect_raw, F64 now);

    /// Gives back the targets and lowers the tiles' priority; called when the map closes.
    void release();

    /// What covers a region instead of its map tile. Not owned; null asks for map tiles only.
    void setSurfaceSource(SKWorldMapSurfaceSource* source) { mSurfaces = source; }

    const std::vector<skworldmap::SelectedTile>& selected() const { return mSelected; }

private:
    bool allocateTargets(S32 width, S32 height);
    void drawGround(const SKWorldMapFrame& frame, S32 width, S32 height, F64 now);
    void tiltShift(const SKWorldMapFrame& frame, const LLRect& rect);
    void pollAgents(const SKWorldMapFrame& frame, F64 now);
    void drawAgents(const SKWorldMapFrame& frame, const LLRect& rect);

    LLRenderTarget mScene;
    LLRenderTarget mBlurA;
    LLRenderTarget mBlurB;
    SKWorldMapTiles mTiles;
    SKWorldMapSurfaceSource* mSurfaces = nullptr;
    std::vector<skworldmap::SelectedTile> mSelected;
    std::vector<U64> mPolled;
};
