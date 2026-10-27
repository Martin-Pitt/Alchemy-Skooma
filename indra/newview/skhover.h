/**
 * @file skhover.h
 * @brief What the mouse is over, and what the hovered widget wants the rest of the viewer to know.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef SK_HOVER_H
#define SK_HOVER_H

#include "llhandle.h"
#include "llsd.h"

#include <boost/signals2.hpp>
#include <functional>
#include <set>
#include <string>

class LLView;
class LLUICtrl;

// A hovered widget's message to everyone else: a kind tag consumers filter on and
// a payload they interpret. Empty kind means nothing is published.
struct SKHoverContext
{
    std::string      kind;
    LLSD             data;
    LLHandle<LLView> source;

    bool isSet() const { return !kind.empty(); }
    bool operator==(const SKHoverContext& o) const;
    bool operator!=(const SKHoverContext& o) const { return !(*this == o); }
};

// Hover state for the whole UI. LLViewerWindow::updateUI already computes the set of
// views under the cursor each frame and fires enter/leave on them; this class keeps
// that set readable, names the deepest view, and carries the hovered widget's
// context to consumers that are nowhere near it (another panel, a world overlay).
class SKHover
{
public:
    using view_handle_set_t = std::set<LLHandle<LLView>>;
    using signal_t          = boost::signals2::signal<void(const SKHoverContext&)>;
    using data_fn_t         = std::function<LLSD()>;

    // Fed by LLViewerWindow::updateUI after it diffs the hover set. Not for widgets.
    static void update(const view_handle_set_t& hovered);

    // The deepest view under the cursor, or nullptr when the mouse is in the world.
    static LLView* leaf();
    static bool isHovered(const LLView* view);
    // True when a UI view is under the cursor. When false, the in-world hover pick
    // in LLSelectMgr (getHoverObjects) is the thing to read instead.
    static bool overUI();

    // Publish a context while a widget is hovered. Publishing again replaces the
    // widget's previous context; retract clears it. Contexts are dropped on their own
    // when the source leaves the hover set, so retract is only needed to end early.
    static void publish(LLView* source, const std::string& kind, const LLSD& data);
    static void retract(LLView* source);

    // The context of the deepest publishing view, or an unset one.
    static const SKHoverContext& context();
    // The current context if it has this kind, else nullptr.
    static const SKHoverContext* context(const std::string& kind);
    // Fired whenever context() changes, including when it clears. Poll instead when
    // the consumer already draws every frame; both are fine.
    static boost::signals2::connection listen(const signal_t::slot_type& slot);

    // The one-liner: publish on mouse enter (data computed then), retract on leave.
    // Lives as long as the control does.
    static void bind(LLUICtrl* ctrl, const std::string& kind, data_fn_t data);
    static void bind(LLUICtrl* ctrl, const std::string& kind, const LLSD& data);

private:
    static void refresh();
};

#endif // SK_HOVER_H
