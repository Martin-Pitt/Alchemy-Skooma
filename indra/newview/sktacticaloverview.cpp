/**
 * @file sktacticaloverview.cpp
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

#include "llviewerprecompiledheaders.h"

#include "sktacticaloverview.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llhudnametag.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrootview.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewerjoystick.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "llworld.h"
#include "pipeline.h"
#include "skcensusavatarshape.h"
#include "skmapmenu.h"
#include "skmappalette.h"
#include "skmappeople.h"
#include "sknetmap.h"
#include "skoverviewcapture.h"
#include "skpopuppanes.h"

#include <algorithm>
#include <cmath>

namespace
{
    const F32 AVATAR_HALF_HEIGHT_M = 0.9f;     // a coarse-only avatar's position over its feet, having no box from the sim
    const F32 MARKER_HALO_PX = 1.5f;           // the dark ring that sets a flat marker off bright geometry
    const F32 MARKER_GHOST_ALPHA = 0.3f;       // its silhouette where the census hides it
    const F32 HEADING_TIP_PER_RADIUS = 1.f;    // the minimap's heading triangle (skmap::palette::HEADING_*_PX) over
    const F32 HEADING_BASE_PER_RADIUS = 0.25f; // its 6 px dot, as fractions of the body's radius beyond it
    const F32 HEADING_HALF_PER_RADIUS = 0.42f;
    const F32 HEADING_THICK_PER_RADIUS = 0.3f; // and extruded this thick
    const F32 TACTICAL_NEAR_M = 0.5f;
    const F32 TACTICAL_MIN_M = 8.f;
    const F32 TACTICAL_MAX_M = 4096.f;
    const F32 BATTLEFIELD_MAX_M = 4096.f;
    const F32 BATTLEFIELD_PITCH_DEG = 89.f;  // the untilted battlefield: short of straight down, so the camera keeps a heading to put north up
    const F32 BATTLEFIELD_MARGIN = 1.05f;
    const F32 FLAT_FADE_S = 0.15f;  // a marker's crossfade between its body and its zoomed-out flat marker
    const size_t MAX_BADGES = 64;           // coded people badged on the tactical view at most, nearest first
    const F32 BADGE_LIFT_PX = 2.f;          // a badge's gap above its marker's top

    /// SKTacticalCameraMode: a fixed north-up view fitting the battlefield, or the orbit that follows the agent.
    bool battlefieldCamera()
    {
        static LLCachedControl<U32> mode(gSavedSettings, "SKTacticalCameraMode", 0);
        return mode == 0;
    }

    const F64 PAN_MARGIN_M = 512.0;     // how far past the battlefield the view's focus may be dragged

    /// The step a pan may take: zoom-to-cursor leaves the focus at whatever depth it picked, so the limit holds the
    /// ground at the view's centre, on the agent's level, within the margin, and never pulls it back from outside.
    LLVector3d clampedPanStep(const LLVector3d& eye, const LLVector3d& focus, const LLVector3d& step, const LLVector3d& lo,
                              const LLVector3d& hi)
    {
        const LLVector3d dir = focus - eye;
        LLVector3d centre = focus;
        const F64 t = std::fabs(dir.mdV[VZ]) > 1.0e-6 ? (gAgent.getPositionGlobal().mdV[VZ] - eye.mdV[VZ]) / dir.mdV[VZ] : -1.0;
        if (t > 0.0)
        {
            centre = eye + dir * t;
        }
        LLVector3d clamped = step;
        for (S32 i = VX; i <= VY; ++i)
        {
            const F64 min_v = llmin(lo.mdV[i] - PAN_MARGIN_M, centre.mdV[i]);
            const F64 max_v = llmax(hi.mdV[i] + PAN_MARGIN_M, centre.mdV[i]);
            clamped.mdV[i] = llclamp(centre.mdV[i] + step.mdV[i], min_v, max_v) - centre.mdV[i];
        }
        return clamped;
    }

    /// Why a press or drag did not pan, with SKOverviewPanDebug, once a second at most.
    void panTrace(const char* why)
    {
        static LLCachedControl<bool> debug(gSavedSettings, "SKOverviewPanDebug", false);
        static F64 last = -1.0;
        const F64 now = LLFrameTimer::getUptimeSeconds().value();
        if (debug && now - last >= 1.0)
        {
            last = now;
            LL_INFOS("SKOverviewPan") << why << LL_ENDL;
        }
    }

    /// The ray through a viewport pixel of a camera, agent space.
    bool cursorRay(const LLMatrix4a& view, const LLMatrix4a& proj, const S32 viewport[4], const LLVector2& at, LLVector3& from,
                   LLVector3& dir)
    {
        LLMatrix4a vp, inv_vp, inv_view;
        vp.setMul(view, proj);
        if (viewport[2] <= 0 || viewport[3] <= 0 || !inv_vp.setInverse(vp) || !inv_view.setInverse(view))
        {
            return false;
        }
        LLVector4a origin, eye, ndc, far_pt;
        origin.set(0.f, 0.f, 0.f, 1.f);
        inv_view.transform4(origin, eye);
        ndc.set(at.mV[VX] / viewport[2] * 2.f - 1.f, at.mV[VY] / viewport[3] * 2.f - 1.f, 0.5f, 1.f);
        inv_vp.transform4(ndc, far_pt);
        const F32* e = eye.getF32ptr();
        const F32* f = far_pt.getF32ptr();
        if (std::fabs(e[3]) < 1e-6f || std::fabs(f[3]) < 1e-6f)
        {
            return false;
        }
        from.set(e[0] / e[3], e[1] / e[3], e[2] / e[3]);
        dir = LLVector3(f[0] / f[3], f[1] / f[3], f[2] / f[3]) - from;
        return dir.normVec() > 1e-6f;
    }

    /// Where a ray meets the plane z = plane_z ahead of it.
    bool rayPlane(const LLVector3& from, const LLVector3& dir, F32 plane_z, LLVector3& hit)
    {
        if (std::fabs(dir.mV[VZ]) < 1e-6f)
        {
            return false;
        }
        const F32 t = (plane_z - from.mV[VZ]) / dir.mV[VZ];
        if (t <= 0.f)
        {
            return false;
        }
        hit = from + dir * t;
        return true;
    }

    // The water surface over a global point, from its region (else the agent's); very low where there is none.
    F32 waterAt(const LLVector3d& pos)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos);
        region = region ? region : gAgent.getRegion();
        return region ? region->getWaterHeight() : -1.0e6f;
    }
}

namespace
{
    const F32 CENSUS_RANGE_M = 128.f;   // the overview's own census reach round what the view centres on

    SKOverviewCapture& cap()
    {
        return SKOverviewCapture::instance();
    }

    const LLStaticHashedString SK_VIEWPORT("sk_viewport");
    const LLStaticHashedString SK_ZO("sk_zo");
    const LLStaticHashedString SK_GHOST_ALPHA("sk_ghost_alpha");
}

SKTacticalOverview::SKTacticalOverview()
{
    SKModalView::instance().registerMode(this);
}

SKTacticalOverview::~SKTacticalOverview()
{
    if (SKModalView::instanceExists())
    {
        SKModalView::getInstance()->unregisterMode(this);
    }
}

// static
void SKTacticalOverview::onFrame()
{
    updateCapsLockCapture();
    syncTactical();
}

// static
bool SKTacticalOverview::shown()
{
    return instanceExists() && instance().mOpen && SKModalView::top() == &instance();
}

F32 SKTacticalOverview::censusReach() const
{
    // As far as the camera looks, which alt-cam and the wheel change; battlefield shows its area and no more, so the
    // census reaches the area's farthest corner, not the camera's height, which a tilt or a wide area takes to kilometres.
    static LLCachedControl<F32> tactical_distance(gSavedSettings, "SKTacticalDistance", 60.f);
    const F32 distance = mOpen && mDistance > 0.f ? mDistance : (F32)tactical_distance;
    F32 reach = llclamp(distance, TACTICAL_MIN_M, battlefieldCamera() ? BATTLEFIELD_MAX_M : TACTICAL_MAX_M) + CENSUS_RANGE_M;
    LLVector3d lo, hi;
    if (battlefieldCamera() && battlefieldBounds(lo, hi))
    {
        const LLVector3d agent = gAgent.getPositionGlobal();
        const F64 dx = llmax(std::fabs(lo.mdV[VX] - agent.mdV[VX]), std::fabs(hi.mdV[VX] - agent.mdV[VX]));
        const F64 dy = llmax(std::fabs(lo.mdV[VY] - agent.mdV[VY]), std::fabs(hi.mdV[VY] - agent.mdV[VY]));
        reach = llmin(reach, (F32)std::sqrt(dx * dx + dy * dy));
    }
    return reach;
}

void SKTacticalOverview::renderModal(const LLRect& /*world_rect_raw*/)
{
    // The display loop skips the world, and with it the overview's per-frame work: the view runs it here.
    updateCapsLockCapture();
    if (!mOpen)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    SKMapPeople& people = SKMapPeople::instance();
    people.feedOverview();
    SKOverviewCapture::noteViewWanted();
    SKOverviewCapture::setViewReach(censusReach());
    SKOverviewCapture::setWorldHidden(true);
    cap().frame();

    LLVector3d centre = gAgentCamera.getFocusGlobal();
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        tacticalCentre(centre); // the flycam leaves the focus behind: the surface under it
    }
    skoverview::ViewShotParams params = skoverview::ViewShotParams::fromViewerCamera(centre);
    people.occupants(params.mOccupants);
    static LLCachedControl<bool> dissolve(gSavedSettings, "SKTacticalDissolve", true);
    static LLCachedControl<F32> dissolve_radius(gSavedSettings, "SKTacticalDissolveRadius", 1.25f);
    static LLCachedControl<F32> dissolve_reach(gSavedSettings, "SKTacticalDissolveReach", 64.f);
    static LLCachedControl<F32> dissolve_soft(gSavedSettings, "SKTacticalDissolveSoftness", 0.25f);
    params.mDissolve.mEnabled = dissolve;
    params.mDissolve.mRadius = dissolve_radius;
    params.mDissolve.mReach = dissolve_reach;
    params.mDissolve.mSoftness = llclamp((F32)dissolve_soft, 0.f, 1.f);
    if (!cap().renderView(params))
    {
        return;
    }
    mDistance = llmax(dist_vec(params.mEye, gAgent.getPosAgentFromGlobal(centre)), TACTICAL_MIN_M);

    static LLCachedControl<F32> tilt(gSavedSettings, "SKTacticalTiltShift", 4.f);
    const S32* vp = cap().viewport();
    if (cap().compositeView(LLRect(vp[0], vp[1] + vp[3], vp[0] + vp[2], vp[1]), llmax((F32)tilt, 0.f)))
    {
        drawOverlays();
        gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    }
}

