/**
 * @file skinventorytreeview.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skinventorytreeview.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llwindow.h"

static LLDefaultChildRegistry::Register<SKInventoryTreeView> r_sk_inventory_tree("sk_inventory_tree");

namespace
{
    // The prototype's .side and .side h4: 4 px above the first heading, 8 px above each, 2 px below.
    constexpr S32 TOP_PAD = 4;
    constexpr S32 HEADING_GAP = 8;
    constexpr S32 HEADING_TEXT = 12;
    constexpr S32 HEADING_BELOW = 2;
    constexpr S32 HEADING_SIDE = 8;
    constexpr S32 HINT_HEIGHT = 32;
    // Rows: the icon at 20 px plus 16 px a level, the arrow hanging left of it.
    constexpr S32 INDENT_LEFT = 20;
    constexpr S32 INDENT_STEP = 16;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 ARROW_SIZE = 12;
    constexpr S32 WHEEL_ROWS = 3;
    constexpr F32 TYPE_AHEAD_SECONDS = 1.f;

    bool is_marketplace_top(const LLViewerInventoryCategory* cat)
    {
        return cat && cat->getParentUUID() == gInventory.getRootFolderID()
            && (cat->getPreferredType() == LLFolderType::FT_MARKETPLACE_LISTINGS || cat->getPreferredType() == LLFolderType::FT_INBOX);
    }

    bool is_system(const LLViewerInventoryCategory* cat)
    {
        return cat && LLFolderType::lookupIsProtectedType(cat->getPreferredType());
    }

    // System folders first, then by name.
    bool folder_less(const LLViewerInventoryCategory* a, const LLViewerInventoryCategory* b)
    {
        const bool sa = is_system(a);
        const bool sb = is_system(b);
        if (sa != sb)
        {
            return sa;
        }
        return LLStringUtil::compareDict(a->getName(), b->getName()) < 0;
    }

    // A folder's subfolders, sorted; false while the folder is not fetched.
    bool subfolders(const LLUUID& folder_id, std::vector<LLViewerInventoryCategory*>& out)
    {
        out.clear();
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, cats, items);
        if (cats)
        {
            for (LLViewerInventoryCategory* cat : *cats)
            {
                out.push_back(cat);
            }
        }
        std::sort(out.begin(), out.end(), folder_less);
        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        return cat && cat->getVersion() != LLViewerInventoryCategory::VERSION_UNKNOWN;
    }

    std::string compact(S32 count)
    {
        if (count < 1000)
        {
            return llformat("%d", count);
        }
        if (count < 10000)
        {
            return llformat("%.1fk", count / 1000.f);
        }
        return llformat("%dk", count / 1000);
    }
}

SKInventoryTreeView::SKInventoryTreeView(const Params& p)
:   LLUICtrl(p),
    mDirty(true),
    mLaidOut(false),
    mContentHeight(0),
    mFavoritesWalked(false),
    mSavedSelected(-1),
    mFlat(false),
    mHover(-1),
    mHoverArrow(false),
    mPressIndex(-1),
    mPressX(0),
    mPressY(0),
    mScrollbar(nullptr),
    mBridgePanel(nullptr)
{
    LLScrollbar::Params sbparams;
    sbparams.name("tree_scrollbar");
    sbparams.rect(LLRect(getRect().getWidth() - LLScrollbar::THIN_THICKNESS, getRect().getHeight(), getRect().getWidth(), 0));
    sbparams.orientation(LLScrollbar::VERTICAL);
    sbparams.doc_size(0);
    sbparams.doc_pos(0);
    sbparams.page_size(1);
    sbparams.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
    sbparams.visible(false);
    sbparams.thin(true);
    mScrollbar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
    addChild(mScrollbar);
}

SKInventoryTreeView::~SKInventoryTreeView()
{
}

const std::string& SKInventoryTreeView::str(const std::string& key) const
{
    auto it = mStrings.find(key);
    return it != mStrings.end() ? it->second : LLStringUtil::null;
}

void SKInventoryTreeView::dropCounts(const LLUUID& id)
{
    // Up the model's chain, and up the chain the folder had when it was counted.
    for (LLUUID cur = id; cur.notNull();)
    {
        mCounts.erase(cur);
        auto known = mCountedParents.find(cur);
        const LLUUID old_parent = known != mCountedParents.end() ? known->second : LLUUID::null;
        const LLInventoryObject* obj = gInventory.getObject(cur);
        const LLUUID parent = obj ? obj->getParentUUID() : LLUUID::null;
        if (old_parent.notNull() && old_parent != parent)
        {
            dropCounts(old_parent);
        }
        cur = parent.notNull() ? parent : old_parent;
        if (cur == old_parent && old_parent.notNull() && !obj)
        {
            mCountedParents.erase(known);
        }
    }
}

void SKInventoryTreeView::onInventoryChanged(const std::set<LLUUID>& changed_ids, U32 mask)
{
    if (mask & LLInventoryObserver::REBUILD)
    {
        // The model was rebuilt: everything is read again, the favourites once more with it.
        mFavoriteSet.clear();
        mFavoritesWalked = false;
        mCounts.clear();
        mCountedParents.clear();
        mDirty = true;
        return;
    }
    const LLUUID& root_id = gInventory.getRootFolderID();
    const LLUUID& library_id = gInventory.getLibraryRootFolderID();
    bool favorites_changed = false;
    for (const LLUUID& id : changed_ids)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        const LLInventoryObject* obj = cat ? cat : gInventory.getObject(id);

        // Favourites: added, removed, renamed or moved, and nothing else.
        if (mFavoritesWalked)
        {
            const bool known = mFavoriteSet.count(id) > 0;
            const bool favorite = cat && cat->getIsFavorite()
                                  && !gInventory.isObjectDescendentOf(id, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH));
            if (favorite && !known)
            {
                mFavoriteSet.insert(id);
                favorites_changed = true;
            }
            else if (!favorite && known)
            {
                mFavoriteSet.erase(id);
                favorites_changed = true;
            }
            else if (known)
            {
                favorites_changed = true; // its name or place may have changed
            }
        }

        // Counts: only the chains of folders that were counted.
        if (!mCounts.empty())
        {
            dropCounts(id);
        }

        // Rows: only where a changed object is a row, or sits in a folder a row shows or a section lists.
        const LLUUID parent = obj ? obj->getParentUUID() : LLUUID::null;
        if (mRowIDs.count(id) || (parent.notNull() && (mRowIDs.count(parent) || parent == root_id || parent == library_id)))
        {
            mDirty = true;
        }
    }
    if (favorites_changed)
    {
        sortFavorites();
        mDirty = true;
    }
}

void SKInventoryTreeView::setSavedSearches(const std::vector<SavedSearch>& searches, S32 selected)
{
    const bool same = searches.size() == mSavedSearches.size()
                      && std::equal(searches.begin(), searches.end(), mSavedSearches.begin(), [](const SavedSearch& a, const SavedSearch& b)
                                    { return a.mQuery == b.mQuery && a.mSuffix == b.mSuffix && a.mToolTip == b.mToolTip; });
    if (same && selected == mSavedSelected)
    {
        return;
    }
    mSavedSearches = searches;
    mSavedSelected = selected;
    if (selected >= 0)
    {
        mSelectedKey = llformat("s/%d", selected);
        mSelectedID.setNull();
    }
    else if (mSelectedKey.rfind("s/", 0) == 0)
    {
        mSelectedKey.clear();
    }
    mDirty = true;
}

void SKInventoryTreeView::setFlat(bool flat)
{
    if (flat == mFlat)
    {
        return;
    }
    mFlat = flat;
    // Columns closes what List had open, and List opens it again.
    if (flat)
    {
        mExpandedBeforeFlat = mExpanded;
        mExpanded.clear();
    }
    else
    {
        mExpanded.insert(mExpandedBeforeFlat.begin(), mExpandedBeforeFlat.end());
        mExpandedBeforeFlat.clear();
    }
    mDirty = true;
}

void SKInventoryTreeView::walkFavorites()
{
    // The one walk, of folders alone, at first use; the changes keep the set from then on.
    mFavoritesWalked = true;
    mFavoriteSet.clear();
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    std::vector<LLUUID> stack{ gInventory.getRootFolderID() };
    while (!stack.empty())
    {
        const LLUUID id = stack.back();
        stack.pop_back();
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(id, cats, items);
        if (!cats)
        {
            continue;
        }
        for (LLViewerInventoryCategory* cat : *cats)
        {
            if (cat->getUUID() == trash_id)
            {
                continue;
            }
            if (cat->getIsFavorite())
            {
                mFavoriteSet.insert(cat->getUUID());
            }
            stack.push_back(cat->getUUID());
        }
    }
    sortFavorites();
}

void SKInventoryTreeView::sortFavorites()
{
    // The outermost favourites, by name; a favourite inside another shows within it.
    std::vector<LLViewerInventoryCategory*> outer;
    for (const LLUUID& id : mFavoriteSet)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        if (!cat)
        {
            continue;
        }
        bool inside = false;
        for (const LLViewerInventoryCategory* up = gInventory.getCategory(cat->getParentUUID()); up && !inside;
             up = gInventory.getCategory(up->getParentUUID()))
        {
            inside = mFavoriteSet.count(up->getUUID()) > 0;
        }
        if (!inside)
        {
            outer.push_back(cat);
        }
    }
    std::sort(outer.begin(), outer.end(), [](const LLViewerInventoryCategory* a, const LLViewerInventoryCategory* b)
              { return LLStringUtil::compareDict(a->getName(), b->getName()) < 0; });
    mFavorites.clear();
    for (const LLViewerInventoryCategory* cat : outer)
    {
        mFavorites.push_back(cat->getUUID());
    }
}

void SKInventoryTreeView::addFolderRow(const LLUUID& folder_id, ESection section, const std::string& key, S32 depth)
{
    const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    if (!cat)
    {
        return;
    }
    Row row;
    row.mKind = Row::KIND_FOLDER;
    row.mSection = section;
    row.mID = folder_id;
    row.mKey = key;
    row.mDepth = depth;
    row.mName = cat->getName();
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, cats, items);
    const bool fetched = cat->getVersion() != LLViewerInventoryCategory::VERSION_UNKNOWN;
    row.mHasChildren = !mFlat && ((cats && !cats->empty()) || !fetched);
    row.mExpanded = row.mHasChildren && mExpanded.count(key) > 0;
    row.mIcon = LLUI::getUIImage(std::string(LLViewerFolderType::lookupIconName(cat->getPreferredType(), row.mExpanded)));
    mRows.push_back(row);
    mRowIDs.insert(folder_id);
    if (row.mExpanded)
    {
        addFolderRows(folder_id, section, key, depth + 1);
    }
}

void SKInventoryTreeView::addFolderRows(const LLUUID& folder_id, ESection section, const std::string& parent_key, S32 depth)
{
    std::vector<LLViewerInventoryCategory*> children;
    subfolders(folder_id, children);
    for (LLViewerInventoryCategory* cat : children)
    {
        addFolderRow(cat->getUUID(), section, parent_key + "/" + cat->getUUID().asString(), depth);
    }
}

void SKInventoryTreeView::rebuild()
{
    mDirty = false;
    if (!mFavoritesWalked)
    {
        walkFavorites();
    }
    mRows.clear();
    mRowIDs.clear();
    auto heading = [this](ESection section, const char* key)
    {
        Row row;
        row.mKind = Row::KIND_HEADING;
        row.mSection = section;
        row.mName = str(key);
        LLStringUtil::toUpper(row.mName);
        row.mHeight = HEADING_GAP + HEADING_TEXT + HEADING_BELOW;
        mRows.push_back(row);
    };

    // Favorites: the favourite folders, then the saved searches; a hint while both are empty.
    heading(SECTION_FAVORITES, "heading_favorites");
    for (const LLUUID& id : mFavorites)
    {
        addFolderRow(id, SECTION_FAVORITES, "f/" + id.asString(), 0);
    }
    for (S32 i = 0; i < (S32)mSavedSearches.size(); ++i)
    {
        Row row;
        row.mKind = Row::KIND_SAVED;
        row.mSection = SECTION_FAVORITES;
        row.mSaved = i;
        row.mKey = llformat("s/%d", i);
        row.mName = mSavedSearches[i].mQuery;
        row.mIcon = LLUI::getUIImage("Search_Icon");
        mRows.push_back(row);
    }
    if (mFavorites.empty() && mSavedSearches.empty())
    {
        Row row;
        row.mKind = Row::KIND_HINT;
        row.mSection = SECTION_FAVORITES;
        row.mName = str("favorites_hint");
        row.mHeight = HINT_HEIGHT;
        mRows.push_back(row);
    }

    // Inventory and Marketplace: My Inventory's own folders, split; no root row.
    std::vector<LLViewerInventoryCategory*> top;
    subfolders(gInventory.getRootFolderID(), top);
    for (ESection section : { SECTION_INVENTORY, SECTION_MARKETPLACE })
    {
        const size_t start = mRows.size();
        heading(section, section == SECTION_INVENTORY ? "heading_inventory" : "heading_marketplace");
        const char* prefix = section == SECTION_INVENTORY ? "i/" : "m/";
        for (LLViewerInventoryCategory* cat : top)
        {
            if (is_marketplace_top(cat) == (section == SECTION_MARKETPLACE))
            {
                addFolderRow(cat->getUUID(), section, prefix + cat->getUUID().asString(), 0);
            }
        }
        if (mRows.size() == start + 1)
        {
            mRows.pop_back(); // no heading over an empty section
        }
    }

    // Library: its own folders.
    const LLUUID& library_id = gInventory.getLibraryRootFolderID();
    if (library_id.notNull())
    {
        const size_t start = mRows.size();
        heading(SECTION_LIBRARY, "heading_library");
        std::vector<LLViewerInventoryCategory*> library;
        subfolders(library_id, library);
        for (LLViewerInventoryCategory* cat : library)
        {
            addFolderRow(cat->getUUID(), SECTION_LIBRARY, "l/" + cat->getUUID().asString(), 0);
        }
        if (mRows.size() == start + 1)
        {
            mRows.pop_back();
        }
    }

    // The selection follows its key; a row that went leaves none.
    if (!mSelectedKey.empty() && indexOfKey(mSelectedKey) < 0)
    {
        mSelectedKey.clear();
        mSelectedID.setNull();
    }
    mHover = mHoverKey.empty() ? -1 : indexOfKey(mHoverKey);
    mLaidOut = false;
}

S32 SKInventoryTreeView::contentWidth() const
{
    return llmax(0, getRect().getWidth() - (mScrollbar->getVisible() ? LLScrollbar::THIN_THICKNESS : 0));
}

void SKInventoryTreeView::layout()
{
    mLaidOut = true;
    S32 top = TOP_PAD;
    for (Row& row : mRows)
    {
        row.mTop = top;
        top += row.mHeight;
    }
    mContentHeight = top;
    const S32 height = getRect().getHeight();
    mScrollbar->setRect(LLRect(getRect().getWidth() - LLScrollbar::THIN_THICKNESS, height, getRect().getWidth(), 0));
    mScrollbar->setDocSize(mContentHeight);
    mScrollbar->setPageSize(height);
    mScrollbar->setVisible(mContentHeight > height);
    mScrollbar->setDocPos(mScrollbar->getDocPos());
}

void SKInventoryTreeView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    mLaidOut = false;
}

S32 SKInventoryTreeView::rowAt(S32 y) const
{
    const S32 down = getRect().getHeight() - y + mScrollbar->getDocPos();
    auto it = std::upper_bound(mRows.begin(), mRows.end(), down, [](S32 value, const Row& row) { return value < row.mTop; });
    if (it == mRows.begin())
    {
        return -1;
    }
    const S32 index = (S32)(it - mRows.begin()) - 1;
    return down < mRows[index].mTop + mRows[index].mHeight ? index : -1;
}

LLRect SKInventoryTreeView::rowRect(S32 index) const
{
    const Row& row = mRows[index];
    const S32 top = getRect().getHeight() - (row.mTop - mScrollbar->getDocPos());
    return LLRect(0, top, contentWidth(), top - row.mHeight);
}

LLRect SKInventoryTreeView::arrowRect(S32 index) const
{
    const LLRect rect = rowRect(index);
    const S32 icon_left = INDENT_LEFT + mRows[index].mDepth * INDENT_STEP;
    return LLRect(icon_left - ARROW_SIZE - 2, rect.mTop, icon_left, rect.mBottom);
}

LLRect SKInventoryTreeView::removeRect(S32 index) const
{
    const LLRect rect = rowRect(index);
    return LLRect(rect.mRight - 22, rect.mTop, rect.mRight - 4, rect.mBottom);
}

S32 SKInventoryTreeView::indexOfKey(const std::string& key) const
{
    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        if (mRows[i].mKey == key)
        {
            return i;
        }
    }
    return -1;
}

S32 SKInventoryTreeView::stepSelectable(S32 from, S32 step) const
{
    for (S32 i = from + step; i >= 0 && i < (S32)mRows.size(); i += step)
    {
        if (mRows[i].isSelectable())
        {
            return i;
        }
    }
    return -1;
}

std::string SKInventoryTreeView::countLabel(const LLUUID& folder_id, std::string* exact)
{
    // Worked out once per folder until the inventory changes: only for the selected row.
    auto it = mCounts.find(folder_id);
    if (it == mCounts.end())
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(folder_id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        const LLViewerInventoryCategory* self = gInventory.getCategory(folder_id);
        bool loading = !self || self->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN;
        for (const LLViewerInventoryCategory* cat : cats)
        {
            loading = loading || cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN;
            mCountedParents[cat->getUUID()] = cat->getParentUUID();
        }
        it = mCounts.emplace(folder_id, std::make_pair((S32)items.size(), loading)).first;
    }
    if (it->second.second)
    {
        return std::string("\xE2\x80\xA6");
    }
    if (exact)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", it->second.first);
        *exact = str("count_tooltip");
        LLStringUtil::format(*exact, args);
    }
    return compact(it->second.first);
}

void SKInventoryTreeView::drawRow(S32 index, const LLRect& rect)
{
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor sel_focus = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static LLUIColor sel_blur = LLUIColorTable::instance().getColor("LtGray_35");
    static LLUIColor hover_color = LLUIColorTable::instance().getColor("White_10");
    static LLPointer<LLUIImage> arrow_closed = LLUI::getUIImage("Arrow_Small_Right");
    static LLPointer<LLUIImage> arrow_open = LLUI::getUIImage("Arrow_Small_Down");
    static LLPointer<LLUIImage> close = LLUI::getUIImage("Icon_Close_Foreground");

    const Row& row = mRows[index];
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (row.mKind == Row::KIND_HEADING)
    {
        font->renderUTF8(row.mName, 0, HEADING_SIDE, rect.mBottom + HEADING_BELOW, dim_color.get(), LLFontGL::LEFT, LLFontGL::BOTTOM);
        return;
    }
    if (row.mKind == Row::KIND_HINT)
    {
        font->renderUTF8(row.mName, 0, (F32)(HEADING_SIDE + 4), (F32)(rect.mTop - 4), dim_color.get(), LLFontGL::LEFT, LLFontGL::TOP,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, rect.getWidth() - 2 * HEADING_SIDE, nullptr, true);
        return;
    }

    // Selection is a clean fill: blue while focused, dim otherwise; in Columns the row marks the path.
    const bool selected = !mSelectedKey.empty() && row.mKey == mSelectedKey;
    if (selected)
    {
        gl_rect_2d(rect, hasFocus() ? sel_focus.get() : sel_blur.get(), true);
    }
    else if (index == mHover)
    {
        gl_rect_2d(rect, hover_color.get(), true);
    }

    const S32 icon_left = INDENT_LEFT + row.mDepth * INDENT_STEP;
    const S32 mid = rect.getCenterY();
    if (row.mHasChildren)
    {
        LLUIImage* arrow = (row.mExpanded ? arrow_open : arrow_closed).get();
        if (arrow)
        {
            // Opacity .75, 1 on hover.
            const F32 alpha = (index == mHover && mHoverArrow) ? 1.f : 0.75f;
            arrow->draw(icon_left - ARROW_SIZE - 2, mid - ARROW_SIZE / 2, ARROW_SIZE, ARROW_SIZE, LLColor4(1.f, 1.f, 1.f, alpha));
        }
    }
    if (row.mIcon.notNull())
    {
        row.mIcon->draw(icon_left, mid - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE);
    }
    const S32 text_left = icon_left + ICON_SIZE + 4;
    S32 right = rect.mRight - 6;

    if (row.mKind == Row::KIND_SAVED)
    {
        if (index == mHover && close.notNull())
        {
            const LLRect remove = removeRect(index);
            close->draw(remove.mLeft + 3, mid - 6, 12, 12, text_color.get());
            right = remove.mLeft - 2;
        }
        F32 query_right = 0.f;
        const SavedSearch& saved = mSavedSearches[row.mSaved];
        font->renderUTF8(saved.mQuery, 0, (F32)text_left, (F32)mid, text_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                         LLFontGL::NO_SHADOW, S32_MAX, llmax(0, right - text_left), &query_right, true);
        if (!saved.mSuffix.empty() && (S32)query_right < right)
        {
            font->renderUTF8(saved.mSuffix, 0, query_right, (F32)mid, dim_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::NO_SHADOW, S32_MAX, right - (S32)query_right, nullptr, true);
        }
        return;
    }

    // The selected folder's recursive count, right-aligned.
    if (selected && !mFlat)
    {
        const std::string count = countLabel(row.mID, nullptr);
        font->renderUTF8(count, 0, right, mid, dim_color.get(), LLFontGL::RIGHT, LLFontGL::VCENTER);
        right -= font->getWidth(count) + 6;
    }
    font->renderUTF8(row.mName, 0, (F32)text_left, (F32)mid, text_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                     LLFontGL::NO_SHADOW, S32_MAX, llmax(0, right - text_left), nullptr, true);
}

void SKInventoryTreeView::draw()
{
    if (mDirty)
    {
        rebuild();
    }
    if (!mLaidOut)
    {
        layout();
    }
    if (!mPendingScrollKey.empty())
    {
        const S32 index = indexOfKey(mPendingScrollKey);
        if (index >= 0 && !isIndexInView(index))
        {
            scrollToIndex(index);
        }
        mPendingScrollKey.clear();
    }

    const S32 height = getRect().getHeight();
    {
        LLLocalClipRect clip(LLRect(0, height, contentWidth(), 0));
        const S32 first = llmax(0, rowAt(height));
        for (S32 i = first; i < (S32)mRows.size(); ++i)
        {
            const LLRect rect = rowRect(i);
            if (rect.mTop < 0)
            {
                break;
            }
            drawRow(i, rect);
        }
        SKInventoryExplorerDrag::drawMark(this);
    }
    LLUICtrl::draw();
}

bool SKInventoryTreeView::isIndexInView(S32 index) const
{
    const LLRect rect = rowRect(index);
    return rect.mBottom >= 0 && rect.mTop <= getRect().getHeight();
}

void SKInventoryTreeView::scrollToIndex(S32 index)
{
    const Row& row = mRows[index];
    const S32 pos = mScrollbar->getDocPos();
    const S32 view = getRect().getHeight();
    if (row.mTop < pos)
    {
        mScrollbar->setDocPos(row.mTop);
    }
    else if (row.mTop + row.mHeight > pos + view)
    {
        mScrollbar->setDocPos(row.mTop + row.mHeight - view);
    }
}

void SKInventoryTreeView::selectIndex(S32 index, bool open)
{
    if (index < 0 || index >= (S32)mRows.size() || !mRows[index].isSelectable())
    {
        return;
    }
    const Row& row = mRows[index];
    mSelectedKey = row.mKey;
    mSelectedID = row.mID;
    if (!mLaidOut)
    {
        layout();
    }
    scrollToIndex(index);
    if (!open)
    {
        return;
    }
    if (row.mKind == Row::KIND_SAVED)
    {
        if (mRunSavedSearch)
        {
            mRunSavedSearch(row.mSaved);
        }
    }
    else if (mOpenCallback)
    {
        const LLUUID id = row.mID;
        mOpenCallback(id);
    }
}

void SKInventoryTreeView::setExpanded(S32 index, bool expanded)
{
    if (mFlat || index < 0 || index >= (S32)mRows.size() || !mRows[index].mHasChildren || mRows[index].mExpanded == expanded)
    {
        return;
    }
    if (expanded)
    {
        mExpanded.insert(mRows[index].mKey);
        const LLViewerInventoryCategory* cat = gInventory.getCategory(mRows[index].mID);
        if (cat && cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
        {
            LLInventoryModelBackgroundFetch::instance().start(mRows[index].mID, false);
        }
    }
    else
    {
        // Collapsing hides the rows under it; a selection among them moves up to this row.
        const std::string& key = mRows[index].mKey;
        if (mSelectedKey.size() > key.size() && mSelectedKey.compare(0, key.size() + 1, key + "/") == 0)
        {
            mSelectedKey = key;
            mSelectedID = mRows[index].mID;
        }
        mExpanded.erase(key);
    }
    rebuild();
}

LLUUID SKInventoryTreeView::favoriteRootOf(const LLUUID& folder_id) const
{
    for (const LLUUID& fav : mFavorites)
    {
        if (fav == folder_id || gInventory.isObjectDescendentOf(folder_id, fav))
        {
            return fav;
        }
    }
    return LLUUID::null;
}

bool SKInventoryTreeView::keyFor(const LLUUID& folder_id, ESection& section, std::string& key, std::vector<std::string>& ancestors) const
{
    // The chain from the section's top row down to the folder.
    std::vector<LLUUID> chain;
    const LLUUID fav = favoriteRootOf(folder_id);
    const LLUUID& root_id = gInventory.getRootFolderID();
    const LLUUID& library_id = gInventory.getLibraryRootFolderID();
    LLUUID id = folder_id;
    while (id.notNull())
    {
        chain.push_back(id);
        if (id == fav)
        {
            break;
        }
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        if (!cat || cat->getParentUUID() == root_id || cat->getParentUUID() == library_id)
        {
            break;
        }
        id = cat->getParentUUID();
    }
    if (chain.empty())
    {
        return false;
    }
    const LLViewerInventoryCategory* top = gInventory.getCategory(chain.back());
    if (!top)
    {
        return false;
    }
    std::string prefix;
    if (fav.notNull())
    {
        section = SECTION_FAVORITES;
        prefix = "f";
    }
    else if (top->getParentUUID() == library_id)
    {
        section = SECTION_LIBRARY;
        prefix = "l";
    }
    else if (top->getParentUUID() == root_id)
    {
        section = is_marketplace_top(top) ? SECTION_MARKETPLACE : SECTION_INVENTORY;
        prefix = section == SECTION_MARKETPLACE ? "m" : "i";
    }
    else
    {
        return false;
    }
    key = prefix;
    ancestors.clear();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if (!key.empty() && key != prefix)
        {
            ancestors.push_back(key);
        }
        key += "/" + it->asString();
    }
    return true;
}

void SKInventoryTreeView::reveal(const LLUUID& folder_id)
{
    if (mDirty)
    {
        rebuild();
    }
    // The folder can be listed twice (Favorites and its own section); keep whichever copy is selected.
    if (mSelectedID == folder_id && indexOfKey(mSelectedKey) >= 0)
    {
        return;
    }
    ESection section;
    std::string key;
    std::vector<std::string> ancestors;
    if (!keyFor(folder_id, section, key, ancestors))
    {
        mSelectedKey.clear();
        mSelectedID.setNull();
        return;
    }
    if (!mFlat)
    {
        bool grew = false;
        for (const std::string& ancestor : ancestors)
        {
            grew = mExpanded.insert(ancestor).second || grew;
        }
        if (grew)
        {
            rebuild();
        }
    }
    else if (!ancestors.empty())
    {
        key = ancestors.front(); // flat: the top-level row marks the path
    }
    // A saved search's row stays selected while it runs.
    if (mSavedSelected >= 0)
    {
        return;
    }
    mSelectedKey = key;
    mSelectedID = folder_id;
    // Scrolls only if no copy of the row is in view.
    if (!mLaidOut)
    {
        layout();
    }
    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        if (mRows[i].mKind == Row::KIND_FOLDER && mRows[i].mID == folder_id && isIndexInView(i))
        {
            return;
        }
    }
    mPendingScrollKey = key;
}

void SKInventoryTreeView::focusSelection(const LLUUID& folder_id)
{
    if (folder_id.notNull())
    {
        reveal(folder_id);
    }
    setFocus(true);
}

void SKInventoryTreeView::onFocusReceived()
{
    LLUICtrl::onFocusReceived();
}

void SKInventoryTreeView::onFocusLost()
{
    mTypeAhead.clear();
    LLUICtrl::onFocusLost();
}

bool SKInventoryTreeView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    setFocus(true);
    const S32 index = rowAt(y);
    if (index < 0 || !mRows[index].isSelectable())
    {
        return true;
    }
    make_ui_sound("UISndClick");
    // Copied: opening can rebuild the rows.
    const Row::EKind kind = mRows[index].mKind;
    const S32 saved = mRows[index].mSaved;
    if (kind == Row::KIND_SAVED && index == mHover && removeRect(index).pointInRect(x, y))
    {
        if (mRemoveSavedSearch)
        {
            mRemoveSavedSearch(saved);
        }
        return true;
    }
    if (mRows[index].mHasChildren && arrowRect(index).pointInRect(x, y))
    {
        setExpanded(index, !mRows[index].mExpanded);
        return true;
    }
    // A click opens the folder; it never expands.
    selectIndex(index, true);
    if (kind == Row::KIND_FOLDER)
    {
        mPressIndex = indexOfKey(mSelectedKey);
        mPressX = x;
        mPressY = y;
        gFocusMgr.setMouseCapture(this);
    }
    return true;
}

bool SKInventoryTreeView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    mPressIndex = -1;
    return LLUICtrl::handleMouseUp(x, y, mask);
}

void SKInventoryTreeView::onMouseCaptureLost()
{
    mPressIndex = -1;
}

bool SKInventoryTreeView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    const S32 index = rowAt(y);
    if (index >= 0 && mRows[index].mHasChildren && !arrowRect(index).pointInRect(x, y))
    {
        setExpanded(index, !mRows[index].mExpanded);
        return true;
    }
    return handleMouseDown(x, y, mask);
}

bool SKInventoryTreeView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const S32 index = rowAt(y);
    if (index < 0 || mRows[index].mKind != Row::KIND_FOLDER)
    {
        return true;
    }
    const LLUUID id = mRows[index].mID;
    if (mRows[index].mKey != mSelectedKey)
    {
        selectIndex(index, true);
    }
    if (mMenuCallback)
    {
        mMenuCallback(x, y, id);
    }
    return true;
}

bool SKInventoryTreeView::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mPressIndex >= 0 && mPressIndex < (S32)mRows.size())
    {
        // A row dragged 5 px carries that folder alone.
        if (abs(x - mPressX) + abs(y - mPressY) >= SKInventoryExplorerDrag::DRAG_THRESHOLD)
        {
            const LLUUID id = mRows[mPressIndex].mID;
            mPressIndex = -1;
            gFocusMgr.setMouseCapture(nullptr);
            SKInventoryExplorerDrag::begin({ id });
        }
        return true;
    }
    mHover = rowAt(y);
    mHoverArrow = mHover >= 0 && mRows[mHover].mHasChildren && arrowRect(mHover).pointInRect(x, y);
    if (mHover >= 0 && !mRows[mHover].isSelectable())
    {
        mHover = -1;
    }
    mHoverKey = mHover >= 0 ? mRows[mHover].mKey : std::string();
    return LLUICtrl::handleHover(x, y, mask);
}

void SKInventoryTreeView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    mHoverKey.clear();
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKInventoryTreeView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    if (mContentHeight <= getRect().getHeight())
    {
        return false;
    }
    mScrollbar->setDocPos(mScrollbar->getDocPos() + delta.mClicks * WHEEL_ROWS * ROW_HEIGHT);
    return true;
}

bool SKInventoryTreeView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 index = rowAt(y);
    if (index < 0 || !mRows[index].isSelectable())
    {
        return LLUICtrl::handleToolTip(x, y, mask);
    }
    const Row& row = mRows[index];
    if (row.mKind == Row::KIND_SAVED)
    {
        LLToolTipMgr::instance().show(removeRect(index).pointInRect(x, y) ? str("remove_favorite") : mSavedSearches[row.mSaved].mToolTip);
        return true;
    }
    std::string exact;
    if (row.mKey == mSelectedKey && !mFlat)
    {
        countLabel(row.mID, &exact);
    }
    LLToolTipMgr::instance().show(exact.empty() ? row.mName : row.mName + "\n" + exact);
    return true;
}

bool SKInventoryTreeView::handleKeyHere(KEY key, MASK mask)
{
    if (mKeyCallback && mKeyCallback(key, mask))
    {
        return true;
    }
    if (mask != MASK_NONE)
    {
        return LLUICtrl::handleKeyHere(key, mask);
    }
    const S32 cur = indexOfKey(mSelectedKey);
    switch (key)
    {
        case KEY_UP:
        case KEY_DOWN:
        {
            // The sections are one list to the keys.
            const S32 next = cur < 0 ? stepSelectable(-1, 1) : stepSelectable(cur, key == KEY_DOWN ? 1 : -1);
            if (next >= 0)
            {
                selectIndex(next, mRows[next].mKind != Row::KIND_SAVED);
            }
            return true;
        }
        case KEY_HOME:
        {
            const S32 first = stepSelectable(-1, 1);
            selectIndex(first, first >= 0 && mRows[first].mKind != Row::KIND_SAVED);
            return true;
        }
        case KEY_END:
        {
            const S32 last = stepSelectable((S32)mRows.size(), -1);
            selectIndex(last, last >= 0 && mRows[last].mKind != Row::KIND_SAVED);
            return true;
        }
        case KEY_RIGHT:
            // Expands, then enters.
            if (cur >= 0 && mRows[cur].mHasChildren)
            {
                if (!mRows[cur].mExpanded)
                {
                    setExpanded(cur, true);
                }
                else if (cur + 1 < (S32)mRows.size() && mRows[cur + 1].mDepth > mRows[cur].mDepth && mRows[cur + 1].mKind == Row::KIND_FOLDER)
                {
                    selectIndex(cur + 1, true);
                }
            }
            return true;
        case KEY_LEFT:
            // Collapses, then goes to the parent.
            if (cur >= 0 && mRows[cur].mExpanded)
            {
                setExpanded(cur, false);
            }
            else if (cur >= 0 && mRows[cur].mDepth > 0)
            {
                for (S32 i = cur - 1; i >= 0; --i)
                {
                    if (mRows[i].mKind == Row::KIND_FOLDER && mRows[i].mDepth == mRows[cur].mDepth - 1)
                    {
                        selectIndex(i, true);
                        break;
                    }
                }
            }
            return true;
        case KEY_RETURN:
            // Enter runs a saved search the keys moved onto.
            if (cur >= 0 && mRows[cur].mKind == Row::KIND_SAVED && mRunSavedSearch)
            {
                mRunSavedSearch(mRows[cur].mSaved);
            }
            [[fallthrough]];
        case KEY_TAB:
            if (mFocusContentCallback)
            {
                mFocusContentCallback();
                return true;
            }
            break;
        default:
            break;
    }
    return LLUICtrl::handleKeyHere(key, mask);
}

bool SKInventoryTreeView::handleUnicodeCharHere(llwchar uni_char)
{
    if (uni_char < 0x20)
    {
        return false;
    }
    // Type-ahead: a case-insensitive prefix, reset after a second.
    if (mTypeAheadTimer.getElapsedTimeF32() > TYPE_AHEAD_SECONDS)
    {
        mTypeAhead.clear();
    }
    mTypeAheadTimer.reset();
    char utf8[8] = {};
    mTypeAhead.append(utf8, (size_t)wchar_to_utf8chars(uni_char, utf8));
    std::string prefix = mTypeAhead;
    LLStringUtil::toLower(prefix);
    const S32 cur = indexOfKey(mSelectedKey);
    const S32 count = (S32)mRows.size();
    const S32 start = mTypeAhead.size() == 1 ? cur + 1 : llmax(cur, 0);
    for (S32 n = 0; n < count; ++n)
    {
        const S32 i = (start + n + count) % count;
        if (!mRows[i].isSelectable())
        {
            continue;
        }
        std::string name = mRows[i].mName;
        LLStringUtil::toLower(name);
        if (name.compare(0, prefix.size(), prefix) == 0)
        {
            selectIndex(i, mRows[i].mKind != Row::KIND_SAVED);
            break;
        }
    }
    return true;
}

bool SKInventoryTreeView::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                            EAcceptance* accept, std::string& tooltip_msg)
{
    const S32 index = rowAt(y);
    const bool first_cargo = LLToolDragAndDrop::getInstance()->getCargoIndex() == 0;
    if (index < 0 || mRows[index].mKind == Row::KIND_SAVED)
    {
        *accept = ACCEPT_NO;
        return true;
    }
    const Row& row = mRows[index];
    LLRect rect = rowRect(index);
    if ((row.mKind == Row::KIND_HEADING || row.mKind == Row::KIND_HINT) && row.mSection == SECTION_FAVORITES)
    {
        // The Favorites heading and its hint take folders as favourites.
        SKInventoryExplorerDrag::dropOnFavorites(drop, cargo_type, cargo_data, accept, tooltip_msg);
    }
    else if (row.mKind == Row::KIND_FOLDER)
    {
        SKInventoryExplorerDrag::dropOnFolder(row.mID, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
        // Held over a folder, it expands.
        S32 sx = 0;
        S32 sy = 0;
        localPointToScreen(x, y, &sx, &sy);
        if (!drop && first_cargo && !SKInventoryExplorerDrag::verdict().mOntoCargo
            && mDropHover.spring(row.mID, sx, sy, SKInventoryExplorerDrag::springSeconds()) && row.mHasChildren && !row.mExpanded)
        {
            setExpanded(index, true);
        }
    }
    else
    {
        *accept = ACCEPT_NO;
        return true;
    }
    SKInventoryExplorerDrag::mark(this, rect);
    if (!drop && first_cargo)
    {
        mDropHover.touch();
        // Autoscroll near the edges.
        const S32 band = SKInventoryExplorerDrag::SCROLL_BAND;
        if ((y < band || y > getRect().getHeight() - band) && mDropHover.tick(SKInventoryExplorerDrag::TICK_SECONDS))
        {
            mScrollbar->setDocPos(mScrollbar->getDocPos() + (y < band ? 1 : -1) * SKInventoryExplorerDrag::SCROLL_STEP);
            S32 sx = 0;
            S32 sy = 0;
            localPointToScreen(x, y, &sx, &sy);
            mDropHover.scrolled(sx, sy);
        }
    }
    return true;
}
