/**
 * @file skmapmode.cpp
 * @brief Map mode: a 3D map of the grid shown in place of the world, under the viewer's own camera.
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

#include "skmapmode.h"
#include "skworldmapsearch.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llpanel.h"
#include "llrootview.h"
#include "lluictrlfactory.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#include <cmath>

using namespace skworldmap;

namespace
{
    constexpr F64 START_DISTANCE = 3000.0;
    constexpr F64 START_ELEVATION = 50.0 * 3.141592653589793 / 180.0;
    constexpr F64 MIN_MAP_DISTANCE = 20.0;      ///< the closest the up and down actions bring the camera to its focus
    constexpr F64 MAX_MAP_DISTANCE = 200000.0;  ///< and the farthest, the grid's breadth

    /// Behind every other view, over the world view: right-clicks go nowhere, so nothing reaches the
    /// hidden world. The left button falls through to SKToolMapPan, the wheel to the camera's zoom.
    class SKWorldMapInputView : public LLView
    {
    public:
        explicit SKWorldMapInputView(const LLView::Params& p) : LLView(p) {}

        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override { return true; }
        bool handleRightMouseUp(S32 x, S32 y, MASK mask) override { return true; }

        /// The wheel zooms toward the map's ground under the cursor; declined, it reaches alt-cam's own zoom.
        bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override
        {
            return SKMapMode::instance().zoomToCursor(delta.mPrecise) || LLView::handleScrollWheel(x, y, delta);
        }
    };

    void deleteLater(const LLHandle<LLView>& handle)
    {
        if (LLView* view = handle.get())
        {
            if (gFocusMgr.getMouseCapture() == view)
            {
                gFocusMgr.setMouseCapture(nullptr);
            }
            if (view->getParent())
            {
                view->getParent()->removeChild(view);
            }
            // Deleted once the event that closed the map, possibly one of these views' own, has unwound.
            doOnIdleOneTime([handle]() { delete handle.get(); });
        }
    }
} // namespace

SKMapMode::SKMapMode()
{
    SKModalView::instance().registerMode(this);
}

SKMapMode::~SKMapMode()
{
    if (SKModalView::instanceExists())
    {
        SKModalView::getInstance()->unregisterMode(this);
    }
}

// static
bool SKMapMode::isOpen()
{
    return instanceExists() && getInstance()->mOpen;
}

bool SKMapMode::groundBelow(const LLVector3d& eye, LLVector3d& ground) const
{
    ground.setVec(eye.mdV[VX], eye.mdV[VY], GROUND_Z);
    ground.mdV[VZ] = mScene.groundAt(ground, GROUND_Z);
    return true;
}

F64 SKMapMode::flycamMaxScale() const
{
    static LLCachedControl<F32> max_scale(gSavedSettings, "SKWorldMapFlycamMaxScale", 60.f);
    return llmax((F64)max_scale, 1.0);
}

void SKMapMode::move(const skmodalview::Move& move)
{
    if (!mOpen || mPanning || SKModalView::top() != this)
    {
        return;
    }
    static LLCachedControl<F32> speed(gSavedSettings, "SKWorldMapMoveSpeed", 0.5f);
    const LLViewerCamera* view = LLViewerCamera::getInstance();
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const F64 distance = llmax((eye - focus).magVec(), 1.0);
    // A share of the ground in view a second, so the map moves the same on screen at any height.
    const F64 ground_width = 2.0 * distance * std::tan(view->getView() * view->getAspect() * 0.5);
    const F64 seconds = llmin((F64)gFrameDTClamped, 0.1);
    const F64 share = llmax((F64)speed, 0.0) * (move.mFast ? 3.0 : 1.0) * seconds;

    LLVector3d step;
    LLVector3 dir;
    if (skmodalview::groundDirection(view->getAtAxis(), view->getUpAxis(), move.mForward, move.mLeft, dir))
    {
        step = LLVector3d(dir) * (ground_width * share);
    }
    LLVector3d new_eye = eye + step;
    if (move.mUp != 0.f)
    {
        // Up backs away from the focus, down closes in; never through it.
        const F64 new_distance = llclamp(distance * std::exp(move.mUp * share * 2.0), MIN_MAP_DISTANCE, llmax(MAX_MAP_DISTANCE, distance));
        new_eye = focus + step + (eye - focus) * (new_distance / distance);
    }
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(new_eye, focus + step, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
}

bool SKMapMode::rlvRestricted() const
{
    return RlvActions::isRlvEnabled() && gRlvHandler.hasBehaviour(RLV_BHVR_SHOWWORLDMAP);
}

void SKMapMode::toggle()
{
    if (mOpen)
    {
        close();
    }
    else
    {
        open();
    }
}

bool SKMapMode::open()
{
    if (mOpen)
    {
        return true;
    }
    // The modal view snapshots the world camera, leaves mouselook and stops the autopilot.
    if (rlvRestricted() || !SKModalView::instance().claim(this, SKModalView::PRIORITY_MAP))
    {
        return false;
    }

    // The open signal captures the last world frame, which exists only when no other mode covered it.
    if (SKModalView::top() == this)
    {
        mOpenSignal();
    }
    mOpen = true;

    // The first open of the session turns up and away from the world camera onto the map's start view
    // (above the agent, looking north, the way maps are read); later opens go straight back to where
    // the map was left.
    const bool first_open = !mHasMapCamera;
    if (first_open)
    {
        const F64 yaw = 0.5 * 3.141592653589793;
        mMapFocus = gAgent.getPositionGlobal();
        mMapFocus.mdV[VZ] = GROUND_Z;
        mMapPosition = mMapFocus - lookDirection(yaw, -START_ELEVATION) * START_DISTANCE;
        mHasMapCamera = true;
    }
    // Under a mode already on top the map waits, camera and input alike, for onTopChanged.
    const bool on_top = SKModalView::top() == this;
    if (on_top)
    {
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(mMapPosition, mMapFocus, LLUUID::null);
        if (first_open)
        {
            // The whole orbit up from the world camera, not a warp, so the move onto the map is seen.
            gAgentCamera.orbitCameraAnimation();
        }
        else
        {
            gAgentCamera.stopCameraAnimation();
            gAgentCamera.skipCameraSmoothing();
        }
    }

    LLRootView* root = gViewerWindow->getRootView();
    LLView::Params p;
    p.name = "world_map_3d_input";
    p.rect = gViewerWindow->getWorldViewRectScaled();
    p.follows.flags = FOLLOWS_ALL;
    // Not opaque: an opaque view swallows what it declines, and alt-cam and the wheel must reach the tools.
    p.mouse_opaque = false;
    SKWorldMapInputView* input = new SKWorldMapInputView(p);
    root->addChild(input);
    mInput = input->getHandle();

    LLPanel* overlay = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>("panel_sk_world_map.xml", nullptr, LLPanel::child_registry_t::instance());
    if (overlay)
    {
        const LLRect world = gViewerWindow->getWorldViewRectScaled();
        const LLRect rect = overlay->getRect();
        overlay->setOrigin(world.mRight - rect.getWidth(), world.mTop - rect.getHeight());
        overlay->getChild<LLUICtrl>("recentre_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SKMapMode::instance().recentre(); });
        root->addChild(overlay);
        root->sendChildToBack(overlay);
        mOverlay = overlay->getHandle();
    }
    SKWorldMapSearch* search = new SKWorldMapSearch();
    if (search->buildFromFile("panel_sk_world_map_search.xml"))
    {
        const LLRect world = gViewerWindow->getWorldViewRectScaled();
        const LLRect rect = search->getRect();
        search->setOrigin(world.getCenterX() - rect.getWidth() / 2, world.mBottom + 18);
        root->addChild(search);
        root->sendChildToBack(search);
        mSearch = search->getHandle();
    }
    else
    {
        delete search;
    }
    root->sendChildToBack(input);
    if (!on_top)
    {
        for (const LLHandle<LLView>& handle : { mInput, mOverlay, mSearch })
        {
            if (LLView* view = handle.get())
            {
                view->setVisible(false);
            }
        }
    }

    LL_INFOS("WorldMap") << "map mode opened" << LL_ENDL;
    return true;
}

void SKMapMode::removeViews()
{
    deleteLater(mInput);
    deleteLater(mOverlay);
    deleteLater(mSearch);
    mInput = LLHandle<LLView>();
    mOverlay = LLHandle<LLView>();
    mSearch = LLHandle<LLView>();
    mPanning = false;
}

void SKMapMode::close()
{
    if (!mOpen)
    {
        return;
    }
    if (SKModalView::top() == this) // under another mode the map kept its camera in onTopChanged
    {
        mMapPosition = gAgentCamera.getCameraPositionGlobal();
        mMapFocus = gAgentCamera.getFocusGlobal();
    }
    removeViews();
    mScene.release();
    mOpen = false;
    // The world camera goes back as it was when the last modal view closes.
    SKModalView::instance().release(this);
    LL_INFOS("WorldMap") << "map mode closed" << LL_ENDL;
}

void SKMapMode::onTopChanged(bool on_top)
{
    if (!mOpen)
    {
        return;
    }
    if (!on_top)
    {
        mMapPosition = gAgentCamera.getCameraPositionGlobal();
        mMapFocus = gAgentCamera.getFocusGlobal();
    }
    for (const LLHandle<LLView>& handle : { mInput, mOverlay, mSearch })
    {
        if (LLView* view = handle.get())
        {
            if (!on_top && gFocusMgr.getMouseCapture() == view)
            {
                gFocusMgr.setMouseCapture(nullptr);
            }
            view->setVisible(on_top);
        }
    }
    mPanning = false;
    if (on_top)
    {
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(mMapPosition, mMapFocus, LLUUID::null);
    }
}

void SKMapMode::shutdown()
{
    // No idle loop runs from here on, so the views go now rather than later.
    for (LLHandle<LLView>* handle : { &mInput, &mOverlay, &mSearch })
    {
        if (LLView* view = handle->get())
        {
            if (gFocusMgr.getMouseCapture() == view)
            {
                gFocusMgr.setMouseCapture(nullptr);
            }
            if (view->getParent())
            {
                view->getParent()->removeChild(view);
            }
            delete view;
        }
        *handle = LLHandle<LLView>();
    }
    mPanning = false;
    mOpen = false;
    mScene.shutdown();
}

void SKMapMode::recentre()
{
    flyTo(gAgent.getPositionGlobal());
}

void SKMapMode::flyTo(const LLVector3d& target)
{
    if (!mOpen)
    {
        return;
    }
    // The flycam drives the camera itself, so it is turned off before the map can move it.
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        LLViewerJoystick::getInstance()->toggleFlycam();
        LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true); // the flycam's view no longer holds the camera
    }
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const LLVector3d offset(target.mdV[VX] - focus.mdV[VX], target.mdV[VY] - focus.mdV[VY], 0.0);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(gAgentCamera.getCameraPositionGlobal() + offset, focus + offset, LLUUID::null);
}

bool SKMapMode::zoomToCursor(F32 clicks)
{
    static LLCachedControl<bool> zoom_to_cursor(gSavedSettings, "SKWorldMapZoomToCursor", true);
    if (!zoom_to_cursor || !mOpen || mPanning || clicks == 0.f || SKModalView::top() != this || SKModalView::flycamActive())
    {
        return false;
    }
    LLVector3d cursor;
    if (!groundUnderCursor(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), cursor))
    {
        return false;
    }
    // Camera and focus scale about the ground under the cursor, so that point stays under it: in by a
    // fourth root of two a click, as alt-cam's own zoom steps, out by the same.
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const F64 distance = (eye - focus).magVec();
    if (distance <= 0.0)
    {
        return false;
    }
    F64 scale = std::pow(std::sqrt(F_SQRT2), static_cast<F64>(clicks));
    scale = llclamp(scale, MIN_MAP_DISTANCE / distance, llmax(MAX_MAP_DISTANCE, distance) / distance);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(cursor + (eye - cursor) * scale, cursor + (focus - cursor) * scale, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
    return true;
}

bool SKMapMode::pickGround(S32 x, S32 y, LLVector3d& ground) const
{
    // Alt-click focus. Logged, as it is rare, so a click that seems ignored shows whether it got here.
    const bool hit = groundUnderCursor(x, y, ground);
    mTraceUntil = LLTimer::getUptimeSeconds() + 1.0;
    LL_INFOS("WorldMap") << "map focus pick at " << x << "," << y << ": "
                         << (hit ? llformat("ground %.1f, %.1f, %.1f", ground.mdV[VX], ground.mdV[VY], ground.mdV[VZ]) : std::string("no ground"))
                         << LL_ENDL;
    return hit;
}

bool SKMapMode::groundUnderCursor(S32 x, S32 y, LLVector3d& ground) const
{
    // The map's surface where the cursor's ray first reaches it: terrain where the map has heights, the
    // ground plane elsewhere. Near the horizon the ray is tilted down a little, as the pan's is, so it lands.
    const LLVector3d eye = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const LLVector3d dir = clampDip(LLVector3d(gViewerWindow->mouseDirectionGlobal(x, y)));
    LLVector3d plane;
    if (!groundHit(eye, dir, plane))
    {
        return false;
    }
    // March toward the plane hit in steps a fraction of the way, then bisect the step that went under.
    // The surface is never below the water, so the ray meets it no later than the plane at the water.
    constexpr S32 STEPS = 256;
    constexpr S32 BISECTIONS = 24;
    const F64 reach = (plane - eye).magVec() * 1.05;
    const auto above = [&](F64 t) {
        const LLVector3d p = eye + dir * t;
        return p.mdV[VZ] > mScene.groundAt(p, GROUND_Z);
    };
    F64 t0 = 0.0;
    for (S32 k = 1; k <= STEPS; ++k)
    {
        const F64 t1 = reach * k / STEPS;
        if (!above(t1))
        {
            F64 lo = t0, hi = t1;
            for (S32 b = 0; b < BISECTIONS; ++b)
            {
                const F64 mid = 0.5 * (lo + hi);
                if (above(mid))
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }
            ground = eye + dir * hi;
            ground.mdV[VZ] = mScene.groundAt(ground, GROUND_Z);
            return true;
        }
        t0 = t1;
    }
    ground = plane;
    return true;
}

bool SKMapMode::beginPan(S32 x, S32 y)
{
    if (!mOpen || !groundUnderCursor(x, y, mPanGrabbed))
    {
        return false;
    }
    // The camera as drawn now: its eye, its axes and its screen. The pan never reads the live camera
    // again, so a camera still settling cannot shake it.
    const LLViewerCamera* view = LLViewerCamera::getInstance();
    mPanView = gAgent.getPosGlobalFromAgent(view->getOrigin());
    mPanEye = gAgentCamera.getCameraPositionGlobal();
    mPanFocus = gAgentCamera.getFocusGlobal();
    mPanBasis.mAt.setVec(view->getAtAxis());
    mPanBasis.mLeft.setVec(view->getLeftAxis());
    mPanBasis.mUp.setVec(view->getUpAxis());
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    mPanBasis.mFocal = (world.getHeight() * 0.5) / std::tan(view->getView() * 0.5);
    mPanBasis.mCenterX = world.getCenterX();
    mPanBasis.mCenterY = world.getCenterY();
    mPanning = true;
    return true;
}

void SKMapMode::pan(S32 x, S32 y)
{
    // Solved whole from the drag's start each time, never step by step, so it cannot drift or feed back.
    LLVector3d offset;
    // The grabbed point slides on the level through it, so a grab on a hill stays under the cursor.
    if (!mPanning || !panOffset(mPanView, clampDip(cursorRay(mPanBasis, x, y)), mPanGrabbed, offset, mPanGrabbed.mdV[VZ]))
    {
        return;
    }
    // Eye and focus move as one, by the same offset from where the drag began, and without smoothing:
    // the camera lands exactly where the solve put it.
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(mPanView + offset, mPanView + (mPanFocus - mPanEye) + offset, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
}

SKWorldMapFrame SKMapMode::frame() const
{
    // Whatever drives the viewer camera, the map draws from it.
    const LLViewerCamera* view = LLViewerCamera::getInstance();
    const LLVector3 at = view->getAtAxis();
    SKWorldMapFrame f;
    f.mEye = gAgent.getPosGlobalFromAgent(view->getOrigin());
    f.mYaw = std::atan2(at.mV[VY], at.mV[VX]);
    f.mPitch = std::asin(llclamp(at.mV[VZ], -1.f, 1.f));
    f.mFovY = view->getView();
    f.mHasFocus = SKModalView::viewFocus(f.mFocus);
    return f;
}

void SKMapMode::limitTilt()
{
    // The camera may tilt toward the horizon only so far: past the soft limit it is drawn back to it,
    // the more strongly the further past, and it never passes the hard limit. Angles are the view's
    // elevation below the horizon, so a smaller one is a shallower look.
    static LLCachedControl<F32> soft_deg(gSavedSettings, "SKWorldMapTiltSoftDegrees", 35.f);
    static LLCachedControl<F32> hard_deg(gSavedSettings, "SKWorldMapTiltHardDegrees", 22.f);
    static LLCachedControl<F32> return_rate(gSavedSettings, "SKWorldMapTiltReturnRate", 4.f);
    if (mPanning || SKModalView::top() != this || gAgentCamera.getCameraAnimating() || LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        return;
    }
    const F64 soft = llclamp((F64)soft_deg, 1.0, 89.0) * DEG_TO_RAD;
    const F64 hard = llclamp((F64)hard_deg, 0.0, soft * RAD_TO_DEG) * DEG_TO_RAD;
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const LLVector3d to_focus = focus - eye;
    const F64 distance = to_focus.magVec();
    if (distance < 1.0e-3)
    {
        return;
    }
    const F64 elevation = std::asin(llclamp(-to_focus.mdV[VZ] / distance, -1.0, 1.0));
    if (elevation >= soft)
    {
        mTiltAdjusting = false;
        return;
    }
    if (!mTiltAdjusting)
    {
        mTiltAdjusting = true;
        LL_INFOS("WorldMap") << "map tilt limit engaged at " << elevation * RAD_TO_DEG << " degrees" << LL_ENDL;
    }
    F64 target = elevation;
    if (elevation < hard)
    {
        target = hard;
    }
    else
    {
        const F64 seconds = llmin((F64)gFrameDTClamped, 0.1);
        target = elevation + (soft - elevation) * (1.0 - std::exp(-llmax((F64)return_rate, 0.0) * seconds));
    }
    const F64 yaw = std::atan2(to_focus.mdV[VY], to_focus.mdV[VX]);
    const LLVector3d new_eye = focus - lookDirection(yaw, -target) * distance;
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(new_eye, focus, LLUUID::null);
    gAgentCamera.skipCameraSmoothing();
}

void SKMapMode::traceCamera()
{
    // For a second after a focus pick, four lines of where the camera and its focus went, so a pick that
    // seems ignored shows what took the focus back.
    const F64 now = LLTimer::getUptimeSeconds();
    if (now > mTraceUntil || now < mTraceNext)
    {
        return;
    }
    mTraceNext = now + 0.25;
    const LLVector3d target = gAgentCamera.getFocusTargetGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    LL_INFOS("WorldMap") << "map camera: target " << llformat("%.1f, %.1f, %.1f", target.mdV[VX], target.mdV[VY], target.mdV[VZ])
                         << " focus " << llformat("%.1f, %.1f, %.1f", focus.mdV[VX], focus.mdV[VY], focus.mdV[VZ])
                         << " eye " << llformat("%.1f, %.1f, %.1f", eye.mdV[VX], eye.mdV[VY], eye.mdV[VZ])
                         << " animating " << gAgentCamera.getCameraAnimating() << " on avatar " << gAgentCamera.getFocusOnAvatar()
                         << " mode " << gAgentCamera.getCameraMode() << " top " << (SKModalView::top() == this)
                         << " flycam " << LLViewerJoystick::getInstance()->getOverrideCamera() << LL_ENDL;
}

void SKMapMode::render(const LLRect& world_rect_raw)
{
    if (mOpen)
    {
        traceCamera();
        limitTilt();
        mScene.render(frame(), world_rect_raw, LLTimer::getUptimeSeconds());
    }
}
