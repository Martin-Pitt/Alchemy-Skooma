/**
 * @file skvizhover.h
 * @brief Links legends to the hover context: a hovered legend row publishes its key, and a published key lights legends.
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

#ifndef SK_VIZHOVER_H
#define SK_VIZHOVER_H

#include "sklegendmodel.h"

class SKLegendView;

namespace SKVizHover
{
    // The hover kind a legend, chart or world mark publishes its SKVizKey under.
    inline constexpr const char* KIND = "viz.key";

    // Publishes the legend's hovered key, and lights it whenever another publisher's key names one of its scales.
    void attach(SKLegendView* legend);

    // The key published now, empty when the hover is not a viz.key; overlays read it once a frame.
    SKVizKey current();

    // A world consumer's answer to its own pick: lights every attached legend over that scale.
    void highlight(const SKVizKey& key);

    // The in-viewport slots of panel_sk_viz_hud.xml, bottom-left; a slot shows while it holds a legend.
    inline constexpr S32 HUD_SLOTS = 4;
    void showInHud(S32 slot, const SKLegend& legend);
    void hideInHud(S32 slot);
}

#endif // SK_VIZHOVER_H