bool SKTacticalOverview::pickGround(S32 x, S32 y, LLVector3d& ground) const
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const S32* vp = cap().viewport();
    const LLVector2 at(x * scale.mV[VX] - vp[0], y * scale.mV[VY] - vp[1]);
    return cap().groundAt(at, ground);
}

void SKTacticalOverview::updateCapsLockCapture()
{
    // Take Caps Lock from the OS only while the world has the keyboard and the tactical action sits on it bare, so
    // chat and editors still type capitals with it.
    static const std::string caps = LLKeyboard::stringFromAccelerator(MASK_NONE, KEY_CAPSLOCK);
    static U32 frame = 0;
    static bool bound = false;
    if (frame++ % 30 == 0)
    {
        const std::string keys = gViewerInput.getKeyBindingAsString("", "tactical_overview");
        bound = false;
        for (size_t at = 0; at <= keys.size();)
        {
            const size_t end = keys.find(" | ", at);
            if (keys.substr(at, end == std::string::npos ? std::string::npos : end - at) == caps)
            {
                bound = true;
                break;
            }
            if (end == std::string::npos)
            {
                break;
            }
            at = end + 3;
        }
    }
    const bool want = bound && gFocusMgr.getAppHasFocus() && !gFocusMgr.getKeyboardFocus();
    if (LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr)
    {
        window->skSetCapsLockCapture(want);
    }
}

void SKTacticalOverview::handleTacticalKey(bool down)
{
    static F64 pressed_at = -1.0;
    static U32 pressed_frame = 0;
    static bool latched = false;
    static bool closing = false;
    static LLCachedControl<F32> tap_s(gSavedSettings, "SKTacticalTapSeconds", 0.35f);
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (down)
    {
        pressed_at = now;
        pressed_frame = LLFrameTimer::getFrameCount();
        closing = latched;
        latched = false;
        gSavedSettings.setBOOL("SKTacticalOverview", !closing);
        return;
    }
    if (closing || pressed_at < 0.0)
    {
        closing = false;
        return;
    }
    // A release within a couple of frames is a tap however long they took: opening the view stalls a frame.
    if (now - pressed_at < (F64)tap_s || LLFrameTimer::getFrameCount() - pressed_frame <= 2)
    {
        latched = true;
        return;
    }
    gSavedSettings.setBOOL("SKTacticalOverview", false);
}

// static
void SKTacticalOverview::syncTactical()
{
    // The setting is the switch, from the key, the menu or anywhere else; the modal view follows it.
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    static bool connected = false;
    if (!connected)
    {
        // A change made while another mode covers the world, when no world frame runs onFrame.
        connected = true;
        gSavedSettings.getControl("SKTacticalOverview")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { syncTactical(); });
        // A new camera mode moves an open overview's camera at once.
        auto replace = [](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (instanceExists() && instance().mOpen && SKModalView::top() == &instance())
            {
                instance().placeTacticalCamera(false);
            }
        };
        gSavedSettings.getControl("SKTacticalCameraMode")->getSignal()->connect(replace);
        gSavedSettings.getControl("SKTacticalBattlefieldNeighbours")->getSignal()->connect(replace);
        auto tilt = [](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (battlefieldCamera() && instanceExists() && instance().mOpen && SKModalView::top() == &instance())
            {
                instance().placeBattlefieldCamera();
            }
        };
        gSavedSettings.getControl("SKTacticalBattlefieldPitch")->getSignal()->connect(tilt);
    }
    if (tactical == (instanceExists() && instance().mOpen))
    {
        return;
    }
    if (tactical)
    {
        instance().openTactical();
    }
    else
    {
        instance().closeTactical();
    }
}

void SKTacticalOverview::openTactical()
{
    const bool over_other_mode = SKModalView::worldCovered();
    if (!SKModalView::instance().claim(this, SKModalView::PRIORITY_TACTICAL_OVERVIEW))
    {
        // Logged in, only RLVa holds the view shut: the switch goes off rather than open later by surprise.
        if (LLStartUp::getStartupState() >= STATE_STARTED)
        {
            gSavedSettings.setBOOL("SKTacticalOverview", false);
        }
        return;
    }
    mOpen = true;
    if (SKModalView::top() == this)
    {
        placeTacticalCamera(over_other_mode);
    }
}

void SKTacticalOverview::closeTactical()
{
    if (!mOpen)
    {
        return;
    }
    if (SKModalView::top() == this && !battlefieldCamera())
    {
        // The next open starts as far out as this one ended.
        const F32 distance = (F32)(gAgentCamera.getCameraPositionGlobal() - gAgentCamera.getFocusGlobal()).magVec();
        gSavedSettings.setF32("SKTacticalDistance", llclamp(distance, TACTICAL_MIN_M, TACTICAL_MAX_M));
    }
    mOpen = false;
    SKOverviewCapture::setWorldHidden(false);
    if (SKPopupPanes* popup = mPopup.get())
    {
        popup->closeAll();
    }
    // The last release puts the world camera back; a map under the overview takes its own back.
    SKModalView::instance().release(this);
}

