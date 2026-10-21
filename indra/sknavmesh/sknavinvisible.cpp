/**
 * @file sknavinvisible.cpp
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

#include "linden_common.h"

#include "sknavinvisible.h"

#include "llmath.h"
#include "sknavbuild.h"

#include <algorithm>
#include <cmath>

namespace sknav
{
    namespace
    {
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

        typedef std::vector<LLVector3> Poly;

        void clip(const Poly& in, const LLVector3& n, F32 k, bool below, Poly& out)
        {
            // A vertex and a crossing per edge at most: rounding can make a piece cross a plane more than twice.
            out.resize(in.size() * 2);
            out.resize(in.size() < 3 ? 0 : clipPlane(in.data(), (S32)in.size(), n, k, below, out.data()));
        }

        F32 polyArea(const Poly& p)
        {
            LLVector3 sum;
            for (size_t i = 1; i + 1 < p.size(); ++i)
            {
                sum += (p[i] - p[0]) % (p[i + 1] - p[0]);
            }
            return 0.5f * sum.length();
        }

        // A convex piece of a colliding neighbour: outward planes, n . p <= k inside, and its bounds.
        struct Piece
        {
            std::vector<std::pair<LLVector3, F32>> mPlanes;
            LLVector3 mLo{ 1e30f, 1e30f, 1e30f };
            LLVector3 mHi{ -1e30f, -1e30f, -1e30f };
            bool mPrism = false;
        };

        bool overlaps(const LLVector3& lo0, const LLVector3& hi0, const LLVector3& lo1, const LLVector3& hi1)
        {
            for (S32 a = 0; a < 3; ++a)
            {
                if (hi0.mV[a] < lo1.mV[a] || lo0.mV[a] > hi1.mV[a])
                {
                    return false;
                }
            }
            return true;
        }

        // A triangle's unit normal; false for a sliver, whose normal is rounding noise (a tessellated sphere's pole).
        bool planeOf(const LLVector3& a, const LLVector3& b, const LLVector3& c, LLVector3& n)
        {
            n = (b - a) % (c - a);
            const F32 len = n.length();
            const F32 longest = llmax((b - a).lengthSquared(), llmax((c - b).lengthSquared(), (a - c).lengthSquared()));
            if (len <= 1e-8f || len <= 1e-6f * longest)
            {
                return false;
            }
            n /= len;
            return true;
        }

        // The piece's bounds as planes: a sharp corner grown outward reaches no further than they do.
        void boxPlanes(Piece& piece)
        {
            for (S32 axis = 0; axis < 3; ++axis)
            {
                LLVector3 n;
                n.mV[axis] = 1.f;
                piece.mPlanes.emplace_back(n, piece.mHi.mV[axis]);
                piece.mPlanes.emplace_back(-n, -piece.mLo.mV[axis]);
            }
        }

        // A convex body's distinct planes from its triangles, oriented away from its vertices' mean; none when it does
        // not reach the part's bounds.
        Piece bodyPiece(const LLVector3* v, size_t count, const LLVector3& offset, const LLVector3& lo, const LLVector3& hi,
                        const InvisibleParams& params)
        {
            Piece piece;
            LLVector3 centroid;
            for (size_t i = 0; i < count; ++i)
            {
                centroid += v[i] + offset;
                update_min_max(piece.mLo, piece.mHi, v[i] + offset);
            }
            const F32 on = params.mCoplanarD;
            if (!overlaps(piece.mLo - LLVector3(on, on, on), piece.mHi + LLVector3(on, on, on), lo, hi))
            {
                return piece;
            }
            centroid /= (F32)llmax(count, (size_t)1);
            for (size_t t = 0; t + 2 < count; t += 3)
            {
                const LLVector3 a = v[t] + offset;
                LLVector3 n;
                if (!planeOf(v[t], v[t + 1], v[t + 2], n))
                {
                    continue;
                }
                F32 k = n * a;
                if (n * centroid > k)
                {
                    n = -n;
                    k = -k;
                }
                bool known = false;
                for (const auto& plane : piece.mPlanes)
                {
                    known = known || (plane.first * n >= params.mCoplanarCos && std::fabs(plane.second - k) <= params.mCoplanarD);
                }
                if (!known)
                {
                    piece.mPlanes.emplace_back(n, k);
                }
            }
            // Every plane grown outward, so a face lying on the body's face is inside it.
            for (auto& plane : piece.mPlanes)
            {
                plane.second += on;
            }
            piece.mLo -= LLVector3(on, on, on);
            piece.mHi += LLVector3(on, on, on);
            boxPlanes(piece);
            return piece;
        }

        // A triangle extruded mProxyTolerance (and mCoplanarD) either way along its normal, its sides mCoplanarD past its
        // edges so a face lying on a shared edge's plane is inside; false when degenerate.
        bool prismPiece(const LLVector3& a, const LLVector3& b, const LLVector3& c, const InvisibleParams& params, Piece& piece)
        {
            LLVector3 n;
            if (!planeOf(a, b, c, n))
            {
                return false;
            }
            const F32 k = n * a;
            const F32 tol = params.mProxyTolerance + params.mCoplanarD;
            piece.mPlanes = { { n, k + tol }, { -n, -k + tol } };
            const LLVector3* p[3] = { &a, &b, &c };
            for (S32 e = 0; e < 3; ++e)
            {
                const LLVector3& from = *p[e];
                LLVector3 s = (*p[(e + 1) % 3] - from) % n;
                const F32 slen = s.length();
                if (slen <= 1e-8f)
                {
                    return false;
                }
                s /= slen;
                if (s * (*p[(e + 2) % 3] - from) > 0.f)
                {
                    s = -s;
                }
                piece.mPlanes.emplace_back(s, s * from + params.mCoplanarD);
            }
            piece.mLo = piece.mHi = a;
            update_min_max(piece.mLo, piece.mHi, b);
            update_min_max(piece.mLo, piece.mHi, c);
            piece.mLo -= LLVector3(tol, tol, tol);
            piece.mHi += LLVector3(tol, tol, tol);
            boxPlanes(piece);
            piece.mPrism = true;
            return true;
        }

        // A colliding neighbour's convex pieces that reach the part's bounds.
        void piecesOf(const CoverCandidate& c, const LLVector3& lo, const LLVector3& hi, const InvisibleParams& params,
                      std::vector<Piece>& out)
        {
            const size_t tris = c.mCount / 3;
            size_t in_bodies = 0;
            for (size_t b = 0; b < c.mBodies && c.mConvexTris; ++b)
            {
                in_bodies += c.mConvexTris[b] > 0 ? (size_t)c.mConvexTris[b] : tris + 1;
            }
            if (c.mShape != Shape::TRIANGLES || (c.mBodies && in_bodies == tris))
            {
                // An analytic shape is one body; a soup's bodies follow one another.
                const size_t bodies = c.mShape != Shape::TRIANGLES ? 1 : c.mBodies;
                size_t at = 0;
                for (size_t b = 0; b < bodies; ++b)
                {
                    const size_t n = c.mShape != Shape::TRIANGLES ? tris : (size_t)c.mConvexTris[b];
                    Piece piece = bodyPiece(c.mVerts + at * 3, n * 3, c.mOffset, lo, hi, params);
                    at += n;
                    if (!piece.mPlanes.empty() && overlaps(piece.mLo, piece.mHi, lo, hi))
                    {
                        out.push_back(std::move(piece));
                    }
                }
                return;
            }
            // A soup that is no body has a surface only: what lies on it within the tolerance is on it.
            const F32 reach = params.mProxyTolerance + params.mCoplanarD;
            for (size_t t = 0; t < tris; ++t)
            {
                const LLVector3 a = c.mVerts[t * 3] + c.mOffset, b = c.mVerts[t * 3 + 1] + c.mOffset, d = c.mVerts[t * 3 + 2] + c.mOffset;
                LLVector3 tlo = a, thi = a;
                update_min_max(tlo, thi, b);
                update_min_max(tlo, thi, d);
                Piece piece;
                if (overlaps(tlo - LLVector3(reach, reach, reach), thi + LLVector3(reach, reach, reach), lo, hi) &&
                    prismPiece(a, b, d, params, piece))
                {
                    out.push_back(std::move(piece));
                }
            }
        }

        // Every vertex inside the box grown by the tolerance.
        bool holds(const CoverCandidate& c, const std::vector<LLVector3>& tris, F32 tolerance)
        {
            const LLVector3 centre = c.mCenter + c.mOffset;
            for (const LLVector3& v : tris)
            {
                const LLVector3 d = v - centre;
                for (S32 k = 0; k < 3; ++k)
                {
                    if (std::fabs(d * c.mAxes[k]) > c.mHalf.mV[k] + tolerance)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    const char* coverVerdictName(CoverVerdict verdict)
    {
        switch (verdict)
        {
            case CoverVerdict::PROXY: return "PROXY";
            case CoverVerdict::EXTRA: return "EXTRA";
            case CoverVerdict::BARRIER: return "BARRIER";
            case CoverVerdict::UNKNOWN_PHYSICS: return "UNKNOWN_PHYSICS";
        }
        return "?";
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
        return faces;
    }

    CoverResult coverVerdict(const std::vector<LLVector3>& tris, const std::vector<CoverCandidate>& candidates,
                             const InvisibleParams& params)
    {
        CoverResult result;
        for (const InvisibleFace& f : invisibleFaces(tris, params))
        {
            result.mPartArea += f.mArea;
        }
        if (tris.size() / 3 > params.mMaxPartTris)
        {
            result.mTooFine = true;
            return result;
        }
        if (candidates.empty())
        {
            result.mVerdict = CoverVerdict::BARRIER;
            result.mOutsideArea = result.mPartArea;
            return result;
        }
        // Never a hatch on a guess: a neighbour may yet turn out not to collide.
        for (const CoverCandidate& c : candidates)
        {
            if (!c.mPhysicsKnown)
            {
                return result;
            }
        }
        // A visible shell nothing collides with is what the eye sees, whatever collides inside it.
        for (const CoverCandidate& c : candidates)
        {
            if (!c.mCollides && holds(c, tris, params.mProxyTolerance))
            {
                result.mVerdict = CoverVerdict::PROXY;
                return result;
            }
        }

        LL_PROFILE_ZONE_NAMED("sk overview cover clip");
        LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const LLVector3& v : tris)
        {
            update_min_max(lo, hi, v);
        }
        std::vector<Piece> pieces;
        for (const CoverCandidate& c : candidates)
        {
            if (c.mCollides)
            {
                piecesOf(c, lo, hi, params, pieces);
            }
        }
        std::vector<Poly> polys;
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            if (((tris[t + 1] - tris[t]) % (tris[t + 2] - tris[t])).length() > 1e-8f)
            {
                polys.push_back({ tris[t], tris[t + 1], tris[t + 2] });
            }
        }
        // Each piece in turn: of what is left, the part outside its first plane, then outside its second but inside
        // the first, and so on, is what that piece leaves; what is inside every plane is taken away.
        std::vector<Poly> next;
        Poly rest, out, tmp;
        for (const Piece& piece : pieces)
        {
            result.mSurfaceClip |= piece.mPrism;
            next.clear();
            for (Poly& p : polys)
            {
                LLVector3 plo(1e30f, 1e30f, 1e30f), phi(-1e30f, -1e30f, -1e30f);
                for (const LLVector3& v : p)
                {
                    update_min_max(plo, phi, v);
                }
                if (!overlaps(plo, phi, piece.mLo, piece.mHi))
                {
                    next.push_back(std::move(p));
                    continue;
                }
                rest = p;
                for (const auto& plane : piece.mPlanes)
                {
                    // Wholly on the inside, or lying in the plane as a face on a shared edge does: nothing leaves.
                    F32 beyond = -1e30f;
                    for (const LLVector3& v : rest)
                    {
                        beyond = llmax(beyond, plane.first * v - plane.second);
                    }
                    if (beyond <= 0.f)
                    {
                        continue;
                    }
                    clip(rest, plane.first, plane.second, false, out);
                    if (out.size() >= 3 && polyArea(out) > 0.f)
                    {
                        next.push_back(out);
                    }
                    clip(rest, plane.first, plane.second, true, tmp);
                    rest.swap(tmp);
                    if (rest.size() < 3)
                    {
                        break;
                    }
                }
            }
            polys.swap(next);
            if (polys.size() > params.mMaxPieces)
            {
                result.mTooFine = true;
                return result;
            }
        }
        for (const Poly& p : polys)
        {
            result.mOutsideArea += polyArea(p);
        }
        if (result.mOutsideArea <= params.mMinFaceArea)
        {
            result.mVerdict = CoverVerdict::PROXY;
            return result;
        }
        result.mVerdict = CoverVerdict::EXTRA;
        for (const Poly& p : polys)
        {
            for (size_t i = 1; i + 1 < p.size(); ++i)
            {
                result.mProtruding.insert(result.mProtruding.end(), { p[0], p[i], p[i + 1] });
            }
        }
        return result;
    }

    bool coverPoolRecord(const Record& rec)
    {
        if (rec.mDynamic || (rec.mExclude & EXCLUDE_SOLID))
        {
            return false;
        }
        if (rec.mRole == Role::SURROUND || !rec.mCollides)
        {
            return true;
        }
        // As the overview's keepRecord: no volumes, nothing drawn live.
        return rec.mRole == Role::NAV && rec.mNavRole != NavRole::MATERIAL_VOLUME && rec.mNavRole != NavRole::EXCLUSION_VOLUME &&
               rec.mFeature != FeatureKind::VEHICLE && rec.mFeature != FeatureKind::ELEVATOR;
    }

    std::vector<CoverCandidate> coverCandidates(const std::vector<const Record*>& records, const LLVector3d& origin,
                                                std::vector<LLVector3>& scratch)
    {
        scratch.clear();
        std::vector<CoverCandidate> out;
        std::vector<size_t> starts;
        for (const Record* rec : records)
        {
            CoverCandidate c;
            c.mPhysicsKnown = rec->mPhysicsKnown;
            const LLVector3 shift(rec->mOrigin - origin);
            if (rec->mRole == Role::SURROUND)
            {
                // A surround collides through the physics record beside it, when it has one.
                const bool twin = std::any_of(records.begin(), records.end(), [rec](const Record* other)
                                              { return other->mPart == rec->mPart && other->mRole == Role::NAV && other->mCollides; });
                if (twin)
                {
                    continue;
                }
                c.mCollides = false;
                c.mCenter = LLVector3((rec->mMin + rec->mMax) * 0.5 - origin);
                c.mAxes[0].set(1.f, 0.f, 0.f);
                c.mAxes[1].set(0.f, 1.f, 0.f);
                c.mAxes[2].set(0.f, 0.f, 1.f);
                c.mHalf = LLVector3((rec->mMax - rec->mMin) * 0.5);
            }
            else if (!rec->mCollides)
            {
                c.mCollides = false;
                c.mCenter = rec->mCenter + shift;
                for (S32 k = 0; k < 3; ++k)
                {
                    c.mAxes[k] = rec->mAxes[k];
                }
                c.mHalf = rec->mHalf;
            }
            else
            {
                c.mShape = rec->mShape;
                if (rec->mShape == Shape::TRIANGLES && !rec->mConvexTris.empty())
                {
                    c.mConvexTris = rec->mConvexTris.data();
                    c.mBodies = rec->mConvexTris.size();
                }
            }
            starts.push_back(scratch.size());
            if (c.mCollides)
            {
                emitRecord(*rec, origin, scratch);
            }
            c.mCount = scratch.size() - starts.back();
            out.push_back(c);
        }
        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i].mVerts = scratch.data() + starts[i];
        }
        return out;
    }
}
