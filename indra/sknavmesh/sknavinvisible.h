/**
 * @file sknavinvisible.h
 * @brief Whether an invisible part stands in for visible geometry, adds to it, or is a barrier of its own.
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

#ifndef SK_NAVINVISIBLE_H
#define SK_NAVINVISIBLE_H

#include "sknavtypes.h"

#include "v3dmath.h"
#include "v3math.h"

#include <vector>

namespace sknav
{
    /// The cover test's tunables.
    struct InvisibleParams
    {
        F32 mProxyTolerance = 0.15f; ///< a part this far out of a visible-only box is still its proxy; a soup's prisms reach this far
        F32 mMinFaceArea = 0.05f;   ///< no more than this sticking out of the colliding neighbours is no extra
        F32 mCoplanarCos = 0.999f;  ///< triangles this parallel
        F32 mCoplanarD = 0.01f;     ///< and this close in plane offset are one face; a face this close outside a solid's lies on it
        U32 mMaxPartTris = 4096;    ///< parts with more triangles are not judged
        U32 mMaxPieces = 65536;     ///< a part clipped into more pieces than this is too fine to judge
    };

    /// A coplanar group of a part's triangles.
    struct InvisibleFace
    {
        LLVector3 mNormal;
        F32 mD = 0.f;               ///< plane offset: n . p
        F32 mArea = 0.f;
        std::vector<U32> mTris;     ///< the part's triangles in it, by index
    };

    enum class CoverVerdict : U8
    {
        PROXY,              ///< stands in for visible geometry: drawn as its class
        EXTRA,              ///< sticks out past the colliding neighbours: drawn as its class, what sticks out hatched
        BARRIER,            ///< nothing visible near it: drawn as its class and hatched whole
        UNKNOWN_PHYSICS,    ///< a neighbour's physics type has not arrived, or the part is too fine: drawn as its class
    };
    const char* coverVerdictName(CoverVerdict verdict);

    /// A visible neighbour as the verdict takes it, values only; never owns what it points at.
    struct CoverCandidate
    {
        const LLVector3* mVerts = nullptr;  ///< colliding: its shape's triangles, three vertices each
        size_t mCount = 0;                  ///< vertices
        LLVector3 mOffset;                  ///< added to every vertex and to mCenter: into the part's frame
        bool mCollides = true;
        bool mPhysicsKnown = true;
        Shape mShape = Shape::BOX;
        const S32* mConvexTris = nullptr;   ///< a soup's convex bodies: triangle count of each
        size_t mBodies = 0;
        LLVector3 mCenter;                  ///< not colliding: the box the part must lie in
        LLVector3 mAxes[3];
        LLVector3 mHalf;
    };

    struct CoverResult
    {
        CoverVerdict mVerdict = CoverVerdict::UNKNOWN_PHYSICS;
        std::vector<LLVector3> mProtruding; ///< EXTRA: what sticks out, three vertices a triangle, in the part's frame
        F32 mOutsideArea = 0.f;     ///< what sticks out; all of it for a barrier
        F32 mPartArea = 0.f;
        bool mSurfaceClip = false;  ///< a soup without bodies was clipped by its triangles' prisms
        bool mTooFine = false;      ///< over mMaxPartTris triangles or mMaxPieces pieces
    };

    /// A part's faces: its triangles (three vertices each) grouped by plane.
    std::vector<InvisibleFace> invisibleFaces(const std::vector<LLVector3>& tris, const InvisibleParams& params);

    /// An invisible part's verdict over its visible neighbours, in this order: none is a barrier; a neighbour whose
    /// physics type is not known yet leaves it unknown; a non-colliding neighbour whose box (grown by mProxyTolerance)
    /// holds every vertex makes it a proxy; with no colliding neighbour it is a barrier; otherwise what is left of its triangles once every colliding neighbour's
    /// convex pieces are taken away sticks out, extra when more than mMinFaceArea.
    CoverResult coverVerdict(const std::vector<LLVector3>& tris, const std::vector<CoverCandidate>& candidates,
                             const InvisibleParams& params);

    /// Whether a census record may be a neighbour in the cover test, short of its look: static, in the solid raster,
    /// and drawn by the overview (or a surround, or visible only, which it keeps for this test alone).
    bool coverPoolRecord(const Record& rec);

    /// The records' shapes as the verdict takes them, in the frame of a region whose origin is given: a colliding
    /// record's emitted triangles, a visible-only record's box, a surround's bounds unless a colliding record of its
    /// part is among them. scratch holds the triangles the candidates point at.
    std::vector<CoverCandidate> coverCandidates(const std::vector<const Record*>& records, const LLVector3d& origin,
                                                std::vector<LLVector3>& scratch);
}

#endif // SK_NAVINVISIBLE_H
