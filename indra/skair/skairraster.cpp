/**
 * @file skairraster.cpp
 * @brief One tile's solid raster from census records and terrain, and the air products read off it.
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

#include "linden_common.h"

#include "skairraster.h"
#include "skairstats.h"

#include "llapp.h"
#include "llstring.h"
#include "skaircolumns.h"
#include "skairfaceoffsets.h"
#include "skairsoup.h"
#include "skairwalls.h"
#include "skcensusprofile.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
    using namespace skworld;

    /// A raster's columns in the fixtures' order, Recast's (x along SL y fastest, z along SL x), for a capture.
    void dumpColumns(const ColumnRaster& r, RasterColumns& out, U8 walk_bit = 0)
    {
        const S32 W = r.mFrame.mWidth;
        out.mWidth = W;
        out.mHeight = W;
        out.mStart.assign(1, 0);
        out.mSpans.clear();
        for (S32 ix = 0; ix < W; ++ix)
        {
            for (S32 iy = 0; iy < W; ++iy)
            {
                const S32 c = ix + iy * W;
                for (U32 k = r.mStart[c]; k < r.mStart[c + 1]; ++k)
                {
                    const U32 code = !walk_bit || (r.mWalk[k] & walk_bit) ? r.mCode[k] : 0;
                    out.mSpans.push_back(RasterColumns::pack(r.mBottom[k], r.mTop[k], code));
                }
                out.mStart.push_back(static_cast<U32>(out.mSpans.size()));
            }
        }
    }

    /// SL triangles back into the fixtures' Recast axes.
    void recastVerts(const std::vector<F32>& sl, std::vector<F32>& out)
    {
        out.resize(sl.size());
        for (size_t i = 0; i + 2 < sl.size(); i += 3)
        {
            toRecast(LLVector3(&sl[i]), &out[i]);
        }
    }
}

namespace skworld
{
    S32 airPadCells(F32 cell)
    {
        return static_cast<S32>(std::ceil(AIR_PAD_M / cell - 1e-4f));
    }

    S32 borderCells(F32 radius, F32 cell)
    {
        const S32 r = static_cast<S32>(std::ceil(radius / cell));
        return r + 2 * r + 3;
    }

    U8 nameCue(const std::string& name, const std::string& description)
    {
        std::string text = name + " " + description;
        LLStringUtil::toLower(text);
        if (text.find("stair") != std::string::npos || text.find("step") != std::string::npos)
        {
            return CUE_STAIR;
        }
        return text.find("ramp") != std::string::npos ? CUE_RAMP : CUE_NONE;
    }

    const char* AirBuildStats::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "extract", "fold", "clearance", "crop", "distil", "seams", "tops" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    const char* ClassBuildStats::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "soup", "rasterise", "fill", "cut", "bridges", "cull", "filters", "walls", "air", "edges" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    namespace
    {
        TileAirPtr coarseAirOf(const TileInput& input, const AirSoup& soup);

        /// The raster stage over a tile's soup at the input's cell.
        bool rasteriseSoup(const TileInput& input, const AirSoup& soup, TileAirResult& result, TileBuildStats* stats,
                           std::chrono::steady_clock::time_point started, RasterCapture* capture)
        {
            const ClassParams& ap = input.mAvatar;
            ClassParams tp = input.mTopParams;
            tp.mSlope = llmax(tp.mSlope, ap.mSlope);
            tp.mClimb = ap.mClimb;
            ClassBuildStats* astats = stats ? &stats->mClass[0] : nullptr;
            ClassBuildStats* tstats = stats ? &stats->mTop : nullptr;
            StageClock clock(astats ? astats->mMs : nullptr);
            const F32 cell = input.mCell;

            // One raster serves AVATAR and the air: AVATAR's border widened to the air's pad when it keeps air.
            const S32 avatar_border = borderCells(ap.mRadius, cell);
            const S32 border = input.mAir ? std::max(avatar_border, input.mAirPad) : avatar_border;
            const ColumnFrame frame = tileFrame(input.mTileX, input.mTileY, input.mTileSize, input.mZMin, input.mZMax, cell, border, ap.mClimb);
            ColumnRasterStats cstats;
            ColumnRaster raster, cut, bridged;
            ColumnSource source;
            source.mSoup = &soup;
            source.mTerrain = &input.mTerrain;
            source.mAfterCut = capture ? &cut : nullptr;
            source.mAfterBridges = capture ? &bridged : nullptr;
            rasteriseColumns(frame, source, raster, &cstats);
            // Records AVATAR leaves out and the air keeps - door leaves - are rasterised again without them over
            // a window round them, whose columns AVATAR's view reads instead.
            std::vector<U8> in_window;
            S32 rect[4];
            ColumnRaster window;
            const bool windowed = leafWindow(frame, soup, in_window, rect);
            if (windowed)
            {
                ColumnSource w;
                w.mSoup = &soup;
                w.mTerrain = &input.mTerrain;
                w.mDropLeaves = true;
                w.mRect = rect;
                w.mBridgeColumns = in_window.data();
                rasteriseColumns(frame, w, window, &cstats);
                if (stats)
                {
                    stats->mWindowCells = static_cast<U32>(std::count(in_window.begin(), in_window.end(), 1));
                    stats->mOwnColumns = stats->mWindowCells;
                    for (S32 k = 0; k < 4; ++k)
                    {
                        stats->mWindowRect[k] = rect[k] - border;
                    }
                }
            }
            if (astats)
            {
                astats->mMs[ClassBuildStats::RASTERISE] += cstats.mClipMs + cstats.mCountMs + cstats.mMergeMs;
                astats->mMs[ClassBuildStats::FILL] += cstats.mFillSolidMs;
                astats->mMs[ClassBuildStats::CUT] += cstats.mCutMs;
                astats->mMs[ClassBuildStats::BRIDGES] += cstats.mBridgesMs;
                astats->mMs[ClassBuildStats::CULL] += cstats.mCullMs;
                astats->mBridges = cstats.mBridges;
                clock.restart();
            }
            if (tstats)
            {
                tstats->mBridges = cstats.mBridges;
            }

            // Both views' walk rules.
            ColumnWalkInput walk;
            walk.mTop = walkParams(tp.mClimb, tp.mHeight, tp.mSlope, cell, cell);
            walk.mAvatar = walkParams(ap.mClimb, ap.mHeight, ap.mSlope, cell, cell);
            walk.mAvatarInset = border - avatar_border;
            const ColumnRaster unwalked = capture ? raster : ColumnRaster();
            walkColumns(raster, walk, windowed ? &window : nullptr, windowed ? in_window.data() : nullptr, &cstats);
            if (astats)
            {
                astats->mHfHash = cstats.mAvatarWalkHash;
                astats->mHfAreaHash = cstats.mAvatarAreaHash;
                astats->mMs[ClassBuildStats::FILTERS] += cstats.mAvatarWalkMs;
                if (tstats)
                {
                    tstats->mBuilt = input.mAir || input.mRunoff;
                    tstats->mMs[ClassBuildStats::FILTERS] += cstats.mTopWalkMs;
                }
                clock.restart();
            }
            if (capture)
            {
                capture->mBmin[0] = frame.mOrigin.mV[VY];
                capture->mBmin[1] = frame.mOrigin.mV[VZ];
                capture->mBmin[2] = frame.mOrigin.mV[VX];
                capture->mCell = frame.mCell;
                capture->mUnit = frame.mUnit;
                capture->mBorder = border;
                capture->mAvatarBorder = avatar_border;
                capture->mClimb = frame.mClimb;
                capture->mCodes = soup.mCodes;
                capture->mReal.resize(AREA_CODE_MAX + 1);
                for (S32 c = 0; c <= AREA_CODE_MAX; ++c)
                {
                    capture->mReal[c] = areaOfCode(static_cast<U8>(c));
                }
                recastVerts(soup.mVerts, capture->mSoupVerts);
                recastVerts(soup.mCutVerts, capture->mCutVerts);
                capture->mSolids.clear();
                for (const auto& [a, b] : soup.mSolids)
                {
                    capture->mSolids.insert(capture->mSolids.end(), { a, b });
                }
                capture->mCuts.clear();
                for (const auto& [a, b] : soup.mCuts)
                {
                    capture->mCuts.insert(capture->mCuts.end(), { a, b });
                }
                capture->mLeafTri.clear();
                capture->mCues.resize(soup.ntris());
                for (S32 t = 0; t < soup.ntris(); ++t)
                {
                    capture->mCues[t] = soup.cue(t);
                    if (soup.mLeaves)
                    {
                        capture->mLeafTri.push_back((soup.mFlags[t] & SOUP_LEAF) ? 1 : 0);
                    }
                }
                capture->mTerrain = input.mTerrain;
                dumpColumns(cut, capture->mRasterised);
                dumpColumns(bridged, capture->mBridged);
                dumpColumns(unwalked, capture->mCulled);
                dumpColumns(raster, capture->mTopPreLedge, TOP_PRE_LEDGE);
                dumpColumns(raster, capture->mTopFiltered, TOP_WALK);
                dumpColumns(raster, capture->mAvatarFiltered, AVATAR_WALK);
                capture->mInWindow = in_window;
                capture->mOwnColumn = in_window;
                // Per span of the culled raster in the fixtures' column order, its AVATAR_FLOOR.
                capture->mAvatarFloorStart.assign(1, 0);
                capture->mAvatarFloor.clear();
                for (S32 ix = 0; ix < frame.mWidth; ++ix)
                {
                    for (S32 iy = 0; iy < frame.mWidth; ++iy)
                    {
                        const S32 c = ix + iy * frame.mWidth;
                        for (U32 k = raster.mStart[c]; k < raster.mStart[c + 1]; ++k)
                        {
                            capture->mAvatarFloor.push_back((raster.mWalk[k] & AVATAR_WALK) ? AirGap::AVATAR_FLOOR : 0);
                        }
                        capture->mAvatarFloorStart.push_back(static_cast<U32>(capture->mAvatarFloor.size()));
                    }
                }
                clock.restart();
            }

            // Faces of every solid, door leaves included, with their areas the air's.
            if (input.mWallFaces)
            {
                extractWallFaces(raster, border, input.mWallMinHeight, result.mFaces);
                if (input.mFaceOffsets)
                {
                    alignWallFaces(result.mFaces, soup.mVerts.data(), soup.ntris(), cell, cell);
                }
            }
            clock.lap(ClassBuildStats::WALLS);

            // The air's padded gaps, read here so the offsets reach the tile's edge columns; the air reuses them.
            AirGrid padded_air;
            std::vector<FaceOffset> padded_faces;
            const S32 air_pad = std::clamp(input.mAirPad, 0, border);
            if (input.mAir && (input.mFaceOffsets || capture))
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                extractAir(raster, border - air_pad, padded_air);
                foldSlivers(padded_air, static_cast<S32>(std::ceil(input.mAirMinGap / cell)));
                if (input.mFaceOffsets)
                {
                    computeFaceOffsets(padded_air, soup.mVerts.data(), soup.ntris(), frame.mClimb, padded_faces, stats ? &stats->mFaceOffsets : nullptr,
                                       stats ? stats->mFaceTraceEvery : 0, stats ? &stats->mFaceTrace : nullptr);
                }
                if (capture)
                {
                    capture->mPadded = padded_air;
                    capture->mPaddedFaceOffsets = padded_faces;
                }
                top_clock.lap(ClassBuildStats::AIR);
                clock.restart();
            }
            const S32 padded_width = padded_air.mWidth;

            if (input.mAir)
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                const S32 overlap = static_cast<S32>(std::ceil(0.5f / cell));
                result.mAir = buildTileAir(raster, border, input.mAirPad, frame.mClimb, overlap, static_cast<S32>(std::ceil(input.mAirMinGap / cell)),
                                          tstats ? &tstats->mAir : nullptr, nullptr, &padded_air);
                if (result.mAir && !padded_faces.empty())
                {
                    // Not shared yet: the table goes in before the air is published.
                    auto& air = const_cast<TileAir&>(*result.mAir);
                    cropFaceOffsets(padded_faces, padded_width, air_pad, air.mWidth, air.mHeight, air.mFaceOffsets);
                    air.countLive();
                    if (stats)
                    {
                        stats->mFaceOffsets.mEntries += static_cast<U32>(air.mFaceOffsets.size());
                    }
                }
                top_clock.lap(ClassBuildStats::AIR);
                clock.restart();
            }
            // Where the air's floors end over a drop, before the rim rule took them, and the run-off down them.
            if (input.mRunoff)
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                extractRunoff(raster, border, frame.mClimb, static_cast<S32>(std::ceil(tp.mHeight / cell)), input.mRunoffMax, result.mLedges,
                              result.mRunoff);
                top_clock.lap(ClassBuildStats::EDGES);
                clock.restart();
            }

            // Past the air's radius the spaces still need air: the tile again at the coarse cell, in this tile's cells.
            if (!input.mAir && input.mCoarseAir)
            {
                const auto coarse_started = std::chrono::steady_clock::now();
                result.mAir = coarseAirOf(input, soup);
                result.mAirBuilt = true;
                result.mCoarseAir = result.mAir != nullptr;
                if (stats)
                {
                    stats->mCoarseAirMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - coarse_started).count();
                }
                clock.restart();
            }

            if (astats)
            {
                astats->mHfSpans = static_cast<U32>(raster.spans());
                astats->mHfMaxSpans = cstats.mMaxSpans;
                astats->mHfColumns = 0;
                for (S32 c = 0; c < frame.columns(); ++c)
                {
                    astats->mHfColumns += raster.count(c) ? 1 : 0;
                }
                astats->mHfBytes = raster.mStart.size() * sizeof(U32) + raster.spans() * (2 * sizeof(U16) + 2);
            }
            result.mMillis = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (stats)
            {
                stats->mExcludeMs = result.mExcludeMillis;
                stats->mRasterMs = result.mMillis;
                stats->mTotalMs = result.mMillis;
            }
            return true;
        }

        TileAirPtr coarseAirOf(const TileInput& input, const AirSoup& soup)
        {
            const S32 factor = static_cast<S32>(std::lround(COARSE_AIR_M / std::max(input.mCell, 1e-3f)));
            if (factor < 1 || (factor & (factor - 1)) || std::fabs(factor * input.mCell - COARSE_AIR_M) > 1e-4f)
            {
                return nullptr;
            }
            // The soup and its codes do not depend on the cell.
            TileInput c = input;
            c.mCell = COARSE_AIR_M;
            c.mAir = true;
            c.mCoarseAir = false;
            c.mAirPad = airPadCells(c.mCell);
            c.mWallFaces = false;
            c.mRunoff = false;
            TileAirResult cr;
            cr.mAirBuilt = true;
            return rasteriseSoup(c, soup, cr, nullptr, std::chrono::steady_clock::now(), nullptr) && cr.mAir ? upsampleAir(*cr.mAir, factor) : nullptr;
        }
    }

    bool rasteriseTile(const TileInput& input, TileAirResult& result, TileBuildStats* stats, RasterCapture* capture)
    {
        SK_WORLD_ZONE("air.raster");
        const auto started = std::chrono::steady_clock::now();
        // An empty tile has no air to keep, which is an answer too.
        result.mAirBuilt = input.mAir;
        AirSoup soup;
        // A tile left at logout stops between its stages: the pool drains before it joins.
        if (!buildAirSoup(input, soup, result, stats) || LLApp::isExiting())
        {
            return false;
        }
        if (!rasteriseSoup(input, soup, result, stats, started, capture))
        {
            return false;
        }
        result.mStatus = TileAirResult::SUCCESS;
        return true;
    }
}
