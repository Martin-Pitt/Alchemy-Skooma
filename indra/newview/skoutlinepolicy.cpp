/**
 * @file skoutlinepolicy.cpp
 * @brief The outline capture's pure decisions: the cell grid of a layer, the atlas slots, render order and budget.
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

#include "linden_common.h"

#include "skoutlinepolicy.h"

#include <algorithm>
#include <cmath>

namespace SKOutlinePolicy
{
    Grid gridFor(F32 min_x, F32 min_y, F32 max_x, F32 max_y, const GridLimits& limits)
    {
        Grid g;
        if (!(max_x >= min_x) || !(max_y >= min_y) || limits.target_cells <= 0 || limits.max_cells <= 2)
        {
            return g;
        }

        const F32 span = std::max({ max_x - min_x, max_y - min_y, limits.min_cell });
        F32 cell = span / (F32)limits.target_cells;
        cell = std::min(std::max(cell, limits.min_cell), std::max(limits.max_cell, limits.min_cell));

        // Coarsen until the padded grid fits the slot; the snap can add a cell per side, so loop rather than solve.
        for (S32 tries = 0; tries < 64; ++tries)
        {
            const S32 ix0 = (S32)std::floor(min_x / cell) - 1;
            const S32 iy0 = (S32)std::floor(min_y / cell) - 1;
            const S32 cols = (S32)std::ceil(max_x / cell) + 1 - ix0;
            const S32 rows = (S32)std::ceil(max_y / cell) + 1 - iy0;
            if (cols <= limits.max_cells && rows <= limits.max_cells)
            {
                g.cell = cell;
                g.cols = cols;
                g.rows = rows;
                g.x0 = ix0 * cell;
                g.y0 = iy0 * cell;
                g.x1 = g.x0 + cols * cell;
                g.y1 = g.y0 + rows * cell;
                return g;
            }
            g.coarsened = true;
            cell *= (F32)std::max(cols, rows) / (F32)(limits.max_cells - 2);
        }
        return Grid();
    }

    SlotAtlas::SlotAtlas(S32 atlas_size, S32 slot_size)
        : mAtlasSize(atlas_size)
        , mSlotSize(std::max(slot_size, 1))
        , mPerRow(std::max(atlas_size / std::max(slot_size, 1), 0))
        , mTaken((size_t)mPerRow * mPerRow, false)
    {
    }

    S32 SlotAtlas::acquire()
    {
        for (size_t i = 0; i < mTaken.size(); ++i)
        {
            if (!mTaken[i])
            {
                mTaken[i] = true;
                ++mUsed;
                return (S32)i;
            }
        }
        return -1;
    }

    void SlotAtlas::release(S32 slot)
    {
        if (inUse(slot))
        {
            mTaken[slot] = false;
            --mUsed;
        }
    }

    void SlotAtlas::clear()
    {
        std::fill(mTaken.begin(), mTaken.end(), false);
        mUsed = 0;
    }

    bool SlotAtlas::inUse(S32 slot) const
    {
        return slot >= 0 && (size_t)slot < mTaken.size() && mTaken[slot];
    }

    void SlotAtlas::origin(S32 slot, S32& x, S32& y) const
    {
        x = (slot % mPerRow) * mSlotSize;
        y = (slot / mPerRow) * mSlotSize;
    }

    Reason reasonFor(bool rendered, bool lost, bool changed, bool moving, F64 since_render, F64 moving_interval)
    {
        if (!rendered)
        {
            return Reason::FIRST;
        }
        if (lost)
        {
            return Reason::CONTEXT;
        }
        if (changed)
        {
            return Reason::CHANGED;
        }
        if (moving && since_render >= moving_interval)
        {
            return Reason::MOVING;
        }
        return Reason::NONE;
    }

    namespace
    {
        S32 rank(Reason r)
        {
            switch (r)
            {
                case Reason::FIRST:
                case Reason::CONTEXT:
                    return 0;
                case Reason::CHANGED:
                    return 1;
                case Reason::MOVING:
                    return 2;
                default:
                    return 3;
            }
        }
    }

    void order(std::vector<Candidate>& candidates)
    {
        std::erase_if(candidates, [](const Candidate& c) { return c.reason == Reason::NONE; });
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& a, const Candidate& b)
                         {
                             if (rank(a.reason) != rank(b.reason))
                             {
                                 return rank(a.reason) < rank(b.reason);
                             }
                             if (a.distance != b.distance)
                             {
                                 return a.distance < b.distance;
                             }
                             return a.waiting > b.waiting;
                         });
    }

    void FrameBudget::begin(F64 budget_ms, S32 max_renders)
    {
        mBudget = budget_ms;
        mMaxRenders = max_renders;
        mRenders = 0;
        mSpent = 0.0;
    }

    bool FrameBudget::fits() const
    {
        if (mBudget <= 0.0 || mRenders >= mMaxRenders)
        {
            return false;
        }
        return mRenders == 0 || mSpent + mEstimate <= mBudget;
    }

    void FrameBudget::spend(F64 ms)
    {
        ++mRenders;
        mSpent += ms;
        // A slow running estimate, so one hitch does not stop the queue for long.
        mEstimate = mEstimate * 0.8 + ms * 0.2;
    }

    bool settled(bool all_top_lod, F64 since_change, F64 settle_seconds)
    {
        return all_top_lod && since_change >= settle_seconds;
    }
}
