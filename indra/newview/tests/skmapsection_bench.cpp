/**
 * @file skmapsection_bench.cpp
 * @brief The mini-map's section build on a captured census, cold and warm, with a fingerprint of what it produced.
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

// SKMAP_SECTION_CENSUS names a census file (sknav-census-*.json). The census is
// replayed into navmeshes as the viewer builds them, the scene input is gathered
// for the analysis box around the census's agent, and buildScene runs cold (a
// fresh section cache) and warm (the same cache again). The fingerprint hashes
// every section piece, the layers' floor levels and every bake cell key, so a
// change that is meant to be faster only can show it produced the same scene.
// Without the variable, or from an unoptimised build, it exits 125, which CTest
// reads as skipped.

#include "linden_common.h"

#include "../skmapgeom.h"
#include "../skmapsection.h"
#include "../skmapview.h"
#include "../sknavpolys.h"

#include "sknavcensusio.h"
#include "sknavlayout.h"
#include "sknavreplay.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using clock_type = std::chrono::steady_clock;

    F64 msSince(clock_type::time_point t0)
    {
        return std::chrono::duration<F64, std::milli>(clock_type::now() - t0).count();
    }

    F64 median(std::vector<F64> v)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    }

    U64 bits(F32 f)
    {
        U32 u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    /// Exact: float bits. Quantised: 0.1 mm, which a reordering of float sums could still move.
    struct Fingerprint
    {
        U64 mExact = sknav::SEED;
        U64 mQuant = sknav::SEED;
        void add(U64 v)
        {
            mExact = sknav::mix(mExact, v);
            mQuant = sknav::mix(mQuant, v);
        }
        void addF(F32 f)
        {
            mExact = sknav::mix(mExact, bits(f));
            mQuant = sknav::mix(mQuant, static_cast<U64>(std::llround(static_cast<F64>(f) * 1.0e4)));
        }
    };

    Fingerprint fingerprint(const skmap::Scene& s)
    {
        Fingerprint fp;
        fp.add(s.mPieces.size());
        for (const skmap::SectionPiece& p : s.mPieces)
        {
            fp.add(static_cast<U64>(static_cast<S64>(p.mPlane)));
            fp.add(p.mFlags);
            fp.add(static_cast<U64>(p.mStyle));
            fp.add(p.mGroup);
            fp.add(p.mSource);
            fp.addF(p.mThickness);
            fp.addF(p.mMin.mV[VX]);
            fp.addF(p.mMin.mV[VY]);
            fp.addF(p.mMax.mV[VX]);
            fp.addF(p.mMax.mV[VY]);
            const skmap::SliceResult& sl = *p.mSlice;
            auto run = [&](const skmap::SliceLoop& l) {
                fp.add(l.mCount);
                for (U32 k = 0; k < l.mCount; ++k)
                {
                    const LLVector2 q = sl.mPts[l.mFirst + k] + p.mShift;
                    fp.addF(q.mV[VX]);
                    fp.addF(q.mV[VY]);
                }
            };
            for (size_t i = 0; i < p.mLoops.size(); ++i)
            {
                fp.add(static_cast<U64>(static_cast<S64>(i < p.mWind.size() ? p.mWind[i] : 0)));
                run(sl.mLoops[p.mLoops[i]]);
            }
            for (U16 id : p.mLines)
            {
                run(id < sl.mLines.size() ? sl.mLines[id] : sl.mLoops[id - sl.mLines.size()]);
            }
        }
        for (const skmap::SceneLayer& layer : s.mLayers)
        {
            fp.add(layer.mPieces.size());
            for (U32 id : layer.mPieces)
            {
                fp.add(id);
            }
            const skmap::LevelGrid& g = layer.mLevels;
            fp.add(static_cast<U64>(static_cast<S64>(g.mX0)) ^ (static_cast<U64>(static_cast<S64>(g.mY0)) << 32));
            fp.add(static_cast<U64>(g.mW) | (static_cast<U64>(g.mH) << 32));
            for (U32 c : g.mCell)
            {
                fp.add(c);
            }
            for (S32 k : g.mLists)
            {
                fp.add(static_cast<U64>(static_cast<S64>(k)));
            }
        }
        for (const auto& cells : s.mCellHashes)
        {
            fp.add(cells.size());
            for (const auto& entry : cells)
            {
                fp.add(static_cast<U64>(static_cast<S64>(entry.first.mTier)));
                fp.add(static_cast<U64>(static_cast<S64>(entry.first.mX)));
                fp.add(static_cast<U64>(static_cast<S64>(entry.first.mY)));
                fp.add(entry.second);
            }
        }
        return fp;
    }

    /// The scene input the viewer would gather from these navmeshes and this census.
    void gatherInput(const sknav::CensusFile& census, const sknav::ReplayResult& replay, skmap::SceneInput& in)
    {
        const S32 c = static_cast<S32>(sknav::AgentClass::AVATAR);
        for (const auto& mesh : replay.mMeshes)
        {
            for (const auto& entry : mesh->mTiles)
            {
                const sknav::TileKey& key = entry.first;
                const sknav::ReplayTile& tile = entry.second;
                if (!tile.mResult || tile.mResult->mStatus != sknav::TileBuildResult::SUCCESS
                    || !sknav::overlapsXY(tile.mMin, tile.mMax, in.mMin, in.mMax))
                {
                    continue;
                }
                // Detour's x runs along Recast x, which is SL y.
                const dtMeshTile* dt = mesh->mNav[c] ? mesh->mNav[c]->getTileAt(key.mY, key.mX, 0) : nullptr;
                if (!dt || !dt->header)
                {
                    continue;
                }
                skmap::TileInput t;
                t.mMesh = mesh->mId;
                t.mTerrain = mesh->mTerrain;
                t.mOrigin = mesh->mOrigin;
                t.mKey = key;
                t.mMin = tile.mMin;
                t.mMax = tile.mMax;
                const LLVector3d mid = (tile.mMin + tile.mMax) * 0.5;
                if (const sknav::CensusRegion* region = census.regionAt(mid.mdV[VX], mid.mdV[VY]))
                {
                    t.mRegion = region->mHandle;
                    t.mWater = region->mWater;
                }
                t.mContentKey = sknav::mix(sknav::mix(sknav::SEED, key.mX), key.mY);
                t.mPolys = sknav::extractTilePolys(*dt);
                t.mSegments = std::make_shared<const std::vector<sknav::WallSegment>>(tile.mResult->mSegments[c]);
                t.mLinks = std::make_shared<const std::vector<sknav::DropLink>>(tile.mResult->mLinks[c]);
                t.mFlights = std::make_shared<const std::vector<sknav::Flight>>(tile.mResult->mFlights[c]);
                in.mTiles.push_back(t);
            }
        }
        for (const sknav::CensusSpan& span : census.mSpans)
        {
            if (span.mKind != sknav::LinkKind::LADDER && span.mKind != sknav::LinkKind::ELEVATOR)
            {
                continue;
            }
            for (const auto& link : span.mLinks[c])
            {
                in.mFeatureLinks.push_back({ link.first, link.second, span.mKind });
            }
        }
        for (const sknav::DoorEntity& door : census.mDoors)
        {
            if (sknav::overlapsXY(door.mMin, door.mMax, in.mMin, in.mMax) && door.mMin.mdV[VZ] <= in.mMax.mdV[VZ]
                && in.mMin.mdV[VZ] <= door.mMax.mdV[VZ])
            {
                skmap::DoorInput d;
                d.mMin = door.mMin;
                d.mMax = door.mMax;
                d.mPlanePoint = door.mPlanePoint;
                d.mPlaneKnown = door.mPlaneKnown;
                d.mNormal = door.mNormal;
                d.mWidth = door.mWidth;
                d.mSill = door.mSill;
                d.mDouble = door.mDouble;
                d.mLeaves = door.mLeaves;
                in.mDoors.push_back(d);
            }
        }
        for (const sknav::CensusRegion& region : census.mRegions)
        {
            in.mRegions.push_back({ region.mOrigin, static_cast<F64>(region.mWidth) });
        }
        in.mRecords = std::shared_ptr<const std::vector<sknav::RecordPtr>>(std::shared_ptr<const std::vector<sknav::RecordPtr>>(),
                                                                           &census.mRecords);
    }

    const char* stageName(skmap::SectionStage stage)
    {
        static const char* names[] = { "levels", "pairs", "slice (misses)", "  z-bins", "  analytic shapes", "  soup segments",
                                       "  chain", "    of it pass 3", "  simplify", "  nesting", "sheet rooms", "islands" };
        return names[static_cast<size_t>(stage)];
    }

    void printProfile(const char* label, F64 section_ms)
    {
        const skmap::SectionProfile& p = skmap::sectionProfile();
        std::printf("%s stages (profiled, section %.2f ms):\n", label, section_ms);
        for (size_t i = 0; i < static_cast<size_t>(skmap::SectionStage::COUNT); ++i)
        {
            std::printf("  %-20s %8.2f ms  %8llu calls\n", stageName(static_cast<skmap::SectionStage>(i)), p.mMs[i],
                        static_cast<unsigned long long>(p.mCalls[i]));
        }
    }
}

int main(int, char**)
{
#ifndef NDEBUG
    std::printf("skipped: numbers from an unoptimised build say nothing\n");
    return 125;
#endif
    const char* path = std::getenv("SKMAP_SECTION_CENSUS");
    if (!path || !*path)
    {
        std::printf("skipped: set SKMAP_SECTION_CENSUS to a census file\n");
        return 125;
    }
    const char* runs_env = std::getenv("SKMAP_SECTION_RUNS");
    const S32 runs = runs_env ? llmax(1, std::atoi(runs_env)) : 7;

    auto t0 = clock_type::now();
    sknav::CensusFile census;
    std::string err;
    if (!sknav::readCensusFile(path, census, &err))
    {
        std::printf("cannot read %s: %s\n", path, err.c_str());
        return 1;
    }
    std::printf("census %s: %zu records, %zu doors, read in %.0f ms\n", path, census.mRecords.size(), census.mDoors.size(), msSince(t0));
    t0 = clock_type::now();
    sknav::ReplayOptions options;
    sknav::ReplayResult replay;
    sknav::replayCensus(census, options, replay);
    std::printf("replay: %zu meshes, %u tiles in %.0f ms\n", replay.mMeshes.size(), replay.mTiles, msSince(t0));

    skmap::SceneInput in;
    skmap::analysisBox(census.mAgent, census.mAgent, 1.f, 1.f, in.mMin, in.mMax);
    in.mOrigin.set(in.mMin.mdV[VX], in.mMin.mdV[VY], 0.0);
    gatherInput(census, replay, in);
    std::printf("input: box %.0f x %.0f m, %zu tiles, %zu feature links, %zu doors\n", in.mMax.mdV[VX] - in.mMin.mdV[VX],
                in.mMax.mdV[VY] - in.mMin.mdV[VY], in.mTiles.size(), in.mFeatureLinks.size(), in.mDoors.size());

    // Cold: a fresh cache each run, as after a teleport. Warm: the same cache again, as a box re-snap.
    std::vector<F64> cold_total, cold_section, warm_total, warm_section;
    skmap::ScenePtr scene;
    Fingerprint fp_cold, fp_warm;
    skmap::SectionStats cold_stats, warm_stats;
    for (S32 r = 0; r < runs; ++r)
    {
        skmap::SectionCache cache;
        t0 = clock_type::now();
        scene = skmap::buildScene(in, &cache);
        cold_total.push_back(msSince(t0));
        cold_section.push_back(scene->mSectionStats.mMs);
        cold_stats = scene->mSectionStats;
        if (r == 0)
        {
            fp_cold = fingerprint(*scene);
        }
        t0 = clock_type::now();
        scene = skmap::buildScene(in, &cache);
        warm_total.push_back(msSince(t0));
        warm_section.push_back(scene->mSectionStats.mMs);
        warm_stats = scene->mSectionStats;
        if (r == 0)
        {
            fp_warm = fingerprint(*scene);
        }
    }
    std::printf("scene: %zu polys, %zu layers, %u records, %u planes, %u pieces\n", scene->mPolys.size(), scene->mLayers.size(),
                cold_stats.mRecords, cold_stats.mPlanes, cold_stats.mPieces);
    std::printf("cold: section %.2f ms (median of %d), scene %.2f ms; %u slices, %u hits\n", median(cold_section), runs,
                median(cold_total), cold_stats.mSlices, cold_stats.mCacheHits);
    std::printf("warm: section %.2f ms (median of %d), scene %.2f ms; %u slices, %u hits\n", median(warm_section), runs,
                median(warm_total), warm_stats.mSlices, warm_stats.mCacheHits);
    std::printf("fingerprint cold exact %016llx quantised %016llx\n", static_cast<unsigned long long>(fp_cold.mExact),
                static_cast<unsigned long long>(fp_cold.mQuant));
    std::printf("fingerprint warm exact %016llx quantised %016llx\n", static_cast<unsigned long long>(fp_warm.mExact),
                static_cast<unsigned long long>(fp_warm.mQuant));

    // Where the time goes; the clocks themselves cost a little, so these runs are kept out of the medians.
    skmap::SectionProfile& profile = skmap::sectionProfile();
    {
        skmap::SectionCache cache;
        profile = skmap::SectionProfile();
        profile.mOn = true;
        scene = skmap::buildScene(in, &cache);
        profile.mOn = false;
        printProfile("cold", scene->mSectionStats.mMs);
        profile = skmap::SectionProfile();
        profile.mOn = true;
        scene = skmap::buildScene(in, &cache);
        profile.mOn = false;
        printProfile("warm", scene->mSectionStats.mMs);
    }
    // The capture is valid only while the census is: release the scene before it.
    scene.reset();
    in.mRecords.reset();
    return fp_cold.mExact == fp_warm.mExact ? 0 : 2;
}
