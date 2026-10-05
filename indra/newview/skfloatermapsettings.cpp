/**
 * @file skfloatermapsettings.cpp
 * @brief The mini-map's settings: presets and every SKMap switch, bound to its setting.
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

#include "skfloatermapsettings.h"

#include "llbutton.h"
#include "llfloaterreg.h"

#include "llviewercontrol.h"

SKFloaterMapSettings::SKFloaterMapSettings(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterMapSettings::postBuild()
{
    getChild<LLButton>("sk_preset_combat")->setCommitCallback([this](LLUICtrl*, const LLSD&) { applyPreset(skmap::Preset::COMBAT); });
    getChild<LLButton>("sk_preset_explore")->setCommitCallback([this](LLUICtrl*, const LLSD&) { applyPreset(skmap::Preset::EXPLORE); });
    getChild<LLButton>("sk_preset_minimal")->setCommitCallback([this](LLUICtrl*, const LLSD&) { applyPreset(skmap::Preset::MINIMAL); });
    mHadSavedRect = !mRectControl.empty() && getControlGroup()->getRect(mRectControl).notEmpty();
    return true;
}

void SKFloaterMapSettings::onOpen(const LLSD& key)
{
    if (!mHadSavedRect && !mPlaced)
    {
        placeBesideMap();
    }
    mPlaced = true;
}

void SKFloaterMapSettings::applyPreset(skmap::Preset preset)
{
    // The bound controls follow their settings, so writing the settings is enough.
    for (const auto& [name, value] : skmap::presetValues(preset))
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            control->setValue(value);
        }
    }
}

void SKFloaterMapSettings::placeBesideMap()
{
    LLFloater* map = LLFloaterReg::findInstance("mini_map");
    if (!map || !map->getVisible() || !gFloaterView)
    {
        return;
    }
    constexpr S32 GAP = 4;
    const LLRect beside = map->getRect();
    const LLRect room = gFloaterView->getSnapRect();
    const S32 width = getRect().getWidth();
    const S32 left = beside.mLeft - GAP - width >= room.mLeft ? beside.mLeft - GAP - width : beside.mRight + GAP;
    setOrigin(left, beside.mTop - getRect().getHeight());
    gFloaterView->adjustToFitScreen(this, false);
}
