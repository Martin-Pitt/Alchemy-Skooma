/**
 * @file skcensusland.h
 * @brief The land a census footprint stands on.
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


#ifndef SK_CENSUSLAND_H
#define SK_CENSUSLAND_H

#include "v3dmath.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace skworld
{
    /// The highest of height over a box's four plan corners and its middle,
    /// at the box's foot: the land a footprint stands on, so a slope does not
    /// hide under its low side.
    inline F32 footprintMax(const LLVector3d& min, const LLVector3d& max, const std::function<F32(const LLVector3d&)>& height)
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
}

#endif // SK_CENSUSLAND_H
