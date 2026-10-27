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

#include "lleditmenuhandler.h"
#include "lluictrl.h"
#include "lluiimage.h"

#include <functional>

class LLScrollbar;

// One row of a folder's contents, read from the inventory model when the folder is listed.
struct SKInventoryRow
{
    LLUUID mID;
    LLUUID mTargetID;       // the folder or item a link points to, else mID
    bool mIsFolder = false;
    bool mIsSystem = false;
    bool mIsLink = false;
    bool mIsWorn = false;
    std::string mName;
    std::string mType;
    LLUUID mCreatorID;
    time_t mAcquired = 0;
    std::string mAcquiredLabel;
    S32 mPermRank = 0;      // Full Perm, Copy & Modify, Copy & Transfer, Copy Only, No Copy
    std::string mPerms;
    std::string mWornOn;
    std::string mDescription;
    LLPointer<LLUIImage> mIcon;
};

// The labels the view draws, handed in by its floater so every string stays in XUI.
struct SKInventoryListLabels
{
    std::vector<std::string> mColumns;
    std::string mFolderCount;   // "Folder ([COUNT])"
    std::string mFolder;
    std::string mDateFormat;
    std::string mLoading;
    std::string mEmpty;
    std::string mWorn;
};

class SKInventoryListView : public LLUICtrl, public LLEditMenuHandler
{
public:
    AL_VIEW_TYPE(SKInventoryListView, LLUICtrl);

    enum EColumn
    {
        COL_NAME,
        COL_TYPE,
        COL_CREATOR,
        COL_ACQUIRED,
        COL_PERMS,
        COL_WORN,
        COL_DESCRIPTION,
        COL_COUNT
    };

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    typedef std::function<void(const LLUUID& id)> id_callback_t;
    typedef std::function<void()> void_callback_t;
    typedef std::function<void(S32 x, S32 y, const uuid_vec_t& ids)> menu_callback_t;

    ~SKInventoryListView();

    static const char* columnKey(EColumn col);
    static EColumn columnFromKey(const std::string& key);

    void setLabels(const SKInventoryListLabels& labels) { mLabels = labels; }
    void setOpenCallback(const id_callback_t& cb) { mOpenCallback = cb; }
    void setUpCallback(const void_callback_t& cb) { mUpCallback = cb; }
    void setSelectionCallback(const void_callback_t& cb) { mSelectionCallback = cb; }
    void setMenuCallback(const menu_callback_t& cb) { mMenuCallback = cb; }
    void setSortCallback(const void_callback_t& cb) { mSortCallback = cb; }

    // Lists the folder; the selection and scroll given are kept where their rows still exist.
    void setFolder(const LLUUID& folder_id, const uuid_vec_t& selection = uuid_vec_t(), S32 scroll = 0);
    const LLUUID& getFolder() const { return mFolderID; }

    // Reads the folder again on the next draw, keeping selection and scroll.
    void markDirty() { mDirty = true; }
    bool listsAny(const uuid_vec_t& ids) const;

    void setSort(EColumn key, bool ascending);
    EColumn getSortKey() const { return mSortKey; }
    bool getSortAscending() const { return mSortAscending; }

    uuid_vec_t getSelection() const;
    void setSelection(const uuid_vec_t& ids);
    S32 getScroll() const;

    S32 getFolderCount() const { return mFolderCount; }
    S32 getItemCount() const { return (S32)mRows.size() - mFolderCount; }
    bool isLoading() const { return mLoading; }

    // LLView
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onFocusReceived() override;
    void onFocusLost() override;

    // LLEditMenuHandler
    LLView* asView() override { return this; }
    void doDelete() override;
    bool canDoDelete() const override;
    void selectAll() override;
    bool canSelectAll() const override { return !mRows.empty(); }
    void deselect() override;
    bool canDeselect() const override;

protected:
    SKInventoryListView(const Params& p);
    friend class LLUICtrlFactory;

    void rebuild();
    void sortRows();
    void updateScrollbar();
    void scrollToRow(S32 row);
    void selectRange(S32 from, S32 to, bool add);
    void moveCursor(S32 row, MASK mask);
    void notifySelection();

    S32 rowAt(S32 y) const;
    S32 columnAt(S32 x) const;
    S32 rowsPerPage() const;
    S32 firstVisibleRow() const;
    std::string cellText(const SKInventoryRow& row, EColumn col) const;
    std::string creatorName(const LLUUID& id) const;

    LLUUID mFolderID;
    std::vector<SKInventoryRow> mRows;
    std::vector<bool> mSelected;
    S32 mFolderCount;
    S32 mCursor;
    S32 mAnchor;
    EColumn mSortKey;
    bool mSortAscending;
    bool mDirty;
    bool mLoading;
    S32 mColumnWidths[COL_COUNT];

    LLScrollbar* mScrollbar;
    SKInventoryListLabels mLabels;

    id_callback_t mOpenCallback;
    void_callback_t mUpCallback;
    void_callback_t mSelectionCallback;
    menu_callback_t mMenuCallback;
    void_callback_t mSortCallback;
};

#endif // SK_SKINVENTORYLISTVIEW_H
