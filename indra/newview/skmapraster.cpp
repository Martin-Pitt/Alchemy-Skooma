/**
 * @file skmapraster.cpp
 * @brief One mini-map layer's floorplan rasterised into a 256 x 256 cell, on a worker.
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

#include "skmapraster.h"

#include "skmappalette.h"

#include "sknavbuild.h"

#include <algorithm>
#include <cmath>

namespace skmap
{
    namespace
    {
        constexpr S32 N = CELL_TEXELS;
        constexpr S32 SUB = 2;                  ///< subsamples per texel a side
        /// Floors and the section are rasterised this many texels past the cell, so an outline at the cell's border
        /// sees the texels beyond it.
        constexpr S32 PAD = 2;
        constexpr S32 NP = N + 2 * PAD;
        constexpr S32 MP = NP * SUB;

        /// Premultiplied float image, composited in place.
        struct Image
        {
            std::vector<F32> mPx = std::vector<F32>(static_cast<size_t>(N) * N * 4, 0.f);

            void over(S32 x, S32 y, F32 r, F32 g, F32 b, F32 a)
            {
                if (a <= 0.f)
                {
                    return;
                }
                F32* p = &mPx[(static_cast<size_t>(y) * N + x) * 4];
                const F32 keep = 1.f - a;
                p[0] = r * a + p[0] * keep;
                p[1] = g * a + p[1] * keep;
                p[2] = b * a + p[2] * keep;
                p[3] = a + p[3] * keep;
            }
        };

        struct Frame
        {
            F32 mX0 = 0.f;      ///< the cell's south-west corner, scene-local
            F32 mY0 = 0.f;
            F32 mTexel = 1.f;

            LLVector2 texelCentre(S32 x, S32 y) const
            {
                return LLVector2(mX0 + (x + 0.5f) * mTexel, mY0 + (y + 0.5f) * mTexel);
            }
            /// Texel range a scene-local box covers, clipped to the cell; false when it misses.
            bool range(F32 minx, F32 miny, F32 maxx, F32 maxy, S32& x0, S32& y0, S32& x1, S32& y1) const
            {
                x0 = llmax(0, static_cast<S32>(std::floor((minx - mX0) / mTexel)));
                y0 = llmax(0, static_cast<S32>(std::floor((miny - mY0) / mTexel)));
                x1 = llmin(N - 1, static_cast<S32>(std::floor((maxx - mX0) / mTexel)));
                y1 = llmin(N - 1, static_cast<S32>(std::floor((maxy - mY0) / mTexel)));
                return x0 <= x1 && y0 <= y1;
            }
        };

        F32 cross2(const LLVector2& o, const LLVector2& a, const LLVector2& b)
        {
            return (a.mV[VX] - o.mV[VX]) * (b.mV[VY] - o.mV[VY]) - (a.mV[VY] - o.mV[VY]) * (b.mV[VX] - o.mV[VX]);
        }

        F32 segmentDistance(const LLVector2& p, const LLVector2& a, const LLVector2& b)
        {
            const LLVector2 ab = b - a;
            const F32 len2 = ab * ab;
            const F32 t = len2 > 0.f ? llclamp(((p - a) * ab) / len2, 0.f, 1.f) : 0.f;
            return (a + ab * t - p).length();
        }

        /// A 1 px anti-aliased line of a width in texels, composited over the image.
        void drawLine(Image& img, const Frame& f, const LLVector2& a, const LLVector2& b, F32 width_texels, const PaletteColour& c)
        {
            const F32 reach = (width_texels * 0.5f + 1.f) * f.mTexel;
            S32 x0, y0, x1, y1;
            if (!f.range(llmin(a.mV[VX], b.mV[VX]) - reach, llmin(a.mV[VY], b.mV[VY]) - reach, llmax(a.mV[VX], b.mV[VX]) + reach,
                         llmax(a.mV[VY], b.mV[VY]) + reach, x0, y0, x1, y1))
            {
                return;
            }
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    const F32 d = segmentDistance(f.texelCentre(x, y), a, b) / f.mTexel;
                    const F32 cover = llclamp(width_texels * 0.5f + 0.5f - d, 0.f, 1.f);
                    img.over(x, y, c.mR, c.mG, c.mB, c.mA * cover);
                }
            }
        }

        bool meetsCell(const Frame& f, const LLVector2& a, const LLVector2& b, F32 pad)
        {
            S32 x0, y0, x1, y1;
            return f.range(llmin(a.mV[VX], b.mV[VX]) - pad, llmin(a.mV[VY], b.mV[VY]) - pad, llmax(a.mV[VX], b.mV[VX]) + pad,
                           llmax(a.mV[VY], b.mV[VY]) + pad, x0, y0, x1, y1);
        }

        LLVector2 plan(const LLVector3& v)
        {
            return LLVector2(v.mV[VX], v.mV[VY]);
        }

        S64 floorDiv(S64 a, S64 b)
        {
            return a >= 0 ? a / b : -((-a + b - 1) / b);
        }

        /// A class mask's outline on the padded texel grid: partial texels, and inside texels (two subsamples or more)
        /// with a 4-neighbour outside, grown by one texel.
        void edgeBand(const std::vector<U8>& counts, std::vector<U8>& band)
        {
            std::vector<U8> edge(counts.size(), 0);
            auto inside = [&](S32 x, S32 y) { return x >= 0 && x < NP && y >= 0 && y < NP && counts[static_cast<size_t>(y) * NP + x] >= 2; };
            for (S32 y = 0; y < NP; ++y)
            {
                for (S32 x = 0; x < NP; ++x)
                {
                    const U8 c = counts[static_cast<size_t>(y) * NP + x];
                    bool e = c > 0 && c < 4;
                    if (!e && c >= 2)
                    {
                        e = !inside(x + 1, y) || !inside(x - 1, y) || !inside(x, y + 1) || !inside(x, y - 1);
                    }
                    edge[static_cast<size_t>(y) * NP + x] = e ? 1 : 0;
                }
            }
            band = edge;
            for (S32 y = 0; y < NP; ++y)
            {
                for (S32 x = 0; x < NP; ++x)
                {
                    if (edge[static_cast<size_t>(y) * NP + x])
                    {
                        continue;
                    }
                    const bool grown = (x + 1 < NP && edge[static_cast<size_t>(y) * NP + x + 1]) || (x > 0 && edge[static_cast<size_t>(y) * NP + x - 1])
                                    || (y + 1 < NP && edge[static_cast<size_t>(y + 1) * NP + x]) || (y > 0 && edge[static_cast<size_t>(y - 1) * NP + x]);
                    band[static_cast<size_t>(y) * NP + x] = grown ? 1 : 0;
                }
            }
        }

        /// The straight colour and alpha one subsample of a floor bakes to; only a graded (ramp) one is shaded by height.
        void floorColour(const ScenePoly& poly, F32 z, F32 base, bool graded, const RasterParams& params, F32 out[4])
        {
            PaletteColour c = poly.mArea == sknav::AREA_GROUND ? palette::MAT_TERRAIN : palette::MAT_OBJECT;
            F32 wash = palette::WASH_ALPHA;
            if (params.mHeatmap)
            {
                c = palette::HEAT_NEUTRAL;
            }
            else if (poly.mArea == sknav::AREA_SUBMERGED)
            {
                const F32 d = llclamp((poly.mWater - z) / palette::WATER_DEPTH_M, 0.f, 1.f);
                c.mR = lerp(palette::WATER_SHALLOW.mR, palette::WATER_DEEP.mR, d);
                c.mG = lerp(palette::WATER_SHALLOW.mG, palette::WATER_DEEP.mG, d);
                c.mB = lerp(palette::WATER_SHALLOW.mB, palette::WATER_DEEP.mB, d);
                wash = lerp(palette::WATER_WASH_SHALLOW, palette::WATER_WASH_DEEP, d);
            }
            F32 k = graded ? llclamp(1.f + palette::RAMP_GRADIENT_PER_M * (z - base), palette::RAMP_GRADIENT_MIN, palette::RAMP_GRADIENT_MAX) : 1.f;
            if (poly.mCoverage == static_cast<U8>(sknav::EnclosureBucket::INDOORS))
            {
                k *= palette::SHADE_INDOORS;
            }
            else if (poly.mCoverage == static_cast<U8>(sknav::EnclosureBucket::SHELTERED))
            {
                k *= palette::SHADE_SHELTERED;
            }
            // The wash over a black backing, brightened by the bake gain.
            const F32 a = wash + palette::BACKING_ALPHA * (1.f - wash);
            const F32 scale = palette::BAKE_GAIN * k * wash / a;
            out[0] = llmin(1.f, c.mR * scale);
            out[1] = llmin(1.f, c.mG * scale);
            out[2] = llmin(1.f, c.mB * scale);
            out[3] = a;
        }
    }

    F32 contourStep(F32 texel)
    {
        static const F32 NICE[] = { 0.5f, 1.f, 2.f, 5.f, 10.f, 20.f };
        const F32 want = 16.f * texel;
        for (F32 n : NICE)
        {
            if (n >= want - 1e-6f)
            {
                return llmax(CONTOUR_STEP_M, n);
            }
        }
        return NICE[5];
    }

    void rasterCell(const Scene& scene, S32 layer, const CellId& cell, const RasterParams& params, std::vector<U8>& out, RasterStats* stats)
    {
        out.assign(static_cast<size_t>(N) * N * 4, 0);
        if (stats)
        {
            *stats = RasterStats();
        }
        if (layer < 0 || layer >= static_cast<S32>(scene.mLayers.size()))
        {
            return;
        }
        const SceneLayer& L = scene.mLayers[layer];
        Frame f;
        f.mTexel = tierTexel(cell.mTier);
        const LLVector3d corner = cell.minCorner();
        f.mX0 = static_cast<F32>(corner.mdV[VX] - scene.mOrigin.mdV[VX]);
        f.mY0 = static_cast<F32>(corner.mdV[VY] - scene.mOrigin.mdV[VY]);
        const F32 size = N * f.mTexel;
        const F32 sub = f.mTexel / SUB;

        // 1-2. Fill, in painter order: ascending mean height, the larger area on a tie. Floors are found over the
        // padded cell, so the section's floor rules hold in the margin too.
        const F32 pad = PAD * f.mTexel;
        std::vector<U32> polys;
        for (U32 id : L.mPolys)
        {
            const ScenePoly& p = scene.mPolys[id];
            if (p.mMaxX >= f.mX0 - pad && p.mMinX <= f.mX0 + size + pad && p.mMaxY >= f.mY0 - pad && p.mMinY <= f.mY0 + size + pad)
            {
                polys.push_back(id);
            }
        }
        std::sort(polys.begin(), polys.end(), [&](U32 a, U32 b) {
            const ScenePoly& pa = scene.mPolys[a];
            const ScenePoly& pb = scene.mPolys[b];
            if (pa.mZMean != pb.mZMean)
            {
                return pa.mZMean < pb.mZMean;
            }
            if (pa.mPlanArea != pb.mPlanArea)
            {
                return pa.mPlanArea < pb.mPlanArea;
            }
            return a < b;
        });
        if (stats)
        {
            stats->mPolys = static_cast<S32>(polys.size());
        }
        // Subsample (i, j) of the padded grid has its centre PAD texels south-west of the cell's own.
        const S32 off = PAD * SUB;
        auto subCentre = [&](S32 i, S32 j) { return LLVector2(f.mX0 + (i - off + 0.5f) * sub, f.mY0 + (j - off + 0.5f) * sub); };
        std::vector<S32> owner(static_cast<size_t>(MP) * MP, -1);
        std::vector<F32> height(static_cast<size_t>(MP) * MP, 0.f);
        for (U32 id : polys)
        {
            const ScenePoly& poly = scene.mPolys[id];
            for (U32 t = 0; t < poly.mTriCount; ++t)
            {
                const LLVector3* tri = &scene.mTris[(poly.mFirstTri + t) * 3];
                const LLVector2 a = plan(tri[0]), b = plan(tri[1]), c = plan(tri[2]);
                const F32 area = cross2(a, b, c);
                if (std::fabs(area) < 1e-10f)
                {
                    continue;
                }
                const S32 sx0 = llmax(0, static_cast<S32>(std::floor((llmin(a.mV[VX], llmin(b.mV[VX], c.mV[VX])) - f.mX0) / sub)) + off);
                const S32 sy0 = llmax(0, static_cast<S32>(std::floor((llmin(a.mV[VY], llmin(b.mV[VY], c.mV[VY])) - f.mY0) / sub)) + off);
                const S32 sx1 = llmin(MP - 1, static_cast<S32>(std::floor((llmax(a.mV[VX], llmax(b.mV[VX], c.mV[VX])) - f.mX0) / sub)) + off);
                const S32 sy1 = llmin(MP - 1, static_cast<S32>(std::floor((llmax(a.mV[VY], llmax(b.mV[VY], c.mV[VY])) - f.mY0) / sub)) + off);
                for (S32 sy = sy0; sy <= sy1; ++sy)
                {
                    for (S32 sx = sx0; sx <= sx1; ++sx)
                    {
                        const LLVector2 p = subCentre(sx, sy);
                        const F32 wa = cross2(b, c, p) / area;
                        const F32 wb = cross2(c, a, p) / area;
                        const F32 wc = 1.f - wa - wb;
                        if (wa < 0.f || wb < 0.f || wc < 0.f)
                        {
                            continue;
                        }
                        const size_t i = static_cast<size_t>(sy) * MP + sx;
                        owner[i] = static_cast<S32>(id);
                        height[i] = wa * tri[0].mV[VZ] + wb * tri[1].mV[VZ] + wc * tri[2].mV[VZ];
                    }
                }
            }
        }
        Image img;
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                F32 sum[4] = { 0.f, 0.f, 0.f, 0.f };
                for (S32 k = 0; k < SUB * SUB; ++k)
                {
                    const size_t i = static_cast<size_t>(y * SUB + k / SUB + off) * MP + (x * SUB + k % SUB + off);
                    if (owner[i] < 0)
                    {
                        continue;
                    }
                    const ScenePoly& poly = scene.mPolys[owner[i]];
                    const bool graded = poly.mRun >= 0 && !scene.mRuns[poly.mRun].mStair;
                    F32 c[4];
                    floorColour(poly, height[i], L.mBaseZ, graded, params, c);
                    sum[0] += c[0] * c[3];
                    sum[1] += c[1] * c[3];
                    sum[2] += c[2] * c[3];
                    sum[3] += c[3];
                }
                F32* p = &img.mPx[(static_cast<size_t>(y) * N + x) * 4];
                for (S32 k = 0; k < 4; ++k)
                {
                    p[k] = sum[k] / (SUB * SUB);
                }
            }
        }

        // 3. Halos: light past a ledge, away from its floor.
        std::vector<F32> light(static_cast<size_t>(N) * N, 0.f);
        for (U32 m : L.mMarks)
        {
            const LineMark& mark = scene.mMarks[m];
            if (mark.mKind != MarkKind::LEDGE || mark.mFloorSide == 0)
            {
                continue;
            }
            const F32 w = llmin(palette::LEDGE_HALO_TEXELS * f.mTexel, palette::LEDGE_HALO_MAX_M);
            const F32 a0 = palette::LEDGE_HALO_ALPHA;
            S32 x0, y0, x1, y1;
            if (!f.range(llmin(mark.mA.mV[VX], mark.mB.mV[VX]) - w, llmin(mark.mA.mV[VY], mark.mB.mV[VY]) - w,
                         llmax(mark.mA.mV[VX], mark.mB.mV[VX]) + w, llmax(mark.mA.mV[VY], mark.mB.mV[VY]) + w, x0, y0, x1, y1))
            {
                continue;
            }
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    const LLVector2 p = f.texelCentre(x, y);
                    const F32 side = cross2(mark.mA, mark.mB, p) * mark.mFloorSide;
                    // A ledge's halo lies off the floor.
                    if (side >= 0.f)
                    {
                        continue;
                    }
                    const F32 d = segmentDistance(p, mark.mA, mark.mB);
                    if (d < w)
                    {
                        F32& h = light[static_cast<size_t>(y) * N + x];
                        h = llmax(h, a0 * (1.f - d / w));
                    }
                }
            }
        }
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                img.over(x, y, 1.f, 1.f, 1.f, light[static_cast<size_t>(y) * N + x]);
            }
        }

        // 4. Contours, at the levels the tier's step keeps.
        if (params.mContours)
        {
            const F32 step = contourStep(f.mTexel);
            for (U32 c : L.mContours)
            {
                const Contour& contour = scene.mContours[c];
                const F32 r = contour.mLevel * CONTOUR_STEP_M / step;
                if (std::fabs(r - std::floor(r + 0.5f)) > 1e-4f || !meetsCell(f, contour.mA, contour.mB, f.mTexel))
                {
                    continue;
                }
                drawLine(img, f, contour.mA, contour.mB, 1.f, palette::CONTOUR);
                if (stats)
                {
                    stats->mContourLevels.push_back(contour.mLevel);
                }
            }
        }

        // 5-6. Stair rungs, evenly spaced over the treads and thinned below 6 px apart; ramp chevrons.
        for (S32 r : L.mRuns)
        {
            const SceneRun& run = scene.mRuns[r];
            const LLVector2 ba = plan(run.mBottomA), bb = plan(run.mBottomB), ta = plan(run.mTopA), tb = plan(run.mTopB);
            if (run.mStair)
            {
                if (!params.mStairs || run.mTreads == 0)
                {
                    continue;
                }
                const S32 n = run.mTreads;
                const F32 spacing_px = (run.mRun / n) / (2.f * f.mTexel);
                S32 stride = 1;
                while (spacing_px * stride < palette::RUNG_MIN_PX && stride < n)
                {
                    stride *= 2;
                }
                for (S32 i = 0; i < n; i += stride)
                {
                    const F32 s = (i + 0.5f) / n;
                    const LLVector2 a = ba + (ta - ba) * s;
                    const LLVector2 b = bb + (tb - bb) * s;
                    if (!meetsCell(f, a, b, palette::RUNG_CASING_TEXELS * f.mTexel))
                    {
                        continue;
                    }
                    drawLine(img, f, a, b, palette::RUNG_CASING_TEXELS, palette::RUNG_CASING);
                    drawLine(img, f, a, b, palette::RUNG_CORE_TEXELS, palette::RUNG_CORE);
                    if (stats)
                    {
                        stats->mRungs++;
                    }
                }
                continue;
            }
            const LLVector2 bottom = (ba + bb) * 0.5f, top = (ta + tb) * 0.5f;
            LLVector2 up = top - bottom;
            const F32 length = up.normVec();
            if (length < 1e-3f)
            {
                continue;
            }
            const LLVector2 across(-up.mV[VY], up.mV[VX]);
            const F32 half = palette::CHEVRON_HALF_WIDTH * ((ta - tb).length() + (ba - bb).length()) * 0.5f;
            const F32 spacing = palette::CHEVRON_SPACING_TEXELS * f.mTexel;
            for (F32 t = spacing * 0.5f; t < length; t += spacing)
            {
                const LLVector2 apex = bottom + up * t;
                const LLVector2 back = apex - up * (half * 0.6f);
                const LLVector2 left = back + across * half, right = back - across * half;
                if (!meetsCell(f, left, right, half + f.mTexel) && !meetsCell(f, apex, apex, half + f.mTexel))
                {
                    continue;
                }
                drawLine(img, f, left, apex, 1.f, palette::CHEVRON);
                drawLine(img, f, apex, right, 1.f, palette::CHEVRON);
                if (stats)
                {
                    stats->mChevrons++;
                }
            }
        }

        // 7. The section: what the planes cut, as class masks over the padded cell, then fills and their outlines.
        if (stats)
        {
            stats->mSectionClass.assign(static_cast<size_t>(N) * N, 0);
        }
        if (params.mSection && !L.mPieces.empty())
        {
            const size_t msize = static_cast<size_t>(MP) * MP;
            std::vector<U8> full(msize, 0), low(msize, 0), prov(msize, 0), scratch;
            std::vector<U16> loops;
            std::vector<S8> winds;
            const F32 px0 = f.mX0 - pad, py0 = f.mY0 - pad, pspan = NP * f.mTexel;
            S32 pieces = 0;
            const SectionPiece* const layer_pieces = layerPieces(scene, L);
            for (size_t k = 0; k < L.mPieces.size(); ++k)
            {
                const SectionPiece& piece = layer_pieces[k];
                if (piece.mMax.mV[VX] < px0 || piece.mMin.mV[VX] > px0 + pspan || piece.mMax.mV[VY] < py0 || piece.mMin.mV[VY] > py0 + pspan)
                {
                    continue;
                }
                ++pieces;
                const S32 i0 = llclamp(static_cast<S32>(std::floor((piece.mMin.mV[VX] - px0) / sub)) - 1, 0, MP - 1);
                const S32 i1 = llclamp(static_cast<S32>(std::floor((piece.mMax.mV[VX] - px0) / sub)) + 1, 0, MP - 1);
                const S32 j0 = llclamp(static_cast<S32>(std::floor((piece.mMin.mV[VY] - py0) / sub)) - 1, 0, MP - 1);
                const S32 j1 = llclamp(static_cast<S32>(std::floor((piece.mMax.mV[VY] - py0) / sub)) + 1, 0, MP - 1);
                const S32 w = i1 - i0 + 1, h = j1 - j0 + 1;
                scratch.assign(static_cast<size_t>(w) * h, 0);
                const bool lines = (piece.mFlags & PIECE_LINES) != 0;
                if (lines)
                {
                    // Lines and demoted loops are stamped, never filled.
                    loops.clear();
                    winds.clear();
                    const size_t nlines = piece.mSlice->mLines.size();
                    for (U16 l : piece.mLines)
                    {
                        if (l >= nlines)
                        {
                            loops.push_back(static_cast<U16>(l - nlines));
                            winds.push_back(0);
                        }
                    }
                    fillCoverage(*piece.mSlice, loops, winds, true, piece.mShift, f.mX0 + (i0 - off) * sub, f.mY0 + (j0 - off) * sub, sub, w, h,
                                 scratch);
                }
                else
                {
                    fillCoverage(*piece.mSlice, piece.mLoops, piece.mWind, false, piece.mShift, f.mX0 + (i0 - off) * sub,
                                 f.mY0 + (j0 - off) * sub, sub, w, h, scratch);
                }
                const bool island = (piece.mFlags & PIECE_ISLAND) != 0;
                const bool blob = (piece.mFlags & PIECE_BLOB) != 0;
                const bool provisional = piece.mStyle == SectionStyle::PROVISIONAL;
                const F32 plane = static_cast<F32>(sectionPlaneZ(piece.mPlane) - scene.mOrigin.mdV[VZ]);
                const S32 low_level = piece.mPlane - SECTION_LOW_Q, high_level = piece.mPlane - SECTION_HIGH_Q;
                // An island is gated by the levels around it rather than its own cells.
                const bool island_low = (piece.mFlags & PIECE_ISLAND_LOW) != 0, island_high = (piece.mFlags & PIECE_ISLAND_HIGH) != 0;
                for (S32 j = 0; j < h; ++j)
                {
                    const S32 pj = j0 + j;
                    for (S32 i = 0; i < w; ++i)
                    {
                        if (!scratch[static_cast<size_t>(j) * w + i])
                        {
                            continue;
                        }
                        const S32 pi = i0 + i;
                        const size_t at = static_cast<size_t>(pj) * MP + pi;
                        // Floor rules: nothing over walkable floor, but an island on its own raised top.
                        if (owner[at] >= 0 && !(island && height[at] > plane - SECTION_HEADROOM_M))
                        {
                            continue;
                        }
                        bool low_role, high_role;
                        if (island)
                        {
                            low_role = island_low;
                            high_role = island_high;
                        }
                        else
                        {
                            const LLVector2 c = subCentre(pi, pj);
                            const S32 gx = static_cast<S32>(std::floor(scene.mOrigin.mdV[VX] + c.mV[VX]));
                            const S32 gy = static_cast<S32>(std::floor(scene.mOrigin.mdV[VY] + c.mV[VY]));
                            low_role = levelListed(L.mLevels, gx, gy, low_level);
                            high_role = levelListed(L.mLevels, gx, gy, high_level);
                        }
                        low_role = low_role && !blob;
                        if (provisional)
                        {
                            prov[at] |= (low_role || high_role) ? 1 : 0;
                            continue;
                        }
                        full[at] |= high_role ? 1 : 0;
                        low[at] |= low_role ? 1 : 0;
                    }
                }
            }

            // Per padded texel, how many of its four subsamples are FULL, HALF and provisional.
            const size_t tsize = static_cast<size_t>(NP) * NP;
            std::vector<U8> cf(tsize, 0), ch(tsize, 0), cp(tsize, 0), cany(tsize, 0);
            for (S32 y = 0; y < NP; ++y)
            {
                for (S32 x = 0; x < NP; ++x)
                {
                    U8 nf = 0, nh = 0, np = 0;
                    for (S32 k = 0; k < SUB * SUB; ++k)
                    {
                        const size_t i = static_cast<size_t>(y * SUB + k / SUB) * MP + (x * SUB + k % SUB);
                        nf += full[i];
                        nh += (low[i] && !full[i]) ? 1 : 0;
                        np += prov[i];
                    }
                    const size_t t = static_cast<size_t>(y) * NP + x;
                    cf[t] = nf;
                    ch[t] = nh;
                    cp[t] = np;
                    cany[t] = nf + nh;
                }
            }
            std::vector<U8> full_edge, any_edge, prov_edge;
            edgeBand(cf, full_edge);
            edgeBand(cany, any_edge);
            edgeBand(cp, prov_edge);
            const bool flat = cell.mTier >= palette::SECTION_FLAT_TIER;
            if (stats)
            {
                stats->mPieces = pieces;
            }
            for (S32 y = 0; y < N; ++y)
            {
                for (S32 x = 0; x < N; ++x)
                {
                    const size_t t = static_cast<size_t>(y + PAD) * NP + (x + PAD);
                    const S64 gx = static_cast<S64>(cell.mX) * N + x, gy = static_cast<S64>(cell.mY) * N + y;
                    U8 bits = 0;
                    if (cf[t])
                    {
                        const PaletteColour& c = palette::SECTION_FULL;
                        img.over(x, y, c.mR, c.mG, c.mB, palette::SECTION_FULL_ALPHA * cf[t] / 4.f);
                        bits |= SECTION_TEXEL_FULL;
                    }
                    if (ch[t])
                    {
                        const PaletteColour& c = palette::SECTION_HALF;
                        // The hatch runs "/", against the void hatch's "\", from global texels so it meets across cells.
                        const bool hatch = ((gx - gy) & (palette::SECTION_HATCH_PITCH - 1)) < palette::SECTION_HATCH_LINE;
                        const F32 a = flat ? palette::SECTION_HALF_FLAT_ALPHA
                                           : palette::SECTION_HALF_TINT_ALPHA + (hatch ? palette::SECTION_HALF_HATCH_ALPHA : 0.f);
                        img.over(x, y, c.mR, c.mG, c.mB, a * ch[t] / 4.f);
                        bits |= SECTION_TEXEL_HALF;
                    }
                    if (any_edge[t] && !full_edge[t] && ch[t] > 0)
                    {
                        const PaletteColour& c = palette::SECTION_HALF;
                        img.over(x, y, c.mR, c.mG, c.mB, palette::SECTION_HALF_EDGE_ALPHA);
                        bits |= SECTION_TEXEL_HALF_EDGE;
                    }
                    if (full_edge[t])
                    {
                        const PaletteColour& c = palette::SECTION_FULL_EDGE;
                        img.over(x, y, c.mR, c.mG, c.mB, palette::SECTION_FULL_EDGE_ALPHA);
                        bits |= SECTION_TEXEL_FULL_EDGE;
                    }
                    if (prov_edge[t] && (floorDiv(gx + gy, palette::SECTION_DASH) & 1))
                    {
                        const PaletteColour& c = palette::SECTION_PROVISIONAL;
                        img.over(x, y, c.mR, c.mG, c.mB, palette::SECTION_PROVISIONAL_ALPHA);
                        bits |= SECTION_TEXEL_PROVISIONAL_EDGE;
                    }
                    if (stats)
                    {
                        stats->mSectionClass[static_cast<size_t>(y) * N + x] = bits;
                        stats->mFullTexels += (bits & SECTION_TEXEL_FULL) ? 1 : 0;
                        stats->mHalfTexels += (bits & SECTION_TEXEL_HALF) ? 1 : 0;
                    }
                }
            }
        }

        // Straight alpha out.
        for (size_t i = 0; i < static_cast<size_t>(N) * N; ++i)
        {
            const F32* p = &img.mPx[i * 4];
            const F32 a = llclamp(p[3], 0.f, 1.f);
            U8* o = &out[i * 4];
            if (a <= 0.f)
            {
                continue;
            }
            for (S32 k = 0; k < 3; ++k)
            {
                o[k] = static_cast<U8>(llclamp(p[k] / a, 0.f, 1.f) * 255.f + 0.5f);
            }
            o[3] = static_cast<U8>(a * 255.f + 0.5f);
        }
        if (stats)
        {
            std::sort(stats->mContourLevels.begin(), stats->mContourLevels.end());
            stats->mContourLevels.erase(std::unique(stats->mContourLevels.begin(), stats->mContourLevels.end()), stats->mContourLevels.end());
        }
    }
}
