/**
 * @file skinventorygalleryview.h
 * @brief Inventory Explorer's Gallery: a stage for the selected item beside a strip of the folder's contents
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#ifndef SK_SKINVENTORYGALLERYVIEW_H
#define SK_SKINVENTORYGALLERYVIEW_H

#include "skinventorydetails.h"

#include <map>

class SKInventoryIconsView;

// The stage takes the room left of the strip, a child Icons view of 88 px tiles that holds the selection.
class SKInventoryGalleryView : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKInventoryGalleryView, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    static constexpr S32 STRIP_WIDTH = 112;
    static constexpr S32 STRIP_TILE = 88;

    bool postBuild() override;
    SKInventoryIconsView* getStrip() const { return mStrip; }

    // The selected item, or the folder's first item while nothing is selected.
    void setItem(const LLUUID& item_id);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onVisibilityChange(bool new_visibility) override;

protected:
    SKInventoryGalleryView(const Params& p);
    friend class LLUICtrlFactory;

    LLRect stageRect() const;
    bool overStage(S32 x, S32 y) const;

    SKInventoryIconsView* mStrip;
    LLUUID mItemID;
    LLFrameTimer mSettle;
    bool mFetching;
    bool mHovered;
    SKInventoryPlayer mPlayer;
    LLRect mAspectRect;
    // The strip's scroll per folder, kept for the session.
    std::map<LLUUID, S32> mStripScrolls;
    LLUUID mStripFolder;
};

#endif // SK_SKINVENTORYGALLERYVIEW_H
