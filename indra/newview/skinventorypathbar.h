/**
 * @file skinventorypathbar.h
 * @brief Inventory Explorer's path bar: a crumb per folder down to the open one, each with a menu of its folders
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

#ifndef SK_SKINVENTORYPATHBAR_H
#define SK_SKINVENTORYPATHBAR_H

#include "lluictrl.h"
#include "lluiimage.h"
#include "skinventoryexplorerdrag.h"

#include <functional>

class SKExplorerPopupList;

// Crumbs from the inventory's root to the open folder on one line, shrinking with an ellipsis as room runs out;
// an arrow after a folder with subfolders opens a menu of them. Crumbs take drops and drag their folder.
class SKInventoryPathBar : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKInventoryPathBar, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    typedef std::function<void(const LLUUID& folder_id)> navigate_callback_t;

    ~SKInventoryPathBar();

    void setFolder(const LLUUID& folder_id);
    // Names or folders changed: the crumbs are read again.
    void refresh();
    void hideMenu();
    void setNavigateCallback(const navigate_callback_t& cb) { mNavigateCallback = cb; }
    // "arrow_tooltip" ("Folders in [FOLDER]") and "no_subfolders".
    void setStrings(const std::string& arrow_tooltip, const std::string& no_subfolders);

    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

protected:
    SKInventoryPathBar(const Params& p);
    friend class LLUICtrlFactory;

    struct Crumb
    {
        LLUUID mID;
        std::string mName;
        LLPointer<LLUIImage> mIcon;     // the first crumb's alone
        bool mArrow = false;            // has subfolders, or may have once loaded
        S32 mNatural = 0;
        LLRect mRect;
        LLRect mArrowRect;
    };

    void readCrumbs();
    void layout();
    // The crumb under the point, and whether the point is on its arrow.
    S32 crumbAt(S32 x, S32 y, bool* on_arrow) const;
    void toggleMenu(S32 index);

    LLUUID mFolderID;
    std::vector<Crumb> mCrumbs;
    S32 mHover;
    bool mHoverArrow;
    std::string mArrowToolTip;
    std::string mNoSubfolders;
    navigate_callback_t mNavigateCallback;

    SKExplorerPopupList* mMenu;
    LLHandle<LLView> mMenuHandle;
    S32 mMenuCrumb;             // the crumb whose menu is open, -1 for none
    U32 mMenuClosedFrame;       // a click on the arrow that closed it does not open it again
    S32 mMenuClosedCrumb;

    S32 mPressCrumb;
    S32 mPressX;
    S32 mPressY;
};

#endif // SK_SKINVENTORYPATHBAR_H
