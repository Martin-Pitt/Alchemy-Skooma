/**
 * @file skinventorysidebar.cpp
 * @brief Inventory Explorer's sidebar: inventory panels that scroll as one
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

#include "skinventorysidebar.h"

#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llscrollcontainer.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

static LLDefaultChildRegistry::Register<SKInventorySidebarPanel> r_sk_sidebar_panel("sk_inventory_sidebar_panel");
static LLDefaultChildRegistry::Register<SKInventoryFavoritesPanel> r_sk_favorites_panel("sk_inventory_favorites_panel");
static LLDefaultChildRegistry::Register<SKInventorySidebar> r_sk_sidebar("sk_inventory_sidebar");

namespace
{
    const U32 SIDEBAR_SORT = LLInventoryFilter::SO_NAME | LLInventoryFilter::SO_FOLDERS_BY_NAME | LLInventoryFilter::SO_SYSTEM_FOLDERS_TO_TOP;

    bool is_marketplace_top(const LLViewerInventoryCategory* cat)
    {
        return cat && cat->getParentUUID() == gInventory.getRootFolderID()
            && (cat->getPreferredType() == LLFolderType::FT_MARKETPLACE_LISTINGS || cat->getPreferredType() == LLFolderType::FT_INBOX);
    }

    // The folder's ancestor directly under My Inventory, or null.
    LLViewerInventoryCategory* top_folder_of(const LLUUID& folder_id)
    {
        const LLUUID& root_id = gInventory.getRootFolderID();
        LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        while (cat && cat->getParentUUID().notNull() && cat->getParentUUID() != root_id)
        {
            cat = gInventory.getCategory(cat->getParentUUID());
        }
        return (cat && cat->getParentUUID() == root_id) ? cat : nullptr;
    }
}

//
// SKSidebarFolderView
//

SKSidebarFolderView::SKSidebarFolderView(const LLFolderView::Params& p)
:   LLFolderView(p),
    mFlat(false)
{
}

bool SKSidebarFolderView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mFlat)
    {
        // A press anywhere on a top-level row selects it; the arrow does not toggle.
        for (auto it = getFoldersBegin(); it != getFoldersEnd(); ++it)
        {
            LLFolderViewFolder* folder = *it;
            const LLRect& rect = folder->getRect();
            if (folder->getVisible() && rect.pointInRect(x, y))
            {
                if (getParentPanel())
                {
                    getParentPanel()->setFocus(true);
                }
                LLEditMenuHandler::gEditMenuHandler = this;
                return folder->LLFolderViewItem::handleMouseDown(x - rect.mLeft, y - rect.mBottom, mask);
            }
        }
    }
    return LLFolderView::handleMouseDown(x, y, mask);
}

bool SKSidebarFolderView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mFlat)
    {
        return true; // nothing expands in a flat column
    }
    return LLFolderView::handleDoubleClick(x, y, mask);
}

bool SKSidebarFolderView::handleKeyHere(KEY key, MASK mask)
{
    SKInventorySidebarPanel* panel = dynamic_cast<SKInventorySidebarPanel*>(getParentPanel());
    if (panel && panel->handleSidebarKey(key, mask))
    {
        return true;
    }
    return LLFolderView::handleKeyHere(key, mask);
}

//
// SKInventorySidebarPanel
//

SKInventorySidebarPanel::Params::Params()
:   section("section", "inventory")
{
}

SKInventorySidebarPanel::SKInventorySidebarPanel(const Params& p)
:   LLInventoryPanel(p),
    mSection(SECTION_INVENTORY)
{
}

void SKInventorySidebarPanel::initFromParams(const Params& p)
{
    const std::string& section = p.section();
    if (section == "favorites")
    {
        mSection = SECTION_FAVORITES;
    }
    else if (section == "marketplace")
    {
        mSection = SECTION_MARKETPLACE;
    }
    else if (section == "library")
    {
        mSection = SECTION_LIBRARY;
    }

    Params params(p);
    params.start_folder.id = (mSection == SECTION_LIBRARY) ? gInventory.getLibraryRootFolderID() : gInventory.getRootFolderID();
    params.show_root_folder = false;
    params.open_first_folder = false;
    params.use_marketplace_folders = (mSection == SECTION_MARKETPLACE);
    LLInventoryPanel::initFromParams(params);

    if (mSection == SECTION_MARKETPLACE)
    {
        // The base hides these two unless the stock outbox setting shows them; here they are the content.
        getFilter().setFilterCategoryTypes(getFilter().getFilterCategoryTypes()
                                           | (1ULL << LLFolderType::FT_INBOX) | (1ULL << LLFolderType::FT_MARKETPLACE_LISTINGS));
    }
    setSortOrder(SIDEBAR_SORT);
}

LLFolderView* SKInventorySidebarPanel::createFolderRoot(LLUUID root_id)
{
    // As the stock root, built as our subclass so the sidebar can take keys first.
    LLFolderView::Params base(mParams.folder_view);
    base.name = getName();
    base.title = getLabel();
    base.rect = LLRect(0, 0, getRect().getWidth(), 0);
    base.parent_panel = this;
    base.listener = mInvFVBridgeBuilder->createBridge(LLAssetType::AT_CATEGORY, LLAssetType::AT_CATEGORY, LLInventoryType::IT_CATEGORY,
                                                      this, &mInventoryViewModel, NULL, root_id);
    base.view_model = &mInventoryViewModel;
    base.grouped_item_model = mGroupedItemBridge;
    base.use_label_suffix = mParams.use_label_suffix;
    base.allow_multiselect = mAllowMultiSelect;
    base.allow_drag = mAllowDrag;
    base.show_empty_message = mShowEmptyMessage;
    base.suppress_folder_menu = mSuppressFolderMenu;
    base.show_item_link_overlays = mShowItemLinkOverlays;
    base.root = NULL;
    base.allow_drop = mParams.allow_drop_on_root;
    base.options_menu = "menu_inventory.xml";

    SKSidebarFolderView::Params p;
    static_cast<LLFolderView::Params&>(p).overwriteFrom(base);
    SKSidebarFolderView* fv = LLUICtrlFactory::create<SKSidebarFolderView>(p);
    fv->setCallbackRegistrar(&mCommitCallbackRegistrar);
    fv->setEnableRegistrar(&mEnableCallbackRegistrar);
    return fv;
}

bool SKInventorySidebarPanel::typedViewsFilter(const LLUUID& id, LLInventoryObject const* objectp)
{
    const LLViewerInventoryCategory* cat = dynamic_cast<const LLViewerInventoryCategory*>(objectp);
    if (!cat)
    {
        return false;
    }
    if (cat->getParentUUID().notNull() && cat->getParentUUID() == gInventory.getRootFolderID())
    {
        if (mSection == SECTION_INVENTORY)
        {
            return !is_marketplace_top(cat);
        }
        if (mSection == SECTION_MARKETPLACE)
        {
            return is_marketplace_top(cat);
        }
    }
    return true;
}

bool SKInventorySidebarPanel::covers(const LLUUID& folder_id) const
{
    switch (mSection)
    {
        case SECTION_LIBRARY:
            return folder_id != gInventory.getLibraryRootFolderID()
                && gInventory.isObjectDescendentOf(folder_id, gInventory.getLibraryRootFolderID());
        case SECTION_INVENTORY:
        case SECTION_MARKETPLACE:
        {
            LLViewerInventoryCategory* top = top_folder_of(folder_id);
            return top && (is_marketplace_top(top) == (mSection == SECTION_MARKETPLACE));
        }
        default:
            return false;
    }
}

S32 SKInventorySidebarPanel::getContentHeight()
{
    LLFolderView* root = getRootFolder();
    if (!root)
    {
        return 0;
    }
    // The span of the rows, wherever the root's own height has put them.
    S32 top = S32_MIN;
    S32 bottom = S32_MAX;
    for (LLView* child : *root->getChildList())
    {
        LLFolderViewItem* item = dynamic_cast<LLFolderViewItem*>(child);
        if (item && item->getVisible())
        {
            top = llmax(top, item->getRect().mTop);
            bottom = llmin(bottom, item->getRect().mBottom);
        }
    }
    return top > bottom ? top - bottom : 0;
}

//
// SKInventoryFavoritesPanel
//

SKInventoryFavoritesPanel::SKInventoryFavoritesPanel(const Params& p)
:   SKInventorySidebarPanel(p)
{
}

bool SKInventoryFavoritesPanel::covers(const LLUUID& folder_id) const
{
    return favoriteRootOf(folder_id).notNull();
}

LLUUID SKInventoryFavoritesPanel::favoriteRootOf(const LLUUID& folder_id) const
{
    LLUUID id = folder_id;
    while (id.notNull())
    {
        if (mRootContentIDs.contains(id))
        {
            return id;
        }
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        id = cat ? cat->getParentUUID() : LLUUID::null;
    }
    return LLUUID::null;
}

void SKInventoryFavoritesPanel::removeItemID(const LLUUID& id)
{
    if (mRootContentIDs.erase(id))
    {
        // A favourite gone from the roots may have favourited folders inside it.
        mBuildRootQueue.emplace_back(id);
    }
    LLInventoryPanel::removeItemID(id);
}

bool SKInventoryFavoritesPanel::isInRootContent(const LLUUID& id, LLFolderViewItem* view_item)
{
    return view_item->isFavorite() && mRootContentIDs.contains(id);
}

void SKInventoryFavoritesPanel::initRootContent()
{
    findAndInitRootContent(gInventory.getRootFolderID());
}

void SKInventoryFavoritesPanel::findAndInitRootContent(const LLUUID& id)
{
    if (mBuildViewsEndTime < LLTimer::getTotalSeconds())
    {
        mBuildRootQueue.emplace_back(id);
        return;
    }

    LLViewerInventoryCategory::cat_array_t* categories;
    LLViewerInventoryItem::item_array_t* items;
    mInventory->lockDirectDescendentArrays(id, categories, items);
    if (categories)
    {
        for (LLViewerInventoryCategory* cat : *categories)
        {
            if (cat->getPreferredType() == LLFolderType::FT_TRASH)
            {
                continue;
            }
            if (cat->getIsFavorite())
            {
                if (!getItemByID(cat->getUUID()))
                {
                    mRootContentIDs.emplace(cat->getUUID());
                    buildViewsTree(cat->getUUID(), cat->getParentUUID(), cat, nullptr, mFolderRoot.get(), BUILD_TIMELIMIT);
                }
            }
            else
            {
                findAndInitRootContent(cat->getUUID());
            }
        }
    }
    mInventory->unlockDirectDescendentArrays(id);
}

bool SKInventoryFavoritesPanel::removeFavorite(const LLUUID& id)
{
    if (!mRootContentIDs.erase(id))
    {
        return false;
    }
    LLFolderViewItem* view_item = getItemByID(id);
    if (view_item)
    {
        LLFolderViewFolder* parent = view_item->getParentFolder();
        LLInventoryPanel::removeItemID(id);
        view_item->destroyView();
        if (parent)
        {
            parent->getViewModelItem()->dirtyDescendantsFilter();
        }
    }
    return true;
}

void SKInventoryFavoritesPanel::itemChanged(const LLUUID& id, U32 mask, const LLInventoryObject* model_item)
{
    LLFolderViewItem* view_item = getItemByID(id);
    if (!model_item && !view_item)
    {
        return;
    }

    const LLViewerInventoryCategory* cat = dynamic_cast<const LLViewerInventoryCategory*>(model_item);
    bool handled = false;
    if (mask & (LLInventoryObserver::UPDATE_FAVORITE | LLInventoryObserver::STRUCTURE | LLInventoryObserver::ADD | LLInventoryObserver::REMOVE))
    {
        if (cat && cat->getIsFavorite() && cat->getPreferredType() != LLFolderType::FT_TRASH)
        {
            if (!view_item && favoriteRootOf(cat->getParentUUID()).isNull())
            {
                // Favourites inside the new one now show as its rows, not as roots.
                LLFavoritesCollector is_favorite;
                LLInventoryModel::cat_array_t cats;
                LLInventoryModel::item_array_t items;
                gInventory.collectDescendentsIf(id, cats, items, false, is_favorite);
                for (LLViewerInventoryCategory* inner : cats)
                {
                    removeFavorite(inner->getUUID());
                }
                mRootContentIDs.emplace(id);
                buildViewsTree(id, cat->getParentUUID(), cat, nullptr, mFolderRoot.get(), BUILD_ONE_FOLDER);
                handled = true;
            }
        }
        else if (removeFavorite(id))
        {
            if (cat)
            {
                mBuildRootQueue.emplace_back(id);
            }
            handled = true;
        }
    }

    if (!handled && (!model_item || model_item->getParentUUID().notNull()))
    {
        LLInventoryPanel::itemChanged(id, mask, model_item);
    }
}

//
// SKInventorySidebar
//

SKInventorySidebar::SKInventorySidebar(const Params& p)
:   LLPanel(p),
    mFavoritesHint(nullptr),
    mFlat(false),
    mRevealing(false),
    mPendingPanel(nullptr)
{
}

bool SKInventorySidebar::postBuild()
{
    static const char* const sections[] = { "favorites", "inventory", "marketplace", "library" };
    for (const char* section : sections)
    {
        std::string name(section);
        mHeadings.push_back(findChild<LLTextBox>("heading_" + name));
        SKInventorySidebarPanel* panel = findChild<SKInventorySidebarPanel>("panel_" + name);
        mPanels.push_back(panel);
        if (panel)
        {
            panel->setSelectCallback([this, panel](const std::deque<LLFolderViewItem*>& items, bool user_action)
                                     {
                                         onPanelSelect(panel, items, user_action);
                                     });
            panel->setKeyCallback([this](KEY key, MASK mask) { return mKeyCallback && mKeyCallback(key, mask); });
        }
    }
    mFavoritesHint = findChild<LLTextBox>("favorites_hint");
    return true;
}

void SKInventorySidebar::setFlat(bool flat)
{
    if (flat == mFlat)
    {
        return;
    }
    mFlat = flat;
    if (flat)
    {
        mOpenBeforeFlat.clear();
    }
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        SKSidebarFolderView* root = panel ? dynamic_cast<SKSidebarFolderView*>(panel->getRootFolder()) : nullptr;
        if (!root)
        {
            continue;
        }
        root->setFlat(flat);
        if (flat)
        {
            // Close what List had open, remembering it; nested open state stays in the closed folders.
            for (auto it = root->getFoldersBegin(); it != root->getFoldersEnd(); ++it)
            {
                LLFolderViewModelItemInventory* model = static_cast<LLFolderViewModelItemInventory*>((*it)->getViewModelItem());
                if ((*it)->isOpen() && model)
                {
                    mOpenBeforeFlat.push_back(model->getUUID());
                    (*it)->setOpen(false);
                }
            }
        }
        else
        {
            for (const LLUUID& id : mOpenBeforeFlat)
            {
                if (LLFolderViewItem* item = panel->getItemByID(id))
                {
                    item->setOpen(true);
                }
            }
        }
    }
}

void SKInventorySidebar::focusSelection()
{
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        LLFolderView* root = panel ? panel->getRootFolder() : nullptr;
        if (root && !root->getSelectionList().empty())
        {
            root->setFocus(true);
            return;
        }
    }
    if (!mPanels.empty() && mPanels[1] && mPanels[1]->getRootFolder())
    {
        mPanels[1]->getRootFolder()->setFocus(true);
    }
}

void SKInventorySidebar::draw()
{
    arrangePanels();
    scrollToPendingReveal();
    LLPanel::draw();
}

void SKInventorySidebar::arrangePanels()
{
    // Content height is known only to the folder views, so the stacking is done here.
    struct Row { LLView* view; S32 height; };
    std::vector<Row> rows;
    for (size_t i = 0; i < mPanels.size(); ++i)
    {
        SKInventorySidebarPanel* panel = mPanels[i];
        LLTextBox* heading = mHeadings[i];
        const S32 content = panel ? panel->getContentHeight() : 0;
        const bool is_favorites = panel && panel->getSection() == SKInventorySidebarPanel::SECTION_FAVORITES;
        const bool show_section = content > 0 || is_favorites;
        if (heading)
        {
            heading->setVisible(show_section);
            if (show_section)
            {
                rows.push_back({ heading, heading->getRect().getHeight() });
            }
        }
        if (is_favorites && mFavoritesHint)
        {
            mFavoritesHint->setVisible(content == 0);
            if (content == 0)
            {
                rows.push_back({ mFavoritesHint, mFavoritesHint->getRect().getHeight() });
            }
        }
        if (panel)
        {
            rows.push_back({ panel, content });
        }
    }

    S32 height = 0;
    for (const Row& row : rows)
    {
        height += row.height;
    }
    height = llmax(height, 1);

    LLRect rect = getRect();
    if (rect.getHeight() != height)
    {
        rect.mBottom = rect.mTop - height;
        setShape(rect);
    }

    const S32 width = rect.getWidth();
    S32 top = height;
    for (const Row& row : rows)
    {
        const S32 left = row.view->getRect().mLeft;
        const S32 right = dynamic_cast<SKInventorySidebarPanel*>(row.view) ? width : width - 4;
        LLRect shape(left, top, right, top - row.height);
        if (row.view->getRect() != shape)
        {
            row.view->setShape(shape);
        }
        top -= row.height;
    }
}

void SKInventorySidebar::reveal(const LLUUID& folder_id)
{
    SKInventorySidebarPanel* target = nullptr;
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        // Favorites comes first in the list, so a favourite's copy wins.
        if (panel && panel->covers(folder_id))
        {
            target = panel;
            break;
        }
    }

    mRevealing = true;
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        if (panel && panel != target)
        {
            panel->clearSelection();
        }
    }
    if (target)
    {
        target->setSelection(folder_id, false);
        mPendingReveal = folder_id;
        mPendingPanel = target;
    }
    mRevealing = false;
}

bool SKInventorySidebar::isRowInView(LLFolderViewItem* row) const
{
    const LLScrollContainer* scroller = dynamic_cast<const LLScrollContainer*>(getParent());
    if (!scroller || !row)
    {
        return false;
    }
    LLRect row_rect;
    row->localRectToOtherView(row->getLocalRect(), &row_rect, this);
    LLRect visible = const_cast<LLScrollContainer*>(scroller)->getVisibleContentRect();
    return visible.overlaps(row_rect);
}

void SKInventorySidebar::scrollToPendingReveal()
{
    if (mPendingReveal.isNull() || !mPendingPanel)
    {
        return;
    }
    LLFolderViewItem* row = mPendingPanel->getItemByID(mPendingReveal);
    if (!row || (mPendingPanel->getRootFolder() && mPendingPanel->getRootFolder()->needsArrange()))
    {
        return; // not built or not placed yet; try again next frame
    }

    bool visible = false;
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        if (panel && isRowInView(panel->getItemByID(mPendingReveal)))
        {
            visible = true;
            break;
        }
    }
    if (!visible)
    {
        LLScrollContainer* scroller = dynamic_cast<LLScrollContainer*>(getParent());
        if (scroller)
        {
            LLRect row_rect;
            row->localRectToOtherView(row->getLocalRect(), &row_rect, this);
            scroller->scrollToShowRect(row_rect);
        }
    }
    mPendingReveal.setNull();
    mPendingPanel = nullptr;
}

void SKInventorySidebar::onPanelSelect(SKInventorySidebarPanel* panel, const std::deque<LLFolderViewItem*>& items, bool user_action)
{
    if (mRevealing || !user_action || items.empty())
    {
        return;
    }
    LLFolderViewModelItemInventory* model = static_cast<LLFolderViewModelItemInventory*>(items.back()->getViewModelItem());
    if (!model)
    {
        return;
    }
    mRevealing = true;
    for (SKInventorySidebarPanel* other : mPanels)
    {
        if (other && other != panel)
        {
            other->clearSelection();
        }
    }
    mRevealing = false;
    if (mOpenCallback)
    {
        mOpenCallback(model->getUUID());
    }
}

LLInventoryPanel* SKInventorySidebar::getInventoryPanel() const
{
    for (SKInventorySidebarPanel* panel : mPanels)
    {
        if (panel->getSection() == SKInventorySidebarPanel::SECTION_INVENTORY)
        {
            return panel;
        }
    }
    return mPanels.empty() ? nullptr : mPanels.front();
}
