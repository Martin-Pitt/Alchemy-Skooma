/**
 * @file sknavfloors.h
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

#ifndef SK_SKNAVFLOORS_H
#define SK_SKNAVFLOORS_H

#include "sknavenclosure.h"

#include "v3dmath.h"

#include <functional>
#include <vector>

namespace sknav
{
    /// The highest of height over a box's four plan corners and its middle,
    /// at the box's foot: the land a footprint stands on, so a slope does not
    /// hide under its low side.
    F32 footprintMax(const LLVector3d& min, const LLVector3d& max, const std::function<F32(const LLVector3d&)>& height);

    /// The built floor among gaps whose floor lies in [lo, hi]: the highest
    /// one AVATAR walks, else the highest fine one, else the highest coarse
    /// one. The bucket is ignored, since a floor is geometry and UNKNOWN only
    /// speaks to open or indoor. False when no gap's floor is in the band.
    bool pickFloor(const std::vector<Enclosure>& gaps, F32 lo, F32 hi, F32& floor_out);
}

#endif // SK_SKNAVFLOORS_H
