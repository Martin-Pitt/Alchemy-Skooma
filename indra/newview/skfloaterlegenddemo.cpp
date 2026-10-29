/**
 * @file skfloaterlegenddemo.cpp
 * @brief Develop floater showing every built-in colour scale and the sample legends.
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

#include "skfloaterlegenddemo.h"

#include "llbutton.h"
#include "llsliderctrl.h"
#include "sklegendsamples.h"
#include "sklegendview.h"
#include "skvizhover.h"

SKFloaterLegendDemo::SKFloaterLegendDemo(const LLSD& key) : LLFloater(key) {}

bool SKFloaterLegendDemo::postBuild()
{
    mComplexity = SKLegendSamples::avatarComplexityScale();
    const std::pair<const char*, SKLegend> legends[] = {
        { "legend_avatar_draw_info", SKLegendSamples::avatarDrawInfo(mComplexity) },
        { "legend_update_type", SKLegendSamples::updateType() },
        { "legend_lod_info", SKLegendSamples::lodInfo() },
        { "legend_buffer_view", SKLegendSamples::bufferView() },
        { "legend_name_tag_distance", SKLegendSamples::nameTagDistance() },
        { "legend_beacons", SKLegendSamples::beacons() },
        { "legend_scales", SKLegendSamples::builtInScales() },
    };
    for (const auto& [name, legend] : legends)
    {
        if (SKLegendView* v = findChild<SKLegendView>(name))
        {
            v->setLegend(legend);
            SKVizHover::attach(v);
        }
    }

    // A runtime domain change, as a setting moving would make.
    if (LLSliderCtrl* limit = findChild<LLSliderCtrl>("complexity_limit"))
    {
        limit->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
            mComplexity->setDomain(0.0, ctrl->getValue().asReal());
        });
    }
    if (LLButton* hud = findChild<LLButton>("toggle_hud"))
    {
        hud->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            mInHud = !mInHud;
            if (mInHud)
            {
                SKVizHover::showInHud(0, SKLegendSamples::nameTagDistance());
                SKVizHover::showInHud(1, SKLegendSamples::updateType());
            }
            else
            {
                SKVizHover::hideInHud(0);
                SKVizHover::hideInHud(1);
            }
        });
    }
    return true;
}

void SKFloaterLegendDemo::onClose(bool app_quitting)
{
    if (mInHud && !app_quitting)
    {
        SKVizHover::hideInHud(0);
        SKVizHover::hideInHud(1);
        mInHud = false;
    }
}
