/**
 * @file skinventorysidebar.h
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

#ifndef SK_SKINVENTORYSIDEBAR_H
#define SK_SKINVENTORYSIDEBAR_H

#include "llfolderview.h"
#include "llinventorypanel.h"

#include <functional>
#include <set>

class LLTextBox;

// The sidebar panels' folder view, letting the sidebar take a key before the stock handling.
class SKSidebarFolderView final : public LLFolderView
{
public:
    AL_VIEW_TYPE(SKSidebarFolderView, LLFolderView);

    struct Params : public LLInitParam::Block<Params, LLFolderView::Params>
    {
    };

    SKSidebarFolderView(const LLFolderView::Params& p);

    // Flat, as Columns' first column: rows select but never expand.
    void setFlat(bool flat) { mFlat = flat; }

    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;

private:
    bool mFlat;
};

// An inventory panel of folders only that never scrolls itself: its scroller is
// kept as the folder view's frame with its bars hidden, the panel is sized to the
// rows, and the sidebar's one outer scroller does the scrolling.
class SKInventorySidebarPanel : public LLInventoryPanel
{
public:
    AL_VIEW_TYPE(SKInventorySidebarPanel, LLInventoryPanel);

    enum ESection
    {
        SECTION_INVENTORY,
        SECTION_MARKETPLACE,
        SECTION_LIBRARY,
        SECTION_FAVORITES,
    };

    struct Params : public LLInitParam::Block<Params, LLInventoryPanel::Params>
    {
        Optional<std::string> section;

        Params();
    };

    void initFromParams(const Params& p);

    // The height of the rows the folder view holds, 0 while it holds none.
    S32 getContentHeight();

    ESection getSection() const { return mSection; }

    // Whether this panel lists the folder as a row.
    virtual bool covers(const LLUUID& folder_id) const;

    typedef std::function<bool(KEY key, MASK mask)> key_callback_t;
    void setKeyCallback(const key_callback_t& cb) { mKeyCallback = cb; }
    bool handleSidebarKey(KEY key, MASK mask) { return mKeyCallback && mKeyCallback(key, mask); }

protected:
    SKInventorySidebarPanel(const Params& p);
    friend class LLUICtrlFactory;

    bool typedViewsFilter(const LLUUID& id, LLInventoryObject const* objectp) override;
    LLFolderView* createFolderRoot(LLUUID root_id) override;

    ESection mSection;
    key_callback_t mKeyCallback;
};

// Favorites: one root per favourite folder, each expanding in place.
class SKInventoryFavoritesPanel final : public SKInventorySidebarPanel
{
public:
    AL_VIEW_TYPE(SKInventoryFavoritesPanel, SKInventorySidebarPanel);

    struct Params : public LLInitParam::Block<Params, SKInventorySidebarPanel::Params>
    {
    };

    bool covers(const LLUUID& folder_id) const override;
    void removeItemID(const LLUUID& id) override;
    bool isInRootContent(const LLUUID& id, LLFolderViewItem* view_item) override;

    // The favourite root the folder is or sits under, null if none.
    LLUUID favoriteRootOf(const LLUUID& folder_id) const;

protected:
    SKInventoryFavoritesPanel(const Params& p);
    friend class LLUICtrlFactory;

    void initRootContent() override;
    void findAndInitRootContent(const LLUUID& id) override;
    void itemChanged(const LLUUID& id, U32 mask, const LLInventoryObject* model_item) override;

    bool removeFavorite(const LLUUID& id);

    std::set<LLUUID> mRootContentIDs;
};

// Headings over the four panels, stacked by content height inside the outer scroller.
class SKInventorySidebar : public LLPanel
{
public:
    AL_VIEW_TYPE(SKInventorySidebar, LLPanel);

    struct Params : public LLInitParam::Block<Params, LLPanel::Params>
    {
    };

    typedef std::function<void(const LLUUID& folder_id)> open_callback_t;
    typedef SKInventorySidebarPanel::key_callback_t key_callback_t;

    bool postBuild() override;
    void draw() override;

    void setOpenCallback(const open_callback_t& cb) { mOpenCallback = cb; }
    // Keys a panel's folder view offers first, before its own handling.
    void setKeyCallback(const key_callback_t& cb) { mKeyCallback = cb; }

    // Columns view: the sidebar is the first column, flat, nothing expanding.
    void setFlat(bool flat);
    // Gives keyboard focus to the panel holding the selected row.
    void focusSelection();

    // Selects the folder's row in the panel that should show it, expanding to it,
    // and scrolls only if no copy of the row is in view.
    void reveal(const LLUUID& folder_id);

protected:
    SKInventorySidebar(const Params& p);
    friend class LLUICtrlFactory;

    void arrangePanels();
    void scrollToPendingReveal();
    void onPanelSelect(SKInventorySidebarPanel* panel, const std::deque<LLFolderViewItem*>& items, bool user_action);
    bool isRowInView(LLFolderViewItem* row) const;

    std::vector<SKInventorySidebarPanel*> mPanels;
    std::vector<LLTextBox*> mHeadings;
    LLTextBox* mFavoritesHint;
    open_callback_t mOpenCallback;
    key_callback_t mKeyCallback;
    bool mFlat;
    std::vector<LLUUID> mOpenBeforeFlat;   // top-level folders open in List, opened again on leaving Columns
    bool mRevealing;
    LLUUID mPendingReveal;
    SKInventorySidebarPanel* mPendingPanel;
};

#endif // SK_SKINVENTORYSIDEBAR_H
