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
// SKMAP_SECTION_EXPECT, when set, is the exact fingerprint the build must give;
// a mismatch exits 3. A third build on the warm cache moves one static box: it must
// give what a cold build of that input gives, or the bench exits 4.
// Without the census variable, or from an unoptimised build,
// it exits 125, which CTest reads as skipped.

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
#include <cfloat>
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

    F64 least(const std::vector<F64>& v)
    {
        return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
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
        /// The pieces' parts and roots, kept out of the two above so the captures' expected values still hold.
        U64 mIds = sknav::SEED;
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
            U64 ids[4];
            std::memcpy(ids, p.mPart.mData, sizeof(p.mPart.mData));
            std::memcpy(ids + 2, p.mRoot.mData, sizeof(p.mRoot.mData));
            for (U64 v : ids)
            {
                fp.mIds = sknav::mix(fp.mIds, v);
            }
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
        static const char* names[] = { "filter and buckets", "layer indexes", "last build's layers", "  changed layers", "levels", "  samples",
                                       "  spread", "  pack", "pairs", "  sort", "pieces", "  reused", "  slice", "    z-bins", "    analytic shapes", "    soup segments", "    chain",
                                       "      nodes", "      snap", "      walk", "      close", "    simplify", "    nesting",
                                       "  sheet rooms", "  islands", "cell hashes (scene)" };
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(skmap::SectionStage::COUNT), "a name per stage");
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

    // A census restamp that moved one thing: the static box nearest the agent that reaches its storey, half a metre
    // east, re-signed.
    skmap::SceneInput moved = in;
    bool moved_one = false;
    {
        auto records = std::make_shared<std::vector<sknav::RecordPtr>>(census.mRecords);
        sknav::RecordPtr* nearest = nullptr;
        F64 best = DBL_MAX;
        for (sknav::RecordPtr& rec : *records)
        {
            if (!rec || rec->mShape != sknav::Shape::BOX || rec->mRole != sknav::Role::NAV || rec->mDynamic
                || rec->mMax.mdV[VZ] < census.mAgent.mdV[VZ] || rec->mMin.mdV[VZ] > census.mAgent.mdV[VZ] + 2.0)
            {
                continue;
            }
            const LLVector3d mid = (rec->mMin + rec->mMax) * 0.5;
            const F64 d = (mid.mdV[VX] - census.mAgent.mdV[VX]) * (mid.mdV[VX] - census.mAgent.mdV[VX])
                        + (mid.mdV[VY] - census.mAgent.mdV[VY]) * (mid.mdV[VY] - census.mAgent.mdV[VY]);
            if (d < best)
            {
                best = d;
                nearest = &rec;
            }
        }
        if (nearest)
        {
            sknav::Record copy = **nearest;
            copy.mCenter.mV[VX] += 0.5f;
            copy.mMin.mdV[VX] += 0.5;
            copy.mMax.mdV[VX] += 0.5;
            copy.mSignature ^= 0x5eed5eed5eed5eedull;
            *nearest = std::make_shared<const sknav::Record>(copy);
            moved_one = true;
            std::printf("moved: part %s, %.1f x %.1f x %.1f m at (%.1f, %.1f, %.1f) global\n", copy.mPart.asString().c_str(),
                        copy.mMax.mdV[VX] - copy.mMin.mdV[VX], copy.mMax.mdV[VY] - copy.mMin.mdV[VY], copy.mMax.mdV[VZ] - copy.mMin.mdV[VZ],
                        copy.mMin.mdV[VX], copy.mMin.mdV[VY], copy.mMin.mdV[VZ]);
        }
        moved.mRecords = records;
    }
    // The agent one snap step east, so the box and the scene origin move with it.
    skmap::SceneInput shifted;
    {
        LLVector3d agent = census.mAgent;
        agent.mdV[VX] += skmap::ANALYSIS_SNAP_M;
        skmap::analysisBox(agent, agent, 1.f, 1.f, shifted.mMin, shifted.mMax);
        shifted.mOrigin = skmap::analysisOrigin(shifted.mMin, &in.mOrigin);
        gatherInput(census, replay, shifted);
    }

    // Cold: a fresh cache each run, as after a teleport. Warm: the same input on the same cache, as a census restamp
    // or navmesh tile rebuild that changed nothing in the box; every layer is the last build's. Moved: the same cache
    // once more, with the one record moved. Shifted: the box one snap step east on that cache, as the agent crossing
    // a 64 m line or the map panning; the scene origin stays, as the viewer keeps it, and records and polygons come
    // and go along the box's edges.
    std::vector<F64> cold_total, cold_section, warm_total, warm_section, moved_section, shifted_section;
    skmap::ScenePtr scene;
    Fingerprint fp_cold, fp_warm, fp_moved, fp_shifted;
    skmap::SectionStats cold_stats, warm_stats, moved_stats, shifted_stats;
    size_t cache_bytes = 0, cache_slices = 0, cold_polys = 0, cold_layers = 0;
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
            cold_polys = scene->mPolys.size();
            cold_layers = scene->mLayers.size();
            cache_bytes = cache.bytes();
            cache_slices = cache.entries();
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
        scene = skmap::buildScene(moved, &cache);
        moved_section.push_back(scene->mSectionStats.mMs);
        moved_stats = scene->mSectionStats;
        if (r == 0)
        {
            fp_moved = fingerprint(*scene);
        }
        // Back to the unmoved records, untimed, so the shift is measured from a warm build of the same box.
        scene = skmap::buildScene(in, &cache);
        scene = skmap::buildScene(shifted, &cache);
        shifted_section.push_back(scene->mSectionStats.mMs);
        shifted_stats = scene->mSectionStats;
        if (r == 0)
        {
            fp_shifted = fingerprint(*scene);
        }
    }
    // What the moved and shifted inputs build from nothing: the warm builds must have built the same.
    Fingerprint fp_moved_cold, fp_shifted_cold;
    {
        skmap::SectionCache fresh;
        fp_moved_cold = fingerprint(*skmap::buildScene(moved, &fresh));
    }
    {
        skmap::SectionCache fresh;
        fp_shifted_cold = fingerprint(*skmap::buildScene(shifted, &fresh));
    }
    std::printf("scene: %zu polys, %zu layers, %u records, %u planes, %u pairs, %u pieces\n", cold_polys, cold_layers,
                cold_stats.mRecords, cold_stats.mPlanes, cold_stats.mPairs, cold_stats.mPieces);
    // The minimum is what the code costs; the median shows how far other load on the machine moved it.
    std::printf("cold: section %.2f ms (min of %d, median %.2f), scene %.2f ms; %u slices, %u hits\n", least(cold_section), runs,
                median(cold_section), least(cold_total), cold_stats.mSlices, cold_stats.mCacheHits);
    std::printf("warm: section %.2f ms (min of %d, median %.2f), scene %.2f ms; %u slices, %u hits, %u pairs reused\n",
                least(warm_section), runs, median(warm_section), least(warm_total), warm_stats.mSlices, warm_stats.mCacheHits,
                warm_stats.mReused);
    if (moved_one)
    {
        std::printf("one record moved: section %.2f ms (min of %d, median %.2f); %u slices, %u hits, %u pairs reused, %u layers paired; "
                    "%s a cold build\n",
                    least(moved_section), runs, median(moved_section), moved_stats.mSlices, moved_stats.mCacheHits, moved_stats.mReused,
                    moved_stats.mLayersPaired, fp_moved.mExact == fp_moved_cold.mExact ? "the same as" : "DIFFERENT from");
    }
    else
    {
        std::printf("one record moved: NONE, no static box reaches the agent's storey\n");
    }
    std::printf("box shifted one snap step: section %.2f ms (min of %d, median %.2f); %u slices, %u hits, %u of %u pairs reused, "
                "%u of %u layers' floors kept, %u in part, %u layers paired; %s a cold build\n",
                least(shifted_section), runs, median(shifted_section), shifted_stats.mSlices, shifted_stats.mCacheHits, shifted_stats.mReused,
                shifted_stats.mPairs, shifted_stats.mLayersKept, shifted_stats.mLayers, shifted_stats.mLayersNear, shifted_stats.mLayersPaired,
                fp_shifted.mExact == fp_shifted_cold.mExact ? "the same as" : "DIFFERENT from");
    // Re-snaps, untimed, as the one step east may leave every layer whole: one step each way from a warm build, then on
    // to four, the farthest the origin is kept, and each against a cold build of its box.
    U32 sweep_near = 0;
    bool sweep_same = true;
    {
        const std::pair<F64, F64> dirs[] = { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 } };
        for (const auto& dir : dirs)
        {
            skmap::SectionCache cache;
            skmap::buildScene(in, &cache);
            LLVector3d origin = in.mOrigin;
            for (const S32 k : { 1, 4 })
            {
                LLVector3d agent = census.mAgent;
                agent.mdV[VX] += dir.first * k * skmap::ANALYSIS_SNAP_M;
                agent.mdV[VY] += dir.second * k * skmap::ANALYSIS_SNAP_M;
                skmap::SceneInput step;
                skmap::analysisBox(agent, agent, 1.f, 1.f, step.mMin, step.mMax);
                step.mOrigin = skmap::analysisOrigin(step.mMin, &origin);
                origin = step.mOrigin;
                gatherInput(census, replay, step);
                const skmap::ScenePtr warm = skmap::buildScene(step, &cache);
                const skmap::SectionStats st = warm->mSectionStats;
                const Fingerprint fp_step = fingerprint(*warm);
                skmap::SectionCache fresh;
                const Fingerprint fp_step_cold = fingerprint(*skmap::buildScene(step, &fresh));
                const bool same = fp_step.mExact == fp_step_cold.mExact && fp_step.mIds == fp_step_cold.mIds;
                sweep_same = sweep_same && same;
                sweep_near += st.mLayersNear;
                std::printf("re-snap (%+.0f, %+.0f) m: section %.2f ms; %u slices, %u of %u pairs reused, %u of %u layers' floors kept, "
                            "%u in part, %u layers paired; %s a cold build\n",
                            dir.first * k * skmap::ANALYSIS_SNAP_M, dir.second * k * skmap::ANALYSIS_SNAP_M, st.mMs, st.mSlices,
                            st.mReused, st.mPairs, st.mLayersKept, st.mLayers, st.mLayersNear, st.mLayersPaired,
                            same ? "the same as" : "DIFFERENT from");
            }
        }
    }
    std::printf("cache after cold: %zu slices, %.2f MB\n", cache_slices, cache_bytes / (1024.0 * 1024.0));
    std::printf("fingerprint cold exact %016llx quantised %016llx\n", static_cast<unsigned long long>(fp_cold.mExact),
                static_cast<unsigned long long>(fp_cold.mQuant));
    std::printf("fingerprint warm exact %016llx quantised %016llx\n", static_cast<unsigned long long>(fp_warm.mExact),
                static_cast<unsigned long long>(fp_warm.mQuant));

    if (std::getenv("SKMAP_SECTION_LAYERS"))
    {
        size_t cells = 0;
        for (size_t l = 0; l < scene->mLayers.size(); ++l)
        {
            const skmap::SceneLayer& layer = scene->mLayers[l];
            const size_t n = static_cast<size_t>(layer.mLevels.mW) * layer.mLevels.mH;
            cells += n;
            size_t listed = 0;
            for (U32 c : layer.mLevels.mCell)
            {
                listed += c ? 1 : 0;
            }
            if (n > 20000 || layer.mPieces.size() > 2000)
            {
                std::printf("layer %3zu: %5zu polys, grid %4d x %4d (%zu cells, %zu listed), %zu pieces\n", l, layer.mPolys.size(),
                            layer.mLevels.mW, layer.mLevels.mH, n, listed, layer.mPieces.size());
            }
        }
        std::printf("grid cells over all layers: %zu\n", cells);
    }
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
        profile = skmap::SectionProfile();
        profile.mOn = true;
        scene = skmap::buildScene(moved, &cache);
        profile.mOn = false;
        printProfile("moved", scene->mSectionStats.mMs);
        scene = skmap::buildScene(in, &cache);
        profile = skmap::SectionProfile();
        profile.mOn = true;
        scene = skmap::buildScene(shifted, &cache);
        profile.mOn = false;
        printProfile("shifted", scene->mSectionStats.mMs);
    }
    // The capture is valid only while the census is: release the scene before it.
    scene.reset();
    in.mRecords.reset();
    moved.mRecords.reset();
    shifted.mRecords.reset();
    // The memo compares parts and roots: a reuse that kept a stale one shows here.
    if (fp_cold.mIds != fp_warm.mIds || fp_moved.mIds != fp_moved_cold.mIds || fp_shifted.mIds != fp_shifted_cold.mIds)
    {
        std::printf("part or root ids differ from a cold build\n");
        return 7;
    }
    if (fp_cold.mExact != fp_warm.mExact)
    {
        return 2;
    }
    if (fp_moved.mExact != fp_moved_cold.mExact)
    {
        return 4;
    }
    // Nothing moved would make the moved build a second warm build, which proves nothing about invalidation.
    if (!moved_one)
    {
        return 5;
    }
    if (fp_shifted.mExact != fp_shifted_cold.mExact)
    {
        return 6;
    }
    if (!sweep_same)
    {
        return 8;
    }
    // No re-snap that took a layer in part would leave the reuse across a re-snap untested on this capture.
    if (sweep_near == 0)
    {
        std::printf("no re-snap took a layer in part\n");
        return 9;
    }
    const char* expect = std::getenv("SKMAP_SECTION_EXPECT");
    if (expect && *expect && std::strtoull(expect, nullptr, 16) != fp_cold.mExact)
    {
        std::printf("fingerprint differs from SKMAP_SECTION_EXPECT %s\n", expect);
        return 3;
    }
    return 0;
}
