/**
 * @file skmaplabels.h
 * @brief Mini-map hover membership with hysteresis, and nametag placement around the pointer.
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

#ifndef SK_MAPLABELS_H
#define SK_MAPLABELS_H

#include "lluuid.h"
#include "v2math.h"

#include <vector>

namespace skmap
{
    constexpr F32 HOVER_ENTER_PX = 10.f;
    constexpr F32 HOVER_EXIT_PX = 12.f;
    constexpr F32 PICK_PX = 16.f;          // right-click reach on the mini-map
    constexpr F32 TACTICAL_PICK_PX = 64.f; // and on the tactical overview, a much larger view
    constexpr F32 TAG_GAP_PX = 2.f;
    constexpr F32 TAG_EASE_S = 0.2f;
    constexpr F32 TAG_MAX_WIDTH_PX = 190.f;
    constexpr F32 TAG_NARROW_WINDOW_PX = 420.f;
    constexpr F32 TAG_NARROW_MAX_PX = 145.f;
    constexpr F32 TAG_EDGE_PX = 8.f;

    /// A tag's widest box for a window width: the study's narrow-stage rule, else the setting.
    F32 tagMaxWidth(F32 window_width, F32 setting);

    /// A dot that can be hovered, in screen pixels.
    struct HoverDot
    {
        LLUUID mId;
        LLVector2 mPos;
    };

    /// The dots whose names show: a dot joins within the enter radius and leaves past the exit
    /// radius, with no dwell. Suppression (Escape, the menu closing) clears it until the pointer
    /// moves.
    class HoverSet
    {
    public:
        /// Members ordered by distance to the pointer, then id.
        const std::vector<LLUUID>& members() const { return mMembers; }
        bool empty() const { return mMembers.empty(); }
        bool suppressed() const { return mSuppressed; }

        void update(const std::vector<HoverDot>& dots, const LLVector2& pointer, F32 enter = HOVER_ENTER_PX, F32 exit = HOVER_EXIT_PX);
        void clear() { mMembers.clear(); }
        /// Clear and hold the names off until the pointer leaves this position.
        void suppress(const LLVector2& pointer);
        /// Lift the suppression on the first change in pointer position; true when it lifted.
        bool notePointer(const LLVector2& pointer);

    private:
        std::vector<LLUUID> mMembers;
        bool mSuppressed = false;
        LLVector2 mSuppressedAt;
    };

    /// A box by its minimum corner in window pixels, y down.
    struct LabelRect
    {
        F64 mX = 0.0;
        F64 mY = 0.0;
        F64 mW = 0.0;
        F64 mH = 0.0;
        bool operator==(const LabelRect& o) const { return mX == o.mX && mY == o.mY && mW == o.mW && mH == o.mH; }
    };

    bool rectOverlap(const LabelRect& a, const LabelRect& b, F64 gap = TAG_GAP_PX);

    /// One tag to place: its dot, its size, and where it was aimed last time, y down.
    struct LabelInput
    {
        LLVector2 mAnchor;
        F64 mW = 0.0;
        F64 mH = 0.0;
        bool mFresh = true;
        F64 mPrevX = 0.0;
        F64 mPrevY = 0.0;
    };

    /// The placement problem in the study's own frame: window pixels, origin top left, y down.
    struct LabelProblem
    {
        std::vector<LabelInput> mLabels;    ///< in hover order
        LLVector2 mCentre;                  ///< the pointer
        std::vector<LabelRect> mObstacles;  ///< drawn dots, the cog and the grip
        F64 mWidth = 0.0;                   ///< the whole viewer window
        F64 mHeight = 0.0;
        LabelRect mRoom;                    ///< where tags may go, clear of the bars; empty is the whole window
    };

    /// The study's search with branch-and-bound pruning: the same answer as solveBruteForce.
    std::vector<LabelRect> solve(const LabelProblem& problem);
    /// The study's placement loop as written (general.template.html, placeLabels).
    std::vector<LabelRect> solveBruteForce(const LabelProblem& problem);

    /// Tags as shown: solved when something they depend on moved, eased towards their targets.
    class LabelLayout
    {
    public:
        struct Member
        {
            LLUUID mId;
            LLVector2 mAnchor;              ///< y down
            F32 mW = 0.f;
            F32 mH = 0.f;
        };
        struct Input
        {
            std::vector<Member> mMembers;   ///< in hover order
            LLVector2 mPointer;             ///< y down
            std::vector<LabelRect> mObstacles;
            F32 mWidth = 0.f;
            F32 mHeight = 0.f;
            LabelRect mRoom;                ///< y down; empty is the whole window
            bool mChrome = false;
        };
        struct Tag
        {
            LLUUID mId;
            F64 mX = 0.0, mY = 0.0;         ///< shown, y down
            F64 mTX = 0.0, mTY = 0.0;       ///< target
            F64 mW = 0.0, mH = 0.0;
            bool mFresh = true;
            LabelRect rect() const { return { mX, mY, mW, mH }; }
        };

        void update(const Input& input, F32 dt);
        void clear();
        const std::vector<Tag>& tags() const { return mTags; }
        U32 solves() const { return mSolves; }

        /// Where a connector from the dot meets the shown box: its nearest point.
        static LLVector2 connectorEnd(const Tag& tag, const LLVector2& anchor);

    private:
        bool needsSolve(const Input& input) const;

        std::vector<Tag> mTags;
        Input mLast;
        bool mHasLast = false;
        U32 mSolves = 0;
    };
}

#endif // SK_MAPLABELS_H
