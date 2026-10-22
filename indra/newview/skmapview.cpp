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
        F64 snapDown(F64 v, F64 step)
        {
            return std::floor(v / step) * step;
        }

        F64 snapUp(F64 v, F64 step)
        {
            return std::ceil(v / step) * step;
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

    F32 zoomByNotches(F32 span, F32 notches)
    {
        return llclamp(span * std::pow(ZOOM_STEP, notches), ZOOM_MIN_M, ZOOM_MAX_M);
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
