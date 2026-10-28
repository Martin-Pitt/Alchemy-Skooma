/**
 * @file skinventorytreeview.h
 * @brief Inventory Explorer's sidebar: Favorites, Inventory, Marketplace and Library as one drawn tree
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

#ifndef SK_SKINVENTORYTREEVIEW_H
#define SK_SKINVENTORYTREEVIEW_H

#include "llframetimer.h"
#include "lluictrl.h"
#include "lluiimage.h"
#include "skinventoryexplorerdrag.h"

#include <functional>
#include <map>
#include <set>

class LLInventoryPanel;
class LLScrollbar;

// The sidebar, drawn from a flat list of the rows it shows: headings, folders at their depth and the saved searches.
// The list is rebuilt only when expansion, the inventory or the saved searches change; drawing walks the rows in view.
class SKInventoryTreeView : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKInventoryTreeView, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    enum ESection
    {
        SECTION_FAVORITES,
        SECTION_INVENTORY,
        SECTION_MARKETPLACE,
        SECTION_LIBRARY,
        SECTION_COUNT
    };

    struct SavedSearch
    {
        std::string mQuery;
        std::string mSuffix;    // " in Folder", dim
        std::string mToolTip;
    };

    static constexpr S32 ROW_HEIGHT = 21;

    typedef std::function<void(const LLUUID& folder_id)> open_callback_t;
    typedef std::function<bool(KEY key, MASK mask)> key_callback_t;
    typedef std::function<void(S32 index)> index_callback_t;
    typedef std::function<void()> void_callback_t;
    typedef std::function<void(S32 x, S32 y, const LLUUID& folder_id)> menu_callback_t;

    ~SKInventoryTreeView();

    // Headings, the Favorites hint and the tooltips, from the floater's XUI.
    void setStrings(const std::map<std::string, std::string>& strings) { mStrings = strings; }
    void setOpenCallback(const open_callback_t& cb) { mOpenCallback = cb; }
    // Keys the floater takes first (→ in Columns).
    void setKeyCallback(const key_callback_t& cb) { mKeyCallback = cb; }
    // Enter and Tab hand focus to the content.
    void setFocusContentCallback(const void_callback_t& cb) { mFocusContentCallback = cb; }
    void setMenuCallback(const menu_callback_t& cb) { mMenuCallback = cb; }
    void setSavedSearchCallbacks(const index_callback_t& run, const index_callback_t& remove)
    {
        mRunSavedSearch = run;
        mRemoveSavedSearch = remove;
    }
    void setSavedSearches(const std::vector<SavedSearch>& searches, S32 selected);

    // Columns: the sidebar is the first column, flat, with no arrows and nothing expanding.
    void setFlat(bool flat);
    // The hidden stock panel the bridges the menus run actions through are built over.
    void setBridgePanel(LLInventoryPanel* panel) { mBridgePanel = panel; }
    LLInventoryPanel* getInventoryPanel() const { return mBridgePanel; }
    // Takes keyboard focus on the folder's row (revealing it), or on the selected row.
    void focusSelection(const LLUUID& folder_id = LLUUID::null);
    // Selects the folder's row in the section that should show it, expanding to it, and scrolls
    // only if no copy of the row is in view.
    void reveal(const LLUUID& folder_id);
    // Follows what the inventory says changed: favourites, counts and rows are updated only where it touches them.
    void onInventoryChanged(const std::set<LLUUID>& changed_ids, U32 mask);

    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;
    void onFocusReceived() override;
    void onFocusLost() override;

protected:
    SKInventoryTreeView(const Params& p);
    friend class LLUICtrlFactory;

    struct Row
    {
        enum EKind
        {
            KIND_HEADING,
            KIND_FOLDER,
            KIND_SAVED,
            KIND_HINT,
        };
        EKind mKind = KIND_FOLDER;
        ESection mSection = SECTION_INVENTORY;
        LLUUID mID;
        std::string mKey;       // section and id path: a favourite's copy expands on its own
        S32 mDepth = 0;
        bool mHasChildren = false;
        bool mExpanded = false;
        S32 mSaved = -1;        // the saved search's index
        std::string mName;
        LLPointer<LLUIImage> mIcon;
        S32 mTop = 0;           // pixels down from the content's top
        S32 mHeight = ROW_HEIGHT;

        bool isSelectable() const { return mKind == KIND_FOLDER || mKind == KIND_SAVED; }
    };

    void rebuild();
    void walkFavorites();
    void sortFavorites();
    // Drops the cached counts of the folder and every folder above it, as the model has it now and as it had it.
    void dropCounts(const LLUUID& id);
    void addFolderRows(const LLUUID& folder_id, ESection section, const std::string& parent_key, S32 depth);
    void addFolderRow(const LLUUID& folder_id, ESection section, const std::string& key, S32 depth);
    void layout();
    S32 contentWidth() const;
    S32 rowAt(S32 y) const;
    LLRect rowRect(S32 index) const;
    LLRect arrowRect(S32 index) const;
    LLRect removeRect(S32 index) const;
    void drawRow(S32 index, const LLRect& rect);
    void selectIndex(S32 index, bool open);
    void setExpanded(S32 index, bool expanded);
    void scrollToIndex(S32 index);
    bool isIndexInView(S32 index) const;
    S32 indexOfKey(const std::string& key) const;
    S32 stepSelectable(S32 from, S32 step) const;
    // The section and key a folder shows under, expanding its ancestors; false if no section lists it.
    bool keyFor(const LLUUID& folder_id, ESection& section, std::string& key, std::vector<std::string>& ancestors) const;
    LLUUID favoriteRootOf(const LLUUID& folder_id) const;
    // The selected folder's recursive item count, compact; "…" while it loads.
    std::string countLabel(const LLUUID& folder_id, std::string* exact);
    const std::string& str(const std::string& key) const;

    std::vector<Row> mRows;
    bool mDirty;
    bool mLaidOut;
    S32 mContentHeight;
    std::set<std::string> mExpanded;
    std::set<std::string> mExpandedBeforeFlat;
    std::set<LLUUID> mFavoriteSet;      // every favourite folder: one walk at first use, then kept by the changes
    std::vector<LLUUID> mFavorites;     // the outermost ones, sorted by name
    bool mFavoritesWalked;
    std::set<LLUUID> mRowIDs;           // the folders the rows show
    std::vector<SavedSearch> mSavedSearches;
    S32 mSavedSelected;
    bool mFlat;

    std::string mSelectedKey;
    LLUUID mSelectedID;
    S32 mHover;
    std::string mHoverKey;            // the hovered row survives a rebuild
    bool mHoverArrow;
    std::string mPendingScrollKey;      // a reveal scrolls once the rows are laid out

    // Counts per folder for the session; dropped when the inventory changes.
    std::map<LLUUID, std::pair<S32, bool>> mCounts;
    // Each folder under a counted one, to its parent as it was counted: a removed or moved folder's old ancestors.
    std::map<LLUUID, LLUUID> mCountedParents;

    // A press that may become a drag of the row.
    S32 mPressIndex;
    S32 mPressX;
    S32 mPressY;

    std::string mTypeAhead;
    LLFrameTimer mTypeAheadTimer;

    SKDragHover mDropHover;

    LLScrollbar* mScrollbar;
    LLInventoryPanel* mBridgePanel;
    std::map<std::string, std::string> mStrings;
    open_callback_t mOpenCallback;
    key_callback_t mKeyCallback;
    void_callback_t mFocusContentCallback;
    menu_callback_t mMenuCallback;
    index_callback_t mRunSavedSearch;
    index_callback_t mRemoveSavedSearch;
};

#endif // SK_SKINVENTORYTREEVIEW_H
