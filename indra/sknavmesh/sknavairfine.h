/**
 * @file sknavairfine.h
 * @brief A tile's air at half its cell over the blocks where its narrow openings are, and how it differs from the tile's own.
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

#ifndef SK_NAVAIRFINE_H
#define SK_NAVAIRFINE_H

#include "sknavair.h"

#include <memory>
#include <vector>

namespace sknav
{
    constexpr S32 FINE_BLOCK_CELLS = 16;        ///< a candidate block, 2 m at 0.125 m
    constexpr S32 FINE_OPENING_CELLS = 2;       ///< an opening this wide or narrower makes its block a candidate
    constexpr F32 FINE_PAD_M = 0.5f;            ///< what the fine raster reads past its window

    /// One gap of a fine column, in the fine grid's height units from the tile air's origin z.
    struct FineGap
    {
        U16 mFloor = 0;
        U16 mCeiling = 0;
        U8 mFlags = 0;
        U8 mClear = 0;
    };

    /// A fine column with air: a level -1 leaf, in fine cells of the tile.
    struct FineLeaf
    {
        U16 mX = 0;
        U16 mY = 0;
        U8 mGapCount = 0;
        U8 mPad = 0;
        U32 mFirstGap = 0;
    };

    /// A rectangle of the tile's own cells, half-open.
    struct FineWindow
    {
        S32 mX0 = 0;
        S32 mY0 = 0;
        S32 mX1 = 0;
        S32 mY1 = 0;
    };

    /// The fine columns of a tile's candidate blocks; the tile's own air stays the truth outside them.
    struct FineAir
    {
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        std::vector<FineWindow> mWindows;   ///< what was rasterised, padded by FINE_PAD_M
        std::vector<FineLeaf> mLeaves;      ///< candidate-block columns with air, in airKey order
        std::vector<FineGap> mGaps;

        size_t bytes() const;
    };
    typedef std::shared_ptr<const FineAir> FineAirPtr;

    /// Per FINE_BLOCK_CELLS block of the grid, row-major along y, 1 where a gap
    /// at least overlap tall has walls at most FINE_OPENING_CELLS apart along x or y.
    std::vector<U8> fineCandidates(const AirGrid& grid, S32 climb, S32 overlap);

    /// Disjoint rectangles covering the candidate blocks: each 8-connected group's bounds, overlapping bounds merged.
    std::vector<FineWindow> fineWindows(const std::vector<U8>& blocks, S32 grid_width, S32 grid_height);

    /// The columns of a window's fine grid that lie in candidate blocks, as
    /// leaves; fine_x0 and fine_y0 place the grid in the tile's fine cells.
    void appendFineLeaves(const AirGrid& fine, S32 fine_x0, S32 fine_y0, const std::vector<U8>& blocks, S32 grid_width, FineAir& out);

    /// Leaves into airKey order, their gaps gathered after them.
    void sortFineLeaves(FineAir& air);

    /// How a window's fine air connects across the coarse grid's cell edges.
    /// A coarse link closes when no fine pair over that edge joins the two
    /// gaps; it separates when the window's fine air does not join them at
    /// all. Openings are the reverse, from fine links the coarse air lacks.
    struct FineCompare
    {
        U32 mCoarseLinks = 0;
        U32 mClosed = 0;
        U32 mClosedSeparating = 0;
        U32 mFineLinks = 0;
        U32 mOpened = 0;
        U32 mOpenedSeparating = 0;
        U32 mFineGaps = 0;
        U32 mNewGaps = 0;           ///< fine gaps no coarse gap of their column holds
        U32 mNewWalkable = 0;       ///< of those, the walkable ones
    };

    /// Compare the window [x0, x1) x [y0, y1) of the coarse grid with its fine
    /// grid, two cells per coarse cell and two units per coarse unit, over the
    /// edges that touch a candidate block.
    void compareFineAir(const AirGrid& coarse, const AirGrid& fine, const FineWindow& window, const std::vector<U8>& blocks, S32 climb,
                        S32 overlap, S32 fine_climb, S32 fine_overlap, FineCompare& out);
}

#endif // SK_NAVAIRFINE_H
