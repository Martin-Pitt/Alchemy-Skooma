/**
 * @file skinventoryexplorerpopup.cpp
 * @brief Inventory Explorer's drop-down list: the path bar's folder menus and the search options
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

#include "skinventoryexplorerpopup.h"

#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewerwindow.h"
#include "llwindow.h"

namespace
{
    constexpr S32 OPTION_HEIGHT = 20;
    constexpr S32 TITLE_HEIGHT = 18;
    constexpr S32 DIVIDER_HEIGHT = 7;
    constexpr S32 PAD = 8;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 REMOVE_SIZE = 16;
    constexpr S32 EDGE = 6;     // kept inside the window
}

SKExplorerPopupList* SKExplorerPopupList::create()
{
    LLUICtrl::Params p;
    p.name("sk_explorer_popup");
    p.rect(LLRect(0, 1, 1, 0));
    p.visible(false);
    p.mouse_opaque(true);
    return LLUICtrlFactory::create<SKExplorerPopupList>(p);
}

SKExplorerPopupList::SKExplorerPopupList(const LLUICtrl::Params& p)
:   LLUICtrl(p),
    mHighlight(-1),
    mHover(-1),
    mScroll(0)
{
}

S32 SKExplorerPopupList::rowHeight(const SKPopupRow& row)
{
    switch (row.mKind)
    {
        case SKPopupRow::ROW_TITLE:   return TITLE_HEIGHT;
        case SKPopupRow::ROW_DIVIDER: return DIVIDER_HEIGHT;
        default:                      return OPTION_HEIGHT;
    }
}

S32 SKExplorerPopupList::contentHeight() const
{
    S32 height = 2;
    for (const SKPopupRow& row : mRows)
    {
        height += rowHeight(row);
    }
    return height;
}

void SKExplorerPopupList::setRows(const std::vector<SKPopupRow>& rows)
{
    mRows = rows;
    if (mHighlight >= (S32)mRows.size() || (mHighlight >= 0 && !mRows[mHighlight].isOption()))
    {
        mHighlight = -1;
    }
    mHover = -1;
    clampScroll();
}

void SKExplorerPopupList::showBelow(LLView* spawn, const LLRect& spawn_rect, S32 width, S32 max_height, S32 dx, S32 dy)
{
    S32 left = 0;
    S32 bottom = 0;
    spawn->localPointToScreen(spawn_rect.mLeft + dx, spawn_rect.mBottom - dy, &left, &bottom);
    const LLRect window = gViewerWindow->getWorldViewRectScaled();
    S32 height = contentHeight();
    if (max_height > 0)
    {
        height = llmin(height, max_height);
    }
    height = llmin(height, llmax(OPTION_HEIGHT, bottom - window.mBottom - EDGE));
    left = llclamp(left, window.mLeft + EDGE, llmax(window.mLeft + EDGE, window.mRight - width - EDGE));
    setRect(LLRect(left, bottom, left + width, bottom - height));
    clampScroll();
    if (!getVisible())
    {
        setVisible(true);
        LLUI::getInstance()->addPopup(this);
    }
}

void SKExplorerPopupList::hide()
{
    if (getVisible())
    {
        // Removing the popup tells it its top was lost, which closes it.
        LLUI::getInstance()->removePopup(this);
    }
}

void SKExplorerPopupList::onTopLost()
{
    if (!getVisible())
    {
        return;
    }
    setVisible(false);
    mHover = -1;
    mHighlight = -1;
    mScroll = 0;
    if (mCloseCallback)
    {
        mCloseCallback();
    }
}

S32 SKExplorerPopupList::firstOption() const
{
    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        if (mRows[i].isOption())
        {
            return i;
        }
    }
    return -1;
}

void SKExplorerPopupList::setHighlight(S32 index)
{
    mHighlight = (index >= 0 && index < (S32)mRows.size() && mRows[index].isOption()) ? index : -1;
    if (mHighlight >= 0)
    {
        scrollToRow(mHighlight);
    }
}

void SKExplorerPopupList::moveHighlight(S32 step)
{
    const S32 count = (S32)mRows.size();
    if (count == 0 || firstOption() < 0)
    {
        return;
    }
    S32 index = mHighlight < 0 ? (step > 0 ? -1 : count) : mHighlight;
    for (S32 tries = 0; tries <= count; ++tries)
    {
        index = (index + step + count) % count;
        if (mRows[index].isOption())
        {
            setHighlight(index);
            return;
        }
    }
}

LLRect SKExplorerPopupList::rowRect(S32 index) const
{
    S32 top = getRect().getHeight() - 1 + mScroll;
    for (S32 i = 0; i < index; ++i)
    {
        top -= rowHeight(mRows[i]);
    }
    return LLRect(1, top, getRect().getWidth() - 1, top - rowHeight(mRows[index]));
}

LLRect SKExplorerPopupList::removeRect(S32 index) const
{
    const LLRect row = rowRect(index);
    return LLRect(row.mRight - REMOVE_SIZE - 4, row.mTop, row.mRight - 2, row.mBottom);
}

S32 SKExplorerPopupList::rowAt(S32 y) const
{
    S32 top = getRect().getHeight() - 1 + mScroll;
    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        const S32 bottom = top - rowHeight(mRows[i]);
        if (y < top && y >= bottom)
        {
            return i;
        }
        top = bottom;
    }
    return -1;
}

void SKExplorerPopupList::clampScroll()
{
    mScroll = llclamp(mScroll, 0, llmax(0, contentHeight() - getRect().getHeight()));
}

void SKExplorerPopupList::scrollToRow(S32 index)
{
    const LLRect row = rowRect(index);
    const S32 height = getRect().getHeight();
    if (row.mTop > height - 1)
    {
        mScroll -= row.mTop - (height - 1);
    }
    else if (row.mBottom < 1)
    {
        mScroll += 1 - row.mBottom;
    }
    clampScroll();
}

void SKExplorerPopupList::draw()
{
    static LLUIColor bg = LLUIColorTable::instance().getColor("MenuDefaultBgColor");
    static LLUIColor border = LLUIColorTable::instance().getColor("MenuItemDisabledColor");
    static LLUIColor highlight = LLUIColorTable::instance().getColor("MenuItemHighlightBgColor");
    static LLUIColor hover = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor text = LLUIColorTable::instance().getColor("MenuItemEnabledColor");
    static LLUIColor disabled = LLUIColorTable::instance().getColor("MenuItemDisabledColor");
    static LLUIColor dim = LLUIColorTable::instance().getColor("LtGray_50");
    static LLPointer<LLUIImage> close = LLUI::getUIImage("Icon_Close_Foreground");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLRect local = getLocalRect();
    gl_drop_shadow(0, local.mTop, local.mRight, 0, LLColor4(0.f, 0.f, 0.f, 0.3f), 4);
    gl_rect_2d(local, bg.get(), true);
    gl_rect_2d(local, border.get(), false);

    LLLocalClipRect clip(LLRect(1, local.mTop - 1, local.mRight - 1, 1));
    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        const SKPopupRow& row = mRows[i];
        const LLRect rect = rowRect(i);
        if (rect.mBottom > local.mTop || rect.mTop < 0)
        {
            continue;
        }
        const F32 mid = (F32)(rect.mBottom + rect.getHeight() / 2);
        if (row.mKind == SKPopupRow::ROW_DIVIDER)
        {
            gl_line_2d(rect.mLeft + 4, (S32)mid, rect.mRight - 4, (S32)mid, border.get());
            continue;
        }
        if (row.mKind == SKPopupRow::ROW_TITLE)
        {
            font->renderUTF8(row.mLabel, 0, (F32)(rect.mLeft + PAD), mid, dim.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            continue;
        }
        if (i == mHighlight)
        {
            gl_rect_2d(rect, highlight.get(), true);
        }
        else if (i == mHover && row.mEnabled)
        {
            gl_rect_2d(rect, hover.get(), true);
        }
        S32 x = rect.mLeft + PAD;
        if (row.mIcon.notNull())
        {
            row.mIcon->draw(x, rect.mBottom + (rect.getHeight() - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
            x += ICON_SIZE + 4;
        }
        S32 right = rect.mRight - PAD;
        const bool show_remove = !row.mRemoveToolTip.empty() && (i == mHover || i == mHighlight);
        if (!row.mRemoveToolTip.empty())
        {
            right -= REMOVE_SIZE;
        }
        if (show_remove && close.notNull())
        {
            const LLRect remove = removeRect(i);
            close->draw(remove.mLeft, remove.mBottom + (remove.getHeight() - 12) / 2, 12, 12, text.get());
        }
        if (!row.mExample.empty())
        {
            const S32 example_width = font->getWidth(row.mExample);
            font->renderUTF8(row.mExample, 0, (F32)right, mid, dim.get(), LLFontGL::RIGHT, LLFontGL::VCENTER, LLFontGL::ITALIC, LLFontGL::NO_SHADOW);
            right -= example_width + PAD;
        }
        const LLColor4 color = row.mEnabled ? text.get() : disabled.get();
        F32 label_right = 0.f;
        font->renderUTF8(row.mLabel, 0, (F32)x, mid, color, LLFontGL::LEFT, LLFontGL::VCENTER,
                         row.mBold ? LLFontGL::BOLD : LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, llmax(0, right - x),
                         &label_right, true);
        if (!row.mHint.empty() && (S32)label_right + PAD < right)
        {
            const S32 hint_x = (S32)label_right + PAD;
            font->renderUTF8(row.mHint, 0, (F32)hint_x, mid, dim.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::NO_SHADOW, S32_MAX, right - hint_x, nullptr, true);
        }
    }
}

bool SKExplorerPopupList::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 index = rowAt(y);
    if (index < 0 || !mRows[index].isOption())
    {
        return true;
    }
    if (!mRows[index].mRemoveToolTip.empty() && removeRect(index).pointInRect(x, y))
    {
        if (mRemoveCallback)
        {
            mRemoveCallback(index);
        }
        return true;
    }
    if (mPickCallback)
    {
        mPickCallback(index);
    }
    return true;
}

bool SKExplorerPopupList::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return true;
}

bool SKExplorerPopupList::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = rowAt(y);
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

void SKExplorerPopupList::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKExplorerPopupList::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    mScroll += delta.mClicks * OPTION_HEIGHT;
    clampScroll();
    mHover = rowAt(y);
    return true;
}

bool SKExplorerPopupList::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 index = rowAt(y);
    if (index < 0)
    {
        return true;
    }
    const SKPopupRow& row = mRows[index];
    if (!row.mRemoveToolTip.empty() && removeRect(index).pointInRect(x, y))
    {
        LLToolTipMgr::instance().show(row.mRemoveToolTip);
    }
    else if (!row.mToolTip.empty())
    {
        LLToolTipMgr::instance().show(row.mToolTip);
    }
    return true;
}
