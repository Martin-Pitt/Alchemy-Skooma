/**
 * @file sknavprobes.cpp
 * @brief Automatic reflection probe spheres for rooms: parts, placement, clearing placed probes, budget and churn.
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

#include "sknavprobes.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>

namespace sknav
{
    namespace
    {
        struct Rect
        {
            S32 mX = 0, mY = 0, mW = 0, mH = 0;
            S32 area() const { return mW * mH; }
        };

        /// The largest rectangle of free cells, by histograms over the rows.
        Rect largestRect(const std::vector<U8>& free_cells, S32 w, S32 h)
        {
            Rect best;
            std::vector<S32> heights(w, 0);
            std::vector<S32> stack;
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    heights[x] = free_cells[y * w + x] ? heights[x] + 1 : 0;
                }
                stack.clear();
                for (S32 x = 0; x <= w; ++x)
                {
                    const S32 cur = x < w ? heights[x] : 0;
                    while (!stack.empty() && heights[stack.back()] >= cur)
                    {
                        const S32 top = heights[stack.back()];
                        stack.pop_back();
                        const S32 left = stack.empty() ? 0 : stack.back() + 1;
                        const S32 width = x - left;
                        if (top * width > best.area())
                        {
                            best.mX = left;
                            best.mW = width;
                            best.mH = top;
                            best.mY = y - top + 1;
                        }
                    }
                    stack.push_back(x);
                }
            }
            return best;
        }

        /// Chamfer distance in cells to the nearest cell outside the plan.
        std::vector<F32> clearance(const std::vector<U8>& mask, S32 w, S32 h)
        {
            const F32 big = 1e9f, diag = 1.41421356f;
            std::vector<F32> d(mask.size());
            for (size_t i = 0; i < mask.size(); ++i)
            {
                d[i] = mask[i] ? big : 0.f;
            }
            auto at = [&](S32 x, S32 y) { return x < 0 || y < 0 || x >= w || y >= h ? 0.f : d[y * w + x]; };
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    F32& v = d[y * w + x];
                    if (v == 0.f)
                    {
                        continue;
                    }
                    v = std::min({ v, at(x - 1, y) + 1.f, at(x, y - 1) + 1.f, at(x - 1, y - 1) + diag, at(x + 1, y - 1) + diag });
                }
            }
            for (S32 y = h - 1; y >= 0; --y)
            {
                for (S32 x = w - 1; x >= 0; --x)
                {
                    F32& v = d[y * w + x];
                    if (v == 0.f)
                    {
                        continue;
                    }
                    v = std::min({ v, at(x + 1, y) + 1.f, at(x, y + 1) + 1.f, at(x + 1, y + 1) + diag, at(x - 1, y + 1) + diag });
                }
            }
            return d;
        }
    }

    bool FloorPlan::in(S32 x, S32 y) const
    {
        return x >= 0 && y >= 0 && x < mW && y < mH && !std::isnan(mFloor[y * mW + x]);
    }

    bool wantsProbes(const Space& space, const ProbeParams& params)
    {
        if (space.mBucket != EnclosureBucket::INDOORS && space.mBucket != EnclosureBucket::SHELTERED)
        {
            return false;
        }
        return space.mCeilingMean > space.mFloorMin && space.mOpenShare < params.mOpenShare;
    }

    FloorPlan floorPlanOf(const Space& space, const ProbeParams& params)
    {
        FloorPlan plan;
        plan.mCell = params.mCellM;
        plan.mCeiling = space.mCeilingMean;
        if (space.mRaster.empty())
        {
            return plan;
        }
        const S64 bias = 1ll << 20;
        S32 x0 = std::numeric_limits<S32>::max(), y0 = x0, x1 = std::numeric_limits<S32>::min(), y1 = x1;
        for (const auto& r : space.mRaster)
        {
            const S32 x = static_cast<S32>(static_cast<S64>((r.first >> 42) & 0x1fffff) - bias);
            const S32 y = static_cast<S32>(static_cast<S64>((r.first >> 21) & 0x1fffff) - bias);
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        plan.mX0 = x0;
        plan.mY0 = y0;
        plan.mW = x1 - x0 + 1;
        plan.mH = y1 - y0 + 1;
        plan.mFloor.assign(static_cast<size_t>(plan.mW) * plan.mH, std::numeric_limits<F32>::quiet_NaN());
        for (const auto& r : space.mRaster)
        {
            const S32 x = static_cast<S32>(static_cast<S64>((r.first >> 42) & 0x1fffff) - bias) - x0;
            const S32 y = static_cast<S32>(static_cast<S64>((r.first >> 21) & 0x1fffff) - bias) - y0;
            // The raster holds floors to the metre; the space's lowest floor is exact.
            const F32 z = std::max(static_cast<F32>(static_cast<S64>(r.first & 0x1fffff) - bias), space.mFloorMin);
            F32& f = plan.mFloor[y * plan.mW + x];
            f = std::isnan(f) ? z : std::min(f, z);
        }
        return plan;
    }

    std::vector<ProbePart> placeParts(const FloorPlan& plan, const ProbeParams& params)
    {
        std::vector<ProbePart> out;
        const S32 w = plan.mW, h = plan.mH;
        if (w <= 0 || h <= 0)
        {
            return out;
        }
        const size_t n = static_cast<size_t>(w) * h;
        std::vector<U8> mask(n, 0);
        for (S32 y = 0; y < h; ++y)
        {
            for (S32 x = 0; x < w; ++x)
            {
                mask[y * w + x] = plan.in(x, y) ? 1 : 0;
            }
        }
        const F32 cell_area = plan.mCell * plan.mCell;
        const S32 min_cells = std::max(1, static_cast<S32>(std::ceil(params.mMinPartM2 / cell_area)));
        // A tile this wide is covered horizontally by a sphere of the largest radius.
        const S32 tile = std::max(1, static_cast<S32>(params.mMaxRadiusM * 1.41421356f / plan.mCell));

        std::vector<S32> owner(n, -1);
        std::vector<U8> free_cells = mask;
        S32 parts = 0;
        for (U32 i = 0; i < params.mMaxRects; ++i)
        {
            const Rect r = largestRect(free_cells, w, h);
            if (r.area() == 0 || (parts > 0 && r.area() < min_cells))
            {
                break;
            }
            const S32 nx = (r.mW + tile - 1) / tile, ny = (r.mH + tile - 1) / tile;
            for (S32 ty = 0; ty < ny; ++ty)
            {
                for (S32 tx = 0; tx < nx; ++tx)
                {
                    const S32 xa = r.mX + tx * r.mW / nx, xb = r.mX + (tx + 1) * r.mW / nx;
                    const S32 ya = r.mY + ty * r.mH / ny, yb = r.mY + (ty + 1) * r.mH / ny;
                    for (S32 y = ya; y < yb; ++y)
                    {
                        for (S32 x = xa; x < xb; ++x)
                        {
                            owner[y * w + x] = parts;
                            free_cells[y * w + x] = 0;
                        }
                    }
                    ++parts;
                }
            }
        }

        // The rest joins the part it floods from first, or starts its own where none reaches.
        std::deque<S32> queue;
        auto flood = [&]()
        {
            while (!queue.empty())
            {
                const S32 i = queue.front();
                queue.pop_front();
                const S32 x = i % w, y = i / w;
                const S32 nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
                for (const auto& p : nb)
                {
                    if (p[0] < 0 || p[1] < 0 || p[0] >= w || p[1] >= h)
                    {
                        continue;
                    }
                    const S32 j = p[1] * w + p[0];
                    if (mask[j] && owner[j] < 0)
                    {
                        owner[j] = owner[i];
                        queue.push_back(j);
                    }
                }
            }
        };
        for (size_t i = 0; i < n; ++i)
        {
            if (owner[i] >= 0)
            {
                queue.push_back(static_cast<S32>(i));
            }
        }
        flood();
        for (size_t i = 0; i < n; ++i)
        {
            if (mask[i] && owner[i] < 0)
            {
                owner[i] = parts++;
                queue.push_back(static_cast<S32>(i));
                flood();
            }
        }

        // Fold the smallest undersized part into the neighbour it shares most edge with.
        struct Stats
        {
            S32 mCells = 0;
            S32 mX0 = 0, mY0 = 0, mX1 = -1, mY1 = -1;
        };
        auto measure = [&]()
        {
            std::vector<Stats> st(parts);
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    const S32 o = owner[y * w + x];
                    if (o < 0)
                    {
                        continue;
                    }
                    Stats& s = st[o];
                    if (s.mCells++ == 0)
                    {
                        s.mX0 = s.mX1 = x;
                        s.mY0 = s.mY1 = y;
                    }
                    s.mX0 = std::min(s.mX0, x);
                    s.mX1 = std::max(s.mX1, x);
                    s.mY0 = std::min(s.mY0, y);
                    s.mY1 = std::max(s.mY1, y);
                }
            }
            return st;
        };
        const S32 min_width = std::max(1, static_cast<S32>(std::ceil(params.mMinPartWidthM / plan.mCell - 1e-4f)));
        while (true)
        {
            const std::vector<Stats> st = measure();
            S32 live = 0;
            for (const Stats& s : st)
            {
                live += s.mCells > 0 ? 1 : 0;
            }
            S32 worst = -1;
            for (S32 p = 0; p < parts; ++p)
            {
                const Stats& s = st[p];
                if (s.mCells == 0)
                {
                    continue;
                }
                const bool small = s.mCells < min_cells;
                const bool narrow = std::min(s.mX1 - s.mX0 + 1, s.mY1 - s.mY0 + 1) < min_width && live > 1;
                if ((small || narrow) && (worst < 0 || s.mCells < st[worst].mCells))
                {
                    worst = p;
                }
            }
            if (worst < 0)
            {
                break;
            }
            std::map<S32, S32> shared;
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    if (owner[y * w + x] != worst)
                    {
                        continue;
                    }
                    const S32 nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
                    for (const auto& p : nb)
                    {
                        if (p[0] >= 0 && p[1] >= 0 && p[0] < w && p[1] < h)
                        {
                            const S32 o = owner[p[1] * w + p[0]];
                            if (o >= 0 && o != worst)
                            {
                                shared[o]++;
                            }
                        }
                    }
                }
            }
            S32 into = -1, most = 0;
            for (const auto& s : shared)
            {
                if (s.second > most)
                {
                    most = s.second;
                    into = s.first;
                }
            }
            for (S32& o : owner)
            {
                if (o == worst)
                {
                    o = into;   // -1 drops a small island nothing borders
                }
            }
        }

        const std::vector<F32> clear = clearance(mask, w, h);
        const std::vector<Stats> st = measure();
        std::vector<S32> order;
        for (S32 p = 0; p < parts; ++p)
        {
            if (st[p].mCells > 0)
            {
                order.push_back(p);
            }
        }
        std::stable_sort(order.begin(), order.end(), [&](S32 a, S32 b) { return st[a].mCells > st[b].mCells; });

        for (size_t k = 0; k < order.size(); ++k)
        {
            const S32 p = order[k];
            F64 cx = 0.0, cy = 0.0;
            F32 lo = std::numeric_limits<F32>::max();
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    if (owner[y * w + x] == p)
                    {
                        cx += x;
                        cy += y;
                        lo = std::min(lo, plan.floorAt(x, y));
                    }
                }
            }
            cx /= st[p].mCells;
            cy /= st[p].mCells;
            S32 best = -1;
            F32 best_clear = -1.f, best_d2 = 0.f;
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    const S32 i = y * w + x;
                    if (owner[i] != p)
                    {
                        continue;
                    }
                    const F32 d2 = static_cast<F32>((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    if (clear[i] > best_clear + 1e-3f || (std::fabs(clear[i] - best_clear) <= 1e-3f && d2 < best_d2))
                    {
                        best = i;
                        best_clear = clear[i];
                        best_d2 = d2;
                    }
                }
            }
            const F32 ox = (plan.mX0 + best % w + 0.5f) * plan.mCell;
            const F32 oy = (plan.mY0 + best / w + 0.5f) * plan.mCell;
            const F32 half = plan.mCeiling > lo ? (plan.mCeiling - lo) * 0.5f : 1.5f;

            F32 reach2 = 0.f;
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    if (owner[y * w + x] != p)
                    {
                        continue;
                    }
                    const F32 ax = (plan.mX0 + x) * plan.mCell, ay = (plan.mY0 + y) * plan.mCell;
                    const F32 dx = std::max(std::fabs(ax - ox), std::fabs(ax + plan.mCell - ox));
                    const F32 dy = std::max(std::fabs(ay - oy), std::fabs(ay + plan.mCell - oy));
                    reach2 = std::max(reach2, dx * dx + dy * dy);
                }
            }

            ProbePart part;
            part.mPart = static_cast<U32>(k);
            part.mOrigin.set(ox, oy, lo + half);
            part.mRadius = std::min(std::sqrt(reach2 + half * half), params.mMaxRadiusM);
            part.mArea = st[p].mCells * cell_area;
            part.mMin.set((plan.mX0 + st[p].mX0) * plan.mCell, (plan.mY0 + st[p].mY0) * plan.mCell, lo);
            part.mMax.set((plan.mX0 + st[p].mX1 + 1) * plan.mCell, (plan.mY0 + st[p].mY1 + 1) * plan.mCell, lo + 2.f * half);
            out.push_back(part);
        }
        return out;
    }

    std::vector<ProbePart> spaceProbes(const Space& space, const ProbeParams& params)
    {
        if (!wantsProbes(space, params))
        {
            return {};
        }
        return placeParts(floorPlanOf(space, params), params);
    }

    F32 distanceTo(const ProbeInfluence& influence, const LLVector3& p)
    {
        const LLVector3 d = p - influence.mCentre;
        if (!influence.mBox)
        {
            return std::max(d.length() - influence.mRadius, 0.f);
        }
        F32 sum = 0.f;
        for (S32 a = 0; a < 3; ++a)
        {
            const F32 out = std::max(std::fabs(d * influence.mAxes[a]) - influence.mHalf.mV[a], 0.f);
            sum += out * out;
        }
        return std::sqrt(sum);
    }

    F32 clearRadius(const LLVector3& origin, F32 radius, const std::vector<ProbeInfluence>& placed, const ProbeParams& params)
    {
        F32 r = radius;
        for (const ProbeInfluence& inf : placed)
        {
            r = std::min(r, distanceTo(inf, origin));
        }
        return r < params.mMinRadiusM ? 0.f : r;
    }

    void keepBudget(std::vector<ProbeSlot>& slots, U32 cap)
    {
        std::stable_sort(slots.begin(), slots.end(), [](const ProbeSlot& a, const ProbeSlot& b)
                         {
                             if (a.mDistance != b.mDistance)
                             {
                                 return a.mDistance < b.mDistance;
                             }
                             return a.mArea > b.mArea;
                         });
        if (slots.size() > cap)
        {
            slots.resize(cap);
        }
    }

    bool shouldReplace(const LLVector3& held_origin, F32 held_radius, const LLVector3& origin, F32 radius, const ProbeParams& params)
    {
        return dist_vec(held_origin, origin) > params.mMoveM || std::fabs(radius - held_radius) > params.mRadiusShare * held_radius;
    }
}
