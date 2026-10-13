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

#include "llbutton.h"
#include "lldraghandle.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llresizebar.h"
#include "llresizehandle.h"

#include "llagentcamera.h"
#include "llviewercontrol.h"
#include "skmapml.h"
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
    mCog = getChild<LLButton>("sk_map_cog");
    mCog->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("sk_map_settings"); });
    layoutCog();
    // Keep it on screen.
    gFloaterView->adjustToFitScreen(this, false);
    return true;
}

void SKFloaterMap::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // Here, not in postBuild: the saved rect, which sizes the map, is applied in between.
    seed();
}

void SKFloaterMap::seed()
{
    if (gSavedSettings.getBOOL("SKMapSeeded") || !mMap)
    {
        return;
    }
    static const char* CLASSIC[] = { "MiniMapScale", "MiniMapRotate", "MiniMapAutoCenter", "MiniMapShowPropertyLines",
                                     "AlchemyMiniMapForSaleParcels", "AlchemyMiniMapCollisionParcels", "AlchemyMinimapChatRings",
                                     "AlchemyMinimapRenderObjects" };
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

void SKFloaterMap::updateTitleButtons()
{
    LLFloater::updateTitleButtons();
    for (S32 i = 0; i < BUTTON_COUNT; ++i)
    {
        mButtonShown[i] = mButtons[i] && mButtons[i]->getVisible();
    }
    layoutCog();
}

void SKFloaterMap::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);
    layoutCog();
}

void SKFloaterMap::onMouseEnter(S32 x, S32 y, MASK mask)
{
    mHovered = true;
    LLFloater::onMouseEnter(x, y, mask);
}

void SKFloaterMap::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = false;
    LLFloater::onMouseLeave(x, y, mask);
}

void SKFloaterMap::layoutCog()
{
    if (!mCog)
    {
        return;
    }
    static LLUICachedControl<S32> close_box_from_top("UICloseBoxFromTop", 0);
    constexpr S32 COG_PX = 16;
    S32 right = getRect().getWidth() - LLPANEL_BORDER_WIDTH;
    S32 top = getRect().getHeight() - close_box_from_top;
    for (S32 i = 0; i < BUTTON_COUNT; ++i)
    {
        if (mButtons[i] && mButtonShown[i] && mButtons[i]->getRect().mLeft - 1 < right)
        {
            right = mButtons[i]->getRect().mLeft - 1;
            top = mButtons[i]->getRect().mTop;
        }
    }
    mCog->setRect(LLRect(right - COG_PX, top, right, top - COG_PX));
}

bool SKFloaterMap::chromeWanted() const
{
    if (!mMap)
    {
        return false;
    }
    if (mMap->isHovered() || mMap->namesShown() || isMinimized() || LLFloaterReg::instanceVisible("sk_map_settings"))
    {
        return true;
    }
    // Over the cog or a title button the map is not hovered, but the floater is, and a pressed button holds the mouse;
    // in mouselook the pointer is the crosshair.
    if (mHovered && !gAgentCamera.cameraMouselook())
    {
        const LLView* holder = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!gFocusMgr.getMouseCapture() || (holder && holder->hasAncestor(this)))
        {
            return true;
        }
    }
    // A resize in progress keeps it, as does focus.
    if (LLView* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
    {
        if ((dynamic_cast<LLResizeHandle*>(captor) || dynamic_cast<LLResizeBar*>(captor)) && captor->getParent() == this)
        {
            return true;
        }
    }
    return gFocusMgr.childHasKeyboardFocus(this);
}

void SKFloaterMap::draw()
{
    // In mouselook the map must not take the mouse from the world.
    const bool opaque = !gAgentCamera.cameraMouselook();
    setMouseOpaque(opaque);
    getDragHandle()->setMouseOpaque(opaque);
    // The Mouselook map shows this map in its place; the floater stays open, undrawn and deaf to the mouse.
    const bool replaced = SKMapML::showing();
    if (replaced == getEnabled())
    {
        setEnabled(!replaced);
        mHovered = false;
        if (mMap)
        {
            mMap->clearHover();
        }
    }
    if (replaced)
    {
        return;
    }

    // Borderless at rest: the base buttons show only with the chrome.
    const bool chrome = chromeWanted();
    for (S32 i = 0; i < BUTTON_COUNT; ++i)
    {
        if (mButtons[i])
        {
            mButtons[i]->setVisible(chrome && mButtonShown[i]);
        }
    }
    if (mMap)
    {
        mMap->setChrome(chrome);
    }
    if (mCog)
    {
        mCog->setVisible(chrome && !isMinimized());
    }
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
