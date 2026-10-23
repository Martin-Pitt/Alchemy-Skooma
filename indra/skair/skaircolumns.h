/**
 * @file skaircolumns.h
 * @brief The air's solid raster of its own: a tile's soup clipped into flat, column-contiguous spans.
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

#ifndef SK_AIRCOLUMNS_H
#define SK_AIRCOLUMNS_H

#include "stdtypes.h"
#include "v3math.h"

#include <vector>

namespace skworld
{
    struct AirSoup;
    struct TerrainWindow;

    /// The largest height a span reaches, in units: thirteen bits.
    constexpr U16 COLUMN_HEIGHT_MAX = 8191;

    /// Per span of a ColumnRaster, what walks it and what its top was named.
    enum ColumnWalk : U8
    {
        TOP_WALK = 1 << 0,          ///< the air stands on it: at its slope, with its headroom, off no rim
        TOP_PRE_LEDGE = 1 << 1,     ///< TOP_WALK before the rim rule: where weather runs off from
        AVATAR_WALK = 1 << 2,       ///< AVATAR stands on it, in its own view of the raster
        WALK_CUE_STAIR = 1 << 3,    ///< the surface that set its top was named a stair
        WALK_CUE_RAMP = 1 << 4,     ///< or a ramp
    };

    /// Where a raster lies and how fine it is: a square of columns, mesh-local SL.
    struct ColumnFrame
    {
        LLVector3 mOrigin;          ///< the grid's corner; z is height unit 0
        F32 mCell = 0.f;            ///< metres per column
        F32 mUnit = 0.f;            ///< metres per height unit
        S32 mWidth = 0;             ///< columns a side, along SL x and along SL y
        S32 mBorder = 0;            ///< columns past the tile proper each side
        F32 mHeightRange = 0.f;     ///< metres from the origin's z to the top of the tile's heights
        S32 mClimb = 0;             ///< units within which two tops merge their codes and a crevice is bridged

        S32 columns() const { return mWidth * mWidth; }
        /// The top of the tile's heights in units: the ceiling of a gap with nothing above it.
        U16 topUnits() const;
    };

    /// One tile's solid raster: every column's solid spans, lowest first, in arrays by field. Column (ix, iy)
    /// covers mOrigin + (ix, iy) * mCell to one cell further, at index ix + iy * mWidth (as AirGrid).
    struct ColumnRaster
    {
        ColumnFrame mFrame;
        std::vector<U32> mStart;    ///< columns + 1 offsets
        std::vector<U16> mBottom;   ///< smin, units
        std::vector<U16> mTop;      ///< smax, units
        std::vector<U8> mCode;      ///< the area code (skairraster.h); 0 for none
        std::vector<U8> mWalk;      ///< ColumnWalk bits

        bool empty() const { return mStart.empty(); }
        U32 count(S32 column) const { return mStart[column + 1] - mStart[column]; }
        size_t spans() const { return mTop.size(); }
    };

    /// What one raster build counts.
    struct ColumnRasterStats
    {
        U32 mRawSpans = 0;          ///< spans clipped and filled, before the merge
        U32 mSpans = 0;             ///< after it, the bridges and the cull
        U32 mMaxSpans = 0;          ///< the most one column held merged
        U32 mHeapColumns = 0;       ///< columns too tall for the stack's merge
        U32 mTopWins = 0;           ///< merges that took the higher top's code where Recast would have kept the higher code
        U32 mBridges = 0;
        U32 mBridgesIgnored = 0;    ///< bridges outside the columns asked for
        F64 mClipMs = 0.0;          ///< clip and emit
        F64 mCountMs = 0.0;         ///< count and fill (the scatter)
        F64 mMergeMs = 0.0;
        F64 mFillSolidMs = 0.0;
        F64 mCutMs = 0.0;
        F64 mBridgesMs = 0.0;
        F64 mCullMs = 0.0;          ///< and the compaction into the arrays
        F64 mTopWalkMs = 0.0;       ///< the air's walk rules
        F64 mAvatarWalkMs = 0.0;    ///< AVATAR's view and its walk rules
        /// Digests of AVATAR's view after its walk rules, relative to its corner: where it walks, and on what.
        U64 mAvatarWalkHash = 0;
        U64 mAvatarAreaHash = 0;
    };

    /// What a raster is made of beyond the frame: the soup, and, for a window raster, the triangles to leave out
    /// and the columns it must be complete over (half-open ix0, iy0, ix1, iy1). The land under it culls it.
    struct ColumnSource
    {
        const AirSoup* mSoup = nullptr;
        const TerrainWindow* mTerrain = nullptr;
        bool mDropLeaves = false;           ///< leave out the triangles AVATAR leaves out
        const S32* mRect = nullptr;         ///< only what touches these columns; null for everything
        const U8* mBridgeColumns = nullptr; ///< per column, where bridges may be laid; null for everywhere
        /// When given, the raster as it stood after the cut, and after the bridges (before the cull), for tests.
        ColumnRaster* mAfterCut = nullptr;
        ColumnRaster* mAfterBridges = nullptr;
    };

    /// Clip, emit, count, scatter and merge every triangle, then every closed body's fill, cut the exclusion
    /// volumes out, lay the crevice bridges and cull what lies under the land: the raster before its walk rules.
    /// Pure; reuses this thread's scratch.
    void rasteriseColumns(const ColumnFrame& frame, const ColumnSource& source, ColumnRaster& out, ColumnRasterStats* stats = nullptr);

    /// What a class walks, in units: the step that still connects, the headroom it needs, and the drop past
    /// which a rim is a ledge (the climb, or two cells at its slope, whichever is more, so a slope it walks is
    /// never read as a ledge).
    struct WalkParams
    {
        S32 mClimb = 0;
        S32 mHeight = 0;
        S32 mLedge = 0;
    };

    /// A class's walk parameters at a cell and unit, from metres and degrees.
    WalkParams walkParams(F32 climb_m, F32 height_m, F32 slope_deg, F32 cell, F32 unit);

    /// The walk rules over a raster's spans, as the arrays give them: a square of width columns (index
    /// x + y * width), each column's spans from start[c] to start[c + 1]. A span walks where its code is not 0,
    /// or where it is the obstacle within climb over a span that walked before the rules (its code inherited,
    /// no chaining); then it walks only with height units of headroom over it (a column's last span is open
    /// above), and, past the rim rule, only where no reachable neighbour floor lies more than mLedge below it
    /// and those within mLedge spread no more than mLedge (Recast's ledge filter: off the grid is a drop,
    /// under a column's first span a virtual floor mLedge below). code_out gets every span's code, pre_rim
    /// (when given) and walk whether it walks before and after the rim rule.
    void walkRules(S32 width, const U32* start, const U16* bottom, const U16* top, const U8* code_in, const WalkParams& p, U8* code_out,
                   U8* pre_rim, U8* walk);

    /// What the tile's raster needs to walk it.
    struct ColumnWalkInput
    {
        WalkParams mTop;            ///< the air's TOP surfaces
        WalkParams mAvatar;
        S32 mAvatarInset = 0;       ///< columns AVATAR's view lies in from the raster's edge
    };

    /// The walk rules of both views over a tile's raster: mCode becomes the air's code (a step-up inherits the
    /// code under it), mWalk gets TOP_WALK and TOP_PRE_LEDGE from the air's rules and AVATAR_WALK from AVATAR's
    /// over its own view: cropped by mAvatarInset, codes under AREA_CODE_BASE masked, and the columns round the
    /// records it leaves out (door leaves) read from the window raster, a raster of the soup without them.
    void walkColumns(ColumnRaster& raster, const ColumnWalkInput& input, const ColumnRaster* window = nullptr, const U8* in_window = nullptr,
                     ColumnRasterStats* stats = nullptr);

    /// The columns round the triangles AVATAR leaves out (each one's cell box a cell wider), and the rectangle a
    /// window raster must be complete over: those columns a cell wider again, half-open ix0, iy0, ix1, iy1.
    /// False when the soup leaves nothing out.
    bool leafWindow(const ColumnFrame& frame, const AirSoup& soup, std::vector<U8>& in_window, S32* rect);

    /// The frame of a tile padded by border columns, as the Recast grid had it.
    ColumnFrame tileFrame(S32 tile_x, S32 tile_y, F32 tile_size, F32 z_min, F32 z_max, F32 cell, S32 border, F32 climb_m);
}

#endif // SK_AIRCOLUMNS_H
