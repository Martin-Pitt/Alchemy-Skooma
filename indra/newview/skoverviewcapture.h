/**
 * @file skoverviewcapture.h
 * @brief The overview: the census as styled shapes over a ghost of the rendered world, for the mini-map's base and
 *        the tactical view.
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

#ifndef SK_OVERVIEWCAPTURE_H
#define SK_OVERVIEWCAPTURE_H

#include "llcamera.h"
#include "llhandle.h"
#include "llmatrix4a.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v2math.h"
#include "skmaplabels.h"
#include "skoverviewcensus.h"
#include "v3dmath.h"

#include <string>
#include <utility>
#include <vector>

class LLRenderTarget;
namespace skmapdraw
{
    struct TagText;
}
class SKPopupPanes;

// Renders terrain, the rendered world (the ghost, SKOverviewGhost) and the census's shapes below a cut plane at the
// navmesh floor under the agent plus SKOverviewCutHeight, each writing a class code, then resolves depth and codes
// into an RGBA8 map. Two shots share the passes: the straight-down map of the agent's region for the mini-map
// (SKMapCaptureBase, every SKOverviewRefresh seconds) and the tactical view (SKTacticalOverview), a tilted orbit
// camera around the agent drawn over the main view every frame.
class SKOverviewCapture : public LLSingleton<SKOverviewCapture>
{
    LLSINGLETON(SKOverviewCapture);
    ~SKOverviewCapture() override;

public:
    struct Stats
    {
        S32 resolution = 0;
        F32 cpu_ms = 0.f;    // cull, sort, draw submission and resolve on the CPU
        F32 gpu_ms = -1.f;   // capture and resolve on the GPU, from the previous capture's timer query
        U32 draws = 0;       // draw infos sorted for the capture, plus the terrain pool
        U32 census_tris = 0;
        U32 captures = 0;
    };

    /// Called once a frame after the world has rendered, before the UI.
    static void onFrame();
    static void cleanupGL();
    /// Called after renderFinalize, before the HUD and UI: draws the tactical overview over the finished frame.
    static void onComposite();
    /// Whether the tactical overview covers the world view this frame: world-space name tags and hover text stay off it.
    static bool tacticalShown();

    /// A world right-click at the root view's (x, y): opens the mini-map's menus on the overview; false when it is off.
    static bool handleRightClick(S32 x, S32 y);

    /// The tactical view takes the wheel for its distance; false when it is off.
    static bool handleScrollWheel(F32 clicks);
    /// The tactical overview key: held, it shows the view until released; tapped, it latches it until the next tap.
    static void handleTacticalKey(bool down);

    /// The resolved map, 0 until the first capture; it covers the region at originGlobal() by regionWidth() metres.
    U32 texture() const;
    const LLVector3d& originGlobal() const { return mOrigin; }
    F32 regionWidth() const { return mWidth; }
    const Stats& stats() const { return mStats; }

private:
    struct Shot
    {
        LLMatrix4a mView;
        LLMatrix4a mProj;
        LLCamera mCull;
        LLVector3 mEye;     // agent space
        F32 mCut = 0.f;     // agent-space z
        F32 mFloor = 0.f;   // the height colours are relative to
        bool mTopDown = false;
    };

    void frame();
    void updateDemand(bool wanted);
    /// Caps Lock is taken from the OS while the tactical action is bound to it and the world has the keyboard.
    static void updateCapsLockCapture();
    /// The floor under the agent from the navmesh and the cut above it, or the agent's height and the old offset.
    void cutHeights(const LLVector3d& at, F32 fallback_z, F32& floor_z, F32& cut_z) const;
    /// What the tactical orbit centres on: the avatar, the alt-cam focus target, or the surface under a flycam.
    void tacticalCentre(LLVector3d& centre) const;
    bool ensureGL(LLRenderTarget& capture, LLRenderTarget& resolved, S32 w, S32 h);
    bool ensureShaders();
    void releaseGL();
    U32 render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot);
    bool captureMap();
    void tactical();
    void composite();
    bool rightClick(S32 x, S32 y);
    /// The ground plane at the tactical centre's height under an overview pixel, through the tactical camera.
    bool groundAt(const LLVector2& at, LLVector3d& world) const;
    void readTimer();

    SKOverviewCensus mCensus;
    LLVector3d mOrigin;
    F32 mWidth = 0.f;
    F64 mLastCapture = -1.0;
    F32 mAgentZ = 0.f;
    F32 mTop = 0.f;
    F32 mBottom = 0.f;
    Stats mStats;
    bool mGLFailed = false;
    bool mTiming = false;
    bool mValid = false;
    bool mDemanding = false;
    LLMatrix4a mTacticalView;
    LLMatrix4a mTacticalProj;
    S32 mTacticalViewport[4] = {};
    bool mTacticalReady = false;
    LLVector3 mTacticalCentre;   // agent space
    F32 mTacticalReach = 0.f;
    U32 mLastCompositeFrame = 0;
    struct TacticalMark
    {
        LLUUID mId;
        LLVector2 mAt;      // overview pixels
    };
    std::vector<TacticalMark> mTacticalMarks;  // the markers drawn last frame, for picks
    // The mini-map's hover nametags, laid out in UI pixels over the overview.
    void drawTacticalLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn, S32 vw, S32 vh);
    skmap::HoverSet mTacticalHover;
    skmap::LabelLayout mTacticalLabels;
    std::vector<skmapdraw::TagText> mTacticalTagTexts;
    bool mTacticalEscDown = false;
    LLHandle<SKPopupPanes> mPopup;

};

#endif // SK_OVERVIEWCAPTURE_H
