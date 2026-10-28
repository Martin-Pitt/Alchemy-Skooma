/**
 * @file skfloaterinventoryexplorer.h
 * @brief Inventory Explorer: a tabbed second inventory floater
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

#ifndef SK_SKFLOATERINVENTORYEXPLORER_H
#define SK_SKFLOATERINVENTORYEXPLORER_H

#include "llfloater.h"
#include "lluiimage.h"
#include "skinventorycolumnsview.h"
#include "skinventoryexplorerdrag.h"
#include "skinventoryexplorermenu.h"
#include "skinventoryiconsview.h"
#include "skinventorylistview.h"

#include <functional>
#include <memory>
#include <set>

class LLButton;
class LLInventoryObserver;
class LLLayoutPanel;
class LLSliderCtrl;
class LLTextBox;
class SKInventoryExplorerContextMenu;
class SKInventorySidebar;

// Everything one tab keeps: its folder, view, history, sort, group, selection and scroll.
struct SKExplorerTab
{
    enum EView
    {
        VIEW_COLUMNS,
        VIEW_ICONS,
        VIEW_LIST,
        VIEW_GALLERY,
    };

    LLUUID mFolderID;
    std::vector<LLUUID> mBack;
    std::vector<LLUUID> mForward;
    EView mView = VIEW_COLUMNS;
    SKInventoryListView::EColumn mSortKey = SKInventoryListView::COL_ACQUIRED;
    bool mSortAscending = false;
    SKInventoryFlatView::EGroup mGroup = SKInventoryFlatView::GROUP_NONE;
    uuid_vec_t mSelection;
    S32 mListScroll = 0;
    S32 mStripScroll = 0;
    SKInventoryColumnsView::scroll_map_t mColumnScrolls;
};

// The top row: flat square tabs, the new-tab button, and empty space that drags the floater.
class SKExplorerTabStrip : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKExplorerTabStrip, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    struct Tab
    {
        std::string mTitle;
        std::string mToolTip;
        LLPointer<LLUIImage> mIcon;
        LLUUID mFolderID;
    };

    typedef std::function<void(S32 index)> index_callback_t;
    typedef std::function<void()> void_callback_t;
    typedef std::function<bool(S32 x, S32 y, MASK mask)> drag_callback_t;

    void setTabs(const std::vector<Tab>& tabs, S32 active);
    void setActivateCallback(const index_callback_t& cb) { mActivateCallback = cb; }
    void setCloseCallback(const index_callback_t& cb) { mCloseCallback = cb; }
    void setNewTabCallback(const void_callback_t& cb) { mNewTabCallback = cb; }
    void setNewTabToolTip(const std::string& tip) { mNewTabToolTip = tip; }
    void setDragCallback(const drag_callback_t& cb) { mDragCallback = cb; }
    // A double-click on the empty space, as on a title bar.
    void setDoubleClickCallback(const void_callback_t& cb) { mDoubleClickCallback = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

protected:
    SKExplorerTabStrip(const Params& p);
    friend class LLUICtrlFactory;

    S32 tabWidth() const;
    S32 tabAt(S32 x) const;
    bool overClose(S32 tab, S32 x) const;
    LLRect plusRect() const;

    std::vector<Tab> mTabs;
    S32 mActive;
    S32 mHover;
    std::string mNewTabToolTip;
    index_callback_t mActivateCallback;
    index_callback_t mCloseCallback;
    void_callback_t mNewTabCallback;
    drag_callback_t mDragCallback;
    void_callback_t mDoubleClickCallback;
    // A drag held over a tab drops into its folder, and switches to it after 500 ms.
    SKDragHover mDropHover;
    S32 mDropTab;
    SKInventoryExplorerDrag::Verdict mDropVerdict;
};

// A pane's 6 px resize edge: drag to resize, double-click for the default, saved on release.
class SKPaneEdge : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKPaneEdge, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<std::string> pane;

        Params();
    };

    typedef std::function<void(const std::string& pane, S32 delta, bool done)> drag_callback_t;
    typedef std::function<void(const std::string& pane)> reset_callback_t;

    void setStartCallback(const reset_callback_t& cb) { mStartCallback = cb; }
    void setDragCallback(const drag_callback_t& cb) { mDragCallback = cb; }
    void setResetCallback(const reset_callback_t& cb) { mResetCallback = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

protected:
    SKPaneEdge(const Params& p);
    friend class LLUICtrlFactory;

    std::string mPane;
    S32 mDragStartX;
    bool mHovered;
    bool mDragging;
    S32 mLastDelta;
    reset_callback_t mStartCallback;
    drag_callback_t mDragCallback;
    reset_callback_t mResetCallback;
};

class SKFloaterInventoryExplorer final : public LLFloater
{
public:
    SKFloaterInventoryExplorer(const LLSD& key);
    ~SKFloaterInventoryExplorer();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;

    void onInventoryChanged(U32 mask);

private:
    SKExplorerTab& currentTab() { return mTabs[mActiveTab]; }

    void navigateTo(const LLUUID& folder_id, bool record_history);
    void goBack();
    void goForward();
    void goUp();
    void openObject(const LLUUID& id, bool new_tab);

    void newTab(const LLUUID& folder_id);
    void closeTab(S32 index);
    void activateTab(S32 index);
    void saveListState();
    void showCurrentTab();
    void showFolder();
    void setView(SKExplorerTab::EView view);
    bool isColumns() const { return !mTabs.empty() && mTabs[mActiveTab].mView == SKExplorerTab::VIEW_COLUMNS; }
    // The List or the Icons view while one shows, else null.
    SKInventoryFlatView* flatView() const;
    void setOption(const std::string& option);
    bool checkOption(const std::string& option) const;
    void applySortAndGroup();
    void startRename(const LLUUID& id);
    void onIconSizeChanged();
    void onHeaderDoubleClick();
    uuid_vec_t activeSelection() const;
    void setActiveSelection(const uuid_vec_t& ids);
    void revealInSidebar();
    bool onSidebarKey(KEY key, MASK mask);

    void refreshToolbar();
    void refreshTabStrip();
    void refreshStatus();
    void refreshDetails();

    void onPaneDragStart(const std::string& pane);
    void onPaneDrag(const std::string& pane, S32 delta, bool done);
    void onPaneReset(const std::string& pane);
    void setPaneWidth(const std::string& pane, S32 width, bool save);
    void onToggleDetails();
    void onListSortChanged();
    void saveSortAndGroup();
    void refreshSortAndGroupButtons();
    void showContextMenu(LLView* view, S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id);

    std::string folderPath(const LLUUID& folder_id) const;
    LLPointer<LLUIImage> folderIcon(const LLUUID& folder_id) const;
    std::string countLabel(S32 folders, S32 items) const;

    std::vector<SKExplorerTab> mTabs;
    S32 mActiveTab;

    SKExplorerTabStrip* mTabStrip;
    SKInventorySidebar* mSidebar;
    SKInventoryListView* mList;
    SKInventoryColumnsView* mColumns;
    SKInventoryIconsView* mIcons;
    LLButton* mSortBtn;
    LLButton* mGroupBtn;
    LLSliderCtrl* mSizeSlider;
    std::set<std::string> mHiddenColumns;
    SKExplorerOptions mOptions;
    LLButton* mBackBtn;
    LLButton* mForwardBtn;
    LLButton* mUpBtn;
    LLButton* mDetailsBtn;
    LLTextBox* mPathText;
    LLTextBox* mStatusText;
    LLTextBox* mDetailsText;
    LLLayoutPanel* mSidebarPanel;
    LLLayoutPanel* mDetailsPanel;
    LLLayoutPanel* mDetailsEdgePanel;
    S32 mPaneDragStartWidth;

    std::unique_ptr<LLInventoryObserver> mObserver;
    std::unique_ptr<SKInventoryExplorerContextMenu> mMenu;
    bool mNeedsRefresh;
    U32 mStatusGeneration;
    S32 mStatusSelected;
    std::string mLastAction;    // shown in place of the selection count until something is selected
};

#endif // SK_SKFLOATERINVENTORYEXPLORER_H
