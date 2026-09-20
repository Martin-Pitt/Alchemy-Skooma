/**
 * @file sknavpalette.cpp
 * @brief One colour per thing the navmesh overlay draws, named so a legend cannot drift from it.
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

#include "llviewerprecompiledheaders.h"

#include "sknavpalette.h"

namespace
{
    struct Swatch
    {
        LLColor4 mColor;
        std::string mName;
    };

    // Ordered as SKNavSwatch is. Alpha is the strength the thing is drawn at
    // when it is the only pass; the overlay scales it per pass from there.
    const Swatch PALETTE[static_cast<S32>(SKNavSwatch::COUNT)] = {
        { LLColor4(0.30f, 0.90f, 0.30f, 0.85f), "sk_nav_ground" },
        { LLColor4(0.30f, 0.60f, 1.00f, 0.85f), "sk_nav_object" },
        { LLColor4(0.20f, 0.40f, 0.90f, 0.55f), "sk_nav_submerged" },
        { LLColor4(0.70f, 0.70f, 0.70f, 0.85f), "sk_nav_level" },
        { LLColor4(1.00f, 0.90f, 0.30f, 0.90f), "sk_nav_connector" },
        { LLColor4(0.15f, 0.85f, 0.95f, 0.70f), "sk_nav_tile_joined" },
        { LLColor4(1.00f, 0.15f, 0.90f, 0.95f), "sk_nav_tile_open" },
        { LLColor4(1.00f, 1.00f, 1.00f, 0.90f), "sk_nav_cover_full" },
        { LLColor4(1.00f, 0.80f, 0.20f, 0.90f), "sk_nav_cover_half" },
        { LLColor4(0.60f, 0.60f, 0.60f, 0.70f), "sk_nav_cover_low" },
        { LLColor4(1.00f, 0.30f, 0.30f, 0.90f), "sk_nav_ledge" },
        { LLColor4(0.70f, 0.30f, 1.00f, 0.80f), "sk_nav_overhang" },
        { LLColor4(0.20f, 1.00f, 1.00f, 0.90f), "sk_nav_drop" },
        { LLColor4(0.20f, 1.00f, 0.20f, 0.60f), "sk_nav_shape_exact" },
        { LLColor4(0.20f, 0.60f, 1.00f, 0.60f), "sk_nav_shape_hull" },
        { LLColor4(1.00f, 0.20f, 0.20f, 0.80f), "sk_nav_shape_box" },
        { LLColor4(1.00f, 0.60f, 0.20f, 0.60f), "sk_nav_shape_render" },
        { LLColor4(1.00f, 1.00f, 0.20f, 0.80f), "sk_nav_shape_dynamic" },
        { LLColor4(0.70f, 0.70f, 0.70f, 0.90f), "sk_nav_cluster" },
    };
}

const LLColor4& skNavSwatchColor(SKNavSwatch swatch)
{
    return PALETTE[static_cast<S32>(swatch)].mColor;
}

const std::string& skNavSwatchName(SKNavSwatch swatch)
{
    return PALETTE[static_cast<S32>(swatch)].mName;
}
