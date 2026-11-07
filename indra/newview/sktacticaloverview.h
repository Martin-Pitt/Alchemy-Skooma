/**
 * @file sktacticaloverview.h
 * @brief The tactical overview: a modal view of the overview capture's camera view, with the mini-map's people over it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_TACTICALOVERVIEW_H
#define SK_TACTICALOVERVIEW_H

#include "llhandle.h"
#include "llmatrix4a.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "skmaplabels.h"
#include "skmodalview.h"
#include "v2math.h"
#include "v3dmath.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class LLVOAvatar;
class SKPopupPanes;
namespace skmapdraw
{
    struct TagText;
}

/// A modal view that replaces the world with the overview capture's camera view (SKOverviewCapture::renderView) from
/// the viewer's own camera, which it tilts over the agent as it opens, so alt-cam, the flycam and the wheel move it as
/// they move the world camera; the mini-map's markers, firing lines, projectiles and labels are drawn over it.
/// SKTacticalOverview is the switch. Main thread.
class SKTacticalOverview : public LLSingleton<SKTacticalOverview>, public SKModalMode
{
    LLSINGLETON(SKTacticalOverview);
    ~SKTacticalOverview() override;

public:
    /// Called once a world frame, beside the overview's own: the key's Caps Lock capture and the switch.
    static void onFrame();
    /// Whether the overview is the modal view on top, drawn in place of the world.
    static bool shown();
    /// A world right-click at the root view's (x, y): opens the mini-map's menus on the overview; false when it is off.
    static bool handleRightClick(S32 x, S32 y);
    /// The tactical overview key: held, it shows the view until released; tapped, it latches it until the next tap.
    static void handleTacticalKey(bool down);

    // SKModalMode
    const char* getModalName() const override { return "tactical overview"; }
    /// Escape, a teleport or RLVa turn the overview off.
    void closeModal() override;
    void onTopChanged(bool on_top) override;
    void renderModal(const LLRect& world_rect_raw) override;
    bool pickGround(S32 x, S32 y, LLVector3d& ground) const override;
    /// SKToolMapPan's drag: the ground point grabbed at the press stays under the cursor.
    bool beginPan(S32 x, S32 y) override;
    void pan(S32 x, S32 y) override;
    void endPan() override { mPan = Pan(); }
    /// The overview is the mini-map writ large.
    std::vector<std::string> coveredFloaters() const override { return { "mini_map" }; }
    /// Clicks reach the pie, which picks the people and objects the overview draws.
    bool passesClicks() const override { return true; }
    /// The agent's height below `eye`, which the flycam's speed is measured from.
    bool groundBelow(const LLVector3d& eye, LLVector3d& ground) const override;
    /// Lower than the world map's, the battlefield being smaller than the grid (SKTacticalFlycamMaxScale).
    F64 flycamMaxScale() const override;
    /// The move actions pan the map on the ground, in widths of the visible ground a second
    /// (SKTacticalKeyPanSpeed), kept over the battlefield. Up and down do nothing here.
    void move(const skmodalview::Move& move) override;

private:
    /// Caps Lock is taken from the OS while the tactical action is bound to it and the world has the keyboard.
    static void updateCapsLockCapture();
    /// Claims or releases the modal view to match SKTacticalOverview.
    static void syncTactical();
    void openTactical();
    void closeTactical();
    /// How far the census reaches round the agent for the view: the camera's distance, or the battlefield's corner.
    F32 censusReach() const;
    /// What the orbit centres on: the avatar, the alt-cam focus target, or the surface under a flycam.
    void tacticalCentre(LLVector3d& centre) const;
    /// Puts the viewer camera at the overview's settings: tilted over the agent, looking the way the camera did.
    void placeTacticalCamera(bool over_other_mode);
    /// Battlefield mode: a fixed north-up camera over the region's centre, far enough to fit it in the window.
    void placeBattlefieldCamera();
    /// Puts the viewer camera there at once, without the ease LLAgentCamera gives a focus change.
    void setTacticalCamera(const LLVector3d& eye, const LLVector3d& focus);
    /// The agent's region, and with SKTacticalBattlefieldNeighbours the connected regions touching it, global.
    bool battlefieldBounds(LLVector3d& lo, LLVector3d& hi) const;
    /// The mini-map's people, firing lines, projectiles and labels over the composited view.
    void drawOverlays();
    /// The avatar markers as their lit physics shapes, hidden by the census (a faint silhouette where they are), giving
    /// way zoomed out to the mini-map's flat markers; fills the marks and labels' anchors with each marker's top.
    template <typename Project>
    void drawMarkers3D(const Project& project, S32 vw, S32 vh, std::vector<std::pair<size_t, LLVector2>>& drawn);
    /// The mini-map's hover nametags, laid out in UI pixels over the overview; an avatar in tagged shows its own name
    /// tag instead of the marker label.
    void drawLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn, const std::vector<LLVOAvatar*>& tagged, S32 vw,
                    S32 vh);
    bool rightClick(S32 x, S32 y);

    bool mOpen = false;         ///< claims the modal view
    F32 mDistance = 0.f;        ///< the open camera's distance from what it centres on
    /// A drag over the overview, from the press SKToolMapPan began it with.
    struct Pan
    {
        bool mActive = false;
        LLVector3 mGrab;        // agent space
        LLMatrix4a mView;       // the view's camera at the press
        LLMatrix4a mProj;
        S32 mViewport[4] = {};
        LLVector3d mEye;
        LLVector3d mFocus;
    } mPan;
    struct Mark
    {
        LLUUID mId;
        LLVector2 mAt;      // view pixels
    };
    std::vector<Mark> mMarks;               ///< the markers drawn last frame, for picks
    std::map<LLUUID, F32> mFlatBlend;       ///< each marker's crossfade from its body (0) to its flat marker (1)
    F64 mMarkerFadeTime = -1.0;
    skmap::HoverSet mHover;
    skmap::LabelLayout mLabels;
    std::vector<skmapdraw::TagText> mTagTexts;
    bool mEscDown = false;
    LLHandle<SKPopupPanes> mPopup;
};

#endif // SK_TACTICALOVERVIEW_H
