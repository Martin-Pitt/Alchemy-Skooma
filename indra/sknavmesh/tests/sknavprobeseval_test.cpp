/**
 * @file sknavprobeseval_test.cpp
 * @brief Probe placement strategies replayed against a census export:
 *        coverage, blends across portals, the neighbour graph, and the
 *        budget's churn along a camera path. Env-gated, so the usual run
 *        skips it; name a census to evaluate with SKNAV_PROBES_REPLAY and
 *        an output directory with SKNAV_PROBES_OUT.
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

#include "../sknavprobes.h"
#include "../sknavreplay.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>

namespace
{
    using namespace sknav;
}

namespace tut
{
    struct sknavprobeseval_data
    {
    };
    typedef test_group<sknavprobeseval_data> sknavprobeseval_group;
    typedef sknavprobeseval_group::object sknavprobeseval_object;
    tut::sknavprobeseval_group sknavprobeseval_tests("sknavprobeseval");

    template<> template<>
    void sknavprobeseval_object::test<1>()
    {
        set_test_name("evaluate probe placement strategies on an exported census");
        const std::string census_path = LLStringUtil::getenv("SKNAV_PROBES_REPLAY");
        if (census_path.empty())
        {
            return;
        }
        std::string out_dir = LLStringUtil::getenv("SKNAV_PROBES_OUT");
        if (out_dir.empty())
        {
            out_dir = "V:\\Scratch\\nav-probes";
        }
        LLFile::mkdir(out_dir);

        CensusFile census;
        std::string error;
        if (!readCensusFile(census_path, census, &error))
        {
            LL_WARNS() << "census read failed: " << error << LL_ENDL;
            ensure("read the census", false);
            return;
        }

        // What the strategies place against, per mesh: the spaces as the pass
        // cut them, and the portals between them. Cached to the output
        // directory, so strategy iterations do not replay the census each
        // time; delete spaces.bin to rebuild it.
        struct WorldMesh
        {
            LLVector3d mOrigin;
            LLVector3d mMin, mMax;
            std::vector<Space> mSpaces;
            std::vector<std::tuple<U32, U32, bool>> mPortals;   // space a, space b, cross mesh
        };
        std::vector<WorldMesh> world;
        const std::string cache_path = out_dir + "\\spaces.bin";
        {
            std::ifstream cache(cache_path.c_str(), std::ios::binary);
            if (cache)
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
                        U32 a = 0, b = 0;
                        U8 cross = 0;
                        cache.read(reinterpret_cast<char*>(&a), sizeof(a));
                        cache.read(reinterpret_cast<char*>(&b), sizeof(b));
                        cache.read(reinterpret_cast<char*>(&cross), sizeof(cross));
                        wm.mPortals.emplace_back(a, b, cross != 0);
                    }
                    world.push_back(std::move(wm));
                }
                std::cerr << "world from cache: " << world.size() << " meshes" << std::endl;
            }
        }
        if (world.empty())
        {
            ReplayOptions options;
            options.mAir = AirMode::EVERYWHERE;     // the export carries only the camera's
                                                    // reach; an experiment wants every tile's air
            ReplayResult replay;
            replayCensus(census, options, replay);
            for (const auto& mesh : replay.mMeshes)
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
                        wm.mPortals.emplace_back(p.mSpaces[0], p.mSpaces[1], p.mCrossMesh);
                    }
                }
                world.push_back(std::move(wm));
            }
            std::ofstream cache(cache_path.c_str(), std::ios::binary);
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
                for (const auto& [a, b, cross] : wm.mPortals)
                {
                    cache.write(reinterpret_cast<const char*>(&a), sizeof(a));
                    cache.write(reinterpret_cast<const char*>(&b), sizeof(b));
                    const U8 cross_byte = cross ? 1 : 0;
                    cache.write(reinterpret_cast<const char*>(&cross_byte), sizeof(cross_byte));
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
        ensure("the replay holds spaces", !cells.empty());
        std::cerr << "meshes with spaces: " << meshes.size() << ", spaces: " << cells.size() << ", world "
                  << (world_max - world_min).length() << " m" << std::endl;

        const auto space_color = [](U32 id)
        {
            const U64 h = mix(SEED, id);
            return llformat("#%02x%02x%02x", S32(90.f + 130.f * ((h & 255) / 255.f)),
                            S32(90.f + 130.f * (((h >> 8) & 255) / 255.f)),
                            S32(90.f + 130.f * (((h >> 16) & 255) / 255.f)));
        };

        struct Strategy { const char* mName; F32 mShare; F32 mSpaceRadius; F32 mHold; };
        const std::vector<Strategy> strategies =
        {
            { "baseline", 1.0f, 0.f, 8.f },
            { "share", 1.5f, 0.f, 8.f },
            { "spacerad", 1.5f, 32.f, 8.f },
            { "stable", 1.5f, 32.f, 24.f },
        };

        std::ofstream metrics((out_dir + "\\metrics.json").c_str());
        metrics << "{ \"census\": \"" << census_path << "\", \"strategies\": [";

        for (size_t st = 0; st < strategies.size(); ++st)
        {
            const Strategy& strat = strategies[st];
            sknav::ProbeParams params;
            params.mCoverageShare = strat.mShare;
            params.mSpaceRadiusM = strat.mSpaceRadius;
            params.mBudgetHoldM = strat.mHold;

            // Place, per mesh and space.
            std::vector<Probe> probes;
            std::vector<std::map<U32, std::vector<U32>>> mesh_spaces(meshes.size());
            U32 spaces_probed = 0;
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

            // The neighbour graph, per mesh: two probes link when their
            // spheres intersect, as the manager's updateNeighbors has it. A
            // portal's spaces count as linked when any probe of the one meets
            // any of the other, which is what lets a blend cross the portal.
            U32 portal_pairs = 0, portal_linked = 0, portal_cross = 0, portal_cross_linked = 0;
            U32 portal_no_probe = 0, portal_far = 0;
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
                for (const auto& [sa, sb, cross] : wm.mPortals)
                {
                    if (sa >= wm.mSpaces.size() || sb >= wm.mSpaces.size())
                    {
                        continue;
                    }
                    portal_pairs += 1;
                    portal_cross += cross;
                    if (mesh_spaces[mi][sa].empty() || mesh_spaces[mi][sb].empty())
                    {
                        ++portal_no_probe;
                        continue;
                    }
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
                    portal_cross_linked += joins && cross;
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
            // it two spheres share, which is what a blend stands on.
            std::map<std::pair<S32, S32>, std::vector<U32>> buckets;
            for (size_t p = 0; p < probes.size(); ++p)
            {
                buckets[{ S32(std::floor(probes[p].mOrigin.mV[VX] / 32.f)),
                          S32(std::floor(probes[p].mOrigin.mV[VY] / 32.f)) }].push_back(static_cast<U32>(p));
            }
            U64 covered = 0, blended = 0, columns = 0;
            for (const SpaceColumns& sc : cells)
            {
                for (const LLVector3& col : sc.mColumns)
                {
                    ++columns;
                    U32 hits = 0;
                    const S32 bx = S32(std::floor(col.mV[VX] / 32.f)), by = S32(std::floor(col.mV[VY] / 32.f));
                    for (S32 oy = -1; oy <= 1 && hits < 2; ++oy)
                    {
                        for (S32 ox = -1; ox <= 1 && hits < 2; ++ox)
                        {
                            const auto found = buckets.find({ bx + ox, by + oy });
                            if (found == buckets.end()) continue;
                            for (U32 p : found->second)
                            {
                                const LLVector3 d = col - probes[p].mOrigin;
                                if (d.lengthSquared() <= probes[p].mRadius * probes[p].mRadius && ++hits == 2) break;
                            }
                        }
                    }
                    covered += hits >= 1;
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
                    std::vector<sknav::ProbeSlot> slots;
                    std::vector<LLVector3> origins;
                    for (size_t p = 0; p < probes.size(); ++p)
                    {
                        const F32 dist = std::max(dist_vec(camera, probes[p].mOrigin) - probes[p].mRadius, 0.f);
                        if (dist > reach) continue;
                        sknav::ProbeSlot slot;
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
                    const std::vector<S32> matched = sknav::matchNearest(held_origins, origins, 4.f);
                    for (auto& slot : slots)
                    {
                        slot.mHeld = matched[slot.mKey] >= 0;
                    }
                    sknav::budgetProbes(slots, cap, params);
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
                            moved += sknav::shouldReplace(held[m].mOrigin, held[m].mRadius, slot.mOrigin, slot.mRadius, params);
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
                    << ", \"spaces_probed\": " << spaces_probed
                    << ", \"coverage\": " << (columns ? 100.0 * covered / columns : 0.0)
                    << ", \"blend\": " << (columns ? 100.0 * blended / columns : 0.0)
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
                      << ", coverage " << (columns ? 100.0 * covered / columns : 0.0) << "%, blend "
                      << (columns ? 100.0 * blended / columns : 0.0) << "%, portals linked "
                      << (portal_pairs ? 100.0 * portal_linked / portal_pairs : 0.0) << "% (no probe "
                      << portal_no_probe << ", too far " << portal_far << ", cross " << portal_cross_linked
                      << "/" << portal_cross << "), links " << links
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
}
