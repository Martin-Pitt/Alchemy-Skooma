/**
 * @file skroomsprobecache_test.cpp
 * @brief Tests for the room probe parts cache: keys, reuse, re-placement, bounds and the disk form.
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

#include <catch2/catch_test_macros.hpp>


#include "../skroomsprobecache.h"
#include "skcensusrecord.h"

#include <filesystem>
#include <map>

namespace
{
    using namespace skworld;

    U64 cell(S32 x, S32 y)
    {
        const S64 bias = 1ll << 20;
        return (static_cast<U64>(x + bias) << 42) | (static_cast<U64>(y + bias) << 21) | static_cast<U64>(bias);
    }

    /// An indoor space of the 0.5 m raster, x0..x1 by y0..y1 metres.
    Space room(U32 id, F32 x0, F32 y0, F32 x1, F32 y1)
    {
        Space s;
        s.mId = id;
        s.mBucket = EnclosureBucket::INDOORS;
        s.mCeilingMean = 3.f;
        s.mAnchor.set(std::floor((x0 + x1) * 0.5f), std::floor((y0 + y1) * 0.5f), 0.f);
        for (S32 y = static_cast<S32>(y0 * 2.f); y < static_cast<S32>(y1 * 2.f); ++y)
        {
            for (S32 x = static_cast<S32>(x0 * 2.f); x < static_cast<S32>(x1 * 2.f); ++x)
            {
                s.mRaster.emplace_back(cell(x, y), 1);
            }
        }
        return s;
    }

    bool sameParts(const std::vector<ProbePart>& a, const std::vector<ProbePart>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            const ProbePart& x = a[i];
            const ProbePart& y = b[i];
            if (x.mPart != y.mPart || x.mOrigin != y.mOrigin || x.mRadius != y.mRadius || x.mSpaceRadius != y.mSpaceRadius ||
                x.mGrowth != y.mGrowth || x.mArea != y.mArea || x.mMin != y.mMin || x.mMax != y.mMax)
            {
                return false;
            }
        }
        return true;
    }
}

namespace
{

    TEST_CASE("skroomsprobecache: keys follow the tiles and the parameters, not their order", "[skroomsprobecache]")
    {
        const ProbeParams params;
        const U64 k = probePartsKey({ 11, 22, 33 }, params);
        { INFO("nonzero"); REQUIRE((k != 0)); }
        { INFO("order free"); REQUIRE((probePartsKey({ 33, 11, 22 }, params)) == (k)); }
        { INFO("a tile changed"); REQUIRE((probePartsKey({ 11, 22, 34 }, params) != k)); }
        { INFO("an unpublished tile is not cached"); REQUIRE((probePartsKey({ 11, 0 }, params) == 0)); }
        { INFO("no tiles is not cached"); REQUIRE((probePartsKey({}, params) == 0)); }
        ProbeParams other;
        other.mSpacingM = 12.f;
        { INFO("a parameter changed"); REQUIRE((probePartsKey({ 11, 22, 33 }, other) != k)); }
    }

    TEST_CASE("skroomsprobecache: same keys give the same parts through a re-cut; a changed tile re-places", "[skroomsprobecache]")
    {
        const ProbeParams params;
        ProbePartsCache cache;
        const U64 key = probePartsKey({ 1, 2 }, params);
        bool placed = false;
        const std::vector<ProbePart> first = cache.partsFor(key, room(7, 0.f, 0.f, 12.f, 6.f), params, &placed);
        { INFO("placed first"); REQUIRE((placed && !first.empty())); }

        // The rooms pass re-cut it a cell narrower under a new id: the tiles did not change.
        const std::vector<ProbePart> again = cache.partsFor(key, room(9, 0.f, 0.f, 11.5f, 6.f), params, &placed);
        { INFO("reused"); REQUIRE((!placed)); }
        { INFO("exactly"); REQUIRE((sameParts(first, again))); }

        // A second space over the same tiles keeps parts of its own.
        const std::vector<ProbePart> other = cache.partsFor(key, room(8, 20.f, 0.f, 26.f, 6.f), params, &placed);
        { INFO("a neighbour places"); REQUIRE((placed && !sameParts(first, other))); }
        { INFO("and the first is still held"); REQUIRE((sameParts(cache.partsFor(key, room(7, 0.f, 0.f, 12.f, 6.f), params, &placed), first) && !placed)); }

        // One of its tiles changed: placed anew from the space as it is now.
        const U64 changed = probePartsKey({ 1, 3 }, params);
        const Space narrower = room(7, 0.f, 0.f, 11.5f, 6.f);
        const std::vector<ProbePart> moved = cache.partsFor(changed, narrower, params, &placed);
        { INFO("re-placed"); REQUIRE((placed)); }
        { INFO("as placement gives"); REQUIRE((sameParts(moved, spaceProbes(narrower, params)))); }
    }

    TEST_CASE("skroomsprobecache: memory is bounded least recently seen first", "[skroomsprobecache]")
    {
        const ProbeParams params;
        const Space s = room(1, 0.f, 0.f, 6.f, 6.f);
        const size_t per = spaceProbes(s, params).size();
        ProbePartsCache cache(per * 2);
        cache.partsFor(10, s, params);
        cache.partsFor(20, s, params);
        cache.find(10, s.mId, s.mAnchor);   // 10 is now newer than 20
        cache.partsFor(30, s, params);
        { INFO("20 let go"); REQUIRE((!cache.hasKey(20))); }
        { INFO("10 and 30 kept"); REQUIRE((cache.hasKey(10) && cache.hasKey(30))); }
        { INFO("within the bound"); REQUIRE((cache.parts() <= per * 2)); }
    }

    TEST_CASE("skroomsprobecache: parts round trip through their packed form", "[skroomsprobecache]")
    {
        const ProbeParams params;
        ProbePartsCache cache;
        const U64 key = probePartsKey({ 5, 6 }, params);
        cache.partsFor(key, room(3, 0.f, 0.f, 12.f, 4.f), params);
        cache.partsFor(key, room(4, 0.f, 8.f, 4.f, 16.f), params);
        const std::vector<CachedSpaceParts> held = *cache.spaces(key);
        const std::vector<U8> bytes = packProbeParts(held, key);

        std::vector<CachedSpaceParts> back;
        { INFO("wrong key refused"); REQUIRE((!unpackProbeParts(bytes.data(), bytes.size(), key + 1, back))); }
        std::vector<U8> bad = bytes;
        bad[bad.size() / 2] ^= 0x40;
        { INFO("damage refused"); REQUIRE((!unpackProbeParts(bad.data(), bad.size(), key, back))); }
        { INFO("short refused"); REQUIRE((!unpackProbeParts(bytes.data(), bytes.size() - 3, key, back))); }
        { INFO("untouched on refusal"); REQUIRE((back.empty())); }

        { INFO("unpacked"); REQUIRE((unpackProbeParts(bytes.data(), bytes.size(), key, back))); }
        { INFO("spaces"); REQUIRE((back.size()) == (held.size())); }
        for (size_t i = 0; i < held.size(); ++i)
        {
            { INFO("same space"); REQUIRE((back[i].mSpace == held[i].mSpace && back[i].mAnchor == held[i].mAnchor)); }
            { INFO("same parts"); REQUIRE((sameParts(back[i].mParts, held[i].mParts))); }
        }

        // Adopted into a fresh cache, a space comes back exactly where it was.
        ProbePartsCache fresh;
        fresh.adopt(key, back);
        bool placed = true;
        const std::vector<ProbePart> parts = fresh.partsFor(key, room(3, 0.f, 0.f, 12.f, 4.f), params, &placed);
        { INFO("adopted"); REQUIRE((!placed && sameParts(parts, held[0].mParts))); }
    }
}
