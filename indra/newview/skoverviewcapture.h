/**
 * @file skoverviewcapture.h
 * @brief The overview: the census as styled shapes over the terrain, for the mini-map's base and
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
#include "skoverviewmovers.h"
#include "v3dmath.h"
#include "v4math.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class LLRenderTarget;
class LLViewerRegion;
namespace skmapdraw
{
    struct TagText;
}
class SKPopupPanes;

// Renders terrain and the census's shapes below a cut plane at the
// navmesh floor under the agent plus SKOverviewCutHeight, each writing a class code, then resolves depth and codes
// into an RGBA8 map. Two kinds of shot share the passes: straight-down maps of every connected region for the mini-map
// (SKMapCaptureBase; the agent's region cut, every SKOverviewRefresh seconds, its neighbours uncut and less often,
// regions further off rarer still) and the tactical view (SKTacticalOverview), a tilted orbit
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
    /// Whether the main view's world can go unrendered this frame: the overview is on and covered it last frame.
    static bool worldHidden();

    /// A world right-click at the root view's (x, y): opens the mini-map's menus on the overview; false when it is off.
    static bool handleRightClick(S32 x, S32 y);

    /// The tactical view takes the wheel for its distance; false when it is off.
    static bool handleScrollWheel(F32 clicks);
    /// The tactical overview key: held, it shows the view until released; tapped, it latches it until the next tap.
    static void handleTacticalKey(bool down);

    /// A region's resolved map: it covers the region at mOrigin by mWidth metres.
    struct Tile
    {
        U32 mTexture = 0;
        LLVector3d mOrigin;
        F32 mWidth = 0.f;
    };
    /// The captured regions' maps, the agent's last so it draws over its neighbours' edges.
    std::vector<Tile> tiles() const;
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
        F32 mWater = 0.f;   // the water surface's height
        LLVector4 mRegion;  // the region styled as the agent's: min x, min y, max x, max y, agent space
        bool mTopDown = false;
    };
    /// A region's top-down map and when it was last taken.
    struct RegionMap;

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
    U32 render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, bool resolve_now = true);
    /// The capture's depth and class codes to the styled map.
    void resolve(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot);
    /// The movers over a copy of the static capture, then resolved: the static capture is not redone for them.
    void drawMovers(const Shot& shot);
    /// The vehicles as their captured outlines on the ground, over the composited overview.
    void drawVehicles(F32 distance);
    /// Captures the region maps that are due: the agent's on its timer, then the most overdue other one.
    void captureMaps(F64 now);
    bool captureMap(RegionMap& map, LLViewerRegion* region, bool agent);
    void tactical();
    void composite();
    bool rightClick(S32 x, S32 y);
    /// The ground plane at the tactical centre's height under an overview pixel, through the tactical camera.
    bool groundAt(const LLVector2& at, LLVector3d& world) const;
    void readTimer();

    SKOverviewCensus mCensus;
    SKOverviewMovers mMovers;
    bool mMoversDrawn = false;
    F32 mTacticalDistance = 0.f;
    std::vector<std::unique_ptr<RegionMap>> mMaps;
    Stats mStats;
    F32 mDemandRange = 0.f;
    bool mGLFailed = false;
    bool mTiming = false;
    bool mDemanding = false;
    LLMatrix4a mTacticalView;
    LLMatrix4a mTacticalProj;
    S32 mTacticalViewport[4] = {};
    bool mTacticalReady = false;
    LLVector3 mTacticalCentre;   // agent space
    F32 mTacticalReach = 0.f;
    U32 mLastCompositeFrame = 0;
    /// What the last tactical capture showed: the same again reuses its image.
    struct TacticalKey
    {
        bool mValid = false;
        LLVector3 mEye;
        LLVector3 mDir;
        F32 mFov = 0.f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        F32 mCut = 0.f;
        U64 mCensus = 0;
        const void* mRegion = nullptr;
    } mTacticalKey;
    /// CPU time of the tactical view, summed over a second and logged.
    struct Cost
    {
        F64 mSince = 0.0;
        F32 mCaptureMs = 0.f;
        F32 mResolveMs = 0.f;
        F32 mCompositeMs = 0.f;
        U32 mFrames = 0;
        U32 mRenders = 0;
        U32 mReuses = 0;
        U32 mScansAtStart = 0;
    } mCost;
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
