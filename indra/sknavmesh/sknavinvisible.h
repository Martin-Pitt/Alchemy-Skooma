/**
 * @file sknavinvisible.h
 * @brief Whether an invisible part stands in for visible geometry or is a barrier of its own.
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

#include "v3math.h"

#include <vector>

namespace sknav
{
    /// The overlap test's tunables.
    struct InvisibleParams
    {
        F32 mNormalDeg = 25.f;      ///< a visible triangle this close to a face's normal, either sign, can cover it
        F32 mSlabMin = 0.3f;        ///< and lies within the face's slab: this much either side of its plane at least,
        F32 mSlabPad = 0.1f;        ///< this much more than the part's half thickness along the normal,
        F32 mSlabMax = 1.f;         ///< and never more than this
        F32 mCoveredShare = 0.5f;   ///< a part covered this much (all faces, or its steep ones) is a stand-in
        F32 mMinFaceArea = 0.05f;   ///< smaller faces (a buried slab's edges) are left out of the verdict
        F32 mSteepNz = 0.5f;        ///< a face whose normal's |z| is below this is steep, wall-like
        F32 mCoplanarCos = 0.999f;  ///< triangles this parallel
        F32 mCoplanarD = 0.01f;     ///< and this close in plane offset are one face
        S32 mMask = 32;             ///< each face's coverage mask is this many cells a side
        U32 mMaxPartTris = 4096;    ///< parts with more triangles are not judged
    };

    /// A coplanar group of a part's triangles, in a frame on its plane.
    struct InvisibleFace
    {
        LLVector3 mNormal;
        F32 mD = 0.f;               ///< plane offset: n . p
        LLVector3 mU;               ///< in-plane axes
        LLVector3 mV;
        F32 mMinU = 0.f, mMaxU = 0.f, mMinV = 0.f, mMaxV = 0.f;
        F32 mHalf = 0.f;            ///< the part's half thickness along the normal
        F32 mArea = 0.f;
        std::vector<U32> mTris;     ///< the part's triangles in it, by index
    };

    /// A run of triangles, three vertices each, that may cover a face; never owned.
    struct InvisibleSpan
    {
        const LLVector3* mVerts = nullptr;
        size_t mCount = 0;          ///< vertices
    };

    enum class InvisibleVerdict : U8
    {
        STAND_IN,   ///< lies on visible geometry: drawn as its class
        BARRIER,    ///< nothing visible on it: drawn as its class and hatched
        UNJUDGED,   ///< too many triangles or no face large enough: drawn as its class
    };

    struct InvisibleResult
    {
        InvisibleVerdict mVerdict = InvisibleVerdict::UNJUDGED;
        std::vector<F32> mShares;   ///< per face, its own cover share; -1 for a face under the minimum area
        F32 mCoveredAll = 0.f;      ///< area-weighted share over the judged faces, opposite faces taking the better
        F32 mCoveredSteep = 0.f;    ///< the same over the steep ones; mCoveredAll when there are none
        F32 mSteepArea = 0.f;
    };

    /// A part's faces: its triangles (three vertices each) grouped by plane.
    std::vector<InvisibleFace> invisibleFaces(const std::vector<LLVector3>& tris, const InvisibleParams& params);

    /// The share of a face the spans' matching triangles cover, from a mask over the face's plane.
    F32 invisibleFaceCover(const InvisibleFace& face, const std::vector<LLVector3>& tris,
                           const std::vector<InvisibleSpan>& visible, const InvisibleParams& params);

    /// A part is a barrier when both its faces and its steep faces are covered less than mCoveredShare; a slab stands
    /// for the surface on either side, so a face takes its opposite face's share when that is more.
    InvisibleResult invisibleVerdict(const std::vector<LLVector3>& tris, const std::vector<InvisibleSpan>& visible,
                                     const InvisibleParams& params);
}

#endif // SK_NAVINVISIBLE_H
