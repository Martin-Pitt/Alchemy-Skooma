/**
 * @file skpopuppanes.cpp
 * @brief Borderless popup panes that open side by side, with focus, keys and outside-click dismissal.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skpopuppanes.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"

#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llrootview.h"

namespace
{
    constexpr F32 TEXT_PAD = 8.f;
}

SKPopupPanes::SKPopupPanes()
    : LLPanel()
{
    setBackgroundVisible(false);
    setMouseOpaque(true);
}

SKPopupPanes::~SKPopupPanes()
{
}

// static
LLHandle<SKPopupPanes> SKPopupPanes::show(LLView* spawner, S32 x, S32 y, std::vector<SKPopupPaneSpec> panes)
{
    if (!spawner || panes.empty() || !gMenuHolder)
    {
        return LLHandle<SKPopupPanes>();
    }
    SKPopupPanes* view = new SKPopupPanes();
    view->setName("sk_popup_panes");
    const LLRect window = gViewerWindow->getRootView()->getRect();
    view->mWindowW = static_cast<F32>(window.getWidth());
    view->mWindowH = static_cast<F32>(window.getHeight());
    view->mSpecs = std::move(panes);
    std::vector<sk::PopupNav::Pane> nav(view->mSpecs.size());
    for (size_t i = 0; i < view->mSpecs.size(); ++i)
    {
        nav[i].mRows = view->rowsOf(view->mSpecs[i]);
        nav[i].mParent = view->mSpecs[i].mParent;
    }
    view->mNav.reset(nav);
    view->mRects.assign(view->mSpecs.size(), sk::PopupRect());
    view->mPlaced.assign(view->mSpecs.size(), false);
    view->mScroll.assign(view->mSpecs.size(), 0.f);

    // The spawner's point in window pixels, y down.
    S32 sx = 0, sy = 0;
    spawner->localPointToScreen(x, y, &sx, &sy);
    const F32 w = sk::PopupLayout::paneWidth(view->mWindowW);
    const F32 down = view->mWindowH - static_cast<F32>(sy);
    S32 placed_roots = 0;
    for (size_t i = 0; i < view->mSpecs.size(); ++i)
    {
        if (view->mSpecs[i].mParent >= 0)
        {
            continue;
        }
        const F32 h = sk::PopupLayout::paneHeight(nav[i].mRows, view->mWindowH);
        if (placed_roots == 0)
        {
            view->mRects[i] = sk::PopupLayout::placeFirst(static_cast<F32>(sx), down, w, h, view->mWindowW, view->mWindowH);
        }
        else
        {
            // Beside the first; any further root goes below the one before it.
            const sk::PopupRect& first = view->mRects[0];
            view->mRects[i] = placed_roots == 1 ? sk::PopupLayout::placeBeside(first, w, h, view->mWindowW, view->mWindowH)
                                                : sk::PopupLayout::clamp({ first.mX, view->mRects[i - 1].bottom() + sk::PopupLayout::GAP, w, h },
                                                                         view->mWindowW, view->mWindowH);
        }
        view->mPlaced[i] = true;
        ++placed_roots;
    }
    gMenuHolder->addChild(view);
    view->layout();

    // Keys come here while it is open; the holder of focus before gets it back after.
    if (LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()))
    {
        view->mSavedFocus = focus->getHandle();
    }
    gFocusMgr.setKeyboardFocus(view);
    LLUI::getInstance()->addPopup(view);
    return view->getDerivedHandle<SKPopupPanes>();
}

void SKPopupPanes::setRefresh(std::function<void(std::vector<SKPopupPaneSpec>&)> fn)
{
    mRefresh = std::move(fn);
}

boost::signals2::connection SKPopupPanes::setClosedCallback(const boost::signals2::signal<void()>::slot_type& cb)
{
    return mClosedSignal.connect(cb);
}

std::vector<sk::PopupRow> SKPopupPanes::rowsOf(const SKPopupPaneSpec& pane) const
{
    std::vector<sk::PopupRow> rows;
    for (const SKPopupItem& item : pane.mItems)
    {
        sk::PopupRow r;
        r.mKind = item.mKind == SKPopupItem::Kind::ACTION    ? sk::PopupRow::Kind::ACTION
                : item.mKind == SKPopupItem::Kind::HEADING   ? sk::PopupRow::Kind::HEADING
                                                             : sk::PopupRow::Kind::SEPARATOR;
        r.mEnabled = item.mEnabled;
        r.mVisible = item.mVisible;
        r.mOpens = item.mOpensPane;
        rows.push_back(r);
    }
    return rows;
}

void SKPopupPanes::layout()
{
    const std::vector<sk::PopupNav::Pane>& panes = mNav.panes();
    const F32 w = sk::PopupLayout::paneWidth(mWindowW);
    // Heights follow the rows, which a refresh may show or hide.
    for (size_t i = 0; i < panes.size(); ++i)
    {
        if (!panes[i].mOpen)
        {
            mPlaced[i] = false;
            continue;
        }
        const F32 h = sk::PopupLayout::paneHeight(panes[i].mRows, mWindowH);
        if (mPlaced[i])
        {
            mRects[i].mH = h;
            mRects[i] = sk::PopupLayout::clamp(mRects[i], mWindowW, mWindowH);
        }
    }
    for (size_t i = 0; i < panes.size(); ++i)
    {
        if (!panes[i].mOpen || mPlaced[i] || panes[i].mParent < 0 || !mPlaced[panes[i].mParent])
        {
            continue;
        }
        const S32 parent = panes[i].mParent;
        const F32 row_top = mRects[parent].mY + sk::PopupLayout::rowTop(panes[parent].mRows, panes[i].mOpener) - mScroll[parent];
        std::vector<sk::PopupRect> others;
        for (size_t j = 0; j < panes.size(); ++j)
        {
            if (panes[j].mParent < 0 && panes[j].mOpen && static_cast<S32>(j) != parent)
            {
                others.push_back(mRects[j]);
            }
        }
        mRects[i] = sk::PopupLayout::placeChild(mRects[parent], row_top, w, sk::PopupLayout::paneHeight(panes[i].mRows, mWindowH), others,
                                                mWindowW, mWindowH);
        mPlaced[i] = true;
        mScroll[i] = 0.f;
    }
    // This view covers the open panes and no more, so clicks between them close it.
    F32 x0 = mWindowW, y0 = mWindowH, x1 = 0.f, y1 = 0.f;
    for (size_t i = 0; i < panes.size(); ++i)
    {
        if (panes[i].mOpen && mPlaced[i])
        {
            x0 = llmin(x0, mRects[i].mX);
            y0 = llmin(y0, mRects[i].mY);
            x1 = llmax(x1, mRects[i].right());
            y1 = llmax(y1, mRects[i].bottom());
        }
    }
    if (x1 > x0 && y1 > y0)
    {
        setRect(LLRect(ll_round(x0), ll_round(mWindowH - y0), ll_round(x1), ll_round(mWindowH - y1)));
    }
}

LLRect SKPopupPanes::toLocal(const sk::PopupRect& r) const
{
    const LLRect& me = getRect();
    return LLRect(ll_round(r.mX) - me.mLeft, ll_round(mWindowH - r.mY) - me.mBottom, ll_round(r.right()) - me.mLeft,
                  ll_round(mWindowH - r.bottom()) - me.mBottom);
}

void SKPopupPanes::hit(S32 x, S32 y, S32& pane, S32& row) const
{
    pane = -1;
    row = -1;
    const F32 wx = static_cast<F32>(x + getRect().mLeft);
    const F32 wy = mWindowH - static_cast<F32>(y + getRect().mBottom);
    const std::vector<sk::PopupNav::Pane>& panes = mNav.panes();
    // Children draw over their parents, so they are hit first.
    for (S32 i = static_cast<S32>(panes.size()) - 1; i >= 0; --i)
    {
        if (!panes[i].mOpen || !mPlaced[i] || !mRects[i].contains(wx, wy))
        {
            continue;
        }
        pane = i;
        const F32 within = wy - mRects[i].mY + mScroll[i];
        for (S32 r = 0; r < static_cast<S32>(panes[i].mRows.size()); ++r)
        {
            const F32 top = sk::PopupLayout::rowTop(panes[i].mRows, r);
            if (top >= 0.f && within >= top && within < top + sk::PopupLayout::rowHeight(panes[i].mRows[r]))
            {
                row = r;
                break;
            }
        }
        return;
    }
}

void SKPopupPanes::ensureVisible(S32 pane, S32 row)
{
    if (pane < 0 || row < 0 || pane >= static_cast<S32>(mRects.size()))
    {
        return;
    }
    const std::vector<sk::PopupRow>& rows = mNav.panes()[pane].mRows;
    const F32 top = sk::PopupLayout::rowTop(rows, row);
    const F32 bottom = top + sk::PopupLayout::rowHeight(rows[row]);
    const F32 visible = mRects[pane].mH - sk::PopupLayout::PADDING;
    if (top - sk::PopupLayout::PADDING < mScroll[pane])
    {
        mScroll[pane] = llmax(0.f, top - sk::PopupLayout::PADDING);
    }
    else if (bottom > mScroll[pane] + visible)
    {
        mScroll[pane] = bottom - visible;
    }
}

void SKPopupPanes::activate(S32 pane, S32 row)
{
    if (pane < 0 || pane >= static_cast<S32>(mSpecs.size()) || row < 0 || row >= static_cast<S32>(mSpecs[pane].mItems.size()))
    {
        return;
    }
    // Every pane closes first; the action runs after, from a copy.
    const std::function<void()> action = mSpecs[pane].mItems[row].mAction;
    closeAll();
    if (action)
    {
        action();
    }
}

void SKPopupPanes::closeAll()
{
    if (mClosing)
    {
        return;
    }
    mClosing = true;
    if (LLView* saved = mSavedFocus.get())
    {
        gFocusMgr.setKeyboardFocus(saved);
    }
    else if (gFocusMgr.getKeyboardFocus() == this)
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
    LLUI::getInstance()->removePopup(this);
    mClosedSignal();
    setVisible(false);
    die();
}

void SKPopupPanes::onTopLost()
{
    closeAll();
}

void SKPopupPanes::draw()
{
    // The popup view draws registered popups as well as the holder drawing its child.
    const U32 frame = LLFrameTimer::getFrameCount();
    if (mClosing || mDrawnFrame == frame)
    {
        return;
    }
    mDrawnFrame = frame;
    if (mRefresh)
    {
        mRefresh(mSpecs);
        std::vector<std::vector<sk::PopupRow>> rows;
        for (const SKPopupPaneSpec& spec : mSpecs)
        {
            rows.push_back(rowsOf(spec));
        }
        mNav.refresh(rows);
        layout();
    }

    static LLUIColor bg_colour = LLUIColorTable::instance().getColor("MenuDefaultBgColor");
    static LLUIColor highlight_colour = LLUIColorTable::instance().getColor("MenuItemHighlightBgColor");
    static LLUIColor enabled_colour = LLUIColorTable::instance().getColor("MenuItemEnabledColor");
    static LLUIColor disabled_colour = LLUIColorTable::instance().getColor("MenuItemDisabledColor");
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const std::vector<sk::PopupNav::Pane>& panes = mNav.panes();
    for (size_t i = 0; i < panes.size(); ++i)
    {
        if (!panes[i].mOpen || !mPlaced[i])
        {
            continue;
        }
        const LLRect box = toLocal(mRects[i]);
        gl_rect_2d(box, bg_colour.get(), true);
        gl_rect_2d(box, disabled_colour.get() % 0.6f, false);
        LLLocalClipRect clip(box);
        const SKPopupPaneSpec& spec = mSpecs[i];
        for (S32 r = 0; r < static_cast<S32>(panes[i].mRows.size()) && r < static_cast<S32>(spec.mItems.size()); ++r)
        {
            const sk::PopupRow& row = panes[i].mRows[r];
            if (!row.mVisible)
            {
                continue;
            }
            const SKPopupItem& item = spec.mItems[r];
            const F32 top = box.mTop - sk::PopupLayout::rowTop(panes[i].mRows, r) + mScroll[i];
            const F32 height = sk::PopupLayout::rowHeight(row);
            if (row.mKind == sk::PopupRow::Kind::SEPARATOR)
            {
                gl_line_2d(box.mLeft + 4, ll_round(top - height * 0.5f), box.mRight - 4, ll_round(top - height * 0.5f), disabled_colour.get() % 0.5f);
                continue;
            }
            // The active row, and a row whose pane is open, are highlighted.
            const bool active = static_cast<S32>(i) == mNav.activePane() && r == mNav.activeRow();
            const bool expanded = row.mOpens >= 0 && row.mOpens < static_cast<S32>(panes.size()) && panes[row.mOpens].mOpen;
            if (row.mKind == sk::PopupRow::Kind::ACTION && (active || expanded))
            {
                gl_rect_2d(box.mLeft + 2, ll_round(top), box.mRight - 2, ll_round(top - height), highlight_colour.get(), true);
            }
            const LLColor4 colour = row.mKind == sk::PopupRow::Kind::ACTION && row.mEnabled ? enabled_colour.get() : disabled_colour.get();
            const F32 mid = top - height * 0.5f;
            F32 detail_w = 0.f;
            if (!item.mDetail.empty())
            {
                detail_w = font->getWidthF32(item.mDetail) + TEXT_PAD;
                font->renderUTF8(item.mDetail, 0, box.mRight - TEXT_PAD, mid, disabled_colour.get(), LLFontGL::RIGHT, LLFontGL::VCENTER,
                                 LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            }
            const S32 room = llmax(0, ll_round(box.getWidth() - 2.f * TEXT_PAD - detail_w));
            font->renderUTF8(item.mLabel, 0, box.mLeft + TEXT_PAD, mid, colour, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::NO_SHADOW, S32_MAX, room, nullptr, true);
        }
    }
}

bool SKPopupPanes::handleKeyHere(KEY key, MASK mask)
{
    sk::PopupKey k;
    switch (key)
    {
        case KEY_UP: k = sk::PopupKey::UP; break;
        case KEY_DOWN: k = sk::PopupKey::DOWN; break;
        case KEY_HOME: k = sk::PopupKey::HOME; break;
        case KEY_END: k = sk::PopupKey::END; break;
        case KEY_LEFT: k = sk::PopupKey::LEFT; break;
        case KEY_RIGHT: k = sk::PopupKey::RIGHT; break;
        case KEY_RETURN: k = sk::PopupKey::ENTER; break;
        case KEY_ESCAPE: k = sk::PopupKey::ESCAPE; break;
        case ' ': k = sk::PopupKey::SPACE; break;
        default: return false;
    }
    switch (mNav.key(k))
    {
        case sk::PopupNav::Result::MOVED:
            ensureVisible(mNav.activePane(), mNav.activeRow());
            break;
        case sk::PopupNav::Result::OPENED:
        case sk::PopupNav::Result::CLOSED_CHILD:
            layout();
            ensureVisible(mNav.activePane(), mNav.activeRow());
            break;
        case sk::PopupNav::Result::ACTIVATE:
            activate(mNav.activePane(), mNav.activeRow());
            break;
        case sk::PopupNav::Result::CLOSE_ALL:
            closeAll();
            break;
        default:
            break;
    }
    return true;
}

bool SKPopupPanes::handleHover(S32 x, S32 y, MASK mask)
{
    S32 pane, row;
    hit(x, y, pane, row);
    if (pane < 0)
    {
        return false;
    }
    if (row >= 0 && mNav.hover(pane, row) == sk::PopupNav::Result::OPENED)
    {
        layout();
    }
    gViewerWindow->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool SKPopupPanes::handleMouseDown(S32 x, S32 y, MASK mask)
{
    S32 pane, row;
    hit(x, y, pane, row);
    mMouseDownInside = pane >= 0;
    return mMouseDownInside;
}

bool SKPopupPanes::handleMouseUp(S32 x, S32 y, MASK mask)
{
    S32 pane, row;
    hit(x, y, pane, row);
    if (pane < 0 || row < 0)
    {
        return pane >= 0;
    }
    const sk::PopupRow& r = mNav.panes()[pane].mRows[row];
    if (!r.actionable())
    {
        return true;
    }
    if (r.mOpens >= 0)
    {
        if (mNav.hover(pane, row) == sk::PopupNav::Result::OPENED)
        {
            layout();
        }
        return true;
    }
    activate(pane, row);
    return true;
}

bool SKPopupPanes::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    S32 pane, row;
    hit(x, y, pane, row);
    return pane >= 0;
}

bool SKPopupPanes::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    S32 pane, row;
    hit(x, y, pane, row);
    if (pane < 0)
    {
        return false;
    }
    const F32 content = sk::PopupLayout::contentHeight(mNav.panes()[pane].mRows);
    mScroll[pane] = llclamp(mScroll[pane] + delta.mPrecise * sk::PopupLayout::ROW, 0.f, llmax(0.f, content - mRects[pane].mH));
    return true;
}
