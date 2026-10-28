/**
 * @file skinventoryexplorerpopup.h
 * @brief Inventory Explorer's drop-down list: the path bar's folder menus and the search options
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

#ifndef SK_SKINVENTORYEXPLORERPOPUP_H
#define SK_SKINVENTORYEXPLORERPOPUP_H

#include "lluictrl.h"
#include "lluiimage.h"

#include <functional>

// One line of the list: an option, a dim heading, or a divider.
struct SKPopupRow
{
    enum EKind
    {
        ROW_OPTION,
        ROW_TITLE,
        ROW_DIVIDER,
    };

    EKind mKind = ROW_OPTION;
    LLPointer<LLUIImage> mIcon;
    std::string mLabel;
    std::string mHint;      // dim, after the label
    std::string mExample;   // dim, at the right
    std::string mToolTip;
    std::string mRemoveToolTip; // set: an x at the right removes the row
    bool mBold = false;
    bool mEnabled = true;
    LLSD mValue;

    bool isOption() const { return mKind == ROW_OPTION && mEnabled; }
};

// A list drawn over everything through the viewer's popup layer; a click elsewhere closes it.
// It belongs to no view: its owner places it in screen coordinates and deletes it.
class SKExplorerPopupList : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKExplorerPopupList, LLUICtrl);

    typedef std::function<void(S32 index)> index_callback_t;
    typedef std::function<void()> void_callback_t;

    static SKExplorerPopupList* create();

    void setRows(const std::vector<SKPopupRow>& rows);
    const std::vector<SKPopupRow>& getRows() const { return mRows; }
    // Opens below a rect of the spawning view, its left edge held inside the window; max_height 0 is unlimited.
    void showBelow(LLView* spawn, const LLRect& spawn_rect, S32 width, S32 max_height, S32 dx, S32 dy);
    void hide();
    bool isOpen() const { return getVisible(); }

    // -1 for none; moves wrap and skip headings, dividers and disabled rows.
    S32 getHighlight() const { return mHighlight; }
    void setHighlight(S32 index);
    void moveHighlight(S32 step);
    // The first option, -1 for none.
    S32 firstOption() const;

    void setPickCallback(const index_callback_t& cb) { mPickCallback = cb; }
    void setRemoveCallback(const index_callback_t& cb) { mRemoveCallback = cb; }
    void setCloseCallback(const void_callback_t& cb) { mCloseCallback = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onTopLost() override;

protected:
    SKExplorerPopupList(const LLUICtrl::Params& p);
    friend class LLUICtrlFactory;

    static S32 rowHeight(const SKPopupRow& row);
    S32 contentHeight() const;
    S32 rowAt(S32 y) const;
    LLRect rowRect(S32 index) const;
    LLRect removeRect(S32 index) const;
    void scrollToRow(S32 index);
    void clampScroll();

    std::vector<SKPopupRow> mRows;
    S32 mHighlight;
    S32 mHover;
    S32 mScroll;    // pixels hidden above the top
    index_callback_t mPickCallback;
    index_callback_t mRemoveCallback;
    void_callback_t mCloseCallback;
};

#endif // SK_SKINVENTORYEXPLORERPOPUP_H
