/**
 * @file skairsoup.cpp
 * @brief One tile's triangle soup in SL axes from census records, exclusion volumes and terrain.
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

#include "skairsoup.h"
#include "skairstats.h"

#include "skcensusprofile.h"
#include "skcensusrecord.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace
{
    using namespace skworld;

    const F32 VOID_Z = -900.f;       ///< terrain samples below this have no region
    const F32 SNAP = 1024.f;         ///< vertices snap to 1/1024 m, exact in binary

    void tri(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c)
    {
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }

    // a, b, c, d counter-clockwise seen from outside.
    void quad(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
    {
        tri(out, a, b, c);
        tri(out, a, c, d);
    }

    /// Whether a triangle faces up within slope_deg of vertical; a double-sided one either way up, since the
    /// sim collides with a hull from either side and a down-facing top is still the floor.
    bool walkable(const F32* t, bool two_sided, F32 cos_slope)
    {
        const F32 e0[3] = { t[3] - t[0], t[4] - t[1], t[5] - t[2] };
        const F32 e1[3] = { t[6] - t[0], t[7] - t[1], t[8] - t[2] };
        const F32 nx = e0[1] * e1[2] - e0[2] * e1[1];
        const F32 ny = e0[2] * e1[0] - e0[0] * e1[2];
        const F32 nz = e0[0] * e1[1] - e0[1] * e1[0];
        const F32 up = nz * (1.f / std::sqrt(ny * ny + nz * nz + nx * nx));
        return (two_sided ? std::fabs(up) : up) > cos_slope;
    }

    /// The plan the tile's passes read, mesh-local: the tile padded by the widest of them.
    struct TileReach
    {
        F32 mX0, mY0, mX1, mY1;

        explicit TileReach(const TileInput& input)
        {
            const S32 border = std::max(borderCells(input.mAvatar.mRadius, input.mCell), input.mAirPad);
            const F32 pad = std::max(AIR_PAD_M, border * input.mCell) + input.mCell;
            mX0 = input.mTileX * input.mTileSize - pad;
            mY0 = input.mTileY * input.mTileSize - pad;
            mX1 = (input.mTileX + 1) * input.mTileSize + pad;
            mY1 = (input.mTileY + 1) * input.mTileSize + pad;
        }

        bool overlaps(const LLVector3* p, size_t n) const
        {
            F32 x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
            for (size_t i = 0; i < n; ++i)
            {
                x0 = std::min(x0, p[i].mV[VX]);
                y0 = std::min(y0, p[i].mV[VY]);
                x1 = std::max(x1, p[i].mV[VX]);
                y1 = std::max(y1, p[i].mV[VY]);
            }
            return x1 >= mX0 && x0 <= mX1 && y1 >= mY0 && y0 <= mY1;
        }
    };

    /// Drops what lies wholly outside the reach. A record is not clipped to its tile, so a big mesh would
    /// otherwise bring every triangle it has to every tile it touches. A solid's fill counts crossings of its
    /// whole surface, so a body is kept or dropped whole; bodies gets the triangle count of each kept one, and
    /// stays empty for a non-solid.
    void clipToReach(const Record& r, const TileReach& reach, std::vector<LLVector3>& tris, std::vector<S32>& bodies)
    {
        bodies.clear();
        size_t kept = 0;
        const auto keep = [&](size_t from, size_t count) {
            if (from != kept)
            {
                std::copy(tris.begin() + from, tris.begin() + from + count, tris.begin() + kept);
            }
            kept += count;
        };
        if (r.mSolid)
        {
            const S32 total = static_cast<S32>(tris.size() / 3);
            S32 pieces = 0;
            for (const S32 count : r.mConvexTris)
            {
                pieces += count;
            }
            // Bodies not told apart arrive as one range and are one closed surface.
            const std::vector<S32> whole{ total };
            const std::vector<S32>& counts = r.mConvexTris.size() > 1 && pieces == total ? r.mConvexTris : whole;
            size_t at = 0;
            for (const S32 count : counts)
            {
                const size_t n = static_cast<size_t>(count) * 3;
                if (count > 0 && reach.overlaps(&tris[at], n))
                {
                    keep(at, n);
                    bodies.push_back(count);
                }
                at += n;
            }
        }
        else
        {
            for (size_t at = 0; at + 2 < tris.size(); at += 3)
            {
                if (reach.overlaps(&tris[at], 3))
                {
                    keep(at, 3);
                }
            }
        }
        tris.resize(kept);
    }

    /// Append triangles, snapped to a grid of 1/1024 m. A float resolves a fraction of a millimetre at SL's
    /// coordinates, and a rotated mesh leaves a flat floor, or a wall on a cell edge, a step or so either side
    /// of where it is. On a cell boundary that noise rounds the floor up a cell in one tile and not in the next,
    /// and puts the wall in the column past its edge. Snapped, a flat surface is exactly flat and a wall on an
    /// edge is on it, in every tile. Returns per triangle whether it lies under the water.
    void addTris(std::vector<F32>& verts, const std::vector<LLVector3>& tris, F32 water, std::vector<U8>* under)
    {
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
        {
            bool below = true;
            for (S32 k = 0; k < 3; ++k)
            {
                const LLVector3& p = tris[i + k];
                verts.push_back(std::round(p.mV[VX] * SNAP) / SNAP);
                verts.push_back(std::round(p.mV[VY] * SNAP) / SNAP);
                verts.push_back(std::round(p.mV[VZ] * SNAP) / SNAP);
                below = below && p.mV[VZ] < water;
            }
            if (under)
            {
                under->push_back(below);
            }
        }
    }
}

namespace skworld
{
    bool buildAirSoup(const TileInput& input, AirSoup& out, TileAirResult& result, TileBuildStats* stats)
    {
        SK_WORLD_ZONE("air.soup");
        const auto started = std::chrono::steady_clock::now();
        out = AirSoup();
        const ClassParams& ap = input.mAvatar;
        const F32 avatar_cos = std::cos(ap.mSlope * DEG_TO_RAD);
        // The air stands on whatever AVATAR does, and more.
        const F32 top_cos = std::cos(llmax(input.mTopParams.mSlope, ap.mSlope) * DEG_TO_RAD);
        const F32 water = input.mTerrain.mWater;

        // The tile cells within two of an excluded record's box, for the stats.
        const S32 n = static_cast<S32>(input.mTileSize / input.mCell);
        const F32 tx0 = input.mTileX * input.mTileSize, ty0 = input.mTileY * input.mTileSize;
        std::vector<U8> window;
        U32 excluded = 0;

        std::vector<LLVector3> tris;
        std::vector<S32> bodies;
        std::vector<U8> under;
        const TileReach reach(input);
        for (const RecordPtr& r : input.mRecords)
        {
            if (!r)
            {
                continue;
            }
            tris.clear();
            emitRecord(*r, input.mMeshOrigin, tris);
            clipToReach(*r, reach, tris, bodies);
            if (tris.empty())
            {
                continue;
            }
            if (r->mExclude && stats)
            {
                // Each excluded record's box, two cells wider, over the tile's cells.
                ++excluded;
                window.resize(static_cast<size_t>(n) * n, 0);
                F32 lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
                for (const LLVector3& p : tris)
                {
                    for (S32 k = 0; k < 2; ++k)
                    {
                        lo[k] = llmin(lo[k], p.mV[k]);
                        hi[k] = llmax(hi[k], p.mV[k]);
                    }
                }
                const S32 ix0 = std::max(0, static_cast<S32>(std::floor((lo[0] - tx0) / input.mCell)) - 2);
                const S32 ix1 = std::min(n, static_cast<S32>(std::ceil((hi[0] - tx0) / input.mCell)) + 2);
                const S32 iy0 = std::max(0, static_cast<S32>(std::floor((lo[1] - ty0) / input.mCell)) - 2);
                const S32 iy1 = std::min(n, static_cast<S32>(std::ceil((hi[1] - ty0) / input.mCell)) + 2);
                for (S32 iy = iy0; iy < iy1; ++iy)
                {
                    std::fill(window.begin() + ix0 + iy * n, window.begin() + std::max(ix0, ix1) + iy * n, 1);
                }
            }
            // What no class builds with is not in the soup at all: a triangle given no area still rasterises as an obstacle.
            if (r->mExclude & EXCLUDE_SOLID)
            {
                continue;
            }
            const S32 before = out.ntris();
            under.clear();
            addTris(out.mVerts, tris, water, &under);
            // A hull is a solid the sim collides with from either side: walkable whichever way its triangles wind.
            const bool hull = r->mProvenance == Provenance::HULL || r->mProvenance == Provenance::BASE_HULL;
            const bool leaf = (r->mExclude & EXCLUDE_AVATAR) != 0;
            const auto cue_at = std::lower_bound(input.mRootCues.begin(), input.mRootCues.end(), std::make_pair(r->mRoot, U8(0)));
            const U8 cue = cue_at != input.mRootCues.end() && cue_at->first == r->mRoot ? cue_at->second : static_cast<U8>(CUE_NONE);
            const U8 flags = static_cast<U8>((leaf ? SOUP_LEAF : 0) | (cue << SOUP_CUE_SHIFT));
            for (S32 t = before; t < before + static_cast<S32>(under.size()); ++t)
            {
                // The area byte: AVATAR's floors over the air's TOP-only ones, so a merge that keeps the higher keeps AVATAR's.
                const F32* v = out.tri(t);
                U8 code = AREA_NONE;
                if (!leaf && walkable(v, hull, avatar_cos))
                {
                    code = AREA_OBJECT + AREA_TOP_ONLY;     // AVATAR never stands on what it leaves out
                }
                else if (walkable(v, hull, top_cos))
                {
                    code = AREA_OBJECT;
                }
                out.mCodes.push_back(code);
                out.mFlags.push_back(flags);
            }
            out.mLeaves = out.mLeaves || leaf;
            // A solid is filled. A decomposition is a set of bodies, not one: its hulls overlap, and a column
            // through two of them would count four crossings and fill none of the middle. Fill each body on its
            // own. A record whose bodies are not told apart arrives as one range and is filled as one closed surface.
            S32 at = before;
            for (const S32 count : bodies)
            {
                out.mSolids.emplace_back(at, at + count);
                at += count;
            }
        }
        // An exclusion volume is a cut, not a shape: its triangles never enter the soup, they only say where
        // the raster is taken out of.
        for (const RecordPtr& r : input.mCuts)
        {
            if (!r)
            {
                continue;
            }
            tris.clear();
            emitRecord(*r, input.mMeshOrigin, tris);
            const S32 before = out.ncuts();
            addTris(out.mCutVerts, tris, water, nullptr);
            if (out.ncuts() > before)
            {
                out.mCuts.emplace_back(before, out.ncuts());
            }
        }
        const TerrainWindow& t = input.mTerrain;
        if (t.mWidth >= 2 && t.mHeight >= 2 && t.mZ.size() >= static_cast<size_t>(t.mWidth * t.mHeight))
        {
            tris.clear();
            for (S32 y = 0; y + 1 < t.mHeight; ++y)
            {
                for (S32 x = 0; x + 1 < t.mWidth; ++x)
                {
                    // A sample below the void sentinel has no region under it: no floor there.
                    const F32 z00 = t.mZ[y * t.mWidth + x], z10 = t.mZ[y * t.mWidth + x + 1], z11 = t.mZ[(y + 1) * t.mWidth + x + 1],
                              z01 = t.mZ[(y + 1) * t.mWidth + x];
                    if (z00 < VOID_Z || z10 < VOID_Z || z11 < VOID_Z || z01 < VOID_Z)
                    {
                        continue;
                    }
                    const LLVector3 a(t.mOrigin.mV[VX] + x * t.mSpacing, t.mOrigin.mV[VY] + y * t.mSpacing, z00);
                    const LLVector3 b(t.mOrigin.mV[VX] + (x + 1) * t.mSpacing, t.mOrigin.mV[VY] + y * t.mSpacing, z10);
                    const LLVector3 c(t.mOrigin.mV[VX] + (x + 1) * t.mSpacing, t.mOrigin.mV[VY] + (y + 1) * t.mSpacing, z11);
                    const LLVector3 d(t.mOrigin.mV[VX] + x * t.mSpacing, t.mOrigin.mV[VY] + (y + 1) * t.mSpacing, z01);
                    quad(tris, a, b, c, d);     // counter-clockwise from above
                }
            }
            const S32 before = out.ntris();
            under.clear();
            addTris(out.mVerts, tris, t.mWater, &under);
            for (size_t i = 0; i < under.size(); ++i)
            {
                // Land is single-sided; under the water it is SUBMERGED.
                const U8 area = under[i] ? AREA_SUBMERGED : AREA_GROUND;
                const F32* v = out.tri(before + static_cast<S32>(i));
                const U8 code = walkable(v, false, avatar_cos) ? static_cast<U8>(area + AREA_TOP_ONLY) : walkable(v, false, top_cos) ? area : AREA_NONE;
                out.mCodes.push_back(code);
                out.mFlags.push_back(0);
            }
        }
        result.mTriangles = out.ntris();
        if (stats)
        {
            stats->mSoupMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
            stats->mExcludedRecords = excluded;
            stats->mExcludeWindowCells = static_cast<U32>(std::count(window.begin(), window.end(), 1));
            stats->mTileCells = static_cast<U32>(n) * n;
            ClassBuildStats& astats = stats->mClass[0];
            astats.mBuilt = true;
            std::vector<U8> used(256, 0);
            for (S32 i = 0; i < out.ntris(); ++i)
            {
                used[out.mCodes[i]] = out.mCodes[i] >= AREA_CODE_BASE;
            }
            astats.mCodes = static_cast<U32>(std::count(used.begin(), used.end(), 1));
            astats.mMs[ClassBuildStats::SOUP] += stats->mSoupMs;
        }
        if (out.ntris() == 0)
        {
            result.mStatus = TileAirResult::EMPTY;
            return false;
        }
        return true;
    }
}
