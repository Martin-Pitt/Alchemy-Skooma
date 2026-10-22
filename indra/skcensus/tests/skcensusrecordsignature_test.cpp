/**
 * @file skcensusrecordsignature_test.cpp
 * @brief A tile's record signature: order free, and no two equal records cancel.
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

#include "../skcensusrecord.h"

#include <catch2/catch_test_macros.hpp>

using namespace skworld;

namespace
{
    RecordPtr box(const char* part, U64 signature, F32 x)
    {
        auto r = std::make_shared<Record>();
        r->mPart = LLUUID(part);
        r->mRoot = r->mPart;
        r->mShape = Shape::BOX;
        r->mSolid = true;
        r->mCenter.set(x, 5.f, 2.f);
        r->mHalf.set(1.f, 1.f, 1.f);
        r->mAxes[0].set(1.f, 0.f, 0.f);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mMin = LLVector3d(x - 1.0, 4.0, 1.0);
        r->mMax = LLVector3d(x + 1.0, 6.0, 3.0);
        r->mSignature = signature;
        return r;
    }

    const char* A = "00000000-0000-0000-0000-00000000000a";
    const char* B = "00000000-0000-0000-0000-00000000000b";
    const char* C = "00000000-0000-0000-0000-00000000000c";
}

TEST_CASE("skcensusrecordsignature: two equal records do not cancel", "[skcensusrecordsignature]")
{
    // Two parts signed alike, as two copies of one prim side by side are.
    const RecordPtr a = box(A, 0x1234, 4.f), b = box(B, 0x1234, 8.f);
    const U64 none = recordSignature({});
    CHECK(recordSignature({ a, b }) != none);
    CHECK(recordSignature({ a, b }) != recordSignature({ a }));
    // A third equal one still moves it.
    const RecordPtr c = box(C, 0x1234, 12.f);
    CHECK(recordSignature({ a, b, c }) != recordSignature({ a }));
}

TEST_CASE("skcensusrecordsignature: order does not matter", "[skcensusrecordsignature]")
{
    const RecordPtr a = box(A, 1, 4.f), b = box(B, 2, 8.f), c = box(C, 3, 12.f);
    CHECK(recordSignature({ a, b, c }) == recordSignature({ c, a, b }));
    CHECK(recordSignature({ a, b, c }) == recordSignature({ b, c, a }));
    CHECK(recordSignature({ a, b }) != recordSignature({ a, b, c }));
}

TEST_CASE("skcensusrecordsignature: a content key does not move with the signature", "[skcensusrecordsignature]")
{
    const U64 key = recordContentKey(*box(A, 1, 4.f));
    // The same shape signed otherwise, as a part re-signed for a reason that is not its shape is.
    CHECK(recordContentKey(*box(A, 77, 4.f)) == key);
    CHECK(recordSignature({ box(A, 77, 4.f) }) != recordSignature({ box(A, 1, 4.f) }));
    // While a shape that moved moves the key.
    CHECK(recordContentKey(*box(A, 77, 4.5f)) != key);
}

TEST_CASE("skcensusrecordsignature: a box emits twelve triangles shifted to the mesh origin", "[skcensusrecordsignature]")
{
    auto r = std::make_shared<Record>(*box(A, 1, 4.f));
    r->mOrigin = LLVector3d(256.0, 512.0, 0.0);
    std::vector<LLVector3> tris;
    emitRecord(*r, LLVector3d(256.0, 480.0, 0.0), tris);
    REQUIRE(tris.size() == 36);
    for (const LLVector3& v : tris)
    {
        CHECK(v.mV[VX] >= 3.f - 1e-4f);
        CHECK(v.mV[VX] <= 5.f + 1e-4f);
        CHECK(v.mV[VY] >= 36.f - 1e-4f);
        CHECK(v.mV[VY] <= 38.f + 1e-4f);
    }
}
