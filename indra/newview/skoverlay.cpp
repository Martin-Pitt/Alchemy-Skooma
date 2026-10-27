/**
 * @file skoverlay.cpp
 * @brief Registered drawers for in-world (3D), world-anchored (2.5D) and screen (2D) overlays.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skoverlay.h"

#include "llviewercamera.h"

#include <vector>

namespace
{
    struct Drawer
    {
        SKOverlays::id_t      id;
        SKOverlayLayer        layer;
        std::string           name;
        SKOverlays::draw_fn_t draw;
    };

    std::vector<Drawer> sDrawers;
    SKOverlays::id_t    sNextId = 1;
    bool                sDrawing = false;
}

SKOverlays::id_t SKOverlays::add(SKOverlayLayer layer, const std::string& name, draw_fn_t draw)
{
    if (!draw)
    {
        return 0;
    }
    id_t id = sNextId++;
    sDrawers.push_back({ id, layer, name, std::move(draw) });
    return id;
}

void SKOverlays::remove(id_t id)
{
    if (!id)
    {
        return;
    }
    for (Drawer& d : sDrawers)
    {
        if (d.id == id)
        {
            // Emptying rather than erasing keeps a drawer that removes itself (or a
            // sibling) mid-draw from invalidating the loop below.
            d.draw = nullptr;
            break;
        }
    }
    if (!sDrawing)
    {
        sDrawers.erase(std::remove_if(sDrawers.begin(), sDrawers.end(),
                                      [](const Drawer& d) { return !d.draw; }),
                       sDrawers.end());
    }
}

bool SKOverlays::any(SKOverlayLayer layer)
{
    for (const Drawer& d : sDrawers)
    {
        if (d.layer == layer && d.draw)
        {
            return true;
        }
    }
    return false;
}

void SKOverlays::draw(SKOverlayLayer layer)
{
    if (!any(layer))
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    sDrawing = true;
    for (size_t i = 0; i < sDrawers.size(); ++i)
    {
        if (sDrawers[i].layer == layer && sDrawers[i].draw)
        {
            sDrawers[i].draw();
        }
    }
    sDrawing = false;
    sDrawers.erase(std::remove_if(sDrawers.begin(), sDrawers.end(),
                                  [](const Drawer& d) { return !d.draw; }),
                   sDrawers.end());
}

bool SKOverlays::projectToScreen(const LLVector3& pos_agent, S32& x, S32& y)
{
    LLCoordGL point;
    if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(pos_agent, point, false))
    {
        return false;
    }
    x = point.mX;
    y = point.mY;
    return true;
}
