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
            // Which side is outside: the part's middle along the normal lies behind the face or before it.
            f.mOut = f.mD >= 0.5f * (hi + lo) ? 1.f : -1.f;
        }
        return faces;
    }

    namespace
    {
        // A visible triangle that can reach the part, placed and with its unit normal.
        struct Candidate
        {
            LLVector3 mP[3];
            LLVector3 mNormal;
        };

        // The spans' triangles that touch the part's bounds grown by the widest slab, gathered once for every face.
        std::vector<Candidate> gather(const std::vector<LLVector3>& tris, const std::vector<InvisibleSpan>& visible,
                                      const InvisibleParams& params)
        {
            LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (const LLVector3& p : tris)
            {
                update_min_max(lo, hi, p);
            }
            const F32 reach = params.mSlabMax + params.mSlabEpsilon;
            lo -= LLVector3(reach, reach, reach);
            hi += LLVector3(reach, reach, reach);
            std::vector<Candidate> out;
            for (const InvisibleSpan& span : visible)
            {
                for (size_t v = 0; v + 2 < span.mCount; v += 3)
                {
                    Candidate c;
                    for (S32 k = 0; k < 3; ++k)
                    {
                        c.mP[k] = span.mVerts[v + k] + span.mOffset;
                    }
                    bool outside = false;
                    for (S32 axis = 0; axis < 3 && !outside; ++axis)
                    {
                        outside = llmax(c.mP[0].mV[axis], llmax(c.mP[1].mV[axis], c.mP[2].mV[axis])) < lo.mV[axis] ||
                                  llmin(c.mP[0].mV[axis], llmin(c.mP[1].mV[axis], c.mP[2].mV[axis])) > hi.mV[axis];
                    }
                    if (outside)
                    {
                        continue;
                    }
                    c.mNormal = (c.mP[1] - c.mP[0]) % (c.mP[2] - c.mP[0]);
                    const F32 len = c.mNormal.length();
                    if (len <= 1e-8f)
                    {
                        continue;
                    }
                    c.mNormal /= len;
                    out.push_back(c);
                }
            }
            return out;
        }

        F32 cover(const InvisibleFace& face, const std::vector<LLVector3>& tris, const std::vector<Candidate>& candidates,
                  const InvisibleParams& params)
        {
            const S32 cells = llmax(params.mMask, 1);
            std::vector<U8> own(cells * cells, 0);
            for (U32 t : face.mTris)
            {
                const UV poly[3] = { project(face, tris[t * 3]), project(face, tris[t * 3 + 1]), project(face, tris[t * 3 + 2]) };
                fill(poly, 3, face, cells, own);
            }
            // Outward the slab reaches its full width; inward never through the part to its far side, which only the
            // opposite face's share may claim. Its own boundary counts as inside it.
            const F32 slab = llclamp(params.mSlabPad + face.mHalf, params.mSlabMin, params.mSlabMax);
            const F32 inward = llmin(slab, llmax(2.f * face.mHalf - params.mSlabPad, face.mHalf));
            const F32 top = face.mD + (face.mOut > 0.f ? slab : inward) + params.mSlabEpsilon;
            const F32 bottom = face.mD - (face.mOut > 0.f ? inward : slab) - params.mSlabEpsilon;
            const F32 cos_limit = std::cos(params.mNormalDeg * DEG_TO_RAD);
            std::vector<U8> covered_mask(cells * cells, 0);
            LLVector3 a[8], b[8];
            UV uv[8];
            for (const Candidate& c : candidates)
            {
                if (std::fabs(c.mNormal * face.mNormal) < cos_limit)
                {
                    continue;
                }
                a[0] = c.mP[0];
                a[1] = c.mP[1];
                a[2] = c.mP[2];
                S32 m = clipPlane(a, 3, face.mNormal, top, true, b);
                if (m < 3)
                {
                    continue;
                }
                m = clipPlane(b, m, face.mNormal, bottom, false, a);
                if (m < 3)
                {
                    continue;
                }
                for (S32 i = 0; i < m; ++i)
                {
                    uv[i] = project(face, a[i]);
                }
                fill(uv, m, face, cells, covered_mask);
            }
            U32 inside = 0, covered = 0;
            for (size_t i = 0; i < own.size(); ++i)
            {
                inside += own[i];
                covered += own[i] & covered_mask[i];
            }
            return inside ? (F32)covered / (F32)inside : 0.f;
        }

        // Whether face j's outline, in face i's frame, overlaps face i's.
        bool overlapsInPlane(const InvisibleFace& i, const InvisibleFace& j, const std::vector<LLVector3>& tris)
        {
            F32 lo_u = 1e30f, hi_u = -1e30f, lo_v = 1e30f, hi_v = -1e30f;
            for (U32 t : j.mTris)
            {
                for (U32 k = 0; k < 3; ++k)
                {
                    const UV uv = project(i, tris[t * 3 + k]);
                    lo_u = llmin(lo_u, uv.mU);
                    hi_u = llmax(hi_u, uv.mU);
                    lo_v = llmin(lo_v, uv.mV);
                    hi_v = llmax(hi_v, uv.mV);
                }
            }
            return llmin(hi_u, i.mMaxU) > llmax(lo_u, i.mMinU) && llmin(hi_v, i.mMaxV) > llmax(lo_v, i.mMinV);
        }
    }

    F32 invisibleFaceCover(const InvisibleFace& face, const std::vector<LLVector3>& tris,
                           const std::vector<InvisibleSpan>& visible, const InvisibleParams& params)
    {
        return cover(face, tris, gather(tris, visible, params), params);
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
        const std::vector<Candidate> candidates = gather(tris, visible, params);
        // Only the largest faces are judged; the rest, like those under the minimum area, are left out.
        std::vector<size_t> order(faces.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&faces](size_t a, size_t b) { return faces[a].mArea > faces[b].mArea; });
        result.mShares.assign(faces.size(), -1.f);
        for (size_t k = 0; k < order.size() && k < params.mMaxFaces; ++k)
        {
            const InvisibleFace& f = faces[order[k]];
            if (f.mArea >= params.mMinFaceArea)
            {
                result.mShares[order[k]] = cover(f, tris, candidates, params);
            }
        }
        F32 total = 0.f, covered = 0.f, steep_covered = 0.f;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            F32 share = result.mShares[i];
            if (share < 0.f)
            {
                continue;
            }
            // A thin slab stands for the surface on either side: its face takes the opposite face's cover when that
            // face lies across the slab from it, over it.
            const InvisibleFace& fi = faces[i];
            const F32 extent = llmin(fi.mMaxU - fi.mMinU, fi.mMaxV - fi.mMinV);
            if (2.f * fi.mHalf <= params.mShareThinRatio * extent)
            {
                for (size_t j = 0; j < faces.size(); ++j)
                {
                    const InvisibleFace& fj = faces[j];
                    if (result.mShares[j] > share && fi.mNormal * fj.mNormal <= -params.mCoplanarCos &&
                        std::fabs(fi.mD + fj.mD) <= 2.f * fi.mHalf + params.mSlabPad && overlapsInPlane(fi, fj, tris))
                    {
                        share = result.mShares[j];
                    }
                }
            }
            total += fi.mArea;
            covered += fi.mArea * share;
            if (std::fabs(fi.mNormal.mV[VZ]) < params.mSteepNz)
            {
                result.mSteepArea += fi.mArea;
                steep_covered += fi.mArea * share;
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
