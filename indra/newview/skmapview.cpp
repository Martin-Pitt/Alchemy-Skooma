/**
 * @file skmapview.cpp
 * @brief The mini-map's view transform, bake tiers, cells and analysis box: pure maths, no viewer state.
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

#include "skmapview.h"

#include <cmath>

namespace skmap
{
    namespace
    {
        S32 floorDiv(S32 a, S32 b)
        {
            S32 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0)))
            {
                --q;
            }
            return q;
        }

        F64 snapDown(F64 v, F64 step)
        {
            return std::floor(v / step) * step;
        }

        F64 snapUp(F64 v, F64 step)
        {
            return std::ceil(v / step) * step;
        }
    }

    F32 tierTexel(S32 tier)
    {
        return std::ldexp(1.f, tier - 5);
    }

    F64 tierCellSize(S32 tier)
    {
        return static_cast<F64>(CELL_TEXELS) * static_cast<F64>(tierTexel(tier));
    }

    S32 chooseTier(F32 mpp, S32 current)
    {
        const F32 target = llmax(mpp, 1e-6f) * 0.5f;
        if (current >= TIER_MIN && current <= TIER_MAX)
        {
            const F32 texel = tierTexel(current);
            if (target / (2.f * TIER_HYSTERESIS) < texel && texel <= TIER_HYSTERESIS * target)
            {
                return current;
            }
        }
        // The coarsest tier whose texel is no larger than the target.
        const S32 tier = static_cast<S32>(std::floor(5.f + std::log2(target) + 1e-6f));
        return llclamp(tier, TIER_MIN, TIER_MAX);
    }

    LLVector3d CellId::minCorner() const
    {
        const F64 size = tierCellSize(mTier);
        return LLVector3d(mX * size, mY * size, 0.0);
    }

    LLVector3d CellId::maxCorner() const
    {
        const F64 size = tierCellSize(mTier);
        return LLVector3d((mX + 1) * size, (mY + 1) * size, 0.0);
    }

    CellId CellId::ancestor(S32 tier) const
    {
        if (tier <= mTier)
        {
            return *this;
        }
        const S32 scale = 1 << (tier - mTier);
        return { tier, floorDiv(mX, scale), floorDiv(mY, scale) };
    }

    CellId cellAt(S32 tier, F64 x, F64 y)
    {
        const F64 size = tierCellSize(tier);
        return { tier, static_cast<S32>(std::floor(x / size)), static_cast<S32>(std::floor(y / size)) };
    }

    void cellsInBox(S32 tier, const LLVector3d& min, const LLVector3d& max, std::vector<CellId>& out)
    {
        out.clear();
        const CellId lo = cellAt(tier, min.mdV[VX], min.mdV[VY]);
        const CellId hi = cellAt(tier, max.mdV[VX], max.mdV[VY]);
        for (S32 y = lo.mY; y <= hi.mY; ++y)
        {
            for (S32 x = lo.mX; x <= hi.mX; ++x)
            {
                out.push_back({ tier, x, y });
            }
        }
    }

    void analysisBox(const LLVector3d& agent, const LLVector3d& centre, F32 long_side_px, F32 short_side_px, LLVector3d& min,
                     LLVector3d& max)
    {
        const F64 aspect = short_side_px > 0.f ? llclamp(static_cast<F64>(long_side_px) / short_side_px, 1.0, 2.0) : 1.0;
        const F64 reach = ANALYSIS_REACH_M * aspect + ANALYSIS_MARGIN_M;
        // A camera far from the agent (cammed away, or mid-teleport across the grid) gets a box of its own:
        // spanning both would grow without bound.
        const F64 dx = agent.mdV[VX] - centre.mdV[VX], dy = agent.mdV[VY] - centre.mdV[VY];
        const LLVector3d& near_agent = dx * dx + dy * dy <= ANALYSIS_JOIN_M * ANALYSIS_JOIN_M ? agent : centre;
        const F64 x0 = llmin(near_agent.mdV[VX], centre.mdV[VX]) - reach;
        const F64 y0 = llmin(near_agent.mdV[VY], centre.mdV[VY]) - reach;
        const F64 x1 = llmax(near_agent.mdV[VX], centre.mdV[VX]) + reach;
        const F64 y1 = llmax(near_agent.mdV[VY], centre.mdV[VY]) + reach;
        // Heights are open: the box is a plan footprint.
        min.set(snapDown(x0, ANALYSIS_SNAP_M), snapDown(y0, ANALYSIS_SNAP_M), -1.0e4);
        max.set(snapUp(x1, ANALYSIS_SNAP_M), snapUp(y1, ANALYSIS_SNAP_M), 1.0e5);
    }

    LLVector3d analysisOrigin(const LLVector3d& min, const LLVector3d* last)
    {
        if (last && std::fabs(min.mdV[VX] - last->mdV[VX]) <= ANALYSIS_ORIGIN_DRIFT_M
            && std::fabs(min.mdV[VY] - last->mdV[VY]) <= ANALYSIS_ORIGIN_DRIFT_M)
        {
            return LLVector3d(last->mdV[VX], last->mdV[VY], 0.0);
        }
        return LLVector3d(min.mdV[VX], min.mdV[VY], 0.0);
    }

    F32 zoomByNotches(F32 span, F32 notches)
    {
        return llclamp(span * std::pow(ZOOM_STEP, notches), ZOOM_MIN_M, ZOOM_MAX_M);
    }

    S32 zoomBucket(F32 mpp)
    {
        return static_cast<S32>(std::floor(std::log(llmax(mpp, 1e-6f)) / std::log(ZOOM_STEP) + 0.5f));
    }

    F32 bucketMpp(S32 bucket)
    {
        return std::pow(ZOOM_STEP, static_cast<F32>(bucket));
    }

    void MapView::set(const LLVector3d& centre, F32 span, F32 rotation, F32 width, F32 height)
    {
        mCentre = centre;
        mSpan = llclamp(span, ZOOM_MIN_M, ZOOM_MAX_M);
        mRotation = rotation;
        mWidth = llmax(width, 1.f);
        mHeight = llmax(height, 1.f);
        mMpp = mSpan / shortSide();
        mCos = std::cos(rotation);
        mSin = std::sin(rotation);
    }

    LLVector2 MapView::toWidgetOffset(F32 dx, F32 dy) const
    {
        const F32 ppm = pixelsPerMetre();
        return LLVector2((dx * mCos - dy * mSin) * ppm, (dx * mSin + dy * mCos) * ppm);
    }

    LLVector2 MapView::toWidget(const LLVector3d& global) const
    {
        const LLVector2 offset = toWidgetOffset(static_cast<F32>(global.mdV[VX] - mCentre.mdV[VX]),
                                                static_cast<F32>(global.mdV[VY] - mCentre.mdV[VY]));
        return LLVector2(mWidth * 0.5f + offset.mV[VX], mHeight * 0.5f + offset.mV[VY]);
    }

    LLVector3d MapView::toGlobal(F32 x, F32 y) const
    {
        const F64 px = (x - mWidth * 0.5f) * mMpp;
        const F64 py = (y - mHeight * 0.5f) * mMpp;
        // The inverse rotation.
        const F64 dx = px * mCos + py * mSin;
        const F64 dy = -px * mSin + py * mCos;
        return LLVector3d(mCentre.mdV[VX] + dx, mCentre.mdV[VY] + dy, mCentre.mdV[VZ]);
    }

    void MapView::visibleBox(LLVector3d& min, LLVector3d& max) const
    {
        const LLVector3d corners[4] = { toGlobal(0.f, 0.f), toGlobal(mWidth, 0.f), toGlobal(0.f, mHeight), toGlobal(mWidth, mHeight) };
        min = corners[0];
        max = corners[0];
        for (const LLVector3d& c : corners)
        {
            for (S32 axis = 0; axis < 2; ++axis)
            {
                min.mdV[axis] = llmin(min.mdV[axis], c.mdV[axis]);
                max.mdV[axis] = llmax(max.mdV[axis], c.mdV[axis]);
            }
        }
    }

    F32 MapView::rotationForHeading(F32 heading)
    {
        return F_PI_BY_TWO - heading;
    }
}
