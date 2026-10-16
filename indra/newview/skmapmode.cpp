/**
 * @file skmapmode.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skmapmode.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llpanel.h"
#include "llrootview.h"
#include "lluictrlfactory.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#include <cmath>

using namespace skworldmap;

namespace
{
    constexpr F64 START_DISTANCE = 1500.0;
    constexpr F64 START_ELEVATION = 50.0 * 3.141592653589793 / 180.0;

    /// Behind every other view, over the world view: a plain left-drag pans the map, and plain clicks
    /// and right-clicks go nowhere, so nothing reaches the hidden world. Alt and Ctrl clicks fall through
    /// to the camera tool and the wheel to the camera's zoom, exactly as in-world.
    class SKWorldMapInputView : public LLView
    {
    public:
        explicit SKWorldMapInputView(const LLView::Params& p) : LLView(p) {}

        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (mask & (MASK_ALT | MASK_CONTROL))
            {
                return false;
            }
            S32 sx, sy;
            localPointToScreen(x, y, &sx, &sy);
            if (SKMapMode::instance().beginPan(sx, sy))
            {
                gFocusMgr.setMouseCapture(this);
            }
            return true;
        }

        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (hasMouseCapture())
            {
                S32 sx, sy;
                localPointToScreen(x, y, &sx, &sy);
                SKMapMode::instance().pan(sx, sy);
                gViewerWindow->setCursor(UI_CURSOR_TOOLPAN);
                return true;
            }
            if (mask & (MASK_ALT | MASK_CONTROL))
            {
                return false;
            }
            gViewerWindow->setCursor(UI_CURSOR_ARROW);
            return true;
        }

        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (hasMouseCapture())
            {
                gFocusMgr.setMouseCapture(nullptr);
                return true;
            }
            return !(mask & (MASK_ALT | MASK_CONTROL));
        }

        bool handleDoubleClick(S32 x, S32 y, MASK mask) override { return !(mask & (MASK_ALT | MASK_CONTROL)); }
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override { return true; }
        bool handleRightMouseUp(S32 x, S32 y, MASK mask) override { return true; }
        void onMouseCaptureLost() override { SKMapMode::instance().endPan(); }
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

F64 SKMapMode::flycamScale(const LLVector3d& eye) const
{
    return flycamHeightScale(eye.mdV[VZ]);
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

    mOpenSignal();
    mOpen = true;

    if (!mHasMapCamera)
    {
        // Above the agent, looking the way the world camera did.
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        const F64 yaw = (std::fabs(at.mV[VX]) + std::fabs(at.mV[VY]) > 1.0e-3f) ? std::atan2(at.mV[VY], at.mV[VX]) : 0.0;
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
    root->sendChildToBack(input);
    if (!on_top)
    {
        input->setVisible(false);
        if (overlay)
        {
            overlay->setVisible(false);
        }
    }

    LL_INFOS("WorldMap") << "map mode opened" << LL_ENDL;
    return true;
}

void SKMapMode::removeViews()
{
    deleteLater(mInput);
    deleteLater(mOverlay);
    mInput = LLHandle<LLView>();
    mOverlay = LLHandle<LLView>();
    mPanning = false;
}

void SKMapMode::close()
{
    if (!mOpen)
    {
        return;
    }
    mMapPosition = gAgentCamera.getCameraPositionGlobal();
    mMapFocus = gAgentCamera.getFocusGlobal();
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
    for (const LLHandle<LLView>& handle : { mInput, mOverlay })
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
    for (LLHandle<LLView>* handle : { &mInput, &mOverlay })
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
    const LLVector3d agent = gAgent.getPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const LLVector3d offset(agent.mdV[VX] - focus.mdV[VX], agent.mdV[VY] - focus.mdV[VY], 0.0);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(gAgentCamera.getCameraPositionGlobal() + offset, focus + offset, LLUUID::null);
}

bool SKMapMode::groundUnderCursor(S32 x, S32 y, LLVector3d& ground) const
{
    const LLVector3d eye = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    return groundHit(eye, LLVector3d(gViewerWindow->mouseDirectionGlobal(x, y)), ground);
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
    if (!mPanning || !panOffset(mPanView, clampDip(cursorRay(mPanBasis, x, y)), mPanGrabbed, offset))
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
    if (!LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        f.mFocus = gAgentCamera.getFocusGlobal();
        f.mHasFocus = true;
    }
    return f;
}

void SKMapMode::render(const LLRect& world_rect_raw)
{
    if (mOpen)
    {
        mScene.render(frame(), world_rect_raw, LLTimer::getUptimeSeconds());
    }
}
