/**
 * @file sktoolmappan.cpp
 * @brief The tool a modal map view uses in place of the pie: a plain left drag pans the map.
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

#include "sktoolmappan.h"

#include "llframetimer.h"
#include "lltoolpie.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "skmodalview.h"

namespace
{
    constexpr F32 PAN_THRESHOLD_PX = 4.f; // raw pixels a press may move and still be a click

    /// What the tool did with a press or drag, with SKOverviewPanDebug, once a second at most.
    void panTrace(const char* what)
    {
        static LLCachedControl<bool> debug(gSavedSettings, "SKOverviewPanDebug", false);
        static F64 last = -1.0;
        const F64 now = LLFrameTimer::getUptimeSeconds().value();
        if (debug && now - last >= 1.0)
        {
            last = now;
            LL_INFOS("SKOverviewPan") << what << LL_ENDL;
        }
    }
} // namespace

SKToolMapPan::SKToolMapPan() : LLTool(std::string("MapPan"))
{
}

// static
LLTool* SKToolMapPan::substitute(LLTool* tool)
{
    return tool == LLToolPie::getInstance() && SKModalView::worldCovered() ? getInstance() : tool;
}

// static
SKModalMode* SKToolMapPan::passingMode()
{
    SKModalMode* mode = SKModalView::top();
    return mode && mode->passesClicks() ? mode : nullptr;
}

bool SKToolMapPan::handleMouseDown(S32 x, S32 y, MASK mask)
{
    SKModalMode* mode = SKModalView::top();
    if (!mode)
    {
        return LLToolPie::getInstance()->handleMouseDown(x, y, mask);
    }
    // Modified presses keep the pie's meaning (alt-cam and grab never reach here: they are its override tools).
    if (mask != MASK_NONE || !mode->beginPan(x, y))
    {
        panTrace(mask != MASK_NONE ? "press passed on: modifier held" : "press passed on: no ground under the cursor");
        return passingMode() ? LLToolPie::getInstance()->handleMouseDown(x, y, mask) : true;
    }
    panTrace("press grabbed the ground");
    mTarget = mode;
    mDragging = false;
    mDownX = x;
    mDownY = y;
    setMouseCapture(true);
    return true;
}

bool SKToolMapPan::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mTarget)
    {
        if (SKModalView::top() != mTarget)
        {
            panTrace("drag dropped: the view closed or was covered");
            setMouseCapture(false);
            return true;
        }
        if (!mDragging)
        {
            const LLVector2 scale = gViewerWindow->getDisplayScale();
            const F32 dx = (F32)(x - mDownX) * scale.mV[VX];
            const F32 dy = (F32)(y - mDownY) * scale.mV[VY];
            mDragging = dx * dx + dy * dy >= PAN_THRESHOLD_PX * PAN_THRESHOLD_PX;
        }
        if (mDragging)
        {
            mTarget->pan(x, y);
            gViewerWindow->setCursor(UI_CURSOR_TOOLGRAB);
        }
        return true;
    }
    // Over a mode that passes clicks on, the pie still shows what a click would do; elsewhere the map can be grabbed.
    if (passingMode())
    {
        LLToolPie::getInstance()->handleHover(x, y, mask);
        if (gViewerWindow->getWindow()->getNextCursor() != UI_CURSOR_ARROW)
        {
            return true;
        }
    }
    SKModalMode* mode = SKModalView::top();
    gViewerWindow->setCursor(mode && !mode->canPan() ? UI_CURSOR_ARROW : UI_CURSOR_TOOLPAN);
    return true;
}

bool SKToolMapPan::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return passingMode() ? LLToolPie::getInstance()->handleMouseUp(x, y, mask) : true;
    }
    const bool click = !mDragging;
    SKModalMode* mode = mTarget;
    const S32 down_x = mDownX;
    const S32 down_y = mDownY;
    setMouseCapture(false);
    // A press that never moved is the click the pie would have had, from where it was pressed.
    if (click && mode == SKModalView::top() && mode->passesClicks())
    {
        panTrace("click passed on to the pie");
        LLToolPie* pie = LLToolPie::getInstance();
        if (pie->handleMouseDown(down_x, down_y, MASK_NONE))
        {
            pie->handleMouseUp(x, y, mask);
        }
    }
    return true;
}

bool SKToolMapPan::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    return passingMode() ? LLToolPie::getInstance()->handleDoubleClick(x, y, mask) : true;
}

bool SKToolMapPan::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return passingMode() && LLToolPie::getInstance()->handleRightMouseDown(x, y, mask);
}

bool SKToolMapPan::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    return passingMode() && LLToolPie::getInstance()->handleRightMouseUp(x, y, mask);
}

bool SKToolMapPan::handleToolTip(S32 x, S32 y, MASK mask)
{
    return passingMode() && LLToolPie::getInstance()->handleToolTip(x, y, mask);
}

void SKToolMapPan::onMouseCaptureLost()
{
    if (mTarget)
    {
        mTarget->endPan();
    }
    mTarget = nullptr;
    mDragging = false;
}
