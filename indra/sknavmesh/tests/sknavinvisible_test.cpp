/**
 * @file sknavinvisible_test.cpp
 * @brief An invisible part stands in for a visible neighbour nothing collides with, adds what sticks out past the
 * colliding ones, or is a barrier with nothing visible near it.
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

#include "../sknavinvisible.h"

#include "llmath.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
    using namespace sknav;

    typedef std::vector<LLVector3> Tris;

    // Turned about z by deg, as the box's axes.
    void turn(F32 deg, LLVector3 axes[3])
    {
        const F32 a = deg * DEG_TO_RAD;
        axes[0].set(std::cos(a), std::sin(a), 0.f);
        axes[1].set(-std::sin(a), std::cos(a), 0.f);
        axes[2].set(0.f, 0.f, 1.f);
    }

    // A box's twelve triangles, faces in the order -x +x -y +y -z +z, wound outward.
    Tris box(const LLVector3& c, F32 deg, const LLVector3& half)
    {
        LLVector3 ax[3];
        turn(deg, ax);
        auto corner = [&](S32 i, S32 j, S32 k)
        {
            return c + ax[0] * ((2 * i - 1) * half.mV[0]) + ax[1] * ((2 * j - 1) * half.mV[1]) +
                   ax[2] * ((2 * k - 1) * half.mV[2]);
        };
        static const S32 QUADS[6][4][3] = {
            { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } }, { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } },
            { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } }, { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
            { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } }, { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } },
        };
        Tris out;
        for (const auto& q : QUADS)
        {
            LLVector3 p[4];
            for (S32 k = 0; k < 4; ++k)
            {
                p[k] = corner(q[k][0], q[k][1], q[k][2]);
            }
            out.insert(out.end(), { p[0], p[1], p[2], p[0], p[2], p[3] });
        }
        return out;
    }

    Tris quad(const LLVector3& p0, const LLVector3& p1, const LLVector3& p2, const LLVector3& p3)
    {
        return { p0, p1, p2, p0, p2, p3 };
    }

    Tris floorQuad(F32 x0, F32 x1, F32 y0, F32 y1, F32 z)
    {
        return quad({ x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z });
    }

    // A census record of a box, colliding or visible only.
    std::shared_ptr<Record> boxRecord(const LLVector3& c, F32 deg, const LLVector3& half, bool collides = true)
    {
        auto r = std::make_shared<Record>();
        r->mPart.generate();
        r->mShape = Shape::BOX;
        r->mCollides = collides;
        r->mCenter = c;
        turn(deg, r->mAxes);
        r->mHalf = half;
        return r;
    }

    // A colliding triangle soup: a physics mesh, or convex bodies when their triangle counts are given.
    std::shared_ptr<Record> soupRecord(const Tris& tris, const std::vector<S32>& bodies = {})
    {
        auto r = std::make_shared<Record>();
        r->mPart.generate();
        r->mShape = Shape::TRIANGLES;
        r->mTriangles = std::make_shared<const Tris>(tris);
        r->mConvexTris = bodies;
        return r;
    }

    CoverResult judge(const Tris& part, const std::vector<std::shared_ptr<Record>>& neighbours)
    {
        std::vector<const Record*> records;
        for (const auto& r : neighbours)
        {
            records.push_back(r.get());
        }
        std::vector<LLVector3> scratch;
        return coverVerdict(part, coverCandidates(records, LLVector3d(), scratch), InvisibleParams());
    }

    F32 area(const Tris& tris)
    {
        F32 sum = 0.f;
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            sum += 0.5f * ((tris[t + 1] - tris[t]) % (tris[t + 2] - tris[t])).length();
        }
        return sum;
    }

    TEST_CASE("sknavinvisible: nothing nearby is a barrier", "[sknavinvisible]")
    {
        const CoverResult r = judge(box({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f }), {});
        CHECK(r.mVerdict == CoverVerdict::BARRIER);
        CHECK(std::fabs(r.mPartArea - 24.f) < 1e-3f);
        CHECK(r.mOutsideArea == r.mPartArea);
        CHECK(r.mProtruding.empty()); // the worker hatches a barrier whole
    }

    TEST_CASE("sknavinvisible: a box inside a phantom shell within tolerance is a proxy", "[sknavinvisible]")
    {
        // Turned and 0.1 m past the shell's faces: inside its box grown by 0.15 m, vertex for vertex.
        const CoverResult r = judge(box({ 0.05f, 0.f, 1.f }, 20.f, { 1.f, 0.5f, 1.05f }),
                                    { boxRecord({ 0.f, 0.f, 1.f }, 20.f, { 1.f, 0.5f, 1.f }, false) });
        CHECK(r.mVerdict == CoverVerdict::PROXY);
        CHECK(r.mProtruding.empty());
    }

    TEST_CASE("sknavinvisible: a box a hair outside a phantom shell's tolerance is not its proxy", "[sknavinvisible]")
    {
        // 0.16 m past the shell on +x, over the 0.15 m it allows; nothing colliding takes any of it away.
        const CoverResult r = judge(box({ 0.08f, 0.f, 1.f }, 0.f, { 1.08f, 0.5f, 1.f }),
                                    { boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 0.5f, 1.f }, false) });
        CHECK(r.mVerdict == CoverVerdict::EXTRA);
        CHECK(std::fabs(r.mOutsideArea - r.mPartArea) < 1e-3f);
    }

    TEST_CASE("sknavinvisible: a box poking past a colliding box is extra and only the overhang is hatched", "[sknavinvisible]")
    {
        // The part spans x 1..3 and the collider x 0..2: its +x face and half of each side stick out, 12 of 24 m2. The
        // side faces flush with the collider's are on it, not out of it.
        const CoverResult r = judge(box({ 2.f, 1.f, 1.f }, 0.f, { 1.f, 1.f, 1.f }), { boxRecord({ 1.f, 1.f, 1.f }, 0.f, { 1.f, 1.f, 1.f }) });
        REQUIRE(r.mVerdict == CoverVerdict::EXTRA);
        // A solid takes away what lies within mCoplanarD of it: a band that wide along the four sides it crosses.
        const F32 on = InvisibleParams().mCoplanarD;
        CHECK(std::fabs(r.mOutsideArea - (12.f - 8.f * on)) < 1e-3f);
        CHECK(std::fabs(area(r.mProtruding) - r.mOutsideArea) < 1e-3f);
        for (const LLVector3& v : r.mProtruding)
        {
            CHECK(v.mV[VX] >= 2.f + on - 1e-4f);
        }
        CHECK_FALSE(r.mSurfaceClip);
    }

    TEST_CASE("sknavinvisible: a thin wall on a colliding floor is extra over its whole height", "[sknavinvisible]")
    {
        // A 4 m by 0.1 m by 3 m wall on a slab whose top is z 0: all but its foot sticks out.
        const CoverResult r = judge(box({ 0.f, 0.f, 1.5f }, 0.f, { 2.f, 0.05f, 1.5f }),
                                    { boxRecord({ 0.f, 0.f, -0.25f }, 0.f, { 5.f, 5.f, 0.25f }) });
        REQUIRE(r.mVerdict == CoverVerdict::EXTRA);
        // Its foot is on the slab, and a band mCoplanarD high round its sides with it.
        const F32 on = InvisibleParams().mCoplanarD;
        CHECK(std::fabs(r.mOutsideArea - (r.mPartArea - 0.4f - 8.2f * on)) < 1e-3f);
        F32 lo = 1e30f, hi = -1e30f;
        for (const LLVector3& v : r.mProtruding)
        {
            lo = llmin(lo, v.mV[VZ]);
            hi = llmax(hi, v.mV[VZ]);
        }
        CHECK(std::fabs(lo - on) < 1e-3f);
        CHECK(std::fabs(hi - 3.f) < 1e-3f);
    }

    TEST_CASE("sknavinvisible: a part lying on a physics mesh within tolerance is a proxy through the prisms", "[sknavinvisible]")
    {
        // A 0.1 m slab on a two-triangle mesh floor: every face within 0.15 m of the floor's surface, over it.
        const CoverResult r = judge(box({ 0.3f, -0.2f, 0.05f }, 30.f, { 1.f, 1.f, 0.05f }), { soupRecord(floorQuad(-5.f, 5.f, -5.f, 5.f, 0.f)) });
        CHECK(r.mVerdict == CoverVerdict::PROXY);
        CHECK(r.mOutsideArea <= InvisibleParams().mMinFaceArea);
        CHECK(r.mSurfaceClip);

        // Raised 0.3 m off it, the slab sticks out but for nothing.
        const CoverResult raised = judge(box({ 0.3f, -0.2f, 0.35f }, 30.f, { 1.f, 1.f, 0.05f }), { soupRecord(floorQuad(-5.f, 5.f, -5.f, 5.f, 0.f)) });
        CHECK(raised.mVerdict == CoverVerdict::EXTRA);
        CHECK(std::fabs(raised.mOutsideArea - raised.mPartArea) < 1e-3f);
    }

    TEST_CASE("sknavinvisible: an unknown physics type is not judged", "[sknavinvisible]")
    {
        auto core = boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 0.5f, 0.5f, 0.5f });
        core->mPhysicsKnown = false;
        const CoverResult r = judge(box({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f }), { core });
        CHECK(r.mVerdict == CoverVerdict::UNKNOWN_PHYSICS);
        CHECK_FALSE(r.mTooFine);
        CHECK(r.mProtruding.empty());
    }

    TEST_CASE("sknavinvisible: a phantom shell beats a colliding core", "[sknavinvisible]")
    {
        // The core alone leaves most of the part sticking out; the shell round both is what the eye sees.
        const Tris part = box({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f });
        auto core = boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 0.5f, 0.5f, 0.5f });
        CHECK(judge(part, { core }).mVerdict == CoverVerdict::EXTRA);
        CHECK(judge(part, { core, boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 1.05f, 1.05f, 1.05f }, false) }).mVerdict == CoverVerdict::PROXY);
    }

    TEST_CASE("sknavinvisible: a part too fine to judge is unknown and flagged", "[sknavinvisible]")
    {
        const Tris one = box({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f });
        Tris many;
        while (many.size() / 3 <= InvisibleParams().mMaxPartTris)
        {
            many.insert(many.end(), one.begin(), one.end());
        }
        const CoverResult r = judge(many, { boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f }) });
        CHECK(r.mVerdict == CoverVerdict::UNKNOWN_PHYSICS);
        CHECK(r.mTooFine);
        // Nothing near it is still a barrier only when it is not too fine to judge at all.
        CHECK(judge(one, {}).mVerdict == CoverVerdict::BARRIER);
    }

    TEST_CASE("sknavinvisible: clipped pieces sum to the part area minus the inside", "[sknavinvisible]")
    {
        // Turned 30 degrees together, the part x 0..2 against a collider x 1..5 and wider: the half x 1..2 is inside.
        LLVector3 ax[3];
        turn(30.f, ax);
        const Tris part = box(ax[0] * 1.f + ax[1] * 1.f + LLVector3(0.f, 0.f, 1.f), 30.f, { 1.f, 1.f, 1.f });
        auto collider = boxRecord(ax[0] * 3.f + ax[1] * 1.f + LLVector3(0.f, 0.f, 1.f), 30.f, { 2.f, 2.f, 2.f });
        const CoverResult r = judge(part, { collider });
        REQUIRE(r.mVerdict == CoverVerdict::EXTRA);
        CHECK(std::fabs(r.mPartArea - area(part)) < 1e-3f);
        CHECK(std::fabs(area(r.mProtruding) - r.mOutsideArea) < 1e-3f);
        CHECK(std::fabs(r.mOutsideArea - (r.mPartArea - 12.f - 8.f * InvisibleParams().mCoplanarD)) < 2e-3f);

        // The same collider split into two convex bodies takes away the same.
        Tris bodies = box(ax[0] * 2.f + ax[1] * 1.f + LLVector3(0.f, 0.f, 1.f), 30.f, { 1.f, 2.f, 2.f });
        const Tris beyond = box(ax[0] * 4.f + ax[1] * 1.f + LLVector3(0.f, 0.f, 1.f), 30.f, { 1.f, 2.f, 2.f });
        bodies.insert(bodies.end(), beyond.begin(), beyond.end());
        const CoverResult split = judge(part, { soupRecord(bodies, { 12, 12 }) });
        CHECK(std::fabs(split.mOutsideArea - r.mOutsideArea) < 2e-3f);
        CHECK_FALSE(split.mSurfaceClip);
    }

    TEST_CASE("sknavinvisible: a surround with a colliding twin collides through it and without one is a shell", "[sknavinvisible]")
    {
        const Tris part = box({ 0.f, 0.f, 1.f }, 0.f, { 1.f, 1.f, 1.f });
        auto surround = std::make_shared<Record>();
        surround->mPart.generate();
        surround->mRole = Role::SURROUND;
        surround->mShape = Shape::TRIANGLES;
        surround->mMin.set(-1.1, -1.1, -0.1);
        surround->mMax.set(1.1, 1.1, 2.1);
        CHECK(judge(part, { surround }).mVerdict == CoverVerdict::PROXY);
        auto twin = boxRecord({ 0.f, 0.f, 1.f }, 0.f, { 0.5f, 0.5f, 0.5f });
        twin->mPart = surround->mPart;
        CHECK(judge(part, { surround, twin }).mVerdict == CoverVerdict::EXTRA);
    }

    TEST_CASE("sknavinvisible: a box's triangles group into its six faces", "[sknavinvisible]")
    {
        const std::vector<InvisibleFace> faces = invisibleFaces(box({ 0.f, 0.f, 1.f }, 30.f, { 1.f, 2.f, 3.f }), InvisibleParams());
        REQUIRE(faces.size() == 6u);
        F32 total = 0.f;
        for (const InvisibleFace& f : faces)
        {
            CHECK(f.mTris.size() == 2u);
            total += f.mArea;
        }
        CHECK(std::fabs(total - 88.f) < 1e-3f);
    }
}
