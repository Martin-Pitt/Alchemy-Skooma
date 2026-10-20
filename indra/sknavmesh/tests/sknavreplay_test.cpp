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

#include "../sknavairdebug.h"
#include "../sknavairdistil.h"
#include "../sknavbuildstats.h"
#include "../sknavenclosure.h"
#include "../sknavflights.h"
#include "../sknavannotate.h"
#include "../sknavlayout.h"
#include "../sknavreplay.h"
#include "../sknavreplaystats.h"
#include "../sknavresidency.h"
#include "../sknavstamp.h"
#include "../sknavtilecache.h"
#include "sknavtestscene.h"

#include "llbase64.h"
#include "llstring.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

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
        ReplayOptions unstamped;
        unstamped.mStamp = false;
        replayCensus(c, unstamped, r);
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
        set_test_name("past the air radius a tile builds no air of its own, as in the viewer, but coarse air for its stamp, unless air is asked for everywhere");
        CensusFile c = scene();
        c.mAirRadius = 50.f;                    // the skybox is some 74 m from the camera
        const LLUUID skybox = c.mRecords.back()->mRoot;
        ReplayResult near_only, everywhere, bare;
        replayCensus(c, ReplayOptions(), near_only);
        ReplayOptions all;
        all.mAir = AirMode::EVERYWHERE;
        replayCensus(c, all, everywhere);
        ReplayOptions no_coarse;
        no_coarse.mCoarseAir = false;
        replayCensus(c, no_coarse, bare);
        const ReplayTile& far_tile = meshOf(near_only, skybox)->mTiles.begin()->second;
        ensure("no air of its own past the radius", !far_tile.mInput->mAir && far_tile.mInput->mCoarseAir);
        ensure("coarse air in its place", far_tile.mResult->mAirBuilt && far_tile.mResult->mCoarseAir && far_tile.mResult->mAir);
        ensure("in the tile's own cells", far_tile.mResult->mAir->mCell == c.mParams.mCell && far_tile.mResult->mAir->mWidth == 128);
        ensure("and stamped with it", far_tile.mResult->mStamp.mStamped && far_tile.mResult->mStamp.mComponents > 0);
        ensure("none without coarse air", !meshOf(bare, skybox)->mTiles.begin()->second.mResult->mAirBuilt);
        ensure("the region's tiles keep theirs", meshOf(near_only, id(900))->mTiles.at(TileKey{ 0, 0 }).mResult->mAirBuilt &&
                                                     !meshOf(near_only, id(900))->mTiles.at(TileKey{ 0, 0 }).mResult->mCoarseAir);
        ensure("air everywhere when asked", meshOf(everywhere, skybox)->mTiles.begin()->second.mResult->mAirBuilt &&
                                                !meshOf(everywhere, skybox)->mTiles.begin()->second.mResult->mCoarseAir);
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
        // SKNAV_REPLAY_STAMP=0 polygonises each tile as it is rasterised, as before the stamp; SKNAV_REPLAY_COARSE=0 leaves far tiles airless.
        options.mStamp = LLStringUtil::getenv("SKNAV_REPLAY_STAMP") != "0";
        options.mCoarseAir = LLStringUtil::getenv("SKNAV_REPLAY_COARSE") != "0";
        options.mKeepRasters = !LLStringUtil::getenv("SKNAV_REPLAY_EDITS").empty();
        // SKNAV_REPLAY_FINE=1 builds the fine air over the narrow openings and measures it.
        options.mFineAir = LLStringUtil::getenv("SKNAV_REPLAY_FINE") == "1";
        // SKNAV_REPLAY_FACE_OFFSETS=0 leaves faces at their cell boundaries; SKNAV_REPLAY_FACE_TRACE=n traces every n-th face for the oracle.
        options.mFaceOffsets = LLStringUtil::getenv("SKNAV_REPLAY_FACE_OFFSETS") != "0";
        options.mAlignWallEdges = LLStringUtil::getenv("SKNAV_REPLAY_ALIGN_EDGES") != "0";
        const std::string face_trace = LLStringUtil::getenv("SKNAV_REPLAY_FACE_TRACE");
        if (!face_trace.empty())
        {
            options.mFaceTraceEvery = std::stoi(face_trace);
        }
        const std::string face_box = LLStringUtil::getenv("SKNAV_REPLAY_FACE_TRACE_BOX");
        if (!face_box.empty())
        {
            std::istringstream in(face_box);
            char comma;
            in >> options.mFaceTraceBox[0] >> comma >> options.mFaceTraceBox[1] >> comma >> options.mFaceTraceBox[2] >> comma >> options.mFaceTraceBox[3];
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
        if (options.mStats)
        {
            // SKNAV_REPLAY_STATS: the measurements; SKNAV_REPLAY_EDITS names "record op" lines to rebuild with.
            std::ofstream measure(dir + "replay-measure.json", std::ios::binary);
            writeReplayStats(measure, c, r);
            const std::string fine_tiles = LLStringUtil::getenv("SKNAV_FINE_DUMP_TILES");
            if (!fine_tiles.empty())
            {
                std::ofstream fd(dir + "fine-dump.jsonl", std::ios::binary);
                std::istringstream list(fine_tiles);
                std::string item;
                while (std::getline(list, item, ';'))
                {
                    S32 m = 0, tx = 0, ty = 0;
                    if (sscanf(item.c_str(), "%d,%d,%d", &m, &tx, &ty) != 3 || m >= static_cast<S32>(r.mMeshes.size()))
                    {
                        continue;
                    }
                    const auto it = r.mMeshes[m]->mTiles.find(TileKey{ tx, ty });
                    if (it == r.mMeshes[m]->mTiles.end() || !it->second.mResult || !it->second.mResult->mAir || !it->second.mResult->mFineAir)
                    {
                        continue;
                    }
                    const TileAir& air = *it->second.mResult->mAir;
                    const FineAir& fa = *it->second.mResult->mFineAir;
                    fd << "{\"mesh\":" << m << ",\"x\":" << tx << ",\"y\":" << ty << ",\"cell\":" << air.mCell << ",\"unit\":" << air.mUnit
                       << ",\"origin\":[" << air.mOrigin.mV[0] << ',' << air.mOrigin.mV[1] << ',' << air.mOrigin.mV[2] << "],\"w\":" << air.mWidth
                       << ",\"h\":" << air.mHeight << ",\"coarse\":[";
                    bool first = true;
                    for (S32 iy = 0; iy < air.mHeight; ++iy)
                    {
                        for (S32 ix = 0; ix < air.mWidth; ++ix)
                        {
                            const S32 leaf = findLeafByKey(air, ix, iy);
                            if (leaf < 0)
                            {
                                continue;
                            }
                            const AirLeaf& l = air.mLeaves[leaf];
                            fd << (first ? "" : ",") << '[' << ix << ',' << iy;
                            for (U32 g = 0; g < l.mGapCount; ++g)
                            {
                                const AirGap& gap = air.mGaps[l.mFirstGap + g];
                                fd << ',' << gap.mFloor << ',' << gap.mCeiling << ',' << static_cast<S32>(gap.mFlags);
                            }
                            fd << ']';
                            first = false;
                        }
                    }
                    fd << "],\"fine\":[";
                    for (size_t i = 0; i < fa.mLeaves.size(); ++i)
                    {
                        const FineLeaf& l = fa.mLeaves[i];
                        fd << (i ? "," : "") << '[' << l.mX << ',' << l.mY;
                        for (U32 g = 0; g < l.mGapCount; ++g)
                        {
                            const FineGap& gap = fa.mGaps[l.mFirstGap + g];
                            fd << ',' << gap.mFloor << ',' << gap.mCeiling << ',' << static_cast<S32>(gap.mFlags);
                        }
                        fd << ']';
                    }
                    fd << "]}\n";
                }
            }
            if (options.mFaceTraceEvery > 0)
            {
                std::ofstream trace(dir + "face-trace.json", std::ios::binary);
                trace << std::setprecision(9) << "[";
                bool first = true;
                for (const auto& mesh : r.mMeshes)
                {
                    for (const auto& [key, tile] : mesh->mTiles)
                    {
                        if (!tile.mStats || !tile.mResult || !tile.mResult->mAir || tile.mResult->mCoarseAir)
                        {
                            continue;
                        }
                        const TileAir& air = *tile.mResult->mAir;
                        for (const FaceOffsetTrace& t : tile.mStats->mFaceTrace)
                        {
                            // Traced over the padded grid: a side off the tile proper reads no table.
                            const S32 ix = static_cast<S32>(std::lround((t.mX0 - air.mOrigin.mV[VX]) / air.mCell));
                            const S32 iy = static_cast<S32>(std::lround((t.mY0 - air.mOrigin.mV[VY]) / air.mCell));
                            if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
                            {
                                continue;
                            }
                            trace << (first ? "" : ",") << "\n{\"tile\":[" << key.mX << "," << key.mY << "],\"x0\":" << t.mX0 << ",\"y0\":" << t.mY0
                                  << ",\"cell\":" << air.mCell
                                  << ",\"side\":" << t.mSide << ",\"floor\":" << t.mFloor << ",\"z\":[" << t.mZLo << "," << t.mZHi
                                  << "],\"offset\":" << static_cast<S32>(t.mOffset) << ",\"table\":"
                                  << faceOffset(air.mFaceOffsets, faceKey(ix, iy, air.mWidth, t.mSide)) * FACE_OFFSET_STEPS
                                  << ",\"tris\":[";
                            for (size_t i = 0; i < t.mTris.size(); ++i)
                            {
                                trace << (i ? "," : "") << t.mTris[i].mV[VX] << "," << t.mTris[i].mV[VY] << "," << t.mTris[i].mV[VZ];
                            }
                            trace << "]}";
                            first = false;
                        }
                    }
                }
                trace << "]\n";
            }
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
                const SlopePartition p = partitionSlopes(verts, idx, areas, two, AREA_OBJECT, in.mParams[0].mClimb, in.mParams[0].mSlope, 63, cues);
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
                if (tile.mStats)
                {
                    const auto keys = [&](const char* name, const std::vector<U64>& list) {
                        out << ",\"" << name << "\":[";
                        for (size_t k = 0; k < list.size(); ++k)
                        {
                            out << (k ? "," : "") << '"' << std::hex << list[k] << std::dec << '"';
                        }
                        out << ']';
                    };
                    keys("stand_only", tile.mStats->mFlightStandOnly);
                    keys("polys_only", tile.mStats->mFlightPolysOnly);
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

    // SKNAV_STAMP_DUMP names a census, SKNAV_STAMP_DUMP_OUT a file: per tile, the
    // compact heightfield's spans, the stamp input and what stampTile made of
    // them, for the Python oracle to stamp again from the rules alone.
    template<> template<>
    void sknavreplay_object::test<10>()
    {
        set_test_name("dump each tile's stamp with what it was stamped from, when a census is named");
        const std::string path = LLStringUtil::getenv("SKNAV_STAMP_DUMP");
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
        ReplayResult r;
        replayCensus(c, options, r);
        const std::string limit = LLStringUtil::getenv("SKNAV_STAMP_DUMP_TILES");
        const size_t most = limit.empty() ? ~size_t(0) : static_cast<size_t>(std::stoul(limit));
        std::ofstream out(LLStringUtil::getenv("SKNAV_STAMP_DUMP_OUT"), std::ios::binary);
        ensure("output", out.good());
        out.precision(9);
        const auto b64 = [](const void* p, size_t n) { return LLBase64::encode(static_cast<const U8*>(p), n); };
        size_t written = 0;
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                if (written >= most || !tile.mStampInput || !tile.mResult->mAir)
                {
                    continue;
                }
                // The raster again: the stage is deterministic, so this is the one the replay stamped.
                TileBuildResult again;
                const TileRasterPtr raster = rasteriseTile(*tile.mInput, again);
                if (!raster)
                {
                    continue;
                }
                const rcCompactHeightfield& chf = *raster->mChf;
                const S32 border = borderCells(tile.mInput->mParams[0].mRadius, tile.mInput->mCell);
                TileStampInput in;
                const TileEnclosure* enc = mesh->mEnclosure ? mesh->mEnclosure->tile(key) : nullptr;
                if (enc && mesh->mRooms && enc->mGaps.size() == again.mAir->mGaps.size())
                {
                    const LLVector3d lo = tile.mMin - mesh->mOrigin, hi = tile.mMax - mesh->mOrigin;
                    stampInput(*again.mAir, *enc, *mesh->mRooms, LLVector3(lo), LLVector3(hi), in);
                }
                TileStamp st;
                stampTile(chf, border, raster->mSpanGap, in, raster->mReal, raster->mFlights, StampParams(), st);
                std::vector<S32> spans;
                spans.reserve(static_cast<size_t>(chf.spanCount) * 9);
                for (S32 z = 0; z < chf.height; ++z)
                {
                    for (S32 x = 0; x < chf.width; ++x)
                    {
                        const rcCompactCell& cell = chf.cells[x + z * chf.width];
                        for (U32 i = cell.index, n = cell.index + cell.count; i < n; ++i)
                        {
                            spans.insert(spans.end(), { x, z, chf.spans[i].y, chf.areas[i] });
                            for (S32 d = 0; d < 4; ++d)
                            {
                                const S32 con = rcGetCon(chf.spans[i], d);
                                spans.push_back(con == RC_NOT_CONNECTED ? -1
                                                                        : static_cast<S32>(chf.cells[(x + rcGetDirOffsetX(d)) + (z + rcGetDirOffsetY(d)) * chf.width].index) + con);
                            }
                            spans.push_back(static_cast<S32>(i < raster->mSpanGap.size() ? raster->mSpanGap[i] : SPAN_NO_GAP));
                        }
                    }
                }
                std::vector<U8> flight_codes;
                for (const Flight& f : raster->mFlights)
                {
                    flight_codes.insert(flight_codes.end(), f.mCodes.begin(), f.mCodes.end());
                }
                out << "{\"mesh\":\"" << mesh->mId << "\",\"x\":" << key.mX << ",\"y\":" << key.mY << ",\"border\":" << border << ",\"w\":" << chf.width
                    << ",\"h\":" << chf.height << ",\"cs\":" << chf.cs << ",\"ch\":" << chf.ch << ",\"bmin\":[" << chf.bmin[0] << ',' << chf.bmin[1] << ','
                    << chf.bmin[2] << "],\"reach\":" << STAMP_PORTAL_REACH_M << ",\"palette_max\":63,\"spans\":\"" << b64(spans.data(), spans.size() * 4)
                    << "\",\"gap_component\":\"" << b64(in.mGapComponent.data(), in.mGapComponent.size() * 2) << "\",\"component_space\":[";
                for (size_t k = 0; k < in.mComponentSpace.size(); ++k)
                {
                    out << (k ? "," : "") << in.mComponentSpace[k];
                }
                out << "],\"portals\":[";
                for (size_t k = 0; k < in.mPortals.size(); ++k)
                {
                    const StampPortal& p = in.mPortals[k];
                    out << (k ? "," : "") << '[';
                    for (const LLVector3* v : { &p.mCentre, &p.mNormal, &p.mA, &p.mB })
                    {
                        out << v->mV[0] << ',' << v->mV[1] << ',' << v->mV[2] << ',';
                    }
                    out << p.mSide[0] << ',' << p.mSide[1] << ',' << static_cast<S32>(p.mKind) << ']';
                }
                out << "],\"real\":[";
                for (size_t k = 0; k < raster->mReal.size(); ++k)
                {
                    out << (k ? "," : "") << static_cast<S32>(raster->mReal[k]);
                }
                out << "],\"flight_codes\":[";
                for (size_t k = 0; k < flight_codes.size(); ++k)
                {
                    out << (k ? "," : "") << static_cast<S32>(flight_codes[k]);
                }
                out << "],\"span_component\":\"" << b64(st.mSpanComponent.data(), st.mSpanComponent.size() * 2) << "\",\"span_code\":\""
                    << b64(st.mSpanCode.data(), st.mSpanCode.size()) << "\",\"colours\":[";
                for (size_t k = 0; k < st.mComponentColour.size(); ++k)
                {
                    out << (k ? "," : "") << static_cast<S32>(st.mComponentColour[k]);
                }
                out << "],\"palette\":[";
                for (size_t k = 1; k < st.mPalette.size(); ++k)
                {
                    out << (k > 1 ? "," : "") << '[' << static_cast<S32>(st.mPalette[k].mSlope) << ',' << static_cast<S32>(st.mPalette[k].mColour) << ']';
                }
                out << "],\"overflow\":" << static_cast<S32>(st.mOverflow) << ",\"door\":" << st.mDoorSpans << ",\"flood\":" << st.mFloodSpans
                    << ",\"pattern_replay\":\"" << std::hex << tile.mStampInput->mPattern << "\",\"pattern\":\"" << in.mPattern << std::dec << "\"}\n";
                ++written;
            }
        }
        std::cout << written << " tiles dumped" << std::endl;
    }

    template<> template<>
    void sknavreplay_object::test<11>()
    {
        set_test_name("a stamped replay's polys each lie in one space, every tile stamped, the bucket flags on Detour's polys");
        const CensusFile c = scene();
        ReplayResult r;
        replayCensus(c, ReplayOptions(), r);
        const ReplayMesh* land = meshOf(r, id(900));
        ensure("rooms", land && land->mRooms);
        std::set<U32> spaces;
        U32 door_edges = 0, door_polys = 0, impure = 0, unstamped = 0;
        for (const auto& [key, tile] : land->mTiles)
        {
            const TileBuildResult& res = *tile.mResult;
            if (res.mStatus != TileBuildResult::SUCCESS || !res.mPolys[0])
            {
                continue;
            }
            unstamped += res.mStamp.mStamped && tile.mBinding ? 0 : 1;
            impure += res.mStamp.mImpurePolys;
            for (size_t p = 0; p < res.mStamp.mPolyComponent.size(); ++p)
            {
                const U16 comp = res.mStamp.mPolyComponent[p];
                if (comp != STAMP_NONE && tile.mBinding && tile.mBinding->mComponentSpaceId[comp])
                {
                    spaces.insert(tile.mBinding->mComponentSpaceId[comp]);
                }
                door_polys += tile.mBinding && (tile.mBinding->mPolyFlags[p] & POLY_DOOR) ? 1 : 0;
            }
            for (const StampPortalEdge& e : res.mStamp.mPortalEdges)
            {
                door_edges += res.mStamp.mPortals[e.mPortal].mKind == static_cast<U8>(PortalKind::DOOR) ? 1 : 0;
            }
        }
        ensure_equals("every tile with polys stamped", unstamped, 0u);
        ensure_equals("no poly over two spaces", impure, 0u);
        ensure("polys in at least the two rooms and outdoors", spaces.size() >= 3);
        // The scene's doorway is under 2.2 m, lower than the avatar: the door's own edges are sknavstamp's test 6.
        (void)door_edges;
        (void)door_polys;
        // The flags reached Detour.
        const dtNavMesh* nav = land->mNav[0];
        U32 flagged = 0;
        for (S32 t = 0; nav && t < nav->getMaxTiles(); ++t)
        {
            const dtMeshTile* dt = nav->getTile(t);
            for (S32 p = 0; dt && dt->header && p < dt->header->polyCount; ++p)
            {
                flagged += (dt->polys[p].flags & (POLY_INDOORS | POLY_OUTDOORS | POLY_SHELTERED)) ? 1 : 0;
            }
        }
        ensure("bucket flags on Detour's polys", flagged > 0);
    }

    // SKNAV_AIR_OVERLAY_CENSUS names a census, SKNAV_AIR_OVERLAY_OUT a folder: one
    // top view per air overlay mode, gathered as the viewer gathers it.
    template<> template<>
    void sknavreplay_object::test<12>()
    {
        set_test_name("render the air overlay's modes headless, when a census is named");
        const std::string path = LLStringUtil::getenv("SKNAV_AIR_OVERLAY_CENSUS");
        if (path.empty())
        {
            return;
        }
        // SKNAV_AIR_OVERLAY_AIR=radius builds air near the census camera only, as the viewer does; files then take a radius_ prefix.
        const bool radius = LLStringUtil::getenv("SKNAV_AIR_OVERLAY_AIR") == "radius";
        const std::string dir = LLStringUtil::getenv("SKNAV_AIR_OVERLAY_OUT") + (radius ? "/radius_" : "/");
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        options.mAir = radius ? AirMode::RADIUS : AirMode::EVERYWHERE;
        ReplayResult r;
        replayCensus(c, options, r);

        // A software stand-in for the overlay's draw: fills by height, then lines, alpha-blended.
        struct Canvas : AirDebugSink
        {
            struct Quad { LLVector3 p[4]; LLColor4 c; F32 z; };
            struct Seg { LLVector3 a, b; LLColor4 c; };
            std::vector<Quad> mQuads;
            std::vector<Seg> mSegs;
            LLVector3 mOffset;
            U32 mBudget = 1000000, mUsed = 0;
            bool spend(U32 n) { if (mUsed + n > mBudget) return false; mUsed += n; return true; }
            bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& cc, const LLVector3& d, const LLColor4& col) override
            {
                if (!spend(6)) return false;
                mQuads.push_back({ { a + mOffset, b + mOffset, cc + mOffset, d + mOffset }, col, a.mV[VZ] + mOffset.mV[VZ] });
                return true;
            }
            bool line(const LLVector3& a, const LLVector3& b, const LLColor4& col) override
            {
                if (!spend(2)) return false;
                mSegs.push_back({ a + mOffset, b + mOffset, col });
                return true;
            }
            void write(const std::string& file, F32 half, bool oblique) const
            {
                const S32 W = 800, H = 800;
                std::vector<F32> img(W * H * 3);
                for (S32 i = 0; i < W * H; ++i) { img[i * 3] = 16 / 255.f; img[i * 3 + 1] = 16 / 255.f; img[i * 3 + 2] = 20 / 255.f; }
                const F32 scale = W / (2.f * half);
                auto px = [&](const LLVector3& p) {
                    const F32 y = oblique ? p.mV[VY] * 0.5f + p.mV[VZ] : p.mV[VY];
                    return LLVector2((p.mV[VX] + half) * scale, H - (y + half) * scale);
                };
                auto blend = [&](S32 x, S32 y, const LLColor4& col) {
                    if (x < 0 || y < 0 || x >= W || y >= H) return;
                    F32* o = &img[(y * W + x) * 3];
                    for (S32 k = 0; k < 3; ++k) o[k] = o[k] * (1.f - col.mV[3]) + col.mV[k] * col.mV[3];
                };
                std::vector<const Quad*> order;
                for (const Quad& q : mQuads) order.push_back(&q);
                std::stable_sort(order.begin(), order.end(), [](const Quad* a, const Quad* b) { return a->z < b->z; });
                for (const Quad* q : order)
                {
                    LLVector2 v[4];
                    F32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    for (S32 k = 0; k < 4; ++k)
                    {
                        v[k] = px(q->p[k]);
                        x0 = llmin(x0, v[k].mV[0]); x1 = llmax(x1, v[k].mV[0]); y0 = llmin(y0, v[k].mV[1]); y1 = llmax(y1, v[k].mV[1]);
                    }
                    if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) continue;
                    for (S32 y = llmax(0, S32(std::floor(y0))); y <= llmin(H - 1, S32(std::ceil(y1))); ++y)
                    {
                        for (S32 x = llmax(0, S32(std::floor(x0))); x <= llmin(W - 1, S32(std::ceil(x1))); ++x)
                        {
                            const F32 sx = x + 0.5f, sy = y + 0.5f;
                            bool pos = true, neg = true;
                            for (S32 k = 0; k < 4; ++k)
                            {
                                const LLVector2& a = v[k];
                                const LLVector2& b = v[(k + 1) % 4];
                                const F32 cr = (b.mV[0] - a.mV[0]) * (sy - a.mV[1]) - (b.mV[1] - a.mV[1]) * (sx - a.mV[0]);
                                pos = pos && cr >= 0.f;
                                neg = neg && cr <= 0.f;
                            }
                            if (pos || neg) blend(x, y, q->c);
                        }
                    }
                }
                for (const Seg& s : mSegs)
                {
                    const LLVector2 a = px(s.a), b = px(s.b);
                    const F32 len = llmax(std::fabs(b.mV[0] - a.mV[0]), std::fabs(b.mV[1] - a.mV[1]));
                    const S32 steps = llmax(1, S32(std::ceil(len)));
                    if (steps > 4 * W) continue;
                    for (S32 i = 0; i <= steps; ++i)
                    {
                        const F32 t = F32(i) / steps;
                        blend(S32(a.mV[0] + (b.mV[0] - a.mV[0]) * t), S32(a.mV[1] + (b.mV[1] - a.mV[1]) * t), s.c);
                    }
                }
                std::ofstream out(file, std::ios::binary);
                out << "P6\n" << W << " " << H << "\n255\n";
                for (const F32 f : img) out.put(static_cast<char>(llclamp(S32(f * 255.f + 0.5f), 0, 255)));
            }
        };

        // Centred on the census agent; the slice on its floor.
        // SKNAV_AIR_OVERLAY_CENTRE="x y z" looks somewhere other than the agent.
        LLVector3d centre = c.mAgent;
        const std::string at = LLStringUtil::getenv("SKNAV_AIR_OVERLAY_CENTRE");
        if (!at.empty())
        {
            std::istringstream(at) >> centre.mdV[VX] >> centre.mdV[VY] >> centre.mdV[VZ];
        }
        F64 agent_floor = centre.mdV[VZ] - 1.0;
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                if (!tile.mResult || !tile.mResult->mAir || tile.mMin.mdV[VX] > centre.mdV[VX] || tile.mMax.mdV[VX] < centre.mdV[VX]
                    || tile.mMin.mdV[VY] > centre.mdV[VY] || tile.mMax.mdV[VY] < centre.mdV[VY])
                {
                    continue;
                }
                F32 floor = 0.f;
                if (airFloorUnder(*tile.mResult->mAir, LLVector3(centre - mesh->mOrigin), floor))
                {
                    agent_floor = mesh->mOrigin.mdV[VZ] + floor;
                }
            }
        }
        std::ofstream log(dir + "render.txt");
        log << "centre " << centre << " floor " << agent_floor << "\n";

        struct Shot { const char* mName; AirDebugMode mMode; F32 mHalf; F32 mSlice; bool mOblique; U32 mBudget; };
        const Shot shots[] = { { "leaves", AirDebugMode::LEAVES, 24.f, 2.5f, false, 1000000 },
                               { "gaps", AirDebugMode::GAPS, 24.f, 2.5f, false, 1000000 },
                               { "gaps_oblique", AirDebugMode::GAPS, 24.f, 2.5f, true, 1000000 },
                               { "space", AirDebugMode::SPACE, 24.f, 2.5f, false, 1000000 },
                               { "stamp", AirDebugMode::STAMP, 24.f, 2.5f, false, 1000000 },
                               { "clearance", AirDebugMode::CLEARANCE, 24.f, 2.5f, false, 1000000 },
                               { "faces", AirDebugMode::FACES, 12.f, 2.5f, false, 1000000 },
                               { "sources", AirDebugMode::SPACE, 160.f, 0.f, false, 1000000 },
                               { "budget", AirDebugMode::GAPS, 96.f, 0.f, false, 200000 } };
        for (const Shot& shot : shots)
        {
            Canvas canvas;
            canvas.mBudget = shot.mBudget;
            struct Near { F64 d2; const ReplayMesh* m; TileKey k; const ReplayTile* t; };
            std::vector<Near> tiles;
            const F64 reach = shot.mHalf * 1.42;
            for (const auto& mesh : r.mMeshes)
            {
                for (const auto& [key, tile] : mesh->mTiles)
                {
                    const F64 dx = llmax(tile.mMin.mdV[VX] - centre.mdV[VX], 0.0, centre.mdV[VX] - tile.mMax.mdV[VX]);
                    const F64 dy = llmax(tile.mMin.mdV[VY] - centre.mdV[VY], 0.0, centre.mdV[VY] - tile.mMax.mdV[VY]);
                    if (dx * dx + dy * dy <= reach * reach) tiles.push_back({ dx * dx + dy * dy, mesh.get(), key, &tile });
                }
            }
            std::sort(tiles.begin(), tiles.end(), [](const Near& a, const Near& b) { return a.d2 < b.d2; });
            U32 drawn = 0, cut = 0, coarse_tiles = 0;
            for (size_t i = 0; i < tiles.size(); ++i)
            {
                const Near& n = tiles[i];
                const LLVector3d rel = n.m->mOrigin - centre;
                canvas.mOffset = LLVector3(rel);
                const F32 lo = shot.mSlice > 0.f ? F32(agent_floor - n.m->mOrigin.mdV[VZ] - shot.mSlice) : -1e30f;
                const F32 hi = shot.mSlice > 0.f ? F32(agent_floor - n.m->mOrigin.mdV[VZ] + shot.mSlice) : 1e30f;
                bool ok = true;
                if (shot.mMode == AirDebugMode::STAMP)
                {
                    const dtNavMesh* nav = n.m->mNav[static_cast<S32>(AgentClass::AVATAR)];
                    const dtMeshTile* dt = nav ? nav->getTileAt(n.k.mY, n.k.mX, 0) : nullptr;
                    if (!dt || !dt->header) continue;
                    for (S32 p = 0; ok && p < dt->header->polyCount; ++p)
                    {
                        const dtPoly& poly = dt->polys[p];
                        if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
                        const U32 id = n.m->polySpace(dt, p);
                        LLColor4 col = spaceColour(id);
                        col.mV[3] = id ? 0.55f : 0.25f;
                        const LLVector3 v0 = fromRecast(&dt->verts[poly.verts[0] * 3]);
                        if (v0.mV[VZ] < lo || v0.mV[VZ] > hi) continue;
                        for (S32 k = 1; ok && k + 1 < poly.vertCount; ++k)
                        {
                            const LLVector3 v1 = fromRecast(&dt->verts[poly.verts[k] * 3]), v2 = fromRecast(&dt->verts[poly.verts[k + 1] * 3]);
                            ok = canvas.quad(v0, v1, v2, v2, col);
                        }
                        for (S32 k = 0; ok && k < poly.vertCount; ++k)
                        {
                            const U16 nei = poly.neis[k];
                            if (nei && !(nei & DT_EXT_LINK) && n.m->polySpace(dt, nei - 1) == id) continue;
                            if (nei & DT_EXT_LINK) continue;
                            LLColor4 e = spaceColour(id);
                            e.mV[3] = 0.95f;
                            ok = canvas.line(fromRecast(&dt->verts[poly.verts[k] * 3]), fromRecast(&dt->verts[poly.verts[(k + 1) % poly.vertCount] * 3]), e);
                        }
                    }
                }
                else
                {
                    if (!n.t->mResult || !n.t->mResult->mAir) continue;
                    const TileAir& air = *n.t->mResult->mAir;
                    const bool coarse = n.t->mResult->mCoarseAir;
                    coarse_tiles += coarse ? 1 : 0;
                    AirDebugInput in;
                    in.mMode = shot.mMode;
                    in.mCoarse = coarse;
                    in.mClimb = air.mUnit > 0.f ? S32(0.75f / air.mUnit) : 0;
                    in.mCentre = LLVector3(centre - n.m->mOrigin);
                    in.mReach = F32(reach);
                    in.mSliceLo = lo;
                    in.mSliceHi = hi;
                    std::vector<U32> gap_space(air.mGaps.size(), 0);
                    const TileEnclosure* te = n.m->mEnclosure ? n.m->mEnclosure->tile(n.k) : nullptr;
                    if (te && te->mAir == n.t->mResult->mAir && te->mGaps.size() == gap_space.size())
                    {
                        const std::vector<U32>& ids = n.m->mEnclosure->mSpaceIds;
                        for (size_t g = 0; g < gap_space.size(); ++g)
                        {
                            const U16 sp = te->mGaps[g].mSpace;
                            gap_space[g] = sp && sp <= ids.size() ? ids[sp - 1] : 0;
                        }
                    }
                    in.mGapSpace = gap_space.data();
                    ok = gatherAirDebug(air, in, canvas);
                    if (ok && coarse)
                    {
                        const F32 z = F32(agent_floor - n.m->mOrigin.mdV[VZ]);
                        const LLVector3 a(n.t->mMin - n.m->mOrigin), b(n.t->mMax - n.m->mOrigin);
                        const LLVector3 q[4] = { LLVector3(a.mV[0], a.mV[1], z), LLVector3(b.mV[0], a.mV[1], z), LLVector3(b.mV[0], b.mV[1], z), LLVector3(a.mV[0], b.mV[1], z) };
                        for (S32 k = 0; ok && k < 4; ++k) ok = canvas.line(q[k], q[(k + 1) % 4], LLColor4(0.65f, 0.45f, 1.f, 0.6f));
                    }
                }
                ++drawn;
                if (!ok)
                {
                    cut = U32(tiles.size() - i);
                    break;
                }
            }
            canvas.write(dir + shot.mName + ".ppm", shot.mHalf, shot.mOblique);
            log << shot.mName << ": " << canvas.mUsed << " verts over " << drawn << " tiles (" << coarse_tiles << " coarse), " << cut
                << " cut by the budget of " << shot.mBudget << "\n";
        }
    }

    // SKNAV_REPLAY_RESIDENCY names a census, SKNAV_REPLAY_RESIDENCY_OUT a JSON
    // file: what a settled spill keeps and frees, what it writes, what a reload
    // costs, that the air comes back the same, and what whole-cluster air adds.
    template<> template<>
    void sknavreplay_object::test<13>()
    {
        set_test_name("spill every settled mesh's detail and reload it: the bytes, the costs and the same spaces, when a census is named");
        const std::string path = LLStringUtil::getenv("SKNAV_REPLAY_RESIDENCY");
        if (path.empty())
        {
            return;
        }
        typedef std::chrono::steady_clock Clock;
        const auto ms = [](Clock::time_point t0) { return std::chrono::duration<F64, std::milli>(Clock::now() - t0).count(); };
        CensusFile c;
        std::string error;
        ensure("read " + path + ": " + error, readCensusFile(path, c, &error));
        ReplayOptions options;
        ReplayResult r;
        replayCensus(c, options, r);

        // Spilled through a real disk cache, as the viewer does.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "sknav-residency";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        TileDiskCache cache(dir.string(), 8ull << 30);
        cache.index();

        struct Spilled
        {
            ReplayMesh* mMesh;
            TileKey mKey;
            U64 mKey64;
            TileAirPtr mAir;
            TileRasterPtr mRaster;
        };
        std::vector<Spilled> spilled;
        size_t detail_air = 0, rasters = 0, coarse_kept = 0, coarse_tiles_air = 0, products = 0, products_after = 0;
        size_t disk = 0;
        U32 detail_tiles = 0, coarse_tiles = 0, repack_differ = 0, reraster_differ = 0;
        F64 pack_ms = 0.0, store_ms = 0.0;
        for (const auto& mesh : r.mMeshes)
        {
            AirSwap away;
            for (auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildResult& res = *tile.mResult;
                if (!res.mAir)
                {
                    continue;
                }
                if (!res.mAirBuilt || res.mCoarseAir || res.mAir->mLeaves.empty())
                {
                    coarse_tiles_air += res.mAir->bytes();
                    ++coarse_tiles;
                    continue;
                }
                // The raster the viewer holds within the radius: built again, the replay having let it go after the stamp.
                TileBuildResult again;
                const TileRasterPtr raster = rasteriseTile(*tile.mInput, again);
                reraster_differ += again.mAir && packSpill(again.mAir.get(), nullptr, 0) == packSpill(res.mAir.get(), nullptr, 0) ? 0 : 1;
                const U64 key64 = spillKey(tileContentKey(*tile.mInput));
                Clock::time_point t0 = Clock::now();
                // The raster reads the air it was built with, so that is the air that goes with it.
                const std::vector<U8> bytes = packSpill(again.mAir.get(), raster.get(), key64);
                pack_ms += ms(t0);
                t0 = Clock::now();
                ensure("stored", cache.store(key64, bytes));
                store_ms += ms(t0);
                disk += bytes.size();
                detail_air += res.mAir->bytes();
                rasters += raster ? raster->bytes() : 0;
                coarse_kept += coarseAir(*res.mAir)->bytes();
                ++detail_tiles;
                spilled.push_back({ mesh.get(), key, key64, again.mAir, raster });
                away[key] = nullptr;
            }
            if (mesh->mEnclosure)
            {
                products += mesh->mEnclosure->bytes() + mesh->mRooms->bytes();
                products_after += withTileAir(*mesh->mEnclosure, away)->bytes() + withTileAir(*mesh->mRooms, away)->bytes();
            }
        }

        // Reload one tile at a time, then the largest mesh across every core as an edit would.
        F64 load_ms = 0.0, worst_ms = 0.0;
        std::map<std::pair<ReplayMesh*, TileKey>, std::pair<TileAirPtr, TileRasterPtr>> back;
        for (const Spilled& s : spilled)
        {
            const Clock::time_point t0 = Clock::now();
            std::vector<U8> bytes;
            TileAirPtr air;
            TileRasterPtr raster;
            ensure("reloaded", cache.load(s.mKey64, bytes) && unpackSpill(bytes.data(), bytes.size(), s.mKey64, air, raster));
            const F64 t = ms(t0);
            load_ms += t;
            worst_ms = std::max(worst_ms, t);
            repack_differ += packSpill(air.get(), raster.get(), s.mKey64) == packSpill(s.mAir.get(), s.mRaster.get(), s.mKey64) ? 0 : 1;
            back[{ s.mMesh, s.mKey }] = { air, raster };
        }
        std::map<ReplayMesh*, U32> per_mesh;
        for (const Spilled& s : spilled)
        {
            ++per_mesh[s.mMesh];
        }
        ReplayMesh* largest = nullptr;
        for (const auto& [m, n] : per_mesh)
        {
            largest = !largest || n > per_mesh[largest] ? m : largest;
        }
        F64 mesh_wall_ms = 0.0;
        const S32 threads = std::max(1, static_cast<S32>(std::thread::hardware_concurrency()) - 2);
        if (largest)
        {
            std::vector<const Spilled*> jobs;
            for (const Spilled& s : spilled)
            {
                if (s.mMesh == largest) jobs.push_back(&s);
            }
            std::atomic<size_t> next{ 0 };
            const Clock::time_point t0 = Clock::now();
            std::vector<std::thread> pool;
            for (S32 k = 0; k < threads; ++k)
            {
                pool.emplace_back([&]() {
                    for (size_t i = next++; i < jobs.size(); i = next++)
                    {
                        std::vector<U8> bytes;
                        TileAirPtr air;
                        TileRasterPtr raster;
                        cache.load(jobs[i]->mKey64, bytes);
                        unpackSpill(bytes.data(), bytes.size(), jobs[i]->mKey64, air, raster);
                    }
                });
            }
            for (std::thread& t : pool) t.join();
            mesh_wall_ms = ms(t0);
        }

        // The largest mesh's enclosure, spaces and stamps over the reloaded air against the air it had.
        bool same_enclosure = true, same_spaces = true;
        U32 stamp_tiles = 0, stamp_differ = 0;
        if (largest)
        {
            // The landed products the next pass reads, pointed at the reloaded air as the viewer points them.
            AirSwap swap;
            for (const auto& [k, v] : back)
            {
                if (k.first == largest) swap[k.second] = v.first;
            }
            const auto run = [&](bool reloaded) {
                auto input = std::make_shared<EnclosureInput>();
                input->mPrevious = reloaded ? withTileAir(*largest->mEnclosure, swap) : largest->mEnclosure;
                input->mGeneration = 1;
                input->mMeshOrigin = largest->mOrigin;
                input->mSky = r.mSky;
                input->mParams = c.mEnclosure;
                for (const auto& [key, tile] : largest->mTiles)
                {
                    const TileBuildResult& res = *tile.mResult;
                    if (res.mStatus == TileBuildResult::FAILED || !res.mAirBuilt || !res.mAir || res.mAir->mLeaves.empty()) continue;
                    const auto it = back.find({ largest, key });
                    input->mTiles.push_back({ key, reloaded && it != back.end() ? it->second.first : res.mAir, skyContributor(largest->mId, key) });
                }
                EnclosurePass pass(input);
                while (!pass.step(1u << 30)) {}
                std::shared_ptr<EnclosureProduct> product = pass.takeProduct();
                std::shared_ptr<RoomsInput> rooms_input = replayRoomsInput(c, *largest, 1);
                if (reloaded)
                {
                    rooms_input->mPrevious = withTileAir(*largest->mRooms, swap);
                }
                SpacesPass spaces(rooms_input, product);
                while (!spaces.step(1u << 30)) {}
                return std::make_pair(std::shared_ptr<const EnclosureProduct>(product), spaces.product());
            };
            const auto a = run(false);
            const auto b = run(true);
            same_enclosure = a.first->mSummary == b.first->mSummary && a.first->mGapCount == b.first->mGapCount && a.first->mSpaceIds == b.first->mSpaceIds;
            for (size_t t = 0; same_enclosure && t < a.first->mTiles.size(); ++t)
            {
                const auto& x = a.first->mTiles[t].mGaps;
                const auto& y = b.first->mTiles[t].mGaps;
                same_enclosure = x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](const GapEnclosure& p, const GapEnclosure& q) {
                                     return p.mSpace == q.mSpace && p.mS == q.mS && p.mE == q.mE && p.mD == q.mD && p.mBucket == q.mBucket && p.mOpen == q.mOpen;
                                 });
            }
            same_spaces = a.second->mSpaces.size() == b.second->mSpaces.size() && a.second->mPortals.size() == b.second->mPortals.size();
            for (size_t s = 0; same_spaces && s < a.second->mSpaces.size(); ++s)
            {
                same_spaces = a.second->mSpaces[s].mId == b.second->mSpaces[s].mId;
            }
            for (const Spilled& s : spilled)
            {
                if (s.mMesh != largest || !s.mRaster) continue;
                const ReplayTile& tile = largest->mTiles.at(s.mKey);
                const auto& [air, raster] = back[{ largest, s.mKey }];
                const LLVector3 lo(tile.mMin - largest->mOrigin), hi(tile.mMax - largest->mOrigin);
                const auto stamped = [&](const std::pair<std::shared_ptr<const EnclosureProduct>, RoomsProductPtr>& p, const TileAirPtr& a_air, const TileRaster& ras) {
                    TileStampInput in;
                    stampInput(*a_air, *p.first->tile(s.mKey), *p.second, lo, hi, in);
                    TileStamp st;
                    stampTile(*ras.mChf, borderCells(tile.mInput->mParams[0].mRadius, tile.mInput->mCell), ras.mSpanGap, in, ras.mReal, ras.mFlights, StampParams(), st);
                    TileBuildResult polys = *tile.mResult;
                    polygoniseTile(*tile.mInput, ras, polys, nullptr, &in, &st);
                    return polys.mData[0];
                };
                if (!a.first->tile(s.mKey)) continue;
                ++stamp_tiles;
                stamp_differ += stamped(a, s.mAir, *s.mRaster) == stamped(b, air, *raster) ? 0 : 1;
            }
        }

        // Whole-cluster air: the tiles under the agent's clusters past the radius, built with detail rather than coarse.
        std::vector<ClusterPtr> home;
        r.mClusters.at(c.mAgent, home);
        U32 cluster_tiles = 0;
        F64 cluster_detail_ms = 0.0, cluster_coarse_ms = 0.0;
        size_t cluster_air = 0;
        for (const auto& mesh : r.mMeshes)
        {
            for (const auto& [key, tile] : mesh->mTiles)
            {
                const TileBuildResult& res = *tile.mResult;
                if (res.mAirBuilt && !res.mCoarseAir) continue;     // detail already
                bool under = false;
                for (const ClusterPtr& h : home) under = under || overlapsXY(h->mMin, h->mMax, tile.mMin, tile.mMax);
                if (!under) continue;
                ++cluster_tiles;
                TileBuildInput detail = *tile.mInput;
                detail.mAir = true;
                detail.mCoarseAir = false;
                TileBuildResult d, k;
                Clock::time_point t0 = Clock::now();
                rasteriseTile(detail, d);
                cluster_detail_ms += ms(t0);
                t0 = Clock::now();
                rasteriseTile(*tile.mInput, k);
                cluster_coarse_ms += ms(t0);
                cluster_air += d.mAir ? d.mAir->bytes() : 0;
            }
        }

        std::string census_name = path;
        std::replace(census_name.begin(), census_name.end(), '\\', '/');
        const F64 MB = 1048576.0;
        const size_t before = detail_air + rasters + coarse_tiles_air;
        const size_t after = coarse_kept + coarse_tiles_air;
        std::ostringstream json;
        json << std::fixed << std::setprecision(3) << "{\"census\":\"" << census_name << "\",\"meshes\":" << r.mMeshes.size()
             << ",\"detail_tiles\":" << detail_tiles << ",\"coarse_tiles\":" << coarse_tiles << ",\"detail_air_mb\":" << detail_air / MB
             << ",\"held_raster_mb\":" << rasters / MB << ",\"coarse_tiles_air_mb\":" << coarse_tiles_air / MB << ",\"coarse_kept_mb\":" << coarse_kept / MB
             << ",\"resident_before_mb\":" << before / MB << ",\"resident_after_mb\":" << after / MB << ",\"products_mb\":" << products / MB
             << ",\"products_after_mb\":" << products_after / MB << ",\"disk_written_mb\":" << disk / MB << ",\"pack_ms_per_tile\":" << pack_ms / std::max(1u, detail_tiles)
             << ",\"store_ms_per_tile\":" << store_ms / std::max(1u, detail_tiles) << ",\"reload_ms_per_tile\":" << load_ms / std::max<size_t>(1, spilled.size())
             << ",\"reload_worst_ms\":" << worst_ms << ",\"largest_mesh_tiles\":" << (largest ? per_mesh[largest] : 0) << ",\"largest_mesh_reload_wall_ms\":" << mesh_wall_ms
             << ",\"threads\":" << threads << ",\"repack_differ\":" << repack_differ << ",\"reraster_differ\":" << reraster_differ
             << ",\"same_enclosure\":" << (same_enclosure ? "true" : "false") << ",\"same_spaces\":" << (same_spaces ? "true" : "false")
             << ",\"stamp_tiles\":" << stamp_tiles << ",\"stamp_differ\":" << stamp_differ << ",\"home_clusters\":" << home.size()
             << ",\"cluster_extra_tiles\":" << cluster_tiles << ",\"cluster_detail_ms\":" << cluster_detail_ms << ",\"cluster_coarse_ms\":" << cluster_coarse_ms
             << ",\"cluster_extra_air_mb\":" << cluster_air / MB << "}";
        std::cout << json.str() << std::endl;
        const std::string out = LLStringUtil::getenv("SKNAV_REPLAY_RESIDENCY_OUT");
        if (!out.empty())
        {
            std::ofstream(out) << json.str() << "\n";
        }
        std::filesystem::remove_all(dir, ec);
        ensure_equals("spills that repack differently", repack_differ, 0u);
        ensure("the same enclosure over reloaded air", same_enclosure);
        ensure("the same spaces over reloaded air", same_spaces);
        ensure_equals("stamps that differ over reloaded air", stamp_differ, 0u);
    }
}