void SKTacticalOverview::closeModal()
{
    gSavedSettings.setBOOL("SKTacticalOverview", false);
    closeTactical();
}

void SKTacticalOverview::onTopChanged(bool on_top)
{
    if (on_top && mOpen)
    {
        placeTacticalCamera(false);
    }
}

void SKTacticalOverview::placeTacticalCamera(bool over_other_mode)
{
    if (battlefieldCamera())
    {
        placeBattlefieldCamera();
        return;
    }
    static LLCachedControl<F32> distance_setting(gSavedSettings, "SKTacticalDistance", 60.f);
    static LLCachedControl<F32> pitch_setting(gSavedSettings, "SKTacticalPitch", 55.f);
    // Over the agent's floor when the camera follows it or another mode had moved it; else what alt-cam looked at.
    LLVector3d focus;
    tacticalCentre(focus);
    if (over_other_mode || gAgentCamera.getFocusOnAvatar())
    {
        F32 floor_z, cut_z;
        cap().cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
        focus = gAgent.getPosGlobalFromAgent(LLVector3(gAgent.getPositionAgent().mV[VX], gAgent.getPositionAgent().mV[VY], floor_z));
    }
    LLVector3 heading = LLViewerCamera::getInstance()->getAtAxis();
    heading.mV[VZ] = 0.f;
    if (heading.normVec() < 1.0e-3f)
    {
        heading.setVec(1.f, 0.f, 0.f);
    }
    const F32 pitch = llclamp((F32)pitch_setting, 5.f, 89.5f) * DEG_TO_RAD;
    const F32 distance = llclamp((F32)distance_setting, TACTICAL_MIN_M, TACTICAL_MAX_M);
    const LLVector3d eye = focus - LLVector3d(heading * (distance * std::cos(pitch))) + LLVector3d(0.0, 0.0, distance * std::sin(pitch));
    setTacticalCamera(eye, focus);
}

void SKTacticalOverview::setTacticalCamera(const LLVector3d& eye, const LLVector3d& focus)
{
    // Snapped, not eased, so the overview can be flicked on and off for bearings.
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(eye, focus, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
}

bool SKTacticalOverview::battlefieldBounds(LLVector3d& lo, LLVector3d& hi) const
{
    static LLCachedControl<bool> neighbours(gSavedSettings, "SKTacticalBattlefieldNeighbours", false);
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return false;
    }
    // The agent's region, and with SKTacticalBattlefieldNeighbours the connected regions that touch it.
    const LLVector3d origin = region->getOriginGlobal();
    const F64 width = region->getWidth();
    lo = origin;
    hi = origin + LLVector3d(width, width, 0.0);
    if (neighbours)
    {
        const LLVector3d own_lo = lo;
        const LLVector3d own_hi = hi;
        for (LLViewerRegion* other : LLWorld::getInstance()->getRegionList())
        {
            const LLVector3d o = other->getOriginGlobal();
            const F64 w = other->getWidth();
            if (other == region || o.mdV[VX] > own_hi.mdV[VX] || o.mdV[VX] + w < own_lo.mdV[VX] || o.mdV[VY] > own_hi.mdV[VY] ||
                o.mdV[VY] + w < own_lo.mdV[VY])
            {
                continue;
            }
            lo.mdV[VX] = llmin(lo.mdV[VX], o.mdV[VX]);
            lo.mdV[VY] = llmin(lo.mdV[VY], o.mdV[VY]);
            hi.mdV[VX] = llmax(hi.mdV[VX], o.mdV[VX] + w);
            hi.mdV[VY] = llmax(hi.mdV[VY], o.mdV[VY] + w);
        }
    }
    return true;
}

void SKTacticalOverview::placeBattlefieldCamera()
{
    LLVector3d lo, hi;
    if (!battlefieldBounds(lo, hi))
    {
        return;
    }
    F32 floor_z, cut_z;
    cap().cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    const LLVector3d floor = gAgent.getPosGlobalFromAgent(LLVector3(0.f, 0.f, floor_z));
    const LLVector3d focus((lo.mdV[VX] + hi.mdV[VX]) * 0.5, (lo.mdV[VY] + hi.mdV[VY]) * 0.5, floor.mdV[VZ]);
    // North up, tilted to look north from the south; far enough that every corner of the area lies in the frustum,
    // which a tilt turns into a trapezoid on the ground.
    static LLCachedControl<F32> pitch_setting(gSavedSettings, "SKTacticalBattlefieldPitch", BATTLEFIELD_PITCH_DEG);
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLRect world = gViewerWindow->getWorldViewRectRaw();
    const F32 aspect = world.getHeight() > 0 ? (F32)world.getWidth() / (F32)world.getHeight() : camera->getAspect();
    const F32 tan_half = std::tan(camera->getView() * 0.5f);
    const F32 pitch = llclamp((F32)pitch_setting, 5.f, BATTLEFIELD_PITCH_DEG) * DEG_TO_RAD;
    const LLVector3 forward(0.f, std::cos(pitch), -std::sin(pitch));
    const LLVector3 up(0.f, std::sin(pitch), std::cos(pitch));
    const F32 half_x = (F32)(hi.mdV[VX] - lo.mdV[VX]) * 0.5f * BATTLEFIELD_MARGIN;
    const F32 half_y = (F32)(hi.mdV[VY] - lo.mdV[VY]) * 0.5f * BATTLEFIELD_MARGIN;
    F32 fit = 0.f;
    for (S32 corner = 0; corner < 4; ++corner)
    {
        // A corner a from the focus sits at depth a.forward + d before an eye d back along forward, so it fits once
        // d covers its offsets across and up over the half-angle tangents.
        const LLVector3 a((corner & 1) ? half_x : -half_x, (corner & 2) ? half_y : -half_y, 0.f);
        const F32 depth = a * forward;
        fit = llmax(fit, llmax(std::fabs(a.mV[VX]) / (tan_half * aspect), std::fabs(a * up) / tan_half) - depth);
    }
    const F32 distance = llclamp(fit, TACTICAL_MIN_M, BATTLEFIELD_MAX_M);
    const LLVector3d north(0.0, 1.0, 0.0);
    const LLVector3d eye = focus - north * (distance * std::cos(pitch)) + LLVector3d(0.0, 0.0, distance * std::sin(pitch));
    setTacticalCamera(eye, focus);
}

bool SKTacticalOverview::groundBelow(const LLVector3d& eye, LLVector3d& ground) const
{
    ground.setVec(eye.mdV[VX], eye.mdV[VY], gAgent.getPositionGlobal().mdV[VZ]);
    return true;
}

F64 SKTacticalOverview::flycamMaxScale() const
{
    static LLCachedControl<F32> max_scale(gSavedSettings, "SKTacticalFlycamMaxScale", 12.f);
    return llmax((F64)max_scale, 1.0);
}

void SKTacticalOverview::tacticalCentre(LLVector3d& centre) const
{
    // Follow: the avatar. Alt-cam: the camera's focus target. Flycam: the surface under the camera.
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        centre = gAgent.getPosGlobalFromAgent(eye);
        F32 ground = LLWorld::getInstance()->resolveLandHeightGlobal(centre);
        LLVector4a start, end, hit;
        start.load3(eye.mV);
        end.set(eye.mV[VX], eye.mV[VY], llmin(ground, eye.mV[VZ]) - 1.f, 1.f);
        S32 face = -1;
        if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, &hit))
        {
            ground = llmax(ground, hit.getF32ptr()[VZ]);
        }
        centre.mdV[VZ] = llmax(ground, waterAt(centre)); // over water the surface, not the seabed
        return;
    }
    centre = gAgentCamera.getFocusOnAvatar() ? gAgent.getPositionGlobal() : gAgentCamera.getFocusTargetGlobal();
}

