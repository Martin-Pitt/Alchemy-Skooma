/**
 * @file skinventoryexplorerdrag.h
 * @brief Inventory Explorer's drag and drop: starting drags, drop verdicts, spring-loading, autoscroll and undo
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

#ifndef SK_SKINVENTORYEXPLORERDRAG_H
#define SK_SKINVENTORYEXPLORERDRAG_H

#include "llframetimer.h"
#include "llrect.h"
#include "lltooldraganddrop.h"
#include "skdragfeedback.h"

#include <functional>
#include <map>

// Drags leave through LLToolDragAndDrop, so they drop anywhere the viewer takes inventory; drops onto the
// Explorer's own widgets go through the gallery's free functions, which carry every stock rule.
namespace SKInventoryExplorerDrag
{
    constexpr S32 DRAG_THRESHOLD = 5;       // pixels before a press on an item lifts it
    constexpr S32 LASSO_THRESHOLD = 4;      // |dx| + |dy| before a press elsewhere draws a rectangle
    constexpr F32 TAB_SPRING_SECONDS = 0.5f;
    constexpr F32 TICK_SECONDS = 0.04f;     // spring and pane autoscroll checks
    constexpr S32 SCROLL_BAND = 24;
    constexpr S32 SCROLL_STEP = 14;
    constexpr S32 SCROLL_REACH = 30;        // a pane counts as hit this far past its top and bottom

    // What dropping on the target under the pointer would do, for the frame it was asked in.
    struct Verdict
    {
        bool mOK = false;
        bool mQuiet = false;    // a refusal shown without the red outline
        bool mOntoCargo = false; // the target is a dragged folder: never marked, never sprung
    };

    // Starts the viewer's drag with the objects; a mix of the Library and your own inventory is refused.
    bool begin(const uuid_vec_t& ids);
    // True while a drag of inventory is under way.
    bool isDragging();

    // One cargo object over a folder; the tooltip ends up holding the verdict for the whole cargo.
    bool dropOnFolder(const LLUUID& dest_id, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                      EAcceptance* accept, std::string& tooltip_msg);
    // One cargo object over the Favorites heading: folders become favourites.
    bool dropOnFavorites(bool drop, EDragAndDropType cargo_type, void* cargo_data, EAcceptance* accept,
                         std::string& tooltip_msg);
    // The verdict of the last target asked about.
    const Verdict& verdict();

    // Every string comes from the floater's XUI.
    void setStrings(const std::map<std::string, std::string>& strings);
    // Where the last action goes: the owning floater's status bar.
    void setStatusCallback(const void* owner, const std::function<void(const std::string&)>& cb);
    // Drops the callback and the undo history, if the owner is still the one that set them.
    void releaseOwner(const void* owner);

    // Undo: plain moves inside My Inventory, and favourite adds and removals; the last 50 are kept.
    void recordFavorites(const uuid_vec_t& ids, bool added);
    // Any other undoable action (a saved search added or removed), with its status text.
    void recordAction(const std::function<void()>& undo_fn, const std::string& text);
    // Refuses the drop with one of the strings, shown red: "drag_into_search".
    bool refuse(const std::string& key, EAcceptance* accept, std::string& tooltip_msg);
    bool canUndo();
    void undo();

    // How long a folder is held before it springs open: the folder view's FolderAutoOpenDelay.
    F32 springSeconds();
    // The mark the last verdict takes: accepted, refused with a reason, or quiet (a dragged folder is never marked).
    EDragMark markKind();
    // Marks the rect the view would light for the last verdict, for this frame's pass.
    void mark(const LLView* view, const LLRect& local_rect);
    // Draws the view's mark of this frame, if it has one, under its text; the open folder's space takes no fill.
    void drawMark(const LLView* view, bool fill = true);
}

// Hover timing one widget keeps while a drag is over it: springs open after a delay, checked every 40 ms; after an
// autoscroll nothing springs until the pointer itself moves 3 px.
class SKDragHover
{
public:
    SKDragHover();

    // Called for each frame's hover; true once the same target has been held for the delay.
    bool spring(const LLUUID& target, S32 screen_x, S32 screen_y, F32 delay);
    // An autoscroll moved the content under a resting pointer.
    void scrolled(S32 screen_x, S32 screen_y);
    // True at most once per period, for autoscroll steps.
    bool tick(F32 period);
    // Marks the frame a drag was over the widget; isActive() is true for that frame and the next.
    void touch();
    bool isActive() const;
    void reset();

private:
    LLUUID mTarget;
    LLFrameTimer mHeld;
    LLFrameTimer mTick;
    LLFrameTimer mLastCheck;
    bool mStill;
    S32 mStillX;
    S32 mStillY;
    U32 mFrame;
};

#endif // SK_SKINVENTORYEXPLORERDRAG_H
