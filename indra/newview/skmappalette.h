/**
 * @file skmappalette.h
 * @brief Every colour, width and alpha the mini-map draws with, in one place.
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

#ifndef SK_MAPPALETTE_H
#define SK_MAPPALETTE_H

#include "stdtypes.h"

namespace skmap
{
    /// A straight-alpha colour, constexpr so the palette is data rather than code.
    struct PaletteColour
    {
        F32 mR = 0.f;
        F32 mG = 0.f;
        F32 mB = 0.f;
        F32 mA = 1.f;
    };

    /// 0xRRGGBBAA.
    constexpr PaletteColour rgba8(U32 v)
    {
        return { static_cast<F32>((v >> 24) & 0xff) / 255.f, static_cast<F32>((v >> 16) & 0xff) / 255.f,
                 static_cast<F32>((v >> 8) & 0xff) / 255.f, static_cast<F32>(v & 0xff) / 255.f };
    }

    constexpr PaletteColour withAlpha(PaletteColour c, F32 a)
    {
        return { c.mR, c.mG, c.mB, a };
    }

    // These are tuning starting points; every one of them lives here.
    namespace palette
    {
        // People.
        constexpr PaletteColour DOT_GENERAL = rgba8(0x60df72ff);
        constexpr PaletteColour DOT_COMBAT = rgba8(0x65ccdaff);
        constexpr PaletteColour CODE_INK = rgba8(0x101014ff);
        constexpr PaletteColour SHADOW = rgba8(0x000000cc);
        constexpr F32 SHADOW_BLUR_PX = 4.f;
        constexpr S32 SHADOW_SPRITE_PX = 32;
        constexpr PaletteColour SELF_DOT = rgba8(0xffffffff);
        constexpr F32 SELF_DOT_RADIUS_PX = 2.6f;
        constexpr PaletteColour SELF_CONE = rgba8(0xffffff4d);
        constexpr F32 SELF_CONE_PX = 56.f;
        constexpr F32 DOT_RADIUS_PX = 3.f;
        constexpr F32 HEADING_TIP_PX = 6.f;         ///< the heading triangle's tip beyond the dot
        constexpr F32 HEADING_BASE_PX = 1.5f;       ///< its base beyond the dot
        constexpr F32 HEADING_HALF_PX = 2.5f;
        constexpr F32 CHEVRON_STROKE_PX = 1.4f;

        // Vehicles.
        constexpr PaletteColour VEH_CIVIL_HULL = rgba8(0x4f9a5aff);
        constexpr PaletteColour VEH_CIVIL_TOP = rgba8(0xbfe8c5ff);
        constexpr PaletteColour VEH_ALLY_HULL = rgba8(0x4f9fabff);
        constexpr PaletteColour VEH_ALLY_TOP = rgba8(0xbdf0f5ff);
        constexpr PaletteColour VEH_NEUTRAL_HULL = rgba8(0x8d959cff);
        constexpr PaletteColour VEH_NEUTRAL_TOP = rgba8(0xd5d9dcff);

        // Chrome and tags.
        constexpr PaletteColour CHROME_TINT = rgba8(0x1010143d);
        constexpr PaletteColour CHROME_BORDER = rgba8(0xc5cdd37a);
        constexpr F32 CHROME_BORDER_PX = 1.f;
        constexpr PaletteColour GRIP = rgba8(0xced5df99);
        constexpr F32 GRIP_PX = 12.f;
        constexpr F32 GRIP_PITCH_PX = 4.f;
        constexpr PaletteColour TAG_BG = rgba8(0x000000b3);
        constexpr PaletteColour TAG_TEXT = rgba8(0xffffffff);
        constexpr PaletteColour TAG_DETAIL = rgba8(0xc5e1e5ff);
        constexpr PaletteColour TAG_CODE = rgba8(0xa6efafff);
        constexpr PaletteColour CONNECTOR = rgba8(0xb9efc066);
        constexpr F32 TAG_PAD_X_PX = 6.f;
        constexpr F32 TAG_PAD_Y_PX = 3.f;
        constexpr F32 TAG_CODE_GAP_PX = 5.f;
        constexpr PaletteColour NORTH = rgba8(0xffffffff);

        // Floors.
        constexpr PaletteColour MAT_OBJECT = { 0.62f, 0.64f, 0.66f, 1.f };
        constexpr PaletteColour MAT_TERRAIN = { 0.48f, 0.60f, 0.45f, 1.f };
        constexpr PaletteColour WATER_SHALLOW = { 0.55f, 0.72f, 0.82f, 1.f };
        /// Dark enough that the denser deep wash still reads darker than the shallows over a dark world.
        constexpr PaletteColour WATER_DEEP = { 0.12f, 0.20f, 0.32f, 1.f };
        constexpr F32 WATER_DEPTH_M = 6.f;
        constexpr F32 WATER_WASH_SHALLOW = 0.15f;
        constexpr F32 WATER_WASH_DEEP = 0.45f;
        constexpr PaletteColour HEAT_NEUTRAL = { 0.60f, 0.60f, 0.60f, 1.f };
        constexpr PaletteColour HEAT_BELOW = { 0.45f, 0.62f, 1.0f, 1.f };
        constexpr PaletteColour HEAT_ABOVE = { 0.85f, 0.45f, 0.42f, 1.f };
        constexpr F32 BACKING_ALPHA = 0.72f;
        constexpr F32 WASH_ALPHA = 0.20f;
        constexpr F32 BAKE_GAIN = 1.33f;
        constexpr F32 SHADE_INDOORS = 0.85f;
        constexpr F32 SHADE_SHELTERED = 0.92f;
        constexpr F32 RAMP_GRADIENT_PER_M = 0.04f;
        constexpr F32 RAMP_GRADIENT_MIN = 0.7f;
        constexpr F32 RAMP_GRADIENT_MAX = 1.3f;

        // Baked surface marks.
        constexpr F32 LEDGE_HALO_ALPHA = 0.18f;
        constexpr F32 LEDGE_HALO_TEXELS = 12.f;
        constexpr F32 LEDGE_HALO_MAX_M = 2.f;
        constexpr F32 WALL_HALO_ALPHA = 0.22f;
        constexpr F32 WALL_HALO_TEXELS = 8.f;
        constexpr F32 WALL_HALO_MAX_M = 1.5f;
        constexpr PaletteColour CONTOUR = { 0.85f, 0.88f, 0.80f, 0.30f };
        constexpr PaletteColour RUNG_CASING = withAlpha(rgba8(0x202428ff), 0.75f);
        constexpr PaletteColour RUNG_CORE = withAlpha(rgba8(0xe8ecefff), 0.75f);
        constexpr F32 RUNG_CASING_TEXELS = 3.f;
        constexpr F32 RUNG_CORE_TEXELS = 1.f;
        constexpr F32 RUNG_MIN_PX = 6.f;
        constexpr PaletteColour CHEVRON = withAlpha(rgba8(0xe8ecefff), 0.5f);
        constexpr F32 CHEVRON_SPACING_TEXELS = 40.f;
        constexpr F32 CHEVRON_HALF_WIDTH = 0.35f;   ///< of the run's width

        // Line marks, drawn as ribbons.
        constexpr PaletteColour COVER_FULL = withAlpha(rgba8(0xe6e9ecff), 0.8f);
        constexpr F32 COVER_FULL_PX = 3.f;
        constexpr PaletteColour COVER_HALF = withAlpha(rgba8(0xc9cfd5ff), 0.8f);
        constexpr F32 COVER_HALF_PX = 2.f;
        constexpr PaletteColour COVER_LOW = withAlpha(rgba8(0x8a9097ff), 0.5f);
        constexpr F32 COVER_LOW_PX = 1.f;
        constexpr F32 HALF_GLYPH_PX = 24.f;         ///< half-shield spacing
        constexpr F32 HALF_GLYPH_SIZE_PX = 5.f;
        constexpr F32 HALF_GLYPH_MAX_MPP = 0.1f;
        constexpr PaletteColour LEDGE = withAlpha(rgba8(0xd0d4d8ff), 0.7f);
        constexpr F32 LEDGE_PX = 1.f;
        constexpr F32 LEDGE_TICK_SPACING_PX = 12.f;
        constexpr F32 LEDGE_TICK_PX = 3.f;
        constexpr F32 LEDGE_TICK_MAX_MPP = 0.15f;
        constexpr PaletteColour DROP_EDGE = withAlpha(rgba8(0xf0b44cff), 0.85f);
        constexpr F32 DROP_EDGE_PX = 2.f;
        constexpr F32 DROP_POINT_PX = 6.f;
        constexpr PaletteColour OVERHANG = withAlpha(rgba8(0x9aa1a8ff), 0.6f);
        constexpr F32 OVERHANG_PX = 1.f;
        constexpr F32 OVERHANG_ON_PX = 2.f;
        constexpr F32 OVERHANG_OFF_PX = 3.f;
        constexpr PaletteColour DOOR_JAMB = rgba8(0xe6e9ecff);
        constexpr F32 DOOR_JAMB_PX = 1.5f;
        constexpr PaletteColour DOOR_LEAF = rgba8(0xb0b6bcff);
        constexpr F32 DOOR_LEAF_PX = 1.f;
        constexpr PaletteColour FEATURE_GLYPH = withAlpha(rgba8(0xe6e9ecff), 0.85f);
        constexpr F32 FEATURE_GLYPH_PX = 8.f;
        constexpr PaletteColour VOID_BOUNDARY = withAlpha(rgba8(0xdfe3e6ff), 0.8f);
        constexpr F32 VOID_BOUNDARY_PX = 3.f;
        constexpr PaletteColour REGION_BOUNDARY = withAlpha(rgba8(0xdfe3e6ff), 0.6f);
        constexpr F32 REGION_BOUNDARY_PX = 2.5f;
        constexpr F32 REGION_DASH_ON_PX = 6.f;
        constexpr F32 REGION_DASH_OFF_PX = 4.f;
        constexpr F32 RIBBON_MIN_RUN_M = 2.f;
        constexpr F32 RIBBON_MIN_RUN_PX = 3.f;
        constexpr PaletteColour HATCH = rgba8(0xffffff1f);
        constexpr S32 HATCH_PITCH_PX = 8;

        // Overlays drawn live.
        constexpr F32 TERRAIN_ALPHA = 0.6f;
        constexpr F32 TERRAIN_GREY = 0.75f;
        constexpr F32 DIM_NEAR = 0.3f;              ///< of the span, where the distance dim starts
        constexpr F32 DIM_FAR = 0.75f;
        constexpr F32 DIM_AMOUNT = 0.55f;
        constexpr PaletteColour COVER_AHEAD_FULL = COVER_FULL;
        constexpr PaletteColour COVER_AHEAD_HALF = COVER_HALF;
        constexpr F32 COVER_AHEAD_PX = 6.f;
        constexpr F32 COVER_AHEAD_OFFSET_PX = 10.f;
        constexpr F32 FIRING_ALPHA_NEAR = 0.20f;
        constexpr F32 FIRING_ALPHA_FAR = 0.05f;
        constexpr F32 FIRING_REACH_M = 64.f;
        constexpr F32 PING_FROM_PX = 4.f;
        constexpr F32 PING_TO_PX = 14.f;
        constexpr F32 PING_SECONDS = 4.f;
        constexpr F32 EVENT_ICON_PX = 5.f;
        constexpr F32 EVENT_ICON_FADE_S = 0.2f;
        constexpr PaletteColour EVENT_LINE = rgba8(0xffffffcc);
        constexpr PaletteColour EVENT_ICON = rgba8(0xff5a50ff);
        constexpr PaletteColour PROJECTILE = rgba8(0xffe9a8ff);
        constexpr PaletteColour DEAD_REGION = rgba8(0xff000040);
    }
}

#endif // SK_MAPPALETTE_H
