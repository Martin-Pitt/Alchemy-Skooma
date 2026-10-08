/**
 * @file sknavfaceoffsets.cpp
 * @brief Where a wall's true face sits inside the solid cell the raster rounded it out to.
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

#include "sknavfaceoffsets.h"

#include "sknavair.h"
#include "sknavwalls.h"

#include "Recast.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sknav
{
    namespace
    {
        constexpr S32 DX[4] = { 1, -1, 0, 0 };
        constexpr S32 DY[4] = { 0, 0, 1, -1 };   // a step d from the air meets the solid's side d, which looks back along it

        S32 clipPlane(const LLVector3* in, S32 n, LLVector3* out, S32 axis, F32 value, bool keep_above)
        {
            S32 m = 0;
            for (S32 i = 0; i < n; ++i)
            {
                const LLVector3& a = in[i];
                const LLVector3& b = in[(i + 1) % n];
                const F32 da = keep_above ? a.mV[axis] - value : value - a.mV[axis];
                const F32 db = keep_above ? b.mV[axis] - value : value - b.mV[axis];
                if (da >= 0.f)
                {
                    out[m++] = a;
                }
                if ((da >= 0.f) != (db >= 0.f))
                {
                    const F32 t = da / (da - db);
                    out[m] = a + (b - a) * t;
                    out[m].mV[axis] = value;
                    ++m;
                }
            }
            return m;
        }

        F32 depthOf(F32 extreme, const LLVector3& lo, const LLVector3& hi, S32 side)
        {
            switch (side)
            {
                case 0: return extreme - lo.mV[VX];
                case 1: return hi.mV[VX] - extreme;
                case 2: return extreme - lo.mV[VY];
                default: return hi.mV[VY] - extreme;
            }
        }

        bool blockedIn(const AirGrid& grid, S32 ix, S32 iy, S32 lo, S32 hi)
        {
            const U32 count = grid.count(ix, iy);
            if (!count)
            {
                return false;
            }
            const AirGap* g = grid.gaps(ix, iy);
            for (U32 i = 0; i < count; ++i)
            {
                if (g[i].mFloor <= lo && g[i].mCeiling >= hi)
                {
                    return false;
                }
            }
            return true;
        }

        constexpr F32 FACE_TOUCH_M = 1e-3f;

        struct Request
        {
            U32 mCol;
            U8 mSide;
            U16 mFloor;
        };
    }

    bool clippedExtreme(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& lo, const LLVector3& hi, S32 axis,
                        bool lowest, F32& out)
    {
        LLVector3 buf[2][12];
        buf[0][0] = a;
        buf[0][1] = b;
        buf[0][2] = c;
        S32 n = 3, cur = 0;
        for (S32 k = 0; k < 3 && n; ++k)
        {
            n = clipPlane(buf[cur], n, buf[cur ^ 1], k, lo.mV[k], true);
            cur ^= 1;
            n = n ? clipPlane(buf[cur], n, buf[cur ^ 1], k, hi.mV[k], false) : 0;
            cur ^= 1;
        }
        if (!n)
        {
            return false;
        }
        F32 e = buf[cur][0].mV[axis];
        for (S32 i = 1; i < n; ++i)
        {
            e = lowest ? std::min(e, buf[cur][i].mV[axis]) : std::max(e, buf[cur][i].mV[axis]);
        }
        out = e;
        return true;
    }

    F32 trueFaceDepth(const std::vector<LLVector3>& tris, const LLVector3& lo, const LLVector3& hi, S32 side)
    {
        const S32 axis = side < 2 ? VX : VY;
        const bool lowest = (side & 1) == 0;
        F32 best = -1.f;
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
        {
            F32 e;
            if (clippedExtreme(tris[i], tris[i + 1], tris[i + 2], lo, hi, axis, lowest, e))
            {
                const F32 d = std::max(0.f, depthOf(e, lo, hi, side));
                best = best < 0.f ? d : std::min(best, d);
            }
        }
        return best;
    }

    void computeFaceOffsets(const AirGrid& grid, const F32* verts, const S32* tris, S32 ntris, S32 climb, std::vector<FaceOffset>& out,
                            FaceOffsetStats* stats, S32 trace_every, std::vector<FaceOffsetTrace>* trace)
    {
        const auto started = std::chrono::steady_clock::now();
        out.clear();
        if (grid.empty() || grid.mCell <= 0.f || grid.mUnit <= 0.f)
        {
            return;
        }
        const S32 w = grid.mWidth, h = grid.mHeight;
        const S32 band_top = static_cast<S32>(std::lround(FACE_BAND_TOP_M / grid.mUnit));
        // Every solid side a floor's jamb band meets.
        std::vector<Request> requests;
        std::vector<U8> needed(static_cast<size_t>(w) * h, 0);
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const U32 count = grid.count(ix, iy);
                const AirGap* g = grid.gaps(ix, iy);
                for (U32 i = 0; i < count; ++i)
                {
                    if (!(g[i].mFlags & AirGap::AVATAR_FLOOR))
                    {
                        continue;
                    }
                    const S32 lo = g[i].mFloor + climb + 1, hi = g[i].mFloor + band_top;
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const S32 nx = ix + DX[d], ny = iy + DY[d];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || !blockedIn(grid, nx, ny, lo, hi))
                        {
                            continue;
                        }
                        const U32 col = static_cast<U32>(nx + ny * w);
                        requests.push_back({ col, static_cast<U8>(d), g[i].mFloor });
                        needed[col] = 1;
                    }
                }
            }
        }
        if (stats)
        {
            stats->mSides += static_cast<U32>(requests.size());
        }
        // The triangles over each column asked about, as a CSR.
        std::vector<LLVector3> sl(static_cast<size_t>(ntris) * 3);
        std::vector<U32> start(static_cast<size_t>(w) * h + 1, 0);
        std::vector<S32> rect(static_cast<size_t>(ntris) * 4);
        std::vector<F32> bounds(static_cast<size_t>(ntris) * 6);     // x0, x1, y0, y1, z0, z1
        const F32 ox = grid.mOrigin.mV[VX], oy = grid.mOrigin.mV[VY], cell = grid.mCell;
        for (S32 t = 0; t < ntris; ++t)
        {
            F32* b = &bounds[t * 6];
            b[0] = b[2] = b[4] = 1e30f;
            b[1] = b[3] = b[5] = -1e30f;
            for (S32 k = 0; k < 3; ++k)
            {
                const F32* v = &verts[tris[t * 3 + k] * 3];
                LLVector3& p = sl[t * 3 + k];
                p.set(v[2], v[0], v[1]);
                for (S32 a = 0; a < 3; ++a)
                {
                    b[a * 2] = std::min(b[a * 2], p.mV[a]);
                    b[a * 2 + 1] = std::max(b[a * 2 + 1], p.mV[a]);
                }
            }
            const F32 x0 = b[0], x1 = b[1], y0 = b[2], y1 = b[3];
            S32* r = &rect[t * 4];
            r[0] = std::max(0, static_cast<S32>(std::floor((x0 - ox) / cell)));
            r[1] = std::min(w - 1, static_cast<S32>(std::floor((x1 - ox) / cell)));
            r[2] = std::max(0, static_cast<S32>(std::floor((y0 - oy) / cell)));
            r[3] = std::min(h - 1, static_cast<S32>(std::floor((y1 - oy) / cell)));
            for (S32 y = r[2]; y <= r[3]; ++y)
            {
                for (S32 x = r[0]; x <= r[1]; ++x)
                {
                    start[x + y * w + 1] += needed[x + y * w];
                }
            }
        }
        for (size_t c = 0; c + 1 < start.size(); ++c)
        {
            start[c + 1] += start[c];
        }
        std::vector<U32> fill(start.begin(), start.end() - 1);
        std::vector<S32> list(start.back());
        for (S32 t = 0; t < ntris; ++t)
        {
            const S32* r = &rect[t * 4];
            for (S32 y = r[2]; y <= r[3]; ++y)
            {
                for (S32 x = r[0]; x <= r[1]; ++x)
                {
                    if (needed[x + y * w])
                    {
                        list[fill[x + y * w]++] = t;
                    }
                }
            }
        }
        // Each side's face: the nearest the triangles in its column come to the air.
        U32 measured = 0;
        for (const Request& q : requests)
        {
            const S32 ix = static_cast<S32>(q.mCol % w), iy = static_cast<S32>(q.mCol / w);
            const LLVector3 lo(ox + ix * cell, oy + iy * cell, grid.height(static_cast<U16>(q.mFloor + climb + 1)));
            const LLVector3 hi(lo.mV[VX] + cell, lo.mV[VY] + cell, grid.height(static_cast<U16>(q.mFloor + band_top)));
            // A triangle that only touches the box, as a slab on its top or a wall in its boundary does, is no face in it.
            const LLVector3 in_lo = lo + LLVector3(FACE_TOUCH_M, FACE_TOUCH_M, FACE_TOUCH_M), in_hi = hi - LLVector3(FACE_TOUCH_M, FACE_TOUCH_M, FACE_TOUCH_M);
            const S32 axis = q.mSide < 2 ? VX : VY;
            const bool lowest = (q.mSide & 1) == 0;
            F32 depth = -1.f;
            for (U32 k = start[q.mCol]; k < start[q.mCol + 1]; ++k)
            {
                const F32* b = &bounds[list[k] * 6];
                // Out of the band, or no nearer the air anywhere than the face found so far.
                const F32 reach = lowest ? b[axis * 2] - lo.mV[axis] : hi.mV[axis] - b[axis * 2 + 1];
                if (b[5] < lo.mV[VZ] || b[4] > hi.mV[VZ] || (depth >= 0.f && reach >= depth))
                {
                    continue;
                }
                const LLVector3* p = &sl[list[k] * 3];
                F32 e;
                if (clippedExtreme(p[0], p[1], p[2], in_lo, in_hi, axis, lowest, e))
                {
                    const F32 d = std::max(0.f, depthOf(e, lo, hi, q.mSide));
                    depth = depth < 0.f ? d : std::min(depth, d);
                }
            }
            if (depth < 0.f)
            {
                continue;
            }
            const S32 steps = std::clamp(static_cast<S32>(std::floor(depth / cell * FACE_OFFSET_STEPS + 1e-3f)), 0, FACE_OFFSET_STEPS);
            if (stats)
            {
                ++stats->mMeasured;
                stats->mTrueSum += depth;
                stats->mResidualSum += depth - steps * cell / FACE_OFFSET_STEPS;
            }
            const F32 cx = lo.mV[VX] + cell * 0.5f, cy = lo.mV[VY] + cell * 0.5f;
            const bool boxed = !stats || stats->mTraceBox[2] <= stats->mTraceBox[0] ||
                               (cx >= stats->mTraceBox[0] && cx <= stats->mTraceBox[2] && cy >= stats->mTraceBox[1] && cy <= stats->mTraceBox[3]);
            if (trace && trace_every > 0 && boxed && measured++ % trace_every == 0)
            {
                FaceOffsetTrace tr;
                tr.mX0 = lo.mV[VX];
                tr.mY0 = lo.mV[VY];
                tr.mSide = q.mSide;
                tr.mFloor = q.mFloor;
                tr.mZLo = lo.mV[VZ];
                tr.mZHi = hi.mV[VZ];
                tr.mOffset = static_cast<U8>(steps);
                for (U32 k = start[q.mCol]; k < start[q.mCol + 1]; ++k)
                {
                    tr.mTris.insert(tr.mTris.end(), &sl[list[k] * 3], &sl[list[k] * 3] + 3);
                }
                trace->push_back(std::move(tr));
            }
            out.push_back(FaceOffset(q.mCol * 4u + q.mSide, static_cast<U32>(steps)));
        }
        // One entry a side, its nearest face over the floors it was measured for; none at the boundary.
        std::sort(out.begin(), out.end(), [](const FaceOffset& a, const FaceOffset& b) { return a.mBits < b.mBits; });
        size_t n = 0;
        U32 last = U32_MAX;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (out[i].key() != last && out[i].offset() > 0)
            {
                out[n++] = out[i];
            }
            last = out[i].key();
        }
        out.resize(n);
        out.shrink_to_fit();
        if (stats)
        {
            stats->mMs += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
    }

    F32 faceOffset(const std::vector<FaceOffset>& table, U32 key)
    {
        const auto it = std::lower_bound(table.begin(), table.end(), key, [](const FaceOffset& f, U32 k) { return f.key() < k; });
        return it != table.end() && it->key() == key ? static_cast<F32>(it->offset()) / FACE_OFFSET_STEPS : 0.f;
    }

    void alignWallFaces(std::vector<WallFace>& faces, const F32* verts, const S32* tris, S32 ntris, F32 cell, F32 unit)
    {
        if (faces.empty() || ntris <= 0 || cell <= 0.f)
        {
            return;
        }
        // Triangles binned over a grid of the faces' extent.
        F32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const WallFace& f : faces)
        {
            x0 = std::min(x0, f.mMin.mV[VX] - cell);
            y0 = std::min(y0, f.mMin.mV[VY] - cell);
            x1 = std::max(x1, f.mMax.mV[VX] + cell);
            y1 = std::max(y1, f.mMax.mV[VY] + cell);
        }
        const S32 w = static_cast<S32>(std::ceil((x1 - x0) / cell)) + 1, h = static_cast<S32>(std::ceil((y1 - y0) / cell)) + 1;
        auto cellRect = [&](F32 ax, F32 bx, F32 ay, F32 by, S32* r) {
            r[0] = std::clamp(static_cast<S32>(std::floor((ax - x0) / cell)), 0, w - 1);
            r[1] = std::clamp(static_cast<S32>(std::floor((bx - x0) / cell)), 0, w - 1);
            r[2] = std::clamp(static_cast<S32>(std::floor((ay - y0) / cell)), 0, h - 1);
            r[3] = std::clamp(static_cast<S32>(std::floor((by - y0) / cell)), 0, h - 1);
        };
        std::vector<LLVector3> sl(static_cast<size_t>(ntris) * 3);
        std::vector<S32> rect(static_cast<size_t>(ntris) * 4);
        std::vector<U32> start(static_cast<size_t>(w) * h + 1, 0);
        for (S32 t = 0; t < ntris; ++t)
        {
            F32 ax = 1e30f, bx = -1e30f, ay = 1e30f, by = -1e30f;
            for (S32 k = 0; k < 3; ++k)
            {
                const F32* v = &verts[tris[t * 3 + k] * 3];
                LLVector3& p = sl[t * 3 + k];
                p.set(v[2], v[0], v[1]);
                ax = std::min(ax, p.mV[VX]);
                bx = std::max(bx, p.mV[VX]);
                ay = std::min(ay, p.mV[VY]);
                by = std::max(by, p.mV[VY]);
            }
            S32* r = &rect[t * 4];
            if (bx < x0 || ax > x1 || by < y0 || ay > y1)
            {
                r[0] = 1;
                r[1] = 0;
                continue;
            }
            cellRect(ax, bx, ay, by, r);
            for (S32 y = r[2]; y <= r[3]; ++y)
            {
                for (S32 x = r[0]; x <= r[1]; ++x)
                {
                    ++start[x + y * w + 1];
                }
            }
        }
        for (size_t c = 0; c + 1 < start.size(); ++c)
        {
            start[c + 1] += start[c];
        }
        std::vector<U32> fill(start.begin(), start.end() - 1);
        std::vector<S32> list(start.back());
        for (S32 t = 0; t < ntris; ++t)
        {
            const S32* r = &rect[t * 4];
            for (S32 y = r[2]; y <= r[3]; ++y)
            {
                for (S32 x = r[0]; x <= r[1]; ++x)
                {
                    list[fill[x + y * w]++] = t;
                }
            }
        }
        std::vector<S32> seen;
        std::vector<LLVector3> near_tris;
        for (WallFace& f : faces)
        {
            // The face's outward direction is the side it looks; the solid lies a cell behind it.
            const S32 side = f.mSide ^ 1;
            const S32 axis = side < 2 ? VX : VY;
            const F32 b = f.mMin.mV[axis];
            LLVector3 lo = f.mMin, hi = f.mMax;
            lo.mV[VZ] += unit;
            hi.mV[VZ] -= unit;
            if (hi.mV[VZ] <= lo.mV[VZ])
            {
                continue;
            }
            if ((side & 1) == 0)
            {
                lo.mV[axis] = b;
                hi.mV[axis] = b + cell;
            }
            else
            {
                lo.mV[axis] = b - cell;
                hi.mV[axis] = b;
            }
            S32 r[4];
            cellRect(lo.mV[VX], hi.mV[VX], lo.mV[VY], hi.mV[VY], r);
            seen.clear();
            near_tris.clear();
            for (S32 y = r[2]; y <= r[3]; ++y)
            {
                for (S32 x = r[0]; x <= r[1]; ++x)
                {
                    for (U32 k = start[x + y * w]; k < start[x + y * w + 1]; ++k)
                    {
                        seen.push_back(list[k]);
                    }
                }
            }
            std::sort(seen.begin(), seen.end());
            seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
            for (const S32 t : seen)
            {
                near_tris.insert(near_tris.end(), &sl[t * 3], &sl[t * 3] + 3);
            }
            const F32 depth = trueFaceDepth(near_tris, lo, hi, side);
            if (depth > 0.f)
            {
                const F32 move = (side & 1) == 0 ? depth : -depth;
                f.mMin.mV[axis] += move;
                f.mMax.mV[axis] += move;
            }
        }
    }

    void cropFaceOffsets(const std::vector<FaceOffset>& padded, S32 padded_width, S32 pad, S32 width, S32 height, std::vector<FaceOffset>& out)
    {
        out.clear();
        for (const FaceOffset& f : padded)
        {
            const S32 col = static_cast<S32>(f.key() >> 2);
            const S32 ix = col % padded_width - pad, iy = col / padded_width - pad;
            if (ix >= 0 && iy >= 0 && ix < width && iy < height)
            {
                out.push_back(FaceOffset(faceKey(ix, iy, width, static_cast<S32>(f.key() & 3u)), f.offset()));
            }
        }
        out.shrink_to_fit();
    }

    U32 recoverEroded(rcCompactHeightfield& chf, const std::vector<U8>& before, F32 radius, S32 shift, S32 grid_width, S32 grid_height,
                      const std::vector<FaceOffset>& table)
    {
        if (table.empty() || before.size() != static_cast<size_t>(chf.spanCount) || radius <= 0.f)
        {
            return 0;
        }
        const S32 reach = static_cast<S32>(std::ceil(radius / chf.cs)) + 1;
        const F32 r_cells = radius / chf.cs;
        // Only a span within reach of a face off its boundary can be given back.
        std::vector<U8> near_face(static_cast<size_t>(chf.width) * chf.height, 0);
        for (const FaceOffset& f : table)
        {
            if (f.offset() == 0)
            {
                continue;
            }
            const S32 col = static_cast<S32>(f.key() >> 2);
            const S32 cx = col / grid_width + shift, cz = col % grid_width + shift;
            for (S32 z = std::max(0, cz - reach - 1); z <= std::min(chf.height - 1, cz + reach + 1); ++z)
            {
                for (S32 x = std::max(0, cx - reach - 1); x <= std::min(chf.width - 1, cx + reach + 1); ++x)
                {
                    near_face[x + z * chf.width] = 1;
                }
            }
        }
        std::vector<U32> seen(static_cast<size_t>(chf.spanCount), 0);
        U32 stamp = 0, recovered = 0;
        struct Visit
        {
            S32 mX, mZ, mSpan;
        };
        std::vector<Visit> stack;
        for (S32 z = 0; z < chf.height; ++z)
        {
            for (S32 x = 0; x < chf.width; ++x)
            {
                const rcCompactCell& cell = chf.cells[x + z * chf.width];
                for (S32 i = static_cast<S32>(cell.index), n = static_cast<S32>(cell.index + cell.count); i < n && near_face[x + z * chf.width]; ++i)
                {
                    if (before[i] == RC_NULL_AREA || chf.areas[i] != RC_NULL_AREA)
                    {
                        continue;
                    }
                    // The nearest boundary over the connected floor round the span, at the cell faces and at the true ones.
                    ++stamp;
                    seen[i] = stamp;
                    stack.assign(1, { x, z, i });
                    F32 at_cells = 1e30f, at_faces = 1e30f;
                    while (!stack.empty() && at_faces >= r_cells)
                    {
                        const Visit v = stack.back();
                        stack.pop_back();
                        const rcCompactSpan& s = chf.spans[v.mSpan];
                        for (S32 dir = 0; dir < 4; ++dir)
                        {
                            const S32 dx = rcGetDirOffsetX(dir), dz = rcGetDirOffsetY(dir);
                            const S32 con = rcGetCon(s, dir);
                            const S32 nx = v.mX + dx, nz = v.mZ + dz;
                            S32 ni = -1;
                            if (con != RC_NOT_CONNECTED)
                            {
                                ni = static_cast<S32>(chf.cells[nx + nz * chf.width].index) + con;
                            }
                            if (ni >= 0 && before[ni] != RC_NULL_AREA)
                            {
                                if (std::abs(nx - x) <= reach && std::abs(nz - z) <= reach && seen[ni] != stamp)
                                {
                                    seen[ni] = stamp;
                                    stack.push_back({ nx, nz, ni });
                                }
                                continue;
                            }
                            // A boundary: its face by the offset of the solid beyond, in the grid's axes (ix along chf z).
                            const S32 ix = v.mZ - shift, iy = v.mX - shift, nix = ix + dz, niy = iy + dx;
                            const S32 side = dz == 1 ? 0 : dz == -1 ? 1 : dx == 1 ? 2 : 3;
                            F32 off = 0.f;
                            if (nix >= 0 && niy >= 0 && nix < grid_width && niy < grid_height)
                            {
                                off = faceOffset(table, faceKey(nix, niy, grid_width, side));
                            }
                            for (S32 k = 0; k < 2; ++k)
                            {
                                const F32 o = k ? off : 0.f;
                                F32 ddx, ddz;
                                if (dx != 0)
                                {
                                    const F32 line = v.mX + (dx > 0 ? 1.f : 0.f) + dx * o;
                                    ddx = std::max({ 0.f, line - (x + 1), x - line });
                                    ddz = static_cast<F32>(std::max({ 0, v.mZ - (z + 1), z - (v.mZ + 1) }));
                                }
                                else
                                {
                                    const F32 line = v.mZ + (dz > 0 ? 1.f : 0.f) + dz * o;
                                    ddz = std::max({ 0.f, line - (z + 1), z - line });
                                    ddx = static_cast<F32>(std::max({ 0, v.mX - (x + 1), x - (v.mX + 1) }));
                                }
                                F32& best = k ? at_faces : at_cells;
                                best = std::min(best, std::sqrt(ddx * ddx + ddz * ddz));
                            }
                        }
                    }
                    if (at_faces >= r_cells && at_cells < r_cells)
                    {
                        chf.areas[i] = before[i];
                        ++recovered;
                    }
                }
            }
        }
        return recovered;
    }
}
