/**
 * @file skmapribbons.cpp
 * @brief Mini-map line marks expanded into screen-width triangle ribbons for one zoom bucket.
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

#include "skmapribbons.h"

#include "skmappalette.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace skmap
{
    namespace
    {
        LLColor4U colour4u(const PaletteColour& c)
        {
            auto u = [](F32 v) { return static_cast<U8>(llclamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            return LLColor4U(u(c.mR), u(c.mG), u(c.mB), u(c.mA));
        }

        bool lineKind(MarkKind kind)
        {
            return kind == MarkKind::WALL || kind == MarkKind::LEDGE || kind == MarkKind::OVERHANG || kind == MarkKind::DROP_EDGE;
        }

        bool coverToggle(MarkKind kind)
        {
            return lineKind(kind) || kind == MarkKind::DROP_POINT;
        }

        bool doorToggle(MarkKind kind)
        {
            return kind == MarkKind::DOOR_JAMB || kind == MarkKind::DOOR_LEAF || kind == MarkKind::DOOR_SHUT || kind == MarkKind::DOOR_THRESHOLD
                || kind == MarkKind::DOOR_SWING || kind == MarkKind::LADDER || kind == MarkKind::ELEVATOR;
        }

        /// Anchors in metres, offsets from them in screen pixels at mMpp.
        struct Writer
        {
            Ribbons& mOut;
            F32 mMpp;

            void vert(const LLVector2& anchor, const LLVector2& px, const LLColor4U& colour)
            {
                mOut.mVerts.emplace_back(anchor.mV[VX] + px.mV[VX] * mMpp, anchor.mV[VY] + px.mV[VY] * mMpp, 0.f);
                mOut.mOffsets.push_back(px);
                mOut.mColours.push_back(colour);
            }

            void tri(const LLVector2& a, const LLVector2& pa, const LLVector2& b, const LLVector2& pb, const LLVector2& c,
                     const LLVector2& pc, const LLColor4U& colour)
            {
                vert(a, pa, colour);
                vert(b, pb, colour);
                vert(c, pc, colour);
            }

            /// From a to b in metres, width in pixels.
            void line(const LLVector2& a, const LLVector2& b, F32 width_px, const LLColor4U& colour)
            {
                LLVector2 d = b - a;
                if (d.normVec() < 1e-6f)
                {
                    return;
                }
                const LLVector2 n(-d.mV[VY] * width_px * 0.5f, d.mV[VX] * width_px * 0.5f);
                tri(a, -n, b, -n, b, n, colour);
                tri(a, -n, b, n, a, n, colour);
            }

            /// A glyph stroke: one anchor in metres, its ends and width in pixels.
            void stroke(const LLVector2& anchor, const LLVector2& pa, const LLVector2& pb, F32 width_px, const LLColor4U& colour)
            {
                LLVector2 d = pb - pa;
                if (d.normVec() < 1e-6f)
                {
                    return;
                }
                const LLVector2 n(-d.mV[VY] * width_px * 0.5f, d.mV[VX] * width_px * 0.5f);
                tri(anchor, pa - n, anchor, pb - n, anchor, pb + n, colour);
                tri(anchor, pa - n, anchor, pb + n, anchor, pa + n, colour);
            }

            /// On and off along a to b, in metres, starting on; width in pixels.
            void dashed(const LLVector2& a, const LLVector2& b, F32 width, F32 on, F32 off, const LLColor4U& colour)
            {
                LLVector2 d = b - a;
                const F32 length = d.normVec();
                if (length < 1e-6f || on <= 0.f)
                {
                    return;
                }
                for (F32 t = 0.f; t < length; t += on + off)
                {
                    line(a + d * t, a + d * llmin(t + on, length), width, colour);
                }
            }

            /// An arc of sweep radians about a centre from a start point, in metres, as segments of at most 10 degrees.
            void arc(const LLVector2& centre, const LLVector2& start, F32 sweep, F32 width_px, const LLColor4U& colour)
            {
                const S32 n = llmax(2, static_cast<S32>(std::ceil(std::fabs(sweep) / (10.f * DEG_TO_RAD))));
                const LLVector2 r = start - centre;
                LLVector2 prev = start;
                for (S32 k = 1; k <= n; ++k)
                {
                    const F32 a = sweep * k / n;
                    const F32 c = std::cos(a), s = std::sin(a);
                    const LLVector2 next = centre + LLVector2(r.mV[VX] * c - r.mV[VY] * s, r.mV[VX] * s + r.mV[VY] * c);
                    line(prev, next, width_px, colour);
                    prev = next;
                }
            }
        };

        /// Whether a mark continues a run: same kind, style and floor side, parallel within 5
        /// degrees, on the run's line, and starting within 0.2 m of one of its ends.
        bool extend(MarkRun& run, MarkRun mark)
        {
            if (run.mKind != mark.mKind || run.mCover != mark.mCover)
            {
                return false;
            }
            LLVector2 dr = run.mB - run.mA, dm = mark.mB - mark.mA;
            if (dr.normVec() < 1e-6f || dm.normVec() < 1e-6f)
            {
                return false;
            }
            if (dr * dm < 0.f)
            {
                std::swap(mark.mA, mark.mB);
                mark.mFloorSide = static_cast<S8>(-mark.mFloorSide);
                dm = -dm;
            }
            if (run.mFloorSide != mark.mFloorSide || dr * dm < std::cos(MERGE_ANGLE_DEG * DEG_TO_RAD))
            {
                return false;
            }
            const LLVector2 normal(-dr.mV[VY], dr.mV[VX]);
            if (std::fabs((mark.mA - run.mA) * normal) > MERGE_GAP_M || std::fabs((mark.mB - run.mA) * normal) > MERGE_GAP_M)
            {
                return false;
            }
            if ((mark.mA - run.mB).length() < MERGE_GAP_M)
            {
                run.mB = mark.mB;
            }
            else if ((mark.mB - run.mA).length() < MERGE_GAP_M)
            {
                run.mA = mark.mA;
            }
            else
            {
                return false;
            }
            run.mHeight = llmax(run.mHeight, mark.mHeight);
            return true;
        }

        typedef std::pair<S32, S32> EndCell;

        EndCell endCell(const LLVector2& p)
        {
            return { static_cast<S32>(std::floor(p.mV[VX] / 0.5f)), static_cast<S32>(std::floor(p.mV[VY] / 0.5f)) };
        }

        void mergeAll(std::vector<MarkRun>& runs)
        {
            // Greedy and order-preserving; repeated until nothing joins, so a mark that
            // bridges two runs still ends as one. Run ends are hashed on a 0.5 m grid.
            bool changed = true;
            while (changed)
            {
                changed = false;
                std::vector<MarkRun> merged;
                std::map<EndCell, std::vector<size_t>> ends;
                auto note = [&](size_t i) {
                    ends[endCell(merged[i].mA)].push_back(i);
                    ends[endCell(merged[i].mB)].push_back(i);
                };
                for (const MarkRun& r : runs)
                {
                    bool joined = false;
                    if (lineKind(r.mKind))
                    {
                        std::vector<size_t> candidates;
                        for (const LLVector2& p : { r.mA, r.mB })
                        {
                            const EndCell c = endCell(p);
                            for (S32 dy = -1; dy <= 1; ++dy)
                            {
                                for (S32 dx = -1; dx <= 1; ++dx)
                                {
                                    const auto it = ends.find({ c.first + dx, c.second + dy });
                                    if (it != ends.end())
                                    {
                                        candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                                    }
                                }
                            }
                        }
                        std::sort(candidates.begin(), candidates.end());
                        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
                        for (size_t i : candidates)
                        {
                            if (extend(merged[i], r))
                            {
                                note(i);
                                joined = true;
                                changed = true;
                                break;
                            }
                        }
                    }
                    if (!joined)
                    {
                        merged.push_back(r);
                        note(merged.size() - 1);
                    }
                }
                runs.swap(merged);
            }
        }

        void glyphs(Writer& w, const MarkRun& run)
        {
            switch (run.mKind)
            {
                case MarkKind::DROP_POINT:
                {
                    LLVector2 dir = run.mB - run.mA;
                    if (dir.normVec() < 1e-6f)
                    {
                        dir.set(0.f, -1.f);
                    }
                    const F32 half = palette::DROP_POINT_PX * 0.5f;
                    const LLVector2 across(-dir.mV[VY], dir.mV[VX]);
                    w.tri(run.mA, dir * half, run.mA, -dir * half + across * half, run.mA, -dir * half - across * half,
                          colour4u(palette::DROP_EDGE));
                    break;
                }
                case MarkKind::LADDER:
                {
                    const F32 h = palette::FEATURE_GLYPH_PX * 0.5f;
                    const F32 width = 1.5f;
                    const LLColor4U c = colour4u(palette::FEATURE_GLYPH);
                    w.stroke(run.mA, LLVector2(-h, -h), LLVector2(-h, h), width, c);
                    w.stroke(run.mA, LLVector2(h, -h), LLVector2(h, h), width, c);
                    w.stroke(run.mA, LLVector2(-h, 0.f), LLVector2(h, 0.f), width, c);
                    break;
                }
                case MarkKind::ELEVATOR:
                {
                    const F32 h = palette::FEATURE_GLYPH_PX * 0.5f;
                    const F32 width = 1.5f;
                    const LLColor4U c = colour4u(palette::FEATURE_GLYPH);
                    const LLVector2 corners[4] = { LLVector2(-h, -h), LLVector2(h, -h), LLVector2(h, h), LLVector2(-h, h) };
                    for (S32 k = 0; k < 4; ++k)
                    {
                        w.stroke(run.mA, corners[k], corners[(k + 1) % 4], width, c);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void mergeMarks(const Scene& scene, S32 layer, const RibbonParams& params, std::vector<MarkRun>& out, const CutCells* cut)
    {
        out.clear();
        if (layer < 0 || layer >= static_cast<S32>(scene.mLayers.size()))
        {
            return;
        }
        for (U32 m : scene.mLayers[layer].mMarks)
        {
            const LineMark& mark = scene.mMarks[m];
            if ((coverToggle(mark.mKind) && !params.mCover) || (doorToggle(mark.mKind) && !params.mDoors))
            {
                continue;
            }
            MarkRun run;
            run.mKind = mark.mKind;
            run.mA = mark.mA;
            run.mB = mark.mB;
            run.mFloorSide = mark.mFloorSide;
            run.mHeight = mark.mHeight;
            run.mSweep = mark.mSweep;
            if (mark.mKind == MarkKind::WALL)
            {
                run.mCover = sknav::coverClass(mark.mHeight, params.mEye, params.mClimb, params.mPeek);
            }
            if (cut && !cut->empty())
            {
                auto in_cut = [&](const LLVector2& p) {
                    return cut->count(indexCell(scene.mOrigin.mdV[VX] + p.mV[VX], scene.mOrigin.mdV[VY] + p.mV[VY])) > 0;
                };
                if (!lineKind(mark.mKind))
                {
                    if (!in_cut(mark.mA))
                    {
                        out.push_back(run);
                    }
                    continue;
                }
                // A line is split at the global 4 m cell lines, so the cut drops exactly the cells it covers.
                const LLVector2 d = mark.mB - mark.mA;
                std::vector<F32> ts = { 0.f, 1.f };
                for (S32 axis : { VX, VY })
                {
                    if (std::fabs(d.mV[axis]) < 1e-6f)
                    {
                        continue;
                    }
                    const F64 g0 = scene.mOrigin.mdV[axis] + mark.mA.mV[axis];
                    const F64 g1 = scene.mOrigin.mdV[axis] + mark.mB.mV[axis];
                    const F64 hi = llmax(g0, g1);
                    for (F64 g = (std::floor(llmin(g0, g1) / INDEX_CELL_M) + 1.0) * INDEX_CELL_M; g < hi; g += INDEX_CELL_M)
                    {
                        ts.push_back(static_cast<F32>((g - g0) / (g1 - g0)));
                    }
                }
                std::sort(ts.begin(), ts.end());
                std::vector<MarkRun> pieces;
                bool dropped = false;
                for (size_t i = 0; i + 1 < ts.size(); ++i)
                {
                    if (ts[i + 1] - ts[i] < 1e-6f)
                    {
                        continue;
                    }
                    if (in_cut(mark.mA + d * ((ts[i] + ts[i + 1]) * 0.5f)))
                    {
                        dropped = true;
                        continue;
                    }
                    MarkRun piece = run;
                    piece.mA = ts[i] > 0.f ? mark.mA + d * ts[i] : mark.mA;
                    piece.mB = ts[i + 1] < 1.f ? mark.mA + d * ts[i + 1] : mark.mB;
                    pieces.push_back(piece);
                }
                if (dropped)
                {
                    out.insert(out.end(), pieces.begin(), pieces.end());
                    continue;
                }
            }
            out.push_back(run);
        }
        mergeAll(out);
        const F32 floor = llmax(palette::RIBBON_MIN_RUN_M, palette::RIBBON_MIN_RUN_PX * params.mMpp);
        out.erase(std::remove_if(out.begin(), out.end(), [&](const MarkRun& r) { return lineKind(r.mKind) && r.length() < floor; }),
                  out.end());
    }

    void buildLayerRibbons(const Scene& scene, S32 layer, const RibbonParams& params, Ribbons& out, const CutCells* cut)
    {
        out.clear();
        std::vector<MarkRun> runs;
        mergeMarks(scene, layer, params, runs, cut);
        out.mMpp = params.mMpp;
        Writer w { out, params.mMpp };
        const F32 mpp = params.mMpp;
        for (const MarkRun& run : runs)
        {
            LLVector2 dir = run.mB - run.mA;
            const F32 length = dir.normVec();
            // Left of A to B; the floor lies there when mFloorSide is +1.
            const LLVector2 left(-dir.mV[VY], dir.mV[VX]);
            const LLVector2 floor_side = left * static_cast<F32>(run.mFloorSide);
            switch (run.mKind)
            {
                case MarkKind::WALL:
                    if (run.mCover == sknav::Cover::FULL)
                    {
                        w.line(run.mA, run.mB, palette::COVER_FULL_PX, colour4u(palette::COVER_FULL));
                    }
                    else if (run.mCover == sknav::Cover::HALF)
                    {
                        const LLColor4U c = colour4u(palette::COVER_HALF);
                        w.line(run.mA, run.mB, palette::COVER_HALF_PX, c);
                        if (mpp < palette::HALF_GLYPH_MAX_MPP && run.mFloorSide != 0)
                        {
                            // A half shield on the floor side: a half disc.
                            const F32 spacing = llmax(2.f, palette::HALF_GLYPH_PX * mpp);
                            const F32 r = palette::HALF_GLYPH_SIZE_PX * 0.5f;
                            for (F32 t = spacing * 0.5f; t < length; t += spacing)
                            {
                                const LLVector2 centre = run.mA + dir * t;
                                for (S32 k = 0; k < 6; ++k)
                                {
                                    const F32 a0 = F_PI * k / 6.f, a1 = F_PI * (k + 1) / 6.f;
                                    w.tri(centre, LLVector2::zero, centre, dir * (r * std::cos(a0)) + floor_side * (r * std::sin(a0)), centre,
                                          dir * (r * std::cos(a1)) + floor_side * (r * std::sin(a1)), c);
                                }
                            }
                        }
                    }
                    else
                    {
                        w.line(run.mA, run.mB, palette::COVER_LOW_PX, colour4u(palette::COVER_LOW));
                    }
                    break;
                case MarkKind::LEDGE:
                {
                    const LLColor4U c = colour4u(palette::LEDGE);
                    w.line(run.mA, run.mB, palette::LEDGE_PX, c);
                    if (mpp < palette::LEDGE_TICK_MAX_MPP && run.mFloorSide != 0)
                    {
                        const F32 spacing = palette::LEDGE_TICK_SPACING_PX * mpp;
                        for (F32 t = spacing * 0.5f; t < length; t += spacing)
                        {
                            const LLVector2 at = run.mA + dir * t;
                            w.stroke(at, LLVector2::zero, -floor_side * palette::LEDGE_TICK_PX, palette::LEDGE_PX, c);
                        }
                    }
                    break;
                }
                case MarkKind::DROP_EDGE:
                    w.line(run.mA, run.mB, palette::DROP_EDGE_PX, colour4u(palette::DROP_EDGE));
                    break;
                case MarkKind::OVERHANG:
                    w.dashed(run.mA, run.mB, palette::OVERHANG_PX, palette::OVERHANG_ON_PX * mpp, palette::OVERHANG_OFF_PX * mpp,
                             colour4u(palette::OVERHANG));
                    break;
                case MarkKind::DOOR_JAMB:
                    w.line(run.mA, run.mB, palette::DOOR_JAMB_PX, colour4u(palette::DOOR));
                    break;
                case MarkKind::DOOR_LEAF:
                    w.line(run.mA, run.mB, palette::DOOR_LEAF_PX, colour4u(palette::DOOR));
                    break;
                case MarkKind::DOOR_SHUT:
                    w.line(run.mA, run.mB, palette::DOOR_SHUT_PX, colour4u(palette::DOOR));
                    break;
                case MarkKind::DOOR_THRESHOLD:
                    w.dashed(run.mA, run.mB, palette::DOOR_THRESHOLD_PX, palette::DOOR_DASH_PX * mpp, palette::DOOR_DASH_PX * mpp,
                             colour4u(palette::DOOR));
                    break;
                case MarkKind::DOOR_SWING:
                    w.arc(run.mA, run.mB, run.mSweep, palette::DOOR_SWING_PX, colour4u(withAlpha(palette::DOOR, palette::DOOR_SWING_ALPHA)));
                    break;
                default:
                    glyphs(w, run);
                    break;
            }
        }
    }

    void buildBoundaryRibbons(const Scene& scene, const RibbonParams& params, Ribbons& out)
    {
        out.clear();
        out.mMpp = params.mMpp;
        Writer w { out, params.mMpp };
        const F32 mpp = params.mMpp;
        for (const LineMark& mark : scene.mBoundaries)
        {
            if (mark.mKind == MarkKind::VOID_BOUNDARY)
            {
                w.line(mark.mA, mark.mB, palette::VOID_BOUNDARY_PX, colour4u(palette::VOID_BOUNDARY));
            }
            else
            {
                w.dashed(mark.mA, mark.mB, palette::REGION_BOUNDARY_PX, palette::REGION_DASH_ON_PX * mpp,
                         palette::REGION_DASH_OFF_PX * mpp, colour4u(palette::REGION_BOUNDARY));
            }
        }
    }
}
