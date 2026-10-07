/**
 * @file sknavreplaystats.cpp
 * @brief A measured replay written out: per-stage times, sizes, how polygons fall across spaces, pathing and edits.
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

#include "sknavreplaystats.h"

#include "sknavlayout.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <ostream>
#include <random>
#include <set>

namespace
{
    using namespace sknav;
    typedef std::chrono::steady_clock Clock;
    constexpr U32 NO_GAP = U32_MAX;
    constexpr S32 BLOCK_CELLS = 16;     ///< a 2 m block at 0.125 m

    F64 since(Clock::time_point t0)
    {
        return std::chrono::duration<F64, std::milli>(Clock::now() - t0).count();
    }

    /// The leaf over every cell of the tile, -1 where there is no air.
    std::vector<S32> leafMap(const TileAir& air)
    {
        std::vector<S32> map(static_cast<size_t>(air.mWidth) * air.mHeight, -1);
        for (size_t i = 0; i < air.mLeaves.size(); ++i)
        {
            const AirLeaf& l = air.mLeaves[i];
            const S32 n = 1 << l.mLevel;
            for (S32 y = l.mY; y < std::min<S32>(l.mY + n, air.mHeight); ++y)
            {
                for (S32 x = l.mX; x < std::min<S32>(l.mX + n, air.mWidth); ++x)
                {
                    map[x + y * air.mWidth] = static_cast<S32>(i);
                }
            }
        }
        return map;
    }

    /// The gap whose [floor - 1 - slack, ceiling) holds a span's top.
    U32 gapOf(const TileAir& air, const std::vector<S32>& leaves, S32 ix, S32 iy, S32 top, S32 slack)
    {
        if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
        {
            return NO_GAP;
        }
        const S32 leaf = leaves[ix + iy * air.mWidth];
        if (leaf < 0)
        {
            return NO_GAP;
        }
        const AirLeaf& l = air.mLeaves[leaf];
        for (U32 k = 0; k < l.mGapCount; ++k)
        {
            const AirGap& g = air.mGaps[l.mFirstGap + k];
            if (top >= static_cast<S32>(g.mFloor) - 1 - slack && top < static_cast<S32>(g.mCeiling))
            {
                return l.mFirstGap + k;
            }
        }
        return NO_GAP;
    }

    F32 segmentDistance(const LLVector3& p, const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 ab = b - a;
        const F32 len2 = ab.lengthSquared();
        const F32 t = len2 > 0.f ? llclamp((p - a) * ab / len2, 0.f, 1.f) : 0.f;
        return dist_vec(p, a + ab * t);
    }

    /// Open cells from a gap along one direction until a wall, or -1 past max or off the tile.
    S32 openRun(const TileAir& air, const std::vector<S32>& leaves, S32 ix, S32 iy, const AirGap& from, S32 dx, S32 dy, S32 climb,
                S32 overlap, S32 max)
    {
        const AirGap* gap = &from;
        for (S32 k = 1; k <= max + 1; ++k)
        {
            const S32 x = ix + dx * k, y = iy + dy * k;
            if (x < 0 || y < 0 || x >= air.mWidth || y >= air.mHeight)
            {
                return -1;
            }
            const S32 leaf = leaves[x + y * air.mWidth];
            const AirGap* next = nullptr;
            if (leaf >= 0)
            {
                const AirLeaf& l = air.mLeaves[leaf];
                for (U32 g = 0; g < l.mGapCount && !next; ++g)
                {
                    next = airAdjacent(*gap, air.mGaps[l.mFirstGap + g], climb, overlap) ? &air.mGaps[l.mFirstGap + g] : nullptr;
                }
            }
            if (!next)
            {
                return k - 1;
            }
            gap = next;
        }
        return -1;
    }

    /// Cells whose gap is an opening at most two cells wide along x or y, walls on both sides.
    void narrowOpenings(const TileAir& air, const std::vector<S32>& leaves, S32 climb, S32 overlap, U32& cells, U32& walkable_cells,
                        U32& blocks, U32& walkable_blocks)
    {
        const S32 bw = (air.mWidth + BLOCK_CELLS - 1) / BLOCK_CELLS, bh = (air.mHeight + BLOCK_CELLS - 1) / BLOCK_CELLS;
        std::vector<U8> block(static_cast<size_t>(bw) * bh, 0);
        for (S32 iy = 0; iy < air.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < air.mWidth; ++ix)
            {
                const S32 leaf = leaves[ix + iy * air.mWidth];
                if (leaf < 0)
                {
                    continue;
                }
                const AirLeaf& l = air.mLeaves[leaf];
                U8 found = 0;
                for (U32 g = 0; g < l.mGapCount; ++g)
                {
                    const AirGap& gap = air.mGaps[l.mFirstGap + g];
                    if (static_cast<S32>(gap.mCeiling) - static_cast<S32>(gap.mFloor) < overlap)
                    {
                        continue;
                    }
                    for (S32 axis = 0; axis < 2; ++axis)
                    {
                        const S32 dx = axis == 0 ? 1 : 0, dy = axis == 0 ? 0 : 1;
                        const S32 lo = openRun(air, leaves, ix, iy, gap, -dx, -dy, climb, overlap, 1);
                        const S32 hi = lo < 0 ? -1 : openRun(air, leaves, ix, iy, gap, dx, dy, climb, overlap, 1);
                        if (lo >= 0 && hi >= 0 && lo + hi + 1 <= 2)
                        {
                            found |= (gap.mFlags & AirGap::TOP) ? 3 : 1;
                            break;
                        }
                    }
                }
                if (found)
                {
                    ++cells;
                    walkable_cells += (found & 2) ? 1 : 0;
                    block[ix / BLOCK_CELLS + (iy / BLOCK_CELLS) * bw] |= found;
                }
            }
        }
        for (const U8 b : block)
        {
            blocks += b ? 1 : 0;
            walkable_blocks += (b & 2) ? 1 : 0;
        }
    }

    /// Footprint distance from the camera, as SKNavMesh::inAirRadius measures it.
    F64 cameraDistance(const CensusFile& census, const ReplayTile& tile)
    {
        const LLVector3d& c = census.mCamera;
        const F64 dx = std::max({ tile.mMin.mdV[VX] - c.mdV[VX], 0.0, c.mdV[VX] - tile.mMax.mdV[VX] });
        const F64 dy = std::max({ tile.mMin.mdV[VY] - c.mdV[VY], 0.0, c.mdV[VY] - tile.mMax.mdV[VY] });
        return std::sqrt(dx * dx + dy * dy);
    }

    void writeClass(std::ostream& out, const ClassBuildStats& c)
    {
        out << "{\"built\":" << (c.mBuilt ? "true" : "false") << ",\"ms\":{";
        for (S32 k = 0; k < ClassBuildStats::NUM_STAGES; ++k)
        {
            out << (k ? "," : "") << '"' << ClassBuildStats::stageName(k) << "\":" << c.mMs[k];
        }
        out << "},\"air_ms\":{";
        for (S32 k = 0; k < AirBuildStats::NUM_STAGES; ++k)
        {
            out << (k ? "," : "") << '"' << AirBuildStats::stageName(k) << "\":" << c.mAir.mMs[k];
        }
        out << "},\"hf_spans\":" << c.mHfSpans << ",\"hf_bytes\":" << c.mHfBytes << ",\"chf_spans\":" << c.mChfSpans << ",\"chf_bytes\":" << c.mChfBytes
            << ",\"walkable_spans\":" << c.mWalkableSpans << ",\"bridges\":" << c.mBridges << ",\"polys\":" << c.mPolys << ",\"verts\":" << c.mVerts
            << ",\"detail_tris\":" << c.mDetailTris << ",\"detour_bytes\":" << c.mDetourBytes << ",\"flights\":" << c.mFlights
            << ",\"flight_polys\":" << c.mFlightPolys << ",\"flight_codes\":" << c.mFlightCodes << ",\"codes\":" << c.mCodes
            << ",\"grid_bytes\":" << c.mAir.mGridBytes << ",\"strip_bytes\":" << c.mAir.mStripBytes << ",\"grid_gaps\":" << c.mAir.mGridGaps << '}';
    }

    struct NoGap
    {
        U32 mMesh;
        LLVector3 mPos;
        F32 mPortal;
    };

    /// Spans, spaces and polys of one tile, written as the fields of its object.
    void writeSpaceFields(std::ostream& out, const CensusFile& census, const ReplayMesh& mesh, U32 mesh_index, const ReplayTile& tile,
                          std::vector<NoGap>& nogaps)
    {
        const TileBuildStats& st = *tile.mStats;
        const TileBuildResult& r = *tile.mResult;
        const TileAir* air = r.mAirBuilt && r.mAir ? r.mAir.get() : nullptr;
        const TileEnclosure* enc = mesh.mEnclosure ? mesh.mEnclosure->tile(tile.mKey) : nullptr;
        if (enc && enc->mAir.get() != air)
        {
            enc = nullptr;
        }
        const S32 climb = static_cast<S32>(std::floor(census.mParams.mParams[static_cast<S32>(AgentClass::AVATAR)].mClimb / census.mParams.mCell));
        std::vector<S32> leaves;
        if (air)
        {
            leaves = leafMap(*air);
        }
        // Each span's gap and space: 0 for none, U16_MAX with no gap at all.
        std::vector<U32> space(st.mSpans.size(), 0);
        U32 nogap = 0, nogap_climb = 0, unspaced = 0;
        for (size_t i = 0; i < st.mSpans.size(); ++i)
        {
            const StatSpan& s = st.mSpans[i];
            // The strict rule first, then a climb below the floor a merged leaf kept.
            U32 gap = air ? gapOf(*air, leaves, s.mX, s.mY, s.mTop, 0) : NO_GAP;
            if (gap == NO_GAP)
            {
                ++nogap;
                gap = air ? gapOf(*air, leaves, s.mX, s.mY, s.mTop, climb) : NO_GAP;
            }
            if (gap == NO_GAP)
            {
                ++nogap_climb;
                space[i] = U16_MAX;
                if (air)
                {
                    const LLVector3 pos(air->mOrigin.mV[VX] + (s.mX + 0.5f) * air->mCell, air->mOrigin.mV[VY] + (s.mY + 0.5f) * air->mCell,
                                        air->mOrigin.mV[VZ] + s.mTop * air->mUnit);
                    F32 best = -1.f;
                    if (mesh.mRooms)
                    {
                        for (const Portal& p : mesh.mRooms->mPortals)
                        {
                            const F32 d = segmentDistance(pos, p.mA, p.mB);
                            best = best < 0.f ? d : llmin(best, d);
                        }
                    }
                    nogaps.push_back({ mesh_index, pos, best });
                }
                continue;
            }
            space[i] = enc && gap < enc->mGaps.size() ? enc->mGaps[gap].mSpace : 0;
            unspaced += space[i] == 0 ? 1 : 0;
        }

        // Spaces touching across a span contact within climb, then a greedy colouring of them.
        const S32 n = air ? air->mWidth : 256;
        std::vector<U32> col_start(static_cast<size_t>(n) * n + 1, 0);
        for (const StatSpan& s : st.mSpans)
        {
            if (s.mX < n && s.mY < n)
            {
                ++col_start[s.mX + s.mY * n + 1];
            }
        }
        for (size_t c = 1; c < col_start.size(); ++c)
        {
            col_start[c] += col_start[c - 1];
        }
        std::vector<U32> col_spans(col_start.back());
        {
            std::vector<U32> fill(col_start.begin(), col_start.end() - 1);
            for (size_t i = 0; i < st.mSpans.size(); ++i)
            {
                const StatSpan& s = st.mSpans[i];
                if (s.mX < n && s.mY < n)
                {
                    col_spans[fill[s.mX + s.mY * n]++] = static_cast<U32>(i);
                }
            }
        }
        std::map<U32, std::set<U32>> adjacent;
        std::set<std::pair<U32, U32>> pairs_raw, pairs_raw_all;
        for (size_t i = 0; i < st.mSpans.size(); ++i)
        {
            const StatSpan& s = st.mSpans[i];
            const U32 sp = space[i] == U16_MAX ? 0 : space[i];
            pairs_raw_all.emplace(s.mCode, sp);
            if (sp)
            {
                pairs_raw.emplace(s.mCode, sp);
                adjacent[sp];
            }
            if (!sp)
            {
                continue;
            }
            static const S32 dirs[2][2] = { { 1, 0 }, { 0, 1 } };
            for (const auto& d : dirs)
            {
                const S32 x = s.mX + d[0], y = s.mY + d[1];
                if (x >= n || y >= n)
                {
                    continue;
                }
                for (U32 k = col_start[x + y * n]; k < col_start[x + y * n + 1]; ++k)
                {
                    const U32 j = col_spans[k];
                    const U32 sj = space[j] == U16_MAX ? 0 : space[j];
                    if (sj && sj != sp && std::abs(static_cast<S32>(st.mSpans[j].mTop) - static_cast<S32>(s.mTop)) <= climb)
                    {
                        adjacent[sp].insert(sj);
                        adjacent[sj].insert(sp);
                    }
                }
            }
        }
        std::vector<std::pair<size_t, U32>> order;
        for (const auto& [sp, nb] : adjacent)
        {
            order.emplace_back(nb.size(), sp);
        }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        std::map<U32, U32> colour;
        U32 colours = 0;
        for (const auto& o : order)
        {
            std::set<U32> used;
            for (const U32 nb : adjacent[o.second])
            {
                const auto it = colour.find(nb);
                if (it != colour.end())
                {
                    used.insert(it->second);
                }
            }
            U32 c = 1;
            while (used.count(c))
            {
                ++c;
            }
            colour[o.second] = c;
            colours = std::max(colours, c);
        }
        std::set<std::pair<U32, U32>> pairs_coloured, pairs_coloured_all;
        for (size_t i = 0; i < st.mSpans.size(); ++i)
        {
            const U32 sp = space[i] == U16_MAX ? 0 : space[i];
            const U32 c = sp ? colour[sp] : 0;
            pairs_coloured_all.emplace(st.mSpans[i].mCode, c);
            if (sp)
            {
                pairs_coloured.emplace(st.mSpans[i].mCode, c);
            }
        }

        // Purity: a poly whose spans lie in more than one space.
        U32 impure = 0, flight_polys = 0, spans_matched = 0;
        F64 area = 0.0, area_impure = 0.0, area_unspaced = 0.0;
        const size_t npolys = st.mPolyArea.size();
        for (size_t p = 0; p < npolys; ++p)
        {
            std::set<U32> in;
            bool lacks = false;
            for (U32 k = st.mPolySpanStart[p]; k < st.mPolySpanStart[p + 1]; ++k)
            {
                const U32 sp = space[st.mPolySpans[k]];
                ++spans_matched;
                if (sp == 0 || sp == U16_MAX)
                {
                    lacks = true;
                }
                else
                {
                    in.insert(sp);
                }
            }
            area += st.mPolyArea[p];
            if (in.size() > 1)
            {
                ++impure;
                area_impure += st.mPolyArea[p];
            }
            if (lacks)
            {
                area_unspaced += st.mPolyArea[p];
            }
            flight_polys += p < st.mPolyFlight.size() ? st.mPolyFlight[p] : 0;
        }

        U32 narrow = 0, narrow_walkable = 0, blocks = 0, blocks_walkable = 0;
        if (air)
        {
            narrowOpenings(*air, leaves, climb, static_cast<S32>(std::ceil(0.5f / air->mUnit)), narrow, narrow_walkable, blocks, blocks_walkable);
        }
        out << ",\"spaces\":" << adjacent.size() << ",\"spans\":" << st.mSpans.size() << ",\"spans_nogap\":" << nogap << ",\"spans_nogap_climb\":" << nogap_climb
            << ",\"spans_unspaced\":" << unspaced << ",\"pairs_raw\":" << pairs_raw.size() << ",\"pairs_raw_all\":" << pairs_raw_all.size()
            << ",\"colours\":" << colours << ",\"pairs_coloured\":" << pairs_coloured.size() << ",\"pairs_coloured_all\":" << pairs_coloured_all.size()
            << ",\"polys\":" << npolys << ",\"polys_impure\":" << impure << ",\"poly_area\":" << area << ",\"poly_area_impure\":" << area_impure
            << ",\"poly_area_unspaced\":" << area_unspaced << ",\"poly_spans\":" << spans_matched << ",\"flight_polys\":" << flight_polys
            << ",\"narrow_cells\":" << narrow << ",\"narrow_walkable_cells\":" << narrow_walkable << ",\"narrow_blocks\":" << blocks
            << ",\"narrow_walkable_blocks\":" << blocks_walkable << ",\"blocks\":"
            << (air ? ((air->mWidth + BLOCK_CELLS - 1) / BLOCK_CELLS) * ((air->mHeight + BLOCK_CELLS - 1) / BLOCK_CELLS) : 0);
    }

    size_t coarseBytes(const CoarseAir& c)
    {
        return c.mStart.size() * sizeof(U32) + c.mGaps.size() * sizeof(CoarseGap);
    }

    /// Plan-view overlap of a box and a convex footprint, by separating axes.
    bool boxTouchesFootprint(F32 x0, F32 y0, F32 x1, F32 y1, const std::vector<LLVector2>& hull)
    {
        if (hull.empty())
        {
            return false;
        }
        F32 hx0 = FLT_MAX, hy0 = FLT_MAX, hx1 = -FLT_MAX, hy1 = -FLT_MAX;
        for (const LLVector2& p : hull)
        {
            hx0 = llmin(hx0, p.mV[VX]);
            hy0 = llmin(hy0, p.mV[VY]);
            hx1 = llmax(hx1, p.mV[VX]);
            hy1 = llmax(hy1, p.mV[VY]);
        }
        if (hx1 < x0 || hx0 > x1 || hy1 < y0 || hy0 > y1)
        {
            return false;
        }
        const LLVector2 corners[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        for (size_t i = 0; i < hull.size(); ++i)
        {
            const LLVector2 a = hull[i], b = hull[(i + 1) % hull.size()];
            const LLVector2 axis(b.mV[VY] - a.mV[VY], a.mV[VX] - b.mV[VX]);
            F32 plo = FLT_MAX, phi = -FLT_MAX, blo = FLT_MAX, bhi = -FLT_MAX;
            for (const LLVector2& p : hull)
            {
                const F32 d = p * axis;
                plo = llmin(plo, d);
                phi = llmax(phi, d);
            }
            for (const LLVector2& c : corners)
            {
                const F32 d = c * axis;
                blo = llmin(blo, d);
                bhi = llmax(bhi, d);
            }
            if (phi < blo || bhi < plo)
            {
                return false;
            }
        }
        return true;
    }
}

namespace sknav
{
    void writeReplayStats(std::ostream& out, const CensusFile& census, const ReplayResult& result)
    {
        out << std::setprecision(7);
        out << "{\"version\":1,\"cell\":" << census.mParams.mCell << ",\"air_radius\":" << census.mAirRadius << ",\"camera\":[" << census.mCamera.mdV[VX]
            << ',' << census.mCamera.mdV[VY] << ',' << census.mCamera.mdV[VZ] << "],\"tiles\":[";
        std::vector<NoGap> nogaps;
        bool first = true;
        for (size_t m = 0; m < result.mMeshes.size(); ++m)
        {
            const ReplayMesh& mesh = *result.mMeshes[m];
            for (const auto& [key, tile] : mesh.mTiles)
            {
                if (!tile.mStats || !tile.mResult)
                {
                    continue;
                }
                const TileBuildStats& st = *tile.mStats;
                const TileBuildResult& r = *tile.mResult;
                out << (first ? "" : ",") << "\n{\"mesh\":" << m << ",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"terrain\":" << (mesh.mTerrain ? "true" : "false")
                    << ",\"camera_m\":" << cameraDistance(census, tile) << ",\"status\":" << static_cast<S32>(r.mStatus) << ",\"tris\":" << r.mTriangles
                    << ",\"soup_ms\":" << st.mSoupMs << ",\"exclude_ms\":" << st.mExcludeMs << ",\"total_ms\":" << st.mTotalMs
                    << ",\"excluded\":" << st.mExcludedRecords << ",\"window_cells\":" << st.mExcludeWindowCells << ",\"tile_cells\":" << st.mTileCells
                    << ",\"shared_codes\":" << st.mSharedCodes << ",\"classes\":[";
                first = false;
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    out << (cls ? "," : "");
                    writeClass(out, st.mClass[cls]);
                }
                out << ']';
                const TileAir* air = r.mAirBuilt && r.mAir ? r.mAir.get() : nullptr;
                const TileEnclosure* enc = mesh.mEnclosure ? mesh.mEnclosure->tile(key) : nullptr;
                out << ",\"air_bytes\":" << (air ? air->bytes() : 0) << ",\"air_leaves\":" << (air ? air->mLeaves.size() : 0)
                    << ",\"air_gaps\":" << (air ? air->mGaps.size() : 0) << ",\"enclosure_bytes\":" << (enc ? enc->bytes() : 0)
                    << ",\"coarse_bytes\":" << (enc ? coarseBytes(enc->mCoarse) : 0) << ",\"coarse_gaps\":" << (enc ? enc->mCoarse.mGaps.size() : 0);
                writeSpaceFields(out, census, mesh, static_cast<U32>(m), tile, nogaps);
                out << '}';
            }
        }
        out << "\n],\"meshes\":[";
        for (size_t m = 0; m < result.mMeshes.size(); ++m)
        {
            const ReplayMesh& mesh = *result.mMeshes[m];
            out << (m ? "," : "") << "\n{\"id\":\"" << mesh.mId << "\",\"terrain\":" << (mesh.mTerrain ? "true" : "false") << ",\"tiles\":" << mesh.mTiles.size()
                << ",\"spaces\":" << (mesh.mRooms ? mesh.mRooms->mSpaces.size() : 0) << ",\"portals\":" << (mesh.mRooms ? mesh.mRooms->mPortals.size() : 0)
                << ",\"rooms_bytes\":" << (mesh.mRooms ? mesh.mRooms->bytes() : 0) << ",\"enclosure_bytes\":" << (mesh.mEnclosure ? mesh.mEnclosure->bytes() : 0)
                << ",\"runs\":[";
            for (size_t k = 0; k < mesh.mRoomsRuns.size(); ++k)
            {
                const ReplayRoomsRun& run = mesh.mRoomsRuns[k];
                out << (k ? "," : "") << "{\"round\":" << run.mRound << ",\"tiles\":" << run.mTiles << ",\"enclosure_ms\":" << run.mEnclosureMs
                    << ",\"seam_ms\":" << run.mSeamMs << ",\"spaces_ms\":" << run.mSpacesMs << ",\"enclosure_stages\":{";
                for (S32 s = 0; s < EnclosurePass::NUM_STAGES; ++s)
                {
                    out << (s ? "," : "") << '"' << EnclosurePass::stageName(s) << "\":" << run.mEnclosureStageMs[s];
                }
                out << "},\"spaces_stages\":{";
                for (S32 s = 0; s < SpacesPass::NUM_STAGES; ++s)
                {
                    out << (s ? "," : "") << '"' << SpacesPass::stageName(s) << "\":" << run.mSpacesStageMs[s];
                }
                out << "}}";
            }
            out << "]}";
        }

        // Door-leaf premise: where each AVATAR span with no gap stands.
        out << "\n],\"nogap\":[";
        for (size_t i = 0; i < nogaps.size(); ++i)
        {
            const NoGap& g = nogaps[i];
            out << (i ? "," : "") << '[' << g.mMesh << ',' << g.mPos.mV[VX] << ',' << g.mPos.mV[VY] << ',' << g.mPos.mV[VZ] << ',' << g.mPortal << ']';
        }

        // findPath between random poly centres of each mesh, with the plain filter.
        out << "],\"find_path\":[";
        std::mt19937 rng(1007);
        dtNavMeshQuery* query = dtAllocNavMeshQuery();
        const dtQueryFilter filter;
        first = true;
        for (size_t m = 0; m < result.mMeshes.size(); ++m)
        {
            const dtNavMesh* nav = result.mMeshes[m]->mNav[static_cast<S32>(AgentClass::AVATAR)];
            if (!nav || !dtStatusSucceed(query->init(nav, 4096)))
            {
                continue;
            }
            std::vector<std::pair<dtPolyRef, LLVector3>> polys;
            for (S32 t = 0; t < nav->getMaxTiles(); ++t)
            {
                const dtMeshTile* tile = nav->getTile(t);
                if (!tile || !tile->header)
                {
                    continue;
                }
                const dtPolyRef base = nav->getPolyRefBase(tile);
                for (S32 p = 0; p < tile->header->polyCount; ++p)
                {
                    const dtPoly& poly = tile->polys[p];
                    if (poly.getType() != DT_POLYTYPE_GROUND || !poly.vertCount)
                    {
                        continue;
                    }
                    LLVector3 c;
                    for (S32 v = 0; v < poly.vertCount; ++v)
                    {
                        const F32* pv = &tile->verts[poly.verts[v] * 3];
                        c += LLVector3(pv[0], pv[1], pv[2]);
                    }
                    polys.emplace_back(base | static_cast<dtPolyRef>(p), c / static_cast<F32>(poly.vertCount));
                }
            }
            if (polys.size() < 2)
            {
                continue;
            }
            std::uniform_int_distribution<size_t> pick(0, polys.size() - 1);
            dtPolyRef path[256];
            for (S32 k = 0; k < 400; ++k)
            {
                const auto& a = polys[pick(rng)];
                const auto& b = polys[pick(rng)];
                S32 count = 0;
                const auto t0 = Clock::now();
                const dtStatus status = query->findPath(a.first, b.first, a.second.mV, b.second.mV, &filter, path, &count, 256);
                const F64 ns = std::chrono::duration<F64, std::nano>(Clock::now() - t0).count();
                const bool reached = dtStatusSucceed(status) && count > 0 && path[count - 1] == b.first;
                out << (first ? "" : ",") << '[' << m << ',' << ns << ',' << (reached ? 1 : 0) << ',' << count << ',' << dist_vec(a.second, b.second) << ']';
                first = false;
            }
        }
        dtFreeNavMeshQuery(query);

        // findAirLeaf over random cells of every tile with air.
        out << "],\"air_leaf_ns\":[";
        first = true;
        U64 sink = 0;
        for (const auto& mesh : result.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildResult& r = *tile.mResult;
                if (!r.mAirBuilt || !r.mAir || r.mAir->mLeaves.empty())
                {
                    continue;
                }
                const TileAir& air = *r.mAir;
                std::vector<std::pair<S32, S32>> cells(4096);
                for (auto& c : cells)
                {
                    c = { static_cast<S32>(rng() % air.mWidth), static_cast<S32>(rng() % air.mHeight) };
                }
                const auto t0 = Clock::now();
                for (const auto& c : cells)
                {
                    sink += static_cast<U64>(findAirLeaf(air, c.first, c.second) + 1);
                }
                const F64 ns = std::chrono::duration<F64, std::nano>(Clock::now() - t0).count() / cells.size();
                out << (first ? "" : ",") << ns;
                first = false;
            }
        }

        // The coarse product past the radius: the same tile's air built at 1 m, WEATHER only.
        out << "],\"air_leaf_sink\":" << sink << ",\"coarse\":[";
        first = true;
        for (size_t m = 0; m < result.mMeshes.size(); ++m)
        {
            const ReplayMesh& mesh = *result.mMeshes[m];
            for (const auto& [key, tile] : mesh.mTiles)
            {
                if (!tile.mInput || !tile.mResult || tile.mResult->mStatus != TileBuildResult::SUCCESS)
                {
                    continue;
                }
                TileBuildInput input = *tile.mInput;
                input.mCell = 1.f;
                input.mAirPad = 2;
                input.mAir = true;
                input.mClasses[static_cast<S32>(AgentClass::AVATAR)] = false;
                input.mClasses[static_cast<S32>(AgentClass::WEATHER)] = true;
                input.mWallFaces = false;
                input.mEdges[0] = input.mEdges[1] = false;
                TileBuildStats stats;
                const auto t0 = Clock::now();
                const TileBuildResult coarse = buildTile(input, &stats);
                const F64 ms = since(t0);
                const ClassBuildStats& w = stats.mClass[static_cast<S32>(AgentClass::WEATHER)];
                F64 raster = 0.0;
                for (S32 s = ClassBuildStats::RASTERISE; s <= ClassBuildStats::FILTERS; ++s)
                {
                    raster += w.mMs[s];
                }
                out << (first ? "" : ",") << "\n{\"mesh\":" << m << ",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"camera_m\":" << cameraDistance(census, tile)
                    << ",\"ms\":" << ms << ",\"soup_ms\":" << stats.mSoupMs << ",\"raster_ms\":" << raster << ",\"air_ms\":" << w.mMs[ClassBuildStats::AIR]
                    << ",\"polys_ms\":" << (stats.mTotalMs - stats.mSoupMs - raster - w.mMs[ClassBuildStats::AIR])
                    << ",\"air_bytes\":" << (coarse.mAir ? coarse.mAir->bytes() : 0) << ",\"air_leaves\":" << (coarse.mAir ? coarse.mAir->mLeaves.size() : 0)
                    << ",\"hf_bytes\":" << w.mHfBytes << '}';
                first = false;
            }
        }
        out << "\n]}\n";
    }

    void writeReplayEdits(std::ostream& out, const CensusFile& census, const ReplayResult& result, const std::vector<ReplayEdit>& edits)
    {
        out << std::setprecision(7) << "{\"edits\":[";
        bool first = true;
        for (size_t e = 0; e < edits.size(); ++e)
        {
            const ReplayEdit& edit = edits[e];
            if (edit.mRecord >= census.mRecords.size())
            {
                continue;
            }
            const RecordPtr& rec = census.mRecords[edit.mRecord];
            RecordPtr changed;
            if (edit.mOp != ReplayEdit::REMOVE)
            {
                auto copy = std::make_shared<Record>(*rec);
                const LLVector3d shift = edit.mOp == ReplayEdit::SHIFT ? LLVector3d(0.5, 0.0, 0.0) : LLVector3d(0.0, 3.0, 0.0);
                copy->mOrigin += shift;
                copy->mMin += shift;
                copy->mMax += shift;
                changed = copy;
            }
            for (size_t m = 0; m < result.mMeshes.size(); ++m)
            {
                const ReplayMesh& mesh = *result.mMeshes[m];
                for (const auto& [key, tile] : mesh.mTiles)
                {
                    const TileBuildInput& before = *tile.mInput;
                    const bool held = std::find(before.mRecords.begin(), before.mRecords.end(), rec) != before.mRecords.end() ||
                                      std::find(before.mCuts.begin(), before.mCuts.end(), rec) != before.mCuts.end();
                    if (!held)
                    {
                        continue;
                    }
                    std::vector<RecordPtr> records;
                    for (const auto* list : { &before.mRecords, &before.mCuts })
                    {
                        for (const RecordPtr& r : *list)
                        {
                            if (r != rec)
                            {
                                records.push_back(r);
                            }
                            else if (edit.mOp == ReplayEdit::SHIFT)
                            {
                                records.push_back(changed);
                            }
                            else if (edit.mOp == ReplayEdit::COPY)
                            {
                                records.push_back(r);
                                records.push_back(changed);
                            }
                        }
                    }
                    TileBuildInput after = before;
                    after.mRecords.clear();
                    after.mCuts.clear();
                    finishTileInput(records, mesh.mOrigin, after);

                    // What the edit reaches, mesh-local in plan: before and after.
                    F32 x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
                    for (const RecordPtr& r : { rec, changed })
                    {
                        if (!r)
                        {
                            continue;
                        }
                        x0 = llmin(x0, static_cast<F32>(r->mMin.mdV[VX] - mesh.mOrigin.mdV[VX]));
                        y0 = llmin(y0, static_cast<F32>(r->mMin.mdV[VY] - mesh.mOrigin.mdV[VY]));
                        x1 = llmax(x1, static_cast<F32>(r->mMax.mdV[VX] - mesh.mOrigin.mdV[VX]));
                        y1 = llmax(y1, static_cast<F32>(r->mMax.mdV[VY] - mesh.mOrigin.mdV[VY]));
                    }
                    bool flight = false;
                    for (const auto& flights : tile.mResult->mFlights)
                    {
                        for (const Flight& f : flights)
                        {
                            flight = flight || boxTouchesFootprint(x0, y0, x1, y1, f.mFootprint);
                        }
                    }
                    bool exclusion = rec->mExclude != 0 || rec->mNavRole == NavRole::EXCLUSION_VOLUME;
                    for (const auto* list : { &before.mRecords, &before.mCuts })
                    {
                        for (const RecordPtr& r : *list)
                        {
                            if (r && r != rec && (r->mExclude || r->mNavRole == NavRole::EXCLUSION_VOLUME) &&
                                r->mMin.mdV[VX] - mesh.mOrigin.mdV[VX] <= x1 && r->mMax.mdV[VX] - mesh.mOrigin.mdV[VX] >= x0 &&
                                r->mMin.mdV[VY] - mesh.mOrigin.mdV[VY] <= y1 && r->mMax.mdV[VY] - mesh.mOrigin.mdV[VY] >= y0)
                            {
                                exclusion = true;
                            }
                        }
                    }
                    const auto t0 = Clock::now();
                    const TileBuildResult rebuilt = buildTile(after);
                    const F64 ms = since(t0);
                    out << (first ? "" : ",") << "\n{\"edit\":" << e << ",\"record\":" << edit.mRecord << ",\"op\":" << static_cast<S32>(edit.mOp)
                        << ",\"mesh\":" << m << ",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"zmin\":[" << before.mZMin << ',' << after.mZMin
                        << "],\"zmax\":[" << before.mZMax << ',' << after.mZMax << "],\"flight\":" << (flight ? 1 : 0) << ",\"exclusion\":" << (exclusion ? 1 : 0)
                        << ",\"rebuild_ms\":" << ms << ",\"base_ms\":" << tile.mResult->mMillis << ",\"status\":" << static_cast<S32>(rebuilt.mStatus) << '}';
                    first = false;
                }
            }
        }
        out << "\n]}\n";
    }
}
