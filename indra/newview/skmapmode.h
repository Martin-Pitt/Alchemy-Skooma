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
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "skworldmapscene.h"

#include <boost/signals2.hpp>
#include <string>

class LLView;

/// While map mode is open the display loop draws the map instead of the world, the way it draws the
/// progress screen while teleporting. The map is drawn from the viewer's own camera, so alt-cam, the
/// Camera Controls floater, the flycam, the scroll wheel and the camera key bindings move it as they
/// move the camera in-world; the camera's distance constraints are lifted while it is open. The
/// world camera is snapshotted on open and put back on close, and the map camera is kept for the
/// next time. The agent stays in the region: its updates keep flowing, carrying the world camera.
class SKMapMode : public LLSingleton<SKMapMode>
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

    /// Key and mouse bindings that move the avatar, which do nothing while the map is open.
    static bool blocksAction(const std::string& function_name);

    /// The camera the sim is told about in agent updates: the world camera while the map is open,
    /// so the interest list stays where the agent is. Leaves the arguments alone otherwise.
    static void worldCameraForUpdate(LLVector3& position_agent, LLVector3& at, LLVector3& left, LLVector3& up);

    /// Fired as the map opens, while the last world frame is still in the targets: where a live
    /// impostor of the current region can be captured.
    typedef boost::signals2::signal<void()> open_signal_t;
    boost::signals2::connection setOpenCallback(const open_signal_t::slot_type& cb) { return mOpenSignal.connect(cb); }

    SKWorldMapScene& scene() { return mScene; }

private:
    struct WorldCamera
    {
        bool mMouselook = false;
        bool mFocusOnAvatar = true;
        LLVector3d mPosition;
        LLVector3d mFocus;
        LLUUID mFocusObject;
        LLVector3d mViewOrigin;     ///< the viewer camera, for agent updates
        LLVector3 mAt, mLeft, mUp;
    };

    SKWorldMapFrame frame() const;
    void removeViews();

    bool mOpen = false;
    bool mHasMapCamera = false;     ///< the map camera has been set once, so later opens resume it
    LLVector3d mMapPosition;
    LLVector3d mMapFocus;
    WorldCamera mWorld;

    bool mPanning = false;
    LLVector3d mPanEye;
    LLVector3d mPanFocus;
    LLVector3d mPanGrabbed;

    LLHandle<LLView> mInput;
    LLHandle<LLView> mOverlay;
    SKWorldMapScene mScene;
    open_signal_t mOpenSignal;
};
