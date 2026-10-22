/**
 * @file skairraster.cpp
 * @brief One navmesh tile from census records and terrain: the Recast pipeline, pure and thread-free.
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

#include "Recast.h"
#include "RecastAlloc.h"
#include "llapp.h"
#include "llstring.h"
#include "skairfaceoffsets.h"
#include "skairsoup.h"
#include "skairwalls.h"
#include "skcensusprofile.h"
#include "skcensusrecord.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace
{
    using namespace skworld;

    const F32 VOID_Z = -900.f;      ///< terrain samples below this have no region

    /// The soup in Recast space (x = SL y, y = SL z, z = SL x), triangles three vertices each, for Recast's raster.
    struct Soup
    {
        std::vector<F32> mVerts;
        std::vector<S32> mTris;

        void assign(const std::vector<F32>& sl)
        {
            mVerts.resize(sl.size());
            for (size_t i = 0; i + 2 < sl.size(); i += 3)
            {
                toRecast(LLVector3(&sl[i]), &mVerts[i]);
            }
            mTris.resize(sl.size() / 3);
            for (size_t i = 0; i < mTris.size(); ++i)
            {
                mTris[i] = static_cast<S32>(i);
            }
        }
        S32 nverts() const { return static_cast<S32>(mVerts.size() / 3); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

    /// The height range a closed body covers in each column of its footprint:
    /// mLo and mHi, indexed from (mX0, mZ0) over mW by mH cells, FLT_MAX and
    /// -FLT_MAX where the body does not pass over a column's centre.
    struct BodyExtent
    {
        S32 mX0 = 0;
        S32 mZ0 = 0;
        S32 mW = 0;
        S32 mH = 0;
        std::vector<F32> mLo;
        std::vector<F32> mHi;
    };

    void bodyExtent(const rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, BodyExtent& out, F32 pad = 0.f)
    {
        out = BodyExtent();
        if (tri_end <= tri_begin)
        {
            return;
        }
        // The columns this body covers, clipped to the tile.
        F32 bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                bmin[k] = std::min(bmin[k], v[k]);
                bmax[k] = std::max(bmax[k], v[k]);
            }
        }
        bmin[0] -= pad;
        bmin[2] -= pad;
        bmax[0] += pad;
        bmax[2] += pad;
        const S32 x0 = std::max(0, static_cast<S32>(std::floor((bmin[0] - hf.bmin[0]) / hf.cs)));
        const S32 x1 = std::min(hf.width - 1, static_cast<S32>(std::floor((bmax[0] - hf.bmin[0]) / hf.cs)));
        const S32 z0 = std::max(0, static_cast<S32>(std::floor((bmin[2] - hf.bmin[2]) / hf.cs)));
        const S32 z1 = std::min(hf.height - 1, static_cast<S32>(std::floor((bmax[2] - hf.bmin[2]) / hf.cs)));
        if (x1 < x0 || z1 < z0)
        {
            return;
        }
        out.mX0 = x0;
        out.mZ0 = z0;
        out.mW = x1 - x0 + 1;
        out.mH = z1 - z0 + 1;
        out.mLo.assign(static_cast<size_t>(out.mW) * out.mH, FLT_MAX);
        out.mHi.assign(static_cast<size_t>(out.mW) * out.mH, -FLT_MAX);

        for (S32 t = tri_begin; t < tri_end; ++t)
        {
            const F32* a = &soup.mVerts[soup.mTris[t * 3] * 3];
            const F32* b = &soup.mVerts[soup.mTris[t * 3 + 1] * 3];
            const F32* c = &soup.mVerts[soup.mTris[t * 3 + 2] * 3];
            // Its footprint, and its plane, in the xz ground plane.
            const F32 d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-9f)
            {
                continue;   // edge on: it adds nothing a neighbouring face does not
            }
            const S32 tx0 = std::max(x0, static_cast<S32>(std::floor((std::min(std::min(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tx1 = std::min(x1, static_cast<S32>(std::floor((std::max(std::max(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tz0 = std::max(z0, static_cast<S32>(std::floor((std::min(std::min(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            const S32 tz1 = std::min(z1, static_cast<S32>(std::floor((std::max(std::max(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            for (S32 cz = tz0; cz <= tz1; ++cz)
            {
                for (S32 cx = tx0; cx <= tx1; ++cx)
                {
                    const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;
                    const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;
                    const F32 l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    const F32 l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    const F32 l2 = 1.f - l0 - l1;
                    if (l0 < -pad || l1 < -pad || l2 < -pad)
                    {
                        continue;
                    }
                    const F32 y = l0 * a[1] + l1 * b[1] + l2 * c[1];
                    const size_t i = static_cast<size_t>(cz - z0) * out.mW + (cx - x0);
                    out.mLo[i] = std::min(out.mLo[i], y);
                    out.mHi[i] = std::max(out.mHi[i], y);
                }
            }
        }
    }

    /// Recast rasterises surfaces, so a closed body arrives hollow: the floor
    /// it stands on stays walkable inside it with the body's own roof as
    /// headroom, and the navmesh offers a storey inside every solid object.
    /// Every body filled here is one the sim collides with as a volume - a
    /// prim, a hull, a sculpt's hull - so its inside is filled whatever it
    /// holds: a surface in there is another record's, or the terrain, and no
    /// avatar stands on it.
    ///
    /// The inside is what the column's centre crosses an odd number of
    /// surfaces to reach, so the crossings are taken in pairs from the bottom
    /// up. A convex body is one pair. A box with a tunnel through it is two,
    /// and the tunnel's floor stays a floor; a hollow prim's hollow is open
    /// the same way. Facing is never consulted: a hull's winding is whatever
    /// the decomposition left it, and the count is the same either way.
    /// A column the body only touches is odd, and is filled from its lowest
    /// crossing to its highest, which is what a convex body would have been.
    void fillSolid(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, S32 climb)
    {
        SK_WORLD_ZONE("air.raster.rasterise.fill");
        if (tri_end <= tri_begin)
        {
            return;
        }
        F32 bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                bmin[k] = std::min(bmin[k], v[k]);
                bmax[k] = std::max(bmax[k], v[k]);
            }
        }
        const S32 x0 = std::max(0, static_cast<S32>(std::floor((bmin[0] - hf.bmin[0]) / hf.cs)));
        const S32 x1 = std::min(hf.width - 1, static_cast<S32>(std::floor((bmax[0] - hf.bmin[0]) / hf.cs)));
        const S32 z0 = std::max(0, static_cast<S32>(std::floor((bmin[2] - hf.bmin[2]) / hf.cs)));
        const S32 z1 = std::min(hf.height - 1, static_cast<S32>(std::floor((bmax[2] - hf.bmin[2]) / hf.cs)));
        if (x1 < x0 || z1 < z0)
        {
            return;
        }
        const S32 w = x1 - x0 + 1;

        // Every surface crossing, by column then height.
        struct Hit
        {
            S32 mColumn;
            F32 mY;
            bool operator<(const Hit& o) const { return mColumn < o.mColumn || (mColumn == o.mColumn && mY < o.mY); }
        };
        std::vector<Hit> hits;
        // A centre on a shared edge is inside both triangles; the two crossings
        // it yields sit at one height and are merged below. The tolerance keeps
        // a centre a rounding error outside both from being in neither.
        const F32 eps = 1e-4f;
        for (S32 t = tri_begin; t < tri_end; ++t)
        {
            const F32* a = &soup.mVerts[soup.mTris[t * 3] * 3];
            const F32* b = &soup.mVerts[soup.mTris[t * 3 + 1] * 3];
            const F32* c = &soup.mVerts[soup.mTris[t * 3 + 2] * 3];
            const F32 d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-9f)
            {
                continue;   // vertical: the column crosses it nowhere; the faces it joins count
            }
            const S32 tx0 = std::max(x0, static_cast<S32>(std::floor((std::min(std::min(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tx1 = std::min(x1, static_cast<S32>(std::floor((std::max(std::max(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tz0 = std::max(z0, static_cast<S32>(std::floor((std::min(std::min(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            const S32 tz1 = std::min(z1, static_cast<S32>(std::floor((std::max(std::max(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            for (S32 cz = tz0; cz <= tz1; ++cz)
            {
                for (S32 cx = tx0; cx <= tx1; ++cx)
                {
                    const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;
                    const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;
                    const F32 l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    const F32 l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    const F32 l2 = 1.f - l0 - l1;
                    if (l0 < -eps || l1 < -eps || l2 < -eps)
                    {
                        continue;
                    }
                    hits.push_back({ (cz - z0) * w + (cx - x0), l0 * a[1] + l1 * b[1] + l2 * c[1] });
                }
            }
        }
        std::sort(hits.begin(), hits.end());

        rcContext ctx(false);
        auto fill = [&](S32 column, F32 lo, F32 hi)
        {
            const F32 top = std::min(hi, hf.bmax[1]);
            const F32 bottom = std::max(lo, hf.bmin[1]);
            if (top <= bottom)
            {
                return;
            }
            // Spans are cell counts in an unsigned short and Recast caps
            // them; a megaprim reaching a kilometre would otherwise wrap.
            S32 smin = static_cast<S32>(std::floor((bottom - hf.bmin[1]) / hf.ch));
            S32 smax = static_cast<S32>(std::ceil((top - hf.bmin[1]) / hf.ch));
            smin = llclamp(smin, 0, RC_SPAN_MAX_HEIGHT);
            smax = llclamp(smax, 0, RC_SPAN_MAX_HEIGHT);
            if (smax <= smin)
            {
                return;
            }
            // The fill carries no area: it is the inside of something, never
            // a floor. Merging keeps the surfaces' own area, so the body's
            // roof stays walkable.
            rcAddSpan(&ctx, hf, x0 + column % w, z0 + column / w, static_cast<unsigned short>(smin),
                      static_cast<unsigned short>(smax), RC_NULL_AREA, climb);
        };
        // The same surface met at a shared edge or vertex is one crossing.
        const F32 same = 1e-3f;
        std::vector<F32> column;
        size_t i = 0;
        while (i < hits.size())
        {
            const S32 at = hits[i].mColumn;
            column.clear();
            for (; i < hits.size() && hits[i].mColumn == at; ++i)
            {
                if (column.empty() || hits[i].mY - column.back() > same)
                {
                    column.push_back(hits[i].mY);
                }
            }
            if (column.size() % 2)
            {
                fill(at, column.front(), column.back());
                continue;
            }
            for (size_t k = 0; k + 1 < column.size(); k += 2)
            {
                fill(at, column[k], column[k + 1]);
            }
        }
    }

    /// Take the navmesh out of a volume. The sim's exclusion volumes are holes
    /// in its own mesh, so they are never a surface to stand on: every walkable
    /// span that overlaps the volume in a column it covers is cleared.
    void cutVolume(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end)
    {
        SK_WORLD_ZONE("air.raster.rasterise.cut");
        BodyExtent body;
        bodyExtent(hf, soup, tri_begin, tri_end, body);
        for (S32 cz = body.mZ0; cz < body.mZ0 + body.mH; ++cz)
        {
            for (S32 cx = body.mX0; cx < body.mX0 + body.mW; ++cx)
            {
                const size_t i = static_cast<size_t>(cz - body.mZ0) * body.mW + (cx - body.mX0);
                if (body.mLo[i] > body.mHi[i])
                {
                    continue;
                }
                const S32 smin = static_cast<S32>(std::floor((body.mLo[i] - hf.bmin[1]) / hf.ch));
                const S32 smax = static_cast<S32>(std::ceil((body.mHi[i] - hf.bmin[1]) / hf.ch));
                for (rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
                {
                    if (s->area != RC_NULL_AREA && static_cast<S32>(s->smin) < smax && static_cast<S32>(s->smax) > smin)
                    {
                        s->area = RC_NULL_AREA;
                    }
                }
            }
        }
    }

    /// A rectangle of heightfield cells, half-open.
    struct CellRect
    {
        S32 mX0 = 0;
        S32 mZ0 = 0;
        S32 mX1 = 0;
        S32 mZ1 = 0;
        bool covers(const rcHeightfield& hf) const { return mX0 <= 0 && mZ0 <= 0 && mX1 >= hf.width && mZ1 >= hf.height; }
    };

    /// What a raster is made of: the soup with an area code per triangle, its
    /// closed bodies, the exclusion volumes cut out of it, and optionally the
    /// triangles to leave out.
    struct RasterSource
    {
        const Soup* mSoup = nullptr;
        const std::vector<U8>* mCodes = nullptr;
        const std::vector<std::pair<S32, S32>>* mSolids = nullptr;
        const Soup* mCutSoup = nullptr;
        const std::vector<std::pair<S32, S32>>* mCuts = nullptr;
        const std::vector<U8>* mDrop = nullptr;
    };

    /// The cells a triangle range's bounding box covers, a cell wider each way.
    CellRect cellsOf(const rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end)
    {
        F32 lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
        }
        return { static_cast<S32>(std::floor((lo[0] - hf.bmin[0]) / hf.cs)) - 1, static_cast<S32>(std::floor((lo[2] - hf.bmin[2]) / hf.cs)) - 1,
                 static_cast<S32>(std::floor((hi[0] - hf.bmin[0]) / hf.cs)) + 2, static_cast<S32>(std::floor((hi[2] - hf.bmin[2]) / hf.cs)) + 2 };
    }

    bool overlaps(const CellRect& a, const CellRect& b)
    {
        return a.mX0 < b.mX1 && b.mX0 < a.mX1 && a.mZ0 < b.mZ1 && b.mZ0 < a.mZ1;
    }

    /// Rasterise, fill and cut, in that order, every triangle, body and cut of
    /// the source that touches the rectangle. The columns inside it come out
    /// exactly as a raster of the whole source would make them, provided the
    /// heightfield has the same grid; columns outside it are incomplete.
    bool rasterisePasses(rcHeightfield& hf, const RasterSource& src, const CellRect& rect, S32 climb, StageClock& clock)
    {
        SK_WORLD_ZONE("air.raster.rasterise.passes");
        rcContext ctx(false);
        const Soup& soup = *src.mSoup;
        const bool whole = rect.covers(hf);
        if (whole && !src.mDrop)
        {
            if (!rcRasterizeTriangles(&ctx, soup.mVerts.data(), soup.nverts(), soup.mTris.data(), src.mCodes->data(), soup.ntris(), hf, climb))
            {
                return false;
            }
        }
        else
        {
            std::vector<S32> tris;
            std::vector<U8> codes;
            for (S32 t = 0; t < soup.ntris(); ++t)
            {
                if ((src.mDrop && (*src.mDrop)[t]) || (!whole && !overlaps(rect, cellsOf(hf, soup, t, t + 1))))
                {
                    continue;
                }
                tris.insert(tris.end(), soup.mTris.begin() + t * 3, soup.mTris.begin() + t * 3 + 3);
                codes.push_back((*src.mCodes)[t]);
            }
            if (!tris.empty() && !rcRasterizeTriangles(&ctx, soup.mVerts.data(), soup.nverts(), tris.data(), codes.data(),
                                                       static_cast<S32>(codes.size()), hf, climb))
            {
                return false;
            }
        }
        clock.lap(ClassBuildStats::RASTERISE);
        for (const auto& range : *src.mSolids)
        {
            if ((src.mDrop && range.first < range.second && (*src.mDrop)[range.first]) ||
                (!whole && !overlaps(rect, cellsOf(hf, soup, range.first, range.second))))
            {
                continue;
            }
            fillSolid(hf, soup, range.first, range.second, climb);
        }
        clock.lap(ClassBuildStats::FILL);
        for (const auto& range : *src.mCuts)
        {
            cutVolume(hf, *src.mCutSoup, range.first, range.second);
        }
        clock.lap(ClassBuildStats::CUT);
        return true;
    }

    /// A span of a surface only the air's slope walks becomes none, as
    /// though AVATAR had rasterised it unwalkable.
    void maskTopOnly(rcHeightfield& hf)
    {
        for (S32 c = 0; c < hf.width * hf.height; ++c)
        {
            for (rcSpan* s = hf.spans[c]; s; s = s->next)
            {
                if (s->area < AREA_CODE_BASE)
                {
                    s->area = RC_NULL_AREA;
                }
            }
        }
    }

    /// The real area of every area code.
    std::vector<U8> realAreas()
    {
        std::vector<U8> out(AREA_CODE_MAX + 1, RC_NULL_AREA);
        for (S32 c = 1; c <= AREA_CODE_MAX; ++c)
        {
            out[c] = areaOfCode(static_cast<U8>(c));
        }
        return out;
    }

    /// A walkable span in the column already within the agent's climb of both
    /// surfaces: they are joined through it and need no bridge.
    bool columnJoins(const rcHeightfield& hf, S32 x, S32 z, S32 lo, S32 hi, S32 climb, U8 min_code)
    {
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            if (s->area >= min_code && rcAbs(static_cast<S32>(s->smax) - lo) <= climb && rcAbs(static_cast<S32>(s->smax) - hi) <= climb)
            {
                return true;
            }
        }
        return false;
    }

    /// Matter that stands in the way of joining two surfaces at lo and hi: a
    /// span that reaches above the lower surface and starts at or below the
    /// higher one. Ground and slabs below the seam are not in the way, so a
    /// crevice on an upper storey still joins; a wall face, a filled body or a
    /// riser is, so the wall's inside cannot tunnel to its outside.
    bool creviceBlocked(const rcHeightfield& hf, S32 x, S32 z, S32 lo, S32 hi)
    {
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            if (static_cast<S32>(s->smax) > lo && static_cast<S32>(s->smin) <= hi)
            {
                return true;
            }
        }
        return false;
    }

    /// A span to lay across a crevice, in the cells of the heightfield it was found on.
    struct Bridge
    {
        S32 mX = 0;
        S32 mZ = 0;
        unsigned short mSMin = 0;
        unsigned short mSMax = 0;
        U8 mArea = RC_NULL_AREA;

        bool operator==(const Bridge& o) const
        {
            return mX == o.mX && mZ == o.mZ && mSMin == o.mSMin && mSMax == o.mSMax && mArea == o.mArea;
        }
    };

    /// Recast joins a span to its neighbours only, so a crevice that leaves a
    /// column without a span at the surfaces' level splits them, and the
    /// erosion that follows widens it: the top of a staircase, solid in the
    /// sim, read as a gap. Find where to join the two sides across one such
    /// column, when their tops are within the agent's climb and nothing stands
    /// at that level between them; a class walks the spans of min_code and up.
    /// Nothing is added here: a span laid while scanning would be read as a
    /// neighbour by a later column and by the column's other axis, which must
    /// judge the heightfield they were given.
    std::vector<Bridge> findBridges(const rcHeightfield& hf, S32 climb, U8 min_code)
    {
        SK_WORLD_ZONE("air.raster.bridges");
        std::vector<Bridge> bridges;
        for (S32 z = 1; z + 1 < hf.height; ++z)
        {
            for (S32 x = 1; x + 1 < hf.width; ++x)
            {
                for (S32 axis = 0; axis < 2; ++axis)
                {
                    const S32 dx = axis == 0 ? 1 : 0;
                    const S32 dz = axis == 0 ? 0 : 1;
                    for (const rcSpan* a = hf.spans[(x - dx) + (z - dz) * hf.width]; a; a = a->next)
                    {
                        if (a->area < min_code)
                        {
                            continue;
                        }
                        for (const rcSpan* b = hf.spans[(x + dx) + (z + dz) * hf.width]; b; b = b->next)
                        {
                            if (b->area < min_code)
                            {
                                continue;
                            }
                            const S32 atop = a->smax, btop = b->smax;
                            if (rcAbs(atop - btop) > climb)
                            {
                                continue;
                            }
                            const S32 lo = std::min(atop, btop), hi = std::max(atop, btop);
                            if (columnJoins(hf, x, z, lo, hi, climb, min_code) || creviceBlocked(hf, x, z, lo, hi))
                            {
                                continue;
                            }
                            // The bridge's surface is the higher one, so no
                            // side is lifted; its floor reaches down to the
                            // lower, so both sides connect.
                            const S32 bottom = hi > lo ? lo : hi - 1;
                            Bridge bridge;
                            bridge.mX = x;
                            bridge.mZ = z;
                            bridge.mSMin = static_cast<unsigned short>(std::max(0, bottom));
                            bridge.mSMax = static_cast<unsigned short>(std::max(hi, bottom + 1));
                            bridge.mArea = std::max(a->area, b->area);
                            bridges.push_back(bridge);
                        }
                    }
                }
            }
        }
        return bridges;
    }

    /// Lay bridges found on a heightfield whose cells sit (dx, dz) inside this one.
    void addBridges(rcHeightfield& hf, const std::vector<Bridge>& bridges, S32 climb, S32 dx = 0, S32 dz = 0)
    {
        rcContext ctx(false);
        for (const Bridge& bridge : bridges)
        {
            rcAddSpan(&ctx, hf, bridge.mX + dx, bridge.mZ + dz, bridge.mSMin, bridge.mSMax, bridge.mArea, climb);
        }
    }

    /// How many bridges of one list the other lacks, counting repeats.
    U32 bridgesMissing(const std::vector<Bridge>& from, const std::vector<Bridge>& in)
    {
        std::vector<Bridge> rest = in;
        U32 missing = 0;
        for (const Bridge& b : from)
        {
            const auto it = std::find(rest.begin(), rest.end(), b);
            if (it == rest.end())
            {
                ++missing;
            }
            else
            {
                rest.erase(it);
            }
        }
        return missing;
    }

    /// The land's surface as a span index at one heightfield cell, or -1 where
    /// the terrain window has no say: outside it, or in the void past a
    /// region's edge. Interpolated on the same two triangles per sample quad
    /// the land is rasterised with, so the surface the cull reads is the one
    /// the build put down.
    S32 groundIndex(const rcHeightfield& hf, const TerrainWindow& t, S32 cx, S32 cz)
    {
        const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;    // Recast x runs along SL y
        const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;    // and Recast z along SL x
        const F32 gx = (pz - t.mOrigin.mV[VX]) / t.mSpacing;
        const F32 gy = (px - t.mOrigin.mV[VY]) / t.mSpacing;
        if (gx < 0.f || gy < 0.f || gx > t.mWidth - 1.f || gy > t.mHeight - 1.f)
        {
            return -1;
        }
        const S32 i0 = llclamp(static_cast<S32>(gx), 0, t.mWidth - 2);
        const S32 j0 = llclamp(static_cast<S32>(gy), 0, t.mHeight - 2);
        const F32 u = llclamp(gx - i0, 0.f, 1.f);
        const F32 v = llclamp(gy - j0, 0.f, 1.f);
        const F32 z00 = t.mZ[j0 * t.mWidth + i0], z10 = t.mZ[j0 * t.mWidth + i0 + 1];
        const F32 z01 = t.mZ[(j0 + 1) * t.mWidth + i0], z11 = t.mZ[(j0 + 1) * t.mWidth + i0 + 1];
        if (z00 < VOID_Z || z10 < VOID_Z || z01 < VOID_Z || z11 < VOID_Z)
        {
            return -1;
        }
        const F32 z = u > v ? z00 + u * (z10 - z00) + v * (z11 - z10)
                            : z00 + v * (z01 - z00) + u * (z11 - z01);
        return llclamp(static_cast<S32>(std::floor((z - hf.bmin[1]) / hf.ch)), 0, RC_SPAN_MAX_HEIGHT);
    }

    /// Nothing legitimate lies below the land: the terrain surface is the
    /// ground, the sim keeps every agent on or above it, and a prim buried in
    /// it is out of reach. A span wholly under the surface at its column is
    /// removed, and one that reaches through it is trimmed to start there, so
    /// no floor, wall face or drop is ever read off the buried part. Columns
    /// the terrain window does not cover - the void past a region's edge, a
    /// mesh clear of the ground - keep everything.
    void cullUnderground(rcHeightfield& hf, const TerrainWindow& t)
    {
        SK_WORLD_ZONE("air.raster.cull");
        if (t.mWidth < 2 || t.mHeight < 2 || t.mZ.size() < static_cast<size_t>(t.mWidth * t.mHeight))
        {
            return;
        }
        for (S32 cz = 0; cz < hf.height; ++cz)
        {
            for (S32 cx = 0; cx < hf.width; ++cx)
            {
                rcSpan** link = &hf.spans[cx + cz * hf.width];
                if (!*link)
                {
                    continue;
                }
                const S32 ground = groundIndex(hf, t, cx, cz);
                if (ground < 0)
                {
                    continue;
                }
                while (*link)
                {
                    rcSpan* s = *link;
                    if (static_cast<S32>(s->smax) < ground)
                    {
                        *link = s->next;    // wholly underground
                        continue;
                    }
                    if (static_cast<S32>(s->smin) < ground)
                    {
                        s->smin = static_cast<unsigned short>(ground);  // the earth fills below the surface
                    }
                    link = &s->next;
                }
            }
        }
    }

    /// The heightfield's spans and the bytes its column heads and span pools hold.
    void measureHeightfield(const rcHeightfield& hf, ClassBuildStats& out)
    {
        out.mHfSpans = 0;
        out.mHfColumns = 0;
        out.mHfMaxSpans = 0;
        for (S32 c = 0; c < hf.width * hf.height; ++c)
        {
            U32 n = 0;
            for (const rcSpan* s = hf.spans[c]; s; s = s->next)
            {
                ++n;
            }
            out.mHfSpans += n;
            out.mHfColumns += n ? 1 : 0;
            out.mHfMaxSpans = std::max(out.mHfMaxSpans, n);
        }
        out.mHfBytes = static_cast<size_t>(hf.width) * hf.height * sizeof(rcSpan*);
        for (const rcSpanPool* pool = hf.pools; pool; pool = pool->next)
        {
            out.mHfBytes += sizeof(rcSpanPool);
        }
    }

    /// A digest of the spans in a cell rectangle, relative to its corner, with
    /// each area passed through a lookup; empty lookup keeps only walkability.
    U64 hashHeightfield(const rcHeightfield& hf, S32 x0, S32 z0, S32 x1, S32 z1, const std::vector<U8>& lut)
    {
        U64 h = 0x9e3779b97f4a7c15ull;
        for (S32 z = z0; z < z1; ++z)
        {
            for (S32 x = x0; x < x1; ++x)
            {
                h = mix(h, static_cast<U64>((x - x0) | (z - z0) << 16));
                for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
                {
                    const U8 a = lut.empty() ? (s->area != RC_NULL_AREA) : lut[s->area];
                    h = mix(h, static_cast<U64>(s->smin) | static_cast<U64>(s->smax) << 16 | static_cast<U64>(a) << 32);
                }
            }
        }
        return h;
    }

    /// A heightfield over a rectangle of other heightfields' cells, sharing their
    /// spans rather than copying them: Recast's filters and compaction run on
    /// it exactly as on a heightfield of that size, edges and all, and what
    /// they change they change in the spans underneath.
    struct CropView
    {
        rcHeightfield mHf;
        S32 mInset = 0;

        CropView(const rcConfig& cfg, S32 inset) : mInset(inset)
        {
            mHf.width = cfg.width;
            mHf.height = cfg.height;
            rcVcopy(mHf.bmin, cfg.bmin);
            rcVcopy(mHf.bmax, cfg.bmax);
            mHf.cs = cfg.cs;
            mHf.ch = cfg.ch;
            mHf.spans = static_cast<rcSpan**>(rcAlloc(sizeof(rcSpan*) * cfg.width * cfg.height, RC_ALLOC_PERM));
        }
        CropView(const CropView&) = delete;
        CropView& operator=(const CropView&) = delete;

        /// Point every column at base's, or at over's where own marks it; again after either changes.
        void point(const rcHeightfield& base, const rcHeightfield* over = nullptr, const std::vector<U8>* own = nullptr)
        {
            for (S32 z = 0; z < mHf.height; ++z)
            {
                for (S32 x = 0; x < mHf.width; ++x)
                {
                    const S32 c = (x + mInset) + (z + mInset) * base.width;
                    mHf.spans[x + z * mHf.width] = over && (*own)[c] ? over->spans[c] : base.spans[c];
                }
            }
        }
    };

    /// Every span's area, column by column, to put back after a pass that rewrites them.
    struct AreaSnapshot
    {
        std::vector<U8> mAreas;
        std::vector<U32> mStart;    ///< per column, where its areas begin

        void take(const rcHeightfield& hf)
        {
            mAreas.clear();
            mStart.assign(1, 0);
            for (S32 c = 0; c < hf.width * hf.height; ++c)
            {
                for (const rcSpan* s = hf.spans[c]; s; s = s->next)
                {
                    mAreas.push_back(static_cast<U8>(s->area));
                }
                mStart.push_back(static_cast<U32>(mAreas.size()));
            }
        }

        /// Every column's areas back, or only those of the columns marked.
        void restore(rcHeightfield& hf, const std::vector<U8>* only = nullptr) const
        {
            for (S32 c = 0; c < hf.width * hf.height; ++c)
            {
                if (only && !(*only)[c])
                {
                    continue;
                }
                U32 i = mStart[c];
                for (rcSpan* s = hf.spans[c]; s; s = s->next)
                {
                    llassert(i < mStart[c + 1]);
                    s->area = mAreas[i++];
                }
                llassert(i == mStart[c + 1]);
            }
        }
    };

    /// A copy of one column's spans into the same column of another heightfield.
    void copyColumn(const rcHeightfield& from, rcHeightfield& to, S32 x, S32 z)
    {
        rcContext ctx(false);
        for (const rcSpan* s = from.spans[x + z * from.width]; s; s = s->next)
        {
            rcAddSpan(&ctx, to, x, z, static_cast<unsigned short>(s->smin), static_cast<unsigned short>(s->smax), static_cast<U8>(s->area), 0);
        }
    }

    /// Recast's three span filters for one agent: the ledge filter's climb
    /// follows the agent's slope, since at its own climb it reads every slope
    /// steeper than atan(climb / two cells) as a ledge - 72 degrees at 0.75 m,
    /// below either slope limit - and cuts lines and holes out of exactly the
    /// steep ground the agent walks. Matched to the slope, it only fires past
    /// the limit, where the slope test has already said no.
    /// The ledge and headroom filters only clear areas, on tests that read no
    /// area but the span's own, so in either order they clear the same spans;
    /// before_ledges keeps the areas between them.
    void filterSpans(rcHeightfield& hf, const ClassParams& p, F32 ch, F32 cs, AreaSnapshot* before_ledges = nullptr)
    {
        SK_WORLD_ZONE("air.raster.filters");
        rcContext ctx(false);
        const S32 climb = static_cast<S32>(std::floor(p.mClimb / ch));
        const S32 height = static_cast<S32>(std::ceil(p.mHeight / ch));
        const F32 ledge_climb = llmax(p.mClimb, 2.f * cs * std::tan(p.mSlope * DEG_TO_RAD));
        rcFilterLowHangingWalkableObstacles(&ctx, climb, hf);
        if (before_ledges)
        {
            rcFilterWalkableLowHeightSpans(&ctx, height, hf);
            before_ledges->take(hf);
            rcFilterLedgeSpans(&ctx, height, static_cast<int>(std::ceil(ledge_climb / ch)), hf);
            return;
        }
        rcFilterLedgeSpans(&ctx, height, static_cast<int>(std::ceil(ledge_climb / ch)), hf);
        rcFilterWalkableLowHeightSpans(&ctx, height, hf);
    }

    struct FreeHeightfield
    {
        void operator()(rcHeightfield* hf) const { rcFreeHeightField(hf); }
    };
    typedef std::unique_ptr<rcHeightfield, FreeHeightfield> HeightfieldPtr;

    rcHeightfield* makeHeightfield(const rcConfig& cfg)
    {
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        if (hf && !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        {
            rcFreeHeightField(hf);
            return nullptr;
        }
        return hf;
    }

    /// The tile's raster grid for an agent, with border cells of padding each side.
    rcConfig tileConfig(const TileInput& input, const ClassParams& p, S32 border)
    {
        rcConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.cs = input.mCell;
        cfg.ch = input.mCell;
        cfg.walkableSlopeAngle = p.mSlope;
        cfg.walkableHeight = static_cast<int>(std::ceil(p.mHeight / cfg.ch));
        cfg.walkableClimb = static_cast<int>(std::floor(p.mClimb / cfg.ch));
        cfg.walkableRadius = static_cast<int>(std::ceil(p.mRadius / cfg.cs));
        cfg.borderSize = border;
        cfg.tileSize = static_cast<int>(input.mTileSize / cfg.cs);
        cfg.width = cfg.tileSize + cfg.borderSize * 2;
        cfg.height = cfg.width;

        // The tile in Recast space: x = SL y, z = SL x, y = SL z.
        const F32 x0 = input.mTileX * input.mTileSize, y0 = input.mTileY * input.mTileSize;
        cfg.bmin[0] = y0 - cfg.borderSize * cfg.cs;
        // Snapped to the cell grid, not to this tile's own lowest point:
        // heights quantise to ch from bmin, so two tiles that started
        // from different z put the same floor on different spans, and the
        // two sides of a seam end up at slightly different heights.
        cfg.bmin[1] = std::floor((input.mZMin - cfg.ch) / cfg.ch) * cfg.ch;
        cfg.bmin[2] = x0 - cfg.borderSize * cfg.cs;
        cfg.bmax[0] = y0 + input.mTileSize + cfg.borderSize * cfg.cs;
        cfg.bmax[1] = std::ceil((input.mZMax + cfg.ch) / cfg.ch) * cfg.ch;
        cfg.bmax[2] = x0 + input.mTileSize + cfg.borderSize * cfg.cs;
        return cfg;
    }

    struct Freed
    {
        rcHeightfield* hf = nullptr;
        ~Freed()
        {
            rcFreeHeightField(hf);
        }
    };
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
        /// What the raster stage makes of a tile before its cell matters: the soup and each triangle's area code.
        struct TileSoup
        {
            AirSoup mAir;
            Soup mSoup;                                     ///< mAir's, in Recast space
            Soup mCutSoup;
            std::vector<U8> mLeafTri;                       ///< per triangle, 1 where AVATAR leaves it out; empty for none
            std::vector<U8> mReal;

            const Soup& main() const { return mSoup; }
            const std::vector<std::pair<S32, S32>>& mainSolids() const { return mAir.mSolids; }
        };

        /// The soup of a tile; false when it holds no triangle.
        bool soupTile(const TileInput& input, TileAirResult& result, TileBuildStats* stats, TileSoup& out)
        {
            if (!buildAirSoup(input, out.mAir, result, stats))
            {
                return false;
            }
            out.mSoup.assign(out.mAir.mVerts);
            out.mCutSoup.assign(out.mAir.mCutVerts);
            if (out.mAir.mLeaves)
            {
                out.mLeafTri.resize(out.mAir.ntris());
                for (S32 t = 0; t < out.mAir.ntris(); ++t)
                {
                    out.mLeafTri[t] = (out.mAir.mFlags[t] & SOUP_LEAF) ? 1 : 0;
                }
            }
            out.mReal = realAreas();
            return true;
        }

        /// A raster's columns, packed, for a capture.
        void dumpColumns(const rcHeightfield& hf, RasterColumns& out)
        {
            out.mWidth = hf.width;
            out.mHeight = hf.height;
            const size_t columns = static_cast<size_t>(hf.width) * hf.height;
            out.mStart.assign(1, 0);
            out.mSpans.clear();
            for (size_t c = 0; c < columns; ++c)
            {
                for (const rcSpan* s = hf.spans[c]; s; s = s->next)
                {
                    out.mSpans.push_back(RasterColumns::pack(s->smin, s->smax, s->area));
                }
                out.mStart.push_back(static_cast<U32>(out.mSpans.size()));
            }
        }

        /// The tile's raster grid: AVATAR's, widened to the air's pad when it keeps air.
        TileAirPtr coarseAirOf(const TileInput& input, const TileSoup& ts);

        /// The raster stage over a tile's soup at the input's cell.
        bool rasteriseSoup(const TileInput& input, const TileSoup& ts, TileAirResult& result, TileBuildStats* stats,
                           std::chrono::steady_clock::time_point started, RasterCapture* capture)
        {
            const ClassParams& ap = input.mAvatar;
            ClassParams tp = input.mTopParams;
            tp.mSlope = llmax(tp.mSlope, ap.mSlope);
            tp.mClimb = ap.mClimb;
            ClassBuildStats* astats = stats ? &stats->mClass[0] : nullptr;
            ClassBuildStats* tstats = stats ? &stats->mTop : nullptr;
            StageClock clock(astats ? astats->mMs : nullptr);
            const rcConfig cfg = tileConfig(input, ap, borderCells(ap.mRadius, input.mCell));
            const rcConfig grid = tileConfig(input, ap, input.mAir ? std::max(cfg.borderSize, input.mAirPad) : cfg.borderSize);
            const S32 inset = grid.borderSize - cfg.borderSize;
            const Soup* main_soup = &ts.main();
            const std::vector<std::pair<S32, S32>>* main_solids = &ts.mainSolids();
            const Soup& cut_soup = ts.mCutSoup;
            const std::vector<std::pair<S32, S32>>& cuts = ts.mAir.mCuts;
            const std::vector<U8>& codes = ts.mAir.mCodes;
            const std::vector<U8>& leaf_tri = ts.mLeafTri;
            const std::vector<U8>& real = ts.mReal;

            Freed mem;
            HeightfieldPtr overlay;
            mem.hf = makeHeightfield(grid);
            if (!mem.hf)
            {
                result.mError = "heightfield";
                return false;
            }
            rcHeightfield& hf = *mem.hf;
            const RasterSource source{ main_soup, &codes, main_solids, &cut_soup, &cuts };
            if (!rasterisePasses(hf, source, CellRect{ 0, 0, hf.width, hf.height }, grid.walkableClimb, clock))
            {
                result.mError = "rasterize";
                return false;
            }
            if (capture)
            {
                capture->mBmin[0] = grid.bmin[0];
                capture->mBmin[1] = grid.bmin[1];
                capture->mBmin[2] = grid.bmin[2];
                capture->mCell = grid.cs;
                capture->mUnit = grid.ch;
                capture->mBorder = grid.borderSize;
                capture->mAvatarBorder = cfg.borderSize;
                capture->mCodes = codes;
                capture->mReal = real;
                capture->mSoupVerts = main_soup->mVerts;     // Recast space, as the fixtures hold it
                capture->mSolids.clear();
                for (const auto& range : *main_solids)
                {
                    capture->mSolids.insert(capture->mSolids.end(), { range.first, range.second });
                }
                capture->mCutVerts = cut_soup.mVerts;
                capture->mCuts.clear();
                for (const auto& range : cuts)
                {
                    capture->mCuts.insert(capture->mCuts.end(), { range.first, range.second });
                }
                capture->mLeafTri = leaf_tri;
                capture->mCues.resize(ts.mAir.ntris());
                for (S32 t = 0; t < ts.mAir.ntris(); ++t)
                {
                    capture->mCues[t] = ts.mAir.cue(t);
                }
                capture->mTerrain = input.mTerrain;
                capture->mClimb = grid.walkableClimb;
                dumpColumns(hf, capture->mRasterised);
                clock.restart();
            }

            // The window: every cell a left-out triangle touches and one more
            // round it is AVATAR's own; its raster reaches a cell further, so
            // a bridge in the outer ring reads the columns past it complete.
            const size_t columns = static_cast<size_t>(hf.width) * hf.height;
            std::vector<U8> in_window, own_column;
            if (!leaf_tri.empty())
            {
                in_window.assign(columns, 0);
                CellRect rect{ hf.width, hf.height, 0, 0 };
                for (S32 t = 0; t < main_soup->ntris(); ++t)
                {
                    if (!leaf_tri[t])
                    {
                        continue;
                    }
                    const CellRect r = cellsOf(hf, *main_soup, t, t + 1);
                    for (S32 z = std::max(0, r.mZ0); z < std::min(hf.height, r.mZ1); ++z)
                    {
                        for (S32 x = std::max(0, r.mX0); x < std::min(hf.width, r.mX1); ++x)
                        {
                            in_window[x + z * hf.width] = 1;
                        }
                    }
                    rect = { std::min(rect.mX0, r.mX0 - 1), std::min(rect.mZ0, r.mZ0 - 1), std::max(rect.mX1, r.mX1 + 1), std::max(rect.mZ1, r.mZ1 + 1) };
                }
                overlay.reset(makeHeightfield(grid));
                const RasterSource window{ main_soup, &codes, main_solids, &cut_soup, &cuts, &leaf_tri };
                if (!overlay || !rasterisePasses(*overlay, window, rect, grid.walkableClimb, clock))
                {
                    result.mError = "rasterize";
                    return false;
                }
                own_column = in_window;
                if (stats)
                {
                    stats->mWindowCells = static_cast<U32>(std::count(in_window.begin(), in_window.end(), 1));
                    stats->mWindowRect[0] = rect.mX0 - grid.borderSize;
                    stats->mWindowRect[1] = rect.mZ0 - grid.borderSize;
                    stats->mWindowRect[2] = rect.mX1 - grid.borderSize;
                    stats->mWindowRect[3] = rect.mZ1 - grid.borderSize;
                }
            }
            CropView view(cfg, inset);
            view.point(hf, overlay.get(), &own_column);

            // Both bridge lists off the unbridged views. A column whose lists
            // differ gets a copy of its own for AVATAR, so neither view lays the
            // other's bridges.
            const std::vector<Bridge> top_bridges = findBridges(hf, grid.walkableClimb, 1);
            const std::vector<Bridge> avatar_bridges = findBridges(view.mHf, cfg.walkableClimb, AREA_CODE_BASE);
            {
                const auto column = [&](const Bridge& b) { return b.mX + b.mZ * hf.width; };
                std::map<S32, std::vector<Bridge>> by_top, by_avatar;
                for (const Bridge& b : top_bridges)
                {
                    by_top[column(b)].push_back(b);
                }
                for (Bridge b : avatar_bridges)
                {
                    b.mX += inset;
                    b.mZ += inset;
                    by_avatar[column(b)].push_back(b);
                }
                std::vector<S32> differ;
                for (const auto& [c, list] : by_avatar)
                {
                    const auto it = by_top.find(c);
                    if (it == by_top.end() || it->second != list)
                    {
                        differ.push_back(c);
                    }
                }
                for (const auto& [c, list] : by_top)
                {
                    const S32 x = c % hf.width - inset, z = c / hf.width - inset;
                    if (!by_avatar.count(c) && x >= 0 && z >= 0 && x < cfg.width && z < cfg.height)
                    {
                        differ.push_back(c);
                    }
                }
                if (!differ.empty() && !overlay)
                {
                    overlay.reset(makeHeightfield(grid));
                    if (!overlay)
                    {
                        result.mError = "heightfield";
                        return false;
                    }
                }
                if (own_column.empty() && !differ.empty())
                {
                    own_column.assign(columns, 0);
                }
                for (const S32 c : differ)
                {
                    if (!own_column[c])
                    {
                        own_column[c] = 1;
                        overlay->spans[c] = nullptr;
                        copyColumn(hf, *overlay, c % hf.width, c / hf.width);
                    }
                }
                for (const auto& [c, list] : by_avatar)
                {
                    if (!own_column.empty() && own_column[c])
                    {
                        addBridges(*overlay, list, cfg.walkableClimb);
                    }
                }
                addBridges(hf, top_bridges, grid.walkableClimb);
            }
            if (astats)
            {
                astats->mBridges = static_cast<U32>(avatar_bridges.size());
            }
            if (tstats)
            {
                tstats->mBridges = static_cast<U32>(top_bridges.size());
            }
            if (stats)
            {
                // The lists against each other over the cells AVATAR reads.
                std::vector<Bridge> top_in;
                for (const Bridge& b : top_bridges)
                {
                    const S32 x = b.mX - inset, z = b.mZ - inset;
                    if (x > 0 && z > 0 && x + 1 < cfg.width && z + 1 < cfg.height)
                    {
                        top_in.push_back(b);
                    }
                }
                std::vector<Bridge> avatar_in = avatar_bridges;
                for (Bridge& b : avatar_in)
                {
                    b.mX += inset;
                    b.mZ += inset;
                }
                stats->mTopOnlyBridges = bridgesMissing(top_in, avatar_in);
                stats->mAvatarOnlyBridges = bridgesMissing(avatar_in, top_in);
                stats->mOwnColumns = static_cast<U32>(std::count(own_column.begin(), own_column.end(), 1));
            }
            clock.lap(ClassBuildStats::BRIDGES);
            if (capture)
            {
                dumpColumns(hf, capture->mBridged);
                clock.restart();
            }
            cullUnderground(hf, input.mTerrain);
            if (overlay)
            {
                cullUnderground(*overlay, input.mTerrain);
            }
            clock.lap(ClassBuildStats::CULL);
            if (capture)
            {
                dumpColumns(hf, capture->mCulled);
                capture->mOwnColumn = own_column;
                capture->mInWindow = in_window;
                clock.restart();
            }

            // The air's filters first, kept aside, then AVATAR's over the same spans.
            const bool top_pass = input.mAir || input.mRunoff;
            AreaSnapshot unfiltered, top_areas, top_ledges;
            if (top_pass)
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                if (tstats)
                {
                    tstats->mBuilt = true;
                }
                unfiltered.take(hf);
                const bool keep_ledges = input.mRunoff || capture;
                filterSpans(hf, tp, grid.ch, grid.cs, keep_ledges ? &top_ledges : nullptr);
                top_clock.lap(ClassBuildStats::FILTERS);
                if (tstats)
                {
                    tstats->mHfHash = hashHeightfield(hf, 0, 0, hf.width, hf.height, {});
                    tstats->mHfAreaHash = hashHeightfield(hf, 0, 0, hf.width, hf.height, real);
                    top_clock.restart();
                }
                top_areas.take(hf);
                if (capture)
                {
                    dumpColumns(hf, capture->mTopFiltered);
                    top_ledges.restore(hf);
                    dumpColumns(hf, capture->mTopPreLedge);
                    top_clock.restart();
                }
                unfiltered.restore(hf);
                top_clock.lap(ClassBuildStats::FILTERS);
                clock.restart();
            }
            view.point(hf, overlay.get(), &own_column);
            maskTopOnly(view.mHf);
            filterSpans(view.mHf, ap, cfg.ch, cfg.cs);
            clock.lap(ClassBuildStats::FILTERS);
            if (astats)
            {
                std::vector<U8> code_lut(AREA_CODE_MAX + 1);
                for (S32 a = 0; a <= AREA_CODE_MAX; ++a)
                {
                    code_lut[a] = static_cast<U8>(a >= AREA_CODE_BASE ? a - AREA_TOP_ONLY : 0);
                }
                astats->mHfHash = hashHeightfield(view.mHf, 0, 0, cfg.width, cfg.height, {});
                astats->mHfAreaHash = hashHeightfield(view.mHf, 0, 0, cfg.width, cfg.height, code_lut);
                clock.restart();
            }
            if (capture)
            {
                dumpColumns(view.mHf, capture->mAvatarFiltered);
                clock.restart();
            }

            // Which of the raster's spans AVATAR stands on, for the air: its own
            // where it shares the span, else a span of its own column at that top.
            SpanFlags avatar_floor;
            if (top_pass)
            {
                avatar_floor.mStart.assign(1, 0);
                for (S32 c = 0; c < hf.width * hf.height; ++c)
                {
                    const S32 x = c % hf.width - inset, z = c / hf.width - inset;
                    const bool inside = x >= 0 && z >= 0 && x < cfg.width && z < cfg.height;
                    const bool own = inside && !own_column.empty() && own_column[c];
                    for (const rcSpan* s = hf.spans[c]; s; s = s->next)
                    {
                        bool walk = inside && !own && s->area != RC_NULL_AREA;
                        for (const rcSpan* o = own ? overlay->spans[c] : nullptr; o && !walk; o = o->next)
                        {
                            walk = o->smax == s->smax && o->area != RC_NULL_AREA;
                        }
                        avatar_floor.mFlags.push_back(walk ? AirGap::AVATAR_FLOOR : 0);
                    }
                    avatar_floor.mStart.push_back(static_cast<U32>(avatar_floor.mFlags.size()));
                }
                if (capture)
                {
                    capture->mAvatarFloorStart = avatar_floor.mStart;
                    capture->mAvatarFloor = avatar_floor.mFlags;
                }
                clock.restart();
            }
            // Faces off AVATAR's view with the window's columns back in, their
            // areas the air's: the faces of every solid, door leaves included.
            if (input.mWallFaces)
            {
                if (!in_window.empty())
                {
                    if (top_pass)
                    {
                        top_areas.restore(hf, &in_window);
                    }
                    std::vector<U8> faces_own = own_column;
                    for (size_t c = 0; c < columns; ++c)
                    {
                        faces_own[c] = own_column[c] && !in_window[c];
                    }
                    view.point(hf, overlay.get(), &faces_own);
                }
                extractWallFaces(view.mHf, cfg.borderSize, input.mWallMinHeight, result.mFaces, real.data());
                if (input.mFaceOffsets)
                {
                    alignWallFaces(result.mFaces, ts.mAir.mVerts.data(), ts.mAir.ntris(), cfg.cs, cfg.ch);
                }
                view.point(hf, overlay.get(), &own_column);
            }
            if (top_pass)
            {
                top_areas.restore(hf);
            }
            clock.lap(ClassBuildStats::WALLS);

            // The air's padded gaps, read here so the offsets reach the tile's edge columns; the air reuses them.
            AirGrid padded_air;
            std::vector<FaceOffset> padded_faces;
            const S32 air_pad = std::clamp(input.mAirPad, 0, grid.borderSize);
            if (input.mAir && (input.mFaceOffsets || capture))
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                extractAir(hf, grid.borderSize - air_pad, padded_air, real.data(), &avatar_floor);
                foldSlivers(padded_air, static_cast<S32>(std::ceil(input.mAirMinGap / grid.ch)));
                if (input.mFaceOffsets)
                {
                    computeFaceOffsets(padded_air, ts.mAir.mVerts.data(), ts.mAir.ntris(), grid.walkableClimb,
                                       padded_faces, stats ? &stats->mFaceOffsets : nullptr, stats ? stats->mFaceTraceEvery : 0,
                                       stats ? &stats->mFaceTrace : nullptr);
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
                const S32 overlap = static_cast<S32>(std::ceil(0.5f / grid.ch));
                result.mAir = buildTileAir(hf, grid.borderSize, input.mAirPad, grid.walkableClimb, overlap,
                                          static_cast<S32>(std::ceil(input.mAirMinGap / grid.ch)), tstats ? &tstats->mAir : nullptr, real.data(),
                                          &avatar_floor, nullptr, &padded_air);
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
            // Where the air's floors end over a drop, and the run-off down them.
            if (input.mRunoff && top_pass)
            {
                // Off the floors before the ledge filter: a roof's rim over a long
                // drop is where weather runs off, though the filter takes it.
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                top_ledges.restore(hf);
                extractRunoff(hf, grid.borderSize, grid.walkableClimb, static_cast<S32>(std::ceil(tp.mHeight / grid.ch)), input.mRunoffMax,
                              input.mRunoffSpacing, result.mLedges, result.mRunoff);
                top_clock.lap(ClassBuildStats::EDGES);
                clock.restart();
            }

            // Past the air's radius the spaces still need air: the tile again at the coarse cell, in this tile's cells.
            if (!input.mAir && input.mCoarseAir)
            {
                const auto coarse_started = std::chrono::steady_clock::now();
                result.mAir = coarseAirOf(input, ts);
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
                measureHeightfield(hf, *astats);
                if (overlay)
                {
                    ClassBuildStats extra;
                    measureHeightfield(*overlay, extra);
                    astats->mHfSpans += extra.mHfSpans;
                    astats->mHfBytes += extra.mHfBytes;
                }
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

        TileAirPtr coarseAirOf(const TileInput& input, const TileSoup& ts)
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
            return rasteriseSoup(c, ts, cr, nullptr, std::chrono::steady_clock::now(), nullptr) && cr.mAir ? upsampleAir(*cr.mAir, factor) : nullptr;
        }
    }

    bool rasteriseTile(const TileInput& input, TileAirResult& result, TileBuildStats* stats, RasterCapture* capture)
    {
        SK_WORLD_ZONE("air.raster");
        const auto started = std::chrono::steady_clock::now();
        // An empty tile has no air to keep, which is an answer too.
        result.mAirBuilt = input.mAir;
        TileSoup ts;
        // A tile left at logout stops between its stages: the pool drains before it joins.
        if (!soupTile(input, result, stats, ts) || LLApp::isExiting())
        {
            return false;
        }
        if (!rasteriseSoup(input, ts, result, stats, started, capture))
        {
            return false;
        }
        result.mStatus = TileAirResult::SUCCESS;
        return true;
    }
}
