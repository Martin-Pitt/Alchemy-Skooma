/**
 * @file skmapribbons.h
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

#ifndef SK_MAPRIBBONS_H
#define SK_MAPRIBBONS_H

#include "skmapgeom.h"

#include "v4coloru.h"

#include <set>
#include <utility>
#include <vector>

namespace skmap
{
    /// Global 4 m cells whose marks are left out: a storey's cut over the interior.
    typedef std::set<std::pair<S32, S32>> CutCells;

    /// What a layer's ribbons are built for. Widths are screen pixels times mMpp.
    struct RibbonParams
    {
        F32 mMpp = 1.f;             ///< the zoom bucket's metres per pixel
        F32 mEye = 1.8f;            ///< the reader's eye height, for cover
        F32 mClimb = 0.75f;
        F32 mPeek = 0.15f;
        bool mCover = true;         ///< walls, ledges, drops and overhangs
        bool mDoors = true;         ///< doors, ladders and elevators
    };

    /// Triangles in scene-local metres at the build scale, z 0, three vertices each, colour per vertex.
    /// Each vertex also keeps its offset from its line in screen pixels, so a draw at another scale moves it by
    /// mOffsets[i] * (mpp - mMpp) and widths stay constant on screen while a zoom outruns the rebuild.
    struct Ribbons
    {
        std::vector<LLVector3> mVerts;
        std::vector<LLVector2> mOffsets;
        std::vector<LLColor4U> mColours;
        F32 mMpp = 1.f;
        void clear()
        {
            mVerts.clear();
            mOffsets.clear();
            mColours.clear();
        }
        /// Vertex i as drawn at another scale.
        LLVector3 at(size_t i, F32 mpp) const
        {
            const F32 k = mpp - mMpp;
            return LLVector3(mVerts[i].mV[VX] + mOffsets[i].mV[VX] * k, mVerts[i].mV[VY] + mOffsets[i].mV[VY] * k, 0.f);
        }
    };

    /// Consecutive collinear marks of one kind and style, merged.
    struct MarkRun
    {
        MarkKind mKind = MarkKind::WALL;
        sknav::Cover mCover = sknav::Cover::NONE;   ///< WALL only
        LLVector2 mA, mB;
        S8 mFloorSide = 0;
        F32 mHeight = 0.f;
        F32 mSweep = 0.f;                           ///< DOOR_SWING: signed radians from mB about mA
        F32 length() const { return (mB - mA).length(); }
    };

    constexpr F32 MERGE_ANGLE_DEG = 5.f;
    constexpr F32 MERGE_GAP_M = 0.2f;

    /// The layer's marks merged into runs, with the length floor applied to walls, ledges,
    /// overhangs and drop edges. A line loses the parts that lie in cut cells, a glyph goes when its anchor does.
    void mergeMarks(const Scene& scene, S32 layer, const RibbonParams& params, std::vector<MarkRun>& out, const CutCells* cut = nullptr);

    /// One layer's ribbons: cover, ledges, drops, overhangs, doors and glyphs.
    void buildLayerRibbons(const Scene& scene, S32 layer, const RibbonParams& params, Ribbons& out, const CutCells* cut = nullptr);
    /// The region and void edges: void solid, region dashed.
    void buildBoundaryRibbons(const Scene& scene, const RibbonParams& params, Ribbons& out);
}

#endif // SK_MAPRIBBONS_H
