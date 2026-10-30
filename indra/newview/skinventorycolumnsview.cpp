/**
 * @file skinventorycolumnsview.cpp
 * @brief Inventory Explorer's Columns view: the path below the top-level folder as a strip of columns
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

#include "skinventorycolumnsview.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewerinventory.h"
#include "skinventorydetails.h"
#include "skinventoryexplorermenu.h"
#include "skinventoryrenameeditor.h"

#include <algorithm>

static LLDefaultChildRegistry::Register<SKInventoryColumnsView> r_sk_inventory_columns("sk_inventory_columns");

namespace
{
    constexpr S32 COLUMN_WIDTH = 230;
    constexpr S32 DETAILS_WIDTH = 280;
    constexpr S32 ROW_HEIGHT = 20;
    constexpr S32 TOP_PAD = 4;
    constexpr S32 CELL_PAD = 4;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 GHOST_ROWS = 600;
    constexpr S32 WHEEL_ROWS = 3;
    constexpr S32 HWHEEL_PIXELS = 30;
    constexpr S32 EDGE_REACH = 40;
    constexpr F32 EDGE_DELAY = 0.25f;

    bool is_root(const LLUUID& folder_id)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        return cat && cat->getParentUUID().isNull();
    }
}

SKInventoryColumnsView::SKInventoryColumnsView(const Params& p)
:   LLUICtrl(p),
    mCurrent(-1),
    mSelectedCount(0),
    mCursor(-1),
    mAnchor(-1),
    mFolderCount(0),
    mItemCount(0),
    mLoading(false),
    mDirty(false),
    mDetailsShown(true),
    mGeneration(0),
    mSortKey(SKInventoryListView::COL_ACQUIRED),
    mSortAscending(false),
    mGroup(SKInventoryFlatView::GROUP_NONE),
    mStripX(0),
    mStripPad(0),
    mHoverColumn(-1),
    mHoverRow(-1),
    mContentGeneration(0),
    mGhostGeneration(U32_MAX),
    mGhostSortKey(SKInventoryListView::COL_ACQUIRED),
    mGhostSortAscending(false),
    mGhostFetchPending(false),
    mPress(PRESS_NONE),
    mPressX(0),
    mPressY(0),
    mPressRow(-1),
    mPressMask(MASK_NONE),
    mPressCollapse(false),
    mSweepLow(-1),
    mSweepHigh(-1),
    mDropColumn(-1),
    mDropRow(-1),
    mDropScrollResidue(0),
    mEdgeDir(0),
    mEdgeScrolling(false),
    mGlowColumn(-1),
    mStripScrollbar(nullptr),
    mDetailsView(nullptr),
    mDetailsPlaced(false)
{
    const S32 bar = LLScrollbar::THIN_THICKNESS;
    LLScrollbar::Params sbparams;
    sbparams.name("strip_scrollbar");
    sbparams.rect(LLRect(0, bar, getRect().getWidth(), 0));
    sbparams.orientation(LLScrollbar::HORIZONTAL);
    sbparams.doc_size(0);
    sbparams.doc_pos(0);
    sbparams.page_size(1);
    sbparams.follows.flags(FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_BOTTOM);
    sbparams.visible(false);
    sbparams.thin(true); // drawn over the columns, which keep the full height
    sbparams.change_callback([this](S32 pos, LLScrollbar*) { mStripX = pos; });
    mStripScrollbar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
    addChild(mStripScrollbar);

    SKInventoryDetailsView::Params dparams;
    dparams.name("columns_details");
    dparams.rect(LLRect(0, getRect().getHeight(), 280, 0));
    dparams.follows.flags(FOLLOWS_NONE);
    dparams.visible(false);
    mDetailsView = LLUICtrlFactory::create<SKInventoryDetailsView>(dparams);
    addChild(mDetailsView);
}

SKInventoryColumnsView::~SKInventoryColumnsView()
{
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
}

void SKInventoryColumnsView::setFolder(const LLUUID& folder_id, const uuid_vec_t& selection)
{
    mFolderID = folder_id;
    mCursor = -1;
    mAnchor = -1;
    rebuild(true);
    setSelection(selection);
}

void SKInventoryColumnsView::setScrollState(S32 strip_x, const scroll_map_t& scrolls)
{
    mStripX = llmax(0, strip_x);
    mScrolls = scrolls;
    updateStripScrollbar();
}

void SKInventoryColumnsView::setSort(SKInventoryListView::EColumn key, bool ascending)
{
    if (key != mSortKey || ascending != mSortAscending)
    {
        mSortKey = key;
        mSortAscending = ascending;
        mDirty = true;
    }
}

void SKInventoryColumnsView::setDetailsShown(bool shown)
{
    if (shown != mDetailsShown)
    {
        mDetailsShown = shown;
        rebuildTrailing();
        revealColumns(false);
    }
}

bool SKInventoryColumnsView::lists(const LLUUID& id) const
{
    if (mListedIDs.contains(id))
    {
        return true;
    }
    // The ghost's rows are few, so they are looked through rather than indexed.
    const Column* current = currentColumn();
    if (current && &mColumns.back() != current && mColumns.back().mKind == KIND_GHOST)
    {
        const Column& ghost = mColumns.back();
        return ghost.mFolderID == id || std::any_of(ghost.mRows.begin(), ghost.mRows.end(), [&](const SKInventoryRow& row) { return row.mID == id; });
    }
    return false;
}

LLUUID SKInventoryColumnsView::nearestLiveFolder() const
{
    for (auto it = mPath.rbegin(); it != mPath.rend(); ++it)
    {
        if (gInventory.getCategory(*it))
        {
            return *it;
        }
    }
    return LLUUID::null;
}

SKInventoryColumnsView::Column* SKInventoryColumnsView::currentColumn()
{
    return (mCurrent >= 0 && mCurrent < (S32)mColumns.size()) ? &mColumns[mCurrent] : nullptr;
}

const SKInventoryColumnsView::Column* SKInventoryColumnsView::currentColumn() const
{
    return (mCurrent >= 0 && mCurrent < (S32)mColumns.size()) ? &mColumns[mCurrent] : nullptr;
}

SKInventoryColumnsView::scroll_map_t SKInventoryColumnsView::getColumnScrolls() const
{
    scroll_map_t scrolls;
    for (const Column& column : mColumns)
    {
        auto it = mScrolls.find(column.mFolderID);
        if (it != mScrolls.end())
        {
            scrolls.insert(*it);
        }
    }
    return scrolls;
}

void SKInventoryColumnsView::focusFirstColumn()
{
    if (mCurrent > 0 && mMoveCallback)
    {
        mMoveCallback(mColumns.front().mFolderID);
    }
    setFocus(true);
    selectFirstRow();
    revealColumns(false);
}

void SKInventoryColumnsView::readColumn(Column& column, bool folders_too, bool fetch)
{
    column.mLoading = SKInventoryListView::readRows(column.mFolderID, mLabels, column.mRows, fetch);
    if (!folders_too)
    {
        // At My Inventory or the Library the sidebar lists the folders, so only loose items stay.
        std::erase_if(column.mRows, [](const SKInventoryRow& row) { return row.mIsFolder && !row.mIsLink; });
    }
    SKInventoryListView::sortRows(column.mRows, mSortKey, mSortAscending);
}

void SKInventoryColumnsView::rebuild(bool reveal_path)
{
    mDirty = false;
    const uuid_vec_t selection = getSelection();
    const Column* cur = currentColumn();
    const LLUUID cursor_id = (cur && mCursor >= 0 && mCursor < (S32)cur->mRows.size()) ? cur->mRows[mCursor].mID : LLUUID::null;

    mPath.clear();
    for (const LLViewerInventoryCategory* cat = gInventory.getCategory(mFolderID); cat;
         cat = cat->getParentUUID().notNull() ? gInventory.getCategory(cat->getParentUUID()) : nullptr)
    {
        mPath.insert(mPath.begin(), cat->getUUID());
    }

    // One column per folder below the top: the sidebar is the top-level folders' column.
    mColumns.clear();
    mListedIDs.clear();
    const size_t first = mPath.size() > 1 ? 1 : 0;
    for (size_t i = first; i < mPath.size(); ++i)
    {
        Column column;
        column.mFolderID = mPath[i];
        column.mPathID = (i + 1 < mPath.size()) ? mPath[i + 1] : LLUUID::null;
        readColumn(column, !is_root(column.mFolderID));
        if (i + 1 == mPath.size())
        {
            // Grouping adds headers in the current column only.
            SKInventoryFlatView::groupRows(column.mRows, mGroup, mLabels);
        }
        mListedIDs.insert(column.mFolderID);
        for (const SKInventoryRow& row : column.mRows)
        {
            mListedIDs.insert(row.mID);
        }
        mColumns.push_back(std::move(column));
    }
    mCurrent = (S32)mColumns.size() - 1;

    // The status counts what the open folder holds, the sidebar's folders included.
    mFolderCount = 0;
    mItemCount = 0;
    mLoading = false;
    if (Column* current = currentColumn())
    {
        mLoading = current->mLoading;
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(current->mFolderID, cats, items);
        mFolderCount = cats ? (S32)cats->size() : 0;
        if (items)
        {
            for (const LLViewerInventoryItem* item : *items)
            {
                // A folder link counts as the folder it opens, as its row does.
                const bool folder_link = item->getIsLinkType() && gInventory.getCategory(item->getLinkedUUID());
                (folder_link ? mFolderCount : mItemCount)++;
            }
        }
    }

    // On navigation each path column moves only to show the row the path goes on through; live updates keep scrolls.
    for (S32 i = 0; reveal_path && i < mCurrent; ++i)
    {
        const Column& column = mColumns[i];
        for (S32 r = 0; r < (S32)column.mRows.size(); ++r)
        {
            if (column.mRows[r].mID == column.mPathID)
            {
                scrollRowIntoView(column, r);
                break;
            }
        }
    }

    ++mGeneration;
    mSelected.assign(currentColumn() ? currentColumn()->mRows.size() : 0, false);
    mSelectedCount = 0;
    mCursor = -1;
    mAnchor = -1;
    if (const Column* current = currentColumn())
    {
        for (S32 r = 0; r < (S32)current->mRows.size(); ++r)
        {
            const LLUUID& id = current->mRows[r].mID;
            if (std::find(selection.begin(), selection.end(), id) != selection.end())
            {
                mSelected[r] = true;
                ++mSelectedCount;
                mCursor = r;
            }
            if (id == cursor_id)
            {
                mCursor = r;
            }
        }
    }
    mAnchor = mCursor;
    rebuildTrailing();
    updateStripScrollbar();
}

void SKInventoryColumnsView::rebuildTrailing()
{
    if (mCurrent < 0)
    {
        mColumns.clear();
        layoutColumns();
        return;
    }
    mColumns.resize(mCurrent + 1);
    const Column& current = mColumns[mCurrent];

    const SKInventoryRow* single = nullptr;
    if (mSelectedCount == 1)
    {
        for (size_t r = 0; r < current.mRows.size(); ++r)
        {
            if (mSelected[r])
            {
                single = &current.mRows[r];
                break;
            }
        }
    }

    Column trailing;
    if (single && single->mIsFolder)
    {
        // A folder or folder link previews its contents, dimmed.
        // Read again only when the folder, the content or the sort moved; a fetch waits for the cursor to rest.
        if (mGhostCache.mFolderID != single->mTargetID || mGhostGeneration != mContentGeneration
            || mGhostSortKey != mSortKey || mGhostSortAscending != mSortAscending)
        {
            if (mGhostCache.mFolderID != single->mTargetID)
            {
                mGhostRestTimer.reset();
                mGhostFetchPending = true;
            }
            mGhostCache = Column();
            mGhostCache.mKind = KIND_GHOST;
            mGhostCache.mFolderID = single->mTargetID;
            readColumn(mGhostCache, true, false);
            if ((S32)mGhostCache.mRows.size() > GHOST_ROWS)
            {
                mGhostCache.mMore = (S32)mGhostCache.mRows.size() - GHOST_ROWS;
                mGhostCache.mRows.resize(GHOST_ROWS);
            }
            mGhostCache.mWidth = COLUMN_WIDTH;
            mGhostGeneration = mContentGeneration;
            mGhostSortKey = mSortKey;
            mGhostSortAscending = mSortAscending;
        }
        trailing = mGhostCache;
    }
    else if (mSelectedCount > 0 && mDetailsShown)
    {
        // A single item, or a selection of several (folders among them), shows the details column.
        trailing.mKind = KIND_DETAILS;
        for (size_t r = 0; r < current.mRows.size(); ++r)
        {
            if (mSelected[r])
            {
                trailing.mRows.push_back(current.mRows[r]);
            }
        }
        trailing.mPathID = trailing.mRows.empty() ? LLUUID::null : trailing.mRows.front().mID;
        trailing.mWidth = DETAILS_WIDTH;
    }
    else if (!current.mRows.empty())
    {
        // As wide as what a selection here could open, so deselecting doesn't shrink the strip.
        const bool items = std::any_of(current.mRows.begin(), current.mRows.end(),
                                       [](const SKInventoryRow& row) { return row.isEntry() && !row.mIsFolder; });
        trailing.mKind = KIND_SPARE;
        trailing.mFolderID = current.mFolderID;
        trailing.mWidth = (items && mDetailsShown) ? DETAILS_WIDTH : COLUMN_WIDTH;
    }
    if (trailing.mWidth > 0)
    {
        mColumns.push_back(std::move(trailing));
    }
    layoutColumns();
}

void SKInventoryColumnsView::layoutColumns()
{
    S32 left = 0;
    for (S32 i = 0; i < (S32)mColumns.size(); ++i)
    {
        Column& column = mColumns[i];
        column.mLeft = left;
        if (column.mKind == KIND_PATH)
        {
            // At My Inventory or the Library with no loose items there is no column at all.
            const bool empty_root = column.mRows.empty() && !column.mLoading && is_root(column.mFolderID);
            column.mWidth = empty_root ? 0 : COLUMN_WIDTH;
        }
        left += column.mWidth;
    }
}

void SKInventoryColumnsView::revealColumns(bool trailing)
{
    const Column* current = currentColumn();
    const S32 view = getRect().getWidth();
    if (current && current->mWidth > 0)
    {
        const S32 left = current->mLeft;
        S32 right = current->mLeft + current->mWidth;
        const Column& last = mColumns.back();
        if (trailing && (last.mKind == KIND_GHOST || last.mKind == KIND_DETAILS))
        {
            right = last.mLeft + last.mWidth;
            // Opening details or a preview gives up the empty room left from a deeper strip, so none sits beside it.
            if (right < mStripX + view)
            {
                mStripX = right - view;
            }
        }
        if (right > mStripX + view)
        {
            mStripX = right - view;
        }
        // The current column wins when both don't fit.
        if (left < mStripX)
        {
            mStripX = left;
        }
    }
    mStripX = llmax(0, mStripX);
    updateStripScrollbar();
}

void SKInventoryColumnsView::updateStripScrollbar()
{
    if (!mStripScrollbar)
    {
        return;
    }
    const S32 view = getRect().getWidth();
    const S32 total = mColumns.empty() ? 0 : mColumns.back().mLeft + mColumns.back().mWidth;
    // A narrower strip gets empty room at its end rather than clamping the scroll.
    mStripPad = llmax(0, mStripX + view - total);
    mStripScrollbar->setDocSize(total + mStripPad);
    mStripScrollbar->setPageSize(view);
    mStripScrollbar->setDocPos(mStripX);
    mStripScrollbar->setVisible(total + mStripPad > view);
}

void SKInventoryColumnsView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    updateStripScrollbar();
}

S32 SKInventoryColumnsView::stripBar() const
{
    return mStripScrollbar && mStripScrollbar->getVisible() ? LLScrollbar::THIN_THICKNESS : 0;
}

S32 SKInventoryColumnsView::rowsPerPage() const
{
    const S32 bar = stripBar();
    return llmax(1, (getRect().getHeight() - bar - TOP_PAD) / ROW_HEIGHT);
}

S32 SKInventoryColumnsView::columnScroll(const Column& column) const
{
    auto it = mScrolls.find(column.mFolderID);
    const S32 rows = (S32)column.mRows.size() + (column.mMore > 0 ? 1 : 0);
    const S32 first = it != mScrolls.end() ? it->second : 0;
    return llclamp(first, 0, llmax(0, rows - rowsPerPage()));
}

void SKInventoryColumnsView::setColumnScroll(const Column& column, S32 first)
{
    const S32 rows = (S32)column.mRows.size() + (column.mMore > 0 ? 1 : 0);
    first = llclamp(first, 0, llmax(0, rows - rowsPerPage()));
    if (first == 0)
    {
        mScrolls.erase(column.mFolderID);
    }
    else
    {
        mScrolls[column.mFolderID] = first;
    }
}

void SKInventoryColumnsView::scrollRowIntoView(const Column& column, S32 row)
{
    const S32 first = columnScroll(column);
    const S32 page = rowsPerPage();
    if (row < first)
    {
        setColumnScroll(column, row);
    }
    else if (row >= first + page)
    {
        setColumnScroll(column, row - page + 1);
    }
}

S32 SKInventoryColumnsView::columnAt(S32 x) const
{
    const S32 strip_x = x + mStripX;
    for (S32 i = 0; i < (S32)mColumns.size(); ++i)
    {
        const Column& column = mColumns[i];
        if (column.mWidth > 0 && strip_x >= column.mLeft && strip_x < column.mLeft + column.mWidth)
        {
            return i;
        }
    }
    return -1;
}

S32 SKInventoryColumnsView::rowAt(const Column& column, S32 y) const
{
    const S32 bar = stripBar();
    const S32 below_top = getRect().getHeight() - TOP_PAD - y;
    if (below_top < 0 || y < bar)
    {
        return -1;
    }
    const S32 row = columnScroll(column) + below_top / ROW_HEIGHT;
    return row < (S32)column.mRows.size() ? row : -1;
}

uuid_vec_t SKInventoryColumnsView::getSelection() const
{
    uuid_vec_t ids;
    const Column* current = currentColumn();
    if (current)
    {
        for (size_t r = 0; r < current->mRows.size() && r < mSelected.size(); ++r)
        {
            if (mSelected[r])
            {
                ids.push_back(current->mRows[r].mID);
            }
        }
    }
    return ids;
}

void SKInventoryColumnsView::setSelection(const uuid_vec_t& ids)
{
    const Column* current = currentColumn();
    const size_t count = current ? current->mRows.size() : 0;
    mSelected.assign(count, false);
    mCursor = -1;
    for (size_t r = 0; r < count; ++r)
    {
        if (std::find(ids.begin(), ids.end(), current->mRows[r].mID) != ids.end())
        {
            mSelected[r] = true;
            mCursor = (S32)r;
        }
    }
    mAnchor = mCursor;
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
    if (current && mCursor >= 0)
    {
        scrollRowIntoView(*current, mCursor);
    }
    rebuildTrailing();
    revealColumns(mSelectedCount > 0);
}

void SKInventoryColumnsView::selectFirstRow()
{
    const Column* current = currentColumn();
    if (current && !current->mRows.empty() && mSelectedCount == 0)
    {
        moveCursor(0, MASK_NONE);
    }
}

void SKInventoryColumnsView::notifySelection(bool reveal)
{
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
    rebuildTrailing();
    revealColumns(reveal);
    if (mSelectionCallback)
    {
        mSelectionCallback();
    }
}

void SKInventoryColumnsView::clearSelection()
{
    mSelected.assign(mSelected.size(), false);
    mCursor = -1;
    mAnchor = -1;
    notifySelection(false);
}

void SKInventoryColumnsView::selectRange(S32 from, S32 to)
{
    mSelected.assign(mSelected.size(), false);
    const Column* current = currentColumn();
    for (S32 i = llmax(0, llmin(from, to)); current && i <= llmax(from, to) && i < (S32)mSelected.size(); ++i)
    {
        mSelected[i] = current->mRows[i].isEntry();
    }
}

void SKInventoryColumnsView::moveCursor(S32 row, MASK mask)
{
    const Column* current = currentColumn();
    if (!current || current->mRows.empty())
    {
        return;
    }
    row = llclamp(row, 0, (S32)current->mRows.size() - 1);
    // A header is stepped over, in the direction of travel.
    const S32 step = row < mCursor ? -1 : 1;
    while (row >= 0 && row < (S32)current->mRows.size() && !current->mRows[row].isEntry())
    {
        row += step;
    }
    if (row < 0 || row >= (S32)current->mRows.size())
    {
        row -= step;
        while (row >= 0 && row < (S32)current->mRows.size() && !current->mRows[row].isEntry())
        {
            row -= step;
        }
        if (row < 0 || row >= (S32)current->mRows.size())
        {
            return;
        }
    }
    if ((mask & MASK_SHIFT) && mAnchor >= 0)
    {
        selectRange(mAnchor, row);
    }
    else
    {
        selectRange(row, row);
        mAnchor = row;
    }
    mCursor = row;
    scrollRowIntoView(*current, row);
    notifySelection(true);
}

bool SKInventoryColumnsView::selectRow(S32 column, S32 row, MASK mask)
{
    if (!mColumns[column].mRows[row].isEntry())
    {
        return true;
    }
    if (column != mCurrent)
    {
        // Another column becomes current, with the row selected; not a history entry.
        const LLUUID folder_id = mColumns[column].mFolderID;
        const LLUUID row_id = mColumns[column].mRows[row].mID;
        if (mMoveCallback)
        {
            mMoveCallback(folder_id);
        }
        const Column* current = currentColumn();
        if (mFolderID != folder_id || !current)
        {
            return true;
        }
        for (S32 r = 0; r < (S32)current->mRows.size(); ++r)
        {
            if (current->mRows[r].mID == row_id)
            {
                return selectRow(mCurrent, r, mask);
            }
        }
        return true;
    }
    if (mask & MASK_CONTROL)
    {
        mSelected[row] = !mSelected[row];
        mAnchor = row;
        mCursor = row;
        notifySelection(true);
    }
    else
    {
        moveCursor(row, mask);
    }
    return true;
}

void SKInventoryColumnsView::openRow(const SKInventoryRow& row)
{
    if (!row.isEntry())
    {
        return;
    }
    const LLUUID id = row.mID;
    const LLUUID target = row.mTargetID;
    const bool is_folder = row.mIsFolder;
    if (mOpenCallback)
    {
        mOpenCallback(id);
    }
    if (is_folder && mFolderID == target)
    {
        selectFirstRow();
    }
}

void SKInventoryColumnsView::draw()
{
    if (mDirty)
    {
        rebuild(false);
    }
    fetchGhostWhenResting();
    updateColumnScrollbars();

    const S32 bar = stripBar();
    const S32 height = getRect().getHeight();
    const bool focused = hasFocus();
    const S32 view = getRect().getWidth();
    mDetailsPlaced = false;
    {
        LLLocalClipRect clip(LLRect(0, height, view, bar));
        for (S32 i = 0; i < (S32)mColumns.size(); ++i)
        {
            const Column& column = mColumns[i];
            if (column.mWidth > 0 && column.mLeft + column.mWidth > mStripX && column.mLeft < mStripX + view)
            {
                drawColumn(column, i, height, focused);
            }
        }
        drawDropTarget(height);
    }
    if (!mDetailsPlaced && mDetailsView->getVisible())
    {
        mDetailsView->setVisible(false);
    }
    LLUICtrl::draw();
}

void SKInventoryColumnsView::fetchGhostWhenResting()
{
    constexpr F32 REST_SECONDS = 0.25f;
    if (!mGhostFetchPending || mGhostRestTimer.getElapsedTimeF32() < REST_SECONDS)
    {
        return;
    }
    mGhostFetchPending = false;
    const bool shown = !mColumns.empty() && mColumns.back().mKind == KIND_GHOST && mColumns.back().mFolderID == mGhostCache.mFolderID;
    if (shown && mGhostCache.mLoading)
    {
        LLInventoryModelBackgroundFetch::instance().start(mGhostCache.mFolderID, false);
    }
}

void SKInventoryColumnsView::drawColumn(const Column& column, S32 index, S32 height, bool focused)
{
    static LLUIColor sel_color = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static LLUIColor path_color = LLUIColorTable::instance().getColor("LtGray_35");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor rule_color = LLUIColorTable::instance().getColor("White_10");
    static LLPointer<LLUIImage> arrow = LLUI::getUIImage("Arrow_Small_Right");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 left = column.mLeft - mStripX;
    const S32 right = left + column.mWidth;
    LLLocalClipRect clip(LLRect(left, height, right, 0));

    gl_line_2d(right - 1, height, right - 1, 0, rule_color.get());

    if (column.mKind == KIND_DETAILS)
    {
        drawDetails(column, height);
        return;
    }
    if (column.mKind == KIND_SPARE)
    {
        return;
    }

    if (column.mRows.empty())
    {
        const std::string& message = column.mLoading ? mLabels.mLoading : mLabels.mEmpty;
        font->renderUTF8(message, 0, left + 2 * CELL_PAD, height - TOP_PAD - ROW_HEIGHT / 2, dim_color.get(),
                         LLFontGL::LEFT, LLFontGL::VCENTER);
        return;
    }

    const bool is_current = index == mCurrent;
    LLColor4 selected = sel_color.get();
    if (!focused)
    {
        selected.mV[VALPHA] *= 0.5f;
    }

    // Only the rows in view are drawn.
    const S32 first = columnScroll(column);
    const S32 rows = (S32)column.mRows.size();
    const S32 last = llmin(rows + (column.mMore > 0 ? 1 : 0), first + rowsPerPage() + 1);
    // The pictures in view and half a page either side are fetched at their smallest ahead of a selection.
    const S32 margin = rowsPerPage() / 2;
    for (S32 r = llmax(0, first - margin); r < llmin(rows, last + margin); ++r)
    {
        SKInventoryFlatView::prefetchPicture(column.mRows[r]);
    }
    // A column that scrolls leaves its bar a column of its own.
    const S32 inner_right = right - ((rows + (column.mMore > 0 ? 1 : 0)) > rowsPerPage() ? 1 + LLScrollbar::THIN_THICKNESS : 0);
    for (S32 r = first; r < last; ++r)
    {
        const S32 top = height - TOP_PAD - (r - first) * ROW_HEIGHT;
        const S32 middle = top - ROW_HEIGHT / 2;
        if (r >= rows)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", column.mMore);
            std::string more = mLabels.mMore;
            LLStringUtil::format(more, args);
            font->renderUTF8(more, 0, left + 2 * CELL_PAD, middle, dim_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER);
            continue;
        }

        const SKInventoryRow& row = column.mRows[r];
        if (row.mKind == SKInventoryRow::KIND_HEADER)
        {
            static LLUIColor bright_color = LLUIColorTable::instance().getColor("White");
            font->renderUTF8(row.mName, 0, (F32)(left + CELL_PAD), (F32)middle, bright_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER,
                             LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX, inner_right - left - 40, nullptr, true);
            const S32 name_width = llmin((S32)LLFontGL::getFontSansSerifSmallBold()->getWidthF32(row.mName), inner_right - left - 40);
            font->renderUTF8(llformat("%d", row.mGroupCount), 0, left + 2 * CELL_PAD + name_width, middle, dim_color.get(),
                             LLFontGL::LEFT, LLFontGL::VCENTER);
            gl_line_2d(left + CELL_PAD, top - ROW_HEIGHT + 1, inner_right - CELL_PAD, top - ROW_HEIGHT + 1, rule_color.get());
            continue;
        }
        if (is_current && r < (S32)mSelected.size() && mSelected[r])
        {
            gl_rect_2d(left, top, inner_right - 1, top - ROW_HEIGHT, selected);
        }
        else if (is_current && focused && r == mCursor)
        {
            // The keyboard cursor on an unselected row: a faint fill.
            static LLUIColor cursor_color = LLUIColorTable::instance().getColor("White_10");
            gl_rect_2d(left, top, inner_right - 1, top - ROW_HEIGHT, cursor_color.get());
        }
        else if (row.mID == column.mPathID)
        {
            gl_rect_2d(left, top, inner_right - 1, top - ROW_HEIGHT, path_color.get());
        }

        S32 x = left + CELL_PAD;
        if (row.mIcon.notNull())
        {
            row.mIcon->draw(x, middle - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE);
            if (row.hasLinkBadge())
            {
                SKInventoryFlatView::drawLinkBadge(LLRect(x, middle + ICON_SIZE / 2, x + ICON_SIZE, middle - ICON_SIZE / 2));
            }
            x += ICON_SIZE + CELL_PAD;
        }
        S32 text_right = inner_right - CELL_PAD;
        if (row.mIsFolder && !row.mIsLink && arrow.notNull())
        {
            text_right -= arrow->getWidth() + CELL_PAD;
            arrow->draw(inner_right - CELL_PAD - arrow->getWidth(), middle - arrow->getHeight() / 2);
        }

        // The ghost's text is dim, brightening under the pointer.
        LLColor4 color = text_color.get();
        U8 style = LLFontGL::NORMAL;
        if (row.mIsLink)
        {
            color = dim_color.get();
            style = LLFontGL::ITALIC;
        }
        font->renderUTF8(row.mName, 0, (F32)x, (F32)middle, color, LLFontGL::LEFT, LLFontGL::VCENTER, style,
                         LLFontGL::NO_SHADOW, S32_MAX, llmax(0, text_right - x), nullptr, LLFontGL::ELLIPSIS_MIDDLE);
    }
}

void SKInventoryColumnsView::updateColumnScrollbars()
{
    // One thin bar a scrolling column, laid over its right edge; the pool only grows.
    const S32 height = getRect().getHeight();
    const S32 view = getRect().getWidth();
    const S32 page = rowsPerPage();
    const S32 thickness = LLScrollbar::THIN_THICKNESS;
    size_t used = 0;
    for (S32 i = 0; i < (S32)mColumns.size(); ++i)
    {
        const Column& column = mColumns[i];
        const bool rows_kind = column.mKind == KIND_PATH || column.mKind == KIND_GHOST;
        const S32 total = (S32)column.mRows.size() + (column.mMore > 0 ? 1 : 0);
        const S32 left = column.mLeft - mStripX;
        if (!rows_kind || column.mWidth <= 0 || total <= page || left + column.mWidth <= 0 || left >= view)
        {
            continue;
        }
        if (used == mColumnScrollbars.size())
        {
            LLScrollbar::Params sbparams;
            sbparams.name("column_scrollbar");
            sbparams.rect(LLRect(0, height, thickness, 0));
            sbparams.orientation(LLScrollbar::VERTICAL);
            sbparams.doc_size(0);
            sbparams.doc_pos(0);
            sbparams.page_size(1);
            sbparams.thin(true);
            sbparams.follows.flags(FOLLOWS_NONE);
            const size_t slot = used;
            sbparams.change_callback([this, slot](S32 pos, LLScrollbar*)
                                     {
                                         if (slot < mColumnScrollbarFolders.size())
                                         {
                                             for (const Column& c : mColumns)
                                             {
                                                 if (c.mFolderID == mColumnScrollbarFolders[slot])
                                                 {
                                                     setColumnScroll(c, pos);
                                                     break;
                                                 }
                                             }
                                         }
                                     });
            LLScrollbar* bar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
            addChild(bar);
            mColumnScrollbars.push_back(bar);
            mColumnScrollbarFolders.push_back(LLUUID::null);
        }
        LLScrollbar* bar = mColumnScrollbars[used];
        mColumnScrollbarFolders[used] = column.mFolderID;
        const LLRect rect(left + column.mWidth - thickness - 1, height - TOP_PAD, left + column.mWidth - 1, stripBar());
        if (bar->getRect() != rect)
        {
            bar->setShape(rect);
        }
        bar->setDocSize(total);
        bar->setPageSize(page);
        if (bar->getDocPos() != columnScroll(column))
        {
            bar->setDocPos(columnScroll(column));
        }
        bar->setVisible(true);
        ++used;
    }
    for (size_t i = used; i < mColumnScrollbars.size(); ++i)
    {
        mColumnScrollbars[i]->setVisible(false);
        mColumnScrollbarFolders[i].setNull();
    }
}

void SKInventoryColumnsView::drawDetails(const Column& column, S32 height)
{
    // The details view is a child laid over the column, drawn with the children.
    const S32 left = column.mLeft - mStripX;
    mDetailsView->setRect(LLRect(left, height, left + column.mWidth - 1, stripBar()));
    uuid_vec_t ids;
    for (const SKInventoryRow& row : column.mRows)
    {
        ids.push_back(row.mID);
    }
    mDetailsView->setSelection(ids);
    mDetailsView->setVisible(true);
    mDetailsPlaced = true;
}

bool SKInventoryColumnsView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    setFocus(true);
    const S32 index = columnAt(x);
    if (index < 0)
    {
        clearSelection();
        return true;
    }

    const Column& column = mColumns[index];
    if (column.mKind == KIND_PATH)
    {
        const S32 row = rowAt(column, y);
        if (row >= 0)
        {
            const bool entry = column.mRows[row].isEntry();
            const LLUUID row_id = column.mRows[row].mID;
            const bool is_current = index == mCurrent;
            const bool selected = is_current && row < (S32)mSelected.size() && mSelected[row] && !(mask & (MASK_CONTROL | MASK_SHIFT));
            const std::vector<bool> before = is_current ? mSelected : std::vector<bool>();
            if (entry)
            {
                make_ui_sound("UISndClick");
            }
            if (!selected)
            {
                selectRow(index, row, mask);
            }
            if (entry)
            {
                pressRow(x, y, mask, row_id, selected, before);
            }
            return true;
        }
        if (index != mCurrent)
        {
            // Empty space in another column makes it current, with nothing selected.
            const LLUUID folder_id = column.mFolderID;
            if (mMoveCallback)
            {
                mMoveCallback(folder_id);
            }
        }
        clearSelection();
        revealColumns(false);
    }
    else if (column.mKind == KIND_GHOST)
    {
        // A click in the ghost moves into it and selects there.
        const S32 row = rowAt(column, y);
        if (row >= 0)
        {
            make_ui_sound("UISndClick");
            const LLUUID folder_id = column.mFolderID;
            const LLUUID row_id = column.mRows[row].mID;
            const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
            // A folder link's target outside the path is a navigation, recorded in history.
            const bool elsewhere = !cat || cat->getParentUUID() != mFolderID;
            if (elsewhere && mOpenCallback)
            {
                mOpenCallback(folder_id);
            }
            else if (mMoveCallback)
            {
                mMoveCallback(folder_id);
            }
            if (mFolderID == folder_id)
            {
                setSelection({ row_id });
                if (mSelectionCallback)
                {
                    mSelectionCallback();
                }
                pressRow(x, y, MASK_NONE, row_id, false, std::vector<bool>());
            }
        }
    }
    return true;
}

bool SKInventoryColumnsView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (childrenHandleDoubleClick(x, y, mask))
    {
        return true;
    }
    const S32 index = columnAt(x);
    if (index < 0)
    {
        return true;
    }
    const Column& column = mColumns[index];
    if (column.mKind == KIND_PATH || column.mKind == KIND_GHOST)
    {
        const S32 row = rowAt(column, y);
        if (row >= 0)
        {
            const SKInventoryRow target = column.mRows[row];
            openRow(target);
        }
    }
    return true;
}

bool SKInventoryColumnsView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleRightMouseDown(x, y, mask))
    {
        return true;
    }
    setFocus(true);
    const S32 index = columnAt(x);
    LLUUID menu_folder = mFolderID;
    if (index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST))
    {
        const Column& column = mColumns[index];
        const S32 row = rowAt(column, y);
        if (row >= 0 && column.mRows[row].isEntry())
        {
            make_ui_sound("UISndClick");
            if (index == mCurrent)
            {
                if (!mSelected[row])
                {
                    moveCursor(row, MASK_NONE);
                }
            }
            else if (column.mKind == KIND_GHOST)
            {
                handleMouseDown(x, y, MASK_NONE);
            }
            else
            {
                selectRow(index, row, MASK_NONE);
            }
            if (mMenuCallback)
            {
                mMenuCallback(x, y, getSelection(), mFolderID);
            }
            return true;
        }
        // Empty space gives that column's folder menu.
        menu_folder = column.mFolderID;
        if (index == mCurrent)
        {
            clearSelection();
        }
    }
    else
    {
        clearSelection();
    }
    if (mMenuCallback)
    {
        mMenuCallback(x, y, uuid_vec_t(), menu_folder);
    }
    return true;
}

bool SKInventoryColumnsView::handleHover(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture() && mDetailsView->getVisible() && mDetailsView->getRect().pointInRect(x, y)
        && childrenHandleHover(x, y, mask))
    {
        return true;
    }
    if (hasMouseCapture() && mPress != PRESS_NONE)
    {
        const S32 dx = x - mPressX;
        const S32 dy = y - mPressY;
        if ((mPress == PRESS_ITEM || mPress == PRESS_DECIDE)
            && dx * dx + dy * dy >= SKInventoryExplorerDrag::DRAG_THRESHOLD * SKInventoryExplorerDrag::DRAG_THRESHOLD)
        {
            if (mPress == PRESS_DECIDE && llabs(dy) >= llabs(dx))
            {
                mPress = PRESS_SWEEP;
                mSweepTick.reset();
            }
            else
            {
                const uuid_vec_t ids = getSelection();
                mPress = PRESS_NONE;
                gFocusMgr.setMouseCapture(nullptr);
                if (SKInventoryExplorerDrag::begin(ids))
                {
                    return LLToolDragAndDrop::getInstance()->handleHover(x, y, mask);
                }
                return true;
            }
        }
        if (mPress == PRESS_SWEEP)
        {
            sweepTo(y);
            // Past the column's top or bottom, it scrolls a row a step.
            const S32 band = SKInventoryExplorerDrag::SCROLL_BAND;
            const S32 dir = y > getRect().getHeight() - band ? -1 : y < band ? 1 : 0;
            const Column* current = currentColumn();
            if (dir != 0 && current && mSweepTick.getElapsedTimeF32() >= SKInventoryExplorerDrag::TICK_SECONDS)
            {
                mSweepTick.reset();
                setColumnScroll(*current, columnScroll(*current) + dir);
                sweepTo(y);
            }
        }
        getWindow()->setCursor(UI_CURSOR_ARROW);
        return true;
    }
    mHoverColumn = columnAt(x);
    mHoverRow = mHoverColumn >= 0 ? rowAt(mColumns[mHoverColumn], y) : -1;
    return LLUICtrl::handleHover(x, y, mask);
}

void SKInventoryColumnsView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverColumn = -1;
    mHoverRow = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKInventoryColumnsView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    if (childrenHandleScrollWheel(x, y, delta))
    {
        return true;
    }
    const S32 index = columnAt(x);
    if (index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST))
    {
        const Column& column = mColumns[index];
        setColumnScroll(column, columnScroll(column) + delta.mClicks * WHEEL_ROWS);
        return true;
    }
    return handleScrollHWheel(x, y, delta);
}

bool SKInventoryColumnsView::handleScrollHWheel(S32 x, S32 y, LLScrollDelta delta)
{
    const S32 total = mColumns.empty() ? 0 : mColumns.back().mLeft + mColumns.back().mWidth;
    const S32 max_x = llmax(0, total - getRect().getWidth());
    mStripX = llclamp(mStripX + delta.mClicks * HWHEEL_PIXELS, 0, llmax(max_x, mStripX));
    updateStripScrollbar();
    return true;
}

bool SKInventoryColumnsView::handleKeyHere(KEY key, MASK mask)
{
    if (mask & MASK_ALT)
    {
        return false;
    }
    const Column* current = currentColumn();
    const S32 last = current ? (S32)current->mRows.size() - 1 : -1;
    const S32 page = rowsPerPage();
    switch (key)
    {
        case KEY_UP:
            moveCursor((mask & MASK_CONTROL) || mCursor < 0 ? 0 : mCursor - 1, mask);
            return true;
        case KEY_DOWN:
            moveCursor((mask & MASK_CONTROL) ? last : (mCursor < 0 ? 0 : mCursor + 1), mask);
            return true;
        case KEY_HOME:
            moveCursor(0, mask);
            return true;
        case KEY_END:
            moveCursor(last, mask);
            return true;
        case KEY_PAGE_UP:
            moveCursor(mCursor < 0 ? 0 : mCursor - page, mask);
            return true;
        case KEY_PAGE_DOWN:
            moveCursor(mCursor < 0 ? 0 : mCursor + page, mask);
            return true;
        case KEY_RIGHT:
            if (mask == MASK_NONE && current && mCursor >= 0 && mCursor <= last && current->mRows[mCursor].mIsFolder)
            {
                const SKInventoryRow target = current->mRows[mCursor];
                openRow(target);
            }
            return true;
        case KEY_LEFT:
            if (mask != MASK_NONE)
            {
                return true;
            }
            if (mCurrent <= 0)
            {
                // From the first column, ← goes back into the sidebar.
                if (mSelectedCount > 0)
                {
                    clearSelection();
                }
                if (mSidebarCallback)
                {
                    mSidebarCallback();
                }
            }
            else if (mUpCallback)
            {
                mUpCallback();
            }
            return true;
        case KEY_RETURN:
            if (mask == MASK_NONE && current && mCursor >= 0 && mCursor <= last)
            {
                const SKInventoryRow target = current->mRows[mCursor];
                openRow(target);
                return true;
            }
            break;
        case KEY_BACKSPACE:
            if (mask == MASK_NONE && mUpCallback)
            {
                mUpCallback();
                return true;
            }
            break;
        case KEY_ESCAPE:
            if (mPress == PRESS_SWEEP)
            {
                mPress = PRESS_NONE;
                if (mSweepRestore.size() == mSelected.size())
                {
                    mSelected = mSweepRestore;
                }
                notifySelection(false);
                gFocusMgr.setMouseCapture(nullptr);
                return true;
            }
            if (mSelectedCount > 0)
            {
                clearSelection();
                return true;
            }
            break;
        case KEY_DELETE:
            if (canDoDelete())
            {
                doDelete();
            }
            return true;
        case KEY_F2:
        {
            const uuid_vec_t selection = getSelection();
            if (selection.size() == 1)
            {
                startRename(selection.front());
            }
            return true;
        }
        case 'A':
            if (mask == MASK_CONTROL)
            {
                selectAll();
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

bool SKInventoryColumnsView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 index = columnAt(x);
    if (index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST))
    {
        const S32 row = rowAt(mColumns[index], y);
        if (row >= 0)
        {
            LLToolTipMgr::instance().show(mColumns[index].mRows[row].mName);
            return true;
        }
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

void SKInventoryColumnsView::onFocusReceived()
{
    gEditMenuHandler = this;
    LLUICtrl::onFocusReceived();
}

void SKInventoryColumnsView::onFocusLost()
{
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
    LLUICtrl::onFocusLost();
}

void SKInventoryColumnsView::doDelete()
{
    SKInventoryExplorerActions::deleteObjects(getSelection());
}

bool SKInventoryColumnsView::canDoDelete() const
{
    return SKInventoryExplorerActions::canDelete(getSelection(), mFolderID);
}

void SKInventoryColumnsView::selectAll()
{
    if (currentColumn())
    {
        const Column* current = currentColumn();
        for (size_t r = 0; r < mSelected.size(); ++r)
        {
            mSelected[r] = current->mRows[r].isEntry();
        }
        notifySelection(true);
    }
}

bool SKInventoryColumnsView::canSelectAll() const
{
    const Column* current = currentColumn();
    return current && !current->mRows.empty();
}

void SKInventoryColumnsView::deselect()
{
    clearSelection();
}

void SKInventoryColumnsView::setGroup(SKInventoryFlatView::EGroup group)
{
    if (group != mGroup)
    {
        mGroup = group;
        mDirty = true;
    }
}

void SKInventoryColumnsView::startRename(const LLUUID& id)
{
    SKInventoryRename::finish(mRenameEditor);
    const Column* current = currentColumn();
    if (!current)
    {
        return;
    }
    for (S32 r = 0; r < (S32)current->mRows.size(); ++r)
    {
        if (current->mRows[r].isEntry() && current->mRows[r].mID == id)
        {
            scrollRowIntoView(*current, r);
            revealColumns(false);
            const S32 top = getRect().getHeight() - TOP_PAD - (r - columnScroll(*current)) * ROW_HEIGHT;
            const S32 left = current->mLeft - mStripX + CELL_PAD + ICON_SIZE + CELL_PAD - 2;
            const S32 text = (S32)LLFontGL::getFontSansSerifSmall()->getWidthF32(current->mRows[r].mName) + 2 * CELL_PAD;
            const S32 width = llmax(120, llmin(text, current->mWidth - (left - (current->mLeft - mStripX)) - CELL_PAD));
            mRenameEditor = SKInventoryRename::start(this, id, LLRect(left, top, left + width, top - ROW_HEIGHT), false, nullptr);
            return;
        }
    }
}

void SKInventoryColumnsView::pressRow(S32 x, S32 y, MASK mask, const LLUUID& row_id, bool was_selected, const std::vector<bool>& before)
{
    // The row pressed, now in the current column, waits to see whether it is dragged or swept.
    const Column* current = currentColumn();
    if (!current)
    {
        return;
    }
    S32 row = -1;
    for (S32 r = 0; r < (S32)current->mRows.size(); ++r)
    {
        if (current->mRows[r].mID == row_id)
        {
            row = r;
            break;
        }
    }
    if (row < 0)
    {
        return;
    }
    mPressX = x;
    mPressY = y;
    mPressRow = row;
    mPressMask = mask;
    mPressCollapse = was_selected && mSelectedCount > 1;
    mPress = was_selected ? PRESS_ITEM : PRESS_DECIDE;
    mSweepRestore = before.size() == current->mRows.size() ? before : mSelected;
    mSweepBase = (mask & (MASK_CONTROL | MASK_SHIFT)) ? mSweepRestore : std::vector<bool>(current->mRows.size(), false);
    mSweepLow = mSweepHigh = -1;
    gFocusMgr.setMouseCapture(this);
}

void SKInventoryColumnsView::sweepTo(S32 y)
{
    const Column* current = currentColumn();
    if (!current || current->mRows.empty() || mSweepBase.size() != current->mRows.size())
    {
        return;
    }
    const S32 below_top = getRect().getHeight() - TOP_PAD - y;
    const S32 at = llclamp(columnScroll(*current) + (below_top >= 0 ? below_top / ROW_HEIGHT : -1), 0, (S32)current->mRows.size() - 1);
    const S32 low = llmin(mPressRow, at);
    const S32 high = llmax(mPressRow, at);
    if (low == mSweepLow && high == mSweepHigh)
    {
        return;
    }
    // Rows leaving the range go back to the base, rows in it take the opposite; the cost is the range's.
    for (S32 r = mSweepLow; r >= 0 && r <= mSweepHigh; ++r)
    {
        mSelected[r] = mSweepBase[r];
    }
    for (S32 r = low; r <= high; ++r)
    {
        mSelected[r] = current->mRows[r].isEntry() && !mSweepBase[r];
    }
    mSweepLow = low;
    mSweepHigh = high;
    mCursor = at;
    mAnchor = mPressRow;
    notifySelection(false);
}

bool SKInventoryColumnsView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return LLUICtrl::handleMouseUp(x, y, mask);
    }
    const EPress press = mPress;
    mPress = PRESS_NONE;
    gFocusMgr.setMouseCapture(nullptr);
    if (press == PRESS_ITEM && mPressCollapse)
    {
        moveCursor(mPressRow, MASK_NONE);
    }
    return true;
}

void SKInventoryColumnsView::onMouseCaptureLost()
{
    mPress = PRESS_NONE;
}

bool SKInventoryColumnsView::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                               EAcceptance* accept, std::string& tooltip_msg)
{
    if (childrenHandleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg))
    {
        return true;
    }
    // A folder row, or a column's own space (its folder); past the columns, the open folder.
    const S32 index = columnAt(x);
    S32 row = -1;
    LLUUID dest = mFolderID;
    if (index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST))
    {
        const Column& column = mColumns[index];
        dest = column.mFolderID;
        row = rowAt(column, y);
        if (row >= 0 && column.mRows[row].isEntry() && column.mRows[row].mIsFolder)
        {
            dest = column.mRows[row].mTargetID;
        }
        else
        {
            row = -1;
        }
    }
    SKInventoryExplorerDrag::dropOnFolder(dest, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    mDropColumn = row >= 0 || (index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST)) ? index : -1;
    mDropRow = row;
    const S32 height = getRect().getHeight();
    LLRect mark_rect(0, height, getRect().getWidth(), 0);
    if (mDropColumn >= 0)
    {
        const Column& column = mColumns[mDropColumn];
        const S32 left = column.mLeft - mStripX;
        mark_rect = LLRect(left, height, left + column.mWidth - 1, 0);
        if (row >= 0)
        {
            const S32 top = height - TOP_PAD - (row - columnScroll(column)) * ROW_HEIGHT;
            mark_rect = LLRect(left, top, left + column.mWidth - 1, top - ROW_HEIGHT);
        }
    }
    SKInventoryExplorerDrag::mark(this, mark_rect);
    if (drop || LLToolDragAndDrop::getInstance()->getCargoIndex() != 0)
    {
        return true;
    }
    mDropHover.touch();

    S32 screen_x = 0;
    S32 screen_y = 0;
    localPointToScreen(x, y, &screen_x, &screen_y);
    const LLUUID spring_id = row >= 0 && !SKInventoryExplorerDrag::verdict().mOntoCargo ? mColumns[index].mRows[row].mID : LLUUID::null;
    if (mDropHover.spring(spring_id, screen_x, screen_y, SKInventoryExplorerDrag::springSeconds()) && mOpenCallback)
    {
        mOpenCallback(spring_id);
        return true;
    }

    // The column under the pointer scrolls near its top and bottom, under a glow.
    const bool scrolls = index >= 0 && (mColumns[index].mKind == KIND_PATH || mColumns[index].mKind == KIND_GHOST);
    const S32 band = SKInventoryExplorerDrag::SCROLL_BAND;
    const S32 rows_dir = !scrolls ? 0 : y > getRect().getHeight() - band ? -1 : y < band ? 1 : 0;
    if (rows_dir == 0 || index != mGlowColumn)
    {
        mColumnGlow.hover(0, false);
    }
    if (rows_dir != 0)
    {
        mGlowColumn = index;
    }
    if (rows_dir != 0 && mDropHover.tick(SKInventoryExplorerDrag::TICK_SECONDS))
    {
        mDropScrollResidue += SKInventoryExplorerDrag::SCROLL_STEP;
        const S32 rows = mDropScrollResidue / ROW_HEIGHT;
        mDropScrollResidue %= ROW_HEIGHT;
        const Column& column = mColumns[index];
        const S32 before = columnScroll(column);
        setColumnScroll(column, before + rows_dir * rows);
        const bool moved = columnScroll(column) != before;
        if (moved)
        {
            mDropHover.scrolled(screen_x, screen_y);
        }
        mColumnGlow.hover(-rows_dir, moved || rows == 0);
    }

    // Resting near the strip's left or right edge scrolls it that way, under a glow.
    const S32 view = getRect().getWidth();
    const S32 total = mColumns.empty() ? 0 : mColumns.back().mLeft + mColumns.back().mWidth;
    S32 dir = x < EDGE_REACH ? -1 : x >= view - EDGE_REACH ? 1 : 0;
    if ((dir < 0 && mStripX <= 0) || (dir > 0 && mStripX + view >= total))
    {
        dir = 0;
    }
    if (dir != mEdgeDir)
    {
        mEdgeDir = dir;
        mEdgeTimer.reset();
    }
    mEdgeScrolling = false;
    const F32 rested = mEdgeTimer.getElapsedTimeF32() - EDGE_DELAY;
    if (dir != 0 && rested > 0.f)
    {
        mEdgeScrolling = true;
        if (mEdgeTick.getElapsedTimeF32() >= SKInventoryExplorerDrag::TICK_SECONDS)
        {
            mEdgeTick.reset();
            const S32 step = (S32)llmin(24.f, 6.f + rested * 1000.f / 40.f);
            mStripX = llclamp(mStripX + dir * step, 0, llmax(0, total - view));
            updateStripScrollbar();
            mDropHover.scrolled(screen_x, screen_y);
        }
    }
    return true;
}

void SKInventoryColumnsView::drawDropTarget(S32 height)
{
    const bool active = mDropHover.isActive();
    mStripGlow.hover(mEdgeDir, mEdgeScrolling);
    mStripGlow.draw(LLRect(0, height, getRect().getWidth(), 0), true, active);
    if (mGlowColumn >= 0 && mGlowColumn < (S32)mColumns.size())
    {
        const Column& column = mColumns[mGlowColumn];
        const S32 left = column.mLeft - mStripX;
        mColumnGlow.draw(LLRect(left, height, left + column.mWidth, 0), false, active);
    }
    if (!active)
    {
        mEdgeDir = 0;
        return;
    }
    if (mDropColumn >= 0 && mDropColumn < (S32)mColumns.size())
    {
        const Column& column = mColumns[mDropColumn];
        const S32 left = column.mLeft - mStripX;
        LLLocalClipRect clip(LLRect(left, height, left + column.mWidth, 0));
        SKInventoryExplorerDrag::drawMark(this);
    }
    else
    {
        // The open folder's own space takes the outline alone.
        SKInventoryExplorerDrag::drawMark(this, false);
    }
}

void SKInventoryColumnsView::undo()
{
    SKInventoryExplorerDrag::undo();
}

bool SKInventoryColumnsView::canUndo() const
{
    return SKInventoryExplorerDrag::canUndo();
}
