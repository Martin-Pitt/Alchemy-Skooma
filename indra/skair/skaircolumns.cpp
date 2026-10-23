/**
 * @file skaircolumns.cpp
 * @brief The air's solid raster of its own: clip, count, scatter, merge, fill, cut, bridge and cull.
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

#include "skaircolumns.h"

#include "skairraster.h"
#include "skairsoup.h"
#include "skcensusprofile.h"
#include "skcensustypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
    using namespace skworld;

    /// A height within this many units of a unit boundary is on it: what keeps it off is float noise, not geometry.
    constexpr F64 SNAP_UNITS = 1e-3;
    constexpr F64 VOID_Z = -900.0;      ///< terrain samples below this have no region
    constexpr S32 STACK_SPANS = 64;     ///< a column merged on the stack; taller ones take the heap

    /// A height in units, snapped onto the unit grid where only float noise keeps it off.
    F64 units(F64 h, F64 unit)
    {
        const F64 q = h / unit;
        const F64 r = std::nearbyint(q);
        return std::fabs(q - r) < SNAP_UNITS ? r : q;
    }

    // ---- packed spans ------------------------------------------------------

    /// A span as emitted and merged: smin | smax << 13 | code << 26 | cue << 32 | column << 40.
    typedef U64 RawSpan;

    inline RawSpan packSpan(U32 column, U32 smin, U32 smax, U32 code, U32 cue)
    {
        return static_cast<U64>(smin) | static_cast<U64>(smax) << 13 | static_cast<U64>(code & 63) << 26 | static_cast<U64>(cue & 3) << 32 |
               static_cast<U64>(column) << 40;
    }
    inline U32 spanColumn(RawSpan s) { return static_cast<U32>(s >> 40); }

    /// A span in a column being merged.
    struct Span
    {
        U16 mMin;
        U16 mMax;
        U8 mCode;
        U8 mCue;
    };

    inline Span unpackSpan(RawSpan s)
    {
        return { static_cast<U16>(s & 0x1fff), static_cast<U16>((s >> 13) & 0x1fff), static_cast<U8>((s >> 26) & 63), static_cast<U8>((s >> 32) & 3) };
    }

    /// Insert a span into a column's merged list, lowest first: merge on overlap or touch. Where two tops are
    /// within climb the higher code is kept, else the code of the higher top ("top wins", where Recast keeps the
    /// incoming span's, which is soup order); the incoming span is compared as widened by the overlaps before.
    /// Ties go to the incoming span. n is the list's length, and grows by at most one.
    void addSpan(Span* col, S32& n, Span in, S32 climb, U32* top_wins)
    {
        S32 i = 0;
        while (i < n && col[i].mMax < in.mMin)
        {
            ++i;
        }
        S32 j = i;
        while (j < n && col[j].mMin <= in.mMax)
        {
            const Span& c = col[j];
            const S32 d = static_cast<S32>(in.mMax) - static_cast<S32>(c.mMax);
            if (d <= climb && d >= -climb)
            {
                if (c.mCode > in.mCode)
                {
                    in.mCode = c.mCode;
                    in.mCue = c.mCue;
                }
            }
            else if (c.mMax > in.mMax)
            {
                if (top_wins && in.mCode > c.mCode)
                {
                    ++*top_wins;
                }
                in.mCode = c.mCode;
                in.mCue = c.mCue;
            }
            in.mMin = std::min(in.mMin, c.mMin);
            in.mMax = std::max(in.mMax, c.mMax);
            ++j;
        }
        // Spans i..j-1 become the one at i.
        if (j == i)
        {
            std::copy_backward(col + i, col + n, col + n + 1);
            ++n;
        }
        else if (j > i + 1)
        {
            std::copy(col + j, col + n, col + i + 1);
            n -= j - i - 1;
        }
        col[i] = in;
    }

    // ---- clipping ------------------------------------------------------------

    struct V3
    {
        F64 v[3];
    };

    /// Recast's dividePoly: the part of a convex polygon at or below offset along axis into out1, the rest into
    /// out2; a vertex on the line goes to both. At most 12 vertices in, 12 each out. Where Recast keeps a piece of no
    /// width beyond the line - an edge or vertex touching it, or a face lying in it - this keeps only a face in the
    /// line, on the side its solid is on: behind its normal along the axis, facing -1 (below the line), +1 (above),
    /// 0 for a triangle with no extent across the axis, whose touching pieces go nowhere.
    void dividePoly(const V3* in, S32 n, V3* out1, S32& n1, V3* out2, S32& n2, F64 offset, S32 axis, S32 facing)
    {
        F64 d[12];
        bool zero = false;
        for (S32 k = 0; k < n; ++k)
        {
            d[k] = offset - in[k].v[axis];
            zero = zero || d[k] == 0.0;
        }
        n1 = n2 = 0;
        for (S32 a = 0, b = n - 1; a < n; b = a, ++a)
        {
            if ((d[a] >= 0) != (d[b] >= 0))
            {
                const F64 s = d[b] / (d[b] - d[a]);
                V3 q;
                for (S32 k = 0; k < 3; ++k)
                {
                    q.v[k] = in[b].v[k] + (in[a].v[k] - in[b].v[k]) * s;
                }
                out1[n1++] = q;
                out2[n2++] = q;
                if (d[a] > 0)
                {
                    out1[n1++] = in[a];
                }
                else if (d[a] < 0)
                {
                    out2[n2++] = in[a];
                }
            }
            else
            {
                if (d[a] >= 0)
                {
                    out1[n1++] = in[a];
                    if (d[a] != 0)
                    {
                        continue;
                    }
                }
                out2[n2++] = in[a];
            }
        }
        const auto onLine = [&](const V3* p, S32 m) {
            for (S32 k = 0; k < m; ++k)
            {
                if (p[k].v[axis] != offset)
                {
                    return false;
                }
            }
            return m > 0;
        };
        // A piece with no width on this side - a touching edge or vertex, or a face in the line - is kept only
        // where it is a face whose solid is on this side.
        // Only a vertex on the line can leave a side with nothing but points on it.
        if (!zero)
        {
            return;
        }
        if (facing >= 0 && onLine(out1, n1))
        {
            n1 = 0;
        }
        if (facing <= 0 && onLine(out2, n2))
        {
            n2 = 0;
        }
    }

    /// The frame in doubles, as every stage reads it.
    struct Frame
    {
        F64 mOx, mOy, mOz, mCell, mUnit, mRange;
        S32 mW;
        S32 mClimb;

        explicit Frame(const ColumnFrame& f)
            : mOx(f.mOrigin.mV[VX]), mOy(f.mOrigin.mV[VY]), mOz(f.mOrigin.mV[VZ]), mCell(f.mCell), mUnit(f.mUnit), mRange(f.mHeightRange),
              mW(f.mWidth), mClimb(f.mClimb)
        {
        }
    };

    /// Clip one triangle into the frame's columns, Recast-style: rows along SL x, then columns along SL y, in a
    /// fixed buffer on the stack. Each piece's height range becomes a span.
    template<typename Store> void emitTriangle(const Frame& g, const F32* t, U8 code, U8 cue, Store& out)
    {
        F64 lo[3], hi[3];
        V3 buf[5][12];
        V3* in = buf[0];
        V3* row = buf[1];
        V3* rest = buf[2];
        V3* cell = buf[3];
        V3* rrest = buf[4];
        for (S32 k = 0; k < 3; ++k)
        {
            in[k].v[0] = t[k * 3];
            in[k].v[1] = t[k * 3 + 1];
            in[k].v[2] = t[k * 3 + 2];
        }
        for (S32 a = 0; a < 3; ++a)
        {
            lo[a] = std::min(std::min(in[0].v[a], in[1].v[a]), in[2].v[a]);
            hi[a] = std::max(std::max(in[0].v[a], in[1].v[a]), in[2].v[a]);
        }
        const S32 W = g.mW;
        const F64 cs = g.mCell;
        // The side of a column edge the triangle's solid lies on, should it lie in one: behind its plan normal.
        const F64 e0x = in[1].v[0] - in[0].v[0], e0y = in[1].v[1] - in[0].v[1], e0z = in[1].v[2] - in[0].v[2];
        const F64 e1x = in[2].v[0] - in[0].v[0], e1y = in[2].v[1] - in[0].v[1], e1z = in[2].v[2] - in[0].v[2];
        const F64 nx = e0y * e1z - e0z * e1y, ny = e0z * e1x - e0x * e1z;
        const S32 facing_x = nx > 0.0 ? -1 : nx < 0.0 ? 1 : 0;
        const S32 facing_y = ny > 0.0 ? -1 : ny < 0.0 ? 1 : 0;
        if (lo[0] > g.mOx + W * cs || hi[0] < g.mOx || lo[1] > g.mOy + W * cs || hi[1] < g.mOy || lo[2] > g.mOz + g.mRange || hi[2] < g.mOz)
        {
            return;
        }
        // On a column edge, the triangle's least x is split there first, so a face lying in it goes to its solid's side.
        const F64 qx = (lo[0] - g.mOx) / cs, fx = std::floor(qx);
        const S32 ix0 = std::clamp(static_cast<S32>(fx) - (qx == fx ? 1 : 0), -1, W - 1);
        const S32 raw_ix1 = static_cast<S32>(std::floor((hi[0] - g.mOx) / cs));
        const S32 ix1 = std::clamp(raw_ix1, 0, W - 1);
        const bool one_row = ix0 == raw_ix1;
        S32 nin = 3;
        for (S32 ix = ix0; ix <= ix1; ++ix)
        {
            S32 nrow, nrest;
            if (one_row)
            {
                // Wholly inside one row: nothing to split off.
                std::swap(in, row);
                nrow = nin;
                nin = 0;
            }
            else
            {
                dividePoly(in, nin, row, nrow, rest, nrest, g.mOx + (ix + 1) * cs, 0, facing_x);
                std::swap(in, rest);
                nin = nrest;
            }
            if (nrow < 3 || ix < 0)
            {
                continue;
            }
            F64 ylo = row[0].v[1], yhi = row[0].v[1];
            for (S32 k = 1; k < nrow; ++k)
            {
                ylo = std::min(ylo, row[k].v[1]);
                yhi = std::max(yhi, row[k].v[1]);
            }
            const F64 qy = (ylo - g.mOy) / cs, fy = std::floor(qy);
            S32 iy0 = static_cast<S32>(fy) - (qy == fy ? 1 : 0);
            S32 iy1 = static_cast<S32>(std::floor((yhi - g.mOy) / cs));
            if (iy1 < 0 || iy0 >= W)
            {
                continue;
            }
            const bool one_column = iy0 == iy1;
            iy0 = std::clamp(iy0, -1, W - 1);
            iy1 = std::clamp(iy1, 0, W - 1);
            for (S32 iy = iy0; iy <= iy1; ++iy)
            {
                S32 ncell, nrr;
                if (one_column)
                {
                    // Wholly inside one column.
                    std::swap(row, cell);
                    ncell = nrow;
                    nrow = 0;
                }
                else
                {
                    dividePoly(row, nrow, cell, ncell, rrest, nrr, g.mOy + (iy + 1) * cs, 1, facing_y);
                    std::swap(row, rrest);
                    nrow = nrr;
                }
                if (ncell < 3 || iy < 0)
                {
                    continue;
                }
                F64 zlo = cell[0].v[2], zhi = cell[0].v[2];
                for (S32 k = 1; k < ncell; ++k)
                {
                    zlo = std::min(zlo, cell[k].v[2]);
                    zhi = std::max(zhi, cell[k].v[2]);
                }
                zlo -= g.mOz;
                zhi -= g.mOz;
                if (zhi < 0.0 || zlo > g.mRange)
                {
                    continue;
                }
                zlo = std::max(zlo, 0.0);
                zhi = std::min(zhi, g.mRange);
                const S32 smin = std::clamp(static_cast<S32>(std::floor(units(zlo, g.mUnit))), 0, COLUMN_HEIGHT_MAX - 1);
                const S32 smax = std::clamp(static_cast<S32>(std::ceil(units(zhi, g.mUnit))), smin + 1, static_cast<S32>(COLUMN_HEIGHT_MAX));
                out.add(static_cast<size_t>(ix) + static_cast<size_t>(iy) * W, Span{ static_cast<U16>(smin), static_cast<U16>(smax), code, cue });
            }
        }
    }

    // ---- bodies --------------------------------------------------------------

    /// A body's columns, clipped to the frame: half-open x0..x1, y0..y1, empty when x1 <= x0.
    struct Box
    {
        S32 mX0 = 0, mY0 = 0, mX1 = 0, mY1 = 0;
        S32 width() const { return mX1 - mX0; }
        S32 count() const { return std::max(0, mX1 - mX0) * std::max(0, mY1 - mY0); }
    };

    Box bodyBox(const Frame& g, const F32* verts, S32 begin, S32 end)
    {
        F64 x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (S32 t = begin; t < end; ++t)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                const F32* p = verts + static_cast<size_t>(t) * 9 + k * 3;
                x0 = std::min(x0, static_cast<F64>(p[0]));
                x1 = std::max(x1, static_cast<F64>(p[0]));
                y0 = std::min(y0, static_cast<F64>(p[1]));
                y1 = std::max(y1, static_cast<F64>(p[1]));
            }
        }
        Box b;
        b.mX0 = std::max(0, static_cast<S32>(std::floor((x0 - g.mOx) / g.mCell)));
        b.mX1 = std::min(g.mW - 1, static_cast<S32>(std::floor((x1 - g.mOx) / g.mCell))) + 1;
        b.mY0 = std::max(0, static_cast<S32>(std::floor((y0 - g.mOy) / g.mCell)));
        b.mY1 = std::min(g.mW - 1, static_cast<S32>(std::floor((y1 - g.mOy) / g.mCell))) + 1;
        return b;
    }

    /// Where each column centre in the box crosses a body's triangles, bucketed by column: counted, then filled.
    /// eps lets a centre a rounding error outside every triangle at a shared edge still count.
    struct Hits
    {
        std::vector<U32> mStart;    ///< box columns + 1, ix fastest
        std::vector<F64> mY;

        template<typename Visit> static void each(const Frame& g, const F32* verts, S32 begin, S32 end, const Box& box, F64 eps, Visit&& visit)
        {
            const F64 cs = g.mCell;
            for (S32 t = begin; t < end; ++t)
            {
                const F32* p = verts + static_cast<size_t>(t) * 9;
                const F64 ax = p[0], ay = p[1], az = p[2], bx = p[3], by = p[4], bz = p[5], cx = p[6], cy = p[7], cz = p[8];
                const F64 d = (bx - cx) * (ay - cy) + (cy - by) * (ax - cx);
                if (std::fabs(d) < 1e-9)
                {
                    continue;   // vertical: the column crosses it nowhere; the faces it joins count
                }
                const S32 tx0 = std::max(box.mX0, static_cast<S32>(std::floor((std::min(std::min(ax, bx), cx) - g.mOx) / cs)));
                const S32 tx1 = std::min(box.mX1 - 1, static_cast<S32>(std::floor((std::max(std::max(ax, bx), cx) - g.mOx) / cs)));
                const S32 ty0 = std::max(box.mY0, static_cast<S32>(std::floor((std::min(std::min(ay, by), cy) - g.mOy) / cs)));
                const S32 ty1 = std::min(box.mY1 - 1, static_cast<S32>(std::floor((std::max(std::max(ay, by), cy) - g.mOy) / cs)));
                for (S32 ix = tx0; ix <= tx1; ++ix)
                {
                    const F64 pz = g.mOx + (ix + 0.5) * cs;
                    for (S32 iy = ty0; iy <= ty1; ++iy)
                    {
                        const F64 px = g.mOy + (iy + 0.5) * cs;
                        const F64 l0 = ((bx - cx) * (px - cy) + (cy - by) * (pz - cx)) / d;
                        const F64 l1 = ((cx - ax) * (px - cy) + (ay - cy) * (pz - cx)) / d;
                        const F64 l2 = 1.0 - l0 - l1;
                        if (l0 < -eps || l1 < -eps || l2 < -eps)
                        {
                            continue;
                        }
                        visit((ix - box.mX0) + (iy - box.mY0) * box.width(), l0 * az + l1 * bz + l2 * cz);
                    }
                }
            }
        }

        void build(const Frame& g, const F32* verts, S32 begin, S32 end, const Box& box, F64 eps)
        {
            mStart.assign(static_cast<size_t>(box.count()) + 1, 0);
            each(g, verts, begin, end, box, eps, [&](S32 c, F64) { ++mStart[c + 1]; });
            for (size_t c = 1; c < mStart.size(); ++c)
            {
                mStart[c] += mStart[c - 1];
            }
            mY.resize(mStart.back());
            std::vector<U32>& at = mFill;
            at.assign(mStart.begin(), mStart.end() - 1);
            each(g, verts, begin, end, box, eps, [&](S32 c, F64 y) { mY[at[c]++] = y; });
        }

        std::vector<U32> mFill;
    };

    /// Sort a column's few crossings in place.
    void sortSmall(F64* v, U32 n)
    {
        for (U32 i = 1; i < n; ++i)
        {
            const F64 x = v[i];
            U32 j = i;
            while (j > 0 && v[j - 1] > x)
            {
                v[j] = v[j - 1];
                --j;
            }
            v[j] = x;
        }
    }

    /// A body's inside as null spans: what each column's centre crosses an odd number of surfaces to reach,
    /// the crossings taken in pairs from the bottom up; a column the body only touches (odd) is filled from
    /// its lowest crossing to its highest. The same surface met at a shared edge is one crossing.
    template<typename Store> void fillBody(const Frame& g, const F32* verts, S32 begin, S32 end, Hits& hits, Store& out)
    {
        const Box box = bodyBox(g, verts, begin, end);
        if (box.mX1 <= box.mX0 || box.mY1 <= box.mY0)
        {
            return;
        }
        hits.build(g, verts, begin, end, box, 1e-4);
        const F64 top_m = g.mOz + g.mRange;
        const auto fill = [&](S32 ix, S32 iy, F64 lo, F64 hi) {
            const F64 top = std::min(hi, top_m), bottom = std::max(lo, g.mOz);
            if (top <= bottom)
            {
                return;
            }
            const S32 smin = std::clamp(static_cast<S32>(std::floor(units(bottom - g.mOz, g.mUnit))), 0, static_cast<S32>(COLUMN_HEIGHT_MAX));
            const S32 smax = std::clamp(static_cast<S32>(std::ceil(units(top - g.mOz, g.mUnit))), 0, static_cast<S32>(COLUMN_HEIGHT_MAX));
            if (smax > smin)
            {
                out.add(static_cast<size_t>(ix) + static_cast<size_t>(iy) * g.mW, Span{ static_cast<U16>(smin), static_cast<U16>(smax), 0, 0 });
            }
        };
        F64 column[256];
        for (S32 ix = box.mX0; ix < box.mX1; ++ix)
        {
            for (S32 iy = box.mY0; iy < box.mY1; ++iy)
            {
                const S32 c = (ix - box.mX0) + (iy - box.mY0) * box.width();
                const U32 a = hits.mStart[c], n = hits.mStart[c + 1] - a;
                if (!n)
                {
                    continue;
                }
                F64* y = &hits.mY[a];
                sortSmall(y, n);
                U32 m = 0;
                for (U32 k = 0; k < n && m < 256; ++k)
                {
                    if (!m || y[k] - column[m - 1] > 1e-3)
                    {
                        column[m++] = y[k];
                    }
                }
                if (m % 2)
                {
                    fill(ix, iy, column[0], column[m - 1]);
                    continue;
                }
                for (U32 k = 0; k + 1 < m; k += 2)
                {
                    fill(ix, iy, column[k], column[k + 1]);
                }
            }
        }
    }

    // ---- the raster's scratch --------------------------------------------------

    /// Every column's merged spans, merged as they arrive: a few inline per column, a column that grows past
    /// them moved to a block of the pool. Emission order is the merge order, as Recast's incremental inserts.
    struct ColumnStore
    {
        static constexpr S32 INLINE = 8;
        static constexpr U8 SPILLED = 0xff;
        struct Spill
        {
            U32 mAt;
            U32 mCount;
            U32 mCap;
        };
        std::vector<U8> mCount;         ///< per column, its spans inline, or SPILLED
        std::vector<Span> mInline;      ///< INLINE per column
        std::vector<U32> mSpill;        ///< per spilled column, its entry in mSpills
        std::vector<Spill> mSpills;
        std::vector<Span> mPool;
        S32 mClimb = 0;
        U32* mTopWins = nullptr;
        U32 mAdded = 0;

        void reset(size_t columns, S32 climb, U32* top_wins)
        {
            mCount.assign(columns, 0);
            mInline.resize(columns * INLINE);
            mSpill.resize(columns);
            mSpills.clear();
            mPool.clear();
            mClimb = climb;
            mTopWins = top_wins;
            mAdded = 0;
        }

        Span* list(size_t c, U32& n)
        {
            if (mCount[c] != SPILLED)
            {
                n = mCount[c];
                return &mInline[c * INLINE];
            }
            const Spill& sp = mSpills[mSpill[c]];
            n = sp.mCount;
            return &mPool[sp.mAt];
        }

        void add(size_t c, Span in)
        {
            ++mAdded;
            if (mCount[c] != SPILLED)
            {
                S32 n = mCount[c];
                if (n < INLINE)
                {
                    addSpan(&mInline[c * INLINE], n, in, mClimb, mTopWins);
                    mCount[c] = static_cast<U8>(n);
                    return;
                }
                // Full: the column moves to the pool.
                mSpill[c] = static_cast<U32>(mSpills.size());
                mSpills.push_back({ static_cast<U32>(mPool.size()), static_cast<U32>(INLINE), 4 * INLINE });
                mPool.resize(mPool.size() + 4 * INLINE);
                std::copy(&mInline[c * INLINE], &mInline[c * INLINE] + INLINE, &mPool[mSpills.back().mAt]);
                mCount[c] = SPILLED;
            }
            Spill& sp = mSpills[mSpill[c]];
            if (sp.mCount + 1 > sp.mCap)
            {
                const U32 at = static_cast<U32>(mPool.size());
                mPool.resize(mPool.size() + 2 * sp.mCap);
                std::copy(mPool.begin() + sp.mAt, mPool.begin() + sp.mAt + sp.mCount, mPool.begin() + at);
                sp.mAt = at;
                sp.mCap *= 2;
            }
            S32 n = static_cast<S32>(sp.mCount);
            addSpan(&mPool[sp.mAt], n, in, mClimb, mTopWins);
            sp.mCount = static_cast<U32>(n);
        }
    };

    struct Scratch
    {
        ColumnStore mStore;
        std::vector<RawSpan> mBridges;
        std::vector<Span> mHeap;
        Hits mHits;
    };

    Scratch& scratch()
    {
        thread_local Scratch s;
        return s;
    }

    /// The cellsOf rule of the window: a triangle's box a cell wider each way, half-open.
    void cellsOf(const Frame& g, const F32* p, S32 n_tris, S32* r)
    {
        F64 x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (S32 k = 0; k < n_tris * 3; ++k)
        {
            x0 = std::min(x0, static_cast<F64>(p[k * 3]));
            x1 = std::max(x1, static_cast<F64>(p[k * 3]));
            y0 = std::min(y0, static_cast<F64>(p[k * 3 + 1]));
            y1 = std::max(y1, static_cast<F64>(p[k * 3 + 1]));
        }
        r[0] = static_cast<S32>(std::floor((x0 - g.mOx) / g.mCell)) - 1;
        r[1] = static_cast<S32>(std::floor((y0 - g.mOy) / g.mCell)) - 1;
        r[2] = static_cast<S32>(std::floor((x1 - g.mOx) / g.mCell)) + 2;
        r[3] = static_cast<S32>(std::floor((y1 - g.mOy) / g.mCell)) + 2;
    }

    bool overlaps(const S32* a, const S32* b)
    {
        return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
    }

    /// The land's surface as a height index at a column's centre, or -1 where the land has no say.
    S32 groundIndex(const Frame& g, const TerrainWindow& t, S32 ix, S32 iy)
    {
        const F64 px = g.mOx + (ix + 0.5) * g.mCell;
        const F64 py = g.mOy + (iy + 0.5) * g.mCell;
        const F64 gx = (px - t.mOrigin.mV[VX]) / t.mSpacing;
        const F64 gy = (py - t.mOrigin.mV[VY]) / t.mSpacing;
        if (gx < 0.0 || gy < 0.0 || gx > t.mWidth - 1.0 || gy > t.mHeight - 1.0)
        {
            return -1;
        }
        const S32 i0 = std::clamp(static_cast<S32>(gx), 0, t.mWidth - 2);
        const S32 j0 = std::clamp(static_cast<S32>(gy), 0, t.mHeight - 2);
        const F64 u = std::clamp(gx - i0, 0.0, 1.0);
        const F64 v = std::clamp(gy - j0, 0.0, 1.0);
        const F64 z00 = t.mZ[j0 * t.mWidth + i0], z10 = t.mZ[j0 * t.mWidth + i0 + 1];
        const F64 z01 = t.mZ[(j0 + 1) * t.mWidth + i0], z11 = t.mZ[(j0 + 1) * t.mWidth + i0 + 1];
        if (z00 < VOID_Z || z10 < VOID_Z || z01 < VOID_Z || z11 < VOID_Z)
        {
            return -1;
        }
        const F64 z = u > v ? z00 + u * (z10 - z00) + v * (z11 - z10) : z00 + v * (z01 - z00) + u * (z11 - z01);
        return std::clamp(static_cast<S32>(std::floor(units(z - g.mOz, g.mUnit))), 0, static_cast<S32>(COLUMN_HEIGHT_MAX));
    }

    /// Each column's merged list, with its bridges when asked and culled under the land when given it, into a
    /// raster's arrays.
    void compact(const Frame& g, const ColumnFrame& frame, Scratch& s, const TerrainWindow* t, bool bridges, ColumnRaster& out,
                 ColumnRasterStats* stats)
    {
        const S32 W = g.mW;
        const size_t columns = static_cast<size_t>(W) * W;
        out.mFrame = frame;
        out.mStart.resize(columns + 1);
        out.mBottom.clear();
        out.mTop.clear();
        out.mCode.clear();
        out.mWalk.clear();
        const size_t total = s.mStore.mAdded + (bridges ? s.mBridges.size() : 0);
        out.mBottom.reserve(total);
        out.mTop.reserve(total);
        out.mCode.reserve(total);
        out.mWalk.reserve(total);
        const bool land = t && t->mWidth >= 2 && t->mHeight >= 2 && t->mZ.size() >= static_cast<size_t>(t->mWidth * t->mHeight);
        size_t b = 0;
        Span stack[STACK_SPANS + 8];
        for (size_t c = 0; c < columns; ++c)
        {
            out.mStart[c] = static_cast<U32>(out.mTop.size());
            U32 n;
            const Span* merged = s.mStore.list(c, n);
            size_t nb = 0;
            while (bridges && b + nb < s.mBridges.size() && spanColumn(s.mBridges[b + nb]) == c)
            {
                ++nb;
            }
            if (!n && !nb)
            {
                continue;
            }
            Span* col = stack;
            if (n + nb > STACK_SPANS)
            {
                s.mHeap.resize(n + nb + 1);
                col = s.mHeap.data();
            }
            for (U32 k = 0; k < n; ++k)
            {
                col[k] = merged[k];
            }
            S32 m = static_cast<S32>(n);
            for (size_t k = 0; k < nb; ++k)
            {
                addSpan(col, m, unpackSpan(s.mBridges[b + k]), g.mClimb, stats ? &stats->mTopWins : nullptr);
            }
            b += nb;
            if (stats)
            {
                stats->mMaxSpans = std::max(stats->mMaxSpans, static_cast<U32>(m));
            }
            const S32 ground = land ? groundIndex(g, *t, static_cast<S32>(c % W), static_cast<S32>(c / W)) : -1;
            for (S32 k = 0; k < m; ++k)
            {
                Span sp = col[k];
                if (ground >= 0)
                {
                    // Nothing legitimate lies below the land: a span wholly under it goes, one reaching through it starts there.
                    if (sp.mMax < ground)
                    {
                        continue;
                    }
                    sp.mMin = static_cast<U16>(std::max<S32>(sp.mMin, ground));
                }
                out.mBottom.push_back(sp.mMin);
                out.mTop.push_back(sp.mMax);
                out.mCode.push_back(sp.mCode);
                out.mWalk.push_back(static_cast<U8>(sp.mCue == CUE_STAIR ? WALK_CUE_STAIR : sp.mCue == CUE_RAMP ? WALK_CUE_RAMP : 0));
            }
        }
        out.mStart[columns] = static_cast<U32>(out.mTop.size());
        if (stats)
        {
            stats->mSpans += static_cast<U32>(out.mTop.size());
        }
    }

    F64 msSince(std::chrono::steady_clock::time_point& t)
    {
        const auto now = std::chrono::steady_clock::now();
        const F64 ms = std::chrono::duration<F64, std::milli>(now - t).count();
        t = now;
        return ms;
    }
}

namespace skworld
{
    U16 ColumnFrame::topUnits() const
    {
        return static_cast<U16>(std::clamp(static_cast<S32>(mHeightRange / mUnit + 0.5f), 0, 0xffff));
    }

    ColumnFrame tileFrame(S32 tile_x, S32 tile_y, F32 tile_size, F32 z_min, F32 z_max, F32 cell, S32 border, F32 climb_m)
    {
        ColumnFrame f;
        f.mCell = cell;
        f.mUnit = cell;
        f.mBorder = border;
        f.mWidth = static_cast<S32>(tile_size / cell) + border * 2;
        // Heights quantise from the origin's z, so it sits on the unit grid, not at this tile's lowest point:
        // two tiles that started from different z would put one floor on different units.
        const F32 bottom = std::floor((z_min - cell) / cell) * cell;
        const F32 top = std::ceil((z_max + cell) / cell) * cell;
        f.mOrigin.set(tile_x * tile_size - border * cell, tile_y * tile_size - border * cell, bottom);
        f.mHeightRange = top - bottom;
        f.mClimb = static_cast<S32>(std::floor(climb_m / cell));
        return f;
    }

    void rasteriseColumns(const ColumnFrame& frame, const ColumnSource& source, ColumnRaster& out, ColumnRasterStats* stats)
    {
        SK_WORLD_ZONE("air.raster.columns");
        const Frame g(frame);
        const AirSoup& soup = *source.mSoup;
        const S32 W = g.mW;
        const size_t columns = static_cast<size_t>(W) * W;
        Scratch& s = scratch();
        auto clock = std::chrono::steady_clock::now();
        ColumnRasterStats local;
        ColumnRasterStats& st = stats ? *stats : local;

        // Clip and merge: triangles in soup order, then the bodies' fills, each span merged into its column as it comes.
        s.mStore.reset(columns, g.mClimb, &st.mTopWins);
        {
            SK_WORLD_ZONE("air.raster.clip");
            S32 r[4];
            for (S32 t = 0; t < soup.ntris(); ++t)
            {
                if (source.mDropLeaves && (soup.mFlags[t] & SOUP_LEAF))
                {
                    continue;
                }
                if (source.mRect)
                {
                    cellsOf(g, soup.tri(t), 1, r);
                    if (!overlaps(source.mRect, r))
                    {
                        continue;
                    }
                }
                emitTriangle(g, soup.tri(t), soup.mCodes[t], soup.cue(t), s.mStore);
            }
        }
        st.mClipMs += msSince(clock);
        {
            SK_WORLD_ZONE("air.raster.fillsolid");
            S32 r[4];
            for (const auto& [begin, end] : soup.mSolids)
            {
                if (begin >= end || (source.mDropLeaves && (soup.mFlags[begin] & SOUP_LEAF)))
                {
                    continue;
                }
                if (source.mRect)
                {
                    cellsOf(g, soup.tri(begin), end - begin, r);
                    if (!overlaps(source.mRect, r))
                    {
                        continue;
                    }
                }
                fillBody(g, soup.mVerts.data(), begin, end, s.mHits, s.mStore);
            }
        }
        st.mFillSolidMs += msSince(clock);
        st.mRawSpans += s.mStore.mAdded;
        st.mHeapColumns += static_cast<U32>(s.mStore.mSpills.size());

        // Cut: the exclusion volumes clear the codes of the spans they overlap.
        {
            SK_WORLD_ZONE("air.raster.cut");
            for (const auto& [begin, end] : soup.mCuts)
            {
                if (begin >= end)
                {
                    continue;
                }
                const Box box = bodyBox(g, soup.mCutVerts.data(), begin, end);
                if (box.mX1 <= box.mX0 || box.mY1 <= box.mY0)
                {
                    continue;
                }
                s.mHits.build(g, soup.mCutVerts.data(), begin, end, box, 0.0);
                for (S32 iy = box.mY0; iy < box.mY1; ++iy)
                {
                    for (S32 ix = box.mX0; ix < box.mX1; ++ix)
                    {
                        const S32 h = (ix - box.mX0) + (iy - box.mY0) * box.width();
                        const U32 ha = s.mHits.mStart[h], hn = s.mHits.mStart[h + 1] - ha;
                        if (!hn)
                        {
                            continue;
                        }
                        F64 lo = s.mHits.mY[ha], hi = lo;
                        for (U32 k = 1; k < hn; ++k)
                        {
                            lo = std::min(lo, s.mHits.mY[ha + k]);
                            hi = std::max(hi, s.mHits.mY[ha + k]);
                        }
                        const S32 smin = static_cast<S32>(std::floor(units(lo - g.mOz, g.mUnit)));
                        const S32 smax = static_cast<S32>(std::ceil(units(hi - g.mOz, g.mUnit)));
                        const size_t c = static_cast<size_t>(ix) + static_cast<size_t>(iy) * W;
                        U32 n;
                        Span* col = s.mStore.list(c, n);
                        for (U32 k = 0; k < n; ++k)
                        {
                            if (col[k].mCode && col[k].mMin < smax && col[k].mMax > smin)
                            {
                                col[k].mCode = 0;
                                col[k].mCue = 0;
                            }
                        }
                    }
                }
            }
        }
        st.mCutMs += msSince(clock);

        // Bridges: a crevice one column wide whose two sides are within climb, with nothing at that level in it,
        // found on the cut raster and laid after, so none is read as a neighbour by another.
        s.mBridges.clear();
        {
            SK_WORLD_ZONE("air.raster.bridges");
            const S32 climb = g.mClimb;
            ColumnStore& store = s.mStore;
            for (S32 ix = 1; ix + 1 < W; ++ix)
            {
                for (S32 iy = 1; iy + 1 < W; ++iy)
                {
                    const size_t c = static_cast<size_t>(ix) + static_cast<size_t>(iy) * W;
                    if (source.mBridgeColumns && !source.mBridgeColumns[c])
                    {
                        continue;
                    }
                    for (S32 axis = 0; axis < 2; ++axis)
                    {
                        const size_t na = axis == 0 ? c - W : c - 1, nb = axis == 0 ? c + W : c + 1;
                        U32 nna, nnb, nc;
                        const Span* la = store.list(na, nna);
                        const Span* lb = store.list(nb, nnb);
                        const Span* lc = store.list(c, nc);
                        for (U32 i = 0; i < nna; ++i)
                        {
                            const Span a = la[i];
                            if (a.mCode < 1)
                            {
                                continue;
                            }
                            for (U32 j = 0; j < nnb; ++j)
                            {
                                const Span b = lb[j];
                                if (b.mCode < 1 || std::abs(static_cast<S32>(a.mMax) - static_cast<S32>(b.mMax)) > climb)
                                {
                                    continue;
                                }
                                const S32 lo = std::min(a.mMax, b.mMax), hi = std::max(a.mMax, b.mMax);
                                bool skip = false;
                                for (U32 k = 0; k < nc && !skip; ++k)
                                {
                                    const Span h = lc[k];
                                    // Already joined through a floor within climb of both; or matter in the way.
                                    skip = (h.mCode >= 1 && std::abs(h.mMax - lo) <= climb && std::abs(h.mMax - hi) <= climb) ||
                                           (h.mMax > lo && h.mMin <= hi);
                                }
                                if (skip)
                                {
                                    continue;
                                }
                                const S32 bottom = hi > lo ? lo : hi - 1;
                                const U8 code = std::max(a.mCode, b.mCode);
                                const U8 cue = a.mCode > b.mCode ? a.mCue : b.mCode > a.mCode ? b.mCue : (b.mCue ? b.mCue : a.mCue);
                                s.mBridges.push_back(packSpan(static_cast<U32>(c), std::max(0, bottom), std::max(hi, bottom + 1), code, cue));
                            }
                        }
                    }
                }
            }
            std::stable_sort(s.mBridges.begin(), s.mBridges.end(), [](RawSpan x, RawSpan y) { return spanColumn(x) < spanColumn(y); });
            st.mBridges += static_cast<U32>(s.mBridges.size());
        }
        st.mBridgesMs += msSince(clock);

        // Compact: each column's merged list with its bridges, culled under the land, into the arrays.
        if (source.mAfterCut)
        {
            compact(g, frame, s, nullptr, false, *source.mAfterCut, nullptr);
        }
        if (source.mAfterBridges)
        {
            compact(g, frame, s, nullptr, true, *source.mAfterBridges, nullptr);
        }
        {
            SK_WORLD_ZONE("air.raster.cull");
            compact(g, frame, s, source.mTerrain, true, out, &st);
        }
        st.mCullMs += msSince(clock);
    }

    WalkParams walkParams(F32 climb_m, F32 height_m, F32 slope_deg, F32 cell, F32 unit)
    {
        WalkParams p;
        p.mClimb = static_cast<S32>(std::floor(climb_m / unit));
        p.mHeight = static_cast<S32>(std::ceil(height_m / unit));
        p.mLedge = static_cast<S32>(std::ceil(llmax(climb_m, 2.f * cell * std::tan(slope_deg * DEG_TO_RAD)) / unit));
        return p;
    }

    void walkRules(S32 width, const U32* start, const U16* bottom, const U16* top, const U8* code_in, const WalkParams& p, U8* code_out,
                   U8* pre_rim, U8* walk)
    {
        constexpr S32 OPEN = 0xffff;
        const size_t columns = static_cast<size_t>(width) * width;
        // Step-up, then headroom: a column at a time.
        for (size_t c = 0; c < columns; ++c)
        {
            const U32 a = start[c], b = start[c + 1];
            bool prev_walk = false;
            U8 prev_code = 0;
            for (U32 k = a; k < b; ++k)
            {
                const bool walks = code_in[k] != 0;
                U8 code = code_in[k];
                if (!walks && prev_walk && std::abs(static_cast<S32>(top[k]) - static_cast<S32>(top[k - 1])) <= p.mClimb)
                {
                    code = prev_code;
                }
                code_out[k] = code;
                prev_walk = walks;
                prev_code = code;
                const S32 ceiling = k + 1 < b ? bottom[k + 1] : OPEN;
                walk[k] = code != 0 && ceiling - static_cast<S32>(top[k]) >= p.mHeight;
            }
        }
        if (pre_rim)
        {
            std::copy(walk, walk + start[columns], pre_rim);
        }
        // The rim: it reads the neighbours' heights alone, so the order it clears in does not matter.
        static const S32 DX[4] = { -1, 0, 1, 0 };
        static const S32 DY[4] = { 0, 1, 0, -1 };
        for (S32 y = 0; y < width; ++y)
        {
            for (S32 x = 0; x < width; ++x)
            {
                const size_t c = static_cast<size_t>(x) + static_cast<size_t>(y) * width;
                const U32 a = start[c], b = start[c + 1];
                for (U32 k = a; k < b; ++k)
                {
                    if (!walk[k])
                    {
                        continue;
                    }
                    const S32 bot = top[k];
                    const S32 tp = k + 1 < b ? bottom[k + 1] : OPEN;
                    S32 min_drop = OPEN;
                    S32 reach_lo = bot, reach_hi = bot;
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const S32 nx = x + DX[d], ny = y + DY[d];
                        if (nx < 0 || ny < 0 || nx >= width || ny >= width)
                        {
                            min_drop = std::min(min_drop, -p.mLedge - bot);
                            continue;
                        }
                        const size_t n = static_cast<size_t>(nx) + static_cast<size_t>(ny) * width;
                        const U32 na = start[n], nb = start[n + 1];
                        // Below the neighbour's first span, a floor mLedge under the grid's bottom.
                        const S32 first = na < nb ? bottom[na] : OPEN;
                        if (std::min(tp, first) - std::max(bot, -p.mLedge) > p.mHeight)
                        {
                            min_drop = std::min(min_drop, -p.mLedge - bot);
                        }
                        for (U32 m = na; m < nb; ++m)
                        {
                            const S32 nbot = top[m];
                            const S32 ntop = m + 1 < nb ? bottom[m + 1] : OPEN;
                            if (std::min(tp, ntop) - std::max(bot, nbot) > p.mHeight)
                            {
                                min_drop = std::min(min_drop, nbot - bot);
                                if (std::abs(nbot - bot) <= p.mLedge)
                                {
                                    reach_lo = std::min(reach_lo, nbot);
                                    reach_hi = std::max(reach_hi, nbot);
                                }
                            }
                        }
                    }
                    if (min_drop < -p.mLedge || reach_hi - reach_lo > p.mLedge)
                    {
                        walk[k] = 0;
                    }
                }
            }
        }
    }

    bool leafWindow(const ColumnFrame& frame, const AirSoup& soup, std::vector<U8>& in_window, S32* rect)
    {
        if (!soup.mLeaves)
        {
            return false;
        }
        const Frame g(frame);
        const S32 W = g.mW;
        in_window.assign(static_cast<size_t>(W) * W, 0);
        rect[0] = rect[1] = W;
        rect[2] = rect[3] = 0;
        bool any = false;
        for (S32 t = 0; t < soup.ntris(); ++t)
        {
            if (!(soup.mFlags[t] & SOUP_LEAF))
            {
                continue;
            }
            S32 r[4];
            cellsOf(g, soup.tri(t), 1, r);
            for (S32 iy = std::max(0, r[1]); iy < std::min(W, r[3]); ++iy)
            {
                for (S32 ix = std::max(0, r[0]); ix < std::min(W, r[2]); ++ix)
                {
                    in_window[ix + static_cast<size_t>(iy) * W] = 1;
                }
            }
            rect[0] = std::min(rect[0], r[0] - 1);
            rect[1] = std::min(rect[1], r[1] - 1);
            rect[2] = std::max(rect[2], r[2] + 1);
            rect[3] = std::max(rect[3], r[3] + 1);
            any = true;
        }
        return any;
    }

    void walkColumns(ColumnRaster& raster, const ColumnWalkInput& input, const ColumnRaster* window, const U8* in_window, ColumnRasterStats* stats)
    {
        thread_local std::vector<U8> code, pre, walk, view_code, view_out, view_walk;
        thread_local std::vector<U32> view_start;
        thread_local std::vector<U16> view_bottom, view_top;
        auto clock = std::chrono::steady_clock::now();
        const S32 W = raster.mFrame.mWidth;
        const size_t n = raster.spans();
        code.resize(n);
        pre.resize(n);
        walk.resize(n);
        {
            SK_WORLD_ZONE("air.raster.walk.top");
            walkRules(W, raster.mStart.data(), raster.mBottom.data(), raster.mTop.data(), raster.mCode.data(), input.mTop, code.data(), pre.data(),
                      walk.data());
        }
        if (stats)
        {
            stats->mTopWalkMs += msSince(clock);
        }

        // AVATAR's view: its crop, codes it does not walk masked, the window's columns in place of the main's.
        {
            SK_WORLD_ZONE("air.raster.walk.avatar");
            const S32 inset = std::clamp(input.mAvatarInset, 0, W / 2);
            const S32 cw = W - 2 * inset;
            const bool windowed = window && in_window;
            view_start.resize(static_cast<size_t>(cw) * cw + 1);
            view_bottom.clear();
            view_top.clear();
            view_code.clear();
            view_bottom.reserve(n);
            view_top.reserve(n);
            view_code.reserve(n);
            for (S32 y = 0; y < cw; ++y)
            {
                for (S32 x = 0; x < cw; ++x)
                {
                    const size_t c = static_cast<size_t>(x + inset) + static_cast<size_t>(y + inset) * W;
                    view_start[x + static_cast<size_t>(y) * cw] = static_cast<U32>(view_top.size());
                    const ColumnRaster& from = windowed && in_window[c] ? *window : raster;
                    for (U32 k = from.mStart[c]; k < from.mStart[c + 1]; ++k)
                    {
                        view_bottom.push_back(from.mBottom[k]);
                        view_top.push_back(from.mTop[k]);
                        view_code.push_back(from.mCode[k] >= AREA_CODE_BASE ? from.mCode[k] : 0);
                    }
                }
            }
            view_start[static_cast<size_t>(cw) * cw] = static_cast<U32>(view_top.size());
            view_out.resize(view_top.size());
            view_walk.resize(view_top.size());
            walkRules(cw, view_start.data(), view_bottom.data(), view_top.data(), view_code.data(), input.mAvatar, view_out.data(), nullptr,
                      view_walk.data());
            if (stats)
            {
                U64 walk_hash = 0x9e3779b97f4a7c15ull, area_hash = walk_hash;
                for (size_t v = 0; v + 1 < view_start.size(); ++v)
                {
                    walk_hash = mix(walk_hash, v);
                    area_hash = mix(area_hash, v);
                    for (U32 k = view_start[v]; k < view_start[v + 1]; ++k)
                    {
                        const U64 span = static_cast<U64>(view_bottom[k]) | static_cast<U64>(view_top[k]) << 16;
                        walk_hash = mix(walk_hash, span | static_cast<U64>(view_walk[k]) << 32);
                        area_hash = mix(area_hash, span | static_cast<U64>(view_walk[k] ? areaOfCode(view_out[k]) : 0) << 32);
                    }
                }
                stats->mAvatarWalkHash = walk_hash;
                stats->mAvatarAreaHash = area_hash;
            }

            // Back onto the raster's spans: its own where the view shares the column, else a span of the window's at that top.
            for (S32 y = 0; y < cw; ++y)
            {
                for (S32 x = 0; x < cw; ++x)
                {
                    const size_t c = static_cast<size_t>(x + inset) + static_cast<size_t>(y + inset) * W;
                    const size_t v = x + static_cast<size_t>(y) * cw;
                    const U32 a = raster.mStart[c], b = raster.mStart[c + 1];
                    if (!(windowed && in_window[c]))
                    {
                        for (U32 k = a; k < b; ++k)
                        {
                            raster.mWalk[k] |= view_walk[view_start[v] + (k - a)] ? AVATAR_WALK : 0;
                        }
                        continue;
                    }
                    for (U32 k = a; k < b; ++k)
                    {
                        for (U32 m = view_start[v]; m < view_start[v + 1]; ++m)
                        {
                            if (view_top[m] == raster.mTop[k] && view_walk[m])
                            {
                                raster.mWalk[k] |= AVATAR_WALK;
                                break;
                            }
                        }
                    }
                }
            }
        }
        for (size_t k = 0; k < n; ++k)
        {
            raster.mCode[k] = code[k];
            raster.mWalk[k] |= (walk[k] ? TOP_WALK : 0) | (pre[k] ? TOP_PRE_LEDGE : 0);
        }
        if (stats)
        {
            stats->mAvatarWalkMs += msSince(clock);
        }
    }
}
