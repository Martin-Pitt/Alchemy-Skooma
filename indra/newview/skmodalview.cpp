/**
 * @file skmodalview.cpp
 * @brief A modal view: something shown in place of the world, under the viewer's own camera.
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

#include "skmodalview.h"

#include "llagent.h"
#include "llapp.h"
#include "llagentcamera.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewerjoystick.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#include <algorithm>
#include <unordered_set>

using namespace skmodalview;

SKModalMode::~SKModalMode()
{
    if (SKModalView::instanceExists())
    {
        SKModalView::getInstance()->forget(this);
    }
}

SKModalView::SKModalView() = default;

SKModalView::~SKModalView() = default;

// static
bool SKModalView::worldCovered()
{
    return instanceExists() && !getInstance()->mClaims.empty();
}

// static
SKModalMode* SKModalView::top()
{
    if (!instanceExists())
    {
        return nullptr;
    }
    return static_cast<SKModalMode*>(const_cast<void*>(getInstance()->mClaims.top()));
}

// static
bool SKModalView::rlvCameraLocked()
{
    if (!RlvActions::isRlvEnabled())
    {
        return false;
    }
    return gRlvHandler.hasBehaviour(RLV_BHVR_SETCAM_UNLOCK) || gRlvHandler.hasBehaviour(RLV_BHVR_SETCAM)
        || gRlvHandler.hasBehaviour(RLV_BHVR_SETCAM_FOCUS) || RlvActions::isCameraDistanceClamped()
        || RlvActions::isCameraPresetLocked();
}

// static
bool SKModalView::canClaim()
{
    return LLStartUp::getStartupState() >= STATE_STARTED && gViewerWindow && gAgentCamera.isInitialized() && !rlvCameraLocked();
}

void SKModalView::registerMode(SKModalMode* mode)
{
    if (mode && std::find(mModes.begin(), mModes.end(), mode) == mModes.end())
    {
        mModes.push_back(mode);
    }
}

void SKModalView::unregisterMode(SKModalMode* mode)
{
    mModes.erase(std::remove(mModes.begin(), mModes.end(), mode), mModes.end());
    release(mode);
}

void SKModalView::forget(SKModalMode* mode)
{
    mModes.erase(std::remove(mModes.begin(), mModes.end(), mode), mModes.end());
    SKModalMode* before = top();
    if (!mClaims.release(mode))
    {
        return;
    }
    notifyTopChange(before, mode);
    updateCoveredFloaters();
    if (mClaims.empty() && !LLApp::isExiting())
    {
        restoreWorldCamera();
    }
}

void SKModalView::updateCoveredFloaters()
{
    const SKModalMode* mode = top();
    const std::vector<std::string> covered = mode ? mode->coveredFloaters() : std::vector<std::string>();
    // setVisible rather than show and hide: no open or close sound, and the floater keeps its place.
    for (auto it = mHiddenFloaters.begin(); it != mHiddenFloaters.end();)
    {
        if (std::find(covered.begin(), covered.end(), *it) != covered.end())
        {
            ++it;
            continue;
        }
        if (LLFloater* floater = LLFloaterReg::findInstance(*it))
        {
            floater->setVisible(true);
        }
        it = mHiddenFloaters.erase(it);
    }
    for (const std::string& name : covered)
    {
        LLFloater* floater = LLFloaterReg::findInstance(name);
        if (!floater || !floater->getVisible() || std::find(mHiddenFloaters.begin(), mHiddenFloaters.end(), name) != mHiddenFloaters.end())
        {
            continue;
        }
        floater->setVisible(false);
        mHiddenFloaters.push_back(name);
        // Saved as still open, so a session that ends under the mode opens it again next time.
        LLFloater::getControlGroup()->setBOOL(LLFloaterReg::getVisibilityControlName(name), true);
    }
}

void SKModalView::notifyTopChange(SKModalMode* before, const SKModalMode* except)
{
    SKModalMode* after = top();
    if (mClosingAll || !before || !after || before == after)
    {
        return;
    }
    if (before != except && mClaims.holds(before))
    {
        before->onTopChanged(false);
    }
    if (after != except)
    {
        after->onTopChanged(true);
    }
}

bool SKModalView::claim(SKModalMode* mode, S32 priority)
{
    if (!mode)
    {
        return false;
    }
    if (!canClaim() || mode->rlvRestricted())
    {
        return false;
    }
    registerMode(mode);
    SKModalMode* before = top();
    if (mClaims.holds(mode))
    {
        mClaims.claim(mode, priority);
        notifyTopChange(before, nullptr);
        updateCoveredFloaters();
        return true;
    }
    if (mClaims.empty())
    {
        snapshotWorldCamera();
        mMove = Move(); // nothing held under an earlier cover carries into this one
    }
    mClaims.claim(mode, priority);
    notifyTopChange(before, mode);
    updateCoveredFloaters();
    LL_INFOS("ModalView") << mode->getModalName() << " covers the world" << LL_ENDL;
    return true;
}

void SKModalView::release(SKModalMode* mode)
{
    SKModalMode* before = top();
    if (!mClaims.release(mode))
    {
        return;
    }
    notifyTopChange(before, mode);
    updateCoveredFloaters();
    LL_INFOS("ModalView") << mode->getModalName() << " released the world" << LL_ENDL;
    if (mClaims.empty() && !LLApp::isExiting()) // a mode torn down on exit leaves the camera alone
    {
        restoreWorldCamera();
    }
}

void SKModalView::snapshotWorldCamera()
{
    // The world camera as it is now; mouselook is left so the mode has a cursor.
    mWorld.mMouselook = gAgentCamera.cameraMouselook();
    mWorld.mFocusOnAvatar = gAgentCamera.getFocusOnAvatar();
    // A camera that follows the avatar is kept relative to it, so it follows it again on release and the
    // sim's camera moves with the agent meanwhile.
    const LLVector3d agent = gAgent.getPositionGlobal();
    mWorld.mPosition = storePoint(gAgentCamera.getCameraPositionGlobal(), agent, mWorld.mFocusOnAvatar);
    mWorld.mFocus = storePoint(gAgentCamera.getFocusGlobal(), agent, mWorld.mFocusOnAvatar);
    LLViewerObject* focus_object = gAgentCamera.getFocusObject();
    mWorld.mFocusObject = focus_object ? focus_object->getID() : LLUUID::null;
    const LLViewerCamera* view = LLViewerCamera::getInstance();
    mWorld.mViewOffset = storePoint(gAgent.getPosGlobalFromAgent(view->getOrigin()), agent, mWorld.mFocusOnAvatar);
    mWorld.mAt = view->getAtAxis();
    mWorld.mLeft = view->getLeftAxis();
    mWorld.mUp = view->getUpAxis();
    if (mWorld.mMouselook)
    {
        gAgentCamera.changeCameraToDefault();
    }
    gAgent.stopAutoPilot();
}

void SKModalView::restoreWorldCamera()
{
    // The world camera exactly as it was; a camera that followed the avatar follows it again from the
    // same offset, wherever the avatar is now, without turning it.
    const LLVector3d agent = gAgent.getPositionGlobal();
    gAgentCamera.setCameraPosAndFocusGlobal(restorePoint(mWorld.mPosition, agent, mWorld.mFocusOnAvatar),
                                            restorePoint(mWorld.mFocus, agent, mWorld.mFocusOnAvatar), mWorld.mFocusObject);
    if (mWorld.mFocusOnAvatar)
    {
        gAgentCamera.setFocusOnAvatar(true, false, false);
    }
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
    // The exposure adapted to a scene not drawn since; the world's first frame meters afresh.
    gPipeline.resetExposureHistory();
    if (mWorld.mMouselook)
    {
        gAgentCamera.changeCameraToMouselook(false);
    }
}

void SKModalView::closeTop()
{
    if (SKModalMode* mode = top())
    {
        mode->closeModal();
        release(mode); // a mode that did not release its claim on close loses it here
    }
}

void SKModalView::closeAll()
{
    mClosingAll = true;
    for (const void* key : mClaims.ordered())
    {
        if (!mClaims.holds(key))
        {
            continue; // an earlier close took it with it
        }
        SKModalMode* mode = static_cast<SKModalMode*>(const_cast<void*>(key));
        mode->closeModal();
        release(mode);
    }
    mClosingAll = false;
}

bool SKModalView::closeIfNeeded(bool leaving_world)
{
    if (leaving_world)
    {
        closeAll();
        return false;
    }
    const bool camera_locked = rlvCameraLocked();
    for (const void* key : mClaims.ordered())
    {
        if (!mClaims.holds(key))
        {
            continue; // an earlier close took it with it
        }
        SKModalMode* mode = static_cast<SKModalMode*>(const_cast<void*>(key));
        if (camera_locked || mode->rlvRestricted())
        {
            mode->closeModal();
            release(mode);
        }
    }
    // Held actions repeat every frame, so what was gathered since the last one is this frame's move.
    const Move move = mMove;
    mMove = Move();
    SKModalMode* mode = top();
    if (mode && move.any() && !flycamActive())
    {
        mode->move(move);
    }
    return !mClaims.empty();
}

void SKModalView::render(const LLRect& world_rect_raw)
{
    if (SKModalMode* mode = top())
    {
        mode->renderModal(world_rect_raw);
    }
}

void SKModalView::shutdown()
{
    // Copied: a mode may unregister itself while it shuts down.
    const std::vector<SKModalMode*> modes = mModes;
    for (SKModalMode* mode : modes)
    {
        mode->shutdownModal();
    }
    mClaims = Claims();
    updateCoveredFloaters();
}

// static
bool SKModalView::interceptsAction(const std::string& function_name, bool released)
{
    static const std::unordered_set<std::string> avatar_actions = {
        "jump", "push_down", "push_forward", "push_backward", "look_up", "look_down", "toggle_fly",
        "turn_left", "turn_right", "slide_left", "slide_right", "stop_moving", "run_forward",
        "run_backward", "run_left", "run_right", "toggle_run", "toggle_down", "toggle_sit",
        "teleport_to", "walk_to",
    };
    if (!worldCovered() || released || !avatar_actions.count(function_name))
    {
        return false;
    }
    // The action, not its key: whatever the bindings put on it moves the mode as it would the avatar.
    SKModalView& self = instance();
    if (addMoveAction(function_name, self.mMove) && gKeyboard && (gKeyboard->currentMask(false) & MASK_SHIFT))
    {
        self.mMove.mFast = true;
    }
    return true;
}

// static
bool SKModalView::flycamActive()
{
    return LLViewerJoystick::getInstance()->getOverrideCamera();
}

// static
bool SKModalView::viewFocus(LLVector3d& focus)
{
    if (!flycamActive())
    {
        focus = gAgentCamera.getFocusGlobal();
        return true;
    }
    const SKModalMode* mode = top();
    const LLVector3d eye = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    return mode && mode->groundBelow(eye, focus);
}

// static
bool SKModalView::canPan()
{
    const SKModalMode* mode = top();
    return mode && !flycamActive() && mode->canPan();
}

// static
bool SKModalView::pickGround(S32 x, S32 y, LLVector3d& ground)
{
    const SKModalMode* mode = top();
    return mode && mode->pickGround(x, y, ground);
}

// static
F64 SKModalView::flycamScale(const LLVector3d& eye)
{
    const SKModalMode* mode = top();
    LLVector3d ground;
    if (!mode || !mode->groundBelow(eye, ground))
    {
        return 1.0;
    }
    return flycamSpeedScale(eye.mdV[VZ] - ground.mdV[VZ], mode->flycamMaxScale());
}

// static
bool SKModalView::worldCamera(LLVector3d& position_global, LLVector3& at, LLVector3& left, LLVector3& up)
{
    if (!worldCovered())
    {
        return false;
    }
    const WorldCamera& world = getInstance()->mWorld;
    position_global = restorePoint(world.mViewOffset, gAgent.getPositionGlobal(), world.mFocusOnAvatar);
    at = world.mAt;
    left = world.mLeft;
    up = world.mUp;
    return true;
}

// static
void SKModalView::worldCameraForUpdate(LLVector3& position_agent, LLVector3& at, LLVector3& left, LLVector3& up)
{
    LLVector3d position;
    if (worldCamera(position, at, left, up))
    {
        position_agent = gAgent.getPosAgentFromGlobal(position);
    }
}
