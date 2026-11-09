/**
 * @file skterrainpng.h
 * @brief Greyscale 16-bit PNG, the dataset's exchange format for a cell's heights.
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

#ifndef SK_SKTERRAINPNG_H
#define SK_SKTERRAINPNG_H

#include "skterraincell.h"

#include <optional>

// LLPngWrapper keeps 8 bits a channel; these keep all 16. PNG row 0 is the cell's south row.
namespace SKTerrainPng
{
    struct Grey16
    {
        U32              mWidth  = 0;
        U32              mHeight = 0;
        std::vector<U16> mPixels; // row-major as stored
    };

    std::optional<Grey16> decode(const U8* data, size_t size);
    std::vector<U8>       encode(const Grey16& image);

    // height_cm = offset_cm + value for values 1..65535; 0 is missing.
    std::optional<SKTerrainLayerSamples> toSamples(const Grey16& image, S32 offset_cm);

    // offset_cm is the lowest known height less one, as the dataset picks it. Fails on a layer with
    // nothing known or a range over 655.34 m, which the format cannot hold.
    std::optional<Grey16> fromSamples(const SKTerrainLayerSamples& samples, S32& offset_cm);
} // namespace SKTerrainPng

#endif // SK_SKTERRAINPNG_H
