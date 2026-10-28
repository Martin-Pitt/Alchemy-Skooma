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
#include "llinventorypanel.h"
#include "lllocalcliprect.h"
#include "lllayoutstack.h"
#include "llrender2dutils.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "skinventorydetails.h"
#include "skinventoryexplorermenu.h"
#include "skinventorygalleryview.h"
#include "skinventorypathbar.h"
#include "skinventorysearchbox.h"
#include "skinventorytreeview.h"

static LLDefaultChildRegistry::Register<SKExplorerTabStrip> r_sk_tab_strip("sk_explorer_tab_strip");
static LLDefaultChildRegistry::Register<SKPaneEdge> r_sk_pane_edge("sk_pane_edge");

namespace
{
    constexpr S32 TAB_WIDTH = 180;
    constexpr S32 TAB_MIN_WIDTH = 60;
    constexpr S32 PLUS_WIDTH = 24;
    constexpr S32 BUTTONS_WIDTH = 40;   // the floater's Minimize and Close, kept clear of tabs
    constexpr S32 CLOSE_WIDTH = 16;
    constexpr S32 PANE_MIN = 140;
    constexpr F32 PANE_MAX_FRACTION = 0.45f;
    constexpr S32 CONTENT_MIN = 200;
    constexpr S32 SIDEBAR_DEFAULT = 220;
    constexpr S32 DETAILS_DEFAULT = 250;
    constexpr S32 SIZE_LABEL_WIDTH = 26; // the size slider label_width in XUI
    const char* const PATH_SEPARATOR = " \xE2\x80\xBA ";
    constexpr F32 SEARCH_DELAY_SHORT = 0.25f;   // a query under 3 characters
    constexpr F32 SEARCH_DELAY = 0.12f;
    constexpr F32 SEARCH_SHOW_SECONDS = 0.25f;
    constexpr F32 SEARCH_SORT_SECONDS = 1.f;      // a full sort after appended hits, at most once a second
    const char* const SAVED_SEARCHES = "SkoomaInvExplorerSavedSearches";

    std::string trimmed(std::string text)
    {
        LLStringUtil::trim(text);
        return text;
    }
}

//
// SKExplorerTabStrip
//

SKExplorerTabStrip::SKExplorerTabStrip(const Params& p)
:   LLUICtrl(p),
    mActive(0),
    mHover(-1),
    mDropTab(-1)
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
    const S32 room = getRect().getWidth() - PLUS_WIDTH - BUTTONS_WIDTH;
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

    if (mDropHover.isActive() && mDropTab >= 0 && mDropTab < (S32)mTabs.size())
    {
        SKInventoryExplorerDrag::drawTargetMark(LLRect(mDropTab * width, height, (mDropTab + 1) * width, 0), mDropVerdict);
    }

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
            make_ui_sound("UISndClickRelease");
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

bool SKExplorerTabStrip::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (tabAt(x) < 0 && !plusRect().pointInRect(x, y) && mDoubleClickCallback)
    {
        mDoubleClickCallback();
        return true;
    }
    return LLUICtrl::handleDoubleClick(x, y, mask);
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