// static
bool SKTacticalOverview::handleRightClick(S32 x, S32 y)
{
    return shown() && instance().rightClick(x, y);
}

bool SKTacticalOverview::rightClick(S32 x, S32 y)
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - cap().viewport()[0], y * scale.mV[VY] - cap().viewport()[1]);
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= cap().viewport()[2] || at.mV[VY] >= cap().viewport()[3])
    {
        return false;
    }
    // The mini-map's pick: every marker within the tactical pick radius, nearest first.
    const F32 radius = skmap::TACTICAL_PICK_PX * llmax(scale.mV[VX], scale.mV[VY]);
    std::vector<std::pair<F32, LLUUID>> near_click;
    for (const Mark& mark : mMarks)
    {
        const F32 dist = dist_vec(mark.mAt, at);
        if (dist <= radius)
        {
            near_click.emplace_back(dist, mark.mId);
        }
    }
    std::sort(near_click.begin(), near_click.end());
    std::vector<LLUUID> ids;
    for (const auto& n : near_click)
    {
        ids.push_back(n.second);
    }
    LLVector3d world;
    if (!cap().groundAt(at, world))
    {
        world = gAgent.getPosGlobalFromAgent(cap().viewCentre());
    }
    if (SKPopupPanes* old = mPopup.get())
    {
        old->closeAll();
    }
    mPopup = SKMapMenu::open(gViewerWindow->getRootView(), x, y, world, SKMapMenu::freeze(ids));
    return true;
}

bool SKTacticalOverview::beginPan(S32 x, S32 y)
{
    // A press between frames still has the view the last frame showed.
    if (!shown() || !cap().viewPickable())
    {
        panTrace("press ignored: no tactical frame shown yet");
        return false;
    }
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - cap().viewport()[0], y * scale.mV[VY] - cap().viewport()[1]);
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= cap().viewport()[2] || at.mV[VY] >= cap().viewport()[3])
    {
        panTrace("press ignored: outside the tactical viewport");
        return false;
    }
    LLVector3 from, dir;
    if (!cursorRay(cap().viewMatrix(), cap().projMatrix(), cap().viewport(), at, from, dir) || dir.mV[VZ] > -1e-6f)
    {
        panTrace("press ignored: the cursor's ray does not point down");
        return false;
    }
    LLVector3d grab_global;
    if (!cap().groundAt(at, grab_global))
    {
        panTrace("press ignored: no ground under the cursor");
        return false;
    }
    mPan = Pan();
    mPan.mActive = true;
    mPan.mGrab = gAgent.getPosAgentFromGlobal(grab_global);
    mPan.mView = cap().viewMatrix();
    mPan.mProj = cap().projMatrix();
    std::copy(cap().viewport(), cap().viewport() + 4, mPan.mViewport);
    mPan.mEye = gAgentCamera.getCameraPositionGlobal();
    mPan.mFocus = gAgentCamera.getFocusGlobal();
    return true;
}

void SKTacticalOverview::pan(S32 x, S32 y)
{
    if (!mPan.mActive)
    {
        return;
    }
    // The camera only translates, so the press's ray through the cursor meets the grab's plane at Q and P - Q puts P
    // back under the cursor; nothing is picked again while dragging.
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mPan.mViewport[0], y * scale.mV[VY] - mPan.mViewport[1]);
    LLVector3 from, dir, q;
    if (!cursorRay(mPan.mView, mPan.mProj, mPan.mViewport, at, from, dir) || !rayPlane(from, dir, mPan.mGrab.mV[VZ], q))
    {
        panTrace("drag step skipped: the cursor's ray misses the grab's plane");
        return;
    }
    LLVector3d step(mPan.mGrab.mV[VX] - q.mV[VX], mPan.mGrab.mV[VY] - q.mV[VY], 0.0);
    LLVector3d lo, hi;
    if (battlefieldBounds(lo, hi))
    {
        step = clampedPanStep(mPan.mEye, mPan.mFocus, step, lo, hi);
    }
    setTacticalCamera(mPan.mEye + step, mPan.mFocus + step);
}

void SKTacticalOverview::move(const skmodalview::Move& move)
{
    if (mPan.mActive || !shown())
    {
        return;
    }
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLVector3 dir;
    if (!skmodalview::groundDirection(camera->getAtAxis(), camera->getUpAxis(), move.mForward, move.mLeft, dir))
    {
        return;
    }

    // A fraction of the visible ground's width a second, so it feels the same at any zoom.
    static LLCachedControl<F32> speed_setting(gSavedSettings, "SKTacticalKeyPanSpeed", 0.5f);
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const F32 distance = llmax((F32)(eye - focus).magVec(), 1.f);
    const F32 ground_width = 2.f * distance * std::tan(camera->getView() * camera->getAspect() * 0.5f);
    const F32 metres = ground_width * llmax((F32)speed_setting, 0.f) * (move.mFast ? 3.f : 1.f) * llmin(gFrameIntervalSeconds.value(), 0.1f);

    LLVector3d step(dir * metres);
    LLVector3d lo, hi;
    if (battlefieldBounds(lo, hi))
    {
        step = clampedPanStep(eye, focus, step, lo, hi);
    }
    setTacticalCamera(eye + step, focus + step);
}

