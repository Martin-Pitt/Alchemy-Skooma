/**
 * @file sknavbuildstats.h
 * @brief What a tile build measures of itself when asked: stage times, sizes and the spans behind its polygons.
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

#ifndef SK_NAVBUILDSTATS_H
#define SK_NAVBUILDSTATS_H

#include "stdtypes.h"

#include <chrono>
#include <vector>

namespace sknav
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
        enum Stage : S32 { EXTRACT, FOLD, FLIGHTS, CLEARANCE, CROP, DISTIL, SEAMS, TOPS, NUM_STAGES };
        F64 mMs[NUM_STAGES] = {};
        size_t mGridBytes = 0;      ///< the cropped fine grid
        size_t mStripBytes = 0;     ///< the four seam strips
        U32 mGridGaps = 0;

        static const char* stageName(S32 stage);
    };

    /// One class's build, in pipeline order.
    struct ClassBuildStats
    {
        enum Stage : S32
        {
            PARTITION, RASTERISE, FILL, CUT, BRIDGES, CULL, FILTERS, COMPACT, ERODE, REGIONS, CONTOURS, POLYMESH, STAMP, FLIGHTMAP,
            DETAIL, WALLS, AIR, EDGES, DETOUR, NUM_STAGES
        };
        bool mBuilt = false;
        F64 mMs[NUM_STAGES] = {};
        U32 mHfSpans = 0;
        size_t mHfBytes = 0;        ///< the column heads and every span pool
        U32 mChfSpans = 0;
        size_t mChfBytes = 0;       ///< cells, spans, areas and the distance field
        U32 mWalkableSpans = 0;     ///< after erosion
        U32 mBridges = 0;
        U32 mPolys = 0;
        U32 mVerts = 0;
        U32 mDetailTris = 0;
        U32 mDetourBytes = 0;
        U32 mFlights = 0;
        U32 mFlightPolys = 0;
        U32 mFlightCodes = 0;
        U32 mCodes = 0;             ///< distinct area codes rasterised
        /// Normalised digests of the heightfield after its filters and of the compact one before regions.
        U64 mHfHash = 0;            ///< spans and whether each is walkable
        U64 mHfAreaHash = 0;        ///< spans and their area codes as the class reads them
        U64 mChfHash = 0;
        AirBuildStats mAir;

        static const char* stageName(S32 stage);
    };

    /// An AVATAR walkable span after erosion, in the tile's own cells.
    struct StatSpan
    {
        U16 mX = 0;                 ///< along SL x
        U16 mY = 0;                 ///< along SL y
        U16 mTop = 0;               ///< in cells from the heightfield's base
        U16 mRegion = 0;
        U8 mCode = 0;               ///< the slope partition's code, before it is unfolded
        U32 mGap = U32_MAX;         ///< its gap in the raster's span map, U32_MAX for none
    };

    struct TileBuildStats
    {
        F64 mSoupMs = 0.0;
        F64 mExcludeMs = 0.0;
        F64 mTotalMs = 0.0;
        F64 mRasterMs = 0.0;            ///< the raster stage, soup to the held raster
        F64 mPolyMs = 0.0;              ///< the polygon stage
        F64 mCoarseAirMs = 0.0;         ///< of the raster stage, the coarse air of a tile without its own
        /// What the raster stage hands the polygon stage: all of it, its compact heightfield, its edge field.
        size_t mHeldBytes = 0;
        size_t mHeldChfBytes = 0;
        size_t mHeldEdgeBytes = 0;
        size_t mHeldSpanGapBytes = 0;
        /// AVATAR's walkable spans on the tile proper, those with no gap of the
        /// air in mSpanGap, and of those, the ones under door leaves and in
        /// columns AVATAR reads from a copy of its own.
        U32 mSpanGapSpans = 0;
        U32 mSpanGapMisses = 0;
        U32 mSpanGapWindowMisses = 0;
        U32 mSpanGapOwnMisses = 0;
        U32 mExcludedRecords = 0;
        U32 mExcludeWindowCells = 0;    ///< tile cells within two of an excluded triangle's box
        U32 mTileCells = 0;
        U32 mSharedCodes = 0;           ///< distinct codes of the shared partition, at the widest slope
        /// Bridges the air's list lays that AVATAR's, found on the same raster, does not, and the reverse.
        U32 mTopOnlyBridges = 0;
        U32 mAvatarOnlyBridges = 0;
        U32 mOwnColumns = 0;            ///< columns AVATAR reads from a copy of its own
        U32 mWindowCells = 0;           ///< raster cells round the records AVATAR leaves out
        S32 mWindowRect[4] = { 0, 0, 0, 0 };   ///< the window raster's cells, from the tile's corner: x0, z0, x1, z1
        ClassBuildStats mClass[1];
        /// The raster's own passes: the air's filters, the air, ledges and run-off.
        ClassBuildStats mTop;
        /// AVATAR's walkable spans on the tile proper and, per poly, which of them it covers.
        std::vector<StatSpan> mSpans;
        std::vector<U32> mPolySpanStart;
        std::vector<U32> mPolySpans;
        std::vector<F32> mPolyArea;     ///< m², from its outline
        std::vector<U8> mPolyFlight;    ///< 1 where a flight claimed the poly
        /// Flights the air marks that got no polys, and the reverse, by key.
        std::vector<U64> mFlightStandOnly;
        std::vector<U64> mFlightPolysOnly;
    };
}

#endif // SK_NAVBUILDSTATS_H
