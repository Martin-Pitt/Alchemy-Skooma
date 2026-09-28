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
#include "../sknavannotate.h"
#include "../sknavlayout.h"
#include "../sknavreplay.h"
#include "../sknavtilecache.h"
#include "sknavtestscene.h"

#include "llstring.h"

#include <chrono>
#include <fstream>
#include <iostream>
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
        c.mParams.mClasses[1] = true;
        c.mParams.mParams[1] = { 0.5f, 0.f, 0.75f, 89.f };
        c.mParams.mAir = true;
        c.mParams.mWallFaces = true;
        c.mParams.mEdges[0] = c.mParams.mEdges[1] = true;
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
        ensure("no air past the radius", !far_tile.mResult->mAirBuilt && !far_tile.mInput->mClasses[1]);
        ensure("the region's tiles keep theirs", meshOf(near_only, id(900))->mTiles.at(TileKey{ 0, 0 }).mResult->mAirBuilt);
        ensure("air everywhere when asked", meshOf(everywhere, skybox)->mTiles.begin()->second.mResult->mAirBuilt);
        c.mBuildWeather = true;
        ReplayResult weather;
        replayCensus(c, ReplayOptions(), weather);
        const ReplayTile& w = meshOf(weather, skybox)->mTiles.begin()->second;
        ensure("WEATHER still built when it is on", !w.mInput->mAir && w.mInput->mClasses[1]);
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
                                fresh->mGaps[leaf.mFirstGap + k].mFlags &= static_cast<U8>(~AirGap::WALKABLE);
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
        std::cout << "\n" << replaySummary(r) << "-> " << dir << "sknavmesh-export.json" << std::endl;
    }
}
