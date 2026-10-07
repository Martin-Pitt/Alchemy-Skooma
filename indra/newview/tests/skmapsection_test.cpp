/**
 * @file skmapsection_test.cpp
 * @brief Mini-map section cut: records sliced by horizontal planes, against the oracle's numbers.
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

#include "../test/lltut.h"

#include "../skmapsection.h"

#include "sknavbuild.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace
{
    using namespace sknav;

    const F32 LOW = 0.4375f;
    const F32 HIGH = 1.4375f;

    Record analytic(Shape shape, const LLVector3& c, const LLVector3& h, const LLVector3& a0 = LLVector3(1.f, 0.f, 0.f),
                    const LLVector3& a1 = LLVector3(0.f, 1.f, 0.f), const LLVector3& a2 = LLVector3(0.f, 0.f, 1.f))
    {
        Record r;
        r.mRole = Role::NAV;
        r.mShape = shape;
        r.mSolid = true;
        r.mCenter = c;
        r.mHalf = h;
        r.mAxes[0] = a0;
        r.mAxes[1] = a1;
        r.mAxes[2] = a2;
        const F32 ex = std::fabs(a0.mV[VX]) * h.mV[0] + std::fabs(a1.mV[VX]) * h.mV[1] + std::fabs(a2.mV[VX]) * h.mV[2];
        const F32 ey = std::fabs(a0.mV[VY]) * h.mV[0] + std::fabs(a1.mV[VY]) * h.mV[1] + std::fabs(a2.mV[VY]) * h.mV[2];
        const F32 ez = std::fabs(a0.mV[VZ]) * h.mV[0] + std::fabs(a1.mV[VZ]) * h.mV[1] + std::fabs(a2.mV[VZ]) * h.mV[2];
        r.mMin.set(c.mV[VX] - ex, c.mV[VY] - ey, c.mV[VZ] - ez);
        r.mMax.set(c.mV[VX] + ex, c.mV[VY] + ey, c.mV[VZ] + ez);
        r.mSignature = 1;
        return r;
    }

    Record box(const LLVector3& c, const LLVector3& h, const LLVector3& a0 = LLVector3(1.f, 0.f, 0.f),
               const LLVector3& a1 = LLVector3(0.f, 1.f, 0.f), const LLVector3& a2 = LLVector3(0.f, 0.f, 1.f))
    {
        return analytic(Shape::BOX, c, h, a0, a1, a2);
    }

    std::vector<LLVector3> emitted(const Record& r)
    {
        std::vector<LLVector3> out;
        emitRecord(r, LLVector3d(0.0, 0.0, 0.0), out);
        return out;
    }

    Record soup(const std::vector<LLVector3>& tris, bool solid, const std::vector<S32>& convex = {})
    {
        Record r;
        r.mRole = Role::NAV;
        r.mShape = Shape::TRIANGLES;
        r.mSolid = solid;
        r.mTriangles = std::make_shared<const std::vector<LLVector3>>(tris);
        r.mConvexTris = convex;
        LLVector3d lo(DBL_MAX, DBL_MAX, DBL_MAX), hi(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (const LLVector3& v : tris)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                lo.mdV[k] = llmin(lo.mdV[k], static_cast<F64>(v.mV[k]));
                hi.mdV[k] = llmax(hi.mdV[k], static_cast<F64>(v.mV[k]));
            }
        }
        r.mMin = lo;
        r.mMax = hi;
        r.mSignature = 2;
        return r;
    }

    std::vector<LLVector3> flipped(std::vector<LLVector3> tris)
    {
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            std::swap(tris[t], tris[t + 2]);
        }
        return tris;
    }

    /// quad_tris (a, b, c, d) as (a, b, c), (a, c, d): a vertical wall from p to q, 0 to 3 m.
    void wallQuad(std::vector<LLVector3>& out, F32 px, F32 py, F32 qx, F32 qy)
    {
        const LLVector3 a(px, py, 0.f), b(qx, qy, 0.f), c(qx, qy, 3.f), d(px, py, 3.f);
        for (const LLVector3& v : { a, b, c, a, c, d })
        {
            out.push_back(v);
        }
    }

    F32 area(const skmap::SliceResult& s, size_t i)
    {
        return skmap::loopArea(&s.mPts[s.mLoops[i].mFirst], s.mLoops[i].mCount);
    }

    /// Sum of wind x |area| over the loops.
    F32 unionArea(const skmap::SliceResult& s)
    {
        F32 a = 0.f;
        for (size_t i = 0; i < s.mLoops.size(); ++i)
        {
            a += s.mLoops[i].mWind * std::fabs(area(s, i));
        }
        return a;
    }

    std::vector<F32> sortedAreas(const skmap::SliceResult& s)
    {
        std::vector<F32> a;
        for (size_t i = 0; i < s.mLoops.size(); ++i)
        {
            a.push_back(std::fabs(area(s, i)));
        }
        std::sort(a.begin(), a.end());
        return a;
    }

    std::vector<S32> winds(const skmap::SliceResult& s)
    {
        std::vector<S32> w;
        for (const skmap::SliceLoop& l : s.mLoops)
        {
            w.push_back(l.mWind);
        }
        return w;
    }

    /// A point's coordinates in the record's unit frame.
    LLVector3 local(const Record& r, const LLVector2& p, F32 z)
    {
        const LLVector3 d = LLVector3(p.mV[VX], p.mV[VY], z) - r.mCenter;
        return LLVector3((d * r.mAxes[0]) / r.mHalf.mV[0], (d * r.mAxes[1]) / r.mHalf.mV[1], (d * r.mAxes[2]) / r.mHalf.mV[2]);
    }

    bool sameLoops(const std::vector<skmap::SliceLoop>& a, const std::vector<skmap::SliceLoop>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].mFirst != b[i].mFirst || a[i].mCount != b[i].mCount || a[i].mParent != b[i].mParent || a[i].mWind != b[i].mWind)
            {
                return false;
            }
        }
        return true;
    }

    bool identical(const skmap::SliceResult& a, const skmap::SliceResult& b)
    {
        return a.mPts.size() == b.mPts.size()
            && (a.mPts.empty() || std::memcmp(a.mPts.data(), b.mPts.data(), a.mPts.size() * sizeof(LLVector2)) == 0)
            && sameLoops(a.mLoops, b.mLoops) && sameLoops(a.mLines, b.mLines);
    }

    F32 segmentDistance(const LLVector2& p, const LLVector2& a, const LLVector2& b)
    {
        const LLVector2 ab = b - a;
        const F32 len2 = ab * ab;
        const F32 t = len2 > 0.f ? llclamp(((p - a) * ab) / len2, 0.f, 1.f) : 0.f;
        return (a + ab * t - p).length();
    }

    const LLVector3 ROT_A0(0.813797681f, 0.469846310f, -0.342020143f);
    const LLVector3 ROT_A1(-0.440969611f, 0.882564119f, 0.163175911f);
    const LLVector3 ROT_A2(0.378522306f, 0.018028311f, 0.925416578f);

    Record rotatedBox()
    {
        return box(LLVector3(0.f, 0.f, 1.f), LLVector3(1.2f, 0.5f, 0.9f), ROT_A0, ROT_A1, ROT_A2);
    }

    /// The hollow prim: an outer box and an inward-facing inner box in one soup.
    std::vector<LLVector3> hollowRing()
    {
        std::vector<LLVector3> tris = emitted(box(LLVector3(0.f, 0.f, 1.5f), LLVector3(1.5f, 1.5f, 1.5f)));
        std::vector<LLVector3> inner = emitted(box(LLVector3(0.2f, 0.f, 1.5f), LLVector3(1.0f, 1.1f, 1.6f)));
        for (size_t t = 0; t + 2 < inner.size(); t += 3)
        {
            std::swap(inner[t + 1], inner[t + 2]);
        }
        tris.insert(tris.end(), inner.begin(), inner.end());
        return tris;
    }
}

namespace tut
{
    struct skmapsection_data {};
    typedef test_group<skmapsection_data> skmapsection_group;
    typedef skmapsection_group::object skmapsection_object;
    tut::skmapsection_group g("skmapsection");

    // An axis-aligned box gives its rectangle; a plane at a face gives the half-open answer.
    template<> template<>
    void skmapsection_object::test<1>()
    {
        const Record r = box(LLVector3(0.f, 0.f, 1.5f), LLVector3(2.f, 0.1f, 1.5f));
        const skmap::SliceResult s = skmap::sliceRecord(r, LOW);
        ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
        ensure_equals("four points", s.mLoops[0].mCount, 4u);
        ensure_approximately_equals_range("area", area(s, 0), 0.8f, 1e-4f);
        for (const LLVector2& p : s.mPts)
        {
            ensure("x in range", p.mV[VX] >= -2.f - 1e-5f && p.mV[VX] <= 2.f + 1e-5f);
            ensure("y in range", p.mV[VY] >= -0.1f - 1e-5f && p.mV[VY] <= 0.1f + 1e-5f);
        }
        ensure("c = 0 is empty", skmap::sliceRecord(r, 0.f).mLoops.empty());
        ensure_approximately_equals_range("c = 1e-4", unionArea(skmap::sliceRecord(r, 1e-4f)), 0.8f, 1e-4f);
        ensure_approximately_equals_range("the top face counts as above", unionArea(skmap::sliceRecord(r, 3.f)), 0.8f, 1e-4f);
        ensure("over the top is empty", skmap::sliceRecord(r, 3.0001f).mLoops.empty());
    }

    // A box at yaw 30, pitch 20, roll 10.
    template<> template<>
    void skmapsection_object::test<2>()
    {
        const Record r = rotatedBox();
        const skmap::SliceResult lo = skmap::sliceRecord(r, LOW);
        const skmap::SliceResult hi = skmap::sliceRecord(r, HIGH);
        ensure_equals("low: four points", lo.mLoops.size() == 1 ? lo.mLoops[0].mCount : 0u, 4u);
        ensure_equals("high: five points", hi.mLoops.size() == 1 ? hi.mLoops[0].mCount : 0u, 5u);
        ensure_approximately_equals_range("low area", area(lo, 0), 2.150948f, 1e-4f);
        ensure_approximately_equals_range("high area", area(hi, 0), 2.503017f, 1e-4f);
        ensure("counter-clockwise", area(lo, 0) > 0.f && area(hi, 0) > 0.f);
    }

    // An ellipsoid rotated about every axis: an inscribed polygon on the exact ellipse.
    template<> template<>
    void skmapsection_object::test<3>()
    {
        const Record r = analytic(Shape::SPHERE, LLVector3(0.f, 0.f, 1.f), LLVector3(1.4f, 0.7f, 1.1f),
                                  LLVector3(0.784885567f, 0.453153894f, -0.422618262f), LLVector3(-0.147763145f, 0.799240839f, 0.582563416f),
                                  LLVector3(0.601764654f, -0.394798214f, 0.694272044f));
        const struct
        {
            F32 c;
            U32 n;
            F32 a;
            F32 exact;
        } cases[] = { { LOW, 24, 2.274120f, 2.30031f }, { HIGH, 25, 2.639449f, 2.66744f } };
        for (const auto& k : cases)
        {
            const skmap::SliceResult s = skmap::sliceRecord(r, k.c);
            ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
            ensure_equals("points", s.mLoops[0].mCount, k.n);
            ensure_approximately_equals_range("area", area(s, 0), k.a, 1e-4f);
            ensure("near the closed form", area(s, 0) / k.exact >= 0.988f);
            for (const LLVector2& p : s.mPts)
            {
                ensure_approximately_equals_range("on the surface", local(r, p, k.c).length(), 1.f, 1e-4f);
            }
        }
    }

    // An upright elliptic cylinder: the whole cross-section at any height inside it.
    template<> template<>
    void skmapsection_object::test<4>()
    {
        const Record r = analytic(Shape::CYLINDER, LLVector3(0.f, 0.f, 1.5f), LLVector3(0.9f, 0.45f, 1.5f),
                                  LLVector3(0.906307787f, 0.422618262f, 0.f), LLVector3(-0.422618262f, 0.906307787f, 0.f));
        for (F32 c : { LOW, HIGH, 3.f })
        {
            const skmap::SliceResult s = skmap::sliceRecord(r, c);
            ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
            ensure_equals("22 points", s.mLoops[0].mCount, 22u);
            ensure_approximately_equals_range("area", area(s, 0), 1.255119f, 1e-4f);
        }
        ensure("c = 0 is empty", skmap::sliceRecord(r, 0.f).mLoops.empty());
    }

    // A lying cylinder: the exact parallelogram of a chord swept along the axis.
    template<> template<>
    void skmapsection_object::test<5>()
    {
        const Record r = analytic(Shape::CYLINDER, LLVector3(0.f, 0.f, 0.9f), LLVector3(0.6f, 0.6f, 1.8f), LLVector3(0.f, 0.f, -1.f),
                                  LLVector3(-0.342020143f, 0.939692621f, 0.f), LLVector3(0.939692621f, 0.342020143f, 0.f));
        const struct
        {
            F32 c, a, chord;
        } cases[] = { { LOW, 2.751999f, 0.764444f }, { HIGH, 1.919766f, 0.533268f } };
        for (const auto& k : cases)
        {
            const skmap::SliceResult s = skmap::sliceRecord(r, k.c);
            ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
            ensure_equals("four points", s.mLoops[0].mCount, 4u);
            ensure_approximately_equals_range("area", area(s, 0), k.a, 1e-4f);
            std::vector<F32> sides;
            for (U32 i = 0; i < 4; ++i)
            {
                sides.push_back((s.mPts[(i + 1) % 4] - s.mPts[i]).length());
            }
            std::sort(sides.begin(), sides.end());
            ensure_approximately_equals_range("chord", sides[0], k.chord, 1e-4f);
            ensure_approximately_equals_range("length", sides[3], 3.6f, 1e-4f);
        }
    }

    // A cylinder tilted 35 degrees: an inscribed prism's section, every vertex on the true cylinder.
    template<> template<>
    void skmapsection_object::test<6>()
    {
        const Record r = analytic(Shape::CYLINDER, LLVector3(0.f, 0.f, 1.2f), LLVector3(0.5f, 0.5f, 1.4f),
                                  LLVector3(0.627506872f, 0.526540785f, -0.573576436f), LLVector3(-0.642787610f, 0.766044443f, 0.f),
                                  LLVector3(0.439385042f, 0.368687826f, 0.819152044f));
        for (F32 c : { LOW, HIGH })
        {
            const skmap::SliceResult s = skmap::sliceRecord(r, c);
            ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
            ensure_equals("18 points", s.mLoops[0].mCount, 18u);
            ensure_approximately_equals_range("area", area(s, 0), 0.939441f, 1e-4f);
            for (const LLVector2& p : s.mPts)
            {
                const LLVector3 u = local(r, p, c);
                ensure_approximately_equals_range("on the cylinder", std::sqrt(u.mV[0] * u.mV[0] + u.mV[1] * u.mV[1]), 1.f, 1e-4f);
            }
        }
    }

    // The tessellation count, and the chord bound on a sheared conjugate pair.
    template<> template<>
    void skmapsection_object::test<7>()
    {
        const std::pair<F32, S32> counts[] = { { 0.004f, 12 }, { 0.3f, 13 }, { 0.45f, 15 }, { 0.9f, 22 },
                                               { 2.f, 32 },    { 5.f, 50 },  { 10.f, 64 },  { 128.f, 64 } };
        for (const auto& c : counts)
        {
            ensure_equals("N(R)", skmap::tessellationCount(c.first), c.second);
        }
        const LLVector2 v1(2.f, 0.f), v2(1.6f, 0.6f);
        std::vector<LLVector2> poly;
        skmap::ellipsePolygon(LLVector2(0.f, 0.f), v1, v2, poly);
        ensure_equals("36 points from the principal semi-axis 2.589448", poly.size(), static_cast<size_t>(36));
        ensure("counter-clockwise", skmap::loopArea(poly.data(), static_cast<U32>(poly.size())) > 0.f);
        F32 worst = 0.f;
        for (S32 i = 0; i < 2000; ++i)
        {
            const F32 t = F_TWO_PI * i / 2000.f;
            const LLVector2 p = v1 * std::cos(t) + v2 * std::sin(t);
            F32 best = FLT_MAX;
            for (size_t k = 0; k < poly.size(); ++k)
            {
                best = llmin(best, segmentDistance(p, poly[k], poly[(k + 1) % poly.size()]));
            }
            worst = llmax(worst, best);
        }
        ensure("within the chord bound", worst <= 0.0100f);
    }

    // A box's emitted soup slices as the box does.
    template<> template<>
    void skmapsection_object::test<8>()
    {
        const Record r = soup(emitted(rotatedBox()), true);
        const skmap::SliceResult lo = skmap::sliceRecord(r, LOW);
        const skmap::SliceResult hi = skmap::sliceRecord(r, HIGH);
        ensure_equals("one low loop", lo.mLoops.size(), static_cast<size_t>(1));
        ensure_equals("one high loop", hi.mLoops.size(), static_cast<size_t>(1));
        ensure_approximately_equals_range("low area", area(lo, 0), 2.150948f, 1e-4f);
        ensure_approximately_equals_range("high area", area(hi, 0), 2.503017f, 1e-4f);
    }

    // A hollow prim is a ring: the inner loop is a hole by nesting, whatever its normals.
    template<> template<>
    void skmapsection_object::test<9>()
    {
        const Record r = soup(hollowRing(), true);
        for (F32 c : { LOW, HIGH })
        {
            const skmap::SliceResult s = skmap::sliceRecord(r, c);
            ensure_equals("two loops", s.mLoops.size(), static_cast<size_t>(2));
            std::vector<S32> w = winds(s);
            std::sort(w.begin(), w.end());
            ensure("winds +1 and -1", w == std::vector<S32>({ -1, 1 }));
            const std::vector<F32> a = sortedAreas(s);
            ensure_approximately_equals_range("hole", a[0], 4.4f, 1e-4f);
            ensure_approximately_equals_range("outer", a[1], 9.0f, 1e-4f);
            ensure_approximately_equals_range("ring", unionArea(s), 4.6f, 1e-4f);
            ensure_equals("one group", s.mGroups.size(), static_cast<size_t>(1));
            ensure_approximately_equals_range("thickness", s.mGroups[0].mThickness, 0.4510f, 1e-4f);
        }
    }

    // A decomposition is sliced body by body; chained as one soup, the nested hull would read as a hole.
    template<> template<>
    void skmapsection_object::test<10>()
    {
        std::vector<LLVector3> tris;
        for (const Record& b : { box(LLVector3(-0.7f, 0.f, 1.2f), LLVector3(0.12f, 0.12f, 1.2f)),
                                 box(LLVector3(0.7f, 0.f, 1.2f), LLVector3(0.12f, 0.12f, 1.2f)),
                                 box(LLVector3(0.45f, 0.f, 0.45f), LLVector3(0.45f, 0.06f, 0.06f), LLVector3(0.766044443f, 0.f, 0.642787610f),
                                     LLVector3(0.f, 1.f, 0.f), LLVector3(-0.642787610f, 0.f, 0.766044443f)),
                                 box(LLVector3(0.f, 0.f, 2.5f), LLVector3(0.85f, 0.12f, 0.1f)),
                                 box(LLVector3(-0.7f, 0.f, 1.0f), LLVector3(0.05f, 0.05f, 1.0f)) })
        {
            const std::vector<LLVector3> t = emitted(b);
            tris.insert(tris.end(), t.begin(), t.end());
        }
        const Record decomposed = soup(tris, true, { 12, 12, 12, 12, 12 });
        const skmap::SliceResult lo = skmap::sliceRecord(decomposed, LOW);
        ensure("low: four +1 loops", winds(lo) == std::vector<S32>({ 1, 1, 1, 1 }));
        const std::vector<F32> la = sortedAreas(lo);
        const F32 expect_lo[4] = { 0.01f, 0.022402f, 0.0576f, 0.0576f };
        for (size_t i = 0; i < 4; ++i)
        {
            ensure_approximately_equals_range("low hull", la[i], expect_lo[i], 1e-4f);
        }
        const skmap::SliceResult hi = skmap::sliceRecord(decomposed, HIGH);
        ensure("high: three +1 loops", winds(hi) == std::vector<S32>({ 1, 1, 1 }));
        const std::vector<F32> ha = sortedAreas(hi);
        const F32 expect_hi[3] = { 0.01f, 0.0576f, 0.0576f };
        for (size_t i = 0; i < 3; ++i)
        {
            ensure_approximately_equals_range("high hull", ha[i], expect_hi[i], 1e-4f);
        }
        // The dowel's hull lies inside its post, so the bodies' union is their sum less the dowel.
        ensure_approximately_equals_range("decomposed union", unionArea(lo) - la[0], 0.137602f, 1e-4f);

        const Record chained = soup(tris, true);
        const skmap::SliceResult one = skmap::sliceRecord(chained, LOW);
        std::vector<S32> w = winds(one);
        std::sort(w.begin(), w.end());
        ensure("chained: winds {+1, +1, +1, -1}", w == std::vector<S32>({ -1, 1, 1, 1 }));
        for (size_t i = 0; i < one.mLoops.size(); ++i)
        {
            if (one.mLoops[i].mWind < 0)
            {
                ensure_approximately_equals_range("the hole is the dowel", std::fabs(area(one, i)), 0.01f, 1e-4f);
            }
        }
        ensure_approximately_equals_range("chained sum", unionArea(one), 0.127602f, 1e-4f);
    }

    // 0.2 mm of jitter in an unwelded soup closes after the snap, and the collinear merge leaves the corners.
    template<> template<>
    void skmapsection_object::test<11>()
    {
        std::vector<LLVector3> tris = emitted(box(LLVector3(0.f, 0.f, 1.5f), LLVector3(1.4f, 0.3f, 1.5f), LLVector3(0.965925826f, 0.258819045f, 0.f),
                                                  LLVector3(-0.258819045f, 0.965925826f, 0.f)));
        U32 state = 12345u;
        for (LLVector3& v : tris)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                state = state * 1664525u + 1013904223u;
                v.mV[k] += (static_cast<F32>(state) / 4294967296.f * 2.f - 1.f) * 0.0002f;
            }
        }
        const skmap::SliceResult s = skmap::sliceRecord(soup(tris, true), LOW);
        ensure_equals("one loop", s.mLoops.size(), static_cast<size_t>(1));
        ensure_equals("no lines", s.mLines.size(), static_cast<size_t>(0));
        ensure_approximately_equals_range("area", area(s, 0), 1.68f, 0.001f);
        ensure_equals("four points after the merge", s.mLoops[0].mCount, 4u);
    }

    // Gaps under 2 cm close; wider ones leave a line.
    template<> template<>
    void skmapsection_object::test<12>()
    {
        const struct
        {
            F32 g;
            size_t loops, lines;
        } cases[] = { { 0.f, 1, 0 }, { 0.0005f, 1, 0 }, { 0.015f, 1, 0 }, { 0.019f, 1, 0 }, { 0.021f, 0, 1 }, { 0.03f, 0, 1 } };
        for (const auto& k : cases)
        {
            std::vector<LLVector3> tris;
            wallQuad(tris, -1.8f, -1.8f, 1.8f, -1.8f);
            wallQuad(tris, 1.8f, -1.8f, 1.8f, 1.8f);
            wallQuad(tris, 1.8f, 1.8f, -1.8f, 1.8f);
            wallQuad(tris, -1.8f, 1.8f, -1.8f, -1.8f + k.g);
            const skmap::SliceResult s = skmap::sliceRecord(soup(tris, false), LOW);
            ensure_equals("loops", s.mLoops.size(), k.loops);
            ensure_equals("lines", s.mLines.size(), k.lines);
            ensure("a sheet", s.mSheet);
            if (k.loops)
            {
                ensure_approximately_equals_range("area", unionArea(s), 12.96f, 1e-3f);
            }
        }
    }

    // Crossing loops in one soup unite, and normals are never read.
    template<> template<>
    void skmapsection_object::test<13>()
    {
        std::vector<LLVector3> tris = emitted(box(LLVector3(0.f, -1.f, 1.5f), LLVector3(1.6f, 0.15f, 1.5f)));
        const std::vector<LLVector3> other = emitted(box(LLVector3(1.45f, 0.3f, 1.5f), LLVector3(0.15f, 1.45f, 1.5f)));
        tris.insert(tris.end(), other.begin(), other.end());
        for (const std::vector<LLVector3>& t : { tris, flipped(tris) })
        {
            const skmap::SliceResult s = skmap::sliceRecord(soup(t, false), LOW);
            ensure("two +1 loops", winds(s) == std::vector<S32>({ 1, 1 }));
            const std::vector<F32> a = sortedAreas(s);
            ensure_approximately_equals_range("0.87", a[0], 0.87f, 1e-4f);
            ensure_approximately_equals_range("0.96", a[1], 0.96f, 1e-4f);
        }
    }

    // Sheet walls with a doorway are one open line.
    template<> template<>
    void skmapsection_object::test<14>()
    {
        std::vector<LLVector3> tris;
        wallQuad(tris, -1.8f, -1.8f, -0.5f, -1.8f);
        wallQuad(tris, 0.5f, -1.8f, 1.8f, -1.8f);
        wallQuad(tris, 1.8f, -1.8f, 1.8f, 1.8f);
        wallQuad(tris, 1.8f, 1.8f, -1.8f, 1.8f);
        wallQuad(tris, -1.8f, 1.8f, -1.8f, -1.8f);
        for (F32 c : { LOW, HIGH })
        {
            const skmap::SliceResult s = skmap::sliceRecord(soup(tris, false), c);
            ensure_equals("no loops", s.mLoops.size(), static_cast<size_t>(0));
            ensure_equals("one line", s.mLines.size(), static_cast<size_t>(1));
        }
    }

    // The hollow ring with every triangle flipped is the same ring.
    template<> template<>
    void skmapsection_object::test<15>()
    {
        const skmap::SliceResult s = skmap::sliceRecord(soup(flipped(hollowRing()), true), LOW);
        std::vector<S32> w = winds(s);
        std::sort(w.begin(), w.end());
        ensure("winds +1 and -1", w == std::vector<S32>({ -1, 1 }));
        ensure_approximately_equals_range("ring", unionArea(s), 4.6f, 1e-4f);
    }

    // The same input twice gives the same bytes, and z-bins change nothing.
    template<> template<>
    void skmapsection_object::test<16>()
    {
        const Record ring = soup(hollowRing(), true);
        ensure("deterministic", identical(skmap::sliceRecord(ring, LOW), skmap::sliceRecord(ring, LOW)));

        std::vector<LLVector3> tris;
        for (S32 i = 0; i < 10000; ++i)
        {
            const F32 x0 = i * 0.01f, x1 = (i + 1) * 0.01f;
            wallQuad(tris, x0, 0.5f * std::sin(x0), x1, 0.5f * std::sin(x1));
        }
        ensure_equals("20 000 triangles", tris.size(), static_cast<size_t>(60000));
        const Record ribbon = soup(tris, false);
        const skmap::ZBins bins = skmap::buildZBins(tris);
        const skmap::SliceResult plain = skmap::sliceRecord(ribbon, LOW);
        const skmap::SliceResult binned = skmap::sliceRecord(ribbon, LOW, &bins);
        ensure("something sliced", !plain.mLines.empty());
        ensure("binned is identical", identical(plain, binned));

        // Boxes stacked up a slope, so the bins differ from plane to plane.
        std::vector<LLVector3> steps;
        for (S32 i = 0; i < 100; ++i)
        {
            const std::vector<LLVector3> t = emitted(box(LLVector3(i * 0.5f, 0.f, 0.1f * i + 0.2f), LLVector3(0.2f, 0.3f, 0.2f)));
            steps.insert(steps.end(), t.begin(), t.end());
        }
        const Record stair = soup(steps, true);
        const skmap::ZBins stair_bins = skmap::buildZBins(steps);
        for (F32 c : { LOW, HIGH, 5.1875f, 9.9375f, 20.f })
        {
            ensure("binned stair is identical", identical(skmap::sliceRecord(stair, c), skmap::sliceRecord(stair, c, &stair_bins)));
        }
    }

    // A 1 m square fills the subsamples whose centres it holds, and its edges stamp the row and column they run along.
    template<> template<>
    void skmapsection_object::test<17>()
    {
        const skmap::SliceResult s = skmap::sliceRecord(box(LLVector3(0.5f, 0.5f, 1.5f), LLVector3(0.5f, 0.5f, 1.5f)), LOW);
        std::vector<U8> mask(64 * 64, 0);
        skmap::fillCoverage(s, { 0 }, { 1 }, false, LLVector2(0.f, 0.f), 0.f, 0.f, 1.f / 32.f, 64, 64, mask);
        S32 set = 0;
        for (S32 y = 0; y < 64; ++y)
        {
            for (S32 x = 0; x < 64; ++x)
            {
                if (mask[y * 64 + x])
                {
                    ++set;
                    ensure("within the square and its far edges", x < 33 && y < 33);
                }
            }
        }
        ensure_equals("1089 subsamples", set, 1089);
    }

    // A 2 cm sheet at 0.25 m subsamples: every row it crosses is covered, and the cover is connected.
    template<> template<>
    void skmapsection_object::test<18>()
    {
        const Record r = box(LLVector3(2.f, 2.f, 1.5f), LLVector3(1.5f, 0.01f, 1.5f), LLVector3(0.866025404f, 0.5f, 0.f),
                             LLVector3(-0.5f, 0.866025404f, 0.f));
        const skmap::SliceResult s = skmap::sliceRecord(r, LOW);
        std::vector<U8> mask(16 * 16, 0);
        skmap::fillCoverage(s, { 0 }, { 1 }, false, LLVector2(0.f, 0.f), 0.f, 0.f, 0.25f, 16, 16, mask);
        const F32 reach = 1.5f * 0.5f + 0.01f * 0.866025404f;
        const S32 row0 = static_cast<S32>(std::floor((2.f - reach) / 0.25f)), row1 = static_cast<S32>(std::floor((2.f + reach) / 0.25f));
        for (S32 y = row0; y <= row1; ++y)
        {
            bool any = false;
            for (S32 x = 0; x < 16; ++x)
            {
                any = any || mask[y * 16 + x];
            }
            ensure("each row it crosses", any);
        }
        // Flood from the first set subsample, 8-connected.
        std::vector<U8> seen(mask.size(), 0);
        std::vector<S32> stack;
        S32 total = 0;
        for (S32 i = 0; i < 256; ++i)
        {
            total += mask[i];
            if (mask[i] && stack.empty() && !seen[i])
            {
                stack.push_back(i);
                seen[i] = 1;
            }
        }
        S32 reached = 0;
        while (!stack.empty())
        {
            const S32 i = stack.back();
            stack.pop_back();
            ++reached;
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const S32 x = i % 16 + dx, y = i / 16 + dy;
                    if (x >= 0 && x < 16 && y >= 0 && y < 16 && mask[y * 16 + x] && !seen[y * 16 + x])
                    {
                        seen[y * 16 + x] = 1;
                        stack.push_back(y * 16 + x);
                    }
                }
            }
        }
        ensure("connected", total > 0 && reached == total);
    }

    // The hollow ring fills only its ring.
    template<> template<>
    void skmapsection_object::test<19>()
    {
        const skmap::SliceResult s = skmap::sliceRecord(soup(hollowRing(), true), LOW);
        std::vector<U16> loops;
        std::vector<S8> w;
        for (size_t i = 0; i < s.mLoops.size(); ++i)
        {
            loops.push_back(static_cast<U16>(i));
            w.push_back(s.mLoops[i].mWind);
        }
        const S32 n = 81;
        const F32 x0 = -2.025f, sub = 0.05f;
        std::vector<U8> mask(n * n, 0);
        skmap::fillCoverage(s, loops, w, true, LLVector2(0.f, 0.f), x0, x0, sub, n, n, mask);
        auto at = [&](F32 x, F32 y) { return mask[static_cast<S32>((y - x0) / sub) * n + static_cast<S32>((x - x0) / sub)]; };
        ensure("the hole is clear", !at(0.2f, 0.f));
        ensure("the ring is set", at(1.3f, 0.f));
    }

    // Levels within 0.5 m merge to the most sampled; four runs at most survive, the most sampled first.
    template<> template<>
    void skmapsection_object::test<20>()
    {
        const struct
        {
            std::vector<std::pair<S32, U32>> in;
            std::vector<S32> out;
        } cases[] = {
            { { { 0, 1 } }, { 0 } },
            { { { 0, 1 }, { 3, 2 } }, { 3 } },
            { { { 0, 2 }, { 3, 2 } }, { 0 } },
            { { { 0, 1 }, { 8, 1 }, { 16, 1 }, { 24, 1 }, { 32, 1 } }, { 0, 8, 16, 24 } },
            { { { 0, 1 }, { 8, 1 }, { 16, 1 }, { 24, 1 }, { 32, 3 } }, { 0, 8, 16, 32 } },
        };
        for (const auto& k : cases)
        {
            std::vector<std::pair<S32, U32>> in = k.in;
            std::vector<S32> out;
            skmap::mergeLevels(in, out);
            ensure("merged levels", out == k.out);
        }
    }

    // Islands: a crate and a sandbag wall pass; stair treads and a broad stage do not.
    template<> template<>
    void skmapsection_object::test<21>()
    {
        struct Rect
        {
            F32 x0, y0, x1, y1, z;
        };
        auto floors = [](const std::vector<Rect>& rects) {
            return [rects](const LLVector2& p, F32& z) {
                bool found = false;
                for (const Rect& r : rects)
                {
                    if (p.mV[VX] >= r.x0 && p.mV[VX] <= r.x1 && p.mV[VY] >= r.y0 && p.mV[VY] <= r.y1)
                    {
                        z = found ? llmax(z, r.z) : r.z;
                        found = true;
                    }
                }
                return found;
            };
        };
        auto outline = [](F32 x0, F32 y0, F32 x1, F32 y1) {
            return std::vector<LLVector2>{ LLVector2(x0, y0), LLVector2(x1, y0), LLVector2(x1, y1), LLVector2(x0, y1) };
        };
        auto thick = [](const std::vector<LLVector2>& o) {
            return 2.f * std::fabs(skmap::loopArea(o.data(), 4)) / ((o[1] - o[0]).length() * 2.f + (o[2] - o[1]).length() * 2.f);
        };
        auto island = [&](const std::vector<LLVector2>& o, const std::vector<Rect>& rects) {
            return skmap::isIsland(o.data(), 4, thick(o), floors(rects));
        };

        const std::vector<LLVector2> crate = outline(-7.f, -3.1f, -6.f, -2.1f);
        ensure("crate", island(crate, { { -8.f, -4.f, 8.f, 4.f, 0.f }, { -7.f, -3.1f, -6.f, -2.1f, 0.7f } }));

        std::vector<Rect> stair = { { -3.f, -2.f, 0.f, 2.f, 0.f }, { 1.5f, -2.f, 4.5f, 2.f, 1.25f } };
        for (S32 i = 0; i < 5; ++i)
        {
            stair.push_back({ 0.3f * i, -1.f, 0.3f * i + 0.3f, 1.f, 0.25f * (i + 1) });
        }
        for (S32 i = 0; i < 5; ++i)
        {
            ensure("no tread is an island", !island(outline(0.3f * i, -1.f, 0.3f * i + 0.3f, 1.f), stair));
        }

        ensure("sandbag wall", island(outline(-2.f, -0.3f, 2.f, 0.3f), { { -5.f, -5.f, 5.f, 5.f, 0.f }, { -2.f, -0.3f, 2.f, 0.3f, 0.625f } }));

        const std::vector<LLVector2> stage = outline(-7.85f, 0.55f, -4.65f, 3.85f);
        ensure_approximately_equals_range("stage thickness", thick(stage), 1.6246f, 1e-4f);
        ensure("a stage is too broad", !island(stage, { { -8.f, -4.f, 8.f, 4.f, 0.f }, { -7.85f, 0.55f, -4.65f, 3.85f, 1.f } }));
    }

    // The cache: a hit returns the same slice, a new signature misses, the budget keeps the latest.
    template<> template<>
    void skmapsection_object::test<22>()
    {
        Record r = rotatedBox();
        r.mSignature = 77;
        skmap::SectionCache cache;
        bool hit = true;
        const skmap::SlicePtr a = cache.get(r, 3, &hit);
        ensure("first is a miss", !hit);
        const skmap::SlicePtr b = cache.get(r, 3, &hit);
        ensure("second is a hit", hit);
        ensure("the same slice", a == b);
        ensure("as sliced directly", identical(*a, skmap::sliceRecord(r, skmap::sectionPlaneZ(3))));
        Record moved = r;
        moved.mSignature = 78;
        cache.get(moved, 3, &hit);
        ensure("a new signature misses", !hit);

        skmap::SectionCache small(a->bytes());
        small.get(r, 3);
        small.get(r, 11);
        ensure_equals("one entry kept", small.entries(), static_cast<size_t>(1));
        small.get(r, 11, &hit);
        ensure("the latest", hit);
        small.get(r, 3, &hit);
        ensure("the older one went", !hit);

        // A large soup goes through its z-bins with the same result.
        std::vector<LLVector3> tris;
        for (S32 i = 0; i < 100; ++i)
        {
            const std::vector<LLVector3> t = emitted(box(LLVector3(i * 0.5f, 0.f, 0.1f * i + 0.2f), LLVector3(0.2f, 0.3f, 0.2f)));
            tris.insert(tris.end(), t.begin(), t.end());
        }
        Record stair = soup(tris, true);
        stair.mSignature = 99;
        for (S32 q : { 3, 11, 40, 79 })
        {
            ensure("binned through the cache", identical(*cache.get(stair, q), skmap::sliceRecord(stair, skmap::sectionPlaneZ(q))));
        }
    }
}
