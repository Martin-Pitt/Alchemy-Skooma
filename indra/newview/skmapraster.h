/**
 * @file skmapraster.h
 * @brief One mini-map layer's floorplan rasterised into a 256 x 256 cell, on a worker.
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

#ifndef SK_MAPRASTER_H
#define SK_MAPRASTER_H

#include "skmapgeom.h"

#include <vector>

namespace skmap
{
    /// Bumped whenever the raster draws differently, so old bakes are never reused.
    constexpr U32 RASTER_VERSION = 1;

    /// The settings a bake reads; each is part of the bake key.
    struct RasterParams
    {
        bool mStairs = true;
        bool mContours = true;
        bool mHeatmap = false;
    };

    /// What a raster drew, for tests and the debug readout.
    struct RasterStats
    {
        S32 mPolys = 0;
        S32 mRungs = 0;
        S32 mChevrons = 0;
        std::vector<S32> mContourLevels;    ///< sorted, unique
    };

    /// The contour spacing for a texel size: about 16 texels (8 px), never under 0.5 m, from
    /// the nice values 0.5, 1, 2, 5, 10 and 20 m.
    F32 contourStep(F32 texel);

    /// Rasterise one layer's cell to 256 x 256 straight-alpha RGBA8, row 0 at the south edge.
    /// Pure and deterministic: the same scene, layer, cell and parameters give the same bytes.
    void rasterCell(const Scene& scene, S32 layer, const CellId& cell, const RasterParams& params, std::vector<U8>& out,
                    RasterStats* stats = nullptr);
}

#endif // SK_MAPRASTER_H