template <typename Project>
void SKTacticalOverview::drawMarkers3D(const Project& project, S32 vw, S32 vh, std::vector<std::pair<size_t, LLVector2>>& drawn)
{
    LL_PROFILE_ZONE_NAMED("sk tactical markers 3d");
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    LLMatrix4a inverse_view;
    inverse_view.setInverse(cap().viewMatrix());
    const LLVector3 eye(inverse_view.getF32ptr()[12], inverse_view.getF32ptr()[13], inverse_view.getF32ptr()[14]);
    const LLVector3 light = LLVector3(-0.45f, 0.45f, 0.77f) / LLVector3(-0.45f, 0.45f, 0.77f).length();

    struct Marker
    {
        LLVector3 mFloor;   // agent space, the avatar's feet
        F32 mHeading = 0.f;
        bool mHasHeading = false;
        LLColor4 mColour;
        size_t mPerson = (size_t)-1;
        LLUUID mId;
        bool mCoarse = false;   // known only from the region's coarse locations: no rotation, a coarse height
        F32 mHeight = 0.f;      // the avatar's own height when known, else the default
        F32 mRadius = 0.f;      // half the larger of its x and y size when known, else in proportion to the height
        bool mHasShape = false; // drawn as its physics shape, from its agent size, pose and rotation
        LLVector3 mCentre;      // agent space, the avatar's position: the shape's origin
        LLQuaternion mRotation;
        skcensus::AvatarPose mPose = skcensus::AvatarPose::STANDING;
    };
    // A loaded avatar's own size, as the sim has it: getScale() is the agent size from its object update (what
    // llGetAgentSize reports), its box centred on the avatar's position, so its base is the floor the sim stands it
    // on. The position is the object's, not the render position, which carries the hover offset. Sitting keeps the
    // same size. Only when no size has come is the shape's body height used, on its pelvis-to-foot.
    auto sized = [](Marker& m, const LLUUID& id)
    {
        LLViewerObject* object = gObjectList.findObject(id);
        LLVOAvatar* avatar = object && object->isAvatar() ? static_cast<LLVOAvatar*>(object) : nullptr;
        if (!avatar || avatar->isDead())
        {
            return;
        }
        const LLVector3 size = avatar->getScale();
        const LLVector3 centre = avatar->getPositionAgent();
        if (size.mV[VZ] > 0.1f)
        {
            m.mFloor = centre - LLVector3(0.f, 0.f, size.mV[VZ] * 0.5f);
            m.mHeight = size.mV[VZ];
            m.mRadius = llmax(size.mV[VX], size.mV[VY]) * 0.5f;
            // The census's physics shape for its pose, about its position, turned as the sim has the avatar.
            m.mHasShape = true;
            m.mCentre = centre;
            m.mRotation = avatar->isSelf() ? avatar->getRenderRotation() : avatar->skPredictedRotationRegion();
            m.mPose = skcensus::avatarPose(avatar);
        }
        else if (avatar->mBodySize.mV[VZ] > 0.1f)
        {
            m.mFloor = avatar->getRenderPosition() - LLVector3(0.f, 0.f, avatar->getPelvisToFoot());
            m.mHeight = avatar->mBodySize.mV[VZ];
        }
    };
    std::vector<Marker> markers;
    for (size_t i = 0; i < people.size(); ++i)
    {
        const SKMapPeople::Person& p = people[i];
        Marker m;
        // Tiers by what is known (SKMapPeople's own flags): the avatar object (live position and rotation), or only a
        // coarse location, whose height may be the capped value, in which case it stands on the land under it.
        m.mCoarse = !p.mLoaded;
        LLVector3d where = p.mPos;
        if (!p.mZKnown)
        {
            where.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(where) + AVATAR_HALF_HEIGHT_M;
        }
        m.mFloor = gAgent.getPosAgentFromGlobal(where) - LLVector3(0.f, 0.f, (F32)(p.mPos.mdV[VZ] - p.mFeetZ));
        m.mHeading = p.mHeading;
        m.mHasHeading = p.mHasHeading && !m.mCoarse;
        const skmap::PaletteColour& coded = skmap::palette::DOT_COMBAT;
        m.mColour = p.mVisibility == skmap::Visibility::DOT_CODED ? LLColor4(coded.mR, coded.mG, coded.mB, 1.f)
                                                                  : LLColor4(p.mColour.mV[0], p.mColour.mV[1], p.mColour.mV[2], 1.f);
        if (m.mCoarse)
        {
            m.mColour = LLColor4(m.mColour.mV[0] * 0.7f, m.mColour.mV[1] * 0.7f, m.mColour.mV[2] * 0.7f, 0.85f);
        }
        else
        {
            sized(m, p.mId);
        }
        m.mPerson = i;
        m.mId = p.mId;
        markers.push_back(m);
    }
    if (isAgentAvatarValid())
    {
        Marker m;
        m.mFloor = gAgent.getPosAgentFromGlobal(LLVector3d(gAgent.getPositionGlobal().mdV[VX], gAgent.getPositionGlobal().mdV[VY], SKMapPeople::feetZ(gAgentAvatarp)));
        sized(m, gAgent.getID());
        const LLVector3 fwd = gAgent.getAtAxis();
        m.mHeading = std::atan2(fwd.mV[VY], fwd.mV[VX]);
        m.mHasHeading = true;
        const skmap::PaletteColour& self = skmap::palette::SELF_DOT;
        m.mColour = LLColor4(self.mR, self.mG, self.mB, 1.f);
        markers.push_back(m);
    }
    if (markers.empty() || !SKOverviewCapture::markerProgram().mProgramObject || !cap().sceneDepth() || !cap().sceneDepth()->isComplete())
    {
        return;
    }

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(cap().projMatrix());
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(cap().viewMatrix());
    gGLLastMatrix = NULL;
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    LLGLDisable cull_face(GL_CULL_FACE);
    SKOverviewCapture::markerProgram().bind();
    SKOverviewCapture::markerProgram().bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, cap().sceneDepth());
    SKOverviewCapture::markerProgram().uniform4f(SK_VIEWPORT, (F32)cap().viewport()[0], (F32)cap().viewport()[1], (F32)vw, (F32)vh);
    SKOverviewCapture::markerProgram().uniform1f(SK_ZO, LLRender::sReverseZ ? 1.f : 0.f);
    SKOverviewCapture::markerProgram().uniform1f(SK_GHOST_ALPHA, MARKER_GHOST_ALPHA);

    gGL.begin(LLRender::TRIANGLES);
    // A triangle facing the camera, lit by the overview's key light; those facing away are left out (no depth buffer).
    auto tri = [&](const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& colour)
    {
        LLVector3 n = (b - a) % (c - a);
        if (n.normVec() < 1e-8f)
        {
            return;
        }
        if (n * (eye - a) < 0.f)
        {
            return;
        }
        const F32 shade = 0.45f + 0.55f * llmax(n * light, 0.f);
        gGL.color4f(colour.mV[0] * shade, colour.mV[1] * shade, colour.mV[2] * shade, colour.mV[3]);
        gGL.vertex3fv(a.mV);
        gGL.vertex3fv(b.mV);
        gGL.vertex3fv(c.mV);
    };
    // Zoomed out past SKTacticalFlatMarkerPixelsPerMetre the bodies give way, crossfaded, to the mini-map's own flat
    // markers at its constant pixel size, drawn over everything so they read as easily as on the mini-map.
    static LLCachedControl<F32> swap_ppm(gSavedSettings, "SKTacticalFlatMarkerPixelsPerMetre", 10.f);
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const F32 fade_step = mMarkerFadeTime < 0.0 ? 1.f : (F32)(now - mMarkerFadeTime) / FLAT_FADE_S;
    mMarkerFadeTime = now;
    std::map<LLUUID, F32> blends;
    const LLVector3 sideways = LLViewerCamera::getInstance()->getLeftAxis();
    auto faded = [](const LLColor4& c, F32 a) { return LLColor4(c.mV[0], c.mV[1], c.mV[2], c.mV[3] * a); };
    struct Flat
    {
        size_t mPerson = (size_t)-1;   // none for the agent's own
        LLVector2 mAt;
        LLVector2 mAhead;
        bool mHasAhead = false;
    };
    std::vector<Flat> flats;
    // Where a point lands in the window, off it or not; false only behind the camera.
    LLMatrix4a vp;
    vp.setMul(cap().viewMatrix(), cap().projMatrix());
    auto screen = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
    {
        LLVector4a p, clip;
        p.set(agent_pos.mV[VX], agent_pos.mV[VY], agent_pos.mV[VZ], 1.f);
        vp.transform4(p, clip);
        const F32 w = clip.getF32ptr()[3];
        if (w <= 1e-4f)
        {
            return false;
        }
        out.set((clip.getF32ptr()[0] / w * 0.5f + 0.5f) * vw, (clip.getF32ptr()[1] / w * 0.5f + 0.5f) * vh);
        return true;
    };
    for (const Marker& m : markers)
    {
        // Kept while any of it, feet to head, is near the window, so feet just off its edge leave the body drawn.
        LLVector2 at0, head;
        if (!screen(m.mFloor, at0) || !screen(m.mFloor + LLVector3(0.f, 0.f, m.mHeight > 0.f ? m.mHeight : 2.f), head))
        {
            continue;
        }
        const F32 margin = 32.f;
        if (llmax(at0.mV[VX], head.mV[VX]) < -margin || llmax(at0.mV[VY], head.mV[VY]) < -margin ||
            llmin(at0.mV[VX], head.mV[VX]) > vw + margin || llmin(at0.mV[VY], head.mV[VY]) > vh + margin)
        {
            continue;
        }
        LLVector2 side;
        const F32 ppm = screen(m.mFloor + sideways, side) ? (side - at0).length() : 0.f;
        const bool has_body = m.mHasShape && !m.mCoarse;
        // Hysteresis as well as the fade: a flat marker turns back into a body only a fifth past the threshold.
        const auto known = mFlatBlend.find(m.mId);
        F32 blend = known != mFlatBlend.end() ? known->second : (ppm < swap_ppm ? 1.f : 0.f);
        const bool small = ppm < swap_ppm * (blend > 0.5f ? 1.2f : 1.f);
        blend = has_body ? llclamp(blend + (small ? fade_step : -fade_step), 0.f, 1.f) : 1.f;
        blends[m.mId] = blend;
        LLVector3 top_c = m.mFloor;
        if (has_body && blend < 1.f)
        {
            // The avatar's own physics shape, about its position, at true size.
            const LLColor4 colour = faded(m.mColour, 1.f - blend);
            const std::vector<LLVector3>& shape = skcensus::avatarShape(m.mPose, m.mHeight);
            F32 top_z = -F32_MAX;
            for (size_t v = 0; v + 2 < shape.size(); v += 3)
            {
                const LLVector3 a = m.mCentre + shape[v] * m.mRotation;
                const LLVector3 b = m.mCentre + shape[v + 1] * m.mRotation;
                const LLVector3 c = m.mCentre + shape[v + 2] * m.mRotation;
                tri(a, b, c, colour);
                top_z = llmax(top_z, llmax(a.mV[VZ], llmax(b.mV[VZ], c.mV[VZ])));
            }
            if (blend < 0.5f)
            {
                top_c = LLVector3(m.mCentre.mV[VX], m.mCentre.mV[VY], top_z);
            }
            if (m.mHasHeading)
            {
                // The minimap's heading wedge as a flat slab: a solid triangle just clear of the body, its top flush
                // with the shape's, along the body's heading, in the minimap's proportions to the body's radius.
                const LLVector3 ahead(std::cos(m.mHeading), std::sin(m.mHeading), 0.f);
                const LLVector3 across(-ahead.mV[VY], ahead.mV[VX], 0.f);
                const F32 body_r = m.mRadius;
                const LLVector3 from(m.mCentre.mV[VX], m.mCentre.mV[VY], top_z - body_r * HEADING_THICK_PER_RADIUS * 0.5f);
                const LLVector3 tip = from + ahead * (body_r * (1.f + HEADING_TIP_PER_RADIUS));
                const LLVector3 base = from + ahead * (body_r * (1.f + HEADING_BASE_PER_RADIUS));
                const LLVector3 l = base + across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 rr = base - across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 up(0.f, 0.f, body_r * HEADING_THICK_PER_RADIUS * 0.5f);
                tri(tip + up, l + up, rr + up, colour);
                tri(tip - up, rr - up, l - up, colour);
                tri(tip - up, l - up, l + up, colour);
                tri(tip - up, l + up, tip + up, colour);
                tri(rr - up, tip - up, tip + up, colour);
                tri(rr - up, tip + up, rr + up, colour);
                tri(l - up, rr - up, rr + up, colour);
                tri(l - up, rr + up, l + up, colour);
            }
        }
        // Labels, hover, picks and menus take the projected top of the marker, or the flat marker's top edge.
        LLVector2 top;
        bool placed = false;
        if (blend >= 0.5f)
        {
            Flat f;
            f.mPerson = m.mPerson;
            f.mAt = at0;
            LLVector2 tip;
            if (m.mHasHeading && screen(m.mFloor + LLVector3(std::cos(m.mHeading), std::sin(m.mHeading), 0.f), tip))
            {
                f.mAhead = tip - at0;
                f.mHasAhead = f.mAhead.normVec() > 1e-4f;
            }
            flats.push_back(f);
            const F32 radius = m.mPerson != (size_t)-1 ? skmapdraw::markerRadius(people[m.mPerson], skmap::FloorRelation::SAME)
                                                       : skmap::palette::SELF_DOT_RADIUS_PX;
            top = at0 + LLVector2(0.f, radius);
            placed = true;
        }
        else
        {
            placed = project(top_c, top);
        }
        if (placed)
        {
            if (m.mPerson != (size_t)-1)
            {
                mMarks.push_back({ m.mId, top });
                drawn.emplace_back(m.mPerson, top);
            }
        }
    }
    mFlatBlend.swap(blends);
    gGL.end();
    gGL.flush();
    SKOverviewCapture::markerProgram().unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    SKOverviewCapture::markerProgram().unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
    // The 2D pass the caller resumes in, where the flat markers draw as the mini-map draws them.
    gUIProgram.bind();
    if (flats.empty())
    {
        return;
    }
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    const F32 px = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
    auto radius_of = [&](const Flat& f)
    {
        return f.mPerson != (size_t)-1 ? skmapdraw::markerRadius(people[f.mPerson], skmap::FloorRelation::SAME)
                                       : skmap::palette::SELF_DOT_RADIUS_PX;
    };
    for (const Flat& f : flats)
    {
        skmapdraw::shadow(f.mAt, radius_of(f));
    }
    gGL.getTextureSlot(0)->unbind();
    gGL.begin(LLRender::TRIANGLES);
    for (const Flat& f : flats)
    {
        const F32 radius = radius_of(f);
        skmapdraw::colour(skmap::palette::CODE_INK);
        skmapdraw::disc(f.mAt, radius + MARKER_HALO_PX);
        if (f.mPerson != (size_t)-1)
        {
            skmapdraw::personMarker(people[f.mPerson], f.mAt, radius, f.mHasAhead ? &f.mAhead : nullptr,
                                    skmap::FloorRelation::SAME, 0.f, px);
            continue;
        }
        skmapdraw::colour(skmap::palette::SELF_DOT);
        if (f.mHasAhead)
        {
            const LLVector2 across(-f.mAhead.mV[VY], f.mAhead.mV[VX]);
            const LLVector2 base = f.mAt + f.mAhead * (radius + skmap::palette::HEADING_BASE_PX);
            skmapdraw::triangle(f.mAt + f.mAhead * (radius + skmap::palette::HEADING_TIP_PX),
                                base + across * skmap::palette::HEADING_HALF_PX, base - across * skmap::palette::HEADING_HALF_PX);
        }
        skmapdraw::disc(f.mAt, radius);
    }
    gGL.end();
    for (const Flat& f : flats)
    {
        if (f.mPerson != (size_t)-1)
        {
            skmapdraw::personCode(people[f.mPerson], f.mAt, skmap::FloorRelation::SAME);
        }
    }
}

