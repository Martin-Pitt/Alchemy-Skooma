/**
 * @file sknavreplay.cpp
 * @brief A census replayed headless: clusters, meshes and tiles, the tile builds, then enclosure, spaces and wind per mesh.
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

#include "sknavreplay.h"

#include "sknavexport.h"
#include "sknavlayout.h"

#include "llformat.h"

#include "DetourAlloc.h"
#include "DetourNavMesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace sknav
{
    namespace
    {
        typedef std::chrono::steady_clock Clock;

        F64 since(Clock::time_point t0)
        {
            return std::chrono::duration<F64, std::milli>(Clock::now() - t0).count();
        }

        /// The land under a point, from the nearest sample: good enough to
        /// ground a cluster the viewer's export did not.
        bool landUnder(const CensusFile& census, F64 x, F64 y, F32& z)
        {
            const CensusRegion* r = census.regionAt(x, y);
            if (!r || !r->mSamples)
            {
                return false;
            }
            const S32 ix = std::clamp(static_cast<S32>(std::lround(x - r->mOrigin.mdV[VX] - r->mPhase)), 0, r->mSamples - 1);
            const S32 iy = std::clamp(static_cast<S32>(std::lround(y - r->mOrigin.mdV[VY] - r->mPhase)), 0, r->mSamples - 1);
            z = r->mZ[static_cast<size_t>(iy) * r->mSamples + ix];
            return true;
        }

        bool groundedFallback(const CensusFile& census, const Cluster& c)
        {
            const LLVector3d corners[5] = {
                c.mMin,
                LLVector3d(c.mMax.mdV[VX], c.mMin.mdV[VY], 0.0),
                LLVector3d(c.mMin.mdV[VX], c.mMax.mdV[VY], 0.0),
                c.mMax,
                (c.mMin + c.mMax) * 0.5,
            };
            F32 land = -1e9f;
            for (const LLVector3d& p : corners)
            {
                F32 z;
                if (landUnder(census, p.mdV[VX], p.mdV[VY], z))
                {
                    land = std::max(land, z);
                }
            }
            return static_cast<F32>(c.mMin.mdV[VZ]) - land <= census.mGroundedMargin;
        }

        bool touches(const ReplayMesh& a, const ReplayMesh& b)
        {
            // A metre of slack: meshes whose tiles only meet at an edge still hand air across it.
            return a.mAirMin.mdV[VX] <= b.mAirMax.mdV[VX] + 1.0 && b.mAirMin.mdV[VX] <= a.mAirMax.mdV[VX] + 1.0 &&
                   a.mAirMin.mdV[VY] <= b.mAirMax.mdV[VY] + 1.0 && b.mAirMin.mdV[VY] <= a.mAirMax.mdV[VY] + 1.0;
        }

        /// SKNavMesh::inAirRadius: the tile's footprint within the radius of the camera.
        bool inAirRadius(const CensusFile& census, const LLVector3d& tmin, const LLVector3d& tmax)
        {
            const LLVector3d& camera = census.mCamera;
            const F64 dx = std::max({ tmin.mdV[VX] - camera.mdV[VX], 0.0, camera.mdV[VX] - tmax.mdV[VX] });
            const F64 dy = std::max({ tmin.mdV[VY] - camera.mdV[VY], 0.0, camera.mdV[VY] - tmax.mdV[VY] });
            return dx * dx + dy * dy <= static_cast<F64>(census.mAirRadius) * census.mAirRadius;
        }

        /// The viewer's launch: air near the camera only, and coarse air past it.
        void applyAirMode(const CensusFile& census, const ReplayOptions& options, ReplayTile& tile, TileBuildInput& input)
        {
            input.mFaceOffsets = options.mFaceOffsets;
            input.mAlignWallEdges = options.mAlignWallEdges;
            if (options.mAir == AirMode::RADIUS && input.mAir && !inAirRadius(census, tile.mMin, tile.mMax))
            {
                input.mAir = false;
                input.mCoarseAir = options.mCoarseAir && options.mStamp;
            }
        }

        /// A Detour tile into a mesh's navmesh, which the navmesh then owns.
        bool addDetour(dtNavMesh*& nav, const std::vector<U8>& data, U32 user_id = 0)
        {
            if (data.empty() || (!nav && !(nav = allocMeshNav())))
            {
                return false;
            }
            unsigned char* copy = static_cast<unsigned char*>(dtAlloc(static_cast<int>(data.size()), DT_ALLOC_PERM));
            std::memcpy(copy, data.data(), data.size());
            if (user_id && data.size() >= sizeof(dtMeshHeader))
            {
                reinterpret_cast<dtMeshHeader*>(copy)->userId = user_id;
            }
            if (!dtStatusSucceed(nav->addTile(copy, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, nullptr)))
            {
                dtFree(copy);
                return false;
            }
            return true;
        }

        /// A binding's flags onto the tile's polys, past their area bits, as the viewer's publish sets them.
        void applyBinding(dtNavMesh& nav, const TileKey& key, const TileBinding& binding)
        {
            const dtMeshTile* tile = nav.getTileAt(key.mY, key.mX, 0);
            if (!tile || !tile->header)
            {
                return;
            }
            const dtPolyRef base = nav.getPolyRefBase(tile);
            for (S32 p = 0; p < tile->header->polyCount && p < static_cast<S32>(binding.mPolyFlags.size()); ++p)
            {
                const unsigned short flags = tile->polys[p].flags;
                if (flags)
                {
                    nav.setPolyFlags(base | static_cast<dtPolyRef>(p), static_cast<unsigned short>((flags & ~POLY_BINDING_FLAGS) | binding.mPolyFlags[p]));
                }
            }
        }

        bool hasAir(const ReplayMesh& m)
        {
            return m.mAirMin.mdV[VX] <= m.mAirMax.mdV[VX];
        }

        LLUUID featureId(const Feature& feature)
        {
            return feature.mRoots.empty() ? LLUUID::null : *std::min_element(feature.mRoots.begin(), feature.mRoots.end());
        }
    }

    U32 ReplayMesh::polySpace(const dtMeshTile* tile, U32 poly) const
    {
        if (!tile || !tile->header)
        {
            return 0;
        }
        const auto it = mSlotSpaces.find(tile->header->userId);
        return it != mSlotSpaces.end() && poly < it->second.size() ? it->second[poly] : 0;
    }

    SpaceQueryFilter::TileSpaces ReplayMesh::polySpaces(const dtMeshTile* tile) const
    {
        const auto it = tile && tile->header ? mSlotSpaces.find(tile->header->userId) : mSlotSpaces.end();
        return it == mSlotSpaces.end() ? SpaceQueryFilter::TileSpaces() : SpaceQueryFilter::TileSpaces{ it->second.data(), static_cast<U32>(it->second.size()) };
    }

    const Flight* ReplayMesh::polyFlight(const dtMeshTile* tile, U32 poly) const
    {
        if (!tile || !tile->header)
        {
            return nullptr;
        }
        const auto it = mSlotFlights.find(tile->header->userId);
        return it != mSlotFlights.end() && poly < it->second.size() ? it->second[poly] : nullptr;
    }

    ReplayMesh::~ReplayMesh()
    {
        if (mUnstampedNav)
        {
            dtFreeNavMesh(mUnstampedNav);
            mUnstampedNav = nullptr;
        }
        for (dtNavMesh*& nav : mNav)
        {
            if (nav)
            {
                dtFreeNavMesh(nav);
                nav = nullptr;
            }
        }
    }

    void stampReplay(ReplayResult& out, const ReplayOptions& options);

    void replayCensus(const CensusFile& census, const ReplayOptions& options, ReplayResult& out)
    {
        out = ReplayResult();
        std::map<LLUUID, U8> cues;
        for (const CensusRoot& root : census.mRoots)
        {
            cues[root.mId] = nameCue(root.mName, root.mDescription);
        }
        const auto cue_of = [&cues](const LLUUID& id) {
            const auto it = cues.find(id);
            return it == cues.end() ? static_cast<U8>(CUE_NONE) : it->second;
        };

        // Clusters, and the viewer's against ours.
        std::vector<RegionSquare> squares;
        for (const CensusRegion& region : census.mRegions)
        {
            squares.push_back({ region.mOrigin, region.mWidth });
        }
        Clock::time_point t0 = Clock::now();
        out.mClusters = cluster(census.mRecords, census.mClusterGap, TILE_M, regionWindow(squares, census.mVoidMargin));
        out.mStages.emplace_back("cluster", since(t0));
        std::unordered_map<const Record*, U32> index;
        for (size_t i = 0; i < census.mRecords.size(); ++i)
        {
            index[census.mRecords[i].get()] = static_cast<U32>(i);
        }
        std::map<LLUUID, const CensusCluster*> theirs;
        for (const CensusCluster& c : census.mClusters)
        {
            theirs[c.mId] = &c;
        }
        std::set<LLUUID> seen;
        for (const ClusterPtr& c : out.mClusters.mClusters)
        {
            seen.insert(c->mId);
            const auto it = theirs.find(c->mId);
            if (it == theirs.end())
            {
                ++out.mClustersOnlyReplay;
                out.mClusterNotes.push_back("only the replay has " + c->mId.asString());
                continue;
            }
            std::vector<U32> mine;
            for (const RecordPtr& r : c->mRecords)
            {
                mine.push_back(index[r.get()]);
            }
            std::vector<U32> other = it->second->mRecords;
            std::sort(mine.begin(), mine.end());
            std::sort(other.begin(), other.end());
            if (mine == other)
            {
                ++out.mClustersSame;
            }
            else
            {
                ++out.mClustersDiffer;
                out.mClusterNotes.push_back(llformat("%s: %zu members here, %zu in the viewer", c->mId.asString().c_str(), mine.size(), other.size()));
            }
        }
        for (const CensusCluster& c : census.mClusters)
        {
            if (!seen.count(c.mId))
            {
                ++out.mClustersOnlyViewer;
                out.mClusterNotes.push_back("only the viewer has " + c.mId.asString());
            }
        }

        // Meshes and tiles, as SKNavMesh::reconcile lays them out.
        t0 = Clock::now();
        const TileBuildInput& params = census.mParams;
        const F32 pad = tilePad(params);
        std::map<LLUUID, std::unique_ptr<ReplayMesh>> meshes;
        std::vector<RecordPtr> ground_records;
        std::vector<const Cluster*> airborne;
        for (const ClusterPtr& c : out.mClusters.mClusters)
        {
            const auto it = theirs.find(c->mId);
            const bool grounded = it != theirs.end() ? it->second->mGrounded : groundedFallback(census, *c);
            if (grounded)
            {
                ground_records.insert(ground_records.end(), c->mRecords.begin(), c->mRecords.end());
            }
            else
            {
                airborne.push_back(c.get());
            }
        }
        std::vector<LaidTile> laid;
        for (const Cluster* c : airborne)
        {
            auto mesh = std::make_unique<ReplayMesh>();
            mesh->mId = c->mId;
            mesh->mOrigin = latticeOrigin(c->mMin);
            if (!layAirborne(*c, mesh->mOrigin, pad, laid))
            {
                ++out.mRefused;
                out.mClusterNotes.push_back(llformat("%s: refused, %zu tiles", c->mId.asString().c_str(), c->mTiles.size()));
            }
            for (LaidTile& lt : laid)
            {
                ReplayTile& tile = mesh->mTiles[lt.mKey];
                tile.mKey = lt.mKey;
                tile.mMin = meshTileMin(mesh->mOrigin, lt.mKey);
                tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
                tile.mMin.mdV[VZ] = c->mMin.mdV[VZ];
                tile.mMax.mdV[VZ] = c->mMax.mdV[VZ];
                auto input = std::make_shared<TileBuildInput>(params);
                input->mRecords.clear();
                input->mCuts.clear();
                input->mMeshOrigin = mesh->mOrigin;
                input->mTileX = lt.mKey.mX;
                input->mTileY = lt.mKey.mY;
                input->mTileSize = static_cast<F32>(TILE_M);
                applyAirMode(census, options, tile, *input);
                finishTileInput(lt.mRecords, mesh->mOrigin, *input);
                tileRootCues(cue_of, *input);
                tile.mInput = input;
            }
            meshes[mesh->mId] = std::move(mesh);
        }
        F64 terrain_ms = 0.0;
        for (const CensusRegion& region : census.mRegions)
        {
            auto mesh = std::make_unique<ReplayMesh>();
            mesh->mId = region.mId;
            mesh->mTerrain = true;
            mesh->mOrigin = latticeOrigin(region.mOrigin);
            if (!layRegion(region.mOrigin, region.mWidth, mesh->mOrigin, ground_records, pad, census.mVoidMargin, squares, laid))
            {
                ++out.mRefused;
                out.mClusterNotes.push_back("region " + region.mId.asString() + ": refused, over the tile limit");
            }
            for (LaidTile& lt : laid)
            {
                ReplayTile& tile = mesh->mTiles[lt.mKey];
                tile.mKey = lt.mKey;
                tile.mOverLand = lt.mOverLand;
                tile.mMin = meshTileMin(mesh->mOrigin, lt.mKey);
                tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
                tile.mMin.mdV[VZ] = region.mLandMin;
                tile.mMax.mdV[VZ] = region.mLandMax;
                auto input = std::make_shared<TileBuildInput>(params);
                input->mRecords.clear();
                input->mCuts.clear();
                input->mMeshOrigin = mesh->mOrigin;
                input->mTileX = lt.mKey.mX;
                input->mTileY = lt.mKey.mY;
                input->mTileSize = static_cast<F32>(TILE_M);
                // Only a region's own land is in its mesh; the void past it has none.
                if (lt.mOverLand)
                {
                    const Clock::time_point t1 = Clock::now();
                    censusTerrain(census, mesh->mOrigin, tile.mMin, tile.mMax, pad, input->mTerrain);
                    terrain_ms += since(t1);
                }
                applyAirMode(census, options, tile, *input);
                finishTileInput(lt.mRecords, mesh->mOrigin, *input);
                tileRootCues(cue_of, *input);
                tile.mInput = input;
            }
            meshes[mesh->mId] = std::move(mesh);
        }
        out.mStages.emplace_back("layout", since(t0) - terrain_ms);
        out.mStages.emplace_back("terrain", terrain_ms);

        // Every tile, on every core.
        t0 = Clock::now();
        std::vector<ReplayTile*> jobs;
        for (auto& m : meshes)
        {
            for (auto& t : m.second->mTiles)
            {
                jobs.push_back(&t.second);
            }
        }
        std::atomic<size_t> next{ 0 };
        const S32 threads = options.mThreads > 0 ? options.mThreads : std::max(1, static_cast<S32>(std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (S32 k = 0; k < threads; ++k)
        {
            pool.emplace_back([&jobs, &next, &options]() {
                for (size_t i = next++; i < jobs.size(); i = next++)
                {
                    if (options.mStamp)
                    {
                        // The raster stage only: the polygons wait for the spaces.
                        auto result = std::make_shared<TileBuildResult>();
                        std::shared_ptr<TileBuildStats> stats = options.mStats ? std::make_shared<TileBuildStats>() : nullptr;
                        if (stats)
                        {
                            stats->mFaceTraceEvery = options.mFaceTraceEvery;
                            std::copy(options.mFaceTraceBox, options.mFaceTraceBox + 4, stats->mFaceOffsets.mTraceBox);
                        }
                        jobs[i]->mRaster = rasteriseTile(*jobs[i]->mInput, *result, stats.get());
                        if (jobs[i]->mRaster)
                        {
                            result->mStatus = TileBuildResult::SUCCESS;
                        }
                        jobs[i]->mResult = result;
                        jobs[i]->mStats = stats;
                    }
                    else if (options.mStats)
                    {
                        auto stats = std::make_shared<TileBuildStats>();
                        stats->mFaceTraceEvery = options.mFaceTraceEvery;
                        std::copy(options.mFaceTraceBox, options.mFaceTraceBox + 4, stats->mFaceOffsets.mTraceBox);
                        jobs[i]->mResult = std::make_shared<const TileBuildResult>(buildTile(*jobs[i]->mInput, stats.get()));
                        jobs[i]->mStats = stats;
                    }
                    else
                    {
                        jobs[i]->mResult = std::make_shared<const TileBuildResult>(buildTile(*jobs[i]->mInput));
                    }
                }
            });
        }
        for (std::thread& t : pool)
        {
            t.join();
        }
        out.mStages.emplace_back("build", since(t0));

        // Published as SKNavMesh::publish does: Detour tiles, and the air to the sky map.
        t0 = Clock::now();
        auto sky = std::make_shared<SkyMap>();
        for (auto& m : meshes)
        {
            ReplayMesh& mesh = *m.second;
            mesh.mAirMin.set(1e300, 1e300, 0.0);
            mesh.mAirMax.set(-1e300, -1e300, 0.0);
            for (auto& t : mesh.mTiles)
            {
                const TileBuildResult& r = *t.second.mResult;
                ++out.mTiles;
                out.mEmpty += r.mStatus == TileBuildResult::EMPTY ? 1 : 0;
                if (r.mStatus == TileBuildResult::FAILED)
                {
                    ++out.mFailed;
                    continue;
                }
                for (S32 cls = 0; cls < NUM_CLASSES && !options.mStamp; ++cls)
                {
                    addDetour(mesh.mNav[cls], r.mData[cls]);
                }
                if (r.mAirBuilt && r.mAir)
                {
                    sky->set(skyContributor(mesh.mId, t.first), mesh.mOrigin + LLVector3d(r.mAir->mOrigin), *r.mAir);
                    mesh.mAirMin.mdV[VX] = std::min(mesh.mAirMin.mdV[VX], t.second.mMin.mdV[VX]);
                    mesh.mAirMin.mdV[VY] = std::min(mesh.mAirMin.mdV[VY], t.second.mMin.mdV[VY]);
                    mesh.mAirMax.mdV[VX] = std::max(mesh.mAirMax.mdV[VX], t.second.mMax.mdV[VX]);
                    mesh.mAirMax.mdV[VY] = std::max(mesh.mAirMax.mdV[VY], t.second.mMax.mdV[VY]);
                }
            }
        }
        out.mSky = sky;
        for (auto& m : meshes)
        {
            out.mMeshes.push_back(std::move(m.second));
        }
        out.mStages.emplace_back("publish", since(t0));

        // Enclosure and spaces per mesh with air, as SKNavRooms::post sets them up.
        F64 enclosure_ms = 0.0, spaces_ms = 0.0;
        F64 spaces_stage_ms[2][SpacesPass::NUM_STAGES] = {};    // the first round, and the rest
        // The slowest mesh of each: its time, tiles and stages.
        F64 spaces_slowest[2] = {};
        size_t slowest_tiles[2] = {};
        F64 slowest_stage_ms[2][SpacesPass::NUM_STAGES] = {};
        for (S32 round = 0; round < std::max(1, options.mRounds); ++round)
        {
            std::vector<EnclosureProductPtr> before;
            for (const auto& m : out.mMeshes)
            {
                before.push_back(m->mEnclosure);
            }
            for (size_t i = 0; i < out.mMeshes.size(); ++i)
            {
                ReplayMesh& mesh = *out.mMeshes[i];
                if (!hasAir(mesh))
                {
                    continue;
                }
                auto input = std::make_shared<EnclosureInput>();
                bool neighboured = false;
                for (size_t k = 0; k < out.mMeshes.size(); ++k)
                {
                    if (k != i && before[k] && hasAir(*out.mMeshes[k]) && touches(mesh, *out.mMeshes[k]))
                    {
                        input->mNeighbours.push_back({ out.mMeshes[k]->mOrigin, before[k] });
                        neighboured = true;
                    }
                }
                if (round > 0 && !neighboured && !options.mDirtyTiles)
                {
                    continue;
                }
                if (round > 0 && options.mDirtyTiles)
                {
                    // Fresh copies of the air nearest the middle, as if those tiles were rebuilt.
                    const LLVector3d mid = (mesh.mAirMin + mesh.mAirMax) * 0.5;
                    std::vector<std::pair<F64, TileKey>> order;
                    for (const auto& t : mesh.mTiles)
                    {
                        const TileBuildResult& r = *t.second.mResult;
                        if (r.mAirBuilt && r.mAir && !r.mAir->mLeaves.empty())
                        {
                            const LLVector3d c = (t.second.mMin + t.second.mMax) * 0.5 - mid;
                            order.emplace_back(c.mdV[VX] * c.mdV[VX] + c.mdV[VY] * c.mdV[VY], t.first);
                        }
                    }
                    std::sort(order.begin(), order.end());
                    for (size_t k = 0; k < std::min<size_t>(order.size(), options.mDirtyTiles); ++k)
                    {
                        ReplayTile& t = mesh.mTiles.at(order[k].second);
                        auto copy = std::make_shared<TileBuildResult>(*t.mResult);
                        copy->mAir = std::make_shared<TileAir>(*copy->mAir);
                        t.mResult = copy;
                    }
                }
                input->mGeneration = static_cast<U64>(round) + 1;
                input->mMeshOrigin = mesh.mOrigin;
                input->mSky = sky;
                input->mPrevious = mesh.mEnclosure;
                input->mParams = census.mEnclosure;
                input->mTimeSeams = options.mStats;
                for (const auto& t : mesh.mTiles)
                {
                    const TileBuildResult& r = *t.second.mResult;
                    if (r.mStatus != TileBuildResult::FAILED && r.mAirBuilt && r.mAir && !r.mAir->mLeaves.empty())
                    {
                        input->mTiles.push_back({ t.first, r.mAir, skyContributor(mesh.mId, t.first) });
                    }
                }
                Clock::time_point t1 = Clock::now();
                EnclosurePass pass(input);
                while (!pass.step(1u << 30))
                {
                }
                std::shared_ptr<EnclosureProduct> product = pass.takeProduct();
                enclosure_ms += since(t1);

                const std::shared_ptr<RoomsInput> rooms = replayRoomsInput(census, mesh, input->mGeneration);
                t1 = Clock::now();
                SpacesPass spaces(rooms, product);
                while (!spaces.step(1u << 30))
                {
                }
                const F64 mesh_ms = since(t1);
                spaces_ms += mesh_ms;
                const S32 kind = std::min(round, 1);
                const bool slowest = mesh_ms > spaces_slowest[kind];
                if (slowest)
                {
                    spaces_slowest[kind] = mesh_ms;
                    slowest_tiles[kind] = input->mTiles.size();
                }
                for (S32 k = 0; k < SpacesPass::NUM_STAGES; ++k)
                {
                    spaces_stage_ms[kind][k] += spaces.stageMillis(k);
                    slowest_stage_ms[kind][k] = slowest ? spaces.stageMillis(k) : slowest_stage_ms[kind][k];
                }
                if (options.mStats)
                {
                    ReplayRoomsRun run;
                    run.mRound = round;
                    run.mTiles = static_cast<U32>(input->mTiles.size());
                    run.mEnclosureMs = product->mMillis;
                    run.mSeamMs = pass.seamMillis();
                    run.mSpacesMs = mesh_ms;
                    for (S32 k = 0; k < EnclosurePass::NUM_STAGES; ++k)
                    {
                        run.mEnclosureStageMs[k] = pass.stageMillis(k);
                    }
                    for (S32 k = 0; k < SpacesPass::NUM_STAGES; ++k)
                    {
                        run.mSpacesStageMs[k] = spaces.stageMillis(k);
                    }
                    mesh.mRoomsRuns.push_back(run);
                }
                mesh.mEnclosure = product;
                mesh.mRooms = spaces.product();
            }
        }
        out.mStages.emplace_back("enclosure", enclosure_ms);
        out.mStages.emplace_back("spaces", spaces_ms);
        if (options.mStamp)
        {
            stampReplay(out, options);
        }
        for (S32 r = 0; r < 2; ++r)
        {
            for (S32 k = 0; k < SpacesPass::NUM_STAGES; ++k)
            {
                out.mStages.emplace_back(std::string(r ? "spaces.again." : "spaces.first.") + SpacesPass::stageName(k), spaces_stage_ms[r][k]);
            }
            const std::string slowest = llformat("spaces.%s.slowest(%u tiles)", r ? "again" : "first", static_cast<U32>(slowest_tiles[r]));
            out.mStages.emplace_back(slowest, spaces_slowest[r]);
            for (S32 k = 0; k < SpacesPass::NUM_STAGES; ++k)
            {
                out.mStages.emplace_back(slowest + "." + SpacesPass::stageName(k), slowest_stage_ms[r][k]);
            }
        }

        t0 = Clock::now();
        for (const auto& m : out.mMeshes)
        {
            if (!m->mEnclosure || !m->mRooms)
            {
                continue;
            }
            auto input = std::make_shared<WindInput>();
            input->mGeneration = m->mEnclosure->mGeneration;
            input->mEnclosure = m->mEnclosure;
            input->mRooms = m->mRooms;
            input->mBands = defaultWindBands();
            WindPass pass(input);
            while (!pass.step())
            {
            }
            m->mWind = pass.product();
        }
        out.mStages.emplace_back("wind", since(t0));
    }

    void stampReplay(ReplayResult& out, const ReplayOptions& options)
    {
        // Every tile's polygons from its held raster, stamped with the spaces that landed.
        const Clock::time_point t0 = Clock::now();
        std::vector<std::pair<ReplayMesh*, ReplayTile*>> jobs;
        for (auto& m : out.mMeshes)
        {
            for (auto& t : m->mTiles)
            {
                if (t.second.mRaster)
                {
                    jobs.emplace_back(m.get(), &t.second);
                }
            }
        }
        std::atomic<size_t> next{ 0 };
        const S32 threads = options.mThreads > 0 ? options.mThreads : std::max(1, static_cast<S32>(std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (S32 k = 0; k < threads; ++k)
        {
            pool.emplace_back([&jobs, &next, &options]() {
                for (size_t i = next++; i < jobs.size(); i = next++)
                {
                    const ReplayMesh& mesh = *jobs[i].first;
                    ReplayTile& tile = *jobs[i].second;
                    const TileBuildInput& input = *tile.mInput;
                    const TileRaster& raster = *tile.mRaster;
                    const Clock::time_point s0 = Clock::now();
                    auto in = std::make_shared<TileStampInput>();
                    const TileAirPtr air = tile.mResult->mAir;
                    const TileEnclosure* enc = mesh.mEnclosure && air ? mesh.mEnclosure->tile(tile.mKey) : nullptr;
                    if (enc && enc->mAir == air && mesh.mRooms)
                    {
                        const LLVector3d lo = tile.mMin - mesh.mOrigin, hi = tile.mMax - mesh.mOrigin;
                        stampInput(*air, *enc, *mesh.mRooms, LLVector3(lo), LLVector3(hi), *in);
                    }
                    TileStamp stamp;
                    const S32 border = borderCells(input.mParams[static_cast<S32>(AgentClass::AVATAR)].mRadius, input.mCell);
                    stampTile(*raster.mChf, border, raster.mSpanGap, *in, raster.mReal, raster.mFlights, StampParams(), stamp);
                    tile.mStampMs = since(s0);
                    if (options.mStats)
                    {
                        auto unstamped = std::make_shared<TileBuildResult>(*tile.mResult);
                        auto ustats = std::make_shared<TileBuildStats>(*tile.mStats);
                        polygoniseTile(input, raster, *unstamped, ustats.get());
                        tile.mUnstamped = unstamped;
                        tile.mUnstampedStats = ustats;
                    }
                    auto result = std::make_shared<TileBuildResult>(*tile.mResult);
                    std::shared_ptr<TileBuildStats> stats = tile.mStats ? std::make_shared<TileBuildStats>(*tile.mStats) : nullptr;
                    polygoniseTile(input, raster, *result, stats.get(), in.get(), &stamp);
                    if (mesh.mRooms)
                    {
                        auto binding = std::make_shared<TileBinding>();
                        bindStamp(result->mStamp, *in, enc, *mesh.mRooms, *binding);
                        tile.mBinding = binding;
                    }
                    if (mesh.mEnclosure)
                    {
                        bindWallSpaces(*mesh.mEnclosure, result->mSegments[static_cast<S32>(AgentClass::AVATAR)]);
                        bindWallSpaces(*mesh.mEnclosure, result->mFaces);
                    }
                    tile.mStampInput = in;
                    tile.mResult = result;
                    tile.mStats = stats;
                    if (!options.mKeepRasters)
                    {
                        tile.mRaster.reset();
                    }
                }
            });
        }
        for (std::thread& t : pool)
        {
            t.join();
        }
        out.mStages.emplace_back("stamp", since(t0));
        F64 storeys_ms = 0.0;
        for (auto& m : out.mMeshes)
        {
            ReplayMesh& mesh = *m;
            U32 slot = 0;
            for (auto& t : mesh.mTiles)
            {
                const TileBuildResult& r = *t.second.mResult;
                if (r.mStatus == TileBuildResult::FAILED)
                {
                    continue;
                }
                ++slot;
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    if (addDetour(mesh.mNav[cls], r.mData[cls], slot) && t.second.mBinding)
                    {
                        applyBinding(*mesh.mNav[cls], t.first, *t.second.mBinding);
                        // The poly's space ids under the slot, as the viewer's publish keeps them.
                        const TileBinding& b = *t.second.mBinding;
                        std::vector<U32>& spaces = mesh.mSlotSpaces[slot];
                        spaces.assign(r.mStamp.mPolyComponent.size(), 0);
                        for (size_t p = 0; p < spaces.size(); ++p)
                        {
                            const U16 c = r.mStamp.mPolyComponent[p];
                            spaces[p] = c < b.mComponentSpaceId.size() ? b.mComponentSpaceId[c] : 0;
                        }
                        std::vector<const Flight*>& flights = mesh.mSlotFlights[slot];
                        flights.assign(spaces.size(), nullptr);
                        for (const Flight& f : r.mFlights[cls])
                        {
                            for (const U16 p : f.mPolys)
                            {
                                if (p < flights.size())
                                {
                                    flights[p] = &f;
                                }
                            }
                        }
                    }
                }
                if (t.second.mUnstamped)
                {
                    addDetour(mesh.mUnstampedNav, t.second.mUnstamped->mData[static_cast<S32>(AgentClass::AVATAR)]);
                }
            }
            if (const dtNavMesh* nav = mesh.mNav[static_cast<S32>(AgentClass::AVATAR)]; nav && mesh.mRooms)
            {
                const Clock::time_point s0 = Clock::now();
                buildStoreys(*nav, [&mesh](const dtMeshTile* tile, U32 poly) { return mesh.polySpace(tile, poly); }, *mesh.mRooms, mesh.mStoreys,
                             [&mesh](const dtMeshTile* tile, U32 poly) { return mesh.polyFlight(tile, poly); });
                storeys_ms += since(s0);
            }
        }
        out.mStages.emplace_back("storeys", storeys_ms);
    }

    ReplayRoomsAgain replayRoomsAgain(const CensusFile& census, const ReplayResult& result, size_t mesh_index, const std::map<TileKey, TileAirPtr>& air)
    {
        ReplayRoomsAgain out;
        const ReplayMesh& mesh = *result.mMeshes[mesh_index];
        auto input = std::make_shared<EnclosureInput>();
        for (size_t k = 0; k < result.mMeshes.size(); ++k)
        {
            const ReplayMesh& other = *result.mMeshes[k];
            if (k != mesh_index && other.mEnclosure && hasAir(other) && touches(mesh, other))
            {
                input->mNeighbours.push_back({ other.mOrigin, other.mEnclosure });
            }
        }
        const U64 generation = (mesh.mEnclosure ? mesh.mEnclosure->mGeneration : 0) + 1;
        input->mGeneration = generation;
        input->mMeshOrigin = mesh.mOrigin;
        input->mSky = result.mSky;
        input->mPrevious = mesh.mEnclosure;
        input->mParams = census.mEnclosure;
        for (const auto& t : mesh.mTiles)
        {
            const TileBuildResult& r = *t.second.mResult;
            const auto it = air.find(t.first);
            const TileAirPtr a = it != air.end() ? it->second : (r.mAirBuilt ? r.mAir : nullptr);
            if (r.mStatus != TileBuildResult::FAILED && a && !a->mLeaves.empty())
            {
                input->mTiles.push_back({ t.first, a, skyContributor(mesh.mId, t.first) });
            }
        }
        Clock::time_point t0 = Clock::now();
        EnclosurePass pass(input);
        while (!pass.step(1u << 30))
        {
        }
        out.mEnclosure = pass.takeProduct();
        out.mEnclosureMs = since(t0);
        t0 = Clock::now();
        SpacesPass spaces(replayRoomsInput(census, mesh, generation), out.mEnclosure);
        while (!spaces.step(1u << 30))
        {
        }
        out.mRooms = spaces.product();
        out.mSpacesMs = since(t0);
        return out;
    }

    std::shared_ptr<RoomsInput> replayRoomsInput(const CensusFile& census, const ReplayMesh& mesh, U64 generation)
    {
        auto rooms = std::make_shared<RoomsInput>();
        rooms->mGeneration = generation;
        rooms->mMeshKey = mix(SEED, mesh.mId.getCRC32());
        rooms->mMeshOrigin = mesh.mOrigin;
        rooms->mPrevious = mesh.mRooms;
        rooms->mParams.mWalkHeadM = census.mEnclosure.mWalkHeadM;
        rooms->mParams.mAgentRadiusM = census.mParams.mParams[static_cast<S32>(AgentClass::AVATAR)].mRadius;
        const LLVector3d dlo(mesh.mAirMin.mdV[VX], mesh.mAirMin.mdV[VY], -1e9), dhi(mesh.mAirMax.mdV[VX], mesh.mAirMax.mdV[VY], 1e9);
        for (const DoorEntity& d : census.mDoors)
        {
            if (overlapsXY(d.mMin, d.mMax, dlo, dhi) && d.mMin.mdV[VZ] <= dhi.mdV[VZ] && dlo.mdV[VZ] <= d.mMax.mdV[VZ])
            {
                rooms->mDoors.push_back(d);
            }
        }
        for (const Feature& f : census.mFeatures)
        {
            if (f.mKind == FeatureKind::ELEVATOR && f.mMax.mdV[VX] >= dlo.mdV[VX] && f.mMin.mdV[VX] <= dhi.mdV[VX] && f.mMax.mdV[VY] >= dlo.mdV[VY] &&
                f.mMin.mdV[VY] <= dhi.mdV[VY])
            {
                rooms->mLifts.emplace_back(f.mMin, f.mMax);
            }
        }
        return rooms;
    }

    void writeReplayExport(std::ostream& file, const CensusFile& census, const ReplayResult& result)
    {
        file << std::setprecision(10);
        const ClassParams& avatar = census.mParams.mParams[static_cast<S32>(AgentClass::AVATAR)];
        writeExportHeader(file, census.mExported, census.mAgent, avatar.mHeight, avatar.mRadius, avatar.mClimb, avatar.mSlope, census.mParams.mCell);
        ExportCounts counts;
        bool first_mesh = true;
        for (const auto& m : result.mMeshes)
        {
            const ReplayMesh& mesh = *m;
            file << (first_mesh ? "" : ",") << "{\"id\":\"" << mesh.mId << "\",\"terrain\":" << (mesh.mTerrain ? "true" : "false") << ",\"origin\":["
                 << mesh.mOrigin.mdV[VX] << ',' << mesh.mOrigin.mdV[VY] << ',' << mesh.mOrigin.mdV[VZ] << "],\"tiles\":[";
            first_mesh = false;
            bool first_tile = true;
            for (const auto& t : mesh.mTiles)
            {
                const TileBuildResult& r = *t.second.mResult;
                TileExport tile;
                tile.mKey = t.first;
                tile.mMin = t.second.mMin;
                tile.mMax = t.second.mMax;
                if (r.mStatus != TileBuildResult::FAILED)
                {
                    tile.mFaces = &r.mFaces;
                    tile.mLedges = &r.mLedges;
                    tile.mRunoff = &r.mRunoff;
                    tile.mAir = r.mAirBuilt ? r.mAir : nullptr;
                    if (tile.mAir && mesh.mEnclosure)
                    {
                        tile.mEnclosure = mesh.mEnclosure->tile(t.first);
                        tile.mEnclosureGeneration = mesh.mEnclosure->mGeneration;
                    }
                    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                    {
                        tile.mSegments[cls] = &r.mSegments[cls];
                        tile.mLinks[cls] = &r.mLinks[cls];
                        tile.mFlights[cls] = &r.mFlights[cls];
                    }
                    tile.mStamp = &r.mStamp;
                    tile.mBinding = t.second.mBinding.get();
                }
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    tile.mNav[cls] = mesh.mNav[cls];
                }
                file << (first_tile ? "" : ",");
                first_tile = false;
                writeExportTile(file, mesh.mOrigin, tile, counts);
            }
            file << ']';
            if (mesh.mRooms)
            {
                writeExportSpaces(file, mesh.mOrigin, *mesh.mRooms, mesh.mWind.get());
            }
            file << '}';
        }
        file << ']';

        std::map<LLUUID, const CensusSpan*> spans;
        for (const CensusSpan& s : census.mSpans)
        {
            spans[s.mId] = &s;
        }
        file << ",\"features\":[";
        for (size_t i = 0; i < census.mFeatures.size(); ++i)
        {
            const Feature& f = census.mFeatures[i];
            const auto it = spans.find(featureId(f));
            const CensusSpan* span = it != spans.end() ? it->second : nullptr;
            file << (i ? "," : "");
            writeExportFeature(file, f, span ? span->mKind : LinkKind::LADDER, span ? span->mExits : nullptr, span ? span->mLinks : nullptr);
        }
        file << "],\"doors\":[";
        for (size_t i = 0; i < census.mDoors.size(); ++i)
        {
            file << (i ? "," : "");
            writeExportDoor(file, census.mDoors[i]);
        }
        file << "]}\n";
    }

    std::string replaySummary(const ReplayResult& result)
    {
        std::ostringstream out;
        out << result.mMeshes.size() << " meshes, " << result.mTiles << " tiles (" << result.mEmpty << " empty, " << result.mFailed << " failed)\n";
        if (result.mRefused)
        {
            out << result.mRefused << " meshes refused, over " << MAX_TILES_PER_MESH << " tiles\n";
        }
        out << "clusters: " << result.mClustersSame << " same, " << result.mClustersDiffer << " differ, " << result.mClustersOnlyViewer
            << " only in the viewer, " << result.mClustersOnlyReplay << " only in the replay\n";
        for (const std::string& note : result.mClusterNotes)
        {
            out << "  " << note << '\n';
        }
        for (const auto& stage : result.mStages)
        {
            out << stage.first << ": " << std::fixed << std::setprecision(1) << stage.second << " ms\n";
        }
        return out.str();
    }
}
