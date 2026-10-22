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
#include <cctype>
#include <cmath>
#include <set>

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

    F32 reach(const Box& box, const F32 point[3])
    {
        F32 d2 = 0.f;
        for (S32 k = 0; k < 3; ++k)
        {
            const F32 d = std::max(std::fabs(box.min[k] - point[k]), std::fabs(box.max[k] - point[k]));
            d2 += d * d;
        }
        return std::sqrt(d2);
    }

    namespace
    {
        F32 planSpan(const Box& b)
        {
            return std::max(b.max[0] - b.min[0], b.max[1] - b.min[1]);
        }
    }

    CrewVerdict crewVerdict(const CrewPiece& piece, const CrewLimits& limits)
    {
        if (piece.hud)
        {
            return CrewVerdict::NOT_LAYER;
        }
        if (reach(piece.box, piece.wearer) <= limits.personal_reach)
        {
            return CrewVerdict::PERSONAL;
        }
        const F32 root_span = planSpan(piece.root);
        if (root_span > 0.f && planSpan(piece.box) >= limits.root_share * root_span)
        {
            return CrewVerdict::LAYER;
        }
        if (piece.avatar_center || piece.name_affinity || piece.temp)
        {
            return CrewVerdict::LAYER;
        }
        return CrewVerdict::NOT_LAYER;
    }

    bool isThinPlane(F32 sx, F32 sy, F32 sz, bool alpha_faces, F32 max_thickness, F32 min_ratio)
    {
        const F32 lo = std::min({ sx, sy, sz });
        const F32 hi = std::max({ sx, sy, sz });
        return alpha_faces && lo <= max_thickness && hi >= min_ratio * std::max(lo, 1.0e-6f);
    }

    bool isSpeck(S32 covered_cells, S32 max_cells)
    {
        return covered_cells <= max_cells;
    }

    F32 insideFraction(const Rect& a, const Rect& b)
    {
        const F32 area = (a.x1 - a.x0) * (a.y1 - a.y0);
        if (!(area > 0.f))
        {
            return 0.f;
        }
        const F32 w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
        const F32 h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
        return w > 0.f && h > 0.f ? w * h / area : 0.f;
    }

    bool mergedInto(const Rect& a, S32 covered_a, const Rect& b, S32 covered_b, F32 inside)
    {
        return covered_a < covered_b && insideFraction(a, b) >= inside;
    }

    namespace
    {
        std::string lower(std::string s)
        {
            for (char& c : s)
            {
                c = (char)std::tolower((unsigned char)c);
            }
            return s;
        }

        // The bracketed tag a vendor leads its names with, if any.
        std::string vendorTag(const std::string& name)
        {
            const size_t open = name.find_first_of("[(");
            if (open == std::string::npos || name.find_first_not_of(" \t") != open)
            {
                return std::string();
            }
            const size_t close = name.find_first_of("])", open + 1);
            if (close == std::string::npos || close == open + 1)
            {
                return std::string();
            }
            return lower(name.substr(open + 1, close - open - 1));
        }

        // Words of four or more letters that name a product rather than a part or a version.
        std::set<std::string> productWords(const std::string& name)
        {
            static const std::set<std::string> generic = { "base", "body", "turret", "main", "seat", "root", "vehicle", "tank",
                                                           "flak", "hull", "gun", "guns", "cannon", "weapon", "object", "primitive",
                                                           "mesh", "attachment", "copy", "final", "version", "with", "edition",
                                                           "full", "temp", "pilot", "driver", "gunner", "crew", "left", "right",
                                                           "front", "rear", "parts", "part", "physics", "visual", "kit" };
            std::set<std::string> words;
            std::string word;
            const std::string tag = vendorTag(name);
            for (size_t i = 0; i <= name.size(); ++i)
            {
                const char c = i < name.size() ? name[i] : ' ';
                if (std::isalpha((unsigned char)c))
                {
                    word.push_back((char)std::tolower((unsigned char)c));
                    continue;
                }
                if (word.size() >= 4 && word != tag && !generic.count(word))
                {
                    words.insert(word);
                }
                word.clear();
            }
            return words;
        }
    }

    bool nameAffinity(const std::string& a, const std::string& b)
    {
        const std::string tag = vendorTag(a);
        if (!tag.empty() && tag == vendorTag(b))
        {
            return true;
        }
        const std::set<std::string> wa = productWords(a);
        for (const std::string& w : productWords(b))
        {
            if (wa.count(w))
            {
                return true;
            }
        }
        return false;
    }
}
