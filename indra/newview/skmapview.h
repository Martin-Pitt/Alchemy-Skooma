/**
 * @file skmapview.h
 * @brief The mini-map's view transform, and analysis box: pure maths, no viewer state.
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

#ifndef SK_MAPVIEW_H
#define SK_MAPVIEW_H

#include "llmath.h"
#include "v2math.h"
#include "v3dmath.h"

#include <vector>

namespace skmap
{
    constexpr F32 ZOOM_MIN_M = 4.f;         ///< the shortest span the map shows
    constexpr F32 ZOOM_MAX_M = 2048.f;     ///< eight regions; the floorplan reaches only its analysis box, region tiles beyond
    constexpr F32 ZOOM_DEFAULT_M = 128.f;
    constexpr F32 ZOOM_STEP = 1.1f;         ///< per wheel notch

    constexpr F64 ANALYSIS_SNAP_M = 64.0;
    constexpr F64 ANALYSIS_MARGIN_M = 32.0;
    constexpr F64 ANALYSIS_REACH_M = 256.0; ///< half the widest span along the short side
    constexpr F64 ANALYSIS_JOIN_M = 256.0;  ///< the agent shares the box only while this close to the map centre in plan

    /// The box the map holds the census and air over: the agent and the map centre, each with the widest
    /// view and a margin around it, snapped outward to 64 m. It depends on the two positions and
    /// the widget's aspect only, never on the zoom.
    void analysisBox(const LLVector3d& agent, const LLVector3d& centre, F32 long_side_px, F32 short_side_px, LLVector3d& min,
                     LLVector3d& max);

    /// A span after wheel notches, ×1.1 each, clamped to 4..2048 m. Positive notches zoom out.
    F32 zoomByNotches(F32 span, F32 notches);

    /// Global metres to widget pixels and back. Pixels are GL style, origin bottom left; the
    /// map centre sits in the middle of the widget. Rotation turns the world counter-clockwise
    /// on screen: 0 is north up.
    class MapView
    {
    public:
        void set(const LLVector3d& centre, F32 span, F32 rotation, F32 width, F32 height);

        const LLVector3d& centre() const { return mCentre; }
        F32 span() const { return mSpan; }
        F32 rotation() const { return mRotation; }
        F32 width() const { return mWidth; }
        F32 height() const { return mHeight; }
        F32 shortSide() const { return llmin(mWidth, mHeight); }
        F32 longSide() const { return llmax(mWidth, mHeight); }
        /// Metres per pixel: the span is along the short side.
        F32 mpp() const { return mMpp; }
        F32 pixelsPerMetre() const { return mMpp > 0.f ? 1.f / mMpp : 0.f; }

        LLVector2 toWidget(const LLVector3d& global) const;
        /// A plan direction or offset in metres to pixels, rotation applied.
        LLVector2 toWidgetOffset(F32 dx, F32 dy) const;
        LLVector3d toGlobal(F32 x, F32 y) const;
        /// The global box the widget shows, rotation included.
        void visibleBox(LLVector3d& min, LLVector3d& max) const;

        /// The rotation that puts a world heading (radians from +x, counter-clockwise) at the top.
        static F32 rotationForHeading(F32 heading);

    private:
        LLVector3d mCentre;
        F32 mSpan = ZOOM_DEFAULT_M;
        F32 mRotation = 0.f;
        F32 mWidth = 1.f;
        F32 mHeight = 1.f;
        F32 mMpp = 1.f;
        F32 mCos = 1.f;
        F32 mSin = 0.f;
    };
}

#endif // SK_MAPVIEW_H
