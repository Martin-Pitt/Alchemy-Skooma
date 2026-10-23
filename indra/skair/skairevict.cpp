/**
 * @file skairevict.cpp
 * @brief The air budget: what an evicted tile keeps, and the order tiles are evicted in.
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

#include "skairevict.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace skworld
{
    TileAirPtr coarseAir(const TileAir& air)
    {
        auto out = std::make_shared<TileAir>();
        out->mOrigin = air.mOrigin;
        out->mCell = air.mCell;
        out->mUnit = air.mUnit;
        out->mWidth = air.mWidth;
        out->mHeight = air.mHeight;
        // The detail's own tops, shared: the coarse air costs its header.
        out->mTops = air.mTops;
        out->countLive();
        return out;
    }

    F64 accumulateNearness(F64 score, F64 distance, F64 radius, F64 dt, F64 decay_seconds)
    {
        const F64 r = std::max(radius, 1.0);
        return score * std::exp(-dt / std::max(decay_seconds, 1e-3)) + dt * r / (r + std::max(distance, 0.0));
    }

    std::vector<size_t> chooseEvictions(const std::vector<EvictionCandidate>& candidates, size_t budget_bytes, F64 protect_distance)
    {
        size_t total = 0;
        for (const EvictionCandidate& c : candidates)
        {
            total += c.mBytes;
        }
        std::vector<size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const EvictionCandidate& x = candidates[a];
            const EvictionCandidate& y = candidates[b];
            return x.mRank != y.mRank ? x.mRank < y.mRank
                 : x.mScore != y.mScore ? x.mScore < y.mScore
                 : x.mDistance > y.mDistance;
        });
        std::vector<size_t> out;
        for (size_t i : order)
        {
            if (total <= budget_bytes)
            {
                break;
            }
            if (candidates[i].mDistance < protect_distance)
            {
                continue;
            }
            total -= candidates[i].mBytes;
            out.push_back(i);
        }
        return out;
    }
}
