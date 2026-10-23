/**
 * @file skroomsprobeseval_test.cpp
 * @brief Probe placement strategies replayed against a census export:
 *        coverage, blends across portals, the neighbour graph, and the
 *        budget's churn along a camera path. Env-gated, so the usual run
 *        skips it; name a census to evaluate with SKROOMS_PROBES_REPLAY and
 *        an output directory with SKROOMS_PROBES_OUT.
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


#include "skroomstestworld.h"
#include "../skroomsprobes.h"
#include "../skroomsspaceproducts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>

namespace
{
    using namespace skworld;
}

namespace
{

    TEST_CASE("skroomsprobeseval: evaluate probe placement strategies on an exported census", "[skroomsprobeseval]")
    {
        const std::string census_path = LLStringUtil::getenv("SKROOMS_PROBES_REPLAY");
        if (census_path.empty())
        {
            return;
        }
        std::string out_dir = LLStringUtil::getenv("SKROOMS_PROBES_OUT");
        if (out_dir.empty())
        {
            out_dir = "V:\\Scratch\\nav-probes";
        }
        LLFile::mkdir(out_dir);

        CensusFile census;
        std::string error;
        skrooms_test::World built;
        bool read = false;
        const auto build = [&](skrooms_test::AirMode mode) {
            read = skrooms_test::buildWorld(census_path, mode, census, built, &error);
            if (!read)
            {
                LL_WARNS() << "census read failed: " << error << LL_ENDL;
            }
            return read;
        };

        // What the strategies place against, per mesh: the spaces as the pass
        // cut them, and the portals between them. Cached to the output
        // directory, so strategy iterations do not replay the census each
        // time; delete spaces.bin to rebuild it.
        struct WorldMesh
        {
            LLVector3d mOrigin;
            LLVector3d mMin, mMax;
            std::vector<Space> mSpaces;
            struct PortalEntry { U32 mA = 0, mB = 0; bool mCross = false; LLVector3 mCentre, mEndA, mEndB; };
            std::vector<PortalEntry> mPortals;   // space a, space b, cross mesh, centre and ends
        };
        std::vector<WorldMesh> world;
        const std::string cache_path = out_dir + "\\spaces.bin";
        constexpr U32 CACHE_MAGIC = 0x32435053; // "SPC2": spaces + portal ends, v2
        {
            std::ifstream cache(cache_path.c_str(), std::ios::binary);
            U32 magic = 0;
            if (cache)
            {
                cache.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            }
            if (cache && magic == CACHE_MAGIC)
            {
                U32 mesh_count = 0;
                cache.read(reinterpret_cast<char*>(&mesh_count), sizeof(mesh_count));
                for (U32 m = 0; m < mesh_count && cache; ++m)
                {
                    WorldMesh wm;
                    cache.read(reinterpret_cast<char*>(&wm.mOrigin), sizeof(wm.mOrigin));
                    cache.read(reinterpret_cast<char*>(&wm.mMin), sizeof(wm.mMin));
                    cache.read(reinterpret_cast<char*>(&wm.mMax), sizeof(wm.mMax));
                    U32 space_count = 0;
                    cache.read(reinterpret_cast<char*>(&space_count), sizeof(space_count));
                    wm.mSpaces.resize(space_count);
                    for (Space& s : wm.mSpaces)
                    {
                        U64 raster_count = 0;
                        cache.read(reinterpret_cast<char*>(&raster_count), sizeof(raster_count));
                        s.mRaster.resize(raster_count);
                        cache.read(reinterpret_cast<char*>(s.mRaster.data()), std::streamsize(raster_count * sizeof(std::pair<U64, U32>)));
                        cache.read(reinterpret_cast<char*>(&s.mBucket), sizeof(s.mBucket));
                        cache.read(reinterpret_cast<char*>(&s.mCeilingMean), sizeof(s.mCeilingMean));
                        cache.read(reinterpret_cast<char*>(&s.mFloorMin), sizeof(s.mFloorMin));
                        cache.read(reinterpret_cast<char*>(&s.mOpenShare), sizeof(s.mOpenShare));
                        cache.read(reinterpret_cast<char*>(&s.mMin), sizeof(s.mMin));
                        cache.read(reinterpret_cast<char*>(&s.mMax), sizeof(s.mMax));
                        cache.read(reinterpret_cast<char*>(&s.mId), sizeof(s.mId));
                        cache.read(reinterpret_cast<char*>(&s.mFlight), sizeof(s.mFlight));
                        cache.read(reinterpret_cast<char*>(&s.mLift), sizeof(s.mLift));
                    }
                    U32 portal_count = 0;
                    cache.read(reinterpret_cast<char*>(&portal_count), sizeof(portal_count));
                    for (U32 p = 0; p < portal_count && cache; ++p)
                    {
                        WorldMesh::PortalEntry e;
                        U8 cross = 0;
                        cache.read(reinterpret_cast<char*>(&e.mA), sizeof(e.mA));
                        cache.read(reinterpret_cast<char*>(&e.mB), sizeof(e.mB));
                        cache.read(reinterpret_cast<char*>(&cross), sizeof(cross));
                        cache.read(reinterpret_cast<char*>(&e.mCentre), sizeof(e.mCentre));
                        if (cache.read(reinterpret_cast<char*>(&e.mEndA), sizeof(e.mEndA)) &&
                            cache.read(reinterpret_cast<char*>(&e.mEndB), sizeof(e.mEndB)))
                        {
                            e.mCross = cross != 0;
                            wm.mPortals.push_back(e);
                        }
                        else
                        {
                            break;      // an old cache without the ends
                        }
                    }
                    world.push_back(std::move(wm));
                }
                std::cerr << "world from cache: " << world.size() << " meshes" << std::endl;
            }
        }
        if (world.empty())
        {
            { INFO("read the census"); REQUIRE((build(skrooms_test::AirMode::EVERYWHERE))); }
            for (const auto& mesh : built.mMeshes)
            {
                if (!mesh->mRooms || mesh->mRooms->mSpaces.empty())
                {
                    continue;
                }
                WorldMesh wm;
                wm.mOrigin = mesh->mOrigin;
                wm.mMin = mesh->mAirMin;
                wm.mMax = mesh->mAirMax;
                wm.mSpaces = mesh->mRooms->mSpaces;
                for (const Portal& p : mesh->mRooms->mPortals)
                {
                    if (p.mSpaces[0] < wm.mSpaces.size() && p.mSpaces[1] < wm.mSpaces.size())
                    {
                        wm.mPortals.push_back({ p.mSpaces[0], p.mSpaces[1], p.mCrossMesh, p.mCentre, p.mA, p.mB });
                    }
                }
                world.push_back(std::move(wm));
            }
            std::ofstream cache(cache_path.c_str(), std::ios::binary);
            cache.write(reinterpret_cast<const char*>(&CACHE_MAGIC), sizeof(CACHE_MAGIC));
            const U32 mesh_count = static_cast<U32>(world.size());
            cache.write(reinterpret_cast<const char*>(&mesh_count), sizeof(mesh_count));
            for (const WorldMesh& wm : world)
            {
                cache.write(reinterpret_cast<const char*>(&wm.mOrigin), sizeof(wm.mOrigin));
                cache.write(reinterpret_cast<const char*>(&wm.mMin), sizeof(wm.mMin));
                cache.write(reinterpret_cast<const char*>(&wm.mMax), sizeof(wm.mMax));
                const U32 space_count = static_cast<U32>(wm.mSpaces.size());
                cache.write(reinterpret_cast<const char*>(&space_count), sizeof(space_count));
                for (const Space& s : wm.mSpaces)
                {
                    const U64 raster_count = s.mRaster.size();
                    cache.write(reinterpret_cast<const char*>(&raster_count), sizeof(raster_count));
                    cache.write(reinterpret_cast<const char*>(s.mRaster.data()), std::streamsize(raster_count * sizeof(std::pair<U64, U32>)));
                    cache.write(reinterpret_cast<const char*>(&s.mBucket), sizeof(s.mBucket));
                    cache.write(reinterpret_cast<const char*>(&s.mCeilingMean), sizeof(s.mCeilingMean));
                    cache.write(reinterpret_cast<const char*>(&s.mFloorMin), sizeof(s.mFloorMin));
                    cache.write(reinterpret_cast<const char*>(&s.mOpenShare), sizeof(s.mOpenShare));
                    cache.write(reinterpret_cast<const char*>(&s.mMin), sizeof(s.mMin));
                    cache.write(reinterpret_cast<const char*>(&s.mMax), sizeof(s.mMax));
                    cache.write(reinterpret_cast<const char*>(&s.mId), sizeof(s.mId));
                    cache.write(reinterpret_cast<const char*>(&s.mFlight), sizeof(s.mFlight));
                    cache.write(reinterpret_cast<const char*>(&s.mLift), sizeof(s.mLift));
                }
                const U32 portal_count = static_cast<U32>(wm.mPortals.size());
                cache.write(reinterpret_cast<const char*>(&portal_count), sizeof(portal_count));
                for (const auto& entry : wm.mPortals)
                {
                    cache.write(reinterpret_cast<const char*>(&entry.mA), sizeof(entry.mA));
                    cache.write(reinterpret_cast<const char*>(&entry.mB), sizeof(entry.mB));
                    const U8 cross_byte = entry.mCross ? 1 : 0;
                    cache.write(reinterpret_cast<const char*>(&cross_byte), sizeof(cross_byte));
                    cache.write(reinterpret_cast<const char*>(&entry.mCentre), sizeof(entry.mCentre));
                    cache.write(reinterpret_cast<const char*>(&entry.mEndA), sizeof(entry.mEndA));
                    cache.write(reinterpret_cast<const char*>(&entry.mEndB), sizeof(entry.mEndB));
                }
            }
            std::cerr << "world cached: " << world.size() << " meshes" << std::endl;
        }

        struct Probe
        {
            LLVector3 mOrigin;      // global
            F32 mRadius = 0.f;
            U32 mMesh = 0;
            U32 mSpace = 0;
        };
        struct SpaceColumns         // a space's walkable columns, for coverage
        {
            std::vector<LLVector3> mColumns;    // global, just above its floor
            U32 mMesh = 0;
            U32 mIndex = 0;
            U32 mId = 0;
        };

        std::vector<const WorldMesh*> meshes;
        std::vector<SpaceColumns> cells;
        LLVector3d world_min(1e30, 1e30, 1e30), world_max(-1e30, -1e30, -1e30);
        size_t biggest = 0, biggest_cells = 0;
        for (const WorldMesh& wm : world)
        {
            const U32 mesh_index = static_cast<U32>(meshes.size());
            meshes.push_back(&wm);
            size_t mesh_cells = 0;
            for (size_t i = 0; i < wm.mSpaces.size(); ++i)
            {
                const Space& s = wm.mSpaces[i];
                SpaceColumns sc;
                sc.mMesh = mesh_index;
                sc.mIndex = static_cast<U32>(i);
                sc.mId = s.mId;
                for (const auto& r : s.mRaster)
                {
                    // The raster's keys pack 0.5 m columns and 1 m floor bins,
                    // biased, as floorPlanOf decodes them.
                    const F32 x = static_cast<F32>(static_cast<S64>((r.first >> 42) & 0x1fffff) - (1ll << 20)) * 0.5f;
                    const F32 y = static_cast<F32>(static_cast<S64>((r.first >> 21) & 0x1fffff) - (1ll << 20)) * 0.5f;
                    const F32 z = static_cast<F32>(static_cast<S64>(r.first & 0x1fffff) - (1ll << 20)) + 0.5f;
                    sc.mColumns.push_back(LLVector3(wm.mOrigin + LLVector3d(x + 0.25, y + 0.25, z)));
                }
                mesh_cells += sc.mColumns.size();
                cells.push_back(sc);
            }
            if (mesh_cells > biggest_cells)
            {
                biggest_cells = mesh_cells;
                biggest = mesh_index;
            }
            world_min = LLVector3d(std::min(world_min.mdV[VX], wm.mMin.mdV[VX]),
                                   std::min(world_min.mdV[VY], wm.mMin.mdV[VY]), 0.0);
            world_max = LLVector3d(std::max(world_max.mdV[VX], wm.mMax.mdV[VX]),
                                   std::max(world_max.mdV[VY], wm.mMax.mdV[VY]), 0.0);
        }
                { INFO("the replay holds spaces"); REQUIRE((!cells.empty())); }
        {
            // Why a space has no probe, classified: run the default
            // placement once over every space and count the rejections by
            // bucket and reason. The floors the part-folding drops are the
            // small ones; the raster-less never had walkable air.
            const skworld::ProbeParams defs;
            std::map<S32, std::map<std::string, U32>> unprobed;  // bucket -> reason -> spaces
            std::map<S32, U32> unprobed_by_bucket;
            for (const WorldMesh& wm : world)
            {
                for (const Space& s : wm.mSpaces)
                {
                    if (skworld::wantsProbes(s, defs) && !skworld::spaceProbes(s, defs).empty())
                    {
                        continue;   // this one places
                    }
                    auto& reasons = unprobed[S32(s.mBucket)];
                    ++unprobed_by_bucket[S32(s.mBucket)];
                    if (s.mRaster.empty())
                    {
                        reasons["no floor raster"]++;
                    }
                    else
                    {
                        F32 area = 0.f;
                        for (const auto& r : s.mRaster)
                        {
                            area += F32(r.second) * 0.25f;
                        }
                        reasons[area < 4.f ? "small, folded" : "folded for width"]++;
                    }
                }
            }
            std::map<U8, std::pair<U32, U32>> by_bucket;    // spaces, with raster >= 2 cells
            for (const WorldMesh& wm : world)
            {
                for (const Space& s : wm.mSpaces)
                {
                    U32 cell_sum = 0;
                    for (const auto& r : s.mRaster) cell_sum += r.second;
                    auto& e = by_bucket[static_cast<U8>(s.mBucket)];
                    e.first += 1;
                    e.second += cell_sum >= 2;
                }
            }
            for (const auto& it : by_bucket)
            {
                std::cerr << "bucket " << S32(it.first) << ": " << it.second.first << " spaces, "
                          << it.second.second << " with raster >= 2, "
                          << unprobed_by_bucket[S32(it.first)] << " place nothing" << std::endl;
            }
            std::ofstream unprobed_csv((out_dir + "\\unprobed.csv").c_str());
            unprobed_csv << "bucket,reason,spaces\n";
            for (const auto& bit : unprobed)
            {
                for (const auto& rit : bit.second)
                {
                    unprobed_csv << bit.first << ',' << rit.first << ',' << rit.second << '\n';
                }
            }
            std::cerr << "unprobed classification in unprobed.csv" << std::endl;
        }
        std::cerr << "meshes with spaces: " << meshes.size() << ", spaces: " << cells.size() << ", world "
                  << (world_max - world_min).length() << " m" << std::endl;

        const auto space_color = [](U32 id)
        {
            const U64 h = mix(SEED, id);
            return llformat("#%02x%02x%02x", S32(90.f + 130.f * ((h & 255) / 255.f)),
                            S32(90.f + 130.f * (((h >> 8) & 255) / 255.f)),
                            S32(90.f + 130.f * (((h >> 16) & 255) / 255.f)));
        };

        struct Strategy { const char* mName; F32 mShare; F32 mSpaceRadius; F32 mHold; F32 mBlendZone; F32 mUnknownRadius; bool mObligations; };
        const std::vector<Strategy> strategies =
        {
            { "baseline", 1.0f, 0.f, 8.f, 0.f, 8.f, false },
            { "obligation", 1.0f, 0.f, 8.f, 0.f, 8.f, true },
            { "obligation2", 1.0f, 0.f, 24.f, 0.f, 8.f, true },
            { "candidate", 1.0f, 0.f, 24.f, 2.f, 8.f, true },
            { "atomic", 1.0f, 0.f, 96.f, 2.f, 8.f, true },
        };

        std::ofstream metrics((out_dir + "\\metrics.json").c_str());
        metrics << "{ \"census\": \"" << census_path << "\", \"strategies\": [";

        for (size_t st = 0; st < strategies.size(); ++st)
        {
            const Strategy& strat = strategies[st];
            skworld::ProbeParams params;
            params.mCoverageShare = strat.mShare;
            params.mSpaceRadiusM = strat.mSpaceRadius;
            params.mBudgetHoldM = strat.mHold;
            params.mBlendZoneM = strat.mBlendZone;
            params.mUnknownRadiusM = strat.mUnknownRadius;

            // Place, per mesh and space.
            std::vector<Probe> probes;
            std::vector<std::map<U32, std::vector<U32>>> mesh_spaces(meshes.size());
            U32 spaces_probed = 0;
            U32 bridges = 0;
            U32 obliged = 0;
            for (size_t mi = 0; mi < meshes.size(); ++mi)
            {
                const WorldMesh& wm = *meshes[mi];
                for (size_t i = 0; i < wm.mSpaces.size(); ++i)
                {
                    const std::vector<ProbePart> parts = spaceProbes(wm.mSpaces[i], params);
                    if (!parts.empty())
                    {
                        ++spaces_probed;
                    }
                    for (const ProbePart& part : parts)
                    {
                        Probe probe;
                        probe.mOrigin = LLVector3(wm.mOrigin + LLVector3d(part.mOrigin));
                        probe.mRadius = std::max(part.mRadius, part.mSpaceRadius);
                        probe.mMesh = static_cast<U32>(mi);
                        probe.mSpace = static_cast<U32>(i);
                        mesh_spaces[mi][static_cast<U32>(i)].push_back(static_cast<U32>(probes.size()));
                        probes.push_back(probe);
                    }
                }
            }

            if (strat.mObligations)
            for (size_t mi = 0; mi < meshes.size(); ++mi)
            {
                const WorldMesh& wm = *meshes[mi];
                // Every portal where the two sides' probe sets have no
                // intersecting pair: each side's nearest sphere grows to
                // cover the portal centre by the blend margin, which forces
                // the intersection the neighbour graph needs. Growth is
                // capped at the octree reach and at a multiple of the
                // sphere's own placed radius, so a corridor of thresholds
                // cannot balloon one sphere; two rounds, re-choosing the
                // nearest after the first round's growth.
                for (S32 round = 0; round < 2; ++round)
                {
                    U32 grew_this_round = 0;
                    for (const auto& entry : wm.mPortals)
                    {
                        if (entry.mA >= wm.mSpaces.size() || entry.mB >= wm.mSpaces.size())
                        {
                            continue;
                        }
                        const auto meets = [&]() -> bool
                        {
                            for (U32 a : mesh_spaces[mi][entry.mA])
                            {
                                for (U32 b : mesh_spaces[mi][entry.mB])
                                {
                                    const F32 dx = probes[a].mOrigin.mV[VX] - probes[b].mOrigin.mV[VX];
                                    const F32 dy = probes[a].mOrigin.mV[VY] - probes[b].mOrigin.mV[VY];
                                    const F32 dz = probes[a].mOrigin.mV[VZ] - probes[b].mOrigin.mV[VZ];
                                    const F32 reach = probes[a].mRadius + probes[b].mRadius;
                                    if (dx * dx + dy * dy + dz * dz < reach * reach) return true;
                                }
                            }
                            return false;
                        };
                        if (meets())
                        {
                            continue;
                        }
                        const LLVector3 portal = LLVector3(wm.mOrigin + LLVector3d(entry.mCentre)) + LLVector3(0.f, 0.f, 1.f);
                        for (U32 side : { entry.mA, entry.mB })
                        {
                            if (mesh_spaces[mi][side].empty())
                            {
                                continue;
                            }
                            U32 best = 0;
                            F32 best_d2 = 1e30f;
                            for (U32 p : mesh_spaces[mi][side])
                            {
                                const F32 d2 = dist_vec_squared(portal, probes[p].mOrigin);
                                if (d2 < best_d2)
                                {
                                    best_d2 = d2;
                                    best = p;
                                }
                            }
                            // The margin: what a blend zone of the owed width
                            // needs at this threshold, plus a metre of grace.
                            const F32 w = std::max(params.mBlendZoneM > 0.f ? params.mBlendZoneM : 2.f,
                                                   params.mBlendShare * std::min(probes[best].mRadius, 24.f));
                            const F32 margin = std::max(w * 0.5f + 1.f, 2.f);
                            const F32 cap = std::min(params.mMaxRadiusM, params.mObligationShare * std::max(probes[best].mRadius, 1.f));
                            const F32 need = std::sqrt(best_d2) + margin;
                            if (need > probes[best].mRadius && need <= cap)
                            {
                                probes[best].mRadius = need;
                                ++grew_this_round;
                            }
                        }
                    }
                    if (!grew_this_round)
                    {
                        break;
                    }
                    obliged += grew_this_round;
                }
            }

            if (strat.mName == std::string("floorbridge") || strat.mName == std::string("candidate"))
            {
                // The graph repair. floorbridge: every portal measured bare.
                // candidate: only the truly bare ones, where both sides are
                // probeless - obligation has already served the rest - and
                // the bridge stays small, at the threshold, reaching just
                // past the mouth it serves.
                for (size_t mi = 0; mi < meshes.size(); ++mi)
                {
                    const WorldMesh& wm = *meshes[mi];
                    for (const auto& entry : wm.mPortals)
                    {
                        const U32 sa = entry.mA, sb = entry.mB;
                        if (sa >= wm.mSpaces.size() || sb >= wm.mSpaces.size())
                        {
                            continue;
                        }
                        if (strat.mName == std::string("candidate"))
                        {
                            if (!mesh_spaces[mi][sa].empty() || !mesh_spaces[mi][sb].empty())
                            {
                                continue;   // at least one side probes: not bare
                            }
                        }
                        const auto meets = [&]() -> bool
                        {
                            for (U32 a : mesh_spaces[mi][sa])
                            {
                                for (U32 b : mesh_spaces[mi][sb])
                                {
                                    const F32 dx = probes[a].mOrigin.mV[VX] - probes[b].mOrigin.mV[VX];
                                    const F32 dy = probes[a].mOrigin.mV[VY] - probes[b].mOrigin.mV[VY];
                                    const F32 dz = probes[a].mOrigin.mV[VZ] - probes[b].mOrigin.mV[VZ];
                                    const F32 reach = probes[a].mRadius + probes[b].mRadius;
                                    if (dx * dx + dy * dy + dz * dz < reach * reach) return true;
                                }
                            }
                            return false;
                        };
                        if (!mesh_spaces[mi][sa].empty() && !mesh_spaces[mi][sb].empty() && meets())
                        {
                            continue;
                        }
                        Probe bridge;
                        bridge.mOrigin = LLVector3(wm.mOrigin + LLVector3d(entry.mCentre)) + LLVector3(0.f, 0.f, 1.f);
                        bridge.mRadius = 3.f;
                        for (U32 side : { sa, sb })
                        {
                            for (U32 p : mesh_spaces[mi][side])
                            {
                                bridge.mRadius = std::max(bridge.mRadius, dist_vec(bridge.mOrigin, probes[p].mOrigin) + 1.f);
                            }
                        }
                        bridge.mRadius = std::min(bridge.mRadius, 8.f);
                        bridge.mMesh = static_cast<U32>(mi);
                        bridge.mSpace = sa;
                        mesh_spaces[mi][sa].push_back(static_cast<U32>(probes.size()));
                        probes.push_back(bridge);
                        ++bridges;
                    }
                }
            }

            // The neighbour graph, per mesh: two probes link when their
            // spheres intersect, as the manager's updateNeighbors has it. A
            // portal's spaces count as linked when any probe of the one meets
            // any of the other, which is what lets a blend cross the portal.
            std::map<std::pair<S32, S32>, std::vector<U32>> buckets_probe;
            for (size_t p = 0; p < probes.size(); ++p)
            {
                buckets_probe[{ S32(std::floor(probes[p].mOrigin.mV[VX] / 32.f)),
                                S32(std::floor(probes[p].mOrigin.mV[VY] / 32.f)) }].push_back(static_cast<U32>(p));
            }
            U32 portal_pairs = 0, portal_linked = 0, portal_cross = 0, portal_cross_linked = 0;
            U32 portal_no_probe = 0, portal_far = 0, portal_bare = 0;
            U64 thresholds_total = 0, thresholds_served = 0, threshold_points = 0, threshold_points_in = 0;
            U32 links = 0;
            std::vector<std::pair<U32, U32>> links_of_biggest;
            const auto linked = [&probes](U32 a, U32 b)
            {
                const F32 dx = probes[a].mOrigin.mV[VX] - probes[b].mOrigin.mV[VX];
                const F32 dy = probes[a].mOrigin.mV[VY] - probes[b].mOrigin.mV[VY];
                const F32 dz = probes[a].mOrigin.mV[VZ] - probes[b].mOrigin.mV[VZ];
                const F32 reach = probes[a].mRadius + probes[b].mRadius;
                return dx * dx + dy * dy + dz * dz < reach * reach;
            };
            for (size_t mi = 0; mi < meshes.size(); ++mi)
            {
                const WorldMesh& wm = *meshes[mi];
                // Every threshold's mouth: the centre, both ends, and the
                // metre and a half beyond each end, all to be under some
                // sphere of this mesh for the threshold to read served. This,
                // not a distance between spheres, is what a seam looks at.
                const auto covered_by_any = [&](const LLVector3& p)
                {
                    const S32 bx = S32(std::floor(p.mV[VX] / 32.f)), by = S32(std::floor(p.mV[VY] / 32.f));
                    for (S32 oy = -1; oy <= 1; ++oy)
                    {
                        for (S32 ox = -1; ox <= 1; ++ox)
                        {
                            const auto found = buckets_probe.find({ bx + ox, by + oy });
                            if (found == buckets_probe.end()) continue;
                            for (U32 q : found->second)
                            {
                                const LLVector3 d = p - probes[q].mOrigin;
                                if (d.lengthSquared() <= probes[q].mRadius * probes[q].mRadius) return true;
                            }
                        }
                    }
                    return false;
                };
                for (const auto& entry : wm.mPortals)
                {
                    const U32 sa = entry.mA, sb = entry.mB;
                    if (sa >= wm.mSpaces.size() || sb >= wm.mSpaces.size())
                    {
                        continue;
                    }
                    ++portal_pairs;
                    portal_cross += entry.mCross;
                    if (mesh_spaces[mi][sa].empty() && mesh_spaces[mi][sb].empty())
                    {
                        ++portal_no_probe;
                        ++portal_bare;
                        continue;
                    }
                    if (mesh_spaces[mi][sa].empty() || mesh_spaces[mi][sb].empty())
                    {
                        ++portal_no_probe;
                    }
                    const LLVector3 centre = LLVector3(wm.mOrigin + LLVector3d(entry.mCentre)) + LLVector3(0.f, 0.f, 1.f);
                    const LLVector3 ea = LLVector3(wm.mOrigin + LLVector3d(entry.mEndA)) + LLVector3(0.f, 0.f, 1.f);
                    const LLVector3 eb = LLVector3(wm.mOrigin + LLVector3d(entry.mEndB)) + LLVector3(0.f, 0.f, 1.f);
                    const LLVector3 dir = eb - ea;
                    const F32 len = dir.length();
                    const LLVector3 along = len > 1e-3f ? dir * (1.5f / len) : LLVector3::zero;
                    const LLVector3 perp = len > 1e-3f ? LLVector3(dir.mV[VY], -dir.mV[VX], 0.f) * (1.2f / len) : LLVector3::zero;
                    U32 served = 0, points = 0, in_probes = 0;
                    for (const LLVector3& p : { centre, ea, eb, centre + along, centre - along, centre + perp, centre - perp })
                    {
                        ++points;
                        in_probes += covered_by_any(p);
                    }
                    thresholds_total++;
                    threshold_points += points;
                    threshold_points_in += in_probes;
                    thresholds_served += in_probes == points;
                    bool joins = false;
                    for (U32 a : mesh_spaces[mi][sa])
                    {
                        for (U32 b : mesh_spaces[mi][sb])
                        {
                            if (linked(a, b)) { joins = true; break; }
                        }
                        if (joins) break;
                    }
                    portal_linked += joins;
                    portal_cross_linked += joins && entry.mCross;
                    portal_far += !joins;
                }
                for (size_t a = 0; a < probes.size(); ++a)
                {
                    if (probes[a].mMesh != mi) continue;
                    for (size_t b = a + 1; b < probes.size(); ++b)
                    {
                        if (probes[b].mMesh != mi) continue;
                        if (linked(static_cast<U32>(a), static_cast<U32>(b)))
                        {
                            ++links;
                            if (mi == biggest) links_of_biggest.emplace_back(static_cast<U32>(a), static_cast<U32>(b));
                        }
                    }
                }
            }

            // Coverage of every space's floor by the spheres, and how much of
            // it two spheres share, which is what a blend stands on. The
            // buckets from the threshold pass already list every probe once.
            const std::map<std::pair<S32, S32>, std::vector<U32>>& buckets = buckets_probe;
            U64 depth[5] = { 0, 0, 0, 0, 0 };   // columns under 0..4+ spheres
            U64 covered = 0, blended = 0, blended_same = 0, blended_cross = 0, columns = 0;
            for (const SpaceColumns& sc : cells)
            {
                for (const LLVector3& col : sc.mColumns)
                {
                    ++columns;
                    U32 hits = 0;
                    bool cross = false;
                    const S32 bx = S32(std::floor(col.mV[VX] / 32.f)), by = S32(std::floor(col.mV[VY] / 32.f));
                    for (S32 oy = -1; oy <= 1 && hits < 5; ++oy)
                    {
                        for (S32 ox = -1; ox <= 1 && hits < 5; ++ox)
                        {
                            const auto found = buckets.find({ bx + ox, by + oy });
                            if (found == buckets.end()) continue;
                            for (U32 p : found->second)
                            {
                                const LLVector3 d = col - probes[p].mOrigin;
                                if (d.lengthSquared() <= probes[p].mRadius * probes[p].mRadius)
                                {
                                    if (hits < 4)
                                    {
                                        cross = cross || probes[p].mMesh != sc.mMesh || probes[p].mSpace != sc.mIndex;
                                    }
                                    ++hits;
                                    if (hits == 5) break;
                                }
                            }
                        }
                    }
                    depth[std::min<U32>(hits, 4u)]++;
                    covered += hits >= 1;
                    blended_same += hits >= 2 && !cross;
                    blended_cross += hits >= 2 && cross;
                    blended += hits >= 2;
                }
            }

            // The budget along a camera path: how much the kept set churns
            // per step, which is what the camera's reach costs the placement.
            const LLVector3d span = world_max - world_min;
            const F32 reach = 192.f;
            const U32 cap = 128;
            U64 created = 0, dropped = 0, moved = 0, steps = 0;
            std::vector<Probe> held;
            const S32 GRID = 12;
            for (S32 gy = 0; gy <= GRID; ++gy)
            {
                for (S32 gx = 0; gx <= GRID; ++gx)
                {
                    const LLVector3 camera(F32(world_min.mdV[VX] + span.mdV[VX] * gx / GRID),
                                           F32(world_min.mdV[VY] + span.mdV[VY] * gy / GRID), 0.f);
                    std::vector<skworld::ProbeSlot> slots;
                    std::vector<LLVector3> origins;
                    for (size_t p = 0; p < probes.size(); ++p)
                    {
                        const F32 dist = std::max(dist_vec(camera, probes[p].mOrigin) - probes[p].mRadius, 0.f);
                        if (dist > reach) continue;
                        skworld::ProbeSlot slot;
                        slot.mKey = static_cast<U64>(origins.size());
                        slot.mSpace = probes[p].mSpace;
                        slot.mOrigin = probes[p].mOrigin;
                        slot.mRadius = probes[p].mRadius;
                        slot.mDistance = dist;
                        slots.push_back(slot);
                        origins.push_back(probes[p].mOrigin);
                    }
                    std::vector<LLVector3> held_origins;
                    for (const Probe& h : held) held_origins.push_back(h.mOrigin);
                    const std::vector<S32> matched = skworld::matchNearest(held_origins, origins, 4.f);
                    for (auto& slot : slots)
                    {
                        slot.mHeld = matched[slot.mKey] >= 0;
                    }
                    skworld::budgetProbes(slots, cap, params);
                    std::vector<Probe> next;
                    std::vector<U8> used_held(held.size(), 0);
                    for (const auto& slot : slots)
                    {
                        const S32 m = matched[slot.mKey];
                        if (m < 0)
                        {
                            ++created;
                        }
                        else
                        {
                            ++used_held[m];
                            moved += skworld::shouldReplace(held[m].mOrigin, held[m].mRadius, slot.mOrigin, slot.mRadius, params);
                        }
                        Probe probe;
                        probe.mOrigin = slot.mOrigin;
                        probe.mRadius = slot.mRadius;
                        probe.mMesh = probes[slot.mKey].mMesh;
                        probe.mSpace = slot.mSpace;
                        next.push_back(probe);
                    }
                    for (size_t h2 = 0; h2 < held.size(); ++h2)
                    {
                        dropped += !used_held[h2];
                    }
                    held.swap(next);
                    ++steps;
                }
            }

            metrics << (st ? "," : "") << "{ \"name\": \"" << strat.mName << "\", \"probes\": " << probes.size()
                    << ", \"bridges\": " << bridges
                    << ", \"obliged\": " << obliged
                    << ", \"spaces_probed\": " << spaces_probed
                    << ", \"thresholds_total\": " << thresholds_total
                    << ", \"thresholds_served\": " << (thresholds_total ? 100.0 * thresholds_served / thresholds_total : 0.0)
                    << ", \"threshold_points_in\": " << (threshold_points ? 100.0 * threshold_points_in / threshold_points : 0.0)
                    << ", \"portals_bare\": " << portal_bare
                    << ", \"coverage\": " << (columns ? 100.0 * covered / columns : 0.0)
                    << ", \"blend\": " << (columns ? 100.0 * blended / columns : 0.0)
                    << ", \"blend_same\": " << (columns ? 100.0 * blended_same / columns : 0.0)
                    << ", \"blend_cross\": " << (columns ? 100.0 * blended_cross / columns : 0.0)
                    << ", \"depth1\": " << (columns ? 100.0 * depth[1] / columns : 0.0)
                    << ", \"depth2\": " << (columns ? 100.0 * depth[2] / columns : 0.0)
                    << ", \"depth3\": " << (columns ? 100.0 * depth[3] / columns : 0.0)
                    << ", \"depth4\": " << (columns ? 100.0 * depth[4] / columns : 0.0)
                    << ", \"portals_linked\": " << (portal_pairs ? 100.0 * portal_linked / portal_pairs : 0.0)
                    << ", \"portals_without_probes\": " << portal_no_probe
                    << ", \"portals_too_far\": " << portal_far
                    << ", \"portals_cross\": " << portal_cross
                    << ", \"portals_cross_linked\": " << portal_cross_linked
                    << ", \"links\": " << links
                    << ", \"churn_per_step\": " << (steps ? (created + dropped) / F64(steps) : 0.0)
                    << ", \"moves_per_step\": " << (steps ? moved / F64(steps) : 0.0)
                    << " }";
            std::cerr << strat.mName << ": probes " << probes.size() << ", spaces probed " << spaces_probed
                      << ", thresholds served " << (thresholds_total ? 100.0 * thresholds_served / thresholds_total : 0.0)
                      << "% (points " << (threshold_points ? 100.0 * threshold_points_in / threshold_points : 0.0)
                      << "%, bare " << portal_bare << "), coverage " << (columns ? 100.0 * covered / columns : 0.0) << "%, blend "
                      << (columns ? 100.0 * blended / columns : 0.0) << "% (same "
                      << (columns ? 100.0 * blended_same / columns : 0.0) << ", cross "
                      << (columns ? 100.0 * blended_cross / columns : 0.0) << "), depth "
                      << (columns ? 100.0 * depth[1] / columns : 0.0) << "/"
                      << (columns ? 100.0 * depth[2] / columns : 0.0) << "/"
                      << (columns ? 100.0 * depth[3] / columns : 0.0) << "/"
                      << (columns ? 100.0 * depth[4] / columns : 0.0) << "%, portals linked "
                      << (portal_pairs ? 100.0 * portal_linked / portal_pairs : 0.0) << "% (no probe "
                      << portal_no_probe << ", too far " << portal_far << "), links " << links
                      << ", churn/step " << (steps ? (created + dropped) / F64(steps) : 0.0) << std::endl;

            // The biggest mesh's world as the strategy draws it: fills,
            // spheres and the links between them. Y is flipped, as SVG's
            // runs down.
            const WorldMesh& mesh = *meshes[biggest];
            const F32 min_x = F32(world_min.mdV[VX]), width = F32(world_max.mdV[VX] - world_min.mdV[VX]);
            const F32 max_y = F32(world_max.mdV[VY]), height = F32(world_max.mdV[VY] - world_min.mdV[VY]);
            char svg_name[256];
            snprintf(svg_name, 256, "%s\\%s.svg", out_dir.c_str(), strat.mName);
            std::ofstream svg(svg_name);
            svg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='" << min_x << ' ' << -max_y << ' '
                << width << ' ' << height << "' width='1024' height='1024'>\n";
            for (const auto& entry : mesh_spaces[biggest])
            {
                const std::string color = space_color(mesh.mSpaces[entry.first].mId);
                for (U32 p : entry.second)
                {
                    const Probe& probe = probes[p];
                    svg << "<circle cx='" << probe.mOrigin.mV[VX] << "' cy='" << -probe.mOrigin.mV[VY]
                        << "' r='" << probe.mRadius << "' fill='" << color << "' fill-opacity='0.15' stroke='none'/>\n";
                }
            }
            for (const auto& link : links_of_biggest)
            {
                svg << "<line x1='" << probes[link.first].mOrigin.mV[VX] << "' y1='" << -probes[link.first].mOrigin.mV[VY]
                    << "' x2='" << probes[link.second].mOrigin.mV[VX] << "' y2='" << -probes[link.second].mOrigin.mV[VY]
                    << "' stroke='#ffcc00' stroke-opacity='0.5' stroke-width='0.4'/>\n";
            }
            for (const auto& entry : mesh_spaces[biggest])
            {
                const std::string color = space_color(mesh.mSpaces[entry.first].mId);
                for (U32 p : entry.second)
                {
                    const Probe& probe = probes[p];
                    svg << "<circle cx='" << probe.mOrigin.mV[VX] << "' cy='" << -probe.mOrigin.mV[VY]
                        << "' r='" << probe.mRadius << "' fill='none' stroke='" << color << "' stroke-width='0.5'/>\n";
                }
            }
            svg << "</svg>\n";
        }
        metrics << "]}";
        std::cerr << "metrics and renders in " << out_dir << std::endl;
    }

    TEST_CASE("skroomsprobeseval: the budgeted resident set as the viewer computes it, per strategy, along a camera path", "[skroomsprobeseval]")
    {
        const std::string census_path = LLStringUtil::getenv("SKROOMS_PROBES_BENCH");
        if (census_path.empty())
        {
            return;
        }
        std::string out_dir = LLStringUtil::getenv("SKROOMS_PROBES_BENCH_OUT");
        if (out_dir.empty())
        {
            out_dir = "V:\\Scratch\\nav-probe-bench\\out";
        }
        LLFile::mkdir(out_dir);

        CensusFile census;
        std::string error;
        skrooms_test::World built;
        bool read = false;
        const auto build = [&](skrooms_test::AirMode mode) {
            read = skrooms_test::buildWorld(census_path, mode, census, built, &error);
            if (!read)
            {
                LL_WARNS() << "census read failed: " << error << LL_ENDL;
            }
            return read;
        };

        // The viewer's spaces: air only within the air radius of the census's
        // camera, as SKAirCells builds it. Cached beside the outputs.
        struct BenchMesh
        {
            LLVector3d mOrigin;
            std::vector<Space> mSpaces;
            std::vector<Portal> mPortals;
            std::vector<std::vector<std::pair<U64, U32>>> mPolyRasters;     ///< per space, its floor from its stamped polys
        };
        std::vector<BenchMesh> world;
        const bool from_polys = false;  // a space's parts come from its air raster: there are no polys
        const std::string cache_path = out_dir + "/spaces-radius.bin";
        constexpr U32 MAGIC = 0x31484E42;   // "BNH1"
        {
            std::ifstream in(cache_path.c_str(), std::ios::binary);
            U32 magic = 0, meshes = 0;
            if (in && in.read(reinterpret_cast<char*>(&magic), 4) && magic == MAGIC &&
                in.read(reinterpret_cast<char*>(&meshes), 4))
            {
                world.resize(meshes);
                for (BenchMesh& m : world)
                {
                    U32 ns = 0, np = 0;
                    in.read(reinterpret_cast<char*>(&m.mOrigin), sizeof(m.mOrigin));
                    in.read(reinterpret_cast<char*>(&ns), 4);
                    m.mSpaces.resize(ns);
                    for (Space& s : m.mSpaces)
                    {
                        U64 nr = 0;
                        in.read(reinterpret_cast<char*>(&nr), 8);
                        s.mRaster.resize(nr);
                        in.read(reinterpret_cast<char*>(s.mRaster.data()), std::streamsize(nr * sizeof(s.mRaster[0])));
                        in.read(reinterpret_cast<char*>(&s.mId), sizeof(s.mId));
                        in.read(reinterpret_cast<char*>(&s.mBucket), sizeof(s.mBucket));
                        in.read(reinterpret_cast<char*>(&s.mMin), sizeof(s.mMin));
                        in.read(reinterpret_cast<char*>(&s.mMax), sizeof(s.mMax));
                        in.read(reinterpret_cast<char*>(&s.mCeilingMean), sizeof(F32));
                        in.read(reinterpret_cast<char*>(&s.mFloorMin), sizeof(F32));
                        in.read(reinterpret_cast<char*>(&s.mOpenShare), sizeof(F32));
                    }
                    if (from_polys)
                    {
                        m.mPolyRasters.resize(ns);
                        for (auto& r : m.mPolyRasters)
                        {
                            U64 nr = 0;
                            in.read(reinterpret_cast<char*>(&nr), 8);
                            r.resize(nr);
                            in.read(reinterpret_cast<char*>(r.data()), std::streamsize(nr * sizeof(r[0])));
                        }
                    }
                    in.read(reinterpret_cast<char*>(&np), 4);
                    m.mPortals.resize(np);
                    for (Portal& p : m.mPortals)
                    {
                        in.read(reinterpret_cast<char*>(p.mSpaces), sizeof(p.mSpaces));
                        in.read(reinterpret_cast<char*>(&p.mCentre), sizeof(p.mCentre));
                        in.read(reinterpret_cast<char*>(&p.mKind), sizeof(p.mKind));
                    }
                }
                if (!in)
                {
                    world.clear();
                }
            }
        }
        if (world.empty())
        {
            { INFO("read the census"); REQUIRE((build(skrooms_test::AirMode::RADIUS))); }
            for (const auto& mesh : built.mMeshes)
            {
                if (!mesh->mRooms || mesh->mRooms->mSpaces.empty())
                {
                    continue;
                }
                world.push_back({ mesh->mOrigin, mesh->mRooms->mSpaces, mesh->mRooms->mPortals, {} });
            }
            std::ofstream out(cache_path.c_str(), std::ios::binary);
            const U32 meshes = static_cast<U32>(world.size());
            out.write(reinterpret_cast<const char*>(&MAGIC), 4);
            out.write(reinterpret_cast<const char*>(&meshes), 4);
            for (const BenchMesh& m : world)
            {
                const U32 ns = static_cast<U32>(m.mSpaces.size()), np = static_cast<U32>(m.mPortals.size());
                out.write(reinterpret_cast<const char*>(&m.mOrigin), sizeof(m.mOrigin));
                out.write(reinterpret_cast<const char*>(&ns), 4);
                for (const Space& s : m.mSpaces)
                {
                    const U64 nr = s.mRaster.size();
                    out.write(reinterpret_cast<const char*>(&nr), 8);
                    out.write(reinterpret_cast<const char*>(s.mRaster.data()), std::streamsize(nr * sizeof(s.mRaster[0])));
                    out.write(reinterpret_cast<const char*>(&s.mId), sizeof(s.mId));
                    out.write(reinterpret_cast<const char*>(&s.mBucket), sizeof(s.mBucket));
                    out.write(reinterpret_cast<const char*>(&s.mMin), sizeof(s.mMin));
                    out.write(reinterpret_cast<const char*>(&s.mMax), sizeof(s.mMax));
                    out.write(reinterpret_cast<const char*>(&s.mCeilingMean), sizeof(F32));
                    out.write(reinterpret_cast<const char*>(&s.mFloorMin), sizeof(F32));
                    out.write(reinterpret_cast<const char*>(&s.mOpenShare), sizeof(F32));
                }
                if (from_polys)
                {
                    for (size_t si = 0; si < m.mSpaces.size(); ++si)
                    {
                        static const std::vector<std::pair<U64, U32>> none;
                        const auto& r = si < m.mPolyRasters.size() ? m.mPolyRasters[si] : none;
                        const U64 nr = r.size();
                        out.write(reinterpret_cast<const char*>(&nr), 8);
                        out.write(reinterpret_cast<const char*>(r.data()), std::streamsize(nr * sizeof(r[0])));
                    }
                }
                out.write(reinterpret_cast<const char*>(&np), 4);
                for (const Portal& p : m.mPortals)
                {
                    out.write(reinterpret_cast<const char*>(p.mSpaces), sizeof(p.mSpaces));
                    out.write(reinterpret_cast<const char*>(&p.mCentre), sizeof(p.mCentre));
                    out.write(reinterpret_cast<const char*>(&p.mKind), sizeof(p.mKind));
                }
            }
        }
        { INFO("the replay holds spaces"); REQUIRE((!world.empty())); }

        SolidLattice solids;
        solids.rebuild(census.mRecords);

        // Everything below is in a frame at the census camera's region corner,
        // so F32 keeps centimetres.
        const LLVector3d frame(std::floor(census.mCamera.mdV[VX] / 256.0) * 256.0,
                               std::floor(census.mCamera.mdV[VY] / 256.0) * 256.0, 0.0);
        LLVector3 camera0(census.mCamera - frame);
        const std::string camera_env = LLStringUtil::getenv("SKROOMS_PROBES_CAMERA");
        if (!camera_env.empty())
        {
            F64 x = 0, y = 0, z = 0;
            if (sscanf(camera_env.c_str(), "%lf,%lf,%lf", &x, &y, &z) == 3)
            {
                camera0 = LLVector3(LLVector3d(x, y, z) - frame);
            }
        }
        const std::string cap_env = LLStringUtil::getenv("SKROOMS_PROBES_CAP");
        const std::string strategy_env = LLStringUtil::getenv("SKROOMS_PROBES_STRATEGY");

        // The world for the plots: every space's floor cells, and the portals.
        {
            std::ofstream wj((out_dir + "\\world.json").c_str());
            wj << "{\"frame\":[" << frame.mdV[VX] << ',' << frame.mdV[VY] << "],\"camera\":[" << camera0.mV[VX] << ','
               << camera0.mV[VY] << ',' << camera0.mV[VZ] << "],\"census\":\"";
            for (char c : census_path) wj << (c == '\\' ? "/" : std::string(1, c));
            wj << "\",\"spaces\":[";
            bool first = true;
            for (size_t mi = 0; mi < world.size(); ++mi)
            {
                const LLVector3 o(world[mi].mOrigin - frame);
                for (size_t si = 0; si < world[mi].mSpaces.size(); ++si)
                {
                    const Space& s = world[mi].mSpaces[si];
                    wj << (first ? "" : ",") << "{\"mesh\":" << mi << ",\"space\":" << si << ",\"id\":" << s.mId
                       << ",\"bucket\":" << S32(s.mBucket) << ",\"cells\":[";
                    first = false;
                    bool fc = true;
                    for (const auto& r : s.mRaster)
                    {
                        const F32 x = static_cast<F32>(static_cast<S64>((r.first >> 42) & 0x1fffff) - (1ll << 20)) * 0.5f + 0.25f;
                        const F32 y = static_cast<F32>(static_cast<S64>((r.first >> 21) & 0x1fffff) - (1ll << 20)) * 0.5f + 0.25f;
                        const F32 z = std::max(static_cast<F32>(static_cast<S64>(r.first & 0x1fffff) - (1ll << 20)), s.mFloorMin);
                        wj << (fc ? "" : ",") << llformat("%.2f,%.2f,%.2f", x + o.mV[VX], y + o.mV[VY], z + o.mV[VZ]);
                        fc = false;
                    }
                    wj << "]}";
                }
            }
            wj << "],\"portals\":[";
            first = true;
            for (size_t mi = 0; mi < world.size(); ++mi)
            {
                const LLVector3 o(world[mi].mOrigin - frame);
                for (const Portal& p : world[mi].mPortals)
                {
                    const LLVector3 c = o + p.mCentre;
                    wj << (first ? "" : ",") << "{\"mesh\":" << mi << ",\"a\":" << p.mSpaces[0] << ",\"b\":" << p.mSpaces[1]
                       << llformat(",\"c\":[%.2f,%.2f,%.2f]}", c.mV[VX], c.mV[VY], c.mV[VZ]);
                    first = false;
                }
            }
            wj << "]}";
        }

        // The viewer's update, as SKNavRoomProbes::update runs it: spaces in
        // reach, parts, the solid test, clearing placed probes (none in a
        // census), the portal pass, slots, matching to held probes, the budget
        // and one move per update.
        enum Status : U8 { KEPT, CAP, SPACING, PER_SPACE, SOLID, CLEARED };
        const char* status_names[] = { "kept", "cap", "spacing", "per_space", "solid", "cleared" };
        struct Candidate
        {
            LLVector3 mOrigin;
            F32 mRadius = 0.f;
            F32 mArea = 0.f;
            U32 mPart = 0;
            U32 mMesh = 0;
            U32 mSpace = 0;
            U8 mBucket = 3;
            bool mBridge = false;
            U8 mStatus = KEPT;
            LLVector3 mLo, mHi;     // the part's floor plan, floor to top; a bridge's own sphere bounds
        };
        struct Held
        {
            LLVector3 mOrigin;
            F32 mRadius = 0.f;
        };
        struct Strategy
        {
            std::string mName;
            U32 mCap = 128;
            ProbeParams mParams;
            bool mMeshSpaces = false;   // number spaces per mesh and space, not by slot alone
        };
        std::vector<Strategy> strategies;
        {
            // The budget before the street fix: one sphere per space, 8 m apart, 128 at most.
            Strategy s;
            s.mName = "before";
            s.mParams.mMaxPerSpace = 1;
            s.mParams.mSpacingM = 8.f;
            strategies.push_back(s);
            s.mName = "before-cap240";
            s.mCap = 240;
            strategies.push_back(s);
            // The viewer's defaults now.
            s = Strategy();
            s.mName = "after";
            s.mCap = 240;
            s.mMeshSpaces = true;
            strategies.push_back(s);
            s.mName = "after-cap128";
            s.mCap = 128;
            strategies.push_back(s);
            for (U32 cap : { 192u, 240u })
            {
                for (U32 per : { 4u, 8u, 16u, 1000u })
                {
                    for (F32 spacing : { 4.f, 6.f, 8.f, 10.f, 12.f })
                    {
                        s = Strategy();
                        s.mName = llformat("per%u-sp%.0f-cap%u", per, spacing, cap);
                        s.mCap = cap;
                        s.mMeshSpaces = true;
                        s.mParams.mMaxPerSpace = per;
                        s.mParams.mSpacingM = spacing;
                        strategies.push_back(s);
                    }
                }
            }
        }
        if (!cap_env.empty())
        {
            for (Strategy& s : strategies) s.mCap = static_cast<U32>(atoi(cap_env.c_str()));
        }

        std::map<std::pair<U32, U32>, std::vector<ProbePart>> part_cache;
        const auto run_update = [&](const Strategy& strat, const LLVector3& camera, std::vector<Held>& held,
                                    std::vector<Candidate>& out, U32& created, U32& dropped, U32& moved)
        {
            const ProbeParams& params = strat.mParams;
            const F32 REACH = 192.f;
            std::vector<Candidate> candidates;
            std::vector<Candidate> rejected;
            for (size_t mi = 0; mi < world.size(); ++mi)
            {
                const LLVector3 o(world[mi].mOrigin - frame);
                for (size_t si = 0; si < world[mi].mSpaces.size(); ++si)
                {
                    const Space& s = world[mi].mSpaces[si];
                    const LLVector3 lo = o + s.mMin, hi = o + s.mMax;
                    bool in = true;
                    for (S32 a = 0; a < 3; ++a)
                    {
                        in = in && lo.mV[a] <= camera.mV[a] + REACH && hi.mV[a] >= camera.mV[a] - REACH;
                    }
                    if (!in || !wantsProbes(s, params))
                    {
                        continue;
                    }
                    auto key = std::make_pair(static_cast<U32>(mi), static_cast<U32>(si));
                    auto found = part_cache.find(key);
                    if (found == part_cache.end())
                    {
                        Space shape = s;
                        if (from_polys && si < world[mi].mPolyRasters.size())
                        {
                            shape.mRaster = world[mi].mPolyRasters[si];
                        }
                        found = part_cache.emplace(key, spaceProbes(shape, params)).first;
                    }
                    for (const ProbePart& part : found->second)
                    {
                        Candidate c;
                        c.mOrigin = o + part.mOrigin;
                        c.mLo = o + part.mMin;
                        c.mHi = o + part.mMax;
                        c.mArea = part.mArea;
                        c.mPart = part.mPart;
                        c.mMesh = static_cast<U32>(mi);
                        c.mSpace = static_cast<U32>(si);
                        c.mBucket = static_cast<U8>(s.mBucket);
                        c.mRadius = std::max(part.mRadius, part.mSpaceRadius);
                        if (solids.contains(frame + LLVector3d(c.mOrigin)))
                        {
                            c.mStatus = SOLID;
                            rejected.push_back(c);
                            continue;
                        }
                        c.mRadius = clearRadius(c.mOrigin, c.mRadius, {}, params);
                        if (c.mRadius <= 0.f)
                        {
                            c.mRadius = std::max(part.mRadius, part.mSpaceRadius);
                            c.mStatus = CLEARED;
                            rejected.push_back(c);
                            continue;
                        }
                        candidates.push_back(c);
                    }
                }
            }

            // The portal pass, line for line with the viewer's.
            {
                std::map<std::pair<U32, U32>, std::vector<U32>> by_space;
                for (size_t i = 0; i < candidates.size(); ++i)
                {
                    by_space[{ candidates[i].mMesh, candidates[i].mSpace }].push_back(static_cast<U32>(i));
                }
                const auto link_pair = [&](U32 a, U32 b)
                {
                    const F32 reach = candidates[a].mRadius + candidates[b].mRadius;
                    return dist_vec_squared(candidates[a].mOrigin, candidates[b].mOrigin) < reach * reach;
                };
                const auto any_link = [&](const std::vector<U32>& as, const std::vector<U32>& bs)
                {
                    for (U32 a : as)
                        for (U32 b : bs)
                            if (link_pair(a, b)) return true;
                    return false;
                };
                const auto nearest_of = [&](const std::vector<U32>& list, const LLVector3& p)
                {
                    U32 best = 0;
                    F32 best_d2 = 1e30f;
                    for (U32 q : list)
                    {
                        const F32 d2 = dist_vec_squared(p, candidates[q].mOrigin);
                        if (d2 < best_d2) { best_d2 = d2; best = q; }
                    }
                    return std::make_pair(best, best_d2);
                };
                for (size_t mi = 0; mi < world.size(); ++mi)
                {
                    const LLVector3 o(world[mi].mOrigin - frame);
                    for (const Portal& portal : world[mi].mPortals)
                    {
                        const LLVector3 pc = o + portal.mCentre;
                        bool in = true;
                        for (S32 a = 0; a < 3; ++a)
                        {
                            in = in && fabsf(pc.mV[a] - camera.mV[a]) <= REACH;
                        }
                        if (!in || portal.mSpaces[0] >= world[mi].mSpaces.size() || portal.mSpaces[1] >= world[mi].mSpaces.size())
                        {
                            continue;
                        }
                        const U32 id_a = portal.mSpaces[0], id_b = portal.mSpaces[1];
                        const std::vector<U32> list_a = by_space[{ U32(mi), id_a }];
                        const std::vector<U32> list_b = by_space[{ U32(mi), id_b }];
                        if (!list_a.empty() && !list_b.empty() && any_link(list_a, list_b))
                        {
                            continue;
                        }
                        const LLVector3 centre = pc + LLVector3(0.f, 0.f, 1.f);
                        bool grew = false;
                        for (const std::vector<U32>* list : { &list_a, &list_b })
                        {
                            if (list->empty()) continue;
                            const auto pair = nearest_of(*list, centre);
                            Candidate& c = candidates[pair.first];
                            const F32 need = std::sqrt(pair.second) + 2.f;
                            const F32 cap = std::min(params.mMaxRadiusM, std::max(c.mRadius * params.mObligationShare, c.mRadius + 2.f));
                            if (need > c.mRadius && need <= cap)
                            {
                                c.mRadius = need;
                                grew = true;
                            }
                        }
                        if (!grew || list_a.empty() || list_b.empty() || !any_link(list_a, list_b))
                        {
                            Candidate bridge;
                            bridge.mOrigin = centre;
                            bridge.mRadius = 3.f;
                            for (const std::vector<U32>* list : { &list_a, &list_b })
                            {
                                if (list->empty()) continue;
                                const auto pair = nearest_of(*list, bridge.mOrigin);
                                bridge.mRadius = std::max(bridge.mRadius, std::sqrt(pair.second) + 1.f);
                            }
                            bridge.mRadius = std::min(bridge.mRadius, 8.f);
                            bridge.mLo = centre - LLVector3(bridge.mRadius, bridge.mRadius, bridge.mRadius);
                            bridge.mHi = centre + LLVector3(bridge.mRadius, bridge.mRadius, bridge.mRadius);
                            bridge.mMesh = static_cast<U32>(mi);
                            bridge.mSpace = id_a;
                            bridge.mBucket = static_cast<U8>(world[mi].mSpaces[id_a].mBucket);
                            bridge.mBridge = true;
                            by_space[{ U32(mi), id_a }].push_back(static_cast<U32>(candidates.size()));
                            candidates.push_back(bridge);
                        }
                    }
                }
            }

            std::map<std::pair<U32, U32>, U32> space_numbers;
            std::vector<ProbeSlot> slots;
            std::vector<LLVector3> origins;
            for (const Candidate& c : candidates)
            {
                ProbeSlot slot;
                slot.mKey = slots.size();
                slot.mSpace = strat.mMeshSpaces
                    ? space_numbers.emplace(std::make_pair(c.mMesh, c.mSpace), U32(space_numbers.size())).first->second
                    : c.mSpace;
                slot.mPart = c.mPart;
                slot.mOrigin = c.mOrigin;
                slot.mRadius = c.mRadius;
                slot.mArea = c.mArea;
                slot.mDistance = std::max(dist_vec(camera, c.mOrigin) - c.mRadius, 0.f);
                slots.push_back(slot);
                origins.push_back(c.mOrigin);
            }
            std::vector<LLVector3> held_origins;
            for (const Held& h : held) held_origins.push_back(h.mOrigin);
            const std::vector<S32> match = matchNearest(held_origins, origins, params.mMatchM);
            for (ProbeSlot& slot : slots) slot.mHeld = match[slot.mKey] >= 0;

            std::vector<std::pair<U64, ProbeDrop>> drops;
            budgetProbes(slots, strat.mCap, params, &drops);
            for (const auto& d : drops)
            {
                candidates[d.first].mStatus = d.second == ProbeDrop::CAP ? CAP : d.second == ProbeDrop::SPACING ? SPACING : PER_SPACE;
            }

            bool did_move = false;
            std::vector<Held> next;
            std::vector<U8> used(held.size(), 0);
            for (const ProbeSlot& slot : slots)
            {
                const Candidate& c = candidates[slot.mKey];
                const S32 m = match[slot.mKey];
                Held h;
                if (m >= 0)
                {
                    h = held[m];
                    used[m] = 1;
                    if (!did_move && shouldReplace(h.mOrigin, h.mRadius, c.mOrigin, c.mRadius, params))
                    {
                        did_move = true;
                        h.mOrigin = c.mOrigin;
                        h.mRadius = c.mRadius;
                        ++moved;
                    }
                }
                else
                {
                    h.mOrigin = c.mOrigin;
                    h.mRadius = c.mRadius;
                    ++created;
                }
                next.push_back(h);
            }
            for (U8 u : used) dropped += !u;
            held.swap(next);
            out = candidates;
            out.insert(out.end(), rejected.begin(), rejected.end());
        };

        // The camera path: from the primary view out 64 m and back along each
        // axis at 4 m a step, a flying camera's second.
        std::vector<LLVector3> path;
        path.push_back(camera0);
        for (S32 axis = 0; axis < 2; ++axis)
        {
            for (S32 dir : { 1, -1 })
            {
                for (S32 k = 1; k <= 16; ++k)
                {
                    LLVector3 p = camera0;
                    p.mV[axis] += dir * 4.f * k;
                    path.push_back(p);
                }
                for (S32 k = 15; k >= 0; --k)
                {
                    LLVector3 p = camera0;
                    p.mV[axis] += dir * 4.f * k;
                    path.push_back(p);
                }
            }
        }

        std::ofstream summary((out_dir + "\\bench.json").c_str());
        summary << "{\"strategies\":[";
        U32 written = 0;
        for (size_t si = 0; si < strategies.size(); ++si)
        {
            const Strategy& strat = strategies[si];
            if (!strategy_env.empty() && ("," + strategy_env + ",").find("," + strat.mName + ",") == std::string::npos)
            {
                continue;
            }
            std::vector<Held> held;
            std::vector<Candidate> primary;
            std::ofstream pj((out_dir + "\\probes-" + strat.mName + ".json").c_str());
            pj << "{\"strategy\":\"" << strat.mName << "\",\"cap\":" << strat.mCap << ",\"path\":[";
            F64 total_ms = 0.0;
            U64 churn_total = 0, moves_total = 0;
            for (size_t k = 0; k < path.size(); ++k)
            {
                std::vector<Candidate> cands;
                U32 created = 0, dropped = 0, moved = 0;
                const auto t0 = std::chrono::steady_clock::now();
                run_update(strat, path[k], held, cands, created, dropped, moved);
                const F64 ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (k == 0)
                {
                    primary = cands;
                }
                else
                {
                    total_ms += ms;
                    churn_total += created + dropped;
                    moves_total += moved;
                }
                pj << (k ? "," : "") << llformat("{\"camera\":[%.2f,%.2f,%.2f],\"created\":%u,\"dropped\":%u,\"moved\":%u,\"kept\":%u,\"ms\":%.3f,\"resident\":[",
                                                 path[k].mV[VX], path[k].mV[VY], path[k].mV[VZ], created, dropped, moved, U32(held.size()), ms);
                for (size_t h = 0; h < held.size(); ++h)
                {
                    pj << (h ? "," : "") << llformat("%.2f,%.2f,%.2f,%.2f", held[h].mOrigin.mV[VX], held[h].mOrigin.mV[VY], held[h].mOrigin.mV[VZ], held[h].mRadius);
                }
                pj << "]}";
            }
            pj << "],\"candidates\":[";
            for (size_t c = 0; c < primary.size(); ++c)
            {
                const Candidate& p = primary[c];
                pj << (c ? "," : "") << llformat("{\"o\":[%.2f,%.2f,%.2f],\"r\":%.2f,\"mesh\":%u,\"space\":%u,\"part\":%u,\"bucket\":%u,\"bridge\":%d,\"status\":\"%s\",\"lo\":[%.2f,%.2f,%.2f],\"hi\":[%.2f,%.2f,%.2f]}",
                                                 p.mOrigin.mV[VX], p.mOrigin.mV[VY], p.mOrigin.mV[VZ], p.mRadius, p.mMesh, p.mSpace, p.mPart,
                                                 U32(p.mBucket), p.mBridge ? 1 : 0, status_names[p.mStatus],
                                                 p.mLo.mV[VX], p.mLo.mV[VY], p.mLo.mV[VZ], p.mHi.mV[VX], p.mHi.mV[VY], p.mHi.mV[VZ]);
            }
            pj << "]}";
            const size_t steps = path.size() - 1;
            summary << (written++ ? "," : "") << llformat("{\"name\":\"%s\",\"cap\":%u,\"churn_per_step\":%.3f,\"moves_per_step\":%.3f,\"ms_per_update\":%.3f}",
                                                   strat.mName.c_str(), strat.mCap, F64(churn_total) / steps, F64(moves_total) / steps, total_ms / steps);
            std::cerr << strat.mName << ": " << primary.size() << " candidates at the primary camera, churn/step "
                      << F64(churn_total) / steps << ", ms/update " << total_ms / steps << std::endl;
        }
        summary << "]}";
    }
}
