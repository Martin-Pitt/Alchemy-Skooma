/**
 * @file skfloatermap.h
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

#ifndef SK_FLOATERMAP_H
#define SK_FLOATERMAP_H

#include "llfloater.h"

class LLButton;
class SKNetMap;

class SKFloaterMap : public LLFloater
{
public:
    AL_VIEW_TYPE(SKFloaterMap, LLFloater);

    SKFloaterMap(const LLSD& key);
    ~SKFloaterMap() override;

    bool postBuild() override;
    void draw() override;
    void setMinimized(bool minimize) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    void onMouseEnter(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    /// The one map, which the Mouselook map also draws through.
    SKNetMap* map() const { return mMap; }

protected:
    /// Records which base buttons the floater shows, so the chrome can hide them at rest.
    void updateTitleButtons() override;

private:
    /// Whether the border, tint, grip and buttons show this frame.
    bool chromeWanted() const;
    /// The cog sits just left of the leftmost base button the floater shows.
    void layoutCog();

    SKNetMap* mMap = nullptr;
    LLButton* mCog = nullptr;
    bool mButtonShown[BUTTON_COUNT] = {};
    /// The pointer is over the floater, the map or any button in front of it.
    bool mHovered = false;
};

#endif // SK_FLOATERMAP_H
