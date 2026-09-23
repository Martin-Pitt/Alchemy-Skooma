/**
 * @file sknavpalette.cpp
 * @brief One colour per thing the navmesh overlay draws.
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
    // Ordered as SKNavSwatch is. Alpha is the strength the thing is drawn at
    // when it is the only pass; the overlay scales it per pass from there.
    const LLColor4 PALETTE[static_cast<S32>(SKNavSwatch::COUNT)] = {
        LLColor4(0.30f, 0.90f, 0.30f, 0.85f),
        LLColor4(0.30f, 0.60f, 1.00f, 0.85f),
        LLColor4(0.20f, 0.40f, 0.90f, 0.55f),
        LLColor4(0.70f, 0.70f, 0.70f, 0.85f),
        LLColor4(0.15f, 0.85f, 0.95f, 0.70f),
        LLColor4(1.00f, 0.15f, 0.90f, 0.95f),
        LLColor4(1.00f, 1.00f, 1.00f, 0.90f),
        LLColor4(1.00f, 0.80f, 0.20f, 0.90f),
        LLColor4(0.60f, 0.60f, 0.60f, 0.70f),
        LLColor4(0.55f, 0.75f, 0.95f, 0.45f),
        LLColor4(1.00f, 0.30f, 0.30f, 0.90f),
        LLColor4(0.70f, 0.30f, 1.00f, 0.80f),
        LLColor4(0.20f, 1.00f, 1.00f, 0.90f),
        LLColor4(0.20f, 1.00f, 0.20f, 0.60f),
        LLColor4(0.20f, 0.60f, 1.00f, 0.60f),
        LLColor4(1.00f, 0.20f, 0.20f, 0.80f),
        LLColor4(1.00f, 0.60f, 0.20f, 0.60f),
        LLColor4(1.00f, 1.00f, 0.20f, 0.80f),
        LLColor4(0.70f, 0.70f, 0.70f, 0.90f),
    };
}

const LLColor4& skNavSwatchColor(SKNavSwatch swatch)
{
    return PALETTE[static_cast<S32>(swatch)];
}
