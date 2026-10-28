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
#include "skinventoryexplorerdrag.h"

#include <functional>
#include <map>
#include <unordered_set>

class LLScrollbar;
class LLViewerInventoryCategory;
class LLViewerInventoryItem;

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
    bool mLinkBroken = false;   // a link with no target keeps the broken-link icon and no badge
    bool mIsWorn = false;
    std::string mName;
    std::string mType;
    std::string mFolderPath; // the path below a searched folder, set by search
    S32 mSearchRank = 0;     // 0 for a name hit, 1 for a description-only hit
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
    // A link shows its original's icon with this badge in the corner.
    bool hasLinkBadge() const { return mIsLink && !mLinkBroken; }
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
    std::string mNoResults;     // a search that found nothing
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
    // Reads one folder or item, false when the model has no such object.
    static bool readRow(const LLUUID& id, const SKInventoryListLabels& labels, SKInventoryRow& row);
    // The store tag at the start of a name (§3), empty for none.
    static std::string brandOf(const std::string& name);
    // The small link badge over the corner of an icon drawn at the rect.
    static void drawLinkBadge(const LLRect& icon);
    // Folders first (system folders first, then by name), items by the key.
    static void sortRows(std::vector<SKInventoryRow>& rows, EColumn key, bool ascending);
    // Orders sorted rows by group, keeping the sort within each, and puts a header before each group.
    static void groupRows(std::vector<SKInventoryRow>& rows, EGroup group, const SKInventoryListLabels& labels);

    void setLabels(const SKInventoryListLabels& labels) { mLabels = labels; }
    void setOpenCallback(const id_callback_t& cb) { mOpenCallback = cb; }
    void setUpCallback(const void_callback_t& cb) { mUpCallback = cb; }
    void setSelectionCallback(const void_callback_t& cb) { mSelectionCallback = cb; }
    void setMenuCallback(const menu_callback_t& cb) { mMenuCallback = cb; }
    // Esc calls this in place of clearing the selection (the Gallery leaves).
    void setEscapeCallback(const void_callback_t& cb) { mEscapeCallback = cb; }

    // Lists the folder; the selection and scroll given are kept where their rows still exist.
    void setFolder(const LLUUID& folder_id, const uuid_vec_t& selection = uuid_vec_t(), S32 scroll = 0);
    const LLUUID& getFolder() const { return mFolderID; }

    // Lists search hits under the folder in place of its contents: each id with its rank.
    void setSearchResults(const std::vector<std::pair<LLUUID, S32>>& hits);
    // Adds hits at the end, unsorted, while a walk runs; setSearchResults sorts them once it ends.
    void appendSearchResults(const std::vector<std::pair<LLUUID, S32>>& hits);
    void clearSearch();
    bool isSearching() const { return mSearching; }
    // Takes keyboard focus, selecting the first entry when asked and nothing is selected.
    void focusContent(bool select_first);
    // Scrolls a selected row to the middle of the view.
    void centerSelection();

    // Reads the folder again on the next draw, keeping selection and scroll.
    void markDirty() { mDirty = true; }
    bool lists(const LLUUID& id) const { return mRowIDs.contains(id); }

    U32 getGeneration() const { return mGeneration; }
    S32 getSelectedCount() const { return mSelectedCount; }

    virtual void setSort(EColumn key, bool ascending);
    EColumn getSortKey() const { return mSortKey; }
    bool getSortAscending() const { return mSortAscending; }
    void setGroup(EGroup group);
    EGroup getGroup() const { return mGroup; }

    uuid_vec_t getSelection() const;
    void setSelection(const uuid_vec_t& ids);
    S32 getScroll() const;
    void setScroll(S32 pos);
    // The first entry that is not a folder, null for none.
    LLUUID firstItem() const;

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
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;
    void onMouseCaptureLost() override;
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
    void undo() override;
    bool canUndo() const override;

