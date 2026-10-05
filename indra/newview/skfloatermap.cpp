/**
 * @file skfloatermap.cpp
 * @brief The mini-map floater: a borderless shell round SKNetMap that shows its chrome on hover.
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

#include "skfloatermap.h"

#include "lldraghandle.h"

#include "llagentcamera.h"
#include "llviewercontrol.h"
#include "sknetmap.h"
#include "skmappolicy.h"

SKFloaterMap::SKFloaterMap(const LLSD& key)
    : LLFloater(key)
{
}

SKFloaterMap::~SKFloaterMap()
{
}

bool SKFloaterMap::postBuild()
{
    mMap = getChild<SKNetMap>("sk_net_map");
    sendChildToBack(mMap);
    sendChildToBack(getDragHandle());
    seed();
    // Keep it on screen.
    gFloaterView->adjustToFitScreen(this, false);
    return true;
}

void SKFloaterMap::seed()
{
    if (gSavedSettings.getBOOL("SKMapSeeded") || !mMap)
    {
        return;
    }
    static const char* CLASSIC[] = { "MiniMapScale", "MiniMapRotate", "MiniMapAutoCenter", "MiniMapShowPropertyLines",
                                     "AlchemyMiniMapForSaleParcels", "AlchemyMiniMapCollisionParcels", "AlchemyMinimapChatRings",
                                     "AlchemyMinimapGuideLine", "AlchemyMinimapRenderObjects" };
    LLSD classic = LLSD::emptyMap();
    for (const char* name : CLASSIC)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            classic[name] = control->getValue();
        }
    }
    const LLSD seeded = skmap::seedFromClassic(classic, mMap->shortSide());
    for (LLSD::map_const_iterator it = seeded.beginMap(); it != seeded.endMap(); ++it)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(it->first))
        {
            control->setValue(it->second);
        }
    }
    gSavedSettings.setBOOL("SKMapSeeded", true);
    LL_INFOS("SKMap") << "seeded the mini-map from the classic map's settings" << LL_ENDL;
}

void SKFloaterMap::draw()
{
    // In mouselook the map must not take the mouse from the world.
    const bool opaque = !gAgentCamera.cameraMouselook();
    setMouseOpaque(opaque);
    getDragHandle()->setMouseOpaque(opaque);
    LLFloater::draw();
}

void SKFloaterMap::setMinimized(bool minimize)
{
    LLFloater::setMinimized(minimize);
    setTitle(minimize ? getString("mini_map_caption") : "");
}

bool SKFloaterMap::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (isMinimized())
    {
        setMinimized(false);
        return true;
    }
    return LLFloater::handleDoubleClick(x, y, mask);
}
