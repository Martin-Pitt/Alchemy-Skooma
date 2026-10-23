/**
 * @file skroomsprobeinvariants_test.cpp
 * @brief Tests for the probe graph's invariants: symmetric truncation at the
 *        degree cap, the neighbour budget it protects, determinism, and the
 *        graph a census replay's placement builds.
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


#include "../skroomsprobes.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>

namespace
{
    using namespace skworld;

    /// The neighbour lists a census replay's placement builds, read from the
    /// cache the evaluation harness writes. Layout: magic, mesh count, per
    /// mesh origin/min/max, per mesh space count and spaces, portals ignored.
    bool readWorld(const std::string& path, std::vector<LLVector3d>& origins,
                   std::vector<std::vector<Space>>& spaces, std::string& error)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in)
        {
            error = "no such file";
            return false;
        }
        U32 magic = 0;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != 0x32435053u)
        {
            error = "wrong magic";
            return false;
        }
        U32 mesh_count = 0;
        in.read(reinterpret_cast<char*>(&mesh_count), sizeof(mesh_count));
        for (U32 m = 0; m < mesh_count && in; ++m)
        {
            LLVector3d origin, vmin, vmax;
            in.read(reinterpret_cast<char*>(&origin), sizeof(origin));
            in.read(reinterpret_cast<char*>(&vmin), sizeof(vmin));
            in.read(reinterpret_cast<char*>(&vmax), sizeof(vmax));
            U32 space_count = 0;
            in.read(reinterpret_cast<char*>(&space_count), sizeof(space_count));
            std::vector<Space> list(space_count);
            for (Space& s : list)
            {
                U64 raster_count = 0;
                in.read(reinterpret_cast<char*>(&raster_count), sizeof(raster_count));
                s.mRaster.resize(raster_count);
                in.read(reinterpret_cast<char*>(s.mRaster.data()), std::streamsize(raster_count * sizeof(std::pair<U64, U32>)));
                in.read(reinterpret_cast<char*>(&s.mBucket), sizeof(s.mBucket));
                in.read(reinterpret_cast<char*>(&s.mCeilingMean), sizeof(s.mCeilingMean));
                in.read(reinterpret_cast<char*>(&s.mFloorMin), sizeof(s.mFloorMin));
                in.read(reinterpret_cast<char*>(&s.mOpenShare), sizeof(s.mOpenShare));
                in.read(reinterpret_cast<char*>(&s.mMin), sizeof(s.mMin));
                in.read(reinterpret_cast<char*>(&s.mMax), sizeof(s.mMax));
                in.read(reinterpret_cast<char*>(&s.mId), sizeof(s.mId));
                in.read(reinterpret_cast<char*>(&s.mFlight), sizeof(s.mFlight));
                in.read(reinterpret_cast<char*>(&s.mLift), sizeof(s.mLift));
            }
            U32 portal_count = 0;
            in.read(reinterpret_cast<char*>(&portal_count), sizeof(portal_count));
            constexpr U32 PORTAL_BYTES = sizeof(U32) + sizeof(U32) + sizeof(U8) + sizeof(LLVector3) * 3;
            in.seekg(std::streamsize(portal_count) * PORTAL_BYTES, std::ios::cur);
            origins.push_back(origin);
            spaces.push_back(std::move(list));
        }
        return bool(in) || in.eof();
    }
}

namespace
{

    TEST_CASE("skroomsprobeinvariants: truncation is symmetric at the degree cap", "[skroomsprobeinvariants]")
    {
        // Node 0 meets twenty partners in order; every kept link must be
        // kept by both ends, or the shader walks into a one-sided edge.
        const auto linked = [](U32 a, U32 b) { return a == 0 || b == 0; };
        const std::vector<std::vector<U32>> lists = symmetricGraph(21, linked, 16);
        { INFO("the hub keeps sixteen"); REQUIRE((lists[0].size()) == (16u)); }
        { INFO("its first partner"); REQUIRE((lists[0].front()) == (1u)); }
        { INFO("its last partner"); REQUIRE((lists[0].back()) == (16u)); }
        for (const U32 other : lists[0])
        {
            { INFO("the link is two-sided"); REQUIRE((lists[other].size()) == (1u)); }
            { INFO("and names the hub back"); REQUIRE((lists[other].front()) == (0u)); }
        }
        U32 kept = 0;
        for (const auto& l : lists)
        {
            kept += static_cast<U32>(l.size());
        }
        { INFO("edges are counted once per end"); REQUIRE((kept == 32u)); }
    }

    TEST_CASE("skroomsprobeinvariants: a full graph under the cap keeps everything", "[skroomsprobeinvariants]")
    {
        const auto all = [](U32, U32) { return true; };
        const std::vector<std::vector<U32>> lists = symmetricGraph(3, all, 2);
        for (U32 i = 0; i < 3; ++i)
        {
            { INFO("both partners kept"); REQUIRE((lists[i].size()) == (2u)); }
            for (const U32 j : lists[i])
            {
                bool back = false;
                for (const U32 k : lists[j])
                {
                    back = back || k == i;
                }
                { INFO("symmetric"); REQUIRE((back)); }
            }
        }
    }

    TEST_CASE("skroomsprobeinvariants: the graph is deterministic and inside the neighbour budget", "[skroomsprobeinvariants]")
    {
        // A ring of sixty, each linked to its four nearest by index, plus a
        // hub everyone links to: more links than any cap allows.
        const auto linked = [](U32 a, U32 b)
        {
            if (a == 0 || b == 0) return true;
            const U32 d = a > b ? a - b : b - a;
            return d == 1 || d == 2;
        };
        const std::vector<std::vector<U32>> first = symmetricGraph(60, linked, 8);
        const std::vector<std::vector<U32>> again = symmetricGraph(60, linked, 8);
        { INFO("same shape"); REQUIRE((first.size()) == (again.size())); }
        U64 ints = 0;
        for (U32 i = 0; i < first.size(); ++i)
        {
            { INFO("byte-identical"); REQUIRE((first[i] == again[i])); }
            ints += first[i].size();
        }
        { INFO("inside the budget the shader packs"); REQUIRE((ints <= 4096ull)); }
    }

    TEST_CASE("skroomsprobeinvariants: the graph a census replay's placement builds holds the invariants", "[skroomsprobeinvariants]")
    {
        const char* path = getenv("SKNAV_PROBES_SPACES");
        if (!path || !*path)
        {
            return;     // env-gated: names the harness's spaces.bin
        }
        std::vector<LLVector3d> origins;
        std::vector<std::vector<Space>> spaces;
        std::string error;
        if (!readWorld(path, origins, spaces, error))
        {
            std::cerr << "world read failed: " << error << std::endl;
            { INFO("read the world"); REQUIRE((false)); }
            return;
        }

        // Place as the viewer does: one part's sphere per entry.
        std::vector<LLVector3> probes;
        const ProbeParams params;
        for (size_t m = 0; m < spaces.size(); ++m)
        {
            for (const Space& s : spaces[m])
            {
                for (const ProbePart& part : spaceProbes(s, params))
                {
                    probes.push_back(LLVector3(origins[m] + LLVector3d(part.mOrigin)));
                }
            }
        }
        std::cerr << "probes placed: " << probes.size() << " over " << spaces.size() << " meshes" << std::endl;
        { INFO("probes were placed"); REQUIRE((!probes.empty())); }

        // Radii: re-derive them from a second placement pass so the
        // predicate has both ends.
        std::vector<F32> radii;
        radii.reserve(probes.size());
        for (size_t m = 0; m < spaces.size(); ++m)
        {
            for (const Space& s : spaces[m])
            {
                for (const ProbePart& part : spaceProbes(s, params))
                {
                    radii.push_back(std::max(part.mRadius, part.mSpaceRadius));
                }
            }
        }
        const auto real_link = [&probes, &radii](U32 a, U32 b)
        {
            const F32 dx = probes[a].mV[VX] - probes[b].mV[VX];
            const F32 dy = probes[a].mV[VY] - probes[b].mV[VY];
            const F32 dz = probes[a].mV[VZ] - probes[b].mV[VZ];
            const F32 reach = radii[a] + radii[b];
            return dx * dx + dy * dy + dz * dz < reach * reach;
        };
        const std::vector<std::vector<U32>> lists = symmetricGraph(static_cast<U32>(probes.size()), real_link, 16);
        U32 max_degree = 0;
        U64 ints = 0;
        for (U32 i = 0; i < lists.size(); ++i)
        {
            max_degree = std::max(max_degree, static_cast<U32>(lists[i].size()));
            ints += lists[i].size();
            for (const U32 j : lists[i])
            {
                bool back = false;
                for (const U32 k : lists[j])
                {
                    back = back || k == i;
                }
                { INFO("symmetric"); REQUIRE((back)); }
            }
        }
        std::cerr << "existence graph: max degree " << max_degree << ", neighbour ints " << ints
                  << " over " << probes.size() << " probes" << std::endl;
        { INFO("degree inside the cap"); REQUIRE((max_degree <= 16)); }

        // The shader packs at most 256 probes' neighbour lists into 4096
        // ints, which is a constraint on the resident set, never on the
        // existence set: simulate a resident tier of the first 256 probes
        // in the placement order and hold it to the budget.
        const U32 resident = std::min<U32>(256, static_cast<U32>(probes.size()));
        const std::vector<std::vector<U32>> rlists = symmetricGraph(resident, real_link, 16);
        U64 rints = 0;
        U32 rdegree = 0;
        for (U32 i = 0; i < rlists.size(); ++i)
        {
            rdegree = std::max(rdegree, static_cast<U32>(rlists[i].size()));
            rints += rlists[i].size();
            for (const U32 j : rlists[i])
            {
                bool back = false;
                for (const U32 k : rlists[j])
                {
                    back = back || k == i;
                }
                { INFO("resident graph symmetric"); REQUIRE((back)); }
            }
        }
        std::cerr << "resident graph: max degree " << rdegree << ", neighbour ints " << rints
                  << " over " << resident << " probes" << std::endl;
        { INFO("resident degree inside the cap"); REQUIRE((rdegree <= 16)); }
        { INFO("resident list inside the shader's neighbour budget"); REQUIRE((rints <= 4096ull)); }
    }
}
