/**
 * @file skmodalview.h
 * @brief A modal view: something shown in place of the world, under the viewer's own camera.
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
#include "llsingleton.h"
#include "lluuid.h"
#include "skmodalviewmath.h"
#include "v3dmath.h"
#include "v3math.h"

#include <string>
#include <vector>

/// A view that covers the world: a map, an overview. It claims the modal view while it is shown and
/// releases the claim when it closes. Only the claim on top draws, picks and scales the flycam.
class SKModalMode
{
public:
    /// A mode destroyed while registered or claimed is forgotten, and the world camera comes back if it
    /// held the last claim.
    virtual ~SKModalMode();

    /// For the log.
    virtual const char* getModalName() const = 0;
    /// Closes the mode, which saves whatever it keeps of its own camera and then releases its claim,
    /// since the last release puts the world camera back. Asked on Escape, teleport, disconnect,
    /// logout, GL restore and RLVa restrictions.
    virtual void closeModal() = 0;
    /// Another mode came over this one (false), or left it on top again (true). Not called for the
    /// mode's own claim or release. A covered mode hides its input and keeps its camera for later.
    virtual void onTopChanged(bool on_top) {}
    /// Drops the mode's views and GL resources at viewer shutdown, without touching the camera.
    virtual void shutdownModal() {}
    /// Draws the mode over the world view's rectangle (raw pixels), in place of the world.
    virtual void renderModal(const LLRect& world_rect_raw) {}
    /// The mode's ground under a point of the world view (scaled UI coordinates), where an alt-click
    /// focuses the camera. False when the point hits nothing.
    virtual bool pickGround(S32 x, S32 y, LLVector3d& ground) const { return false; }
    /// How much faster the flycam translates from `eye` while the mode is on top.
    virtual F64 flycamScale(const LLVector3d& eye) const { return 1.0; }
    /// RLVa restrictions of the mode's own, on top of the camera locks every mode honours.
    virtual bool rlvRestricted() const { return false; }
};

/// While any mode claims it, the display loop draws the top mode instead of the world, the way it draws
/// the progress screen while teleporting, and keeps the world's object work running. The world camera
/// and the agent camera mode are snapshotted on the first claim and put back on the last release; while
/// claimed the sim, the audio listener and the voice ear keep that world camera, carried with the agent
/// when it followed it, and the avatar cannot be moved, walked by autopilot, put in mouselook or
/// photographed. The camera's distance constraints are lifted so the mode can roam.
class SKModalView : public LLSingleton<SKModalView>
{
    LLSINGLETON(SKModalView);
    ~SKModalView();

public:
    /// Claim priorities: the tactical overview stays on top of the map while it is shown.
    static constexpr S32 PRIORITY_MAP = 0;
    static constexpr S32 PRIORITY_TACTICAL_OVERVIEW = 100;

    /// True while a mode covers the world. Cheap enough for every frame.
    static bool worldCovered();
    /// The mode on top, or null.
    static SKModalMode* top();
    /// RLVa locks the camera, so no mode may take it.
    static bool rlvCameraLocked();
    /// Whether a mode could claim the view now: logged in, the window and camera up, RLVa willing.
    static bool canClaim();

    /// A registered mode is shut down with the viewer.
    void registerMode(SKModalMode* mode);
    void unregisterMode(SKModalMode* mode);

    /// Covers the world with `mode`, registering it. The first claim snapshots the world camera, leaves
    /// mouselook and stops the autopilot. Claiming again changes the priority. False, any existing claim
    /// left as it was, when the view cannot be claimed or the mode's own RLVa restriction holds it shut.
    bool claim(SKModalMode* mode, S32 priority);
    /// The last release puts the world camera back as it was, following the agent again if it did.
    void release(SKModalMode* mode);
    bool holds(const SKModalMode* mode) const { return mClaims.holds(mode); }

    void closeTop();
    void closeAll();
    /// Called by the display loop each frame while covered: closes every mode on a teleport, disconnect,
    /// logout or GL restore, and any mode RLVa no longer allows. Returns whether the world is still covered.
    bool closeIfNeeded(bool leaving_world);

    /// Draws the top mode.
    void render(const LLRect& world_rect_raw);
    /// Shuts every registered mode down while GL and the texture list are still up; no camera restore.
    void shutdown();

    /// Key and mouse bindings that move the avatar, which do nothing while covered.
    static bool blocksAction(const std::string& function_name);
    /// The top mode's ground under the cursor, for alt-click focus.
    static bool pickGround(S32 x, S32 y, LLVector3d& ground);
    /// The top mode's flycam travel scale; 1 when uncovered.
    static F64 flycamScale(const LLVector3d& eye);

    /// The world camera while covered, carried along with the agent when it followed it, for whatever
    /// listens or reports from the camera (agent updates, audio, voice). False, arguments untouched, when uncovered.
    static bool worldCamera(LLVector3d& position_global, LLVector3& at, LLVector3& left, LLVector3& up);
    /// The camera the sim is told about in agent updates: the world camera while covered, so the
    /// interest list stays where the agent is. Leaves the arguments alone otherwise.
    static void worldCameraForUpdate(LLVector3& position_agent, LLVector3& at, LLVector3& left, LLVector3& up);

private:
    friend class SKModalMode;
    /// Drops a mode being destroyed without calling into it.
    void forget(SKModalMode* mode);

    struct WorldCamera
    {
        bool mMouselook = false;
        bool mFocusOnAvatar = true;
        LLVector3d mPosition;       ///< relative to the agent when mFocusOnAvatar, else global
        LLVector3d mFocus;          ///< likewise
        LLUUID mFocusObject;
        LLVector3d mViewOffset;     ///< the viewer camera, relative like mPosition, for agent updates and the ears
        LLVector3 mAt, mLeft, mUp;
    };

    void notifyTopChange(SKModalMode* before, const SKModalMode* except);
    void snapshotWorldCamera();
    void restoreWorldCamera();

    skmodalview::Claims mClaims;
    std::vector<SKModalMode*> mModes;
    WorldCamera mWorld;
    bool mClosingAll = false; ///< no mode is handed the top while every mode is closing
};
