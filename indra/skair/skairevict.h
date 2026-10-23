/**
 * @file skairevict.h
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

#ifndef SK_AIREVICT_H
#define SK_AIREVICT_H

#include "skairdistil.h"

#include <vector>

namespace skworld
{
    /// A tile's air with only its solid tops, which the sky map reads: what an
    /// evicted tile keeps.
    TileAirPtr coarseAir(const TileAir& air);

    /// Nearness integrated over time with a decay: a tile the camera sits by
    /// climbs, one it left long ago sinks. Distance is from the camera to the
    /// tile's footprint.
    F64 accumulateNearness(F64 score, F64 distance, F64 radius, F64 dt, F64 decay_seconds = 60.0);

    struct EvictionCandidate
    {
        size_t mBytes = 0;
        F64 mScore = 0.0;       ///< accumulateNearness
        F64 mDistance = 0.0;
        U8 mRank = 0;           ///< what the candidate serves: higher ranks evict last
    };

    /// Indices to evict, lowest rank first, then least near, until the rest
    /// fit the budget. Nothing within protect_distance is ever chosen, so the
    /// result may still leave the total over it.
    std::vector<size_t> chooseEvictions(const std::vector<EvictionCandidate>& candidates, size_t budget_bytes, F64 protect_distance);
}

#endif // SK_AIREVICT_H
