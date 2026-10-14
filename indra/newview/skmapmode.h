/**
 * @file skmapmode.h
 * @brief Map mode: a 3D map of the grid shown in place of the world, with an orbit and a fly camera.
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

#include "llagentcamera.h"
#include "llhandle.h"
#include "llsingleton.h"
#include "skworldmapmath.h"
#include "skworldmapscene.h"

#include <boost/signals2.hpp>

class LLView;

/// While map mode is open the display loop draws the map instead of the world, the way it draws the
/// progress screen instead while teleporting. The agent stays in the region and keeps sending agent
/// updates; the world camera is left alone and put back on close. The map camera is kept for the next time.
class SKMapMode : public LLSingleton<SKMapMode>
{
    LLSINGLETON(SKMapMode);
    ~SKMapMode();

public:
    enum ECamera
    {
        CAMERA_ORBIT,
        CAMERA_FLY
    };

    /// True while the map replaces the world. Cheap enough for every frame.
    static bool isOpen();

    void toggle();
    bool open();
    void close();

    /// Centres the camera on the agent, keeping its height and heading.
    void home();
    void switchCamera();
    ECamera camera() const { return mCamera; }

    /// Moves the camera for held keys, then draws the map over the world view's rectangle (raw pixels).
    void render(const LLRect& world_rect_raw);

    /// Fired as the map opens, while the last world frame is still in the targets: where a live
    /// impostor of the current region can be captured.
    typedef boost::signals2::signal<void()> open_signal_t;
    boost::signals2::connection setOpenCallback(const open_signal_t::slot_type& cb) { return mOpenSignal.connect(cb); }

    SKWorldMapScene& scene() { return mScene; }

    // Input from the map's view, in screen pixels.
    void drag(S32 dx, S32 dy, bool rotate);
    void zoom(F32 notches);

private:
    struct WorldCamera
    {
        bool mMouselook = false;
        bool mFocusOnAvatar = true;
        LLVector3d mPosition;
        LLVector3d mFocus;
        LLUUID mFocusObject;
    };

    void moveForKeys(F64 dt);
    SKWorldMapFrame frame() const;
    void startFromWorld();

    bool mOpen = false;
    bool mStarted = false;              ///< the map camera has been set once, so later opens resume it
    ECamera mCamera = CAMERA_ORBIT;
    skworldmap::OrbitCamera mOrbit;
    skworldmap::FlyCamera mFly;
    WorldCamera mWorld;
    F64 mLastFrame = 0.0;
    LLHandle<LLView> mInput;
    SKWorldMapScene mScene;
    open_signal_t mOpenSignal;
};
