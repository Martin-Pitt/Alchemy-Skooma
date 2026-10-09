/**
 * @file sknavairdebug.cpp
 * @brief What the air overlay draws of a tile's air, as floors and edges in mesh-local metres.
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

#include "sknavairdebug.h"

#include "sknavairdistil.h"
#include "sknavtypes.h"

#include <cmath>
#include <cstdlib>

namespace sknav
{
    namespace
    {
        constexpr F32 FLOOR_LIFT_M = 0.03f;

        LLColor4 ramp(const LLColor4& a, const LLColor4& b, F32 t)
        {
            t = llclamp(t, 0.f, 1.f);
            return LLColor4(a.mV[0] + (b.mV[0] - a.mV[0]) * t, a.mV[1] + (b.mV[1] - a.mV[1]) * t, a.mV[2] + (b.mV[2] - a.mV[2]) * t, 1.f);
        }

        LLColor4 withAlpha(LLColor4 c, F32 a)
        {
            c.mV[VALPHA] = a;
            return c;
        }

        LLColor4 areaColour(U8 area)
        {
            static const LLColor4 AREAS[4] = { LLColor4(0.45f, 0.45f, 0.5f, 1.f), LLColor4(0.2f, 0.85f, 1.f, 1.f),
                                               LLColor4(0.7f, 0.45f, 1.f, 1.f), LLColor4(1.f, 0.95f, 0.3f, 1.f) };
            return AREAS[area & 3];
        }

        bool open(const AirGap& g) { return g.mCeiling == AIR_CEILING_OPEN || (g.mFlags & AirGap::OPEN); }

        /// Whether the cell holds a gap whose floor is g's within climb: no wall and no step between them.
        bool sameAirAt(const TileAir& air, S32 ix, S32 iy, const AirGap& g, S32 climb)
        {
            if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
            {
                return true;    // the neighbour tile's to say; no edge along every seam
            }
            const S32 leaf = findLeafByKey(air, ix, iy);
            if (leaf < 0)
            {
                return false;
            }
            AirColumn column;
            airColumn(air, leaf, column);
            for (U32 k = 0; k < column.mCount; ++k)
            {
                if (std::abs(static_cast<S32>(air.mGaps[column.mGap[k]].mFloor) - static_cast<S32>(g.mFloor)) <= climb)
                {
                    return true;
                }
            }
            return false;
        }
    }

    LLColor4 airLevelColour(S32 level)
    {
        static const LLColor4 LEVELS[AIR_MAX_LEVEL + 1] = { LLColor4(0.2f, 0.9f, 1.f, 1.f), LLColor4(0.3f, 1.f, 0.45f, 1.f),
                                                            LLColor4(1.f, 0.95f, 0.3f, 1.f), LLColor4(1.f, 0.6f, 0.15f, 1.f),
                                                            LLColor4(1.f, 0.3f, 0.85f, 1.f) };
        return LEVELS[llclamp(level, 0, AIR_MAX_LEVEL)];
    }

    LLColor4 airFlagsColour(U8 flags)
    {
        if (flags & AirGap::FLIGHT)
        {
            return LLColor4(1.f, 0.3f, 0.9f, 1.f);
        }
        if (flags & AirGap::AVATAR_FLOOR)
        {
            return LLColor4(0.3f, 1.f, 0.45f, 1.f);
        }
        if (flags & AirGap::TOP)
        {
            return LLColor4(1.f, 0.7f, 0.2f, 1.f);
        }
        return LLColor4(0.55f, 0.55f, 0.6f, 1.f);
    }

    LLColor4 airClearColour(U8 clear)
    {
        const F32 t = static_cast<F32>(llmin(clear, CLEAR_MAX)) / CLEAR_MAX;
        const LLColor4 wall(1.f, 0.15f, 0.1f, 1.f), mid(1.f, 0.9f, 0.2f, 1.f), clear_c(0.2f, 0.9f, 1.f, 1.f);
        return t < 0.5f ? ramp(wall, mid, t * 2.f) : ramp(mid, clear_c, t * 2.f - 1.f);
    }

    LLColor4 spaceColour(U32 id)
    {
        if (id == 0)
        {
            return LLColor4(0.3f, 0.3f, 0.32f, 1.f);
        }
        // The spaces overlay's pastel (buildSpacesDraw), so the two read alike.
        const U64 h = mix(SEED, id);
        return LLColor4(0.35f + 0.6f * ((h & 255) / 255.f), 0.35f + 0.6f * (((h >> 8) & 255) / 255.f),
                        0.35f + 0.6f * (((h >> 16) & 255) / 255.f), 1.f);
    }

    LLColor4 faceOffsetColour(U32 offset)
    {
        return ramp(LLColor4(1.f, 0.75f, 0.2f, 1.f), LLColor4(1.f, 0.2f, 0.9f, 1.f), static_cast<F32>(offset) / FACE_OFFSET_STEPS);
    }

    bool airFloorUnder(const TileAir& air, const LLVector3& at, F32& floor)
    {
        if (air.mCell <= 0.f)
        {
            return false;
        }
        const S32 ix = static_cast<S32>(std::floor((at.mV[VX] - air.mOrigin.mV[VX]) / air.mCell));
        const S32 iy = static_cast<S32>(std::floor((at.mV[VY] - air.mOrigin.mV[VY]) / air.mCell));
        if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
        {
            return false;
        }
        const S32 leaf = findLeafByKey(air, ix, iy);
        if (leaf < 0)
        {
            return false;
        }
        AirColumn column;
        airColumn(air, leaf, column);
        bool found = false;
        for (U32 k = 0; k < column.mCount; ++k)
        {
            const U32 i = column.mGap[k];
            const F32 z = air.mOrigin.mV[VZ] + air.mGaps[i].mFloor * air.mUnit;
            if (z <= at.mV[VZ] + air.mUnit && (!found || z > floor))
            {
                floor = z;
                found = true;
            }
        }
        return found;
    }

    bool gatherAirDebug(const TileAir& air, const AirDebugInput& in, AirDebugSink& sink, AirDebugStats* stats)
    {
        if (in.mMode == AirDebugMode::OFF || in.mMode == AirDebugMode::STAMP || air.mCell <= 0.f)
        {
            return true;
        }
        const F32 dim = in.mCoarse ? 0.55f : 1.f;
        const F32 cell = air.mCell;
        const LLVector3& o = air.mOrigin;
        const F32 reach2 = in.mReach * in.mReach;
        auto refused = [&]() {
            if (stats)
            {
                stats->mCut = true;
            }
            return false;
        };
        auto planOut = [&](F32 x, F32 y) {
            if (in.mReach <= 0.f)
            {
                return false;
            }
            const F32 dx = x - in.mCentre.mV[VX], dy = y - in.mCentre.mV[VY];
            return dx * dx + dy * dy > reach2;
        };
        auto floorZ = [&](U16 floor) { return o.mV[VZ] + floor * air.mUnit; };
        auto inSlice = [&](F32 z) { return z >= in.mSliceLo && z <= in.mSliceHi; };

        if (in.mMode == AirDebugMode::FACES)
        {
            for (const FaceOffset& f : air.mFaceOffsets)
            {
                const U32 column = f.key() / 4u;
                const S32 side = static_cast<S32>(f.key() % 4u);
                const S32 ix = static_cast<S32>(column % static_cast<U32>(air.mWidth));
                const S32 iy = static_cast<S32>(column / static_cast<U32>(air.mWidth));
                const F32 x0 = o.mV[VX] + ix * cell, y0 = o.mV[VY] + iy * cell;
                if (planOut(x0 + cell * 0.5f, y0 + cell * 0.5f))
                {
                    continue;
                }
                static const S32 STEP[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                const S32 nx = ix + STEP[side][0], ny = iy + STEP[side][1];
                const S32 leaf = (nx < 0 || ny < 0 || nx >= air.mWidth || ny >= air.mHeight) ? -1 : findLeafByKey(air, nx, ny);
                if (leaf < 0)
                {
                    continue;
                }
                const F32 off = cell * f.offset() / FACE_OFFSET_STEPS;
                // The boundary and the true face, each one cell edge long, along the side the face looks out of.
                LLVector3 b0, b1, t0, t1;
                switch (side)
                {
                    case TileAir::SIDE_NEG_X:
                        b0.set(x0, y0, 0.f), b1.set(x0, y0 + cell, 0.f), t0.set(x0 + off, y0, 0.f), t1.set(x0 + off, y0 + cell, 0.f);
                        break;
                    case TileAir::SIDE_POS_X:
                        b0.set(x0 + cell, y0, 0.f), b1.set(x0 + cell, y0 + cell, 0.f), t0.set(x0 + cell - off, y0, 0.f),
                            t1.set(x0 + cell - off, y0 + cell, 0.f);
                        break;
                    case TileAir::SIDE_NEG_Y:
                        b0.set(x0, y0, 0.f), b1.set(x0 + cell, y0, 0.f), t0.set(x0, y0 + off, 0.f), t1.set(x0 + cell, y0 + off, 0.f);
                        break;
                    default:
                        b0.set(x0, y0 + cell, 0.f), b1.set(x0 + cell, y0 + cell, 0.f), t0.set(x0, y0 + cell - off, 0.f),
                            t1.set(x0 + cell, y0 + cell - off, 0.f);
                        break;
                }
                AirColumn gaps;
                airColumn(air, leaf, gaps);
                for (U32 k = 0; k < gaps.mCount; ++k)
                {
                    const AirGap& g = air.mGaps[gaps.mGap[k]];
                    const F32 z = floorZ(g.mFloor);
                    if (!(g.mFlags & AirGap::AVATAR_FLOOR) || !inSlice(z))
                    {
                        continue;
                    }
                    const LLVector3 up(0.f, 0.f, z + 0.05f);
                    if (!sink.line(b0 + up, b1 + up, LLColor4(0.5f, 0.5f, 0.55f, 0.6f * dim))
                        || !sink.line(t0 + up, t1 + up, withAlpha(faceOffsetColour(f.offset()), dim)))
                    {
                        return refused();
                    }
                    if (stats)
                    {
                        ++stats->mFaces;
                    }
                }
            }
            return true;
        }

        for (const AirLeaf& l : air.mLeaves)
        {
            const S32 n = 1 << l.mLevel;
            const F32 side = n * cell;
            const F32 x0 = o.mV[VX] + l.mX * cell, y0 = o.mV[VY] + l.mY * cell;
            if (planOut(x0 + side * 0.5f, y0 + side * 0.5f))
            {
                continue;
            }
            for (U32 i = l.mFirstGap; i < l.mFirstGap + l.mGapCount; ++i)
            {
                const AirGap& g = air.mGaps[i];
                const F32 zf = floorZ(g.mFloor);
                if (!inSlice(zf))
                {
                    continue;
                }
                const F32 z = zf + FLOOR_LIFT_M;
                const LLVector3 c[4] = { LLVector3(x0, y0, z), LLVector3(x0 + side, y0, z), LLVector3(x0 + side, y0 + side, z),
                                         LLVector3(x0, y0 + side, z) };
                auto outline = [&](const LLVector3* p, const LLColor4& colour) {
                    for (S32 k = 0; k < 4; ++k)
                    {
                        if (!sink.line(p[k], p[(k + 1) % 4], colour))
                        {
                            return false;
                        }
                    }
                    return true;
                };
                bool ok = true;
                switch (in.mMode)
                {
                    case AirDebugMode::LEAVES:
                    {
                        const LLColor4 colour = airLevelColour(l.mLevel);
                        ok = sink.quad(c[0], c[1], c[2], c[3], withAlpha(colour, 0.12f * dim)) && outline(c, withAlpha(colour, 0.9f * dim));
                        break;
                    }
                    case AirDebugMode::SPACE:
                    {
                        const U32 id = in.mGapSpace ? in.mGapSpace[i] : 0;
                        ok = sink.quad(c[0], c[1], c[2], c[3], withAlpha(spaceColour(id), (id ? 0.55f : 0.25f) * dim));
                        break;
                    }
                    case AirDebugMode::CLEARANCE:
                        ok = sink.quad(c[0], c[1], c[2], c[3], withAlpha(airClearColour(g.mClear), 0.55f * dim));
                        break;
                    case AirDebugMode::GAPS:
                    {
                        // Floors and edges only: a fill by kind, the floor outline by area class, the
                        // ceiling's outline, and verticals only at corners where a neighbour's air differs.
                        ok = sink.quad(c[0], c[1], c[2], c[3], withAlpha(airFlagsColour(g.mFlags), 0.35f * dim))
                             && outline(c, withAlpha(areaColour(g.area()), 0.85f * dim));
                        const bool is_open = open(g);
                        const F32 top = is_open ? zf + in.mOpenTick : floorZ(g.mCeiling) - FLOOR_LIFT_M;
                        const LLColor4 wall = withAlpha(LLColor4(0.85f, 0.85f, 0.9f, 1.f), 0.7f * dim);
                        if (ok && !is_open)
                        {
                            const LLVector3 lift(0.f, 0.f, top - z);
                            const LLVector3 ceil[4] = { c[0] + lift, c[1] + lift, c[2] + lift, c[3] + lift };
                            ok = outline(ceil, withAlpha(LLColor4(0.6f, 0.75f, 1.f, 1.f), 0.6f * dim));
                        }
                        else if (ok)
                        {
                            const LLVector3 mid(x0 + side * 0.5f, y0 + side * 0.5f, z);
                            const LLColor4 tick = (g.mFlags & AirGap::OWN_SKY) ? LLColor4(0.3f, 0.75f, 1.f, 1.f) : LLColor4(0.5f, 0.6f, 0.75f, 1.f);
                            ok = sink.line(mid, LLVector3(mid.mV[VX], mid.mV[VY], zf + in.mOpenTick), withAlpha(tick, 0.9f * dim));
                        }
                        // Each corner looks at the two cells beside it outside the leaf and the one across it; a
                        // corner partway along a straight wall or step draws nothing, so only its ends and turns stand.
                        const S32 X = l.mX, Y = l.mY;
                        const S32 probe[4][6] = { { X - 1, Y, X, Y - 1, X - 1, Y - 1 },
                                                  { X + n, Y, X + n - 1, Y - 1, X + n, Y - 1 },
                                                  { X + n, Y + n - 1, X + n - 1, Y + n, X + n, Y + n },
                                                  { X - 1, Y + n - 1, X, Y + n, X - 1, Y + n } };
                        for (S32 k = 0; ok && k < 4; ++k)
                        {
                            const bool a_diff = !sameAirAt(air, probe[k][0], probe[k][1], g, in.mClimb);
                            const bool b_diff = !sameAirAt(air, probe[k][2], probe[k][3], g, in.mClimb);
                            const bool d_diff = !sameAirAt(air, probe[k][4], probe[k][5], g, in.mClimb);
                            const bool straight = d_diff && a_diff != b_diff;
                            if ((!a_diff && !b_diff && !d_diff) || straight)
                            {
                                continue;
                            }
                            ok = sink.line(c[k], LLVector3(c[k].mV[VX], c[k].mV[VY], top), wall);
                        }
                        break;
                    }
                    default:
                        break;
                }
                if (!ok)
                {
                    return refused();
                }
                if (stats)
                {
                    ++stats->mGaps;
                }
            }
        }
        return true;
    }
}
