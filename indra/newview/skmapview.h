/**
 * @file skmapview.h
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

#ifndef SK_MAPVIEW_H
#define SK_MAPVIEW_H

#include "llmath.h"
#include "v2math.h"
#include "v3dmath.h"

#include <vector>

namespace skmap
{
    constexpr F32 ZOOM_MIN_M = 4.f;         ///< the shortest span the map shows
    constexpr F32 ZOOM_MAX_M = 512.f;
    constexpr F32 ZOOM_DEFAULT_M = 128.f;
    constexpr F32 ZOOM_STEP = 1.1f;         ///< per wheel notch, and the ribbon bucket ratio

    constexpr S32 TIER_MIN = 0;
    constexpr S32 TIER_MAX = 6;
    constexpr S32 CELL_TEXELS = 256;
    constexpr F32 TIER_HYSTERESIS = 1.15f;

    constexpr F64 ANALYSIS_SNAP_M = 64.0;
    constexpr F64 ANALYSIS_MARGIN_M = 32.0;
    constexpr F64 ANALYSIS_REACH_M = 256.0; ///< half the widest span along the short side
    constexpr F64 ANALYSIS_JOIN_M = 256.0;  ///< the agent shares the box only while this close to the map centre in plan

    /// A tier's texel in metres: 1/32 m at tier 0 to 2 m at tier 6.
    F32 tierTexel(S32 tier);
    /// A tier's cell side in metres: 256 texels, 8 m to 512 m.
    F64 tierCellSize(S32 tier);

    /// The tier for a map scale, keeping the current one inside a 15% margin either way.
    /// mpp is metres per screen pixel; the target texel is half of it.
    S32 chooseTier(F32 mpp, S32 current);

    /// One bake cell on the global grid of its tier.
    struct CellId
    {
        S32 mTier = 0;
        S32 mX = 0;
        S32 mY = 0;
        bool operator==(const CellId& o) const { return mTier == o.mTier && mX == o.mX && mY == o.mY; }
        bool operator!=(const CellId& o) const { return !(*this == o); }
        bool operator<(const CellId& o) const
        {
            return mTier != o.mTier ? mTier < o.mTier : mX != o.mX ? mX < o.mX : mY < o.mY;
        }
        LLVector3d minCorner() const;
        LLVector3d maxCorner() const;
        /// The cell of a coarser tier that holds this one.
        CellId ancestor(S32 tier) const;
    };

    /// The cell of a tier holding a global position.
    CellId cellAt(S32 tier, F64 x, F64 y);
    /// Every cell of the tier that meets the global box, row by row.
    void cellsInBox(S32 tier, const LLVector3d& min, const LLVector3d& max, std::vector<CellId>& out);

    /// The box the scene is analysed over: the agent and the map centre, each with the widest
    /// view and a margin around it, snapped outward to 64 m. It depends on the two positions and
    /// the widget's aspect only, never on the zoom.
    void analysisBox(const LLVector3d& agent, const LLVector3d& centre, F32 long_side_px, F32 short_side_px, LLVector3d& min,
                     LLVector3d& max);

    /// A span after wheel notches, ×1.1 each, clamped to 4..512 m. Positive notches zoom out.
    F32 zoomByNotches(F32 span, F32 notches);
    /// The ×1.1 bucket a scale falls in, and back: ribbons are rebuilt only when it moves.
    S32 zoomBucket(F32 mpp);
    F32 bucketMpp(S32 bucket);

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