void SKTacticalOverview::drawOverlays()
{
    const S32* viewport = cap().viewport();
    const S32 vx = viewport[0], vy = viewport[1], vw = viewport[2], vh = viewport[3];
    {
        // The mini-map's own markers and visibility rules (SKMapPeople, skmapdraw::personMarker), at a constant pixel
        // size; headings are the avatars' facings laid on the ground and projected through the tilted camera.
        LL_PROFILE_ZONE_NAMED("sk tactical markers");
        LLMatrix4a vp;
        vp.setMul(cap().viewMatrix(), cap().projMatrix());
        auto project = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
        {
            LLVector4a p, clip;
            p.set(agent_pos.mV[VX], agent_pos.mV[VY], agent_pos.mV[VZ], 1.f);
            vp.transform4(p, clip);
            const F32 w = clip.getF32ptr()[3];
            if (w <= 1e-4f)
            {
                return false;
            }
            out.set((clip.getF32ptr()[0] / w * 0.5f + 0.5f) * vw, (clip.getF32ptr()[1] / w * 0.5f + 0.5f) * vh);
            return out.mV[VX] > -32.f && out.mV[VY] > -32.f && out.mV[VX] < vw + 32.f && out.mV[VY] < vh + 32.f;
        };
        auto ground_ahead = [&](const LLVector3& agent_pos, const LLVector2& at, F32 heading, LLVector2& ahead) -> bool
        {
            LLVector2 tip;
            if (!project(agent_pos + LLVector3(std::cos(heading), std::sin(heading), 0.f), tip))
            {
                return false;
            }
            ahead = tip - at;
            return ahead.normVec() > 1e-4f;
        };

        gGL.setViewport(vx, vy, vw, vh);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, (F32)vw, 0.f, (F32)vh, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGLLastMatrix = NULL;
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gUIProgram.bind();
        gGL.getTextureSlot(0)->unbind();

        static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
        const F32 px = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
        const LLVector3d centre = gAgent.getPosGlobalFromAgent(cap().viewCentre());
        const F64 reach = cap().viewReach();
        SKMapPeople::instance().noteDrawn(centre - LLVector3d(reach, reach, reach), centre + LLVector3d(reach, reach, reach));
        mMarks.clear();
        std::vector<std::pair<size_t, LLVector2>> drawn;
        drawMarkers3D(project, vw, vh, drawn);
        gGL.flush();
        {
            // Only a mouselook LookAt is precise enough to draw here; a body's facing is the mini-map's rough hint.
            auto clip_project = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
            {
                LLVector4a p, clip;
                p.set(agent_pos.mV[VX], agent_pos.mV[VY], agent_pos.mV[VZ], 1.f);
                vp.transform4(p, clip);
                const F32 w = clip.getF32ptr()[3];
                if (w <= 1e-4f)
                {
                    return false;
                }
                out.set((clip.getF32ptr()[0] / w * 0.5f + 0.5f) * vw, (clip.getF32ptr()[1] / w * 0.5f + 0.5f) * vh);
                return true;
            };
            gGL.begin(LLRender::TRIANGLES);
            for (const auto& entry : SKMapPeople::instance().firingLines())
            {
                const SKMapPeople::FiringLine& line = entry.second;
                LLVector2 a, b;
                if (!line.mPrecise || !clip_project(gAgent.getPosAgentFromGlobal(line.mFrom), a) ||
                    !clip_project(gAgent.getPosAgentFromGlobal(line.mTo), b))
                {
                    continue;
                }
                LLVector2 d = b - a;
                if (d.normVec() < 1e-3f)
                {
                    continue;
                }
                // The mini-map's line, brighter at the shooter and fainter over its reach.
                const F32 reach = static_cast<F32>(dist_vec(line.mFrom, line.mTo)) / skmap::palette::FIRING_REACH_M;
                const LLColor4 near_c(1.f, 1.f, 1.f, skmap::palette::FIRING_ALPHA_NEAR);
                const LLColor4 far_c(1.f, 1.f, 1.f,
                                     lerp(skmap::palette::FIRING_ALPHA_NEAR, skmap::palette::FIRING_ALPHA_FAR, llclamp(reach, 0.f, 1.f)));
                const LLVector2 n(-d.mV[VY] * 0.5f, d.mV[VX] * 0.5f);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
                gGL.color4fv(far_c.mV);
                gGL.vertex2f((b - n).mV[VX], (b - n).mV[VY]);
                gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
                gGL.color4fv(far_c.mV);
                gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a + n).mV[VX], (a + n).mV[VY]);
            }
            gGL.end();
            gGL.flush();
        }
        {
            // Projectiles over the markers, as on the mini-map: a streak the prim's length, fading to its tail.
            std::map<LLUUID, LLColor4> shooters;
            for (const SKMapPeople::Person& person : SKMapPeople::instance().people())
            {
                shooters[person.mId] = person.mColour;
            }
            const LLColor4 plain = LLColor4(skmap::palette::PROJECTILE.mR, skmap::palette::PROJECTILE.mG, skmap::palette::PROJECTILE.mB,
                                            skmap::palette::PROJECTILE.mA);
            gGL.begin(LLRender::TRIANGLES);
            // Fast movers' recent path, faded from the head's opacity to clear at the oldest point.
            static LLCachedControl<F32> trail_px(gSavedSettings, "SKMapProjectileMinWidthPx", 2.f);
            const F64 now = LLFrameTimer::getUptimeSeconds().value();
            for (const auto& entry : SKMapPeople::instance().fastTrails())
            {
                const SKMapPeople::FastTrail& trail = entry.second;
                const auto& pts = trail.mPoints;
                const F32 half = llmax(static_cast<F32>(trail_px), 0.5f) * 0.5f;
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                {
                    LLVector2 a, b;
                    if (!project(gAgent.getPosAgentFromGlobal(pts[i].second), a) ||
                        !project(gAgent.getPosAgentFromGlobal(pts[i + 1].second), b))
                    {
                        continue;
                    }
                    LLVector2 d = a - b;
                    if (d.normVec() < 1e-4f)
                    {
                        continue;
                    }
                    const F32 life = llmax(trail.mLife, 1e-3f);
                    const F32 alpha_a = trail.mAlpha * (1.f - llclamp(static_cast<F32>(now - pts[i].first) / life, 0.f, 1.f));
                    const F32 alpha_b = trail.mAlpha * (1.f - llclamp(static_cast<F32>(now - pts[i + 1].first) / life, 0.f, 1.f));
                    const LLVector2 n(-d.mV[VY] * half, d.mV[VX] * half);
                    auto at = [&](const LLVector2& v, F32 alpha)
                    {
                        gGL.color4f(plain.mV[VRED], plain.mV[VGREEN], plain.mV[VBLUE], alpha);
                        gGL.vertex2f(v.mV[VX], v.mV[VY]);
                    };
                    at(b - n, alpha_b);
                    at(a - n, alpha_a);
                    at(a + n, alpha_a);
                    at(b - n, alpha_b);
                    at(a + n, alpha_a);
                    at(b + n, alpha_b);
                }
            }
            for (const SKMapPeople::Projectile& p : SKMapPeople::instance().projectiles())
            {
                const LLVector3 head_agent = gAgent.getPosAgentFromGlobal(p.mPos);
                LLVector2 head, back;
                if (!project(head_agent, head) || !project(head_agent - p.mDir, back))
                {
                    continue;
                }
                LLVector2 dir = head - back;
                const F32 ppm = dir.normVec();
                if (ppm < 1e-4f)
                {
                    continue;
                }
                const auto found = p.mShooter.notNull() ? shooters.find(p.mShooter) : shooters.end();
                const LLColor4& colour = found != shooters.end() ? found->second : plain;
                if (!p.mBullet)
                {
                    LLVector3d box[8];
                    p.corners(box);
                    LLVector2 corners[8];
                    bool seen = true;
                    for (S32 k = 0; k < 8 && seen; ++k)
                    {
                        seen = project(gAgent.getPosAgentFromGlobal(box[k]), corners[k]);
                    }
                    if (seen)
                    {
                        skmapdraw::projectileFootprint(corners, dir, llmin(p.mSpeed * 0.15f, 6.f) * ppm, colour);
                    }
                    continue;
                }
                skmapdraw::projectile(head, dir, p.mLength * ppm, p.mWidth * ppm, colour);
            }
            gGL.end();
            gGL.flush();
        }
        // Avatars shown large enough carry their in-world name tag, the one LLVOAvatar keeps, in place of the marker label.
        static LLCachedControl<F32> tag_ppm(gSavedSettings, "SKTacticalNametagPixelsPerMetre", 8.f);
        const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
        const LLVector3 across = LLViewerCamera::getInstance()->getLeftAxis();
        std::vector<LLVOAvatar*> tagged(drawn.size(), nullptr);
        for (size_t i = 0; i < drawn.size(); ++i)
        {
            if (drawn[i].first >= people.size())
            {
                continue;
            }
            LLViewerObject* obj = gObjectList.findObject(people[drawn[i].first].mId);
            LLVOAvatar* av = obj && obj->isAvatar() && !obj->isDead() ? static_cast<LLVOAvatar*>(obj) : nullptr;
            LLVector2 a, b;
            if (av && av->mNameText.notNull() && project(av->getPositionAgent(), a) && project(av->getPositionAgent() + across, b)
                && (a - b).length() >= tag_ppm)
            {
                tagged[i] = av;
            }
        }
        drawLabels(drawn, tagged, vw, vh);
        gGL.flush();
        gUIProgram.unbind();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGLLastMatrix = NULL;
    }
}

