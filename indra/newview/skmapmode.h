/**
 * @file skmapmode.h
 * @brief Map mode: a 3D map of the grid shown in place of the world, under the viewer's own camera.
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

#include "llhandle.h"
#include "llsingleton.h"
#include "skmodalview.h"
#include "v3dmath.h"
#include "v3math.h"
#include "skworldmapscene.h"

#include <boost/signals2.hpp>
#include <string>

class LLView;

/// Map mode is a modal view: while it is open the display loop draws the map instead of the world. The
/// map is drawn from the viewer's own camera, so alt-cam, the Camera Controls floater, the flycam, the
/// scroll wheel and the camera key bindings move it as they move the camera in-world. The modal view
/// snapshots the world camera and puts it back; the map camera is kept here for the next time.
class SKMapMode : public LLSingleton<SKMapMode>, public SKModalMode
{
    LLSINGLETON(SKMapMode);
    ~SKMapMode();

public:
    /// True while the map replaces the world. Cheap enough for every frame.
    static bool isOpen();

    void toggle();
    bool open();
    void close();

    /// Drops the map's views, targets and tiles without touching the camera; called at viewer
    /// shutdown, while GL and the texture list are still up.
    void shutdown();

    // SKModalMode
    const char* getModalName() const override { return "world map"; }
    void closeModal() override { close(); }
    /// Under another mode the map keeps its camera and hides its input; on top again it takes both back.
    void onTopChanged(bool on_top) override;
    void shutdownModal() override { shutdown(); }
    void renderModal(const LLRect& world_rect_raw) override { render(world_rect_raw); }
    bool pickGround(S32 x, S32 y, LLVector3d& ground) const override { return groundUnderCursor(x, y, ground); }
    /// The flycam's height above the map over a person's scale, so it crosses regions low down and the
    /// grid high up.
    F64 flycamScale(const LLVector3d& eye) const override;
    /// RLVa hides the world map; the camera locks are the modal view's.
    bool rlvRestricted() const override;

    /// Moves the camera and its focus across the map until the focus is over the agent.
    void recentre();

    /// Draws the map over the world view's rectangle (raw pixels).
    void render(const LLRect& world_rect_raw);

    /// The map's ground under a point of the world view (scaled UI coordinates), for alt-click focus.
    bool groundUnderCursor(S32 x, S32 y, LLVector3d& ground) const;

    /// A plain left-drag: the camera and its focus slide across the ground together, keeping their
    /// height and angle, so the ground point grabbed stays under the cursor.
    bool beginPan(S32 x, S32 y);
    void pan(S32 x, S32 y);
    void endPan() { mPanning = false; }

    /// Fired as the map opens, while the last world frame is still in the targets: where a live
    /// impostor of the current region can be captured.
    typedef boost::signals2::signal<void()> open_signal_t;
    boost::signals2::connection setOpenCallback(const open_signal_t::slot_type& cb) { return mOpenSignal.connect(cb); }

    SKWorldMapScene& scene() { return mScene; }

private:
    SKWorldMapFrame frame() const;
    void removeViews();

    bool mOpen = false;
    bool mHasMapCamera = false;     ///< the map camera has been set once, so later opens resume it
    LLVector3d mMapPosition;
    LLVector3d mMapFocus;

    bool mPanning = false;
    LLVector3d mPanEye;
    LLVector3d mPanView;            ///< the viewer camera's origin at the drag's start, which the cursor rays leave from
    LLVector3d mPanFocus;
    LLVector3d mPanGrabbed;
    skworldmap::CursorBasis mPanBasis; ///< the camera the drag started from, which every cursor ray is cast through

    LLHandle<LLView> mInput;
    LLHandle<LLView> mOverlay;
    SKWorldMapScene mScene;
    open_signal_t mOpenSignal;
};
