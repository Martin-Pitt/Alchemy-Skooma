/**
 * @file sktoolmappan.h
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

#pragma once

#include "llsingleton.h"
#include "lltool.h"

class SKModalMode;

/// Stands in for the pie tool while a modal view covers the world (LLToolMgr::getCurrentTool), so build
/// tools, alt-cam and mouselook keep theirs. A plain press on the mode's ground captures the mouse, and a
/// move past a few pixels pans the mode's camera; a press that never moves is a click, which goes to the pie
/// on release when the mode passes clicks on and is swallowed otherwise.
class SKToolMapPan : public LLTool, public LLSingleton<SKToolMapPan>
{
    LLSINGLETON(SKToolMapPan);
    LOG_CLASS(SKToolMapPan);

public:
    /// The tool to use in place of `tool`: this one for the pie while a mode is on top, else `tool`.
    static LLTool* substitute(LLTool* tool);

    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    /// The mode on top when it passes clicks on to the world's tool, else null.
    static SKModalMode* passingMode();

    SKModalMode* mTarget = nullptr; ///< the mode the press grabbed, while the mouse is captured
    bool mDragging = false;
    S32 mDownX = 0;
    S32 mDownY = 0;
};
