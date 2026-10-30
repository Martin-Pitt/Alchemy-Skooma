/**
 * @file skinventorylistview.cpp
 * @brief Inventory Explorer's List view: one widget drawing a folder's rows from a flat model
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

#include "skinventorylistview.h"

#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "llui.h"

static LLDefaultChildRegistry::Register<SKInventoryListView> r_sk_inventory_list("sk_inventory_list");

namespace
{
    constexpr S32 ROW_HEIGHT = 20;
    constexpr S32 HEADER_HEIGHT = 20;
    constexpr S32 CELL_PAD = 4;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 RENAME_MIN_WIDTH = 120;
    constexpr S32 DEFAULT_WIDTHS[SKInventoryFlatView::COL_COUNT] = { 220, 160, 110, 130, 120, 50, 100, 220 };

    // Builds with smooth scrolling have LLScrollbar::setSmoothScroll; the List steps by whole rows there too.
    template <class T>
    auto step_by_rows(T* bar, int) -> decltype(bar->setSmoothScroll(false), void())
    {
        bar->setSmoothScroll(false);
    }

    template <class T>
    void step_by_rows(T*, long)
    {
    }
}

SKInventoryListView::SKInventoryListView(const Params& p)
:   SKInventoryFlatView(p),
    mSearching(false),
    mHeaderClicks(0)
{
    std::copy(std::begin(DEFAULT_WIDTHS), std::end(DEFAULT_WIDTHS), mColumnWidths);
    std::fill(std::begin(mHidden), std::end(mHidden), false);
    // Rows, not pixels: the wheel and page keys move instantly, with no glide.
    step_by_rows(mScrollbar, 0);
    updateColumns();
}

void SKInventoryListView::setSort(EColumn key, bool ascending)
{
    // A sort chosen anywhere but this header starts the header's cycle again.
    mHeaderClicks = 0;
    SKInventoryFlatView::setSort(key, ascending);
}

void SKInventoryListView::setHiddenColumns(const std::set<std::string>& keys)
{
    for (S32 i = 0; i < COL_COUNT; ++i)
    {
        mHidden[i] = i != COL_NAME && keys.contains(columnKey((EColumn)i));
    }
    updateColumns();
}

void SKInventoryListView::setSearching(bool searching)
{
    mSearching = searching;
    updateColumns();
}

void SKInventoryListView::updateColumns()
{
    mShown.clear();
    for (S32 i = 0; i < COL_COUNT; ++i)
    {
        if (!mHidden[i] && (i != COL_FOLDER || mSearching))
        {
            mShown.push_back((EColumn)i);
        }
    }
}

S32 SKInventoryListView::rowsPerPage() const
{
    return llmax(1, (getRect().getHeight() - HEADER_HEIGHT) / ROW_HEIGHT);
}

void SKInventoryListView::layoutRows()
{
    const S32 bar = LLScrollbar::THIN_THICKNESS;
    mScrollbar->setRect(LLRect(getRect().getWidth() - bar, getRect().getHeight() - HEADER_HEIGHT, getRect().getWidth(), 0));
    mScrollbar->setDocSize((S32)mRows.size());
    mScrollbar->setPageSize(rowsPerPage());
    mScrollbar->setVisible((S32)mRows.size() > rowsPerPage());
    mScrollbar->setDocPos(mScrollbar->getDocPos());
}

void SKInventoryListView::scrollToRow(S32 row)
{
    const S32 first = mScrollbar->getDocPos();
    const S32 page = rowsPerPage();
    if (row < first)
    {
        mScrollbar->setDocPos(row);
    }
    else if (row >= first + page)
    {
        mScrollbar->setDocPos(row - page + 1);
    }
}

S32 SKInventoryListView::rowFromKey(KEY key, MASK mask) const
{
    const S32 page = rowsPerPage();
    if (mCursor < 0)
    {
        return (key == KEY_LEFT || key == KEY_RIGHT) ? -1 : firstEntry();
    }
    S32 target = -1;
    switch (key)
    {
        case KEY_UP:        target = nextEntry(mCursor, -1); break;
        case KEY_DOWN:      target = nextEntry(mCursor, 1); break;
        case KEY_PAGE_UP:   target = nextEntry(llmax(0, mCursor - page) + 1, -1); break;
        case KEY_PAGE_DOWN: target = nextEntry(llmin((S32)mRows.size() - 1, mCursor + page) - 1, 1); break;
        default:            return -1; // ←/→ do nothing in the List
    }
    if (target < 0)
    {
        target = (key == KEY_UP || key == KEY_PAGE_UP) ? firstEntry() : lastEntry();
    }
    return target;
}

S32 SKInventoryListView::columnAt(S32 x) const
{
    S32 left = 0;
    for (EColumn col : mShown)
    {
        if (x >= left && x < left + mColumnWidths[col])
        {
            return col;
        }
        left += mColumnWidths[col];
    }
    return -1;
}

S32 SKInventoryListView::rowAt(S32 x, S32 y, bool* on_name) const
{
    const S32 below_header = getRect().getHeight() - HEADER_HEIGHT - y;
    if (below_header < 0)
    {
        return -1;
    }
    const S32 row = mScrollbar->getDocPos() + below_header / ROW_HEIGHT;
    if (row >= (S32)mRows.size())
    {
        return -1;
    }
    if (on_name)
    {
        bool multiline = false;
        *on_name = nameRect(row, multiline).pointInRect(x, y);
    }
    return row;
}

LLRect SKInventoryListView::nameRect(S32 row, bool& multiline) const
{
    multiline = false;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 top = getRect().getHeight() - HEADER_HEIGHT - (row - mScrollbar->getDocPos()) * ROW_HEIGHT;
    const S32 left = CELL_PAD + ICON_SIZE + CELL_PAD - 2;
    const S32 text = (S32)font->getWidthF32(mRows[row].mName) + 2 * CELL_PAD;
    const S32 width = llmax(RENAME_MIN_WIDTH, llmin(text, mColumnWidths[COL_NAME] - left));
    return LLRect(left, top, left + width, top - ROW_HEIGHT);
}

std::string SKInventoryListView::cellText(const SKInventoryRow& row, EColumn col) const
{
    switch (col)
    {
        case COL_NAME:        return row.mName;
        case COL_FOLDER:      return row.mFolderPath;
        case COL_TYPE:        return row.mType;
        case COL_CREATOR:     return row.mCreatorName;
        case COL_ACQUIRED:    return row.mAcquiredLabel;
        case COL_PERMS:       return row.mPerms;
        case COL_WORN:        return row.mWornOn;
        case COL_DESCRIPTION: return row.mDescription;
        default:              return std::string();
    }
}

void SKInventoryListView::drawContent()
{
    static LLUIColor sel_focus = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static LLUIColor sel_blur = LLUIColorTable::instance().getColor("LtGray_35");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor header_bg = LLUIColorTable::instance().getColor("DkGray2");
    static LLUIColor rule_color = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor bright_color = LLUIColorTable::instance().getColor("White");
    static LLUIColor link_color = LLUIColorTable::instance().getColor("EmphasisColor");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 width = contentWidth();
    const S32 height = getRect().getHeight();

    // The header stays at the top, with the sort direction on the sorted column.
    // The header's background runs over the bar's column too.
    gl_rect_2d(0, height, getRect().getWidth(), height - HEADER_HEIGHT, header_bg.get());
    S32 left = 0;
    for (EColumn col : mShown)
    {
        if (left >= width)
        {
            break;
        }
        std::string label = col < (S32)mLabels.mColumns.size() ? mLabels.mColumns[col] : std::string(columnKey(col));
        if (col == mSortKey)
        {
            label += mSortAscending ? " \xE2\x96\xB4" : " \xE2\x96\xBE";
        }
        const S32 cell = llmin(mColumnWidths[col], width - left);
        font->renderUTF8(label, 0, (F32)(left + CELL_PAD), (F32)(height - HEADER_HEIGHT / 2), text_color.get(),
                         LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX,
                         cell - 2 * CELL_PAD, nullptr, true);
        left += mColumnWidths[col];
    }
    gl_line_2d(0, height - HEADER_HEIGHT, width, height - HEADER_HEIGHT, rule_color.get());

    if (mRows.empty())
    {
        const std::string& message = mLoading ? mLabels.mLoading : (mSearching ? mLabels.mNoResults : mLabels.mEmpty);
        font->renderUTF8(message, 0, width / 2, height - HEADER_HEIGHT - 2 * ROW_HEIGHT, dim_color.get(),
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }

    // Only the rows in view are drawn.
    const bool focused = hasFocus();
    const S32 first = mScrollbar->getDocPos();
    const S32 last = llmin((S32)mRows.size(), first + rowsPerPage() + 1);
    // The pictures in view and half a page either side are fetched at their smallest ahead of a selection.
    const S32 margin = rowsPerPage() / 2;
    for (S32 r = llmax(0, first - margin); r < llmin((S32)mRows.size(), last + margin); ++r)
    {
        prefetchPicture(mRows[r]);
    }
    LLLocalClipRect clip(LLRect(0, height - HEADER_HEIGHT, width, 0));
    for (S32 r = first; r < last; ++r)
    {
        const SKInventoryRow& row = mRows[r];
        const S32 top = height - HEADER_HEIGHT - (r - first) * ROW_HEIGHT;
        const S32 middle = top - ROW_HEIGHT / 2;

        if (row.mKind == SKInventoryRow::KIND_HEADER)
        {
            // A group header spans the row: name bold, count dim, a rule under it.
            font->renderUTF8(row.mName, 0, (F32)CELL_PAD, (F32)middle, bright_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER,
                             LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX, width - 60, nullptr, true);
            const S32 name_width = llmin((S32)LLFontGL::getFontSansSerifSmallBold()->getWidthF32(row.mName), width - 60);
            font->renderUTF8(llformat("%d", row.mGroupCount), 0, 2 * CELL_PAD + name_width, middle, dim_color.get(),
                             LLFontGL::LEFT, LLFontGL::VCENTER);
            gl_line_2d(CELL_PAD, top - ROW_HEIGHT + 1, width - CELL_PAD, top - ROW_HEIGHT + 1, rule_color.get());
            continue;
        }
        if (row.mKind == SKInventoryRow::KIND_SHOW_ALL)
        {
            font->renderUTF8(row.mName, 0, width / 2, middle, link_color.get(), LLFontGL::HCENTER, LLFontGL::VCENTER);
            continue;
        }

        if (mSelected[r])
        {
            gl_rect_2d(0, top, width, top - ROW_HEIGHT, focused ? sel_focus.get() : sel_blur.get());
        }
        else if ((focused && r == mCursor) || r == mHoverRow)
        {
            // The keyboard cursor or the pointer on an unselected row: a faint fill.
            static LLUIColor cursor_color = LLUIColorTable::instance().getColor("White_10");
            gl_rect_2d(0, top, width, top - ROW_HEIGHT, cursor_color.get());
        }
        left = 0;
        for (EColumn col : mShown)
        {
            if (left >= width)
            {
                break;
            }
            S32 x = left + CELL_PAD;
            const S32 cell = llmin(mColumnWidths[col], width - left);
            if (col == COL_NAME && row.mIcon.notNull())
            {
                row.mIcon->draw(x, middle - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE);
                if (row.hasLinkBadge())
                {
                    drawLinkBadge(LLRect(x, middle + ICON_SIZE / 2, x + ICON_SIZE, middle - ICON_SIZE / 2));
                }
                x += ICON_SIZE + CELL_PAD;
            }
            const std::string text = cellText(row, col);
            if (!text.empty())
            {
                LLColor4 color = text_color.get();
                U8 style = LLFontGL::NORMAL;
                if (col == COL_NAME && row.mIsLink)
                {
                    color = dim_color.get();
                    style = LLFontGL::ITALIC;
                }
                else if (col == COL_WORN)
                {
                    color = bright_color.get();
                    style = LLFontGL::BOLD;
                }
                else if (col == COL_DESCRIPTION || col == COL_TYPE || col == COL_FOLDER)
                {
                    color = dim_color.get();
                }
                font->renderUTF8(text, 0, (F32)x, (F32)middle, color, LLFontGL::LEFT, LLFontGL::VCENTER, style,
                                 LLFontGL::NO_SHADOW, S32_MAX, llmax(0, left + cell - CELL_PAD - x), nullptr,
                                 col == COL_DESCRIPTION ? LLFontGL::ELLIPSIS_END : LLFontGL::ELLIPSIS_MIDDLE);
            }
            left += mColumnWidths[col];
        }
    }
}

bool SKInventoryListView::handleHeaderMouseDown(S32 x, S32 y, MASK mask)
{
    if (y < getRect().getHeight() - HEADER_HEIGHT)
    {
        return false;
    }
    const S32 col = columnAt(x);
    if (col >= 0)
    {
        // Each header cycles: its first direction, flipped, then back to the default sort.
        // setSort forgets the clicks, so they are counted after it.
        if (col != mSortKey)
        {
            setSort((EColumn)col, col != COL_ACQUIRED);
            mHeaderClicks = 1;
        }
        else if (mHeaderClicks == 1)
        {
            setSort(mSortKey, !mSortAscending);
            mHeaderClicks = 2;
        }
        else
        {
            setSort(COL_ACQUIRED, false);
        }
        if (mSortCallback)
        {
            mSortCallback();
        }
    }
    return true;
}

bool SKInventoryListView::handleHeaderRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (y < getRect().getHeight() - HEADER_HEIGHT)
    {
        return false;
    }
    if (mHeaderMenuCallback)
    {
        mHeaderMenuCallback(x, y);
    }
    return true;
}

bool SKInventoryListView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(x, y, nullptr);
    const S32 col = columnAt(x);
    if (row >= 0 && col >= 0 && mRows[row].isEntry())
    {
        const std::string text = cellText(mRows[row], (EColumn)col);
        if (!text.empty())
        {
            LLToolTipMgr::instance().show(text);
            return true;
        }
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

bool SKInventoryListView::isDragHandle(S32 x, S32 y, S32 row) const
{
    return columnAt(x) == COL_NAME;
}

LLRect SKInventoryListView::rowsArea() const
{
    return LLRect(0, getRect().getHeight() - HEADER_HEIGHT, contentWidth(), 0);
}

S32 SKInventoryListView::contentDown(S32 y) const
{
    return mScrollbar->getDocPos() * ROW_HEIGHT + (getRect().getHeight() - HEADER_HEIGHT - y);
}

S32 SKInventoryListView::contentToLocal(S32 down) const
{
    return getRect().getHeight() - HEADER_HEIGHT - (down - mScrollbar->getDocPos() * ROW_HEIGHT);
}

void SKInventoryListView::rowsInBand(S32 top, S32 bottom, S32 left, S32 right, std::vector<S32>& rows) const
{
    // Rows span the width, so any band across them meets them.
    const S32 first = llmax(0, top / ROW_HEIGHT);
    const S32 last = llmin((S32)mRows.size() - 1, bottom / ROW_HEIGHT);
    for (S32 r = first; r <= last; ++r)
    {
        rows.push_back(r);
    }
}

LLRect SKInventoryListView::rowRect(S32 row) const
{
    const S32 top = getRect().getHeight() - HEADER_HEIGHT - (row - mScrollbar->getDocPos()) * ROW_HEIGHT;
    return LLRect(0, top, contentWidth(), top - ROW_HEIGHT);
}

void SKInventoryListView::scrollPixels(S32 pixels)
{
    // The List steps by whole rows, at least one a step.
    S32 rows = pixels / ROW_HEIGHT;
    if (rows == 0 && pixels != 0)
    {
        rows = pixels > 0 ? 1 : -1;
    }
    mScrollbar->setDocPos(mScrollbar->getDocPos() + rows);
}
