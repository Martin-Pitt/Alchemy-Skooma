/**
 * @file sknavreplay_test.cpp
 * @brief Tests for the census replay: the whole pipeline from a census, against the passes run directly.
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

#include "../sknavairdistil.h"
#include "../sknavbuildstats.h"
#include "../sknavflights.h"
#include "../sknavannotate.h"
#include "../sknavlayout.h"
#include "../sknavreplay.h"
#include "../sknavreplaystats.h"
#include "../sknavtilecache.h"
#include "sknavtestscene.h"

#include "llbase64.h"
#include "llstring.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>

namespace
{
    using namespace sknav;

    const LLVector3d REGION(256000.0, 256000.0, 0.0);
    const F32 GROUND = 20.f;
    const F64 SHIFT = 4.0;          // the scene's offset into the region, both axes

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        u.mData[15] = 0x5a;
        return u;
    }

    /// A census record for an axis-aligned solid, region-local metres over the region's origin.
    RecordPtr solid(U32 root, U32 part, const LLVector3d& origin, F64 x0, F64 y0, F64 z0, F64 x1, F64 y1, F64 z1)
    {
        auto r = std::make_shared<Record>();
        r->mRoot = id(root);
        r->mPart = id(part);
        r->mShape = Shape::BOX;
        r->mSolid = true;
        r->mOrigin = origin;
        r->mCenter.set(static_cast<F32>((x0 + x1) / 2), static_cast<F32>((y0 + y1) / 2), static_cast<F32>((z0 + z1) / 2));
        r->mHalf.set(static_cast<F32>((x1 - x0) / 2), static_cast<F32>((y1 - y0) / 2), static_cast<F32>((z1 - z0) / 2));
        r->mAxes[0].set(1.f, 0.f, 0.f);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mMin = origin + LLVector3d(x0, y0, z0);
        r->mMax = origin + LLVector3d(x1, y1, z1);
        r->mSignature = mix(mix(SEED, root), part);
        return r;
    }

    /// The rooms tests' two roofed rooms standing on a 64 m region, a
    /// skybox floating well off its edge, and the door out of the west room.
    CensusFile scene()
    {
        CensusFile c;
        c.mExported = "2026-09-27T12:00:00Z";
        c.mAgent = REGION + LLVector3d(10.0, 10.0, GROUND);
        c.mCamera = c.mAgent;
        c.mParams.mCell = 0.25f;                 // coarser than the viewer, to keep the test quick
        c.mParams.mTopParams = { 0.5f, 0.f, 0.75f, 89.f };
        c.mParams.mAir = true;
        c.mParams.mWallFaces = true;
        c.mParams.mEdges[0] = true;
        c.mParams.mRunoff = true;
        c.mEnclosure.mWalkHeadM = 2.f;

        CensusRegion r;
        r.mId = id(900);
        r.mHandle = (static_cast<U64>(256000) << 32) | 256000;
        r.mOrigin = REGION;
        r.mName = "Replay";
        r.mWidth = 64.f;
        r.mWater = 0.f;
        r.mLandMin = r.mLandMax = GROUND;
        const F32 pad = 5.f;                    // the air pad, 16 cells of 0.25, plus a metre
        r.mPhase = std::ceil(pad) - pad;
        r.mSamples = static_cast<S32>(std::ceil(r.mWidth - r.mPhase));
        r.mZ.assign(static_cast<size_t>(r.mSamples) * r.mSamples, GROUND);
        c.mRegions.push_back(r);

        const sknav_test::Scene rooms = sknav_test::twoRooms();
        U32 n = 1;
        for (const sknav_test::Solid& s : rooms.mSolids)
        {
            c.mRecords.push_back(solid(n, n, REGION, s.mX0 + SHIFT, s.mY0 + SHIFT, s.mZ0 + GROUND, s.mX1 + SHIFT, s.mY1 + SHIFT, s.mZ1 + GROUND));
            ++n;
        }
        // A skybox 500 m up, off the region's west edge: a mesh of its own.
        c.mRecords.push_back(solid(500, 500, REGION, -80.0, 4.0, 520.0, -70.0, 14.0, 520.25));
        c.mRecords.push_back(solid(500, 501, REGION, -80.0, 4.0, 520.25, -79.75, 14.0, 523.0));

        const ClusterSet clusters = cluster(c.mRecords, c.mClusterGap);
        for (const ClusterPtr& cl : clusters.mClusters)
        {
            CensusCluster out;
            out.mId = cl->mId;
            out.mGrounded = cl->mMin.mdV[VZ] < 100.0;
            for (const RecordPtr& rec : cl->mRecords)
            {
                out.mRecords.push_back(static_cast<U32>(std::find(c.mRecords.begin(), c.mRecords.end(), rec) - c.mRecords.begin()));
            }
            c.mClusters.push_back(out);
        }

        DoorEntity d;
        d.mId = id(1000);
        d.mRegion = r.mHandle;
        d.mMin = REGION + LLVector3d(2.0 + SHIFT, 5.0 + SHIFT, GROUND);
        d.mMax = REGION + LLVector3d(2.25 + SHIFT, 6.0 + SHIFT, GROUND + 2.2);
        d.mPlanePoint = REGION + LLVector3d(2.125 + SHIFT, 5.5 + SHIFT, GROUND);
        d.mNormal.set(1.f, 0.f, 0.f);
        d.mWidth = 1.f;
        d.mHeight = 2.2f;
        d.mSill = GROUND;
        d.mLeaves = { id(1001) };
        c.mDoors.push_back(d);
        return c;
    }

    /// The export with the passes' own timings taken out, which no two runs share.
    std::string exported(const CensusFile& c, const ReplayResult& r)
    {
        std::ostringstream out;
        writeReplayExport(out, c, r);
        return std::regex_replace(out.str(), std::regex("\"ms\":[-0-9.e+]+"), "\"ms\":0");
    }

    const ReplayMesh* meshOf(const ReplayResult& r, const LLUUID& mesh)
    {
        for (const auto& m : r.mMeshes)
        {
            if (m->mId == mesh)
            {
                return m.get();
            }
        }
        return nullptr;
    }

    bool sameProducts(const TileBuildResult& a, const TileBuildResult& b)
    {
        if (a.mStatus != b.mStatus || a.mAirBuilt != b.mAirBuilt || a.mFaces.size() != b.mFaces.size() || !a.mAir != !b.mAir)
        {
            return false;
        }
        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            if (a.mData[c] != b.mData[c] || a.mSegments[c].size() != b.mSegments[c].size() || a.mFlights[c].size() != b.mFlights[c].size())
            {
                return false;
            }
        }
        return !a.mAir || packTileAir(*a.mAir) == packTileAir(*b.mAir);
    }
}

namespace tut
{
    struct sknavreplay_data {};
    typedef test_group<sknavreplay_data> sknavreplay_group;
    typedef sknavreplay_group::object sknavreplay_object;
    tut::sknavreplay_group g("sknavreplay");

    template<> template<>
    void sknavreplay_object::test<1>()
    {
        set_test_name("a census replays into the meshes the viewer lays out, its clusters agreeing");
        const CensusFile c = scene();
        ReplayResult r;
        replayCensus(c, ReplayOptions(), r);
        ensure_equals("the viewer's clusters", r.mClustersSame, 2u);
        ensure_equals("none differ", r.mClustersDiffer + r.mClustersOnlyViewer + r.mClustersOnlyReplay, 0u);
        ensure_equals("the region's mesh and the skybox's", r.mMeshes.size(), 2u);
        const ReplayMesh* land = meshOf(r, id(900));
        ensure("the region's mesh", land && land->mTerrain && land->mOrigin == REGION);
        ensure_equals("its 2 x 2 tiles and no void", land->mTiles.size(), 4u);
        const ReplayMesh* sky = meshOf(r, c.mRecords.back()->mRoot);
        ensure("the skybox's own mesh", sky && !sky->mTerrain && sky->mOrigin == LLVector3d(255904.0, 256000.0, 0.0));
        ensure_equals("nothing failed", r.mFailed, 0u);
        ensure("rooms", land->mRooms && land->mRooms->mSpaces.size() >= 3);
        ensure("wind", land->mWind && !land->mWind->mSlabs.empty());
        bool door = false;
        for (const Portal& p : land->mRooms->mPortals)
        {
            door = door || p.mKind == PortalKind::DOOR;
        }
        ensure("the door is a door portal", door);

        // A cluster the viewer drew differently is reported, not hidden.
        CensusFile other = c;
        other.mClusters[0].mRecords.pop_back();
        ReplayResult r2;
        replayCensus(other, ReplayOptions(), r2);
        ensure_equals("one differs", r2.mClustersDiffer, 1u);
    }

    template<> template<>
    void sknavreplay_object::test<2>()
    {
        set_test_name("the replay's products are what the passes make when run directly on the same input");
        const CensusFile c = scene();
        ReplayResult r;
        replayCensus(c, ReplayOptions(), r);
        const ReplayMesh* land = meshOf(r, id(900));
        ensure("mesh", land != nullptr);

        // Tile (0, 0) built by hand: the mesh origin is the region's, the
        // window runs a pad of 5 m past the tile on every side, the void
        // deep below where it leaves the region, and every ground record
        // reaches it.
        TileBuildInput in = c.mParams;
        in.mMeshOrigin = REGION;
        in.mTileSize = 32.f;
        in.mTerrain.mOrigin.set(-5.f, -5.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = in.mTerrain.mHeight = 43;
        in.mTerrain.mZ.assign(43 * 43, -1000.f);
        for (S32 y = 5; y < 43; ++y)
        {
            for (S32 x = 5; x < 43; ++x)
            {
                in.mTerrain.mZ[y * 43 + x] = GROUND;
            }
        }
        in.mTerrain.mWater = 0.f;
        for (const RecordPtr& rec : c.mRecords)
        {
            if (rec->mMin.mdV[VZ] < 100.0)
            {
                in.mRecords.push_back(rec);
            }
        }
        in.mZMin = GROUND - 1.f;                // the land is the lowest there is
        in.mZMax = GROUND + 3.25f + 3.f;        // the roof's top, plus headroom
        const ReplayTile& tile = land->mTiles.at(TileKey{ 0, 0 });
        ensure_equals("the same input, by its content key", tileContentKey(*tile.mInput), tileContentKey(in));
        const TileBuildResult direct = buildTile(in);
        ensure("the same tile products", sameProducts(direct, *tile.mResult));

        // Enclosure, spaces and wind over the mesh's air, run directly.
        auto enc = std::make_shared<EnclosureInput>();
        enc->mGeneration = 1;
        enc->mMeshOrigin = REGION;
        enc->mParams = c.mEnclosure;
        auto sky = std::make_shared<SkyMap>();
        for (const auto& t : land->mTiles)
        {
            const TileAirPtr& air = t.second.mResult->mAir;
            if (air && !air->mLeaves.empty())
            {
                enc->mTiles.push_back({ t.first, air, skyContributor(id(900), t.first) });
                sky->set(skyContributor(id(900), t.first), REGION + LLVector3d(air->mOrigin), *air);
            }
        }
        enc->mSky = sky;
        std::shared_ptr<EnclosureProduct> product = std::const_pointer_cast<EnclosureProduct>(computeEnclosure(enc));
        ensure_equals("the same enclosure", product->mSummary, land->mEnclosure->mSummary);
        ensure_equals("the same gaps", product->mGapCount, land->mEnclosure->mGapCount);

        auto rooms = std::make_shared<RoomsInput>();
        rooms->mGeneration = 1;
        rooms->mMeshKey = mix(SEED, id(900).getCRC32());
        rooms->mMeshOrigin = REGION;
        rooms->mParams.mWalkHeadM = c.mEnclosure.mWalkHeadM;
        rooms->mDoors = c.mDoors;
        const RoomsProductPtr spaces = computeSpaces(rooms, product);
        ensure_equals("the same spaces", spaces->mSpaces.size(), land->mRooms->mSpaces.size());
        for (size_t i = 0; i < spaces->mSpaces.size(); ++i)
        {
            ensure_equals("the same space ids", spaces->mSpaces[i].mId, land->mRooms->mSpaces[i].mId);
            ensure_equals("the same floor", spaces->mSpaces[i].mFloorArea, land->mRooms->mSpaces[i].mFloorArea);
        }
        ensure_equals("the same portals", spaces->mPortals.size(), land->mRooms->mPortals.size());

        auto wind = std::make_shared<WindInput>();
        wind->mEnclosure = product;
        wind->mRooms = spaces;
        wind->mBands = defaultWindBands();
        const WindProductPtr w = computeWind(wind);
        ensure_equals("the same slabs", w->mSlabs.size(), land->mWind->mSlabs.size());
        for (size_t i = 0; i < w->mSlabs.size(); ++i)
        {
            ensure("the same slab cells", std::memcmp(w->mSlabs[i]->mCells.data(), land->mWind->mSlabs[i]->mCells.data(),
                                                      w->mSlabs[i]->mCells.size() * sizeof(WindCell)) == 0);
        }
    }

    template<> template<>
    void sknavreplay_object::test<3>()
    {
        set_test_name("a census through its file replays to the same export as the census in memory");
        const CensusFile c = scene();
        std::ostringstream file;
        writeCensus(file, c);
        CensusFile back;
        std::string error;
        ensure("read back: " + error, readCensus(file.str(), back, &error));
        ReplayResult a, b;
        replayCensus(c, ReplayOptions(), a);
        replayCensus(back, ReplayOptions(), b);
        const std::string ea = exported(c, a), eb = exported(back, b);
        ensure("an export with tiles and spaces", ea.find("\"air\":{") != std::string::npos && ea.find("\"spaces\":[{") != std::string::npos);
        ensure("the same export", ea == eb);
        // The scene as a file, for the tools to be tried on.
        const std::string scene_out = LLStringUtil::getenv("SKNAV_CENSUS_SCENE");
        if (!scene_out.empty())
        {
            std::ofstream(scene_out, std::ios::binary) << file.str();
        }
    }

    template<> template<>
    void sknavreplay_object::test<5>()
    {
        set_test_name("past the air radius a tile builds no air, as in the viewer, unless air is asked for everywhere");
        CensusFile c = scene();
        c.mAirRadius = 50.f;                    // the skybox is some 74 m from the camera
        const LLUUID skybox = c.mRecords.back()->mRoot;
        ReplayResult near_only, everywhere;
        replayCensus(c, ReplayOptions(), near_only);
        ReplayOptions all;
        all.mAir = AirMode::EVERYWHERE;
        replayCensus(c, all, everywhere);
        const ReplayTile& far_tile = meshOf(near_only, skybox)->mTiles.begin()->second;
        ensure("no air past the radius", !far_tile.mResult->mAirBuilt && !far_tile.mInput->mAir);
        ensure("the region's tiles keep theirs", meshOf(near_only, id(900))->mTiles.at(TileKey{ 0, 0 }).mResult->mAirBuilt);
        ensure("air everywhere when asked", meshOf(everywhere, skybox)->mTiles.begin()->second.mResult->mAirBuilt);
    }

    template<> template<>
    void sknavreplay_object::test<6>()
    {
        set_test_name("each tile of a census's largest mesh rebuilt, as it was and with floor lost, reading the memo is the pass reading none, when a file is named");
        const std::string path = LLStringUtil::getenv("SKNAV_REPLAY_VERIFY");
        if (path.empty())
        {
            return;
        }
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayResult r;
        replayCensus(c, ReplayOptions(), r);
        const ReplayMesh* big = nullptr;
        size_t most = 0;
        for (const auto& m : r.mMeshes)
        {
            size_t n = 0;
            for (const TileEnclosure& te : m->mEnclosure ? m->mEnclosure->mTiles : std::vector<TileEnclosure>())
            {
                n += te.mAir && !te.mAir->mLeaves.empty() ? 1 : 0;
            }
            if (m->mRooms && n > most)
            {
                most = n;
                big = m.get();
            }
        }
        ensure("a mesh with spaces", big != nullptr);
        const std::string limit = LLStringUtil::getenv("SKNAV_REPLAY_VERIFY_TILES");
        const size_t cap = limit.empty() ? most : static_cast<size_t>(std::stoul(limit));
        auto bare = std::make_shared<RoomsProduct>(*big->mRooms);
        bare->mMemo.reset();
        typedef std::chrono::steady_clock Clock;
        auto ms = [](Clock::time_point t) { return std::chrono::duration<F64, std::milli>(Clock::now() - t).count(); };
        size_t checked = 0;
        U64 reused = 0;
        F64 memo_ms = 0.0, bare_ms = 0.0;
        for (size_t i = 0; i < big->mEnclosure->mTiles.size() && checked < cap; ++i)
        {
            const TileEnclosure& te = big->mEnclosure->mTiles[i];
            if (!te.mAir || te.mAir->mLeaves.empty())
            {
                continue;
            }
            for (S32 lost = 0; lost < 2; ++lost)
            {
                // The same air under a new pointer, as a rebuild that moved nothing
                // gives; or with the floor of the tile's middle 4 m square gone.
                auto fresh = std::make_shared<TileAir>(*te.mAir);
                if (lost)
                {
                    const S32 half = static_cast<S32>(2.f / fresh->mCell), mx = fresh->mWidth / 2, my = fresh->mHeight / 2;
                    for (const AirLeaf& leaf : fresh->mLeaves)
                    {
                        if (leaf.mX >= mx - half && leaf.mX < mx + half && leaf.mY >= my - half && leaf.mY < my + half)
                        {
                            for (U32 k = 0; k < leaf.mGapCount; ++k)
                            {
                                fresh->mGaps[leaf.mFirstGap + k].mFlags &= static_cast<U8>(~AirGap::TOP);
                            }
                        }
                    }
                }
                auto enc_memo = std::make_shared<EnclosureProduct>(*big->mEnclosure);
                enc_memo->mTiles[i].mAir = fresh;
                auto enc_bare = std::make_shared<EnclosureProduct>(*enc_memo);
                const std::shared_ptr<RoomsInput> in_memo = replayRoomsInput(c, *big, 99);
                const std::shared_ptr<RoomsInput> in_bare = replayRoomsInput(c, *big, 99);
                in_bare->mPrevious = bare;
                Clock::time_point t0 = Clock::now();
                const RoomsProductPtr with_memo = computeSpaces(in_memo, enc_memo);
                memo_ms += ms(t0);
                t0 = Clock::now();
                const RoomsProductPtr without = computeSpaces(in_bare, enc_bare);
                bare_ms += ms(t0);
                std::cout << "tile " << te.mKey.mX << "," << te.mKey.mY << (lost ? " lost" : " same") << ": " << with_memo->mMillis << " ms reading the memo ("
                          << with_memo->mReused << " reused), " << without->mMillis << " ms without" << std::endl;
                ensure("the incremental pass is the full pass", sknav_test::dumpRooms(*enc_memo, *with_memo) == sknav_test::dumpRooms(*enc_bare, *without));
                reused += with_memo->mReused;
            }
            ++checked;
        }
        std::cout << checked << " of " << most << " tiles twice, " << reused << " reused, " << memo_ms << " ms reading the memo, " << bare_ms
                  << " ms without" << std::endl;
    }

    template<> template<>
    void sknavreplay_object::test<4>()
    {
        set_test_name("replay a census file into a navmesh export, when one is named");
        const std::string path = LLStringUtil::getenv("SKNAV_CENSUS_REPLAY");
        if (path.empty())
        {
            return;
        }
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        const std::string rounds = LLStringUtil::getenv("SKNAV_REPLAY_ROUNDS");
        if (!rounds.empty())
        {
            options.mRounds = std::stoi(rounds);
        }
        const std::string dirty = LLStringUtil::getenv("SKNAV_REPLAY_DIRTY");
        if (!dirty.empty())
        {
            options.mDirtyTiles = static_cast<U32>(std::stoi(dirty));
        }
        if (LLStringUtil::getenv("SKNAV_REPLAY_AIR") == "everywhere")
        {
            options.mAir = AirMode::EVERYWHERE;
        }
        const std::string threads = LLStringUtil::getenv("SKNAV_REPLAY_THREADS");
        if (!threads.empty())
        {
            options.mThreads = std::stoi(threads);
        }
        options.mStats = !LLStringUtil::getenv("SKNAV_REPLAY_STATS").empty();
        ReplayResult r;
        replayCensus(c, options, r);
        std::string dir = LLStringUtil::getenv("SKNAV_REPLAY_OUT");
        if (dir.empty())
        {
            dir = path.substr(0, path.find_last_of("/\\") + 1);
        }
        else if (dir.back() != '/' && dir.back() != '\\')
        {
            dir += '/';
        }
        std::ofstream out(dir + "sknavmesh-export.json", std::ios::binary);
        writeReplayExport(out, c, r);
        out.close();
        std::ofstream stats(dir + "replay-stats.json", std::ios::binary);
        stats << "{\"census\":\"" << jsonEscape(path) << "\",\"meshes\":" << r.mMeshes.size() << ",\"tiles\":" << r.mTiles
              << ",\"empty\":" << r.mEmpty << ",\"failed\":" << r.mFailed << ",\"clusters\":{\"same\":" << r.mClustersSame
              << ",\"differ\":" << r.mClustersDiffer << ",\"only_viewer\":" << r.mClustersOnlyViewer << ",\"only_replay\":" << r.mClustersOnlyReplay
              << "},\"stages_ms\":{";
        for (size_t i = 0; i < r.mStages.size(); ++i)
        {
            stats << (i ? "," : "") << '"' << r.mStages[i].first << "\":" << r.mStages[i].second;
        }
        stats << "}}\n";
        if (options.mStats)
        {
            // SKNAV_REPLAY_STATS: the measurements; SKNAV_REPLAY_EDITS names "record op" lines to rebuild with.
            std::ofstream measure(dir + "replay-measure.json", std::ios::binary);
            writeReplayStats(measure, c, r);
            const std::string edits_path = LLStringUtil::getenv("SKNAV_REPLAY_EDITS");
            if (!edits_path.empty())
            {
                std::ifstream in(edits_path);
                std::vector<ReplayEdit> edits;
                U32 record = 0, op = 0;
                while (in >> record >> op)
                {
                    edits.push_back({ record, static_cast<ReplayEdit::Op>(op) });
                }
                std::ofstream edit_out(dir + "replay-edits.json", std::ios::binary);
                writeReplayEdits(edit_out, c, r, edits);
            }
        }
        std::cout << "\n" << replaySummary(r) << "-> " << dir << "sknavmesh-export.json" << std::endl;
    }

    // The slope partition of every replayed tile, for the stair oracle:
    // SKNAV_PARTITION_DUMP names a census, SKNAV_PARTITION_DUMP_OUT the text
    // file of each tile's walkable triangles with their group, kind and region.
    template<> template<>
    void sknavreplay_object::test<7>()
    {
        set_test_name("dump a census's slope partition, when one is named");
        const std::string path = LLStringUtil::getenv("SKNAV_PARTITION_DUMP");
        if (path.empty())
        {
            return;
        }
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        options.mRounds = 1;
        ReplayResult r;
        replayCensus(c, options, r);
        std::ofstream out(LLStringUtil::getenv("SKNAV_PARTITION_DUMP_OUT"), std::ios::binary);
        ensure("output", out.good());
        out.precision(9);
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildInput& in = *tile.mInput;
                std::vector<LLVector3> verts, tris;
                std::vector<S32> idx;
                std::vector<U8> areas, two;
                std::vector<LLUUID> roots;
                const F32 cos_slope = std::cos(in.mParams[0].mSlope * DEG_TO_RAD);
                const auto add = [&](const std::vector<LLVector3>& t, U8 area, bool two_sided, const LLUUID& root) {
                    for (size_t i = 0; i + 2 < t.size(); i += 3)
                    {
                        LLVector3 n = (t[i + 1] - t[i]) % (t[i + 2] - t[i]);
                        n.normalize();
                        const F32 up = two_sided ? std::fabs(n.mV[VZ]) : n.mV[VZ];
                        for (S32 k = 0; k < 3; ++k)
                        {
                            idx.push_back(static_cast<S32>(verts.size()));
                            verts.push_back(t[i + k]);
                        }
                        areas.push_back(up > cos_slope ? area : 0);
                        two.push_back(two_sided ? 1 : 0);
                        roots.push_back(root);
                    }
                };
                for (const RecordPtr& rec : in.mRecords)
                {
                    if (rec)
                    {
                        tris.clear();
                        emitRecord(*rec, in.mMeshOrigin, tris);
                        add(tris, AREA_OBJECT, rec->mProvenance == Provenance::HULL || rec->mProvenance == Provenance::BASE_HULL, rec->mRoot);
                    }
                }
                const size_t objects = areas.size();
                const TerrainWindow& t = in.mTerrain;
                tris.clear();
                for (S32 y = 0; t.mWidth >= 2 && y + 1 < t.mHeight; ++y)
                {
                    for (S32 x = 0; x + 1 < t.mWidth; ++x)
                    {
                        const auto at = [&](S32 i, S32 j) { return LLVector3(t.mOrigin.mV[VX] + i * t.mSpacing, t.mOrigin.mV[VY] + j * t.mSpacing, t.mZ[j * t.mWidth + i]); };
                        tris.insert(tris.end(), { at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y), at(x + 1, y + 1), at(x, y + 1) });
                    }
                }
                add(tris, AREA_GROUND, false, LLUUID::null);
                if (objects == 0)
                {
                    continue;
                }
                std::vector<U8> cues(roots.size(), CUE_NONE);
                for (size_t i = 0; i < roots.size(); ++i)
                {
                    const auto it = std::lower_bound(in.mRootCues.begin(), in.mRootCues.end(), std::make_pair(roots[i], U8(0)));
                    cues[i] = it != in.mRootCues.end() && it->first == roots[i] ? it->second : static_cast<U8>(CUE_NONE);
                }
                const SlopePartition p = partitionSlopes(verts, idx, areas, two, AREA_OBJECT, in.mParams[0].mClimb, 63, cues);
                out << "tile " << key.mX << ' ' << key.mY << ' ' << in.mMeshOrigin.mdV[VX] << ' ' << in.mMeshOrigin.mdV[VY] << ' '
                    << in.mMeshOrigin.mdV[VZ] << ' ' << in.mTileX * in.mTileSize << ' ' << in.mTileY * in.mTileSize << ' ' << in.mTileSize << '\n';
                for (size_t i = 0; i < areas.size(); ++i)
                {
                    if (!areas[i])
                    {
                        continue;
                    }
                    const S32 g = i < p.mGroup.size() ? p.mGroup[i] : -1;
                    out << "t";
                    for (S32 k = 0; k < 3; ++k)
                    {
                        const LLVector3& v = verts[idx[i * 3 + k]];
                        out << ' ' << v.mV[VX] << ' ' << v.mV[VY] << ' ' << v.mV[VZ];
                    }
                    out << ' ' << static_cast<S32>(areas[i]) << ' ' << g << ' ' << (g >= 0 ? static_cast<S32>(p.mKind[g]) : -1) << ' '
                        << (i < p.mRegion.size() ? p.mRegion[i] : -1) << ' ' << roots[i] << '\n';
                }
                for (const Flight& f : p.mFlights)
                {
                    out << "f " << f.mBottom.mV[VX] << ' ' << f.mBottom.mV[VY] << ' ' << f.mBottom.mV[VZ] << ' ' << f.mTop.mV[VX] << ' '
                        << f.mTop.mV[VY] << ' ' << f.mTop.mV[VZ] << ' ' << static_cast<S32>(f.mKind) << ' ' << static_cast<S32>(f.mRule) << ' '
                        << static_cast<S32>(f.mTreads) << ' ' << f.mRise << ' ' << f.mRun << '\n';
                }
            }
        }
    }

    // SKNAV_REPLAY_IDENTITY names a census: every tile built while measuring
    // itself must pack to the same product as the tile built without.
    template<> template<>
    void sknavreplay_object::test<8>()
    {
        set_test_name("a tile measuring itself builds the tile it builds without, when a census is named");
        const std::string path = LLStringUtil::getenv("SKNAV_REPLAY_IDENTITY");
        if (path.empty())
        {
            return;
        }
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        options.mAir = AirMode::EVERYWHERE;
        options.mRounds = 1;
        options.mStats = true;
        ReplayResult r;
        replayCensus(c, options, r);
        const auto packed = [](TileBuildResult result) {
            result.mMillis = 0.f;
            result.mExcludeMillis = 0.f;
            return packTileProduct(result, 0);
        };
        U32 tiles = 0, differ = 0;
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildResult plain = buildTile(*tile.mInput);
                const bool same = packed(plain) == packed(*tile.mResult) && plain.mStatus == tile.mResult->mStatus &&
                                  (!plain.mAir) == (!tile.mResult->mAir) &&
                                  (!plain.mAir || packTileAir(*plain.mAir) == packTileAir(*tile.mResult->mAir));
                differ += same ? 0 : 1;
                ++tiles;
            }
        }
        std::cout << tiles << " tiles, " << differ << " differ measured from unmeasured" << std::endl;
        ensure_equals("tiles that differ", differ, 0u);
    }

    // SKNAV_REPLAY_DUMP names a census, SKNAV_REPLAY_DUMP_OUT a file: one line of
    // digests per tile, so two builds of the pipeline can be diffed tile by tile.
    template<> template<>
    void sknavreplay_object::test<9>()
    {
        set_test_name("dump every tile's product digests, when a census is named");
        const std::string path = LLStringUtil::getenv("SKNAV_REPLAY_DUMP");
        if (path.empty())
        {
            return;
        }
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        options.mAir = LLStringUtil::getenv("SKNAV_REPLAY_AIR") == "radius" ? AirMode::RADIUS : AirMode::EVERYWHERE;
        options.mRounds = 1;
        options.mStats = true;
        ReplayResult r;
        replayCensus(c, options, r);
        std::ofstream out(LLStringUtil::getenv("SKNAV_REPLAY_DUMP_OUT"), std::ios::binary);
        ensure("output", out.good());
        const auto bytes = [](const std::vector<U8>& v) {
            U64 h = mix(0, v.size());
            for (const U8 b : v)
            {
                h = mix(h, b);
            }
            return h;
        };
        const auto vec = [](U64 h, const LLVector3& v) { return mixF32(mixF32(mixF32(h, v.mV[0]), v.mV[1]), v.mV[2]); };
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildResult& res = *tile.mResult;
                out << "{\"mesh\":\"" << mesh->mId << "\",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"status\":" << static_cast<S32>(res.mStatus)
                    << ",\"air_built\":" << res.mAirBuilt;
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    U64 seg = 0, link = 0, flight = 0;
                    for (const WallSegment& g : res.mSegments[cls])
                    {
                        seg = mix(mixF32(vec(vec(seg, g.mA), g.mB), g.mValue), static_cast<U64>(g.mKind) | static_cast<U64>(g.mArea) << 8);
                    }
                    for (const DropLink& l : res.mLinks[cls])
                    {
                        link = mix(mixF32(vec(vec(link, l.mFrom), l.mTo), l.mHeight), static_cast<U64>(l.mKind));
                    }
                    for (const Flight& f : res.mFlights[cls])
                    {
                        flight = mix(vec(vec(flight, f.mBottom), f.mTop), f.mKey);
                        for (const U8 code : f.mCodes)
                        {
                            flight = mix(flight, code >= AREA_CODE_BASE ? code - AREA_TOP_ONLY : code);   // the partition's own code
                        }
                        for (const U16 p : f.mPolys)
                        {
                            flight = mix(flight, p);
                        }
                    }
                    const ClassBuildStats* cs = tile.mStats ? &tile.mStats->mClass[cls] : nullptr;
                    out << ",\"c" << cls << "\":{\"polys\":" << res.mPolys[cls] << ",\"detour\":\"" << std::hex << bytes(res.mData[cls])
                        << "\",\"segments\":\"" << seg << "\",\"links\":\"" << link << "\",\"flights\":\"" << flight << "\",\"hf\":\""
                        << (cs ? cs->mHfHash : 0) << "\",\"hf_area\":\"" << (cs ? cs->mHfAreaHash : 0) << "\",\"chf\":\""
                        << (cs ? cs->mChfHash : 0) << "\"" << std::dec << ",\"nseg\":" << res.mSegments[cls].size()
                        << ",\"nlink\":" << res.mLinks[cls].size() << ",\"nflight\":" << res.mFlights[cls].size()
                        << ",\"bridges\":" << (cs ? cs->mBridges : 0) << "}";
                }
                if (tile.mStats)
                {
                    const ClassBuildStats& ts = tile.mStats->mTop;
                    out << ",\"top\":{\"hf\":\"" << std::hex << ts.mHfHash << "\",\"hf_area\":\"" << ts.mHfAreaHash << std::dec
                        << "\",\"bridges\":" << ts.mBridges << "}";
                }
                out << ",\"nledge\":" << res.mLedges.size() << ",\"nrunoff\":" << res.mRunoff.size();
                U64 face = 0;
                for (const WallFace& f : res.mFaces)
                {
                    face = mix(vec(vec(face, f.mMin), f.mMax), static_cast<U64>(f.mSide) | static_cast<U64>(f.mArea) << 8);
                }
                out << ",\"faces\":\"" << std::hex << face << std::dec << "\",\"nface\":" << res.mFaces.size();
                if (res.mAir)
                {
                    U64 walk = 0, area = 0, rest = 0;
                    for (const AirGap& g : res.mAir->mGaps)
                    {
                        walk = mix(walk, static_cast<U64>(g.mFloor) | static_cast<U64>(g.mFlags & 1) << 16);
                        area = mix(area, g.mFlags & AirGap::AREA_MASK);
                        rest = mix(rest, static_cast<U64>(g.mCeiling) | static_cast<U64>(g.mFlags & 0x0e) << 16 | static_cast<U64>(g.mClear) << 24);
                    }
                    // The air as it was before AVATAR's floor was marked on it, and that mark apart.
                    TileAir plain = *res.mAir;
                    U32 avatar_floors = 0;
                    for (AirGap& g : plain.mGaps)
                    {
                        avatar_floors += (g.mFlags & 0x40) ? 1 : 0;
                        g.mFlags &= static_cast<U8>(~0x40);
                    }
                    for (S32 k = 0; k < 4; ++k)
                    {
                        for (AirGap& g : plain.mSeams[k].mGaps)
                        {
                            g.mFlags &= static_cast<U8>(~0x40);
                        }
                    }
                    out << ",\"air_plain\":\"" << std::hex << bytes(packTileAir(plain)) << std::dec << "\",\"avatar_floors\":" << avatar_floors;
                    out << ",\"air\":\"" << std::hex << bytes(packTileAir(*res.mAir)) << "\",\"air_walk\":\"" << walk << "\",\"air_area\":\"" << area
                        << "\",\"air_rest\":\"" << rest << std::dec << "\",\"gaps\":" << res.mAir->mGaps.size()
                        << ",\"leaves\":" << res.mAir->mLeaves.size();
                }
                if (tile.mStats)
                {
                    const TileBuildStats& st = *tile.mStats;
                    out << ",\"top_only_bridges\":" << st.mTopOnlyBridges << ",\"avatar_only_bridges\":" << st.mAvatarOnlyBridges
                        << ",\"own_columns\":" << st.mOwnColumns << ",\"excluded\":" << st.mExcludedRecords << ",\"window_cells\":" << st.mWindowCells
                        << ",\"window_rect\":[" << st.mWindowRect[0] << ',' << st.mWindowRect[1] << ',' << st.mWindowRect[2] << ','
                        << st.mWindowRect[3] << ']';
                    // AVATAR's walkable spans after erosion, a digest per 2 m block of the tile.
                    std::map<S32, U64> blocks;
                    for (const StatSpan& s : st.mSpans)
                    {
                        U64& h = blocks[(s.mX / 16) + (s.mY / 16) * 1024];
                        h = mix(h, static_cast<U64>(s.mX) | static_cast<U64>(s.mY) << 16 | static_cast<U64>(s.mTop) << 32);
                    }
                    out << ",\"span_blocks\":{";
                    bool first = true;
                    for (const auto& [b, h] : blocks)
                    {
                        out << (first ? "" : ",") << "\"" << b % 1024 << ',' << b / 1024 << "\":\"" << std::hex << h << std::dec << '"';
                        first = false;
                    }
                    out << '}';
                }
                std::vector<U64> keys;
                for (const Flight& f : res.mFlights[0])
                {
                    keys.push_back(f.mKey);
                }
                std::sort(keys.begin(), keys.end());
                out << ",\"flight_keys\":[";
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    out << (i ? ",\"" : "\"") << std::hex << keys[i] << std::dec << '"';
                }
                out << "],\"ms\":" << (tile.mStats ? tile.mStats->mTotalMs : 0.0) << "}\n";
            }
        }

        // The spans AVATAR's polys cover, per tile: x, y and top as three U16s, base64.
        std::ofstream cover(LLStringUtil::getenv("SKNAV_REPLAY_DUMP_OUT") + ".cover", std::ios::binary);
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                if (!tile.mStats)
                {
                    continue;
                }
                const TileBuildStats& st = *tile.mStats;
                std::vector<U8> covered(st.mSpans.size(), 0);
                for (const U32 i : st.mPolySpans)
                {
                    covered[i] = 1;
                }
                std::vector<U8> packed;
                for (size_t i = 0; i < st.mSpans.size(); ++i)
                {
                    if (covered[i])
                    {
                        for (const U16 v : { st.mSpans[i].mX, st.mSpans[i].mY, st.mSpans[i].mTop })
                        {
                            packed.push_back(static_cast<U8>(v));
                            packed.push_back(static_cast<U8>(v >> 8));
                        }
                    }
                }
                cover << "{\"mesh\":\"" << mesh->mId << "\",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"spans\":\""
                      << LLBase64::encode(packed.data(), packed.size()) << "\"}\n";
            }
        }

        // Whether AVATAR's mesh crosses each door: from just before its plane
        // to just past it, at its sill.
        std::ofstream doors(LLStringUtil::getenv("SKNAV_REPLAY_DUMP_OUT") + ".doors", std::ios::binary);
        for (const DoorEntity& d : c.mDoors)
        {
            S32 state = 0;  // 0 off every mesh, 1 one side on, 2 both on but no way through, 3 through
            F32 length = 0.f;
            for (const auto& mesh : r.mMeshes)
            {
                const dtNavMesh* nav = mesh->mNav[0];
                if (!nav || !d.mPlaneKnown)
                {
                    continue;
                }
                dtNavMeshQuery* query = dtAllocNavMeshQuery();
                query->init(nav, 4096);
                dtQueryFilter filter;
                const F32 ext[3] = { 0.4f, 1.f, 0.4f };
                F32 at[2][3], snapped[2][3];
                dtPolyRef ref[2] = { 0, 0 };
                for (S32 k = 0; k < 2; ++k)
                {
                    const LLVector3d p = d.mPlanePoint + LLVector3d(d.mNormal * (k ? 0.75f : -0.75f)) + LLVector3d(0.0, 0.0, 0.3) - mesh->mOrigin;
                    toRecast(LLVector3(static_cast<F32>(p.mdV[VX]), static_cast<F32>(p.mdV[VY]), static_cast<F32>(p.mdV[VZ])), at[k]);
                    query->findNearestPoly(at[k], ext, &filter, &ref[k], snapped[k]);
                }
                const S32 on = (ref[0] ? 1 : 0) + (ref[1] ? 1 : 0);
                S32 here = on == 2 ? 2 : on;
                if (on == 2)
                {
                    dtPolyRef path[512];
                    int n = 0;
                    const dtStatus st = query->findPath(ref[0], ref[1], snapped[0], snapped[1], &filter, path, &n, 512);
                    if (dtStatusSucceed(st) && !dtStatusDetail(st, DT_PARTIAL_RESULT) && n > 0 && path[n - 1] == ref[1])
                    {
                        here = 3;
                        F32 straight[512 * 3];
                        int ns = 0;
                        query->findStraightPath(snapped[0], snapped[1], path, n, straight, nullptr, nullptr, &ns, 512);
                        for (int i = 1; i < ns; ++i)
                        {
                            length += dtVdist(&straight[(i - 1) * 3], &straight[i * 3]);
                        }
                    }
                }
                dtFreeNavMeshQuery(query);
                state = std::max(state, here);
            }
            doors << "{\"door\":\"" << d.mId << "\",\"state\":" << state << ",\"length\":" << length << "}\n";
        }
    }
}
