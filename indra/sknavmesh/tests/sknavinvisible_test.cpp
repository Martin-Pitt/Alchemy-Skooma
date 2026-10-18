/**
 * @file sknavinvisible_test.cpp
 * @brief Invisible parts on visible geometry stand in for it; those with nothing on them are barriers.
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
#include <cstdio>
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

    // A box's twelve triangles, faces in the order -x +x -y +y -z +z.
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
            { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 } }, { { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
            { { 0, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } },
            { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } }, { { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 1 } },
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

    Tris quad(const LLVector3& p0, const LLVector3& p1, const LLVector3& p2, const LLVector3& p3, bool both = false)
    {
        Tris out{ p0, p1, p2, p0, p2, p3 };
        if (both)
        {
            out.insert(out.end(), { p0, p2, p1, p0, p3, p2 });
        }
        return out;
    }

    Tris wall(F32 x0, F32 x1, F32 y, F32 z0, F32 z1)
    {
        return quad({ x0, y, z0 }, { x1, y, z0 }, { x1, y, z1 }, { x0, y, z1 });
    }

    Tris floorQuad(F32 x0, F32 x1, F32 y0, F32 y1, F32 z, bool both = false)
    {
        return quad({ x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z }, both);
    }

    InvisibleResult judge(const Tris& part, const Tris& mesh)
    {
        std::vector<InvisibleSpan> spans;
        if (!mesh.empty())
        {
            spans.push_back({ mesh.data(), mesh.size() });
        }
        return invisibleVerdict(part, spans, InvisibleParams());
    }

    // The oracle's shares (V:\Scratch\combat-minimap-invisible\oracle), exact; the mask is 32 cells a side.
    constexpr F32 MASK_TOL = 1.5f / 32.f;

    void expectShares(const InvisibleResult& r, const std::vector<F32>& oracle)
    {
        REQUIRE(r.mShares.size() == oracle.size());
        for (size_t i = 0; i < oracle.size(); ++i)
        {
            INFO("face " << i << ": " << r.mShares[i] << " oracle " << oracle[i]);
            CHECK(std::fabs(r.mShares[i] - oracle[i]) <= MASK_TOL);
        }
    }
}

TEST_CASE("sknavinvisible: a box against a mesh wall, a little off and turned, stands in for it", "[sknavinvisible]")
{
    const InvisibleResult r = judge(box({ 5.f, 0.12f, 1.5f }, 3.f, { 5.f, 0.1f, 1.5f }), wall(0.f, 10.f, 0.f, 0.f, 3.f));
    CHECK(r.mVerdict == InvisibleVerdict::STAND_IN);
    expectShares(r, { 0.f, 0.f, 0.999f, 0.652f, 0.f, 0.f });
}

TEST_CASE("sknavinvisible: a box with nothing on it is a barrier", "[sknavinvisible]")
{
    const InvisibleResult r = judge(box({ 3.f, 0.f, 1.5f }, 0.f, { 3.f, 0.15f, 1.5f }), {});
    CHECK(r.mVerdict == InvisibleVerdict::BARRIER);
    expectShares(r, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
}

TEST_CASE("sknavinvisible: a box poking past its mesh is a barrier under half covered, a stand-in over", "[sknavinvisible]")
{
    const Tris part = box({ 5.f, 0.f, 1.5f }, 0.f, { 5.f, 0.1f, 1.5f });
    const InvisibleResult forty = judge(part, wall(0.f, 4.f, 0.f, 0.f, 3.f));
    CHECK(forty.mVerdict == InvisibleVerdict::BARRIER);
    expectShares(forty, { 0.f, 0.f, 0.4f, 0.4f, 0.f, 0.f });
    const InvisibleResult seventy = judge(part, wall(0.f, 7.f, 0.f, 0.f, 3.f));
    CHECK(seventy.mVerdict == InvisibleVerdict::STAND_IN);
    expectShares(seventy, { 0.f, 0.f, 0.7f, 0.7f, 0.f, 0.f });
}

TEST_CASE("sknavinvisible: a double-sided floor covers a slab once", "[sknavinvisible]")
{
    const InvisibleResult r = judge(box({ 10.f, 10.f, -0.25f }, 0.f, { 10.f, 10.f, 0.25f }), floorQuad(0.f, 20.f, 0.f, 20.f, 0.f, true));
    CHECK(r.mVerdict == InvisibleVerdict::STAND_IN);
    expectShares(r, { 0.f, 0.f, 0.f, 0.f, 0.f, 1.f });
    CHECK(r.mCoveredAll <= 1.f);
}

TEST_CASE("sknavinvisible: a thin wall standing on a mesh floor is a barrier though its foot is covered", "[sknavinvisible]")
{
    const InvisibleResult r = judge(box({ 5.f, 5.f, 1.5f }, 0.f, { 4.f, 0.1f, 1.5f }), floorQuad(0.f, 10.f, 0.f, 10.f, 0.f));
    CHECK(r.mVerdict == InvisibleVerdict::BARRIER);
    expectShares(r, { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f });
}

TEST_CASE("sknavinvisible: a deck slab under its mesh floor stands in, its open edges notwithstanding", "[sknavinvisible]")
{
    const InvisibleResult r = judge(box({ 10.f, 10.f, -0.4f }, 0.f, { 10.f, 10.f, 0.2f }), floorQuad(0.f, 20.f, 0.f, 20.f, -0.05f));
    CHECK(r.mVerdict == InvisibleVerdict::STAND_IN);
    expectShares(r, { 0.f, 0.f, 0.f, 0.f, 0.f, 1.f });
}

TEST_CASE("sknavinvisible: mesh turned past the normal limit or beyond the slab covers nothing", "[sknavinvisible]")
{
    const Tris mesh = wall(0.f, 10.f, 0.f, 0.f, 3.f);
    const InvisibleResult turned = judge(box({ 5.f, 0.f, 1.5f }, 40.f, { 5.f, 0.1f, 1.5f }), mesh);
    CHECK(turned.mVerdict == InvisibleVerdict::BARRIER);
    expectShares(turned, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
    const InvisibleResult beyond = judge(box({ 5.f, 0.6f, 1.5f }, 0.f, { 5.f, 0.1f, 1.5f }), mesh);
    CHECK(beyond.mVerdict == InvisibleVerdict::BARRIER);
    expectShares(beyond, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
}

TEST_CASE("sknavinvisible: parts too fine to judge are left unjudged", "[sknavinvisible]")
{
    InvisibleParams params;
    params.mMaxPartTris = 4;
    const Tris part = box({ 0.f, 0.f, 0.f }, 0.f, { 1.f, 1.f, 1.f });
    CHECK(invisibleVerdict(part, {}, params).mVerdict == InvisibleVerdict::UNJUDGED);
    const Tris speck = box({ 0.f, 0.f, 0.f }, 0.f, { 0.05f, 0.05f, 0.05f });
    CHECK(invisibleVerdict(speck, {}, InvisibleParams()).mVerdict == InvisibleVerdict::UNJUDGED);
}

// The oracle's sweep, printed for diffing against oracle_sweep.csv: sknavmesh_tests "[.sweep]".
TEST_CASE("sknavinvisible: offset wall sweep", "[.sweep]")
{
    const F32 degs[] = { 0.f, 5.f, 10.f, 15.f, 20.f, 24.f, 26.f, 30.f, 40.f };
    std::printf("offset,deg,face,share\n");
    for (const F32 deg : degs)
    {
        for (S32 o = 0; o <= 16; ++o)
        {
            const F32 offset = o * 0.05f;
            const InvisibleResult r = judge(box({ 5.f, offset, 1.5f }, deg, { 5.f, 0.1f, 1.5f }), wall(0.f, 10.f, 0.f, 0.f, 3.f));
            for (size_t f = 0; f < r.mShares.size(); ++f)
            {
                std::printf("%.2f,%g,%u,%.4f\n", offset, deg, (U32)f, r.mShares[f]);
            }
        }
    }
}