protected:
    SKInventoryFlatView(const LLUICtrl::Params& p);

    // What the views lay out and draw differently.
    virtual void layoutRows() = 0;                              // after the rows or the size change
    virtual void drawContent() = 0;
    virtual S32 rowAt(S32 x, S32 y, bool* on_name) const = 0;   // -1 for none
    virtual void scrollToRow(S32 row) = 0;
    virtual S32 rowFromKey(KEY key, MASK mask) const = 0;       // where an arrow or page key goes, -1 for nowhere
    virtual LLRect nameRect(S32 row, bool& multiline) const = 0;
    // A press here lifts the row (Icons: the tile; List: the Name cell); elsewhere it draws a selection rectangle.
    virtual bool isDragHandle(S32 x, S32 y, S32 row) const { return true; }
    // Where the rows are drawn, below any header.
    virtual LLRect rowsArea() const { return getLocalRect(); }
    // Pixels from the top of the content, scroll included, and back.
    virtual S32 contentDown(S32 y) const = 0;
    virtual S32 contentToLocal(S32 down) const = 0;
    // The rows meeting a band of the content, in content pixels and view x.
    virtual void rowsInBand(S32 top, S32 bottom, S32 left, S32 right, std::vector<S32>& rows) const = 0;
    virtual LLRect rowRect(S32 row) const = 0;
    // Scrolls by pixels, positive downwards.
    virtual void scrollPixels(S32 pixels) = 0;
    // The width rows are laid out in: the bar keeps its own column while the view scrolls.
    S32 contentWidth() const;
    virtual bool handleHeaderMouseDown(S32 x, S32 y, MASK mask) { return false; }
    virtual bool handleHeaderRightMouseDown(S32 x, S32 y, MASK mask) { return false; }

    static void readFolderRow(LLViewerInventoryCategory* cat, const SKInventoryListLabels& labels, SKInventoryRow& row);
    static void readItemRow(LLViewerInventoryItem* item, const SKInventoryListLabels& labels, SKInventoryRow& row);

    void rebuild();
    void readSearchRows();
    bool readSearchRow(const std::pair<LLUUID, S32>& hit, SKInventoryRow& row);
    // The path below the searched folder, worked out once per parent while the search lasts.
    const std::string& searchPath(const LLUUID& parent_id);
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
    void findRenameRow();

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
    bool mSearching;
    std::vector<std::pair<LLUUID, S32>> mSearchHits;
    std::map<LLUUID, std::string> mSearchPaths;

    // A slow second click on the only selected row's name renames it.
    S32 mSlowClickRow;
    bool mSlowClickArmed;
    LLFrameTimer mSlowClickTimer;
    LLHandle<LLView> mRenameEditor;
    LLUUID mRenameID;
    S32 mRenameRow;

    // A press that may become a drag of the selection or a selection rectangle.
    enum EPress
    {
        PRESS_NONE,
        PRESS_ITEM,
        PRESS_LASSO,
    };
    void startLasso();
    void updateLasso(S32 x, S32 y);
    void endPress(bool cancel);
    void clickRow(S32 row, MASK mask);
    void drawLasso();
    void drawDropTarget();

    EPress mPress;
    S32 mPressX;
    S32 mPressY;
    S32 mPressDown;             // the press in content pixels, so the rectangle stays put as it scrolls
    S32 mPressRow;
    MASK mPressMask;
    bool mPressCollapse;        // a plain press on a selected row selects it alone if it is not dragged
    bool mPressToggle;          // a Ctrl press on a selected row deselects it only if it is not dragged
    bool mLassoOn;
    S32 mLassoX;
    S32 mLassoY;
    std::vector<bool> mLassoBase;       // what each swept row toggles against
    std::vector<bool> mLassoRestore;    // the selection at the press, for Esc
    std::vector<S32> mLassoSwept;
    LLFrameTimer mLassoTick;

    SKDragHover mDropHover;
    S32 mDropRow;               // the folder row under a drag, -1 for the open folder's space
    SKInventoryExplorerDrag::Verdict mDropVerdict;

    LLScrollbar* mScrollbar;
    SKInventoryListLabels mLabels;

    id_callback_t mOpenCallback;
    void_callback_t mUpCallback;
    void_callback_t mSelectionCallback;
    menu_callback_t mMenuCallback;
    void_callback_t mEscapeCallback;
};

#endif // SK_SKINVENTORYFLATVIEW_H
