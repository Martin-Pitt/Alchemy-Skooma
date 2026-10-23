/**
 * @file skairstats.h
 * @brief What a tile build measures of itself when asked: stage times, sizes and spans.
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

#ifndef SK_AIRSTATS_H
#define SK_AIRSTATS_H

#include "stdtypes.h"
#include "skairfaceoffsets.h"

#include <chrono>
#include <vector>

namespace skworld
{
    /// Laps of a steady clock added into a table of stage times; idle without a table.
    class StageClock
    {
    public:
        explicit StageClock(F64* ms) : mMs(ms)
        {
            if (mMs)
            {
                mLast = std::chrono::steady_clock::now();
            }
        }
        void lap(S32 stage)
        {
            if (mMs)
            {
                const auto now = std::chrono::steady_clock::now();
                mMs[stage] += std::chrono::duration<F64, std::milli>(now - mLast).count();
                mLast = now;
            }
        }
        void restart()
        {
            if (mMs)
            {
                mLast = std::chrono::steady_clock::now();
            }
        }

    private:
        F64* mMs;
        std::chrono::steady_clock::time_point mLast;
    };

    /// The air passes of one tile, in the order buildTileAir runs them.
    struct AirBuildStats
    {
        enum Stage : S32 { EXTRACT, FOLD, CLEARANCE, CROP, DISTIL, SEAMS, TOPS, NUM_STAGES };
        F64 mMs[NUM_STAGES] = {};
        size_t mGridBytes = 0;      ///< the cropped fine grid
        size_t mStripBytes = 0;     ///< the four seam strips
        U32 mGridGaps = 0;

        static const char* stageName(S32 stage);
    };

    /// One view of the solid raster (AVATAR's, or the air's TOP surfaces), in pipeline order.
    struct ClassBuildStats
    {
        enum Stage : S32 { SOUP, RASTERISE, FILL, CUT, BRIDGES, CULL, FILTERS, WALLS, AIR, EDGES, NUM_STAGES };
        bool mBuilt = false;
        F64 mMs[NUM_STAGES] = {};
        U32 mHfSpans = 0;
        size_t mHfBytes = 0;        ///< the column heads and every span pool
        U32 mHfColumns = 0;         ///< columns holding at least one span
        U32 mHfMaxSpans = 0;        ///< the most spans one column holds
        U32 mBridges = 0;
        U32 mCodes = 0;             ///< distinct area codes rasterised
        /// Normalised digests of the class's view after its walk rules.
        U64 mHfHash = 0;            ///< spans and whether each is walkable
        U64 mHfAreaHash = 0;        ///< spans and the areas the class walks
        AirBuildStats mAir;

        static const char* stageName(S32 stage);
    };

    struct TileBuildStats
    {
        F64 mSoupMs = 0.0;
        F64 mExcludeMs = 0.0;
        F64 mTotalMs = 0.0;
        F64 mRasterMs = 0.0;            ///< soup to the air products
        F64 mCoarseAirMs = 0.0;         ///< of the raster stage, the coarse air of a tile without its own
        /// The air's face offsets, and every mFaceTraceEvery-th side traced for an oracle when set.
        FaceOffsetStats mFaceOffsets;
        S32 mFaceTraceEvery = 0;
        std::vector<FaceOffsetTrace> mFaceTrace;
        U32 mExcludedRecords = 0;
        U32 mExcludeWindowCells = 0;    ///< tile cells within two of an excluded triangle's box
        U32 mTileCells = 0;
        U32 mOwnColumns = 0;            ///< columns AVATAR reads from the window raster
        U32 mWindowCells = 0;           ///< raster cells round the records AVATAR leaves out
        S32 mWindowRect[4] = { 0, 0, 0, 0 };   ///< the window raster's cells, from the tile's corner: x0, z0, x1, z1
        ClassBuildStats mClass[1];
        /// The raster's own passes: the air's filters, the air, ledges and run-off.
        ClassBuildStats mTop;
    };
}

#endif // SK_AIRSTATS_H
