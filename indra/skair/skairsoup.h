/**
 * @file skairsoup.h
 * @brief One tile's triangle soup in SL axes: what its raster is made of, with an area code and flags per triangle.
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

#ifndef SK_AIRSOUP_H
#define SK_AIRSOUP_H

#include "skairraster.h"

#include <utility>
#include <vector>

namespace skworld
{
    struct TileBuildStats;

    /// Per triangle of a soup: AVATAR leaves it out (a door leaf the air keeps), and its NameCue.
    constexpr U8 SOUP_LEAF = 1 << 0;
    constexpr U8 SOUP_CUE_SHIFT = 1;
    constexpr U8 SOUP_CUE_MASK = 3 << SOUP_CUE_SHIFT;

    /// What a tile's raster is made of, mesh-local SL metres. Triangles are three vertices each, in record
    /// order and the land last; heights are snapped to a millimetre grid, so a flat surface clipped by two
    /// tiles gives the same height in both. Records every class leaves out are not in it.
    struct AirSoup
    {
        std::vector<F32> mVerts;                        ///< nine per triangle
        std::vector<U8> mCodes;                         ///< per triangle, its area code (skairraster.h)
        std::vector<U8> mFlags;                         ///< per triangle, SOUP_LEAF and the cue
        std::vector<std::pair<S32, S32>> mSolids;       ///< closed bodies' triangle ranges, filled by their crossings
        std::vector<F32> mCutVerts;                     ///< exclusion volumes, nine per triangle
        std::vector<std::pair<S32, S32>> mCuts;         ///< their ranges
        bool mLeaves = false;                           ///< any triangle carries SOUP_LEAF

        S32 ntris() const { return static_cast<S32>(mCodes.size()); }
        S32 ncuts() const { return static_cast<S32>(mCutVerts.size() / 9); }
        const F32* tri(S32 t) const { return &mVerts[static_cast<size_t>(t) * 9]; }
        U8 cue(S32 t) const { return static_cast<U8>((mFlags[t] & SOUP_CUE_MASK) >> SOUP_CUE_SHIFT); }
    };

    /// The real area an area code stands for: 1-3 for themselves, AVATAR's moved back down, else none.
    inline U8 areaOfCode(U8 code)
    {
        return code <= AREA_TOP_ONLY ? code : (code - AREA_TOP_ONLY <= AREA_SUBMERGED ? static_cast<U8>(code - AREA_TOP_ONLY) : AREA_NONE);
    }

    /// The soup of a tile: every record that reaches the tile padded by its widest pass, the cuts, the land.
    /// False, with the result EMPTY, when it holds no triangle.
    bool buildAirSoup(const TileInput& input, AirSoup& out, TileAirResult& result, TileBuildStats* stats = nullptr);
}

#endif // SK_AIRSOUP_H
