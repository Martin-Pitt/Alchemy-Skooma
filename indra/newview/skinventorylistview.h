/**
 * @file skinventorylistview.h
 * @brief Inventory Explorer's List view: one widget drawing a folder's rows from a flat model
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

#ifndef SK_SKINVENTORYLISTVIEW_H
#define SK_SKINVENTORYLISTVIEW_H

#include "skinventoryflatview.h"

#include <set>

class SKInventoryListView : public SKInventoryFlatView
{
public:
    AL_VIEW_TYPE(SKInventoryListView, SKInventoryFlatView);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    typedef std::function<void(S32 x, S32 y)> point_callback_t;

    void setSortCallback(const void_callback_t& cb) { mSortCallback = cb; }
    // A right-click on the header: the "Columns" menu.
    void setHeaderMenuCallback(const point_callback_t& cb) { mHeaderMenuCallback = cb; }

    void setSort(EColumn key, bool ascending) override;

    // Hidden columns by key; Name always shows, Folder only while searching.
    void setHiddenColumns(const std::set<std::string>& keys);
    void setSearching(bool searching);
    bool isColumnHidden(EColumn col) const { return mHidden[col]; }

    bool handleToolTip(S32 x, S32 y, MASK mask) override;

protected:
    SKInventoryListView(const Params& p);
    friend class LLUICtrlFactory;

    void layoutRows() override;
    void drawContent() override;
    S32 rowAt(S32 x, S32 y, bool* on_name) const override;
    void scrollToRow(S32 row) override;
    S32 rowFromKey(KEY key, MASK mask) const override;
    LLRect nameRect(S32 row, bool& multiline) const override;
    bool handleHeaderMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHeaderRightMouseDown(S32 x, S32 y, MASK mask) override;

    void updateColumns();
    S32 rowsPerPage() const;
    S32 columnAt(S32 x) const;
    std::string cellText(const SKInventoryRow& row, EColumn col) const;

    S32 mColumnWidths[COL_COUNT];
    bool mHidden[COL_COUNT];
    bool mSearching;
    std::vector<EColumn> mShown;    // the columns drawn, in order
    S32 mHeaderClicks;              // clicks on the sorted header since it was chosen, 0 for the default sort

    void_callback_t mSortCallback;
    point_callback_t mHeaderMenuCallback;
};

#endif // SK_SKINVENTORYLISTVIEW_H
