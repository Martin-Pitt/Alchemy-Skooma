/**
 * @file skcensuscluster_test.cpp
 * @brief Clustering of census records: linksets, gaps, skyboxes, movers, stable ids and tile keys.
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

#include "../skcensuscluster.h"
#include "../skcensuslattice.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

namespace
{
    using namespace skworld;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        return u;
    }

    RecordPtr box(U32 root, U32 part, F64 x0, F64 y0, F64 z0, F64 x1, F64 y1, F64 z1, bool dynamic = false)
    {
        auto r = std::make_shared<Record>();
        r->mRoot = id(root);
        r->mPart = id(part);
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        r->mDynamic = dynamic;
        r->mSignature = mix(mix(SEED, root), part);
        return r;
    }

    const Cluster* holding(const ClusterSet& s, U32 root)
    {
        for (const ClusterPtr& c : s.mClusters)
        {
            for (const RecordPtr& r : c->mRecords)
            {
                if (r->mRoot == id(root))
                {
                    return c.get();
                }
            }
        }
        return nullptr;
    }
}

TEST_CASE("skcensuscluster: a linkset is one cluster however far apart its parts are", "[skcensuscluster]")
{
    std::vector<RecordPtr> recs = {
        box(1, 1, 0, 0, 0, 1, 1, 1),
        box(1, 2, 50, 50, 0, 51, 51, 1),
    };
    const ClusterSet s = cluster(recs, 0.5);
    CHECK(s.mClusters.size() == 1u); // one cluster
    CHECK(s.mClusters[0]->mRecords.size() == 2u); // both parts
    CHECK(s.mClusters[0]->mId == id(1)); // id is the root
}

TEST_CASE("skcensuscluster: a 0.4 m gap joins under a 0.5 m inflation; 0.6 m separates", "[skcensuscluster]")
{
    std::vector<RecordPtr> close = { box(1, 1, 0, 0, 0, 1, 1, 1), box(2, 2, 1.4, 0, 0, 2.4, 1, 1) };
    std::vector<RecordPtr> apart = { box(1, 1, 0, 0, 0, 1, 1, 1), box(2, 2, 2.1, 0, 0, 3.1, 1, 1) };
    CHECK(cluster(close, 0.5).mClusters.size() == 1u); // 0.4 m joins
    CHECK(cluster(apart, 0.5).mClusters.size() == 2u); // 0.6 m separates
    CHECK(cluster(apart, 0.6).mClusters.size() == 1u); // 1.1 m joins at gap 0.6
}

TEST_CASE("skcensuscluster: a skybox is its own cluster; a mover never joins; ids do not depend on input order", "[skcensuscluster]")
{
    std::vector<RecordPtr> recs = {
        box(7, 7, 10, 10, 3000, 20, 20, 3010),
        box(3, 3, 10, 10, 0, 20, 20, 10),
        box(2, 2, 19.9, 10, 0, 30, 20, 10),
        box(9, 9, 15, 15, 0, 16, 16, 2, true),
    };
    ClusterSet s = cluster(recs, 0.5);
    CHECK(s.mClusters.size() == 2u); // skybox apart, mover out
    const Cluster* ground = holding(s, 3);
    CHECK(ground != nullptr); // ground cluster
    CHECK(ground->mRecords.size() == 2u); // ground has both
    CHECK(ground->mId == id(2)); // smallest root is the id
    CHECK(holding(s, 9) == nullptr); // mover absent

    std::reverse(recs.begin(), recs.end());
    const ClusterSet t = cluster(recs, 0.5);
    CHECK(holding(t, 3)->mId == id(2)); // id stable under order
}

TEST_CASE("skcensuscluster: tile keys lie on the global lattice and a bridging object keeps them", "[skcensuscluster]")
{
    std::vector<RecordPtr> recs = { box(1, 1, 30, 5, 0, 34, 6, 2), box(5, 5, 60, 5, 0, 64, 6, 2) };
    ClusterSet s = cluster(recs, 0.5);
    CHECK(s.mClusters.size() == 2u); // two clusters
    const Cluster* a = holding(s, 1);
    CHECK((a->mTiles.count({ 0, 0 }) == 1 && a->mTiles.count({ 1, 0 }) == 1)); // a spans tiles 0 and 1
    const U64 sigTile0 = a->mTiles.at({ 0, 0 });
    const U64 sigTile1 = a->mTiles.at({ 1, 0 });

    recs.push_back(box(3, 3, 34.2, 5, 0, 59.8, 6, 2));
    s = cluster(recs, 0.5);
    CHECK(s.mClusters.size() == 1u); // bridged into one
    const Cluster* m = holding(s, 1);
    CHECK(m->mId == id(1)); // merged id is the smallest root
    CHECK(m->mTiles.at({ 0, 0 }) == sigTile0); // tile 0 unchanged
    CHECK(m->mTiles.at({ 1, 0 }) != sigTile1); // tile 1 changed: the bridge is in it

    std::vector<ClusterPtr> hit;
    s.at(LLVector3d(62, 5.5, 1), hit);
    CHECK(hit.size() == 1u); // lookup by position
    hit.clear();
    s.at(LLVector3d(62, 40, 1), hit);
    CHECK(hit.empty()); // miss outside the box
}

TEST_CASE("skcensuscluster: a vehicle, left out of every class, is in no cluster: it neither joins the building it parks against nor stretches a cluster along its track", "[skcensuscluster]")
{
    std::vector<RecordPtr> records;
    records.push_back(box(1, 1, 0, 0, 0, 4, 4, 4));
    records.push_back(box(3, 3, 10, 0, 0, 14, 4, 4));
    auto vehicle = std::make_shared<Record>(*box(2, 2, 4.2, 0, 0, 9.8, 4, 4));
    vehicle->mExclude = EXCLUDE_ALL;
    records.push_back(vehicle);
    auto door = std::make_shared<Record>(*box(4, 4, 20, 0, 0, 20.1, 1, 2));
    door->mExclude = EXCLUDE_AVATAR;
    records.push_back(door);
    const ClusterSet s = cluster(records, 0.5, 8.0);
    CHECK(holding(s, 2) == nullptr); // the vehicle is in no cluster
    CHECK((holding(s, 1) && holding(s, 3) && holding(s, 1) != holding(s, 3))); // the buildings stay apart
    CHECK(holding(s, 4) != nullptr); // a door, left out of one class, is still clustered
}

TEST_CASE("skcensuscluster: a 65 km cloud plane over one region tiles only the region and its void, and joins only what that window sees", "[skcensuscluster]")
{
    const ClusterWindow window = regionWindow({ { LLVector3d(0, 0, 0), 256.0 } }, 256.f);
    const size_t window_tiles = static_cast<size_t>((768 / TILE_M) * (768 / TILE_M));
    std::vector<RecordPtr> records = {
        box(1, 1, -32768, -32768, 1955, 32768, 32768, 1956),
        box(2, 2, 100, 100, 1950, 110, 110, 1960),
        box(3, 3, 5000, 5000, 1950, 5010, 5010, 1960),
    };
    const ClusterSet s = cluster(records, 0.5, TILE_M, window);
    const Cluster* plane = holding(s, 1);
    CHECK(plane != nullptr); // the plane is clustered
    CHECK(plane->mTiles.size() <= window_tiles); // no more tiles than the window
    CHECK(holding(s, 2) == plane); // it joins what is inside the window
    CHECK(holding(s, 3) != plane); // but not what lies kilometres off
    CHECK(holding(s, 3)->mTiles.empty()); // the far record makes no tiles
    std::vector<LaidTile> laid;
    CHECK(layAirborne(*plane, latticeOrigin(plane->mMin), 2.f, laid)); // its mesh is laid
    CHECK((!laid.empty() && laid.size() <= window_tiles)); // within the window
}

TEST_CASE("skcensuscluster: a mesh over MAX_TILES_PER_MESH is refused, airborne or region, not cut short", "[skcensuscluster]")
{
    const ClusterSet s = cluster({ box(1, 1, 0, 0, 100, 2048, 2048, 101) }, 0.5);
    CHECK(s.mClusters[0]->mTiles.size() > static_cast<size_t>(MAX_TILES_PER_MESH)); // the unclipped plate wants more than a mesh holds
    std::vector<LaidTile> laid(1);
    CHECK(!layAirborne(*s.mClusters[0], LLVector3d(), 2.f, laid)); // airborne refused
    CHECK(laid.empty()); // with nothing laid

    // A 512 m void about a region holds (8 + 16 + 16)^2 tiles.
    const std::vector<RegionSquare> regions = { { LLVector3d(0, 0, 0), 256.0 } };
    const std::vector<RecordPtr> ground = { box(2, 2, -600, -600, 0, 900, 900, 1) };
    CHECK(!layRegion(LLVector3d(), 256.0, LLVector3d(), ground, 2.f, 512.f, regions, laid)); // region refused
    CHECK(laid.empty()); // with nothing laid
    CHECK((layRegion(LLVector3d(), 256.0, LLVector3d(), ground, 2.f, 256.f, regions, laid) && laid.size() == 576u)); // a 256 m void fits
}

TEST_CASE("skcensuscluster: clusters inside the window are exactly what they were without one", "[skcensuscluster]")
{
    std::vector<RecordPtr> records = {
        box(1, 1, 10, 10, 0, 40, 40, 20),
        box(1, 2, 200, 30, 0, 230, 60, 5),
        box(2, 2, 40.3, 10, 0, 70, 20, 4),
        box(3, 3, -200, -100, 30, -150, 300, 40),
        box(4, 4, 400, 400, 2000, 500, 500, 2001),
    };
    const ClusterSet a = cluster(records, 0.5);
    const ClusterSet b = cluster(records, 0.5, TILE_M, regionWindow({ { LLVector3d(0, 0, 0), 256.0 } }, 256.f));
    CHECK(b.mClusters.size() == a.mClusters.size()); // same count
    for (size_t i = 0; i < a.mClusters.size(); ++i)
    {
        const Cluster& x = *a.mClusters[i];
        const Cluster& y = *b.mClusters[i];
        CHECK(x.mId == y.mId); // same id
        CHECK(x.mRecords == y.mRecords); // same members
        CHECK((x.mMin == y.mMin && x.mMax == y.mMax)); // same bounds
        CHECK(x.mTiles == y.mTiles); // same tiles
    }
}

TEST_CASE("skcensuscluster: tile range lookup matches inclusive linear overlap with translated negative keys", "[skcensuscluster]")
{
    std::map<TileKey, int> tiles;
    for (S32 x = -8; x <= 8; ++x)
        for (S32 y = -8; y <= 8; ++y)
            if ((x + y) % 3) tiles[{x, y}] = 1;
    tiles[{1000000, 1000000}] = 1;
    const LLVector3d origin(256000., 512000., 0.);
    for (F64 a : { -96., -31.5, 0., 32., 95.5 })
    {
        for (F64 width : { 0., 0.5, 32., 100. })
        {
            const auto mn = origin + LLVector3d(a, -32., 0.);
            const auto mx = mn + LLVector3d(width, width, 0.);
            std::vector<TileKey> expected, actual;
            for (const auto& entry : tiles)
            {
                const auto lo = meshTileMin(origin, entry.first);
                if (overlapsXY(lo, lo + LLVector3d(TILE_M, TILE_M, 0.), mn, mx)) expected.push_back(entry.first);
            }
            visitTilesIn(tiles, origin, mn, mx, [&](const TileKey& key, const int&) { actual.push_back(key); });
            CHECK(actual == expected); // same tiles in the same order, including edges and corners
        }
    }
    size_t visited = 0;
    visitTilesIn(tiles, origin, origin + LLVector3d(2., 2., 0.), origin + LLVector3d(1., 1., 0.),
                 [&](const TileKey&, const int&) { ++visited; });
    CHECK(visited == size_t(0)); // inverted bounds are empty
    tiles[{std::numeric_limits<S32>::max(), 0}] = 1;
    visited = 0;
    const F64 edge = static_cast<F64>(std::numeric_limits<S32>::max()) * TILE_M;
    visitTilesIn(tiles, origin, origin + LLVector3d(edge + 1., 1., 0.),
                 origin + LLVector3d(edge + 2., 2., 0.),
                 [&](const TileKey&, const int&) { ++visited; });
    CHECK(visited == size_t(1)); // last representable column terminates without increment overflow
}
