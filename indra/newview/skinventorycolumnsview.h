/**
 * @file skinventorycolumnsview.h
 * @brief Inventory Explorer's Columns view: the path below the top-level folder as a strip of columns
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

#ifndef SK_SKINVENTORYCOLUMNSVIEW_H
#define SK_SKINVENTORYCOLUMNSVIEW_H

#include "lleditmenuhandler.h"
#include "lluictrl.h"
#include "skinventorylistview.h"

#include <functional>
#include <map>
#include <unordered_set>

class LLScrollbar;

// One widget draws the whole strip, each column only over its visible rows.
class SKInventoryColumnsView : public LLUICtrl, public LLEditMenuHandler
{
public:
    AL_VIEW_TYPE(SKInventoryColumnsView, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    typedef std::function<void(const LLUUID& folder_id)> id_callback_t;
    typedef std::function<void()> void_callback_t;
    typedef std::function<void(S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id)> menu_callback_t;
    typedef std::map<LLUUID, S32> scroll_map_t;

    ~SKInventoryColumnsView();

    void setLabels(const SKInventoryListLabels& labels) { mLabels = labels; }
    // A click between columns: the folder becomes current without a history entry.
    void setMoveCallback(const id_callback_t& cb) { mMoveCallback = cb; }
    // Double-click, Enter or →: folders navigate, items open.
    void setOpenCallback(const id_callback_t& cb) { mOpenCallback = cb; }
    void setUpCallback(const void_callback_t& cb) { mUpCallback = cb; }
    void setSidebarCallback(const void_callback_t& cb) { mSidebarCallback = cb; }
    void setSelectionCallback(const void_callback_t& cb) { mSelectionCallback = cb; }
    void setMenuCallback(const menu_callback_t& cb) { mMenuCallback = cb; }

    // Lays the strip out to the folder, keeping the strip's and the columns' scrolls.
    void setFolder(const LLUUID& folder_id, const uuid_vec_t& selection = uuid_vec_t());
    const LLUUID& getFolder() const { return mFolderID; }

    // A tab's scrolls: the strip's and each column's first row, per folder.
    void setScrollState(S32 strip_x, const scroll_map_t& scrolls);
    S32 getStripScroll() const { return mStripX; }
    const scroll_map_t& getColumnScrolls() const { return mScrolls; }

    void setSort(SKInventoryListView::EColumn key, bool ascending);
    void setDetailsShown(bool shown);

    // Reads every column again on the next draw, keeping selection and scrolls.
    void markDirty() { mDirty = true; }
    bool lists(const LLUUID& id) const;
    // The deepest folder on the path that still exists, for a path whose folder went.
    LLUUID nearestLiveFolder() const;

    uuid_vec_t getSelection() const;
    void setSelection(const uuid_vec_t& ids);
    // Selects the current column's first row when nothing is selected.
    void selectFirstRow();

    U32 getGeneration() const { return mGeneration; }
    S32 getSelectedCount() const { return mSelectedCount; }
    S32 getFolderCount() const { return mFolderCount; }
    S32 getItemCount() const { return mItemCount; }
    bool isLoading() const { return mLoading; }

    // LLView
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleScrollHWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onFocusReceived() override;
    void onFocusLost() override;

    // LLEditMenuHandler
    LLView* asView() override { return this; }
    void doDelete() override;
    bool canDoDelete() const override;
    void selectAll() override;
    bool canSelectAll() const override;
    void deselect() override;
    bool canDeselect() const override { return mSelectedCount > 0; }

protected:
    SKInventoryColumnsView(const Params& p);
    friend class LLUICtrlFactory;

    enum EKind
    {
        KIND_PATH,
        KIND_GHOST,
        KIND_DETAILS,
        KIND_SPARE,
    };

    struct Column
    {
        EKind mKind = KIND_PATH;
        LLUUID mFolderID;
        LLUUID mPathID;     // the row the path goes on through, drawn grey
        std::vector<SKInventoryRow> mRows;
        S32 mMore = 0;      // rows a ghost leaves out
        bool mLoading = false;
        S32 mLeft = 0;
        S32 mWidth = 0;
    };

    void rebuild();
    void rebuildTrailing();
    void layoutColumns();
    void readColumn(Column& column, bool folders_too);
    void revealColumns(bool trailing);
    void updateStripScrollbar();
    void notifySelection(bool reveal);

    Column* currentColumn();
    const Column* currentColumn() const;
    S32 columnAt(S32 x) const;
    S32 rowAt(const Column& column, S32 y) const;
    S32 rowsPerPage() const;
    S32 columnScroll(const Column& column) const;
    void setColumnScroll(const Column& column, S32 first);
    void scrollRowIntoView(const Column& column, S32 row);

    void moveCursor(S32 row, MASK mask);
    void selectRange(S32 from, S32 to);
    void clearSelection();
    bool selectRow(S32 column, S32 row, MASK mask);
    void openRow(const SKInventoryRow& row);
    void drawColumn(const Column& column, S32 index, S32 height, bool focused);
    void drawDetails(const Column& column, S32 height);

    LLUUID mFolderID;
    std::vector<Column> mColumns;
    S32 mCurrent;       // index of the current column, -1 if none
    std::vector<bool> mSelected;
    S32 mSelectedCount;
    S32 mCursor;
    S32 mAnchor;
    S32 mFolderCount;
    S32 mItemCount;
    bool mLoading;
    bool mDirty;
    bool mDetailsShown;
    U32 mGeneration;
    SKInventoryListView::EColumn mSortKey;
    bool mSortAscending;
    std::unordered_set<LLUUID> mListedIDs;
    std::vector<LLUUID> mPath;

    S32 mStripX;
    S32 mStripPad;      // room added past the last column so the scroll needn't clamp
    scroll_map_t mScrolls;
    S32 mHoverColumn;
    S32 mHoverRow;

    LLScrollbar* mStripScrollbar;
    SKInventoryListLabels mLabels;

    id_callback_t mMoveCallback;
    id_callback_t mOpenCallback;
    void_callback_t mUpCallback;
    void_callback_t mSidebarCallback;
    void_callback_t mSelectionCallback;
    menu_callback_t mMenuCallback;
};

#endif // SK_SKINVENTORYCOLUMNSVIEW_H
