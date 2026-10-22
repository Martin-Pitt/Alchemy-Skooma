/**
 * @file skfloatertacticaloptions.cpp
 * @brief The tactical overview's options: its camera mode and view settings, bound to their settings.
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

#include "skfloatertacticaloptions.h"

#include "llviewercontrol.h"

SKFloaterTacticalOptions::SKFloaterTacticalOptions(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterTacticalOptions::postBuild()
{
    getChild<LLUICtrl>("sk_tactical_camera_mode")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshMode(); });
    refreshMode();
    return true;
}

void SKFloaterTacticalOptions::refreshMode()
{
    const bool battlefield = gSavedSettings.getU32("SKTacticalCameraMode") == 0;
    getChildView("sk_tactical_neighbours")->setEnabled(battlefield);
    // One tilt slider, for whichever camera is chosen.
    getChild<LLUICtrl>("sk_tactical_pitch")->setControlName(battlefield ? "SKTacticalBattlefieldPitch" : "SKTacticalPitch");
}
