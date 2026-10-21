/**
 * @file sknavfloors.cpp
 * @brief Floors under a footprint or a point, from land samples and air gaps.
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

#include "sknavfloors.h"

#include <algorithm>

namespace sknav
{
    F32 footprintMax(const LLVector3d& min, const LLVector3d& max, const std::function<F32(const LLVector3d&)>& height)
    {
        const F64 z = min.mdV[VZ];
        const LLVector3d points[] = {
            LLVector3d(min.mdV[VX], min.mdV[VY], z),
            LLVector3d(max.mdV[VX], min.mdV[VY], z),
            LLVector3d(min.mdV[VX], max.mdV[VY], z),
            LLVector3d(max.mdV[VX], max.mdV[VY], z),
            LLVector3d((min.mdV[VX] + max.mdV[VX]) * 0.5, (min.mdV[VY] + max.mdV[VY]) * 0.5, z),
        };
        F32 best = height(points[0]);
        for (size_t i = 1; i < std::size(points); ++i)
        {
            best = std::max(best, height(points[i]));
        }
        return best;
    }

    bool pickFloor(const std::vector<Enclosure>& gaps, F32 lo, F32 hi, F32& floor_out)
    {
        // Rank: 2 an AVATAR floor, 1 any other fine gap, 0 coarse.
        S32 best_rank = -1;
        F32 best = 0.f;
        for (const Enclosure& g : gaps)
        {
            if (g.mFloor < lo || g.mFloor > hi)
            {
                continue;
            }
            const S32 rank = g.mAvatarFloor ? 2 : g.mFine ? 1 : 0;
            if (rank > best_rank || (rank == best_rank && g.mFloor > best))
            {
                best_rank = rank;
                best = g.mFloor;
            }
        }
        if (best_rank < 0)
        {
            return false;
        }
        floor_out = best;
        return true;
    }
}
