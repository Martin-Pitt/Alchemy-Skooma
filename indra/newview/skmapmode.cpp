/**
 * @file skmapmode.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skmapmode.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llrootview.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <cmath>

using namespace skworldmap;

namespace
{
    constexpr F64 FOV_Y = 45.0 * 3.141592653589793 / 180.0;
    constexpr F64 START_DISTANCE = 1500.0;
    constexpr F64 START_ELEVATION = 50.0 * 3.141592653589793 / 180.0;
    constexpr F64 ROTATE_PER_PX = 0.006;
    constexpr F64 LOOK_PER_PX = 0.004;
    constexpr F64 ZOOM_PER_NOTCH = 1.15;
    constexpr F64 ORBIT_KEY_YAW = 1.2;          ///< radians per second
    constexpr F64 ORBIT_KEY_PAN = 0.8;          ///< distances per second

    bool movementKey(KEY key)
    {
        switch (key)
        {
            case 'W': case 'A': case 'S': case 'D': case 'Q': case 'E':
            case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
            case KEY_PAGE_UP: case KEY_PAGE_DOWN:
                return true;
            default:
                return false;
        }
    }

    /// Full-screen and behind every other view: takes the clicks, drags, wheel and keys that would
    /// otherwise reach the world, so WASD flies the map rather than walking the avatar.
    class SKWorldMapInputView : public LLUICtrl
    {
    public:
        explicit SKWorldMapInputView(const LLUICtrl::Params& p) : LLUICtrl(p) {}

        bool handleMouseDown(S32 x, S32 y, MASK mask) override { return beginDrag(x, y, (mask & (MASK_ALT | MASK_CONTROL)) != 0); }
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override { return beginDrag(x, y, true); }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override { return endDrag(); }
        bool handleRightMouseUp(S32 x, S32 y, MASK mask) override { return endDrag(); }

        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (hasMouseCapture())
            {
                SKMapMode::instance().drag(x - mLastX, y - mLastY, mRotate);
                mLastX = x;
                mLastY = y;
            }
            gViewerWindow->setCursor(hasMouseCapture() ? UI_CURSOR_TOOLPAN : UI_CURSOR_ARROW);
            return true;
        }

        bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override
        {
            SKMapMode::instance().zoom(delta.mPrecise);
            return true;
        }

        bool handleKeyHere(KEY key, MASK mask) override
        {
            if (mask & (MASK_CONTROL | MASK_ALT))
            {
                return false;
            }
            switch (key)
            {
                case KEY_ESCAPE:
                    SKMapMode::instance().close();
                    return true;
                case KEY_HOME:
                case 'H':
                    SKMapMode::instance().home();
                    return true;
                case 'C':
                    SKMapMode::instance().switchCamera();
                    return true;
                default:
                    // Held keys are read each frame; taking them here keeps them from the avatar.
                    return movementKey(key);
            }
        }

        bool handleUnicodeCharHere(llwchar uni_char) override
        {
            const llwchar upper = LLStringOps::toUpper(uni_char);
            return upper < 128 && (movementKey(static_cast<KEY>(upper)) || upper == 'H' || upper == 'C');
        }

        void onMouseCaptureLost() override { mRotate = false; }

    private:
        bool beginDrag(S32 x, S32 y, bool rotate)
        {
            setFocus(true);
            gFocusMgr.setMouseCapture(this);
            mLastX = x;
            mLastY = y;
            mRotate = rotate;
            return true;
        }

        bool endDrag()
        {
            if (hasMouseCapture())
            {
                gFocusMgr.setMouseCapture(nullptr);
            }
            return true;
        }

        S32 mLastX = 0;
        S32 mLastY = 0;
        bool mRotate = false;
    };
} // namespace

SKMapMode::SKMapMode() = default;

SKMapMode::~SKMapMode() = default;

// static
bool SKMapMode::isOpen()
{
    return instanceExists() && getInstance()->mOpen;
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
    if (LLStartUp::getStartupState() < STATE_STARTED || !gViewerWindow)
    {
        return false;
    }

    // The world camera as it is now; mouselook is left so the map has a cursor.
    mWorld.mMouselook = gAgentCamera.cameraMouselook();
    mWorld.mFocusOnAvatar = gAgentCamera.getFocusOnAvatar();
    mWorld.mPosition = gAgentCamera.getCameraPositionGlobal();
    mWorld.mFocus = gAgentCamera.getFocusGlobal();
    LLViewerObject* focus_object = gAgentCamera.getFocusObject();
    mWorld.mFocusObject = focus_object ? focus_object->getID() : LLUUID::null;
    if (mWorld.mMouselook)
    {
        gAgentCamera.changeCameraToDefault();
    }

    mOpenSignal();

    if (!mStarted)
    {
        startFromWorld();
    }

    LLUICtrl::Params p;
    p.name = "world_map_3d_input";
    p.rect = gViewerWindow->getWorldViewRectScaled();
    p.follows.flags = FOLLOWS_ALL;
    p.mouse_opaque = true;
    p.tab_stop = false;
    SKWorldMapInputView* view = new SKWorldMapInputView(p);
    LLRootView* root = gViewerWindow->getRootView();
    root->addChild(view);
    root->sendChildToBack(view);
    view->setFocus(true);
    mInput = view->getHandle();

    mOpen = true;
    mLastFrame = LLTimer::getUptimeSeconds();
    LL_INFOS("WorldMap") << "map mode opened" << LL_ENDL;
    return true;
}

void SKMapMode::close()
{
    if (!mOpen)
    {
        return;
    }
    mOpen = false;

    if (LLView* view = mInput.get())
    {
        if (gFocusMgr.getMouseCapture() == view)
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        if (view->hasFocus())
        {
            gFocusMgr.setKeyboardFocus(nullptr);
        }
        if (view->getParent())
        {
            view->getParent()->removeChild(view);
        }
        // Deleted once the event that closed the map, possibly the view's own, has unwound.
        LLHandle<LLView> handle = mInput;
        doOnIdleOneTime([handle]() { delete handle.get(); });
    }
    mInput = LLHandle<LLView>();
    mScene.release();

    // Map mode never moves the world camera; this puts back what the open changed, and a detached
    // camera exactly where it was. A camera following the avatar rebuilds itself from the avatar.
    if (mWorld.mMouselook)
    {
        gAgentCamera.changeCameraToMouselook(false);
    }
    else if (!mWorld.mFocusOnAvatar)
    {
        gAgentCamera.setCameraPosAndFocusGlobal(mWorld.mPosition, mWorld.mFocus, mWorld.mFocusObject);
        gAgentCamera.stopCameraAnimation();
    }
    LL_INFOS("WorldMap") << "map mode closed" << LL_ENDL;
}

void SKMapMode::startFromWorld()
{
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    mOrbit.mYaw = (std::fabs(at.mV[VX]) + std::fabs(at.mV[VY]) > 1.0e-3f) ? std::atan2(at.mV[VY], at.mV[VX]) : mOrbit.mYaw;
    mOrbit.mElevation = START_ELEVATION;
    mOrbit.mDistance = START_DISTANCE;
    mOrbit.mFocus = gAgent.getPositionGlobal();
    mOrbit.mFocus.mdV[VZ] = GROUND_Z;
    mOrbit.clamp();
    mFly = toFly(mOrbit);
    mCamera = CAMERA_ORBIT;
    mStarted = true;
}

void SKMapMode::home()
{
    if (mCamera == CAMERA_FLY)
    {
        mOrbit = toOrbit(mFly);
    }
    mOrbit.mFocus = gAgent.getPositionGlobal();
    mOrbit.mFocus.mdV[VZ] = GROUND_Z;
    if (mCamera == CAMERA_FLY)
    {
        mFly = toFly(mOrbit);
    }
}

void SKMapMode::switchCamera()
{
    if (mCamera == CAMERA_ORBIT)
    {
        mFly = toFly(mOrbit);
        mCamera = CAMERA_FLY;
    }
    else
    {
        mOrbit = toOrbit(mFly);
        mCamera = CAMERA_ORBIT;
    }
}

void SKMapMode::drag(S32 dx, S32 dy, bool rotate)
{
    if (mCamera == CAMERA_FLY)
    {
        mFly.mYaw -= dx * LOOK_PER_PX;
        mFly.mPitch += dy * LOOK_PER_PX;
        mFly.clamp();
        return;
    }
    if (rotate)
    {
        mOrbit.mYaw -= dx * ROTATE_PER_PX;
        mOrbit.mElevation += dy * ROTATE_PER_PX;
        mOrbit.clamp();
        return;
    }
    // Grab the ground: the point under the cursor follows it, foreshortened along the look.
    const F64 height_px = std::max(1.0, static_cast<F64>(gViewerWindow->getWorldViewRectScaled().getHeight()));
    const F64 metres_per_px = 2.0 * mOrbit.mDistance * std::tan(FOV_Y * 0.5) / height_px;
    const LLVector3d right(std::sin(mOrbit.mYaw), -std::cos(mOrbit.mYaw), 0.0);
    const LLVector3d ahead(std::cos(mOrbit.mYaw), std::sin(mOrbit.mYaw), 0.0);
    const F64 along = metres_per_px / std::max(std::sin(mOrbit.mElevation), 0.3);
    mOrbit.mFocus -= right * (dx * metres_per_px) + ahead * (dy * along);
}

void SKMapMode::zoom(F32 notches)
{
    if (mCamera == CAMERA_FLY)
    {
        const F64 step = flySpeed(mFly.mPosition.mdV[VZ], false) * 0.25 * -notches;
        mFly.mPosition += lookDirection(mFly.mYaw, mFly.mPitch) * step;
        mFly.clamp();
        return;
    }
    mOrbit.mDistance *= std::pow(ZOOM_PER_NOTCH, static_cast<F64>(notches));
    mOrbit.clamp();
}

void SKMapMode::moveForKeys(F64 dt)
{
    LLView* view = mInput.get();
    if (!view || !view->hasFocus() || !gKeyboard)
    {
        return;
    }
    auto held = [](KEY a, KEY b) { return gKeyboard->getKeyDown(a) || gKeyboard->getKeyDown(b); };
    const F64 forward = (held('W', KEY_UP) ? 1.0 : 0.0) - (held('S', KEY_DOWN) ? 1.0 : 0.0);
    const F64 strafe = (held('D', KEY_RIGHT) ? 1.0 : 0.0) - (held('A', KEY_LEFT) ? 1.0 : 0.0);
    const F64 rise = (held('E', KEY_PAGE_UP) ? 1.0 : 0.0) - (held('Q', KEY_PAGE_DOWN) ? 1.0 : 0.0);
    if (forward == 0.0 && strafe == 0.0 && rise == 0.0)
    {
        return;
    }
    const bool boost = (gKeyboard->currentMask(false) & MASK_SHIFT) != 0;

    if (mCamera == CAMERA_FLY)
    {
        LLVector3d f, r, u;
        viewBasis(mFly.mYaw, mFly.mPitch, f, r, u);
        const F64 speed = flySpeed(mFly.mPosition.mdV[VZ], boost) * dt;
        mFly.mPosition += f * (forward * speed) + r * (strafe * speed) + LLVector3d(0.0, 0.0, rise * speed);
        mFly.clamp();
        return;
    }

    // The orbit: WASD slides the focus, Q and E turn about it.
    const F64 pan = mOrbit.mDistance * ORBIT_KEY_PAN * dt * (boost ? 4.0 : 1.0);
    const LLVector3d right(std::sin(mOrbit.mYaw), -std::cos(mOrbit.mYaw), 0.0);
    const LLVector3d ahead(std::cos(mOrbit.mYaw), std::sin(mOrbit.mYaw), 0.0);
    mOrbit.mFocus += ahead * (forward * pan) + right * (strafe * pan);
    mOrbit.mYaw += rise * ORBIT_KEY_YAW * dt;
    mOrbit.clamp();
}

SKWorldMapFrame SKMapMode::frame() const
{
    SKWorldMapFrame f;
    f.mFovY = FOV_Y;
    if (mCamera == CAMERA_FLY)
    {
        f.mEye = mFly.mPosition;
        f.mYaw = mFly.mYaw;
        f.mPitch = mFly.mPitch;
    }
    else
    {
        f.mEye = mOrbit.eye();
        f.mYaw = mOrbit.mYaw;
        f.mPitch = mOrbit.pitch();
        f.mFocus = mOrbit.mFocus;
        f.mHasFocus = true;
    }
    return f;
}

void SKMapMode::render(const LLRect& world_rect_raw)
{
    if (!mOpen)
    {
        return;
    }
    const F64 now = LLTimer::getUptimeSeconds();
    moveForKeys(llclamp(now - mLastFrame, 0.0, 0.1));
    mLastFrame = now;
    mScene.render(frame(), world_rect_raw, now);
}
