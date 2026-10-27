/**
 * @file skfloaterinventoryexplorer.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skfloaterinventoryexplorer.h"

#include "llbutton.h"
#include "lldraghandle.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "lllocalcliprect.h"
#include "lllayoutstack.h"
#include "llrender2dutils.h"
#include "lltextbox.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "skinventoryexplorermenu.h"
#include "skinventorysidebar.h"

static LLDefaultChildRegistry::Register<SKExplorerTabStrip> r_sk_tab_strip("sk_explorer_tab_strip");
static LLDefaultChildRegistry::Register<SKPaneEdge> r_sk_pane_edge("sk_pane_edge");

namespace
{
    constexpr S32 TAB_WIDTH = 180;
    constexpr S32 TAB_MIN_WIDTH = 60;
    constexpr S32 PLUS_WIDTH = 24;
    constexpr S32 CLOSE_WIDTH = 16;
    constexpr S32 PANE_MIN = 140;
    constexpr F32 PANE_MAX_FRACTION = 0.45f;
    constexpr S32 CONTENT_MIN = 200;
    constexpr S32 SIDEBAR_DEFAULT = 220;
    constexpr S32 DETAILS_DEFAULT = 250;
    const char* const PATH_SEPARATOR = " \xE2\x80\xBA ";
}

//
// SKExplorerTabStrip
//

SKExplorerTabStrip::SKExplorerTabStrip(const Params& p)
:   LLUICtrl(p),
    mActive(0),
    mHover(-1)
{
}

void SKExplorerTabStrip::setTabs(const std::vector<Tab>& tabs, S32 active)
{
    mTabs = tabs;
    mActive = active;
}

S32 SKExplorerTabStrip::tabWidth() const
{
    if (mTabs.empty())
    {
        return TAB_WIDTH;
    }
    const S32 room = getRect().getWidth() - PLUS_WIDTH;
    return llclamp(room / (S32)mTabs.size(), TAB_MIN_WIDTH, TAB_WIDTH);
}

S32 SKExplorerTabStrip::tabAt(S32 x) const
{
    const S32 index = x / tabWidth();
    return (x >= 0 && index < (S32)mTabs.size()) ? index : -1;
}

bool SKExplorerTabStrip::overClose(S32 tab, S32 x) const
{
    if (tab < 0 || mTabs.size() < 2 || (tab != mActive && tab != mHover))
    {
        return false;
    }
    const S32 right = (tab + 1) * tabWidth();
    return x >= right - CLOSE_WIDTH - 4 && x < right - 2;
}

LLRect SKExplorerTabStrip::plusRect() const
{
    const S32 left = (S32)mTabs.size() * tabWidth();
    return LLRect(left, getRect().getHeight(), left + PLUS_WIDTH, 0);
}

void SKExplorerTabStrip::draw()
{
    static LLUIColor strip_bg = LLUIColorTable::instance().getColor("Black_50");
    static LLUIColor active_bg = LLUIColorTable::instance().getColor("DkGray2");
    static LLUIColor hover_bg = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor separator = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor text_active = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor text_dim = LLUIColorTable::instance().getColor("LtGray_50");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 height = getRect().getHeight();
    const S32 width = tabWidth();
    gl_rect_2d(0, height, getRect().getWidth(), 0, strip_bg.get());

    LLLocalClipRect clip(getLocalRect());
    for (S32 i = 0; i < (S32)mTabs.size(); ++i)
    {
        const S32 left = i * width;
        const bool active = i == mActive;
        if (active)
        {
            // The active tab takes the toolbar's background and overlaps the strip's border.
            gl_rect_2d(left, height, left + width, -1, active_bg.get());
        }
        else
        {
            if (i == mHover)
            {
                gl_rect_2d(left, height, left + width, 0, hover_bg.get());
            }
            if (i + 1 < (S32)mTabs.size() && i + 1 != mActive)
            {
                gl_line_2d(left + width, height - 5, left + width, 5, separator.get());
            }
        }

        S32 x = left + 6;
        const Tab& tab = mTabs[i];
        if (tab.mIcon.notNull())
        {
            tab.mIcon->draw(x, (height - 16) / 2, 16, 16, active || i == mHover ? LLColor4::white : LLColor4(1.f, 1.f, 1.f, 0.6f));
            x += 20;
        }
        const bool show_close = mTabs.size() > 1 && (active || i == mHover);
        const S32 text_right = left + width - (show_close ? CLOSE_WIDTH + 6 : 6);
        font->renderUTF8(tab.mTitle, 0, (F32)x, (F32)(height / 2), (active || i == mHover) ? text_active.get() : text_dim.get(),
                         LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                         llmax(0, text_right - x), nullptr, true);
        if (show_close)
        {
            font->renderUTF8("\xC3\x97", 0, left + width - CLOSE_WIDTH / 2 - 4, height / 2, text_active.get(),
                             LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }

    const LLRect plus = plusRect();
    if (mHover == (S32)mTabs.size())
    {
        gl_rect_2d(plus, hover_bg.get());
    }
    font->renderUTF8("+", 0, plus.getCenterX(), height / 2, text_active.get(), LLFontGL::HCENTER, LLFontGL::VCENTER);

    LLUICtrl::draw();
}

bool SKExplorerTabStrip::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 tab = tabAt(x);
    if (tab >= 0)
    {
        if (overClose(tab, x))
        {
            if (mCloseCallback)
            {
                mCloseCallback(tab);
            }
        }
        else if (mActivateCallback)
        {
            mActivateCallback(tab);
        }
        return true;
    }
    if (plusRect().pointInRect(x, y))
    {
        if (mNewTabCallback)
        {
            mNewTabCallback();
        }
        return true;
    }

    // Empty space moves the floater, as a title bar would.
    return mDragCallback && mDragCallback(x, y, mask);
}

bool SKExplorerTabStrip::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = tabAt(x);
    if (mHover < 0 && plusRect().pointInRect(x, y))
    {
        mHover = (S32)mTabs.size();
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SKExplorerTabStrip::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKExplorerTabStrip::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 tab = tabAt(x);
    if (tab >= 0)
    {
        LLToolTipMgr::instance().show(mTabs[tab].mToolTip);
        return true;
    }
    if (plusRect().pointInRect(x, y))
    {
        LLToolTipMgr::instance().show(mNewTabToolTip);
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

//
// SKPaneEdge
//

SKPaneEdge::Params::Params()
:   pane("pane")
{
}

SKPaneEdge::SKPaneEdge(const Params& p)
:   LLUICtrl(p),
    mPane(p.pane()),
    mDragStartX(0),
    mHovered(false),
    mDragging(false),
    mLastDelta(0)
{
}

void SKPaneEdge::onMouseCaptureLost()
{
    // A drag cut short still ends, and its width is saved.
    if (mDragging)
    {
        mDragging = false;
        if (mDragCallback)
        {
            mDragCallback(mPane, mLastDelta, true);
        }
    }
    LLUICtrl::onMouseCaptureLost();
}

void SKPaneEdge::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

void SKPaneEdge::draw()
{
    static LLUIColor highlight = LLUIColorTable::instance().getColor("EmphasisColor");
    if (hasMouseCapture() || mHovered)
    {
        gl_rect_2d(getLocalRect(), highlight.get());
    }
    LLUICtrl::draw();
}

bool SKPaneEdge::handleMouseDown(S32 x, S32 y, MASK mask)
{
    S32 screen_y = 0;
    localPointToScreen(x, y, &mDragStartX, &screen_y);
    mDragging = true;
    mLastDelta = 0;
    if (mStartCallback)
    {
        mStartCallback(mPane);
    }
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SKPaneEdge::handleHover(S32 x, S32 y, MASK mask)
{
    mHovered = true;
    getWindow()->setCursor(UI_CURSOR_SIZEWE);
    if (hasMouseCapture() && mDragCallback)
    {
        S32 screen_x = 0;
        S32 screen_y = 0;
        localPointToScreen(x, y, &screen_x, &screen_y);
        mLastDelta = screen_x - mDragStartX;
        mDragCallback(mPane, mLastDelta, false);
    }
    return true;
}

bool SKPaneEdge::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        S32 screen_x = 0;
        S32 screen_y = 0;
        localPointToScreen(x, y, &screen_x, &screen_y);
        mLastDelta = screen_x - mDragStartX;
        // Releasing capture ends the drag through onMouseCaptureLost.
        gFocusMgr.setMouseCapture(nullptr);
    }
    return true;
}

bool SKPaneEdge::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mResetCallback)
    {
        mResetCallback(mPane);
    }
    return true;
}

//
// SKFloaterInventoryExplorer
//

class SKExplorerInventoryObserver final : public LLInventoryObserver
{
public:
    SKExplorerInventoryObserver(SKFloaterInventoryExplorer* floater) : mFloater(floater) {}
    void changed(U32 mask) override { mFloater->onInventoryChanged(mask); }

private:
    SKFloaterInventoryExplorer* mFloater;
};

SKFloaterInventoryExplorer::SKFloaterInventoryExplorer(const LLSD& key)
:   LLFloater(key),
    mActiveTab(0),
    mTabStrip(nullptr),
    mSidebar(nullptr),
    mList(nullptr),
    mBackBtn(nullptr),
    mForwardBtn(nullptr),
    mUpBtn(nullptr),
    mDetailsBtn(nullptr),
    mPathText(nullptr),
    mStatusText(nullptr),
    mDetailsText(nullptr),
    mSidebarPanel(nullptr),
    mDetailsPanel(nullptr),
    mDetailsEdgePanel(nullptr),
    mPaneDragStartWidth(0),
    mNeedsRefresh(true),
    mStatusGeneration(0),
    mStatusSelected(-1)
{
}

SKFloaterInventoryExplorer::~SKFloaterInventoryExplorer()
{
    if (mObserver)
    {
        gInventory.removeObserver(mObserver.get());
    }
}

bool SKFloaterInventoryExplorer::postBuild()
{
    mTabStrip = getChild<SKExplorerTabStrip>("tab_strip");
    mSidebar = getChild<SKInventorySidebar>("sidebar");
    mList = getChild<SKInventoryListView>("list_view");
    mColumns = getChild<SKInventoryColumnsView>("columns_view");
    mBackBtn = getChild<LLButton>("back_btn");
    mForwardBtn = getChild<LLButton>("forward_btn");
    mUpBtn = getChild<LLButton>("up_btn");
    mDetailsBtn = getChild<LLButton>("details_btn");
    mPathText = getChild<LLTextBox>("path_text");
    mStatusText = getChild<LLTextBox>("status_text");
    mDetailsText = getChild<LLTextBox>("details_text");
    mSidebarPanel = getChild<LLLayoutPanel>("sidebar_panel");
    mDetailsPanel = getChild<LLLayoutPanel>("details_panel");
    mDetailsEdgePanel = getChild<LLLayoutPanel>("details_edge_panel");

    mTabStrip->setNewTabToolTip(getString("new_tab_tooltip"));
    mTabStrip->setActivateCallback([this](S32 index) { activateTab(index); });
    mTabStrip->setCloseCallback([this](S32 index) { closeTab(index); });
    mTabStrip->setNewTabCallback([this]() { newTab(currentTab().mFolderID); });
    mTabStrip->setDragCallback([this](S32 x, S32 y, MASK mask)
                               {
                                   LLDragHandle* handle = getDragHandle();
                                   if (!handle)
                                   {
                                       return false;
                                   }
                                   S32 hx = 0;
                                   S32 hy = 0;
                                   mTabStrip->localPointToOtherView(x, y, &hx, &hy, handle);
                                   return handle->handleMouseDown(hx, hy, mask);
                               });

    mBackBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { goBack(); });
    mForwardBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { goForward(); });
    mUpBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { goUp(); });
    mDetailsBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onToggleDetails(); });

    // Icons and Gallery are not built yet; they say so.
    for (const char* name : { "view_icons_btn", "view_gallery_btn" })
    {
        LLButton* button = getChild<LLButton>(name);
        button->setToolTip(button->getToolTip() + " " + getString("view_unavailable"));
        button->setEnabled(false);
    }
    getChild<LLButton>("view_columns_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setView(SKExplorerTab::VIEW_COLUMNS); });
    getChild<LLButton>("view_list_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setView(SKExplorerTab::VIEW_LIST); });

    for (const char* name : { "sidebar_edge", "details_edge" })
    {
        SKPaneEdge* edge = getChild<SKPaneEdge>(name);
        edge->setStartCallback([this](const std::string& pane) { onPaneDragStart(pane); });
        edge->setDragCallback([this](const std::string& pane, S32 delta, bool done) { onPaneDrag(pane, delta, done); });
        edge->setResetCallback([this](const std::string& pane) { onPaneReset(pane); });
    }

    SKInventoryListLabels labels;
    for (const char* key : { "col_name", "col_type", "col_creator", "col_acquired", "col_perms", "col_worn", "col_description" })
    {
        labels.mColumns.push_back(getString(key));
    }
    labels.mFolderCount = getString("folder_count");
    labels.mFolder = getString("folder");
    labels.mDateFormat = getString("date_format");
    labels.mLoading = getString("loading");
    labels.mEmpty = getString("empty_folder");
    labels.mWorn = getString("worn");
    labels.mMore = getString("n_more");
    mList->setLabels(labels);
    mColumns->setLabels(labels);
    mList->setOpenCallback([this](const LLUUID& id) { openObject(id, false); });
    mList->setUpCallback([this]() { goUp(); });
    mList->setSelectionCallback([this]() { refreshDetails(); });
    mList->setSortCallback([this]() { onListSortChanged(); });
    mList->setMenuCallback([this](S32 x, S32 y, const uuid_vec_t& ids) { showContextMenu(mList, x, y, ids, mList->getFolder()); });

    mColumns->setMoveCallback([this](const LLUUID& folder_id) { navigateTo(folder_id, false); });
    mColumns->setOpenCallback([this](const LLUUID& id) { openObject(id, false); });
    mColumns->setUpCallback([this]() { goUp(); });
    mColumns->setSidebarCallback([this]() { mSidebar->focusSelection(); });
    mColumns->setSelectionCallback([this]() { refreshDetails(); });
    mColumns->setMenuCallback([this](S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id)
                              { showContextMenu(mColumns, x, y, ids, folder_id); });

    mSidebar->setOpenCallback([this](const LLUUID& folder_id) { navigateTo(folder_id, true); });
    mSidebar->setKeyCallback([this](KEY key, MASK mask) { return onSidebarKey(key, mask); });

    mMenu = std::make_unique<SKInventoryExplorerContextMenu>([this](const LLUUID& id, bool new_tab) { openObject(id, new_tab); });

    const bool details = gSavedPerAccountSettings.getBOOL("SkoomaInvExplorerDetailsShown");
    mDetailsBtn->setToggleState(details);
    mColumns->setDetailsShown(details);
    setPaneWidth("sidebar", gSavedPerAccountSettings.getS32("SkoomaInvExplorerSidebarWidth"), false);
    setPaneWidth("details", gSavedPerAccountSettings.getS32("SkoomaInvExplorerDetailsWidth"), false);

    mObserver = std::make_unique<SKExplorerInventoryObserver>(this);
    gInventory.addObserver(mObserver.get());

    newTab(gInventory.getRootFolderID());
    return true;
}

void SKFloaterInventoryExplorer::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
}

void SKFloaterInventoryExplorer::draw()
{
    if (mNeedsRefresh)
    {
        mNeedsRefresh = false;
        refreshToolbar();
        refreshTabStrip();
        refreshDetails();
        mStatusSelected = -1;
    }
    // The status follows the view's counts, read only when they move.
    const U32 generation = isColumns() ? mColumns->getGeneration() : mList->getGeneration();
    const S32 selected = isColumns() ? mColumns->getSelectedCount() : mList->getSelectedCount();
    if (generation != mStatusGeneration || selected != mStatusSelected)
    {
        mStatusGeneration = generation;
        mStatusSelected = selected;
        refreshStatus();
    }
    LLFloater::draw();
}

bool SKFloaterInventoryExplorer::handleKeyHere(KEY key, MASK mask)
{
    if (mask == MASK_CONTROL)
    {
        switch (key)
        {
            case 'T':
                newTab(currentTab().mFolderID);
                return true;
            case 'W':
                closeTab(mActiveTab);
                return true;
            case KEY_TAB:
                activateTab((mActiveTab + 1) % (S32)mTabs.size());
                return true;
            case KEY_RETURN:
            {
                const uuid_vec_t selection = activeSelection();
                if (selection.size() == 1)
                {
                    openObject(selection.front(), true);
                }
                return true;
            }
            case '1':
                setView(SKExplorerTab::VIEW_COLUMNS);
                return true;
            case '3':
                setView(SKExplorerTab::VIEW_LIST);
                return true;
            case '2':
            case '4':
                return true; // Icons and Gallery are not built yet
            default:
                break;
        }
    }
    else if (mask == (MASK_CONTROL | MASK_SHIFT) && key == KEY_TAB)
    {
        activateTab((mActiveTab + (S32)mTabs.size() - 1) % (S32)mTabs.size());
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

void SKFloaterInventoryExplorer::onInventoryChanged(U32 mask)
{
    constexpr U32 RELEVANT = LLInventoryObserver::ADD | LLInventoryObserver::REMOVE | LLInventoryObserver::LABEL
        | LLInventoryObserver::STRUCTURE | LLInventoryObserver::INTERNAL | LLInventoryObserver::REBUILD
        | LLInventoryObserver::UPDATE_CREATE;
    if (!(mask & RELEVANT) || mTabs.empty())
    {
        return;
    }

    const LLUUID& folder_id = mList->getFolder();
    bool affects_list = (mask & LLInventoryObserver::REBUILD) != 0;
    for (const LLUUID& id : gInventory.getChangedIDs())
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        if (id == folder_id || (obj && obj->getParentUUID() == folder_id) || mList->lists(id))
        {
            affects_list = true;
            break;
        }
    }
    if (affects_list)
    {
        mList->markDirty();
    }
    bool affects_columns = (mask & LLInventoryObserver::REBUILD) != 0;
    for (const LLUUID& id : gInventory.getChangedIDs())
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        if (mColumns->lists(id) || (obj && mColumns->lists(obj->getParentUUID())))
        {
            affects_columns = true;
            break;
        }
    }
    if (affects_columns)
    {
        mColumns->markDirty();
    }

    // A tab whose folder is gone goes back to its nearest live ancestor the columns know of, else My Inventory,
    // and its history forgets folders that are gone.
    auto is_gone = [](const LLUUID& id) { return !gInventory.getCategory(id); };
    for (S32 i = 0; i < (S32)mTabs.size(); ++i)
    {
        std::erase_if(mTabs[i].mBack, is_gone);
        std::erase_if(mTabs[i].mForward, is_gone);
        if (!gInventory.getCategory(mTabs[i].mFolderID))
        {
            const LLUUID live = (i == mActiveTab && mColumns->getFolder() == mTabs[i].mFolderID) ? mColumns->nearestLiveFolder() : LLUUID::null;
            mTabs[i].mFolderID = live.notNull() ? live : gInventory.getRootFolderID();
            mTabs[i].mSelection.clear();
            mTabs[i].mListScroll = 0;
            if (i == mActiveTab)
            {
                showCurrentTab();
            }
        }
    }
    mNeedsRefresh = true;
}

void SKFloaterInventoryExplorer::saveListState()
{
    if (mTabs.empty())
    {
        return;
    }
    SKExplorerTab& tab = currentTab();
    if (isColumns())
    {
        tab.mSelection = mColumns->getSelection();
        tab.mStripScroll = mColumns->getStripScroll();
        tab.mColumnScrolls = mColumns->getColumnScrolls();
    }
    else
    {
        tab.mSelection = mList->getSelection();
        tab.mListScroll = mList->getScroll();
        tab.mSortKey = mList->getSortKey();
        tab.mSortAscending = mList->getSortAscending();
    }
}

void SKFloaterInventoryExplorer::showCurrentTab()
{
    SKExplorerTab& tab = currentTab();
    const bool columns = isColumns();
    mList->setVisible(!columns);
    mColumns->setVisible(columns);
    getChild<LLButton>("view_columns_btn")->setToggleState(columns);
    getChild<LLButton>("view_list_btn")->setToggleState(!columns);
    // Columns shows details as a column of its own, in place of the pane.
    const bool details = mDetailsBtn->getToggleState();
    mDetailsPanel->setVisible(details && !columns);
    mDetailsEdgePanel->setVisible(details && !columns);
    mSidebar->setFlat(columns);
    if (columns)
    {
        mColumns->setSort(tab.mSortKey, tab.mSortAscending);
        mColumns->setScrollState(tab.mStripScroll, tab.mColumnScrolls);
        mColumns->setFolder(tab.mFolderID, tab.mSelection);
    }
    else
    {
        mList->setSort(tab.mSortKey, tab.mSortAscending);
        mList->setFolder(tab.mFolderID, tab.mSelection, tab.mListScroll);
    }
    revealInSidebar();
    mNeedsRefresh = true;
}

void SKFloaterInventoryExplorer::showFolder()
{
    const SKExplorerTab& tab = currentTab();
    if (isColumns())
    {
        mColumns->setFolder(tab.mFolderID);
    }
    else
    {
        mList->setFolder(tab.mFolderID);
    }
    revealInSidebar();
    mNeedsRefresh = true;
}

void SKFloaterInventoryExplorer::revealInSidebar()
{
    const LLUUID& folder_id = currentTab().mFolderID;
    if (!isColumns())
    {
        mSidebar->reveal(folder_id);
        return;
    }
    // In Columns the sidebar marks the top-level folder the path runs through.
    LLUUID top = folder_id;
    const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    while (cat && cat->getParentUUID().notNull())
    {
        const LLViewerInventoryCategory* parent = gInventory.getCategory(cat->getParentUUID());
        if (!parent || parent->getParentUUID().isNull())
        {
            break;
        }
        cat = parent;
    }
    mSidebar->reveal(cat ? cat->getUUID() : top);
}

void SKFloaterInventoryExplorer::setView(SKExplorerTab::EView view)
{
    if (mTabs.empty() || currentTab().mView == view)
    {
        return;
    }
    saveListState();
    currentTab().mView = view;
    gSavedPerAccountSettings.setS32("SkoomaInvExplorerView", (S32)view);
    showCurrentTab();
}

uuid_vec_t SKFloaterInventoryExplorer::activeSelection() const
{
    return isColumns() ? mColumns->getSelection() : mList->getSelection();
}

void SKFloaterInventoryExplorer::setActiveSelection(const uuid_vec_t& ids)
{
    if (isColumns())
    {
        mColumns->setSelection(ids);
    }
    else
    {
        mList->setSelection(ids);
    }
}

bool SKFloaterInventoryExplorer::onSidebarKey(KEY key, MASK mask)
{
    // In Columns the sidebar is column 0, so -> steps into the first column.
    if (isColumns() && key == KEY_RIGHT && mask == MASK_NONE)
    {
        mColumns->setFocus(true);
        mColumns->selectFirstRow();
        return true;
    }
    return false;
}

void SKFloaterInventoryExplorer::navigateTo(const LLUUID& folder_id, bool record_history)
{
    if (folder_id.isNull() || !gInventory.getCategory(folder_id))
    {
        return;
    }
    SKExplorerTab& tab = currentTab();
    if (tab.mFolderID == folder_id)
    {
        revealInSidebar();
        return;
    }
    if (record_history)
    {
        tab.mBack.push_back(tab.mFolderID);
        tab.mForward.clear();
    }
    tab.mFolderID = folder_id;
    tab.mSelection.clear();
    tab.mListScroll = 0;
    showFolder();
}

// Steps through one history stack onto the next live folder that differs from the current one.
static bool step_history(SKExplorerTab& tab, std::vector<LLUUID>& from, std::vector<LLUUID>& to, LLUUID& target)
{
    while (!from.empty())
    {
        const LLUUID id = from.back();
        from.pop_back();
        if (id != tab.mFolderID && gInventory.getCategory(id))
        {
            to.push_back(tab.mFolderID);
            target = id;
            return true;
        }
    }
    return false;
}

void SKFloaterInventoryExplorer::goBack()
{
    SKExplorerTab& tab = currentTab();
    LLUUID target;
    if (step_history(tab, tab.mBack, tab.mForward, target))
    {
        navigateTo(target, false);
    }
}

void SKFloaterInventoryExplorer::goForward()
{
    SKExplorerTab& tab = currentTab();
    LLUUID target;
    if (step_history(tab, tab.mForward, tab.mBack, target))
    {
        navigateTo(target, false);
    }
}

void SKFloaterInventoryExplorer::goUp()
{
    const LLViewerInventoryCategory* cat = gInventory.getCategory(currentTab().mFolderID);
    if (cat && cat->getParentUUID().notNull())
    {
        const LLUUID came_from = cat->getUUID();
        navigateTo(cat->getParentUUID(), true);
        setActiveSelection({ came_from });
        mNeedsRefresh = true;
    }
}

void SKFloaterInventoryExplorer::openObject(const LLUUID& id, bool new_tab)
{
    const LLUUID folder_id = SKInventoryExplorerActions::folderOf(id);
    if (folder_id.isNull())
    {
        SKInventoryExplorerActions::openItem(id);
    }
    else if (new_tab)
    {
        newTab(folder_id);
    }
    else
    {
        navigateTo(folder_id, true);
    }
}

void SKFloaterInventoryExplorer::newTab(const LLUUID& folder_id)
{
    SKExplorerTab tab;
    tab.mFolderID = folder_id;
    tab.mView = mTabs.empty() ? (SKExplorerTab::EView)llclamp(gSavedPerAccountSettings.getS32("SkoomaInvExplorerView"), 0, 3)
                              : currentTab().mView;
    if (tab.mView != SKExplorerTab::VIEW_LIST)
    {
        tab.mView = SKExplorerTab::VIEW_COLUMNS; // Icons and Gallery are not built yet
    }
    tab.mSortKey = SKInventoryListView::columnFromKey(gSavedPerAccountSettings.getString("SkoomaInvExplorerSortKey"));
    tab.mSortAscending = gSavedPerAccountSettings.getBOOL("SkoomaInvExplorerSortAscending");
    tab.mGroup = gSavedPerAccountSettings.getString("SkoomaInvExplorerGroup");

    saveListState();
    const S32 index = mTabs.empty() ? 0 : mActiveTab + 1;
    mTabs.insert(mTabs.begin() + index, tab);
    mActiveTab = index;
    showCurrentTab();
}

void SKFloaterInventoryExplorer::closeTab(S32 index)
{
    if (mTabs.size() < 2 || index < 0 || index >= (S32)mTabs.size())
    {
        return; // the last tab can't close
    }
    if (index != mActiveTab)
    {
        saveListState();
    }
    const bool closing_active = index == mActiveTab;
    mTabs.erase(mTabs.begin() + index);
    if (closing_active)
    {
        // The tab to its right takes over, else the last.
        mActiveTab = llmin(index, (S32)mTabs.size() - 1);
        showCurrentTab();
    }
    else
    {
        if (index < mActiveTab)
        {
            --mActiveTab;
        }
        mNeedsRefresh = true;
    }
}

void SKFloaterInventoryExplorer::activateTab(S32 index)
{
    if (index < 0 || index >= (S32)mTabs.size() || index == mActiveTab)
    {
        return;
    }
    saveListState();
    mActiveTab = index;
    showCurrentTab();
}

std::string SKFloaterInventoryExplorer::folderPath(const LLUUID& folder_id) const
{
    std::vector<std::string> names;
    const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    while (cat)
    {
        names.push_back(cat->getName());
        cat = cat->getParentUUID().notNull() ? gInventory.getCategory(cat->getParentUUID()) : nullptr;
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        path += (path.empty() ? "" : PATH_SEPARATOR) + *it;
    }
    return path;
}

LLPointer<LLUIImage> SKFloaterInventoryExplorer::folderIcon(const LLUUID& folder_id) const
{
    const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    return LLUI::getUIImage(LLViewerFolderType::lookupIconName(cat ? cat->getPreferredType() : LLFolderType::FT_NONE, false));
}

void SKFloaterInventoryExplorer::refreshToolbar()
{
    const SKExplorerTab& tab = mTabs[mActiveTab];
    const LLViewerInventoryCategory* cat = gInventory.getCategory(tab.mFolderID);
    mBackBtn->setEnabled(!tab.mBack.empty());
    mForwardBtn->setEnabled(!tab.mForward.empty());
    mUpBtn->setEnabled(cat && cat->getParentUUID().notNull());
    mPathText->setText(folderPath(tab.mFolderID));
}

void SKFloaterInventoryExplorer::refreshTabStrip()
{
    std::vector<SKExplorerTabStrip::Tab> tabs;
    for (const SKExplorerTab& tab : mTabs)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(tab.mFolderID);
        SKExplorerTabStrip::Tab strip_tab;
        strip_tab.mTitle = cat ? cat->getName() : std::string();
        strip_tab.mToolTip = folderPath(tab.mFolderID);
        strip_tab.mIcon = folderIcon(tab.mFolderID);
        tabs.push_back(strip_tab);
    }
    mTabStrip->setTabs(tabs, mActiveTab);
}

std::string SKFloaterInventoryExplorer::countLabel(S32 folders, S32 items) const
{
    LLStringUtil::format_map_t args;
    args["[FOLDERS]"] = llformat("%d", folders);
    args["[ITEMS]"] = llformat("%d", items);
    std::string folders_text = getString(folders == 1 ? "one_folder" : "n_folders", args);
    std::string items_text = getString(items == 1 ? "one_item" : "n_items", args);
    if (folders && items)
    {
        LLStringUtil::format_map_t both;
        both["[FOLDERS]"] = folders_text;
        both["[ITEMS]"] = items_text;
        return getString("folders_and_items", both);
    }
    return folders ? folders_text : items_text;
}

void SKFloaterInventoryExplorer::refreshStatus()
{
    std::string text;
    const bool columns = isColumns();
    const S32 selected = columns ? mColumns->getSelectedCount() : mList->getSelectedCount();
    const S32 folders = columns ? mColumns->getFolderCount() : mList->getFolderCount();
    const S32 items = columns ? mColumns->getItemCount() : mList->getItemCount();
    const bool loading = columns ? mColumns->isLoading() : mList->isLoading();
    if (loading && folders + items == 0)
    {
        text = getString("loading");
    }
    else
    {
        text = countLabel(folders, items);
        if (selected > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", selected);
            LLStringUtil::format_map_t parts;
            parts["[A]"] = text;
            parts["[B]"] = getString("n_selected", args);
            text = getString("status_join", parts);
        }
    }
    if (mStatusText->getText() != text)
    {
        mStatusText->setText(text);
    }
}

void SKFloaterInventoryExplorer::refreshDetails()
{
    const uuid_vec_t selection = activeSelection();
    std::string text;
    if (selection.empty())
    {
        text = getString("no_selection");
    }
    else if (selection.size() > 1)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)selection.size());
        text = getString("n_selected", args);
    }
    else if (const LLInventoryObject* obj = gInventory.getObject(selection.front()))
    {
        // The per-type rows arrive with the details pane; the name stands in for them.
        text = obj->getName();
    }
    mDetailsText->setText(text);
}

void SKFloaterInventoryExplorer::onPaneDragStart(const std::string& pane)
{
    mPaneDragStartWidth = (pane == "sidebar" ? mSidebarPanel : mDetailsPanel)->getRect().getWidth();
}

void SKFloaterInventoryExplorer::onPaneDrag(const std::string& pane, S32 delta, bool done)
{
    // The sidebar grows to the right, the details pane to the left.
    const S32 width = mPaneDragStartWidth + (pane == "sidebar" ? delta : -delta);
    setPaneWidth(pane, width, done);
    if (done)
    {
        mPaneDragStartWidth = 0;
    }
}

void SKFloaterInventoryExplorer::onPaneReset(const std::string& pane)
{
    mPaneDragStartWidth = 0;
    setPaneWidth(pane, pane == "sidebar" ? SIDEBAR_DEFAULT : DETAILS_DEFAULT, true);
}

void SKFloaterInventoryExplorer::setPaneWidth(const std::string& pane, S32 width, bool save)
{
    const bool is_sidebar = pane == "sidebar";
    LLLayoutPanel* panel = is_sidebar ? mSidebarPanel : mDetailsPanel;
    LLLayoutPanel* other = is_sidebar ? mDetailsPanel : mSidebarPanel;
    const S32 floater_width = getRect().getWidth();
    const S32 other_width = other->getVisible() ? other->getRect().getWidth() : 0;
    // Min 140, max 45% of the floater, and the content keeps 200 while details show.
    S32 max_width = (S32)(floater_width * PANE_MAX_FRACTION);
    if (mDetailsPanel->getVisible())
    {
        max_width = llmin(max_width, floater_width - other_width - CONTENT_MIN - 12);
    }
    width = llclamp(width, PANE_MIN, llmax(PANE_MIN, max_width));
    panel->setTargetDim(width);
    if (save)
    {
        gSavedPerAccountSettings.setS32(is_sidebar ? "SkoomaInvExplorerSidebarWidth" : "SkoomaInvExplorerDetailsWidth", width);
    }
}

void SKFloaterInventoryExplorer::onToggleDetails()
{
    const bool shown = !mDetailsBtn->getToggleState();
    mDetailsBtn->setToggleState(shown);
    mDetailsPanel->setVisible(shown && !isColumns());
    mDetailsEdgePanel->setVisible(shown && !isColumns());
    mColumns->setDetailsShown(shown);
    gSavedPerAccountSettings.setBOOL("SkoomaInvExplorerDetailsShown", shown);
}

void SKFloaterInventoryExplorer::onListSortChanged()
{
    SKExplorerTab& tab = currentTab();
    tab.mSortKey = mList->getSortKey();
    tab.mSortAscending = mList->getSortAscending();
    gSavedPerAccountSettings.setString("SkoomaInvExplorerSortKey", SKInventoryListView::columnKey(tab.mSortKey));
    gSavedPerAccountSettings.setBOOL("SkoomaInvExplorerSortAscending", tab.mSortAscending);
}

void SKFloaterInventoryExplorer::showContextMenu(LLView* view, S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id)
{
    if (ids.empty())
    {
        // Empty space gives the folder's own menu.
        mMenu->setListedFolder(folder_id, true);
        mMenu->show(view, { folder_id }, x, y);
    }
    else
    {
        mMenu->setListedFolder(folder_id, false);
        mMenu->show(view, ids, x, y);
    }
}
