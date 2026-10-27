/**
 * @file skinventoryflatview.h
 * @brief Inventory Explorer's flat model and the base of the views drawn from it (List, Icons)
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

#ifndef SK_SKINVENTORYFLATVIEW_H
#define SK_SKINVENTORYFLATVIEW_H

#include "lleditmenuhandler.h"
#include "llframetimer.h"
#include "llhandle.h"
#include "lluictrl.h"
#include "lluiimage.h"

#include <functional>
#include <map>
#include <unordered_set>

class LLScrollbar;

// One row of a folder's contents, read from the inventory model when the folder is listed.
// Group headers and the paging row sit among the rows as rows of their own kind.
struct SKInventoryRow
{
    enum EKind
    {
        KIND_ENTRY,
        KIND_HEADER,    // a group's heading: mName, mGroupCount
        KIND_SHOW_ALL,  // "Show all N (showing 600)"
    };

    EKind mKind = KIND_ENTRY;
    LLUUID mID;
    LLUUID mTargetID;       // the folder or item a link points to, else mID
    bool mIsFolder = false;
    bool mIsSystem = false;
    bool mIsLink = false;
    bool mIsWorn = false;
    std::string mName;
    std::string mType;
    std::string mFolderPath; // the path below a searched folder, set by search
    LLUUID mCreatorID;
    std::string mCreatorName;
    time_t mAcquired = 0;
    std::string mAcquiredLabel;
    S32 mPermRank = 0;      // Full Perm, Copy & Modify, Copy & Transfer, Copy Only, No Copy
    std::string mPerms;
    std::string mWornOn;
    std::string mDescription;
    LLAssetType::EType mAssetType = LLAssetType::AT_NONE;
    LLUUID mImageID;        // a thumbnail, or a texture's own image
    bool mImageIsAsset = false;
    F32 mAspect = 1.f;      // width over height, from a "(w:h)" description tail
    LLPointer<LLUIImage> mIcon;
    std::string mGroup;
    S32 mGroupRank = 0;
    S32 mGroupCount = 0;

    bool isEntry() const { return mKind == KIND_ENTRY; }
};

// The labels the views draw, handed in by the floater so every string stays in XUI.
struct SKInventoryListLabels
{
    std::vector<std::string> mColumns;
    std::string mFolderCount;   // "Folder ([COUNT])"
    std::string mFolder;
    std::string mDateFormat;
    std::string mLoading;
    std::string mEmpty;
    std::string mWorn;
    std::string mMore;          // "and [COUNT] more"
    std::string mShowAll;       // "Show all [TOTAL] (showing [SHOWN])"
    std::map<std::string, std::string> mGroups;   // group heading keys to their labels
};

class SKInventoryFlatView : public LLUICtrl, public LLEditMenuHandler
{
public:
    AL_VIEW_TYPE(SKInventoryFlatView, LLUICtrl);

    enum EColumn
    {
        COL_NAME,
        COL_FOLDER,
        COL_TYPE,
        COL_CREATOR,
        COL_ACQUIRED,
        COL_PERMS,
        COL_WORN,
        COL_DESCRIPTION,
        COL_COUNT
    };

    enum EGroup
    {
        GROUP_NONE,
        GROUP_TYPE,
        GROUP_DATE,
        GROUP_BRAND,
        GROUP_PERMS,
        GROUP_WORN,
        GROUP_CREATOR,
        GROUP_COUNT
    };

    static constexpr S32 PAGE_ROWS = 600;

    typedef std::function<void(const LLUUID& id)> id_callback_t;
    typedef std::function<void()> void_callback_t;
    typedef std::function<void(S32 x, S32 y, const uuid_vec_t& ids)> menu_callback_t;

    ~SKInventoryFlatView();

    static const char* columnKey(EColumn col);
    static EColumn columnFromKey(const std::string& key);
    static const char* groupKey(EGroup group);
    static EGroup groupFromKey(const std::string& key);

    // Reads a folder's direct contents, starting a fetch while it is incomplete; true while it loads.
    static bool readRows(const LLUUID& folder_id, const SKInventoryListLabels& labels, std::vector<SKInventoryRow>& rows,
                         bool fetch = true);
    // Folders first (system folders first, then by name), items by the key.
    static void sortRows(std::vector<SKInventoryRow>& rows, EColumn key, bool ascending);
    // Orders sorted rows by group, keeping the sort within each, and puts a header before each group.
    static void groupRows(std::vector<SKInventoryRow>& rows, EGroup group, const SKInventoryListLabels& labels);

    void setLabels(const SKInventoryListLabels& labels) { mLabels = labels; }
    void setOpenCallback(const id_callback_t& cb) { mOpenCallback = cb; }
    void setUpCallback(const void_callback_t& cb) { mUpCallback = cb; }
    void setSelectionCallback(const void_callback_t& cb) { mSelectionCallback = cb; }
    void setMenuCallback(const menu_callback_t& cb) { mMenuCallback = cb; }

    // Lists the folder; the selection and scroll given are kept where their rows still exist.
    void setFolder(const LLUUID& folder_id, const uuid_vec_t& selection = uuid_vec_t(), S32 scroll = 0);
    const LLUUID& getFolder() const { return mFolderID; }

    // Reads the folder again on the next draw, keeping selection and scroll.
    void markDirty() { mDirty = true; }
    bool lists(const LLUUID& id) const { return mRowIDs.contains(id); }

    U32 getGeneration() const { return mGeneration; }
    S32 getSelectedCount() const { return mSelectedCount; }

    void setSort(EColumn key, bool ascending);
    EColumn getSortKey() const { return mSortKey; }
    bool getSortAscending() const { return mSortAscending; }
    void setGroup(EGroup group);
    EGroup getGroup() const { return mGroup; }

    uuid_vec_t getSelection() const;
    void setSelection(const uuid_vec_t& ids);
    S32 getScroll() const;

    S32 getFolderCount() const { return mFolderCount; }
    S32 getItemCount() const { return mItemCount; }
    bool isLoading() const { return mLoading; }

    // Opens the inline editor over the only selected row's name.
    void startRename(const LLUUID& id);
    // Opens the context menu over the cursor row, from the keyboard.
    void showMenuFromKeyboard();

    // LLView
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onFocusReceived() override;
    void onFocusLost() override;

    // LLEditMenuHandler
    LLView* asView() override { return this; }
    void cut() override;
    bool canCut() const override;
    void copy() override;
    bool canCopy() const override;
    void paste() override;
    bool canPaste() const override;
    void doDelete() override;
    bool canDoDelete() const override;
    void selectAll() override;
    bool canSelectAll() const override { return mEntryCount > 0; }
    void deselect() override;
    bool canDeselect() const override { return mSelectedCount > 0; }

protected:
    SKInventoryFlatView(const LLUICtrl::Params& p);

    // What the views lay out and draw differently.
    virtual void layoutRows() = 0;                              // after the rows or the size change
    virtual void drawContent() = 0;
    virtual S32 rowAt(S32 x, S32 y, bool* on_name) const = 0;   // -1 for none
    virtual void scrollToRow(S32 row) = 0;
    virtual S32 rowFromKey(KEY key, MASK mask) const = 0;       // where an arrow or page key goes, -1 for nowhere
    virtual LLRect nameRect(S32 row, bool& multiline) const = 0;
    virtual bool handleHeaderMouseDown(S32 x, S32 y, MASK mask) { return false; }
    virtual bool handleHeaderRightMouseDown(S32 x, S32 y, MASK mask) { return false; }

    void rebuild();
    void sortAndGroup();
    void selectRange(S32 from, S32 to, bool add);
    void moveCursor(S32 row, MASK mask);
    void notifySelection();
    S32 nextEntry(S32 row, S32 step) const;
    S32 firstEntry() const { return nextEntry(-1, 1); }
    S32 lastEntry() const { return nextEntry((S32)mRows.size(), -1); }
    void requestCreatorName(SKInventoryRow& row);
    void applyArrivedNames();
    void endRename();

    LLUUID mFolderID;
    std::vector<SKInventoryRow> mAllRows;   // every entry, sorted, before paging
    std::vector<SKInventoryRow> mRows;      // what is laid out: entries, headers and the paging row
    std::vector<bool> mSelected;
    std::unordered_set<LLUUID> mRowIDs;
    std::map<LLUUID, boost::signals2::connection> mNameRequests;
    LLFrameTimer mNameTimer;
    bool mNamesArrived;
    U32 mGeneration;
    S32 mSelectedCount;
    S32 mEntryCount;
    S32 mFolderCount;
    S32 mItemCount;
    S32 mCursor;
    S32 mAnchor;
    EColumn mSortKey;
    bool mSortAscending;
    EGroup mGroup;
    bool mShowAll;
    bool mDirty;
    bool mLoading;

    // A slow second click on the only selected row's name renames it.
    S32 mSlowClickRow;
    bool mSlowClickArmed;
    LLFrameTimer mSlowClickTimer;
    LLHandle<LLView> mRenameEditor;

    LLScrollbar* mScrollbar;
    SKInventoryListLabels mLabels;

    id_callback_t mOpenCallback;
    void_callback_t mUpCallback;
    void_callback_t mSelectionCallback;
    menu_callback_t mMenuCallback;
};

#endif // SK_SKINVENTORYFLATVIEW_H
