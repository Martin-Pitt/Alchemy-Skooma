/**
 * @file skdatavizstyle.h
 * @brief The pixel and colour tokens every legend and chart is drawn with.
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

#ifndef SK_DATAVIZSTYLE_H
#define SK_DATAVIZSTYLE_H

#include "lluicolortable.h"
#include "v4color.h"

namespace SKDataVizStyle
{
    inline constexpr S32 PAD        = 8;   // inset on every side
    inline constexpr S32 MIN_PAD    = 2;   // the inset of a collapsed legend
    inline constexpr S32 SWATCH     = 10;  // square marks
    inline constexpr S32 LINE_MARK  = 14;  // line marks, at their real width
    inline constexpr S32 MARK_GAP   = 6;   // mark to label
    inline constexpr S32 RAMP_W     = 160; // a ramp's widest bar
    inline constexpr S32 RAMP_H     = 8;
    inline constexpr S32 RING_D     = 48;  // a ring ramp's diameter
    inline constexpr S32 TICK       = 3;
    inline constexpr S32 CARET      = 5;
    inline constexpr S32 MIN_WIDTH  = 64;  // narrower than this says "too small"
    inline constexpr F32 BG_ALPHA   = 0.55f; // a transparent black under the content, no border
    inline constexpr F32 OTHER_DIM  = 0.25f; // what the unmatched classes fade to while one is hovered

    // The skin's colours, with the house values where the skin has none.
    inline LLColor4 colour(const char* name, const LLColor4& fallback)
    {
        return LLUIColorTable::instance().getColor(name, fallback).get();
    }
    inline LLColor4 bg() { return colour("SkDataVizBg", LLColor4(0.f, 0.f, 0.f, 1.f)); }
    inline LLColor4 text() { return colour("SkDataVizText", LLColor4(0.949f, 0.949f, 0.949f, 1.f)); }
    inline LLColor4 dim() { return colour("SkDataVizDim", LLColor4(0.627f, 0.627f, 0.659f, 1.f)); }
    inline LLColor4 unit() { return colour("SkDataVizUnit", LLColor4(0.471f, 0.471f, 0.502f, 1.f)); }
    inline LLColor4 selected() { return colour("SkDataVizSelected", LLColor4(1.f, 1.f, 1.f, 1.f)); }
    inline LLColor4 threshold() { return colour("SkDataVizThreshold", LLColor4(0.541f, 0.565f, 0.592f, 1.f)); }
    inline LLColor4 reference() { return colour("SkDataVizReference", LLColor4(0.361f, 0.549f, 1.f, 1.f)); }
}

#endif // SK_DATAVIZSTYLE_H
