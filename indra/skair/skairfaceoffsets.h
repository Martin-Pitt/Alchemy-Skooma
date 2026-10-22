/**
 * @file skairfaceoffsets.h
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

#ifndef SK_AIRFACEOFFSETS_H
#define SK_AIRFACEOFFSETS_H

#include "stdtypes.h"
#include "v3math.h"

#include <vector>

namespace skworld
{
    struct AirGrid;
    struct TileAir;

    /// Steps a cell is divided into for a face's position.
    constexpr S32 FACE_OFFSET_STEPS = 16;

    /// The band over a floor a jamb is looked for in: from a step above it to this.
    constexpr F32 FACE_BAND_TOP_M = 1.5f;

    /// A solid column's side facing air: how far the true face lies in from
    /// the cell boundary, in FACE_OFFSET_STEPS of a cell, rounded towards the
    /// air, the nearest over the floors whose jamb bands meet it. The key is
    /// column * 4 + side, the side being the direction the face looks
    /// (TileAir::Side). Only faces off the boundary carry an entry; sorted by key.
    struct FaceOffset
    {
        U32 mBits = 0;      ///< key << 5 | offset

        FaceOffset() = default;
        FaceOffset(U32 key, U32 offset) : mBits(key << 5 | offset) {}
        U32 key() const { return mBits >> 5; }
        U32 offset() const { return mBits & 31u; }
    };

    inline U32 faceKey(S32 ix, S32 iy, S32 width, S32 side) { return static_cast<U32>(ix + iy * width) * 4u + static_cast<U32>(side); }

    struct FaceOffsetStats
    {
        U32 mSides = 0;         ///< solid sides facing a floor's jamb band
        U32 mEntries = 0;       ///< entries kept for the tile proper
        F64 mTrueSum = 0.0;     ///< metres from the boundary to the true face, summed over sides with triangles
        F64 mResidualSum = 0.0; ///< metres from the quantised face to the true face
        U32 mMeasured = 0;      ///< sides with triangles
        F64 mMs = 0.0;
        /// Traces only sides whose column centre lies in this box, mesh-local x0, y0, x1, y1, when x1 > x0.
        F32 mTraceBox[4] = { 0.f, 0.f, 0.f, 0.f };
    };

    /// One measured side and the triangles it was measured from, for an oracle.
    struct FaceOffsetTrace
    {
        F32 mX0 = 0.f;          ///< the solid cell's corner, mesh-local
        F32 mY0 = 0.f;
        S32 mSide = 0;
        U16 mFloor = 0;
        F32 mZLo = 0.f;         ///< mesh-local metres
        F32 mZHi = 0.f;
        U8 mOffset = 0;
        std::vector<LLVector3> mTris;   ///< mesh-local SL
    };

    /// The face offsets of a grid's solid columns from the triangles that
    /// made them, given in Recast space as a soup (skairraster.h's toRecast).
    /// A column is solid for a floor AVATAR stands on where no gap of it spans
    /// the band from climb + 1 to FACE_BAND_TOP_M over that floor, as the spaces read it.
    /// trace_every > 0 traces every that many measured sides.
    void computeFaceOffsets(const AirGrid& grid, const F32* verts, const S32* tris, S32 ntris, S32 climb, std::vector<FaceOffset>& out,
                            FaceOffsetStats* stats = nullptr, S32 trace_every = 0, std::vector<FaceOffsetTrace>* trace = nullptr);

    /// A padded grid's table in the cropped grid's columns, pad cells in from each side.
    void cropFaceOffsets(const std::vector<FaceOffset>& padded, S32 padded_width, S32 pad, S32 width, S32 height, std::vector<FaceOffset>& out);

    /// The offset of a column's side, in cells; 0 when it has none.
    F32 faceOffset(const std::vector<FaceOffset>& table, U32 key);

    /// The extreme of a triangle clipped to a box along an axis: the least
    /// coordinate when lowest, else the greatest. False when nothing is left.
    bool clippedExtreme(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& lo, const LLVector3& hi, S32 axis,
                        bool lowest, F32& out);

    /// The true face of a side over a box of solid behind it: metres from the
    /// box's air-side boundary in, or a negative value when no triangle reaches the box.
    F32 trueFaceDepth(const std::vector<LLVector3>& tris, const LLVector3& lo, const LLVector3& hi, S32 side);

    struct WallFace;

    /// Move each wall face from its cell boundary back to the true face of the
    /// triangles in the cell of solid behind it, over its height less a unit
    /// each end so the floor and the top do not count. Faces with no triangle
    /// behind them stay. Soup in Recast space, faces mesh-local SL.
    void alignWallFaces(std::vector<WallFace>& faces, const F32* verts, const S32* tris, S32 ntris, F32 cell, F32 unit);
}

#endif // SK_AIRFACEOFFSETS_H
