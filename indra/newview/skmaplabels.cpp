/**
 * @file skmaplabels.cpp
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

#include "linden_common.h"

#include "skmaplabels.h"

#include <algorithm>
#include <cmath>

namespace skmap
{
    namespace
    {
        constexpr S32 RINGS = 12;
        constexpr S32 STEPS = 48;
        constexpr F64 PI = 3.14159265358979323846;

        F64 dist(F64 ax, F64 ay, F64 bx, F64 by)
        {
            return std::hypot(ax - bx, ay - by);
        }

        /// The study's candidate order for one tag: its previous target, the rings, the grid.
        template <typename Fn>
        void forEachCandidate(const LabelProblem& p, const LabelInput& tag, F64 r, F64 preferred, Fn&& fn)
        {
            const F64 w = tag.mW, h = tag.mH;
            const F64 cx = p.mCentre.mV[VX], cy = p.mCentre.mV[VY];
            if (!tag.mFresh)
            {
                fn(tag.mPrevX, tag.mPrevY);
            }
            for (S32 ring = 0; ring < RINGS; ++ring)
            {
                for (S32 j = 0; j < STEPS; ++j)
                {
                    const F64 angle = preferred + j * PI / 24.0;
                    const F64 rr = r + 2.0 + ring * 8.0;
                    fn(cx + std::cos(angle) * (rr + w * 0.45) - w / 2.0, cy + std::sin(angle) * (rr + h * 0.5) - h / 2.0);
                }
            }
            for (F64 y = 8.0; y + h < p.mHeight - 8.0; y += h + 2.0)
            {
                for (F64 x = 8.0; x + w < p.mWidth - 7.0; x += w + 2.0)
                {
                    fn(x, y);
                }
            }
        }

        F64 radius(const LabelProblem& p)
        {
            F64 r = 13.0;
            for (const LabelInput& tag : p.mLabels)
            {
                r = std::max(r, dist(tag.mAnchor.mV[VX], tag.mAnchor.mV[VY], p.mCentre.mV[VX], p.mCentre.mV[VY]));
            }
            return r;
        }

        F64 preferredAngle(const LabelProblem& p, size_t n)
        {
            const LabelInput& tag = p.mLabels[n];
            const F64 fallback = -PI + n * 2.0 * PI / p.mLabels.size();
            const F64 ax = tag.mAnchor.mV[VX], ay = tag.mAnchor.mV[VY];
            const F64 cx = p.mCentre.mV[VX], cy = p.mCentre.mV[VY];
            return dist(ax, ay, cx, cy) > 2.0 ? std::atan2(ay - cy, ax - cx) : fallback;
        }

        /// Every part of the cost but the conflicts.
        F64 geometricCost(const LabelProblem& p, const LabelInput& tag, F64 preferred, F64 x, F64 y)
        {
            const F64 cx = p.mCentre.mV[VX], cy = p.mCentre.mV[VY];
            const F64 dx = x + tag.mW / 2.0 - cx, dy = y + tag.mH / 2.0 - cy;
            const F64 angle_error = 1.0 - std::cos(std::atan2(dy, dx) - preferred);
            return dist(x + tag.mW / 2.0, y + tag.mH / 2.0, tag.mAnchor.mV[VX], tag.mAnchor.mV[VY]) + angle_error * 160.0
                 + (tag.mFresh ? 0.0 : dist(x, y, tag.mPrevX, tag.mPrevY) * 0.08);
        }

        LabelRect clampCandidate(const LabelProblem& p, const LabelInput& tag, F64 x, F64 y)
        {
            LabelRect q;
            q.mX = std::max(8.0, std::min(p.mWidth - tag.mW - 8.0, x));
            q.mY = std::max(8.0, std::min(p.mHeight - tag.mH - 8.0, y));
            q.mW = tag.mW;
            q.mH = tag.mH;
            return q;
        }
    }

    F32 tagMaxWidth(F32 window_width, F32 setting)
    {
        return window_width < TAG_NARROW_WINDOW_PX ? llmin(TAG_NARROW_MAX_PX, (window_width - 28.f) / 2.f) : setting;
    }

    void HoverSet::update(const std::vector<HoverDot>& dots, const LLVector2& pointer, F32 enter, F32 exit)
    {
        std::vector<std::pair<F32, LLUUID>> next;
        for (const HoverDot& dot : dots)
        {
            const bool member = std::find(mMembers.begin(), mMembers.end(), dot.mId) != mMembers.end();
            const F32 d = dist_vec(dot.mPos, pointer);
            if (d <= (member ? exit : enter))
            {
                next.emplace_back(d, dot.mId);
            }
        }
        std::sort(next.begin(), next.end());
        mMembers.clear();
        for (const auto& n : next)
        {
            if (std::find(mMembers.begin(), mMembers.end(), n.second) == mMembers.end())
            {
                mMembers.push_back(n.second);
            }
        }
    }

    void HoverSet::suppress(const LLVector2& pointer)
    {
        mMembers.clear();
        mSuppressed = true;
        mSuppressedAt = pointer;
    }

    bool HoverSet::notePointer(const LLVector2& pointer)
    {
        if (mSuppressed && pointer != mSuppressedAt)
        {
            mSuppressed = false;
            return true;
        }
        return false;
    }

    bool rectOverlap(const LabelRect& a, const LabelRect& b, F64 gap)
    {
        return a.mX < b.mX + b.mW + gap && a.mX + a.mW + gap > b.mX && a.mY < b.mY + b.mH + gap && a.mY + a.mH + gap > b.mY;
    }

    std::vector<LabelRect> solveBruteForce(const LabelProblem& p)
    {
        std::vector<LabelRect> placed = p.mObstacles;
        std::vector<LabelRect> out;
        const F64 r = radius(p);
        for (size_t n = 0; n < p.mLabels.size(); ++n)
        {
            const LabelInput& tag = p.mLabels[n];
            const F64 preferred = preferredAngle(p, n);
            LabelRect best;
            F64 score = HUGE_VAL;
            forEachCandidate(p, tag, r, preferred, [&](F64 x, F64 y) {
                const LabelRect q = clampCandidate(p, tag, x, y);
                S32 conflicts = 0;
                for (const LabelRect& b : placed)
                {
                    conflicts += rectOverlap(q, b) ? 1 : 0;
                }
                const F64 cost = conflicts * 1e6 + geometricCost(p, tag, preferred, q.mX, q.mY);
                if (cost < score)
                {
                    best = q;
                    score = cost;
                }
            });
            placed.push_back(best);
            out.push_back(best);
        }
        return out;
    }

    std::vector<LabelRect> solve(const LabelProblem& p)
    {
        std::vector<LabelRect> placed = p.mObstacles;
        std::vector<LabelRect> out;
        const F64 r = radius(p);
        for (size_t n = 0; n < p.mLabels.size(); ++n)
        {
            const LabelInput& tag = p.mLabels[n];
            const F64 preferred = preferredAngle(p, n);
            LabelRect best;
            F64 score = HUGE_VAL;
            forEachCandidate(p, tag, r, preferred, [&](F64 x, F64 y) {
                const LabelRect q = clampCandidate(p, tag, x, y);
                const F64 geometric = geometricCost(p, tag, preferred, q.mX, q.mY);
                // A candidate that cannot beat the best is never counted; counting stops as soon
                // as it cannot. Ties keep the first, as the study does.
                if (geometric >= score)
                {
                    return;
                }
                F64 cost = geometric;
                for (const LabelRect& b : placed)
                {
                    if (rectOverlap(q, b))
                    {
                        cost += 1e6;
                        if (cost >= score)
                        {
                            return;
                        }
                    }
                }
                best = q;
                score = cost;
            });
            placed.push_back(best);
            out.push_back(best);
        }
        return out;
    }

    void LabelLayout::clear()
    {
        mTags.clear();
        mHasLast = false;
    }

    bool LabelLayout::needsSolve(const Input& in) const
    {
        if (!mHasLast || in.mMembers.size() != mLast.mMembers.size() || in.mWidth != mLast.mWidth || in.mHeight != mLast.mHeight
            || in.mChrome != mLast.mChrome || dist_vec(in.mPointer, mLast.mPointer) >= 1.f)
        {
            return true;
        }
        for (size_t i = 0; i < in.mMembers.size(); ++i)
        {
            const Member& a = in.mMembers[i];
            const Member& b = mLast.mMembers[i];
            if (a.mId != b.mId || a.mW != b.mW || a.mH != b.mH || dist_vec(a.mAnchor, b.mAnchor) >= 1.f)
            {
                return true;
            }
        }
        // Other drawn dots are obstacles: one moving a pixel moves the answer.
        if (in.mObstacles.size() != mLast.mObstacles.size())
        {
            return true;
        }
        for (size_t i = 0; i < in.mObstacles.size(); ++i)
        {
            if (std::fabs(in.mObstacles[i].mX - mLast.mObstacles[i].mX) >= 1.0 || std::fabs(in.mObstacles[i].mY - mLast.mObstacles[i].mY) >= 1.0)
            {
                return true;
            }
        }
        return false;
    }

    void LabelLayout::update(const Input& in, F32 dt)
    {
        const bool resized = mHasLast && (in.mWidth != mLast.mWidth || in.mHeight != mLast.mHeight);
        if (needsSolve(in))
        {
            // Keep the tags of members that stay, so their previous targets carry over.
            std::vector<Tag> next;
            for (const Member& m : in.mMembers)
            {
                Tag tag;
                tag.mId = m.mId;
                for (const Tag& old : mTags)
                {
                    if (old.mId == m.mId)
                    {
                        tag = old;
                        break;
                    }
                }
                tag.mW = m.mW;
                tag.mH = m.mH;
                next.push_back(tag);
            }
            LabelProblem problem;
            problem.mCentre = in.mPointer;
            problem.mObstacles = in.mObstacles;
            problem.mWidth = in.mWidth;
            problem.mHeight = in.mHeight;
            for (size_t i = 0; i < next.size(); ++i)
            {
                LabelInput li;
                li.mAnchor = in.mMembers[i].mAnchor;
                li.mW = next[i].mW;
                li.mH = next[i].mH;
                li.mFresh = next[i].mFresh;
                li.mPrevX = next[i].mTX;
                li.mPrevY = next[i].mTY;
                problem.mLabels.push_back(li);
            }
            const std::vector<LabelRect> placed = solve(problem);
            for (size_t i = 0; i < next.size(); ++i)
            {
                next[i].mTX = placed[i].mX;
                next[i].mTY = placed[i].mY;
                if (next[i].mFresh)
                {
                    next[i].mX = next[i].mTX;
                    next[i].mY = next[i].mTY;
                    next[i].mFresh = false;
                }
            }
            mTags.swap(next);
            mLast = in;
            mHasLast = true;
            ++mSolves;
        }

        const F64 mix = 1.0 - std::exp(-static_cast<F64>(dt) / TAG_EASE_S);
        for (Tag& tag : mTags)
        {
            tag.mX += (tag.mTX - tag.mX) * mix;
            tag.mY += (tag.mTY - tag.mY) * mix;
        }
        // Snapping one tag can block another's path: resolve the whole layout together.
        bool overlap = resized;
        for (size_t i = 0; i < mTags.size() && !overlap; ++i)
        {
            for (size_t j = i + 1; j < mTags.size() && !overlap; ++j)
            {
                overlap = rectOverlap(mTags[i].rect(), mTags[j].rect());
            }
        }
        if (overlap)
        {
            for (Tag& tag : mTags)
            {
                tag.mX = tag.mTX;
                tag.mY = tag.mTY;
            }
        }
    }

    LLVector2 LabelLayout::connectorEnd(const Tag& tag, const LLVector2& anchor)
    {
        return LLVector2(static_cast<F32>(llclamp(static_cast<F64>(anchor.mV[VX]), tag.mX, tag.mX + tag.mW)),
                         static_cast<F32>(llclamp(static_cast<F64>(anchor.mV[VY]), tag.mY, tag.mY + tag.mH)));
    }
}
