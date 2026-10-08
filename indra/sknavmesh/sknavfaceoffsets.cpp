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
#include "sknavairdistil.h"
#include "sknavwalls.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

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

    FaceProbe airFaceProbe(const TileAir& air, S32 climb)
    {
        return [&air, climb](const LLVector3& at, S32 dir, F32 reach) -> F32 {
            const F32 cell = air.mCell;
            const S32 floor = static_cast<S32>(std::lround((at.mV[VZ] - air.mOrigin.mV[VZ]) / air.mUnit));
            const S32 lo = floor + climb + 1, hi = floor + static_cast<S32>(std::lround(FACE_BAND_TOP_M / air.mUnit));
            const S32 axis = dir < 2 ? VX : VY;
            const F32 sgn = (dir & 1) ? -1.f : 1.f;
            S32 ix = static_cast<S32>(std::floor((at.mV[VX] - air.mOrigin.mV[VX]) / cell));
            S32 iy = static_cast<S32>(std::floor((at.mV[VY] - air.mOrigin.mV[VY]) / cell));
            const S32 steps = static_cast<S32>(std::ceil(reach / cell)) + 1;
            for (S32 k = 0; k <= steps; ++k, (axis == VX ? ix : iy) += (dir & 1) ? -1 : 1)
            {
                if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
                {
                    return -1.f;
                }
                const S32 leaf = findLeafByKey(air, ix, iy);
                if (leaf < 0)
                {
                    continue;
                }
                const AirLeaf& l = air.mLeaves[leaf];
                bool blocked = l.mGapCount > 0;
                for (U32 g = l.mFirstGap; g < l.mFirstGap + l.mGapCount && blocked; ++g)
                {
                    blocked = !(air.mGaps[g].mFloor <= lo && air.mGaps[g].mCeiling >= hi);
                }
                if (!blocked)
                {
                    continue;
                }
                // The near side of the solid cell, moved in to its true face.
                const F32 base = (axis == VX ? air.mOrigin.mV[VX] + ix * cell : air.mOrigin.mV[VY] + iy * cell) + ((dir & 1) ? cell : 0.f);
                const F32 face = base + sgn * faceOffset(air.mFaceOffsets, faceKey(ix, iy, air.mWidth, dir)) * cell;
                const F32 d = (face - at.mV[axis]) * sgn;
                return d <= reach ? std::max(0.f, d) : -1.f;
            }
            return -1.f;
        };
    }

    void alignWallEdges(U8* data, S32 size, F32 radius, F32 cell, const FaceProbe& probe, EdgeAlignStats* stats)
    {
        if (!data || size < static_cast<S32>(sizeof(dtMeshHeader)))
        {
            return;
        }
        dtMeshHeader* header = reinterpret_cast<dtMeshHeader*>(data);
        if (header->magic != DT_NAVMESH_MAGIC || header->version != DT_NAVMESH_VERSION)
        {
            return;
        }
        U8* p = data + dtAlign4(sizeof(dtMeshHeader));
        F32* verts = reinterpret_cast<F32*>(p);
        p += dtAlign4(sizeof(F32) * 3 * header->vertCount);
        dtPoly* polys = reinterpret_cast<dtPoly*>(p);
        p += dtAlign4(sizeof(dtPoly) * header->polyCount);
        p += dtAlign4(sizeof(dtLink) * header->maxLinkCount);
        dtPolyDetail* details = reinterpret_cast<dtPolyDetail*>(p);
        p += dtAlign4(sizeof(dtPolyDetail) * header->detailMeshCount);
        F32* dverts = reinterpret_cast<F32*>(p);
        p += dtAlign4(sizeof(F32) * 3 * header->detailVertCount);
        p += dtAlign4(sizeof(U8) * 4 * header->detailTriCount);
        dtBVNode* nodes = reinterpret_cast<dtBVNode*>(p);

        // Recast space is SL (y, z, x): SL x is rc z, SL y is rc x.
        auto sl = [&](const F32* v) { return LLVector3(v[2], v[0], v[1]); };
        const F32 reach = radius + 2.f * cell;
        struct Push
        {
            LLVector2 mN;
            F32 mShift;
        };
        std::vector<std::vector<Push>> pushes(header->vertCount);
        struct Pushed
        {
            U16 mA, mB;
            S32 mDir;
            F32 mBefore;
        };
        std::vector<Pushed> pushed;
        for (S32 i = 0; i < header->polyCount; ++i)
        {
            const dtPoly& poly = polys[i];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount < 3)
            {
                continue;
            }
            LLVector2 centre;
            for (S32 j = 0; j < poly.vertCount; ++j)
            {
                const LLVector3 v = sl(&verts[poly.verts[j] * 3]);
                centre += LLVector2(v.mV[VX], v.mV[VY]);
            }
            centre /= static_cast<F32>(poly.vertCount);
            for (S32 j = 0; j < poly.vertCount; ++j)
            {
                if (poly.neis[j] != 0)
                {
                    continue;   // a neighbour, or a portal to another tile
                }
                const U16 ia = poly.verts[j], ib = poly.verts[(j + 1) % poly.vertCount];
                const LLVector3 a = sl(&verts[ia * 3]), b = sl(&verts[ib * 3]);
                LLVector2 d(b.mV[VX] - a.mV[VX], b.mV[VY] - a.mV[VY]);
                const F32 len = d.normVec();
                if (len < 1e-4f)
                {
                    continue;
                }
                LLVector2 n(d.mV[VY], -d.mV[VX]);
                if ((LLVector2(a.mV[VX], a.mV[VY]) - centre) * n < 0.f)
                {
                    n = -n;
                }
                // Walls on the grid's axes only: a sloping edge has no one side to read.
                S32 dir;
                if (std::fabs(n.mV[VX]) > 0.999f)
                {
                    dir = n.mV[VX] > 0.f ? 0 : 1;
                }
                else if (std::fabs(n.mV[VY]) > 0.999f)
                {
                    dir = n.mV[VY] > 0.f ? 2 : 3;
                }
                else
                {
                    if (stats)
                    {
                        ++stats->mOblique;
                    }
                    continue;
                }
                // The nearest true face across the whole edge, sampled a cell apart; open anywhere, no wall.
                F32 nearest = 1e30f;
                const S32 samples = std::max(1, static_cast<S32>(std::ceil(len / cell)));
                for (S32 k = 0; k < samples && nearest >= 0.f; ++k)
                {
                    const F32 t = (k + 0.5f) / samples;
                    const LLVector3 at = a + (b - a) * t;
                    const F32 f = probe(at, dir, reach);
                    nearest = f < 0.f ? -1.f : std::min(nearest, f);
                }
                if (nearest < 0.f)
                {
                    continue;
                }
                const F32 shift = std::min(nearest - radius, 2.f * cell);
                if (stats)
                {
                    ++stats->mWallEdges;
                    stats->mErrBefore += std::fabs(nearest - radius);
                }
                if (shift <= 0.f)
                {
                    if (stats)
                    {
                        stats->mErrAfter += std::fabs(nearest - radius);
                    }
                    continue;
                }
                pushes[ia].push_back({ n, shift });
                pushes[ib].push_back({ n, shift });
                pushed.push_back({ ia, ib, dir, std::fabs(nearest - radius) });
            }
        }
        // Each vertex moves so every wall edge it ends sits its shift further out.
        const F32 eps = 1e-3f;
        std::vector<LLVector2> moved(header->vertCount);
        for (S32 v = 0; v < header->vertCount; ++v)
        {
            std::vector<Push>& ps = pushes[v];
            if (ps.empty())
            {
                continue;
            }
            LLVector2 m;
            const Push& p0 = ps[0];
            const Push* p1 = nullptr;
            for (size_t k = 1; k < ps.size(); ++k)
            {
                if (std::fabs(ps[k].mN * p0.mN) < 0.99f)
                {
                    p1 = &ps[k];
                }
            }
            if (p1)
            {
                // Two walls meet: the point their moved lines share.
                const F32 det = p0.mN.mV[VX] * p1->mN.mV[VY] - p0.mN.mV[VY] * p1->mN.mV[VX];
                m.set((p0.mShift * p1->mN.mV[VY] - p1->mShift * p0.mN.mV[VY]) / det, (p1->mShift * p0.mN.mV[VX] - p0.mShift * p1->mN.mV[VX]) / det);
            }
            else
            {
                F32 s = ps[0].mShift;
                for (const Push& q : ps)
                {
                    s = std::min(s, q.mShift);
                }
                m = p0.mN * s;
            }
            // A vertex on the tile's border stays, so the tile across it, which reads other air, agrees.
            const F32* rc = &verts[v * 3];
            const bool border = std::fabs(rc[0] - header->bmin[0]) < eps || std::fabs(rc[0] - header->bmax[0]) < eps ||
                                std::fabs(rc[2] - header->bmin[2]) < eps || std::fabs(rc[2] - header->bmax[2]) < eps;
            moved[v] = border ? LLVector2() : m;
        }
        // A move that would leave any polygon it touches not convex is dropped, until none would.
        auto convex = [&](const dtPoly& poly) {
            S32 sign = 0;
            for (S32 j = 0; j < poly.vertCount; ++j)
            {
                LLVector2 q[3];
                for (S32 k = 0; k < 3; ++k)
                {
                    const U16 v = poly.verts[(j + k) % poly.vertCount];
                    const LLVector3 at = sl(&verts[v * 3]);
                    q[k] = LLVector2(at.mV[VX], at.mV[VY]) + moved[v];
                }
                const F32 cr = (q[1].mV[VX] - q[0].mV[VX]) * (q[2].mV[VY] - q[1].mV[VY]) - (q[1].mV[VY] - q[0].mV[VY]) * (q[2].mV[VX] - q[1].mV[VX]);
                if (std::fabs(cr) > 1e-6f)
                {
                    const S32 s = cr > 0.f ? 1 : -1;
                    if (sign && s != sign)
                    {
                        return false;
                    }
                    sign = s;
                }
            }
            return true;
        };
        for (bool changed = true; changed;)
        {
            changed = false;
            for (S32 i = 0; i < header->polyCount; ++i)
            {
                const dtPoly& poly = polys[i];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || convex(poly))
                {
                    continue;
                }
                for (S32 j = 0; j < poly.vertCount; ++j)
                {
                    if (!moved[poly.verts[j]].isExactlyZero())
                    {
                        moved[poly.verts[j]].setZero();
                        changed = true;
                        if (stats)
                        {
                            ++stats->mDropped;
                        }
                    }
                }
            }
        }
        // Detail vertices on a moved edge go with it.
        for (S32 i = 0; i < header->detailMeshCount && i < header->polyCount; ++i)
        {
            const dtPoly& poly = polys[i];
            const dtPolyDetail& pd = details[i];
            for (U32 k = 0; k < pd.vertCount; ++k)
            {
                F32* dv = &dverts[(pd.vertBase + k) * 3];
                for (S32 j = 0; j < poly.vertCount; ++j)
                {
                    const U16 ia = poly.verts[j], ib = poly.verts[(j + 1) % poly.vertCount];
                    if (moved[ia].isExactlyZero() && moved[ib].isExactlyZero())
                    {
                        continue;
                    }
                    const F32* va = &verts[ia * 3];
                    const F32* vb = &verts[ib * 3];
                    const F32 ex = vb[0] - va[0], ez = vb[2] - va[2], l2 = ex * ex + ez * ez;
                    if (l2 < 1e-8f)
                    {
                        continue;
                    }
                    const F32 t = ((dv[0] - va[0]) * ex + (dv[2] - va[2]) * ez) / l2;
                    const F32 px = va[0] + ex * t - dv[0], pz = va[2] + ez * t - dv[2];
                    if (t < 0.f || t > 1.f || px * px + pz * pz > eps * eps)
                    {
                        continue;
                    }
                    const LLVector2 m = moved[ia] * (1.f - t) + moved[ib] * t;
                    dv[0] += m.mV[VY];
                    dv[2] += m.mV[VX];
                    break;
                }
            }
        }
        // Kept within the tile's bounds.
        U32 count = 0;
        for (S32 v = 0; v < header->vertCount; ++v)
        {
            if (!moved[v].isExactlyZero())
            {
                verts[v * 3 + 0] = llclamp(verts[v * 3 + 0] + moved[v].mV[VY], header->bmin[0], header->bmax[0]);
                verts[v * 3 + 2] = llclamp(verts[v * 3 + 2] + moved[v].mV[VX], header->bmin[2], header->bmax[2]);
                ++count;
            }
        }
        if (stats)
        {
            stats->mMovedVerts += count;
        }
        // The residual after the moves, by the same probe, over the edges that were pushed.
        if (stats)
        {
            for (const Pushed& e : pushed)
            {
                const LLVector3 a = sl(&verts[e.mA * 3]), b = sl(&verts[e.mB * 3]);
                const F32 len = dist_vec(LLVector2(a.mV[VX], a.mV[VY]), LLVector2(b.mV[VX], b.mV[VY]));
                F32 nearest = 1e30f;
                const S32 samples = std::max(1, static_cast<S32>(std::ceil(len / cell)));
                for (S32 k = 0; k < samples && nearest >= 0.f; ++k)
                {
                    const F32 f = probe(a + (b - a) * ((k + 0.5f) / samples), e.mDir, reach);
                    nearest = f < 0.f ? -1.f : std::min(nearest, f);
                }
                stats->mErrAfter += nearest >= 0.f ? std::fabs(nearest - radius) : e.mBefore;
                ++stats->mMovedEdges;
            }
        }
        // The BV tree's boxes, refitted to the moved polygons.
        if (header->bvNodeCount > 0)
        {
            const F32 q = header->bvQuantFactor;
            std::function<S32(S32)> refit = [&](S32 k) -> S32 {
                dtBVNode& node = nodes[k];
                if (node.i >= 0)
                {
                    const dtPoly& poly = polys[node.i];
                    F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                    auto take = [&](const F32* v) {
                        for (S32 a = 0; a < 3; ++a)
                        {
                            lo[a] = std::min(lo[a], v[a]);
                            hi[a] = std::max(hi[a], v[a]);
                        }
                    };
                    for (S32 j = 0; j < poly.vertCount; ++j)
                    {
                        take(&verts[poly.verts[j] * 3]);
                    }
                    if (node.i < header->detailMeshCount)
                    {
                        const dtPolyDetail& pd = details[node.i];
                        for (U32 j = 0; j < pd.vertCount; ++j)
                        {
                            take(&dverts[(pd.vertBase + j) * 3]);
                        }
                    }
                    for (S32 a = 0; a < 3; ++a)
                    {
                        // Only ever grown: a box that still holds its polygon is no worse for being loose.
                        node.bmin[a] = std::min<U16>(node.bmin[a], static_cast<U16>(llclamp(static_cast<S32>(std::floor((lo[a] - header->bmin[a]) * q)), 0, 0xffff)));
                        node.bmax[a] = std::max<U16>(node.bmax[a], static_cast<U16>(llclamp(static_cast<S32>(std::ceil((hi[a] - header->bmin[a]) * q)), 0, 0xffff)));
                    }
                    return k + 1;
                }
                const S32 end = k - node.i;
                S32 c = k + 1;
                while (c < end)
                {
                    const S32 next = refit(c);
                    for (S32 a = 0; a < 3; ++a)
                    {
                        node.bmin[a] = std::min(node.bmin[a], nodes[c].bmin[a]);
                        node.bmax[a] = std::max(node.bmax[a], nodes[c].bmax[a]);
                    }
                    c = next;
                }
                return end;
            };
            for (S32 k = 0; k < header->bvNodeCount;)
            {
                k = refit(k);
            }
        }
    }
}