bool SKExplorerTabStrip::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                           EAcceptance* accept, std::string& tooltip_msg)
{
    const S32 tab = tabAt(x);
    if (tab < 0)
    {
        return false;
    }
    SKInventoryExplorerDrag::dropOnFolder(mTabs[tab].mFolderID, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    mDropTab = tab;
    mDropVerdict = SKInventoryExplorerDrag::verdict();
    if (drop || LLToolDragAndDrop::getInstance()->getCargoIndex() != 0)
    {
        return true;
    }
    mDropHover.touch();
    S32 screen_x = 0;
    S32 screen_y = 0;
    localPointToScreen(x, y, &screen_x, &screen_y);
    LLUUID key;
    key.mData[0] = (U8)(tab + 1);
    if (mDropHover.spring(key, screen_x, screen_y, SKInventoryExplorerDrag::TAB_SPRING_SECONDS) && tab != mActive && mActivateCallback)
    {
        mActivateCallback(tab);
    }
    return true;
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
    mColumns(nullptr),
    mIcons(nullptr),
    mSortBtn(nullptr),
    mGroupBtn(nullptr),
    mSizeSlider(nullptr),
    mBackBtn(nullptr),
    mForwardBtn(nullptr),
    mUpBtn(nullptr),
    mDetailsBtn(nullptr),
    mPathBar(nullptr),
    mSearchBox(nullptr),
    mSearchClearBtn(nullptr),
    mSearchPending(false),
    mStatusText(nullptr),
    mDetails(nullptr),
    mGallery(nullptr),
    mGalleryStrip(nullptr),
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
    SKInventoryExplorerDrag::releaseOwner(this);
    SKInventoryExplorerContextMenu::closeOptionsMenu();
    if (mObserver)
    {
        gInventory.removeObserver(mObserver.get());
    }
}

bool SKFloaterInventoryExplorer::postBuild()
{
    mTabStrip = getChild<SKExplorerTabStrip>("tab_strip");
    mSidebar = getChild<SKInventoryTreeView>("sidebar");
    // One hidden stock panel, never drawn or focused, hosts the bridges the menus' stock actions run through.
    mSidebar->setBridgePanel(getChild<LLInventoryPanel>("bridge_panel"));
    std::map<std::string, std::string> tree_strings;
    for (const char* key : { "heading_favorites", "heading_inventory", "heading_marketplace", "heading_library", "favorites_hint",
                             "count_tooltip" })
    {
        tree_strings[key] = getString(std::string("sidebar_") + key);
    }
    tree_strings["remove_favorite"] = getString("tip_remove_favorite");
    mSidebar->setStrings(tree_strings);
    mList = getChild<SKInventoryListView>("list_view");
    mColumns = getChild<SKInventoryColumnsView>("columns_view");
    mIcons = getChild<SKInventoryIconsView>("icons_view");
    mSortBtn = getChild<LLButton>("sort_btn");
    mGroupBtn = getChild<LLButton>("group_btn");
    mSizeSlider = getChild<LLSliderCtrl>("size_slider");
    mBackBtn = getChild<LLButton>("back_btn");
    mForwardBtn = getChild<LLButton>("forward_btn");
    mUpBtn = getChild<LLButton>("up_btn");
    mDetailsBtn = getChild<LLButton>("details_btn");
    mPathBar = getChild<SKInventoryPathBar>("path_bar");
    mSearchBox = getChild<SKInventorySearchBox>("search_box");
    mSearchClearBtn = getChild<LLButton>("search_clear_btn");
    mStatusText = getChild<LLTextBox>("status_text");
    mDetails = getChild<SKInventoryDetailsView>("details_view");
    mGallery = getChild<SKInventoryGalleryView>("gallery_view");
    mGalleryStrip = mGallery->getStrip();
    mSidebarPanel = getChild<LLLayoutPanel>("sidebar_panel");
    mDetailsPanel = getChild<LLLayoutPanel>("details_panel");
    mDetailsEdgePanel = getChild<LLLayoutPanel>("details_edge_panel");

    mTabStrip->setNewTabToolTip(getString("new_tab_tooltip"));
    std::map<std::string, std::string> drag_strings;
    for (const char* key : { "drag_move", "drag_link", "drag_add_favorites", "drag_only_folders", "drag_already_favorites",
                             "drag_library_readonly", "drag_into_itself", "drag_system_folders", "drag_library_copies",
                             "drag_already_here", "drag_favorites_unavailable", "drag_links_off", "drag_link_own_only",
                             "drag_folder_link", "drag_no_link_here", "status_copied", "status_copied_one",
                             "status_moved_no_undo", "status_moved_no_undo_one", "status_moved", "status_moved_one", "status_linked", "status_linked_one",
                             "status_favorites_added", "status_favorites_removed", "status_undone", "status_cant_undo",
                             "drag_into_search" })
    {
        drag_strings[key] = getString(key);
    }
    SKInventoryExplorerDrag::setStrings(drag_strings);
    LLHandle<LLFloater> status_handle = getHandle();
    SKInventoryExplorerDrag::setStatusCallback(this, [status_handle](const std::string& text)
                                               {
                                                   if (SKFloaterInventoryExplorer* self = (SKFloaterInventoryExplorer*)status_handle.get())
                                                   {
                                                       self->mLastAction = text;
                                                       self->refreshStatus();
                                                   }
                                               });
    // The strip runs the full width; the floater's buttons sit on it, as on a header.
    for (const char* name : { "llfloater_close_btn", "llfloater_minimize_btn", "llfloater_collapse_btn", "llfloater_restore_btn" })
    {
        if (LLView* button = findChildView(name, false))
        {
            sendChildToFront(button);
        }
    }
    mTabStrip->setActivateCallback([this](S32 index) { activateTab(index); });
    mTabStrip->setCloseCallback([this](S32 index) { closeTab(index); });
    mTabStrip->setNewTabCallback([this]() { newTab(currentTab().mFolderID); });
    mTabStrip->setDoubleClickCallback([this]() { onHeaderDoubleClick(); });
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

    getChild<LLButton>("view_gallery_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setView(SKExplorerTab::VIEW_GALLERY); });
    getChild<LLButton>("view_icons_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setView(SKExplorerTab::VIEW_ICONS); });

    mOptions.mSet = [this](const std::string& option) { setOption(option); };
    mOptions.mCheck = [this](const std::string& option) { return checkOption(option); };
    // Group and Sort open their menus at the button's left edge, 2 px below.
    for (LLButton* button : { mSortBtn, mGroupBtn })
    {
        button->setCommitCallback([this, button](LLUICtrl*, const LLSD&)
                                  {
                                      SKInventoryExplorerContextMenu::showOptionsMenu(button == mSortBtn ? "menu_sk_inventory_explorer_sort.xml"
                                                                                                         : "menu_sk_inventory_explorer_group.xml",
                                                                                      mOptions, button, 0, -2);
                                  });
    }
    mSizeSlider->setValue((F32)llclamp(gSavedPerAccountSettings.getS32("SkoomaInvExplorerIconSize"), SKInventoryIconsView::MIN_TILE,
                                  SKInventoryIconsView::MAX_TILE));
    mSizeSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIconSizeChanged(); });
    mIcons->setTileSize((S32)mSizeSlider->getValueF32());

    const LLSD hidden = gSavedPerAccountSettings.getLLSD("SkoomaInvExplorerHiddenColumns");
    for (LLSD::array_const_iterator it = hidden.beginArray(); it != hidden.endArray(); ++it)
    {
        mHiddenColumns.insert(it->asString());
    }
    mList->setHiddenColumns(mHiddenColumns);
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
    for (const char* key : { "col_name", "col_folder", "col_type", "col_creator", "col_acquired", "col_perms", "col_worn", "col_description" })
    {
        labels.mColumns.push_back(getString(key));
    }
    labels.mFolderCount = getString("folder_count");
    labels.mFolder = getString("folder");
    labels.mDateFormat = getString("date_format");
    labels.mLoading = getString("loading");
    labels.mEmpty = getString("empty_folder");
    labels.mNoResults = getString("search_no_results");
    labels.mWorn = getString("worn");
    labels.mMore = getString("n_more");
    labels.mShowAll = getString("show_all");
    for (const char* key : { "folders", "objects", "clothing", "bodyparts", "textures", "sounds", "animations", "gestures", "landmarks",
                             "notecards", "scripts", "materials", "settings", "callingcards", "other", "today", "yesterday", "this_week",
                             "this_month", "earlier_this_year", "unknown", "system_folders", "no_brand", "full_perm", "copy_modify",
                             "copy_transfer", "copy_only", "no_copy", "worn", "not_worn", "you" })
    {
        labels.mGroups[key] = getString(std::string("group_") + key);
    }
    mColumns->setLabels(labels);
    std::map<std::string, std::string> details_strings;
    for (const char* key : { "no_selection", "n_selected", "loading", "link_suffix", "yes", "perm_full", "perm_modify", "perm_copy",
                             "perm_transfer", "row_folder", "row_type", "row_creator", "row_acquired", "row_you", "row_next_owner",
                             "row_worn", "row_attaches_to", "row_description", "row_resolution", "row_channels", "row_aspect",
                             "row_length", "row_format", "row_loops", "row_priority", "row_ease", "row_joints", "row_hand_pose",
                             "row_expression", "row_base_colour", "row_metallic", "row_roughness", "row_alpha_mode",
                             "row_double_sided", "row_textures", "row_location", "row_region", "row_parcel", "row_position",
                             "row_maturity", "row_slurl", "row_runtime", "row_experience", "section_image", "section_sound",
                             "section_animation", "section_material", "section_landmark", "channels_rgb", "channels_rgba",
                             "sound_format", "sound_mono", "sound_stereo", "sound_failed", "animation_failed", "material_failed",
                             "anim_loop", "anim_loops_no", "anim_loops_whole", "anim_loops_range", "anim_overrides", "region_body",
                             "region_hands", "region_face", "region_wings", "region_tail", "region_hind", "alpha_opaque",
                             "alpha_mask", "alpha_blend", "tex_base", "tex_normal", "tex_mr", "tex_emissive", "landmark_loads",
                             "tip_landmark_loads", "map_loading", "runtime_mono", "runtime_lso", "runtime_lsl_luau", "runtime_slua",
                             "runtime_lsl", "tip_runtime_unknown", "experience_no_cap", "player_locally", "player_inworld",
                             "player_stop", "tip_sound_local", "tip_sound_inworld", "tip_anim_local", "tip_anim_inworld",
                             "tip_aspect", "aspect_library", "aspect_no_modify", "aspect_too_long", "aspect_bad_chars", "gallery_by", "gallery_resolution",
                             "gallery_resolution_full" })
    {
        details_strings[key] = getString(std::string("details_") + key);
    }
    SKInventoryDetails::setStrings(details_strings, labels);
    // Esc in the Gallery leaves for the view it came from.
    mGalleryStrip->setEscapeCallback([this]() { setView(currentTab().mPrevView); });
    for (SKInventoryFlatView* view : { (SKInventoryFlatView*)mList, (SKInventoryFlatView*)mIcons, (SKInventoryFlatView*)mGalleryStrip })
    {
        view->setLabels(labels);
        view->setOpenCallback([this](const LLUUID& id) { openObject(id, false); });
        view->setUpCallback([this]() { goUp(); });
        view->setSelectionCallback([this]() { refreshDetails(); });
        view->setMenuCallback([this, view](S32 x, S32 y, const uuid_vec_t& ids) { showContextMenu(view, x, y, ids, view->getFolder()); });
    }
    mList->setSortCallback([this]() { onListSortChanged(); });
    mList->setHeaderMenuCallback([this](S32 x, S32 y)
                                 { SKInventoryExplorerContextMenu::showOptionsMenu("menu_sk_inventory_explorer_columns.xml", mOptions, mList, x, y); });

    mColumns->setMoveCallback([this](const LLUUID& folder_id) { navigateTo(folder_id, false); });
    mColumns->setOpenCallback([this](const LLUUID& id) { openObject(id, false); });
    mColumns->setUpCallback([this]() { goUp(); });
    // ← from the first column: the sidebar takes focus on the top-level folder's row.
    mColumns->setSidebarCallback([this]() { mSidebar->focusSelection(currentTab().mFolderID); });
    mColumns->setSelectionCallback([this]() { refreshDetails(); });
    mColumns->setMenuCallback([this](S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id)
                              { showContextMenu(mColumns, x, y, ids, folder_id); });

    mSidebar->setOpenCallback([this](const LLUUID& folder_id)
                              {
                                  // A saved search lasts until anything else in the sidebar is clicked.
                                  if (!mTabs.empty() && currentTab().mSearchSaved)
                                  {
                                      commitSearch(std::string(), false);
                                  }
                                  navigateTo(folder_id, true);
                              });
    mSidebar->setSavedSearchCallbacks([this](S32 index) { runSavedSearch(index); },
                                      [this](S32 index)
                                      {
                                          const LLSD list = gSavedPerAccountSettings.getLLSD(SAVED_SEARCHES);
                                          if (index >= 0 && index < list.size())
                                          {
                                              setSavedSearch(list[index]["query"].asString(), list[index]["folder"].asUUID(), false, true);
                                          }
                                      });
    mSidebar->setFocusContentCallback([this]() { focusContent(true); });
    // A sidebar row's menu is the folder's, as the content views give it.
    mSidebar->setMenuCallback([this](S32 x, S32 y, const LLUUID& folder_id)
                              {
                                  const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
                                  showContextMenu(mSidebar, x, y, { folder_id }, cat ? cat->getParentUUID() : LLUUID::null);
                              });

    mPathBar->setStrings(getString("path_arrow_tooltip"), getString("path_no_subfolders"));
    mPathBar->setNavigateCallback([this](const LLUUID& folder_id) { navigateTo(folder_id, true); });

    mSearchBox->setKeystrokeCallback([this](LLLineEditor*, void*) { onSearchTyped(); }, nullptr);
    mSearchClearBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
                                       {
                                           // The clear x keeps focus in the box.
                                           mSearchBox->setText(LLStringUtil::null);
                                           commitSearch(std::string(), false);
                                           mSearchBox->setFocus(true);
                                           mDropdown->refresh();
                                       });
    mDropdown = std::make_unique<SKSearchDropdown>(mSearchBox);
    std::map<std::string, std::string> search_strings;
    for (const char* key : { "title", "recent", "add_favorite", "remove_favorite", "everywhere", "in_folder", "remove_history",
                             "key_type", "key_brand", "key_creator", "key_perm", "key_is", "key_after", "key_before", "perm_full",
                             "perm_copy", "perm_nocopy", "perm_mod", "perm_nomod", "perm_trans", "perm_notrans", "is_worn", "is_link",
                             "is_favorite", "is_folder", "is_item", "date_1d", "date_7d", "date_30d", "date_365d", "date_jan1",
                             "date_today" })
    {
        search_strings[key] = getString(std::string("search_") + key);
    }
    mDropdown->setStrings(search_strings);
    mDropdown->setEditCallback([this]() { onSearchTyped(); });
    mDropdown->setFolderCallback([this]() { return mTabs.empty() ? LLUUID::null : currentTab().mFolderID; });
    mDropdown->setSavedCallbacks([this]()
                                 {
                                     return !mTabs.empty()
                                            && savedSearchIndex(trimmed(mSearchBox->getText()), currentTab().mFolderID) >= 0;
                                 },
                                 [this]() { toggleSavedSearch(); });
    mDropdown->setLeaveCallbacks([this]() { focusContent(true); }, [this]() { focusContent(false); });
    mSearchBox->setKeyHook([this](KEY key, MASK mask) { return mDropdown->handleKey(key, mask); });
    mSearchBox->setFocusCallback([this]() { mDropdown->refresh(); });
    mSearchBox->setClickCallback([this]() { mDropdown->refresh(); });
    mSidebar->setKeyCallback([this](KEY key, MASK mask) { return onSidebarKey(key, mask); });

    mMenu = std::make_unique<SKInventoryExplorerContextMenu>([this](const LLUUID& id, bool new_tab) { openObject(id, new_tab); },
                                                             [this](const LLUUID& id) { startRename(id); }, mOptions);
    // Resolved when an action runs, so a panel built late or never is no hazard here.
    LLHandle<LLFloater> handle = getHandle();
    SKInventoryExplorerActions::setBridgePanelGetter([handle]() -> LLInventoryPanel*
                                                     {
                                                         SKFloaterInventoryExplorer* floater =
                                                             static_cast<SKFloaterInventoryExplorer*>(handle.get());
                                                         return (floater && floater->mSidebar) ? floater->mSidebar->getInventoryPanel() : nullptr;
                                                     });
    std::map<std::string, std::string> menu_strings;
    for (const char* key : { "menu_title", "full_perm_only", "tip_cut", "tip_copy", "tip_paste", "tip_favorite", "tip_unfavorite", "tip_delete" })
    {
        menu_strings[key] = getString(std::string("menu_") + key);
    }
    mMenu->setStrings(menu_strings);
    mMenu->setCreatedCallback([handle](const LLUUID& id)
                              {
                                  if (SKFloaterInventoryExplorer* floater = static_cast<SKFloaterInventoryExplorer*>(handle.get()))
                                  {
                                      floater->mPendingRename = id;
                                      floater->mPendingRenameTimer.reset();
                                  }
                              });

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

void SKFloaterInventoryExplorer::onVisibilityChange(bool new_visibility)
{
    // The popups belong to no view, so they are closed with the floater.
    if (!new_visibility)
    {
        if (mDropdown)
        {
            mDropdown->hide();
        }
        if (mPathBar)
        {
            mPathBar->hideMenu();
        }
    }
    LLFloater::onVisibilityChange(new_visibility);
}

void SKFloaterInventoryExplorer::onClose(bool app_quitting)
{
    if (mDropdown)
    {
        mDropdown->hide();
    }
    if (mPathBar)
    {
        mPathBar->hideMenu();
    }
    LLFloater::onClose(app_quitting);
}

void SKFloaterInventoryExplorer::draw()
{
    // The typed text is searched once typing pauses: longer while it is short.
    if (mSearchPending)
    {
        const std::string text = trimmed(mSearchBox->getText());
        if (mSearchTyped.getElapsedTimeF32() >= (text.size() < 3 ? SEARCH_DELAY_SHORT : SEARCH_DELAY))
        {
            mSearchPending = false;
            commitSearch(text, false);
        }
    }
    stepSearch();
    mDropdown->update();
    // A new object is opened where it landed, selected, and renamed in place.
    if (mPendingRename.notNull() && !mTabs.empty())
    {
        const LLInventoryObject* obj = gInventory.getObject(mPendingRename);
        if (!obj || mPendingRenameTimer.getElapsedTimeF32() > 10.f)
        {
            mPendingRename.setNull();
        }
        else if (obj->getParentUUID() != currentTab().mFolderID || isSearching())
        {
            commitSearch(std::string(), false);
            mSearchBox->setText(LLStringUtil::null);
            navigateTo(obj->getParentUUID(), true);
        }
        else
        {
            SKInventoryFlatView* flat = flatView();
            if (flat ? flat->lists(mPendingRename) : mColumns->lists(mPendingRename))
            {
                const LLUUID id = mPendingRename;
                mPendingRename.setNull();
                setActiveSelection({ id });
                startRename(id);
            }
        }
    }
    mSearchClearBtn->setVisible(!mSearchBox->getText().empty());

    if (mNeedsRefresh)
    {
        mNeedsRefresh = false;
        refreshToolbar();
        refreshTabStrip();
        refreshDetails();
        mStatusSelected = -1;
    }
    // The status follows the view's counts, read only when they move.
    SKInventoryFlatView* flat = flatView();
    const U32 generation = flat ? flat->getGeneration() : mColumns->getGeneration();
    const S32 selected = flat ? flat->getSelectedCount() : mColumns->getSelectedCount();
    const bool search_status_changed = refreshSearchStatus();
    if (generation != mStatusGeneration || selected != mStatusSelected || search_status_changed)
    {
        if (selected != mStatusSelected && selected > 0)
        {
            mLastAction.clear();
        }
        mStatusGeneration = generation;
        mStatusSelected = selected;
        refreshStatus();
    }
    LLFloater::draw();

    // A tick on the size slider marks the default size.
    if (mSizeSlider->getVisible())
    {
        static LLUIColor tick_color = LLUIColorTable::instance().getColor("LtGray_50");
        static LLPointer<LLUIImage> thumb = LLUI::getUIImage("SliderThumb_Off");
        const LLRect rect = mSizeSlider->getRect();
        const S32 half = thumb.notNull() ? thumb->getWidth() / 2 : 4;
        const F32 t = (F32)(SKInventoryIconsView::DEFAULT_TILE - SKInventoryIconsView::MIN_TILE)
                    / (F32)(SKInventoryIconsView::MAX_TILE - SKInventoryIconsView::MIN_TILE);
        const S32 track_left = rect.mLeft + SIZE_LABEL_WIDTH;
        const S32 x = track_left + half + (S32)(t * (rect.mRight - track_left - 2 * half));
        gl_line_2d(x, rect.mBottom, x, rect.mBottom + 4, tick_color.get());
    }
}

void SKFloaterInventoryExplorer::onHeaderDoubleClick()
{
    // The floater has no header of its own, so the strip's empty space stands in for it: LLFloater's header
    // double-click restores a minimized floater and otherwise collapses it, where it can collapse.
    if (isMinimized())
    {
        setMinimized(false);
    }
    else if (LLView* collapse = findChildView("llfloater_collapse_btn", false); collapse && collapse->getVisible() && collapse->getEnabled())
    {
        setCollapsed(true);
    }
    // Asked of the button, not isMinimizeable(), which builds that replace minimize with collapse do not have.
    else if (LLView* minimize = findChildView("llfloater_minimize_btn", false); minimize && minimize->getVisible() && minimize->getEnabled())
    {
        setMinimized(true);
    }
}

bool SKFloaterInventoryExplorer::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // A double-click on the size slider gives the default size.
    if (mSizeSlider && mSizeSlider->getVisible() && mSizeSlider->getRect().pointInRect(x, y))
    {
        mSizeSlider->setValue((F32)SKInventoryIconsView::DEFAULT_TILE);
        onIconSizeChanged();
        return true;
    }
    return LLFloater::handleDoubleClick(x, y, mask);
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
            case 'Z':
                SKInventoryExplorerDrag::undo();
                return true;
            case 'F':
                mSearchBox->setFocus(true);
                mSearchBox->selectAll();
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
            case '2':
                setView(SKExplorerTab::VIEW_ICONS);
                return true;
            case '3':
                setView(SKExplorerTab::VIEW_LIST);
                return true;
            case '4':
                setView(SKExplorerTab::VIEW_GALLERY);
                return true;
            default:
                break;
        }
    }
    else if (mask == MASK_NONE && key == KEY_ESCAPE && !mTabs.empty() && currentTab().mView == SKExplorerTab::VIEW_GALLERY)
    {
        setView(currentTab().mPrevView);
        return true;
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
        | LLInventoryObserver::UPDATE_CREATE | LLInventoryObserver::UPDATE_FAVORITE;
    if (!(mask & RELEVANT) || mTabs.empty())
    {
        return;
    }

    for (SKInventoryFlatView* view : { (SKInventoryFlatView*)mList, (SKInventoryFlatView*)mIcons, (SKInventoryFlatView*)mGalleryStrip })
    {
        const LLUUID& folder_id = view->getFolder();
        bool affects_view = (mask & LLInventoryObserver::REBUILD) != 0;
        for (const LLUUID& id : gInventory.getChangedIDs())
        {
            const LLInventoryObject* obj = gInventory.getObject(id);
            if (id == folder_id || (obj && obj->getParentUUID() == folder_id) || view->lists(id))
            {
                affects_view = true;
                break;
            }
        }
        if (affects_view)
        {
            view->markDirty();
        }
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
    // A search checks the changed hits again and walks the changed folders, so it follows without starting over.
    if (mSearch.isActive())
    {
        mSearch.noteChanges(gInventory.getChangedIDs());
    }
    mDropdown->onInventoryChanged();

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
    // The sidebar follows only what changed.
    mSidebar->onInventoryChanged(gInventory.getChangedIDs(), mask);
    // The details read the item again when it changed: a rename, a description, an aspect.
    for (SKInventoryDetailsView* details : { mDetails, mColumns->getDetailsView() })
    {
        if (details->getItemID().notNull() && gInventory.getChangedIDs().contains(details->getItemID()))
        {
            details->refresh();
        }
    }
    if (mGallery->getItemID().notNull() && gInventory.getChangedIDs().contains(mGallery->getItemID()))
    {
        mGallery->refresh();
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
    else if (SKInventoryFlatView* flat = flatView())
    {
        tab.mSelection = flat->getSelection();
        tab.mListScroll = flat->getScroll();
    }
}

void SKFloaterInventoryExplorer::showCurrentTab()
{
    SKExplorerTab& tab = currentTab();
    const bool columns = isColumns();
    SKInventoryFlatView* flat = flatView();
    mList->setVisible(flat == mList);
    mIcons->setVisible(flat == mIcons);
    mColumns->setVisible(columns);
    // The Gallery takes the whole floater: no sidebar and no details pane.
    const bool gallery = flat == mGalleryStrip;
    mGallery->setVisible(gallery);
    mSidebarPanel->setVisible(!gallery);
    getChild<LLLayoutPanel>("sidebar_edge_panel")->setVisible(!gallery);
    getChild<LLButton>("view_gallery_btn")->setToggleState(gallery);
    // The size slider shows in Icons alone; its space stays.
    mSizeSlider->setVisible(flat == mIcons);
    getChild<LLButton>("view_columns_btn")->setToggleState(columns);
    getChild<LLButton>("view_icons_btn")->setToggleState(flat == mIcons);
    getChild<LLButton>("view_list_btn")->setToggleState(flat == mList);
    // Columns shows details as a column of its own, in place of the pane.
    const bool details = mDetailsBtn->getToggleState();
    mDetailsPanel->setVisible(details && !columns && !gallery);
    mDetailsEdgePanel->setVisible(details && !columns && !gallery);
    mSidebar->setFlat(columns);
    if (columns)
    {
        mColumns->setSort(tab.mSortKey, tab.mSortAscending);
        mColumns->setGroup(tab.mGroup);
        mColumns->setScrollState(tab.mStripScroll, tab.mColumnScrolls);
        mColumns->setFolder(tab.mFolderID, tab.mSelection);
    }
    else
    {
        flat->setSort(tab.mSortKey, tab.mSortAscending);
        flat->setGroup(tab.mGroup);
        if (!isSearching())
        {
            flat->clearSearch();
        }
        flat->setFolder(tab.mFolderID, tab.mSelection, tab.mListScroll);
    }
    mList->setSearching(isSearching() && flat == mList);
    // The box shows the tab's own query.
    mSearchPending = false;
    if (trimmed(mSearchBox->getText()) != tab.mSearch)
    {
        mSearchBox->setText(tab.mSearch);
    }
    applySearch();
    refreshSortAndGroupButtons();
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
        flatView()->setFolder(tab.mFolderID);
    }
    // A typed search stays while browsing, now under the new folder.
    applySearch();
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
    if (currentTab().mView != SKExplorerTab::VIEW_GALLERY)
    {
        currentTab().mPrevView = currentTab().mView;
    }
    currentTab().mView = view;
    gSavedPerAccountSettings.setS32("SkoomaInvExplorerView", (S32)view);
    showCurrentTab();
}

uuid_vec_t SKFloaterInventoryExplorer::activeSelection() const
{
    SKInventoryFlatView* flat = flatView();
    return flat ? flat->getSelection() : mColumns->getSelection();
}

void SKFloaterInventoryExplorer::setActiveSelection(const uuid_vec_t& ids)
{
    if (isColumns())
    {
        mColumns->setSelection(ids);
    }
    else
    {
        flatView()->setSelection(ids);
    }
}

bool SKFloaterInventoryExplorer::onSidebarKey(KEY key, MASK mask)
{
    // In Columns the sidebar is column 0, so -> steps into the first column.
    if (isColumns() && key == KEY_RIGHT && mask == MASK_NONE)
    {
        mColumns->focusFirstColumn();
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
    tab.mSortKey = SKInventoryListView::columnFromKey(gSavedPerAccountSettings.getString("SkoomaInvExplorerSortKey"));
    tab.mSortAscending = gSavedPerAccountSettings.getBOOL("SkoomaInvExplorerSortAscending");
    tab.mGroup = SKInventoryFlatView::groupFromKey(gSavedPerAccountSettings.getString("SkoomaInvExplorerGroup"));

    saveListState();
    if (!mTabs.empty())
    {
        make_ui_sound("UISndWindowOpen"); // as a floater opening
    }
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
    make_ui_sound("UISndWindowClose"); // as a floater closing
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
    mPathBar->setFolder(tab.mFolderID);
    refreshSavedSearches();
    // The placeholder names what the box searches.
    LLStringUtil::format_map_t args;
    args["[FOLDER]"] = cat ? cat->getName() : std::string();
    mSearchBox->setLabel(getString("search_placeholder", args));
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
        if (!tab.mSearch.empty())
        {
            // While searching: the search icon and the query, and where it searches.
            LLStringUtil::format_map_t args;
            args["[QUERY]"] = tab.mSearch;
            args["[PATH]"] = strip_tab.mToolTip;
            strip_tab.mTitle = tab.mSearch;
            strip_tab.mToolTip = getString("search_tab_tooltip", args);
            strip_tab.mIcon = LLUI::getUIImage("Search_Icon");
        }
        strip_tab.mFolderID = tab.mFolderID;
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
    const SKInventoryFlatView* flat = flatView();
    const S32 selected = flat ? flat->getSelectedCount() : mColumns->getSelectedCount();
    const S32 folders = flat ? flat->getFolderCount() : mColumns->getFolderCount();
    const S32 items = flat ? flat->getItemCount() : mColumns->getItemCount();
    const bool loading = flat ? flat->isLoading() : mColumns->isLoading();
    if (loading && folders + items == 0)
    {
        text = getString("loading");
    }
    else if (!mSearchStatus.empty())
    {
        LLStringUtil::format_map_t parts;
        parts["[A]"] = countLabel(folders, items);
        parts["[B]"] = mSearchStatus;
        text = getString("status_join", parts);
    }
    else
    {
        text = countLabel(folders, items);
        if (!mLastAction.empty() && selected == 0)
        {
            LLStringUtil::format_map_t parts;
            parts["[A]"] = text;
            parts["[B]"] = mLastAction;
            text = getString("status_join", parts);
        }
        else if (selected > 0)
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
    std::string path;
    if (isSearching() && selection.size() == 1)
    {
        if (const LLInventoryObject* obj = gInventory.getObject(selection.front()))
        {
            path = folderPath(obj->getParentUUID());
        }
    }
    mDetails->setSelection(selection, path);
    // The stage shows the selection, or the folder's first item while nothing is selected.
    if (flatView() == mGalleryStrip)
    {
        mGallery->setItem(selection.size() == 1 ? selection.front() : (selection.empty() ? mGalleryStrip->firstItem() : LLUUID::null));
    }
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
    const bool pane = shown && !isColumns() && flatView() != mGalleryStrip;
    mDetailsPanel->setVisible(pane);
    mDetailsEdgePanel->setVisible(pane);
    mColumns->setDetailsShown(shown);
    gSavedPerAccountSettings.setBOOL("SkoomaInvExplorerDetailsShown", shown);
}

void SKFloaterInventoryExplorer::onListSortChanged()
{
    SKExplorerTab& tab = currentTab();
    tab.mSortKey = mList->getSortKey();
    tab.mSortAscending = mList->getSortAscending();
    saveSortAndGroup();
}

void SKFloaterInventoryExplorer::saveSortAndGroup()
{
    const SKExplorerTab& tab = currentTab();
    gSavedPerAccountSettings.setString("SkoomaInvExplorerSortKey", SKInventoryFlatView::columnKey(tab.mSortKey));
    gSavedPerAccountSettings.setBOOL("SkoomaInvExplorerSortAscending", tab.mSortAscending);
    gSavedPerAccountSettings.setString("SkoomaInvExplorerGroup", SKInventoryFlatView::groupKey(tab.mGroup));
    refreshSortAndGroupButtons();
}

void SKFloaterInventoryExplorer::refreshSortAndGroupButtons()
{
    const SKExplorerTab& tab = currentTab();
    LLStringUtil::format_map_t sort_args;
    sort_args["[KEY]"] = getString(std::string("col_") + SKInventoryFlatView::columnKey(tab.mSortKey));
    sort_args["[DIR]"] = getString(tab.mSortAscending ? "sort_up" : "sort_down");
    mSortBtn->setLabel(getString("sort_label", sort_args));
    LLStringUtil::format_map_t group_args;
    group_args["[GROUP]"] = getString(std::string("group_by_") + SKInventoryFlatView::groupKey(tab.mGroup));
    mGroupBtn->setLabel(getString("group_label", group_args));
    // Lit while grouping.
    mGroupBtn->setToggleState(tab.mGroup != SKInventoryFlatView::GROUP_NONE);
}

SKInventoryFlatView* SKFloaterInventoryExplorer::flatView() const
{
    if (mTabs.empty())
    {
        return nullptr;
    }
    switch (mTabs[mActiveTab].mView)
    {
        case SKExplorerTab::VIEW_ICONS: return mIcons;
        case SKExplorerTab::VIEW_LIST:  return mList;
        case SKExplorerTab::VIEW_GALLERY: return mGalleryStrip;
        default:                        return isSearching() ? mList : nullptr;
    }
}

void SKFloaterInventoryExplorer::applySortAndGroup()
{
    const SKExplorerTab& tab = currentTab();
    if (SKInventoryFlatView* flat = flatView())
    {
        flat->setSort(tab.mSortKey, tab.mSortAscending);
        flat->setGroup(tab.mGroup);
    }
    else
    {
        mColumns->setSort(tab.mSortKey, tab.mSortAscending);
        mColumns->setGroup(tab.mGroup);
    }
    saveSortAndGroup();
}

void SKFloaterInventoryExplorer::setOption(const std::string& option)
{
    const size_t colon = option.find(':');
    if (colon == std::string::npos || mTabs.empty())
    {
        return;
    }
    const std::string kind = option.substr(0, colon);
    const std::string value = option.substr(colon + 1);
    SKExplorerTab& tab = currentTab();
    if (kind == "sort")
    {
        // A new key starts descending for Acquired, ascending otherwise.
        const SKInventoryFlatView::EColumn key = SKInventoryFlatView::columnFromKey(value);
        if (key != tab.mSortKey)
        {
            tab.mSortKey = key;
            tab.mSortAscending = key != SKInventoryFlatView::COL_ACQUIRED;
        }
        applySortAndGroup();
    }
    else if (kind == "dir")
    {
        tab.mSortAscending = value == "asc";
        applySortAndGroup();
    }
    else if (kind == "group")
    {
        tab.mGroup = SKInventoryFlatView::groupFromKey(value);
        applySortAndGroup();
    }
    else if (kind == "view")
    {
        if (value == "columns")
        {
            setView(SKExplorerTab::VIEW_COLUMNS);
        }
        else if (value == "icons")
        {
            setView(SKExplorerTab::VIEW_ICONS);
        }
        else if (value == "list")
        {
            setView(SKExplorerTab::VIEW_LIST);
        }
    }
    else if (kind == "col")
    {
        if (!mHiddenColumns.erase(value))
        {
            mHiddenColumns.insert(value);
        }
        mList->setHiddenColumns(mHiddenColumns);
        LLSD hidden = LLSD::emptyArray();
        for (const std::string& key : mHiddenColumns)
        {
            hidden.append(key);
        }
        gSavedPerAccountSettings.setLLSD("SkoomaInvExplorerHiddenColumns", hidden);
    }
}

bool SKFloaterInventoryExplorer::checkOption(const std::string& option) const
{
    const size_t colon = option.find(':');
    if (colon == std::string::npos || mTabs.empty())
    {
        return false;
    }
    const std::string kind = option.substr(0, colon);
    const std::string value = option.substr(colon + 1);
    const SKExplorerTab& tab = mTabs[mActiveTab];
    if (kind == "sort")
    {
        return value == SKInventoryFlatView::columnKey(tab.mSortKey);
    }
    if (kind == "dir")
    {
        return tab.mSortAscending == (value == "asc");
    }
    if (kind == "group")
    {
        return value == SKInventoryFlatView::groupKey(tab.mGroup);
    }
    if (kind == "view")
    {
        static const char* const VIEWS[] = { "columns", "icons", "list", "gallery" };
        return value == VIEWS[llclamp((S32)tab.mView, 0, 3)];
    }
    if (kind == "col")
    {
        return !mHiddenColumns.contains(value);
    }
    return false;
}

void SKFloaterInventoryExplorer::startRename(const LLUUID& id)
{
    if (SKInventoryFlatView* flat = flatView())
    {
        flat->startRename(id);
    }
    else
    {
        mColumns->startRename(id);
    }
}

void SKFloaterInventoryExplorer::onIconSizeChanged()
{
    // Snaps to the default within 5 px of it.
    S32 size = (S32)mSizeSlider->getValueF32();
    if (std::abs(size - SKInventoryIconsView::DEFAULT_TILE) <= 5)
    {
        size = SKInventoryIconsView::DEFAULT_TILE;
        mSizeSlider->setValue((F32)size);
    }
    mIcons->setTileSize(size);
    gSavedPerAccountSettings.setS32("SkoomaInvExplorerIconSize", size);
}

void SKFloaterInventoryExplorer::showContextMenu(LLView* view, S32 x, S32 y, const uuid_vec_t& ids, const LLUUID& folder_id)
{
    // Search results offer Show in Folder.
    if (isSearching())
    {
        mMenu->setShowInFolderCallback([this](const LLUUID& id) { showInFolder(id); });
    }
    else
    {
        mMenu->setShowInFolderCallback(nullptr);
    }
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

void SKFloaterInventoryExplorer::onSearchTyped()
{
    if (mTabs.empty())
    {
        return;
    }
    mDropdown->onTyped();
    // The caret moving changes nothing; an edit waits for typing to pause, and makes a saved search a typed one.
    if (!mSearchPending && trimmed(mSearchBox->getText()) == currentTab().mSearch)
    {
        return;
    }
    currentTab().mSearchSaved = false;
    mSearchPending = true;
    mSearchTyped.reset();
}

void SKFloaterInventoryExplorer::commitSearch(const std::string& text, bool saved)
{
    if (mTabs.empty())
    {
        return;
    }
    mSearchPending = false;
    SKExplorerTab& tab = currentTab();
    tab.mSearchSaved = saved && !text.empty();
    mNeedsRefresh = true;
    if (tab.mSearch == text)
    {
        return;
    }
    // Starting or ending a search swaps Columns for the flat List, so the state is saved first.
    const bool mode_changes = tab.mSearch.empty() != text.empty();
    if (mode_changes)
    {
        saveListState();
    }
    tab.mSearch = text;
    if (mode_changes)
    {
        tab.mSelection.clear();
        tab.mListScroll = 0;
        showCurrentTab();
    }
    else
    {
        applySearch();
    }
}

void SKFloaterInventoryExplorer::applySearch()
{
    SKInventoryFlatView* flat = flatView();
    if (!isSearching() || !flat)
    {
        mSearch.stop();
        return;
    }
    const SKExplorerTab& tab = currentTab();
    if (!mSearch.isActive() || mSearch.folder() != tab.mFolderID || mSearch.text() != tab.mSearch)
    {
        mSearch.start(tab.mFolderID, tab.mSearch);
    }
    pushSearchResults();
}

void SKFloaterInventoryExplorer::stepSearch()
{
    if (!mSearch.isActive())
    {
        return;
    }
    mSearch.step();
    SKInventoryFlatView* flat = flatView();
    if (!flat || !isSearching() || mSearchShown.getElapsedTimeF32() < SEARCH_SHOW_SECONDS)
    {
        return;
    }
    const std::vector<SKInventorySearch::hit_t>& hits = mSearch.hits();
    if (mSearch.getRemovals() != mSearchShownRemovals)
    {
        // Hits left or changed rank: the list is read again whole.
        pushSearchResults();
    }
    else if (hits.size() > mSearchShownCount)
    {
        // While the walk runs, new hits are added at the end; the list sorts once the walk ends.
        flat->appendSearchResults(std::vector<SKInventorySearch::hit_t>(hits.begin() + mSearchShownCount, hits.end()));
        mSearchShownCount = hits.size();
        mSearchUnsorted = true;
        mSearchShown.reset();
    }
    else if (mSearchUnsorted && mSearch.isDone() && mSearchSorted.getElapsedTimeF32() >= SEARCH_SORT_SECONDS)
    {
        pushSearchResults();
    }
}

void SKFloaterInventoryExplorer::pushSearchResults()
{
    mSearchShown.reset();
    mSearchSorted.reset();
    mSearchUnsorted = false;
    mSearchShownCount = mSearch.hits().size();
    mSearchShownRemovals = mSearch.getRemovals();
    if (SKInventoryFlatView* flat = flatView(); flat && isSearching())
    {
        flat->setSearchResults(mSearch.hits());
    }
}

bool SKFloaterInventoryExplorer::refreshSearchStatus()
{
    const U32 key = isSearching() ? mSearch.getStateGeneration() : U32_MAX - 1;
    if (key == mSearchStatusKey)
    {
        return false;
    }
    mSearchStatusKey = key;
    mSearchStatus.clear();
    if (!isSearching())
    {
        return true;
    }
    const std::string flagged = mSearch.query().flaggedTerm();
    if (!flagged.empty())
    {
        LLStringUtil::format_map_t args;
        args["[TERM]"] = flagged;
        mSearchStatus = getString("search_flagged", args);
    }
    else if (mSearch.isActive() && mSearch.getLoadingCount() > 0)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", mSearch.getLoadingCount());
        mSearchStatus = getString("search_loading", args);
    }
    else if (mSearch.isActive() && !mSearch.isDone())
    {
        mSearchStatus = getString("search_walking");
    }
    return true;
}

void SKFloaterInventoryExplorer::showInFolder(const LLUUID& id)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj || mTabs.empty())
    {
        return;
    }
    const LLUUID parent_id = obj->getParentUUID();
    // Clears the search, opens the folder, selects the object and scrolls it to the middle.
    mSearchBox->setText(LLStringUtil::null);
    commitSearch(std::string(), false);
    navigateTo(parent_id, true);
    setActiveSelection({ id });
    if (SKInventoryFlatView* flat = flatView())
    {
        flat->centerSelection();
    }
    focusContent(false);
}

void SKFloaterInventoryExplorer::focusContent(bool select_first)
{
    mDropdown->hide();
    if (SKInventoryFlatView* flat = flatView())
    {
        flat->focusContent(select_first);
    }
    else if (select_first)
    {
        mColumns->focusFirstColumn();
    }
    else
    {
        mColumns->setFocus(true);
    }
}

S32 SKFloaterInventoryExplorer::savedSearchIndex(const std::string& query, const LLUUID& folder_id) const
{
    if (query.empty())
    {
        return -1;
    }
    // Both sides in saved form, so "everywhere" and a deleted folder compare alike.
    const LLUUID wanted = savedFolder(folder_id);
    const LLSD list = gSavedPerAccountSettings.getLLSD(SAVED_SEARCHES);
    for (S32 i = 0; i < list.size(); ++i)
    {
        if (list[i]["query"].asString() == query && savedFolder(list[i]["folder"].asUUID()) == wanted)
        {
            return i;
        }
    }
    return -1;
}

LLUUID SKFloaterInventoryExplorer::savedFolder(const LLUUID& folder_id)
{
    if (folder_id.isNull() || folder_id == gInventory.getRootFolderID() || !gInventory.getCategory(folder_id))
    {
        return LLUUID::null;
    }
    return folder_id;
}

void SKFloaterInventoryExplorer::toggleSavedSearch()
{
    const std::string query = trimmed(mSearchBox->getText());
    if (query.empty() || mTabs.empty())
    {
        return;
    }
    const LLUUID& folder_id = currentTab().mFolderID;
    setSavedSearch(query, folder_id, savedSearchIndex(query, folder_id) < 0, true);
}

void SKFloaterInventoryExplorer::setSavedSearch(const std::string& query, const LLUUID& folder_id, bool saved, bool record)
{
    const S32 index = savedSearchIndex(query, folder_id);
    LLSD list = gSavedPerAccountSettings.getLLSD(SAVED_SEARCHES);
    if (!list.isArray())
    {
        list = LLSD::emptyArray();
    }
    if (saved && index < 0)
    {
        LLSD entry;
        entry["query"] = query;
        // Searches of My Inventory itself are saved as everywhere.
        entry["folder"] = savedFolder(folder_id);
        list.append(entry);
    }
    else if (!saved && index >= 0)
    {
        LLSD kept = LLSD::emptyArray();
        for (S32 i = 0; i < list.size(); ++i)
        {
            if (i != index)
            {
                kept.append(list[i]);
            }
        }
        list = kept;
    }
    else
    {
        return;
    }
    gSavedPerAccountSettings.setLLSD(SAVED_SEARCHES, list);
    refreshSavedSearches();
    if (record)
    {
        LLHandle<LLFloater> handle = getHandle();
        SKInventoryExplorerDrag::recordAction([handle, query, folder_id, saved]()
                                              {
                                                  if (SKFloaterInventoryExplorer* self = (SKFloaterInventoryExplorer*)handle.get())
                                                  {
                                                      self->setSavedSearch(query, folder_id, !saved, false);
                                                  }
                                              },
                                              getString(saved ? "status_search_saved" : "status_search_removed"));
    }
}

void SKFloaterInventoryExplorer::runSavedSearch(S32 index)
{
    const LLSD list = gSavedPerAccountSettings.getLLSD(SAVED_SEARCHES);
    if (index < 0 || index >= list.size() || mTabs.empty())
    {
        return;
    }
    const std::string query = list[index]["query"].asString();
    LLUUID folder_id = list[index]["folder"].asUUID();
    if (folder_id.isNull() || !gInventory.getCategory(folder_id))
    {
        folder_id = gInventory.getRootFolderID();
    }
    mSearchBox->setText(query);
    commitSearch(query, true);
    navigateTo(folder_id, true);
}

void SKFloaterInventoryExplorer::refreshSavedSearches()
{
    if (!mSidebar || mTabs.empty())
    {
        return;
    }
    const SKExplorerTab& tab = currentTab();
    const LLSD list = gSavedPerAccountSettings.getLLSD(SAVED_SEARCHES);
    std::vector<SKInventoryTreeView::SavedSearch> rows;
    S32 selected = -1;
    for (S32 i = 0; i < list.size(); ++i)
    {
        SKInventoryTreeView::SavedSearch row;
        row.mQuery = list[i]["query"].asString();
        const LLUUID folder_id = list[i]["folder"].asUUID();
        const LLViewerInventoryCategory* cat = folder_id.notNull() ? gInventory.getCategory(folder_id) : nullptr;
        if (cat && folder_id != gInventory.getRootFolderID())
        {
            LLStringUtil::format_map_t args;
            args["[FOLDER]"] = cat->getName();
            row.mSuffix = getString("saved_search_suffix", args);
            row.mToolTip = getString("saved_search_in", args);
        }
        else
        {
            row.mToolTip = getString("saved_search_everywhere");
        }
        // Selected while both its folder and its query are the tab's.
        const LLUUID runs_in = cat ? folder_id : gInventory.getRootFolderID();
        if (selected < 0 && row.mQuery == tab.mSearch && runs_in == tab.mFolderID)
        {
            selected = i;
        }
        rows.push_back(row);
    }
    mSidebar->setSavedSearches(rows, selected);
}
