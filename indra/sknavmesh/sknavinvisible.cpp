/**
 * @file sknavinvisible.cpp
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

#include "linden_common.h"

#include "sknavinvisible.h"

#include "llmath.h"

#include <algorithm>
#include <cmath>

namespace sknav
{
    namespace
    {
        struct UV
        {
            F32 mU = 0.f;
            F32 mV = 0.f;
        };

        // Keeps the part of a convex polygon on one side of a plane: n . p <= k, or >= k when !below.
        S32 clipPlane(const LLVector3* in, S32 count, const LLVector3& n, F32 k, bool below, LLVector3* out)
        {
            S32 m = 0;
            for (S32 i = 0; i < count; ++i)
            {
                const LLVector3& a = in[i];
                const LLVector3& b = in[(i + 1) % count];
                F32 da = n * a - k;
                F32 db = n * b - k;
                if (!below)
                {
                    da = -da;
                    db = -db;
                }
                if (da <= 0.f)
                {
                    out[m++] = a;
                }
                if ((da <= 0.f) != (db <= 0.f))
                {
                    out[m++] = a + (b - a) * (da / (da - db));
                }
            }
            return m;
        }

        // Sets every mask cell whose centre a convex polygon holds, either winding.
        void fill(const UV* poly, S32 count, const InvisibleFace& face, S32 cells, std::vector<U8>& mask)
        {
            if (count < 3)
            {
                return;
            }
            F32 area = 0.f;
            F32 lo_u = 1e30f, hi_u = -1e30f, lo_v = 1e30f, hi_v = -1e30f;
            for (S32 i = 0; i < count; ++i)
            {
                const UV& a = poly[i];
                const UV& b = poly[(i + 1) % count];
                area += a.mU * b.mV - b.mU * a.mV;
                lo_u = llmin(lo_u, a.mU);
                hi_u = llmax(hi_u, a.mU);
                lo_v = llmin(lo_v, a.mV);
                hi_v = llmax(hi_v, a.mV);
            }
            if (std::fabs(area) <= 1e-12f)
            {
                return;
            }
            const F32 sign = area > 0.f ? 1.f : -1.f;
            const F32 du = (face.mMaxU - face.mMinU) / cells;
            const F32 dv = (face.mMaxV - face.mMinV) / cells;
            if (du <= 0.f || dv <= 0.f)
            {
                return;
            }
            const S32 i0 = llclamp((S32)std::floor((lo_u - face.mMinU) / du - 0.5f), 0, cells - 1);
            const S32 i1 = llclamp((S32)std::ceil((hi_u - face.mMinU) / du - 0.5f), 0, cells - 1);
            const S32 j0 = llclamp((S32)std::floor((lo_v - face.mMinV) / dv - 0.5f), 0, cells - 1);
            const S32 j1 = llclamp((S32)std::ceil((hi_v - face.mMinV) / dv - 0.5f), 0, cells - 1);
            for (S32 j = j0; j <= j1; ++j)
            {
                const F32 v = face.mMinV + (j + 0.5f) * dv;
                for (S32 i = i0; i <= i1; ++i)
                {
                    const F32 u = face.mMinU + (i + 0.5f) * du;
                    bool inside = true;
                    for (S32 e = 0; e < count && inside; ++e)
                    {
                        const UV& a = poly[e];
                        const UV& b = poly[(e + 1) % count];
                        inside = sign * ((b.mU - a.mU) * (v - a.mV) - (b.mV - a.mV) * (u - a.mU)) >= 0.f;
                    }
                    if (inside)
                    {
                        mask[j * cells + i] = 1;
                    }
                }
            }
        }

        UV project(const InvisibleFace& face, const LLVector3& p)
        {
            return { face.mU * p, face.mV * p };
        }
    }

    std::vector<InvisibleFace> invisibleFaces(const std::vector<LLVector3>& tris, const InvisibleParams& params)
    {
        std::vector<InvisibleFace> faces;
        for (U32 t = 0; t + 2 < tris.size(); t += 3)
        {
            LLVector3 n = (tris[t + 1] - tris[t]) % (tris[t + 2] - tris[t]);
            const F32 len = n.length();
            if (len <= 1e-8f)
            {
                continue;
            }
            n /= len;
            const F32 d = n * tris[t];
            InvisibleFace* into = nullptr;
            for (InvisibleFace& f : faces)
            {
                if (f.mNormal * n >= params.mCoplanarCos && std::fabs(f.mD - d) <= params.mCoplanarD)
                {
                    into = &f;
                    break;
                }
            }
            if (!into)
            {
                faces.emplace_back();
                into = &faces.back();
                into->mNormal = n;
                into->mD = d;
            }
            into->mTris.push_back(t / 3);
            into->mArea += 0.5f * len;
        }
        for (InvisibleFace& f : faces)
        {
            const LLVector3 ref = std::fabs(f.mNormal.mV[VZ]) < 0.9f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f);
            f.mU = ref % f.mNormal;
            f.mU.normVec();
            f.mV = f.mNormal % f.mU;
            f.mMinU = f.mMinV = 1e30f;
            f.mMaxU = f.mMaxV = -1e30f;
            for (U32 t : f.mTris)
            {
                for (U32 k = 0; k < 3; ++k)
                {
                    const UV uv = project(f, tris[t * 3 + k]);
                    f.mMinU = llmin(f.mMinU, uv.mU);
                    f.mMaxU = llmax(f.mMaxU, uv.mU);
                    f.mMinV = llmin(f.mMinV, uv.mV);
                    f.mMaxV = llmax(f.mMaxV, uv.mV);
                }
            }
            F32 lo = 1e30f, hi = -1e30f;
            for (const LLVector3& p : tris)
            {
                lo = llmin(lo, f.mNormal * p);
                hi = llmax(hi, f.mNormal * p);
            }
            f.mHalf = 0.5f * (hi - lo);
        }
        return faces;
    }

    F32 invisibleFaceCover(const InvisibleFace& face, const std::vector<LLVector3>& tris,
                           const std::vector<InvisibleSpan>& visible, const InvisibleParams& params)
    {
        const S32 cells = llmax(params.mMask, 1);
        std::vector<U8> own(cells * cells, 0);
        for (U32 t : face.mTris)
        {
            const UV poly[3] = { project(face, tris[t * 3]), project(face, tris[t * 3 + 1]), project(face, tris[t * 3 + 2]) };
            fill(poly, 3, face, cells, own);
        }
        const F32 slab = llclamp(params.mSlabPad + face.mHalf, params.mSlabMin, params.mSlabMax);
        const F32 cos_limit = std::cos(params.mNormalDeg * DEG_TO_RAD);
        // The face's box grown by the slab: triangles wholly outside it are skipped before any clipping.
        LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (U32 t : face.mTris)
        {
            for (U32 k = 0; k < 3; ++k)
            {
                update_min_max(lo, hi, tris[t * 3 + k]);
            }
        }
        lo -= LLVector3(slab, slab, slab);
        hi += LLVector3(slab, slab, slab);
        std::vector<U8> cover(cells * cells, 0);
        LLVector3 a[8], b[8];
        UV uv[8];
        for (const InvisibleSpan& span : visible)
        {
            for (size_t v = 0; v + 2 < span.mCount; v += 3)
            {
                const LLVector3* p = span.mVerts + v;
                bool out = false;
                for (S32 axis = 0; axis < 3 && !out; ++axis)
                {
                    out = llmax(p[0].mV[axis], llmax(p[1].mV[axis], p[2].mV[axis])) < lo.mV[axis] ||
                          llmin(p[0].mV[axis], llmin(p[1].mV[axis], p[2].mV[axis])) > hi.mV[axis];
                }
                if (out)
                {
                    continue;
                }
                const LLVector3 n = (p[1] - p[0]) % (p[2] - p[0]);
                const F32 len = n.length();
                if (len <= 1e-8f || std::fabs(n * face.mNormal) < cos_limit * len)
                {
                    continue;
                }
                a[0] = p[0];
                a[1] = p[1];
                a[2] = p[2];
                S32 m = clipPlane(a, 3, face.mNormal, face.mD + slab, true, b);
                if (m < 3)
                {
                    continue;
                }
                m = clipPlane(b, m, face.mNormal, face.mD - slab, false, a);
                if (m < 3)
                {
                    continue;
                }
                for (S32 i = 0; i < m; ++i)
                {
                    uv[i] = project(face, a[i]);
                }
                fill(uv, m, face, cells, cover);
            }
        }
        U32 inside = 0, covered = 0;
        for (size_t i = 0; i < own.size(); ++i)
        {
            inside += own[i];
            covered += own[i] & cover[i];
        }
        return inside ? (F32)covered / (F32)inside : 0.f;
    }

    InvisibleResult invisibleVerdict(const std::vector<LLVector3>& tris, const std::vector<InvisibleSpan>& visible,
                                     const InvisibleParams& params)
    {
        InvisibleResult result;
        if (tris.size() / 3 > params.mMaxPartTris)
        {
            return result;
        }
        const std::vector<InvisibleFace> faces = invisibleFaces(tris, params);
        result.mShares.reserve(faces.size());
        for (const InvisibleFace& f : faces)
        {
            result.mShares.push_back(f.mArea < params.mMinFaceArea ? -1.f : invisibleFaceCover(f, tris, visible, params));
        }
        F32 total = 0.f, covered = 0.f, steep_covered = 0.f;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            F32 share = result.mShares[i];
            if (share < 0.f)
            {
                continue;
            }
            for (size_t j = 0; j < faces.size(); ++j)
            {
                if (result.mShares[j] >= 0.f && faces[i].mNormal * faces[j].mNormal <= -params.mCoplanarCos)
                {
                    share = llmax(share, result.mShares[j]);
                }
            }
            total += faces[i].mArea;
            covered += faces[i].mArea * share;
            if (std::fabs(faces[i].mNormal.mV[VZ]) < params.mSteepNz)
            {
                result.mSteepArea += faces[i].mArea;
                steep_covered += faces[i].mArea * share;
            }
        }
        if (total <= 0.f)
        {
            return result;
        }
        result.mCoveredAll = covered / total;
        result.mCoveredSteep = result.mSteepArea > 0.f ? steep_covered / result.mSteepArea : result.mCoveredAll;
        result.mVerdict = result.mCoveredAll < params.mCoveredShare && result.mCoveredSteep < params.mCoveredShare
                              ? InvisibleVerdict::BARRIER
                              : InvisibleVerdict::STAND_IN;
        return result;
    }
}