void SKTacticalOverview::drawLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn,
                                           const std::vector<LLVOAvatar*>& tagged, S32 vw, S32 vh)
{
    static LLCachedControl<F32> enter(gSavedSettings, "SKMapHoverEnterPx", skmap::HOVER_ENTER_PX);
    static LLCachedControl<F32> exit(gSavedSettings, "SKMapHoverExitPx", skmap::HOVER_EXIT_PX);
    static LLCachedControl<U32> max_width(gSavedSettings, "SKMapNametagMaxWidth", 190);
    LL_PROFILE_ZONE_NAMED("sk tactical labels");
    // Labels work in UI pixels, as on the mini-map, so they keep its sizes under any display scale.
    const LLVector2 scale(LLFontGL::sScaleX, LLFontGL::sScaleY);
    const F32 ui_w = vw / scale.mV[VX], ui_h = vh / scale.mV[VY];
    auto ui = [&](const LLVector2& px) { return LLVector2(px.mV[VX] / scale.mV[VX], px.mV[VY] / scale.mV[VY]); };
    auto down = [&](const LLVector2& at) { return LLVector2(at.mV[VX], ui_h - at.mV[VY]); };
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();

    const LLFontGL::UIOrigin origin = LLFontGL::sCurOrigin;
    const F32 depth = LLFontGL::sCurDepth, cur_sx = LLFontGL::sCurScaleX, cur_sy = LLFontGL::sCurScaleY;
    LLFontGL::sCurOrigin.set(0.f, 0.f);
    LLFontGL::sCurDepth = 0.f;
    LLFontGL::sCurScaleX = LLFontGL::sCurScaleY = 1.f;
    gGL.pushUIMatrix();
    gGL.loadUIIdentity();
    gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);

    {
        // Coded people carry the mini-map's badge, a fixed size over their marker's top whatever the zoom; nearest the
        // camera first, a badge mostly covering one already placed is left out, and the nearest draws on top. A flat
        // marker carries its code itself, so it takes no badge.
        const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
        std::vector<std::pair<F32, size_t>> coded;
        for (size_t i = 0; i < drawn.size(); ++i)
        {
            const size_t person = drawn[i].first;
            if (person >= people.size())
            {
                continue;
            }
            const auto blend = mFlatBlend.find(people[person].mId);
            const bool flat = blend != mFlatBlend.end() && blend->second >= 0.5f;
            if (!flat && !tagged[i] && people[person].mVisibility == skmap::Visibility::DOT_CODED &&
                !people[person].mCode.empty())
            {
                coded.emplace_back((gAgent.getPosAgentFromGlobal(people[person].mPos) - camera).lengthSquared(), i);
            }
        }
        std::sort(coded.begin(), coded.end());
        std::vector<std::pair<size_t, LLVector2>> badges;
        std::vector<std::pair<LLVector2, F32>> placed;
        for (const auto& [dist, i] : coded)
        {
            if (badges.size() >= MAX_BADGES)
            {
                break;
            }
            const SKMapPeople::Person& p = people[drawn[i].first];
            const F32 radius = skmapdraw::markerRadius(p, skmap::FloorRelation::SAME);
            const LLVector2 at = ui(drawn[i].second) + LLVector2(0.f, radius + BADGE_LIFT_PX);
            bool covered = false;
            for (const auto& [other, other_r] : placed)
            {
                covered = covered || (other - at).length() < llmax(radius, other_r);
            }
            if (!covered)
            {
                placed.emplace_back(at, radius);
                badges.emplace_back(drawn[i].first, at);
            }
        }
        for (auto it = badges.rbegin(); it != badges.rend(); ++it)
        {
            skmapdraw::personBadge(people[it->first], it->second);
        }
    }

    // Who is under the pointer, by the mini-map's hover rules.
    const LLVector2 pointer(static_cast<F32>(gViewerWindow->getCurrentMouseX()) - cap().viewport()[0] / scale.mV[VX],
                            static_cast<F32>(gViewerWindow->getCurrentMouseY()) - cap().viewport()[1] / scale.mV[VY]);
    mHover.notePointer(pointer);
    const bool esc = gKeyboard && gKeyboard->getKeyDown(KEY_ESCAPE);
    if (esc && !mEscDown && !mHover.empty())
    {
        mHover.suppress(pointer);
    }
    mEscDown = esc;
    std::vector<skmap::HoverDot> dots;
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (drawn[i].first < people.size() && !tagged[i])
        {
            dots.push_back({ people[drawn[i].first].mId, ui(drawn[i].second) });
        }
    }
    if (gFocusMgr.getMouseCapture() || mHover.suppressed())
    {
        mHover.clear();
    }
    else
    {
        mHover.update(dots, pointer, enter, exit);
    }
    if (mHover.empty())
    {
        mLabels.clear();
        mTagTexts.clear();
    }
    else
    {
        auto same_floor = [](const LLUUID&) { return skmap::FloorRelation::SAME; };
        mTagTexts = skmapdraw::tagTexts(mHover.members(), same_floor, skmapdraw::FloorLabels(),
                                                skmap::tagMaxWidth(ui_w, static_cast<F32>(max_width)));
        skmap::LabelLayout::Input in;
        in.mPointer = down(pointer);
        in.mWidth = ui_w;
        in.mHeight = ui_h;
        for (const skmapdraw::TagText& t : mTagTexts)
        {
            for (const skmap::HoverDot& d : dots)
            {
                if (d.mId == t.mId)
                {
                    in.mMembers.push_back({ t.mId, down(d.mPos), t.mWidth, t.mHeight });
                    break;
                }
            }
        }
        for (const skmap::HoverDot& d : dots)
        {
            const LLVector2 p = down(d.mPos);
            in.mObstacles.push_back({ p.mV[VX] - 5.0, p.mV[VY] - 5.0, 10.0, 10.0 });
        }
        mLabels.update(in, LLFrameTimer::getFrameDeltaTimeF32());
    }

    // Connectors, then the tags over them.
    gGL.getTextureSlot(0)->unbind();
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::CONNECTOR);
    for (const skmap::LabelLayout::Tag& tag : mLabels.tags())
    {
        for (const skmap::HoverDot& d : dots)
        {
            if (d.mId == tag.mId)
            {
                const LLVector2 end = skmap::LabelLayout::connectorEnd(tag, down(d.mPos));
                skmapdraw::line(d.mPos, down(end), 1.f);
                break;
            }
        }
    }
    gGL.end();
    for (const skmap::LabelLayout::Tag& tag : mLabels.tags())
    {
        for (const skmapdraw::TagText& t : mTagTexts)
        {
            if (t.mId == tag.mId)
            {
                const S32 left = ll_round(static_cast<F32>(tag.mX)), top = ll_round(ui_h - static_cast<F32>(tag.mY));
                skmapdraw::drawTag(LLRect(left, top, left + ll_round(static_cast<F32>(tag.mW)), top - ll_round(static_cast<F32>(tag.mH))), t);
                break;
            }
        }
    }

    // The in-world tags, nearest the camera last so it draws on top, just above each marker's projected top.
    std::vector<std::pair<F32, size_t>> order;
    const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (tagged[i])
        {
            order.emplace_back((tagged[i]->getPositionAgent() - eye).lengthSquared(), i);
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
    for (const auto& [dist, i] : order)
    {
        const LLVector2 at = ui(drawn[i].second);
        tagged[i]->mNameText->drawFlat(at.mV[VX], at.mV[VY] + 6.f);
    }

    gGL.popUIMatrix();
    LLFontGL::sCurOrigin = origin;
    LLFontGL::sCurDepth = depth;
    LLFontGL::sCurScaleX = cur_sx;
    LLFontGL::sCurScaleY = cur_sy;
}
