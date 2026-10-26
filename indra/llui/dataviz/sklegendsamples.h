/**
 * @file sklegendsamples.h
 * @brief Hand-built legends that exercise every part of the model, for the demo floater and the tests.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_LEGENDSAMPLES_H
#define SK_LEGENDSAMPLES_H

#include "sklegendmodel.h"

#include <memory>

// Specs only: the real debug views take their legends from their own drawers' scales.
namespace SKLegendSamples
{
    // A scale whose domain the caller moves at runtime, as a setting would.
    std::shared_ptr<SKColorScale> avatarComplexityScale();

    SKLegend avatarDrawInfo(const std::shared_ptr<const SKColorScale>& complexity);
    SKLegend updateType();
    SKLegend lodInfo();
    SKLegend bufferView();
    SKLegend nameTagDistance();
    SKLegend beacons();
    // Every built-in scale, one section each.
    SKLegend builtInScales();
}

#endif // SK_LEGENDSAMPLES_H
