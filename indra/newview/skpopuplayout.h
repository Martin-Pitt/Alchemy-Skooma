/**
 * @file skpopuplayout.h
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

#ifndef SK_POPUPLAYOUT_H
#define SK_POPUPLAYOUT_H

#include "stdtypes.h"

#include <vector>

namespace sk
{
    /// A pane's box in window pixels, origin top left, y down.
    struct PopupRect
    {
        F32 mX = 0.f;
        F32 mY = 0.f;
        F32 mW = 0.f;
        F32 mH = 0.f;
        F32 right() const { return mX + mW; }
        F32 bottom() const { return mY + mH; }
        bool overlaps(const PopupRect& o) const { return mX < o.right() && right() > o.mX && mY < o.bottom() && bottom() > o.mY; }
        bool contains(F32 x, F32 y) const { return x >= mX && x < right() && y >= mY && y < bottom(); }
    };

    /// What the layout and the keys need to know of a row.
    struct PopupRow
    {
        enum class Kind { ACTION, HEADING, SEPARATOR };
        Kind mKind = Kind::ACTION;
        bool mEnabled = true;
        bool mVisible = true;
        S32 mOpens = -1;            ///< the pane this row opens, or -1
        bool actionable() const { return mVisible && mEnabled && mKind == Kind::ACTION; }
    };

    /// Where panes go: the root pane beside the click, a second root pane beside it, a child
    /// pane beside the pane that opens it, everything 8 px inside the window.
    class PopupLayout
    {
    public:
        static constexpr F32 WIDTH = 174.f;
        static constexpr F32 PADDING = 4.f;
        static constexpr F32 ROW = 22.f;
        static constexpr F32 SEPARATOR = 9.f;
        static constexpr F32 EDGE = 8.f;
        static constexpr F32 GAP = 4.f;
        static constexpr F32 SIDE = 178.f;      ///< a pane's width and the gap to its neighbour

        static F32 paneWidth(F32 window_w);
        /// The rows' own height, padding included.
        static F32 contentHeight(const std::vector<PopupRow>& rows);
        /// The pane's height: its content, capped at the window less its edges (it scrolls).
        static F32 paneHeight(const std::vector<PopupRow>& rows, F32 window_h);
        /// A row's top, from its pane's top, before scrolling; -1 for a hidden row.
        static F32 rowTop(const std::vector<PopupRow>& rows, S32 index);
        static F32 rowHeight(const PopupRow& row);

        static PopupRect clamp(PopupRect r, F32 window_w, F32 window_h);
        /// The first root pane, its corner 4 px off the click.
        static PopupRect placeFirst(F32 x, F32 y, F32 w, F32 h, F32 window_w, F32 window_h);
        /// A second root pane beside the first: left if there is room, else right, else below.
        static PopupRect placeBeside(const PopupRect& first, F32 w, F32 h, F32 window_w, F32 window_h);
        /// A child pane level with its row: left of its parent, else right; below every root pane
        /// when it would cover a root pane other than its parent.
        static PopupRect placeChild(const PopupRect& parent, F32 row_top, F32 w, F32 h, const std::vector<PopupRect>& other_roots,
                                    F32 window_w, F32 window_h);
    };

    enum class PopupKey { UP, DOWN, HOME, END, LEFT, RIGHT, ENTER, SPACE, ESCAPE };

    /// The panes' open state and the active row, moved by keys and the pointer.
    class PopupNav
    {
    public:
        struct Pane
        {
            std::vector<PopupRow> mRows;
            S32 mParent = -1;       ///< -1 for a root pane
            bool mOpen = false;
            S32 mOpener = -1;       ///< the row of the parent that opened it
        };
        enum class Result { NONE, MOVED, OPENED, CLOSED_CHILD, ACTIVATE, CLOSE_ALL };

        /// Root panes open, the active row the first actionable row of pane 0.
        void reset(const std::vector<Pane>& panes);
        /// New rows for the same panes, keeping what is open and active where it still can be.
        void refresh(const std::vector<std::vector<PopupRow>>& rows);

        Result key(PopupKey key);
        /// The pointer is over a row: it becomes active, and a row that opens a pane opens it.
        Result hover(S32 pane, S32 row);
        /// Open a pane from its opener row, closing its parent's other children.
        void open(S32 pane);
        void close(S32 pane);

        const std::vector<Pane>& panes() const { return mPanes; }
        S32 activePane() const { return mPane; }
        S32 activeRow() const { return mRow; }

        /// The next actionable row in a direction, wrapping; -1 when there is none.
        static S32 step(const std::vector<PopupRow>& rows, S32 from, S32 direction);
        static S32 first(const std::vector<PopupRow>& rows);
        static S32 last(const std::vector<PopupRow>& rows);

    private:
        std::vector<Pane> mPanes;
        S32 mPane = 0;
        S32 mRow = -1;
    };
}

#endif // SK_POPUPLAYOUT_H
