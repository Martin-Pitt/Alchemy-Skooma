/**
 * @file skpopuplayout.cpp
 * @brief Placement and keyboard moves for side-by-side popup panes: pure, no UI.
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

#include "linden_common.h"

#include "skpopuplayout.h"

#include "llmath.h"

namespace sk
{
    F32 PopupLayout::paneWidth(F32 window_w)
    {
        return llmin(WIDTH, window_w - 2.f * EDGE);
    }

    F32 PopupLayout::rowHeight(const PopupRow& row)
    {
        if (!row.mVisible)
        {
            return 0.f;
        }
        return row.mKind == PopupRow::Kind::SEPARATOR ? SEPARATOR : ROW;
    }

    F32 PopupLayout::contentHeight(const std::vector<PopupRow>& rows)
    {
        F32 h = 2.f * PADDING;
        for (const PopupRow& row : rows)
        {
            h += rowHeight(row);
        }
        return h;
    }

    F32 PopupLayout::paneHeight(const std::vector<PopupRow>& rows, F32 window_h)
    {
        return llmin(contentHeight(rows), window_h - 2.f * EDGE);
    }

    F32 PopupLayout::rowTop(const std::vector<PopupRow>& rows, S32 index)
    {
        if (index < 0 || index >= static_cast<S32>(rows.size()) || !rows[index].mVisible)
        {
            return -1.f;
        }
        F32 y = PADDING;
        for (S32 i = 0; i < index; ++i)
        {
            y += rowHeight(rows[i]);
        }
        return y;
    }

    PopupRect PopupLayout::clamp(PopupRect r, F32 window_w, F32 window_h)
    {
        r.mX = llmax(EDGE, llmin(window_w - r.mW - EDGE, r.mX));
        r.mY = llmax(EDGE, llmin(window_h - r.mH - EDGE, r.mY));
        return r;
    }

    PopupRect PopupLayout::placeFirst(F32 x, F32 y, F32 w, F32 h, F32 window_w, F32 window_h)
    {
        return clamp({ x + GAP, y + GAP, w, h }, window_w, window_h);
    }

    PopupRect PopupLayout::placeBeside(const PopupRect& first, F32 w, F32 h, F32 window_w, F32 window_h)
    {
        PopupRect r { first.mX, first.mY, w, h };
        if (first.mX - SIDE >= EDGE)
        {
            r.mX = first.mX - SIDE;
        }
        else if (first.mX + 2.f * SIDE - GAP <= window_w - EDGE)
        {
            r.mX = first.mX + SIDE;
        }
        else
        {
            r.mY = first.bottom() + GAP;
        }
        return clamp(r, window_w, window_h);
    }

    PopupRect PopupLayout::placeChild(const PopupRect& parent, F32 row_top, F32 w, F32 h, const std::vector<PopupRect>& other_roots,
                                      F32 window_w, F32 window_h)
    {
        PopupRect r { parent.mX - w - 2.f, row_top, w, h };
        if (r.mX < EDGE)
        {
            r.mX = parent.right() + 2.f;
        }
        bool covers = false;
        for (const PopupRect& root : other_roots)
        {
            covers = covers || r.overlaps(root);
        }
        if (covers)
        {
            // Neither side is free: open below the whole group.
            F32 lowest = parent.bottom();
            for (const PopupRect& root : other_roots)
            {
                lowest = llmax(lowest, root.bottom());
            }
            r.mX = parent.mX;
            r.mY = lowest + GAP;
        }
        return clamp(r, window_w, window_h);
    }

    S32 PopupNav::step(const std::vector<PopupRow>& rows, S32 from, S32 direction)
    {
        const S32 n = static_cast<S32>(rows.size());
        if (n == 0)
        {
            return -1;
        }
        S32 i = from;
        for (S32 k = 0; k < n; ++k)
        {
            i = from < 0 && k == 0 ? (direction > 0 ? 0 : n - 1) : (i + direction + n) % n;
            if (rows[i].actionable())
            {
                return i;
            }
        }
        return -1;
    }

    S32 PopupNav::first(const std::vector<PopupRow>& rows)
    {
        return step(rows, -1, 1);
    }

    S32 PopupNav::last(const std::vector<PopupRow>& rows)
    {
        return step(rows, -1, -1);
    }

    void PopupNav::reset(const std::vector<Pane>& panes)
    {
        mPanes = panes;
        for (Pane& p : mPanes)
        {
            p.mOpen = p.mParent < 0;
            p.mOpener = -1;
        }
        mPane = 0;
        mRow = mPanes.empty() ? -1 : first(mPanes[0].mRows);
    }

    void PopupNav::refresh(const std::vector<std::vector<PopupRow>>& rows)
    {
        for (size_t i = 0; i < mPanes.size() && i < rows.size(); ++i)
        {
            mPanes[i].mRows = rows[i];
        }
        if (mPane >= 0 && mPane < static_cast<S32>(mPanes.size()))
        {
            const std::vector<PopupRow>& r = mPanes[mPane].mRows;
            if (mRow >= static_cast<S32>(r.size()) || (mRow >= 0 && !r[mRow].actionable()))
            {
                mRow = step(r, mRow, 1);
            }
        }
    }

    void PopupNav::close(S32 pane)
    {
        if (pane < 0 || pane >= static_cast<S32>(mPanes.size()))
        {
            return;
        }
        for (S32 i = 0; i < static_cast<S32>(mPanes.size()); ++i)
        {
            if (mPanes[i].mParent == pane && mPanes[i].mOpen)
            {
                close(i);
            }
        }
        Pane& p = mPanes[pane];
        if (p.mParent >= 0)
        {
            p.mOpen = false;
            if (mPane == pane)
            {
                mPane = p.mParent;
                mRow = p.mOpener;
            }
        }
    }

    void PopupNav::open(S32 pane)
    {
        if (pane < 0 || pane >= static_cast<S32>(mPanes.size()) || mPanes[pane].mParent < 0)
        {
            return;
        }
        const S32 parent = mPanes[pane].mParent;
        for (S32 i = 0; i < static_cast<S32>(mPanes.size()); ++i)
        {
            if (i != pane && mPanes[i].mParent == parent && mPanes[i].mOpen)
            {
                close(i);
            }
        }
        const std::vector<PopupRow>& rows = mPanes[parent].mRows;
        for (S32 r = 0; r < static_cast<S32>(rows.size()); ++r)
        {
            if (rows[r].mOpens == pane)
            {
                mPanes[pane].mOpener = r;
                break;
            }
        }
        mPanes[pane].mOpen = true;
    }

    PopupNav::Result PopupNav::hover(S32 pane, S32 row)
    {
        if (pane < 0 || pane >= static_cast<S32>(mPanes.size()) || row < 0 || row >= static_cast<S32>(mPanes[pane].mRows.size())
            || !mPanes[pane].mRows[row].actionable())
        {
            return Result::NONE;
        }
        const bool moved = mPane != pane || mRow != row;
        mPane = pane;
        mRow = row;
        const S32 opens = mPanes[pane].mRows[row].mOpens;
        if (opens >= 0)
        {
            if (!mPanes[opens].mOpen)
            {
                open(opens);
                return Result::OPENED;
            }
            return moved ? Result::MOVED : Result::NONE;
        }
        // A plain row closes the children its pane has open, and says so: the panes' bounds shrink.
        bool closed = false;
        for (S32 i = 0; i < static_cast<S32>(mPanes.size()); ++i)
        {
            if (mPanes[i].mParent == pane && mPanes[i].mOpen)
            {
                close(i);
                closed = true;
                mPane = pane;
                mRow = row;
            }
        }
        return closed ? Result::CLOSED_CHILD : moved ? Result::MOVED : Result::NONE;
    }

    PopupNav::Result PopupNav::key(PopupKey key)
    {
        if (key == PopupKey::ESCAPE)
        {
            return Result::CLOSE_ALL;
        }
        if (mPane < 0 || mPane >= static_cast<S32>(mPanes.size()))
        {
            return Result::NONE;
        }
        const std::vector<PopupRow>& rows = mPanes[mPane].mRows;
        switch (key)
        {
            case PopupKey::UP:
            case PopupKey::DOWN:
            {
                const S32 next = step(rows, mRow, key == PopupKey::DOWN ? 1 : -1);
                if (next < 0 || next == mRow)
                {
                    return Result::NONE;
                }
                mRow = next;
                return Result::MOVED;
            }
            case PopupKey::HOME:
            case PopupKey::END:
            {
                const S32 next = key == PopupKey::HOME ? first(rows) : last(rows);
                if (next < 0)
                {
                    return Result::NONE;
                }
                mRow = next;
                return Result::MOVED;
            }
            case PopupKey::LEFT:
            {
                if (mPanes[mPane].mParent < 0)
                {
                    return Result::NONE;
                }
                close(mPane);
                return Result::CLOSED_CHILD;
            }
            case PopupKey::RIGHT:
            case PopupKey::ENTER:
            case PopupKey::SPACE:
            {
                if (mRow < 0 || !rows[mRow].actionable())
                {
                    return Result::NONE;
                }
                const S32 opens = rows[mRow].mOpens;
                if (opens >= 0)
                {
                    if (key == PopupKey::SPACE)
                    {
                        return Result::NONE;
                    }
                    open(opens);
                    mPane = opens;
                    mRow = first(mPanes[opens].mRows);
                    return Result::OPENED;
                }
                return key == PopupKey::RIGHT ? Result::NONE : Result::ACTIVATE;
            }
            default:
                return Result::NONE;
        }
    }
}
