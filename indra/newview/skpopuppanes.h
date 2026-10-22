/**
 * @file skpopuppanes.h
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

#ifndef SK_POPUPPANES_H
#define SK_POPUPPANES_H

#include "llpanel.h"

#include "skpopuplayout.h"

#include <boost/signals2.hpp>
#include <functional>
#include <string>
#include <vector>

/// One row of a pane.
struct SKPopupItem
{
    enum class Kind { ACTION, HEADING, SEPARATOR };
    Kind mKind = Kind::ACTION;
    std::string mLabel;
    std::string mDetail;                 ///< right-aligned, dim: "checking…", "›"
    bool mEnabled = true;
    bool mVisible = true;
    S32 mOpensPane = -1;                 ///< index of the pane this row opens, or -1
    std::function<void()> mAction;       ///< run after every pane has closed
};

struct SKPopupPaneSpec
{
    std::string mName;
    std::vector<SKPopupItem> mItems;
    S32 mParent = -1;                    ///< -1: a root pane; else the pane that opens it
};

/// Root panes open at once, the second beside the first; a row can open a child pane beside its
/// own, all growing towards the window's centre from the click. Held by the menu holder above the floaters and registered as a popup, so a click
/// anywhere else closes everything; it takes keyboard focus while open and gives it back.
class SKPopupPanes : public LLPanel
{
public:
    AL_VIEW_TYPE(SKPopupPanes, LLPanel);

    /// Shows panes 0..n at window point (x, y) of spawner; pane 1, if a root, sits beside pane 0.
    static LLHandle<SKPopupPanes> show(LLView* spawner, S32 x, S32 y, std::vector<SKPopupPaneSpec> panes);
    void setRefresh(std::function<void(std::vector<SKPopupPaneSpec>&)> fn);   ///< called each frame while open
    boost::signals2::connection setClosedCallback(const boost::signals2::signal<void()>::slot_type& cb);
    void closeAll();

    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    void onTopLost() override;

    ~SKPopupPanes() override;

protected:
    SKPopupPanes();

private:
    /// The layout's view of the specs' rows.
    std::vector<sk::PopupRow> rowsOf(const SKPopupPaneSpec& pane) const;
    /// Place every open pane that has no place yet, and size this view round them.
    void layout();
    /// The pane and row under a point of this view, or -1s.
    void hit(S32 x, S32 y, S32& pane, S32& row) const;
    /// Window pixels, y down, to this view's GL pixels.
    LLRect toLocal(const sk::PopupRect& r) const;
    void ensureVisible(S32 pane, S32 row);
    void activate(S32 pane, S32 row);

    std::vector<SKPopupPaneSpec> mSpecs;
    sk::PopupNav mNav;
    std::vector<sk::PopupRect> mRects;      ///< per pane; valid while placed
    std::vector<bool> mPlaced;
    std::vector<F32> mAnchor;               ///< per child pane, its opener row's top when placed, unscrolled
    std::vector<F32> mScroll;               ///< per pane, pixels scrolled down
    sk::PopupFacing mFacing;                ///< right and down from the click, flipped where the first pane does not fit
    F32 mWindowW = 0.f;
    F32 mWindowH = 0.f;
    std::function<void(std::vector<SKPopupPaneSpec>&)> mRefresh;
    boost::signals2::signal<void()> mClosedSignal;
    LLHandle<LLView> mSavedFocus;
    U32 mDrawnFrame = 0;
    bool mClosing = false;
    bool mMouseDownInside = false;
};

#endif // SK_POPUPPANES_H
