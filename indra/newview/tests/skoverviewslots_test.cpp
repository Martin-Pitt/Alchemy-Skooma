/**
 * @file skoverviewslots_test.cpp
 * @brief The census overview's page runs, its stable linkset ids and the CPU command builder.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "../skoverviewslots.h"

#include "v3math.h"

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <map>
#include <algorithm>
#include <array>
#include <random>
#include <vector>

namespace
{
    using skoverview::SlotAllocator;

    // Planes that hold everything: each 0*x + 0*y + 0*z - 1 <= 0.
    void openPlanes(F32 planes[6][4])
    {
        for (S32 p = 0; p < 6; ++p)
        {
            planes[p][0] = planes[p][1] = planes[p][2] = 0.f;
            planes[p][3] = -1.f;
        }
    }

    skoverview::SlotCull slotAt(U32 first, U32 count, F32 min_z, F32 max_z, bool ground)
    {
        skoverview::SlotCull slot;
        slot.mFirst = first;
        slot.mCount = count;
        slot.mGround = ground ? 1 : 0;
        slot.mLo[2] = min_z;
        slot.mHi[0] = slot.mHi[1] = 1.f;
        slot.mHi[2] = max_z;
        slot.mLo[3] = min_z;
        slot.mHi[3] = max_z;
        return slot;
    }

    TEST_CASE("skoverviewslots: a freed run is reused by the next fit", "[skoverviewslots]")
    {
        SlotAllocator page(300);
        const U32 a = page.allocate(30);
        const U32 b = page.allocate(60);
        const U32 c = page.allocate(90);
        CHECK(a == 0);
        CHECK(b == 30);
        CHECK(c == 90);
        page.release(b, 60);
        CHECK(page.used() == 120);
        // First fit: the hole the freed run left, before the tail.
        CHECK(page.allocate(36) == 30);
        CHECK(page.allocate(24) == 66);
        CHECK(page.allocate(15) == 180);
        CHECK(page.used() == 195);
    }

    TEST_CASE("skoverviewslots: coalesces neighbouring free runs", "[skoverviewslots]")
    {
        SlotAllocator page(90);
        const U32 a = page.allocate(30);
        const U32 b = page.allocate(30);
        const U32 c = page.allocate(30);
        CHECK(page.freeRuns().empty());
        page.release(a, 30);
        page.release(c, 30);
        CHECK(page.freeRuns().size() == 2);
        // The middle run joins both its neighbours into one run of the whole page.
        page.release(b, 30);
        REQUIRE(page.freeRuns().size() == 1);
        CHECK(page.freeRuns().begin()->first == 0);
        CHECK(page.freeRuns().begin()->second == 90);
        CHECK(page.empty());
        CHECK(page.allocate(90) == 0);
    }

    TEST_CASE("skoverviewslots: a run larger than a page gets a page of its own", "[skoverviewslots]")
    {
        // The census sizes a new page to the larger of the setting and the run; a page holds exactly what it was made for.
        SlotAllocator page(3);
        CHECK(page.allocate(6) == SlotAllocator::NONE);
        SlotAllocator own(6);
        CHECK(own.allocate(6) == 0);
        CHECK(own.allocate(3) == SlotAllocator::NONE);
        CHECK(own.used() == own.capacity());
    }

    TEST_CASE("skoverviewslots: a run over the limit is refused", "[skoverviewslots]")
    {
        SlotAllocator page(skoverview::MAX_PAGE_VERTICES);
        CHECK(page.allocate(skoverview::MAX_PAGE_VERTICES + 3) == SlotAllocator::NONE);
        CHECK(page.allocate(0) == SlotAllocator::NONE);
        CHECK(page.used() == 0);
        CHECK(page.allocate(skoverview::MAX_PAGE_VERTICES) == 0);
    }

    TEST_CASE("skoverviewslots: a random alloc free sequence never overlaps", "[skoverviewslots]")
    {
        constexpr U32 CAPACITY = 3000;
        SlotAllocator page(CAPACITY);
        std::mt19937 rng(20261021u);
        std::map<U32, U32> live;
        for (S32 step = 0; step < 20000; ++step)
        {
            const bool grow = live.empty() || (rng() % 3) != 0;
            if (grow)
            {
                const U32 count = 3 * (1 + rng() % 40);
                const U32 first = page.allocate(count);
                if (first == SlotAllocator::NONE)
                {
                    continue;
                }
                CHECK(first + count <= CAPACITY);
                live.emplace(first, count);
            }
            else
            {
                auto it = live.begin();
                std::advance(it, rng() % live.size());
                page.release(it->first, it->second);
                live.erase(it);
            }
        }
        // Live runs and free runs tile the page, none overlapping, no two free runs touching.
        std::map<U32, std::pair<U32, bool>> all;
        U32 used = 0;
        for (const auto& [first, count] : live)
        {
            all.emplace(first, std::make_pair(count, true));
            used += count;
        }
        for (const auto& [first, count] : page.freeRuns())
        {
            CHECK(all.emplace(first, std::make_pair(count, false)).second);
        }
        CHECK(page.used() == used);
        U32 at = 0;
        bool last_free = false;
        for (const auto& [first, run] : all)
        {
            CHECK(first == at);
            CHECK_FALSE((last_free && !run.second));
            at = first + run.first;
            last_free = !run.second;
        }
        CHECK(at == CAPACITY);
    }

    TEST_CASE("skoverviewslots: the level keeps the ground groups and the band", "[skoverviewslots]")
    {
        F32 planes[6][4];
        openPlanes(planes);
        const std::vector<skoverview::SlotCull> slots = {
            slotAt(0, 3, 20.f, 22.f, true),     // on the ground, far below the band
            slotAt(3, 6, 40.f, 50.f, false),    // in the band
            slotAt(9, 3, 80.f, 90.f, false),    // above it
            slotAt(12, 3, 52.f, 60.f, false),   // touches its top
        };
        std::vector<U32> first, count;

        skoverview::LevelCull everything;
        skoverview::buildCommands(slots, everything, planes, first, count);
        CHECK(count == std::vector<U32>({ 3, 6, 3, 3 }));

        skoverview::LevelCull level;
        level.mAll = false;
        level.mGround = true;
        level.mLo = 45.f;
        level.mHi = 52.f;
        skoverview::buildCommands(slots, level, planes, first, count);
        CHECK(first == std::vector<U32>({ 0, 3, 9, 12 }));
        CHECK(count == std::vector<U32>({ 3, 6, 0, 3 }));
        std::vector<S32> runs_first, runs_count;
        skoverview::mergeRuns(first, count, runs_first, runs_count);
        // The first two meet and draw as one run; the fourth starts a run of its own.
        CHECK(runs_first == std::vector<S32>({ 0, 12 }));
        CHECK(runs_count == std::vector<S32>({ 9, 3 }));

        level.mGround = false;
        skoverview::buildCommands(slots, level, planes, first, count);
        CHECK(count == std::vector<U32>({ 0, 6, 0, 3 }));

        // A plane x <= -1 holds none of the boxes, which span x 0 to 1.
        planes[0][0] = 1.f;
        planes[0][3] = 1.f;
        skoverview::buildCommands(slots, everything, planes, first, count);
        CHECK(count == std::vector<U32>({ 0, 0, 0, 0 }));
        // x <= 0.5 cuts through them: each is kept.
        planes[0][3] = -0.5f;
        skoverview::buildCommands(slots, everything, planes, first, count);
        CHECK(count == std::vector<U32>({ 3, 6, 3, 3 }));
    }

    TEST_CASE("skoverviewslots: stable ids are reused lowest first", "[skoverviewslots]")
    {
        skoverview::IdPool ids;
        CHECK(ids.acquire() == 0);
        CHECK(ids.acquire() == 1);
        CHECK(ids.acquire() == 2);
        CHECK(ids.acquire() == 3);
        CHECK(ids.bound() == 4);
        ids.release(2);
        ids.release(0);
        CHECK(ids.bound() == 4);
        CHECK(ids.freeCount() == 2);
        CHECK(ids.acquire() == 0);
        CHECK(ids.acquire() == 2);
        CHECK(ids.acquire() == 4);
        // Releasing the top ids lowers the bound past every free id below them.
        ids.release(4);
        ids.release(2);
        ids.release(3);
        CHECK(ids.bound() == 2);
        CHECK(ids.freeCount() == 0);
        CHECK(ids.acquire() == 2);
    }

    // Flags as the census sets them; indexRun keeps them opaque.
    constexpr U8 PASS = 1;
    constexpr U8 OPEN = 2;

    // A soup's triangles as (flags, corners), each rotated to start at its least corner so winding is kept.
    using Tri = std::array<F32, 10>;
    std::vector<Tri> soupTris(const std::vector<LLVector3>& corners, const std::vector<U8>& per_tri)
    {
        std::vector<Tri> tris;
        for (size_t t = 0; t + 2 < corners.size(); t += 3)
        {
            std::array<std::array<F32, 3>, 3> c;
            for (S32 k = 0; k < 3; ++k)
            {
                c[k] = { corners[t + k].mV[0], corners[t + k].mV[1], corners[t + k].mV[2] };
            }
            const size_t least = std::min_element(c.begin(), c.end()) - c.begin();
            Tri tri;
            tri[0] = per_tri[t / 3];
            for (S32 k = 0; k < 3; ++k)
            {
                for (S32 a = 0; a < 3; ++a)
                {
                    tri[1 + k * 3 + a] = c[(least + k) % 3][a];
                }
            }
            tris.push_back(tri);
        }
        std::sort(tris.begin(), tris.end());
        return tris;
    }

    std::vector<Tri> indexedTris(const std::vector<LLVector3>& verts, const std::vector<U8>& per_vertex,
                                 const std::vector<U32>& indices)
    {
        std::vector<LLVector3> corners;
        std::vector<U8> per_tri;
        for (size_t i = 0; i < indices.size(); ++i)
        {
            corners.push_back(verts[indices[i]]);
            if (i % 3 == 0)
            {
                per_tri.push_back(per_vertex[indices[i]]);
            }
        }
        return soupTris(corners, per_tri);
    }

    TEST_CASE("skoverviewslots: a run welds shared corners only where their flags match", "[skoverviewslots]")
    {
        const LLVector3 a(0.f, 0.f, 0.f), b(1.f, 0.f, 0.f), c(1.f, 1.f, 0.f), d(0.f, 1.f, 0.f);
        const std::vector<LLVector3> quad = { a, b, c, a, c, d };
        {
            std::vector<LLVector3> verts = quad;
            std::vector<U8> flags = { 0, 0 };
            std::vector<U32> indices;
            skoverview::indexRun(verts, flags, indices);
            CHECK(verts.size() == 4);
            CHECK(flags.size() == 4);
            REQUIRE(indices.size() == 6);
            CHECK(indexedTris(verts, flags, indices) == soupTris(quad, std::vector<U8>{ 0, 0 }));
        }
        {
            std::vector<LLVector3> verts = quad;
            std::vector<U8> flags = { 0, OPEN };
            std::vector<U32> indices;
            skoverview::indexRun(verts, flags, indices);
            CHECK(verts.size() == 6);
            REQUIRE(indices.size() == 6);
            CHECK(indexedTris(verts, flags, indices) == soupTris(quad, std::vector<U8>{ 0, OPEN }));
        }
        {
            std::vector<LLVector3> verts;
            std::vector<U8> flags;
            std::vector<U32> indices;
            skoverview::indexRun(verts, flags, indices);
            CHECK(verts.empty());
            CHECK(flags.empty());
            CHECK(indices.empty());
        }
    }

    TEST_CASE("skoverviewslots: an indexed run draws the soup's triangles", "[skoverviewslots]")
    {
        // A grid's triangles, shuffled, with flags by row: welded and reordered, the same triangles, wound the same.
        std::mt19937 rng(20261026u);
        const S32 side = 24;
        std::vector<std::pair<std::array<LLVector3, 3>, U8>> tris;
        for (S32 y = 0; y < side; ++y)
        {
            for (S32 x = 0; x < side; ++x)
            {
                const LLVector3 p00((F32)x, (F32)y, 0.f), p10((F32)x + 1, (F32)y, 0.f);
                const LLVector3 p11((F32)x + 1, (F32)y + 1, 0.f), p01((F32)x, (F32)y + 1, 0.f);
                const U8 flag = y % 5 == 0 ? PASS : 0;
                tris.push_back({ { p00, p10, p11 }, flag });
                tris.push_back({ { p00, p11, p01 }, flag });
            }
        }
        std::shuffle(tris.begin(), tris.end(), rng);
        std::vector<LLVector3> soup;
        std::vector<U8> per_tri;
        for (const auto& [corners, flag] : tris)
        {
            soup.insert(soup.end(), corners.begin(), corners.end());
            per_tri.push_back(flag);
        }
        std::vector<LLVector3> verts = soup;
        std::vector<U8> flags = per_tri;
        std::vector<U32> indices;
        skoverview::indexRun(verts, flags, indices);
        REQUIRE(indices.size() == soup.size());
        CHECK(verts.size() < soup.size() / 2);
        for (const U32 i : indices)
        {
            REQUIRE(i < verts.size());
        }
        // Fetch order: the vertices are first used in the order they are stored.
        U32 next = 0;
        bool fetch_order = true;
        for (const U32 i : indices)
        {
            fetch_order = fetch_order && i <= next;
            next = std::max(next, i + 1);
        }
        CHECK(fetch_order);
        CHECK(indexedTris(verts, flags, indices) == soupTris(soup, per_tri));
    }
}
