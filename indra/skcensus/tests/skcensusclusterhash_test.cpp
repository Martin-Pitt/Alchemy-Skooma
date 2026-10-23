/**
 * @file skcensusclusterhash_test.cpp
 * @brief The census content hash per cluster and per scan, and the delta between two scans.
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

#include "../skcensusdelta.h"
#include "../skcensuscluster.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
    using namespace skworld;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        u.mData[15] = 1;
        return u;
    }

    RecordPtr box(U32 root, U32 part, F64 x0, F64 y0, F64 x1, F64 y1, bool dynamic = false, U64 extra = 0)
    {
        auto r = std::make_shared<Record>();
        r->mRoot = id(root);
        r->mPart = id(part);
        r->mMin.set(x0, y0, 20.0);
        r->mMax.set(x1, y1, 24.0);
        r->mDynamic = dynamic;
        r->mSignature = mix(mix(mix(SEED, root), part), extra);
        return r;
    }

    // Two buildings 40 m apart, each a linkset of three parts.
    std::vector<RecordPtr> twoBuildings()
    {
        return { box(1, 11, 0, 0, 4, 4), box(1, 12, 4, 0, 8, 4), box(1, 13, 8, 0, 12, 4),
                 box(2, 21, 50, 0, 54, 4), box(2, 22, 54, 0, 58, 4), box(2, 23, 58, 0, 62, 4) };
    }

    // Each cluster's hash, by its id.
    std::map<LLUUID, U64> hashes(const std::vector<RecordPtr>& records)
    {
        std::map<LLUUID, U64> out;
        for (const ClusterPtr& c : cluster(records, 0.5).mClusters)
        {
            out[c->mId] = c->mHash;
        }
        return out;
    }

    U64 scanHash(const std::vector<RecordPtr>& records)
    {
        return censusHash(records, {}, {}, ClusterWindow(), 0.5);
    }

    TEST_CASE("skcensusclusterhash: the same records in another order hash the same", "[skcensusclusterhash]")
    {
        std::vector<RecordPtr> records = twoBuildings();
        records.push_back(box(3, 31, 20, 20, 21, 21, true));
        const std::map<LLUUID, U64> before = hashes(records);
        const U64 scan = scanHash(records);
        REQUIRE(before.size() == 2);

        std::reverse(records.begin(), records.end());
        std::rotate(records.begin(), records.begin() + 2, records.end());
        CHECK(hashes(records) == before);
        CHECK(scanHash(records) == scan);

        // A record listed twice is not the record once, nor nothing.
        std::vector<RecordPtr> doubled = records;
        doubled.push_back(records.front());
        CHECK(scanHash(doubled) != scan);
    }

    TEST_CASE("skcensusclusterhash: a visible-only record joins no cluster and still moves the census hash", "[skcensusclusterhash]")
    {
        std::vector<RecordPtr> records = twoBuildings();
        const std::map<LLUUID, U64> before = hashes(records);
        const U64 scan = scanHash(records);
        // A phantom part between the buildings, touching both: it would bridge them into one cluster if it collided.
        auto shell = std::make_shared<Record>(*box(4, 41, 11, 0, 51, 4));
        shell->mCollides = false;
        records.push_back(shell);
        const ClusterSet set = cluster(records, 0.5);
        CHECK(hashes(records) == before);
        for (const ClusterPtr& c : set.mClusters)
        {
            CHECK(std::find(c->mRecords.begin(), c->mRecords.end(), records.back()) == c->mRecords.end());
        }
        // Its arrival is news to the overview's cover test, which keys on the census hash.
        CHECK(scanHash(records) != scan);
    }

    TEST_CASE("skcensusclusterhash: a dynamic record moving leaves the cluster hash alone", "[skcensusclusterhash]")
    {
        std::vector<RecordPtr> records = twoBuildings();
        records.push_back(box(3, 31, 20, 20, 21, 21, true, 1));
        const std::map<LLUUID, U64> before = hashes(records);
        const U64 scan = scanHash(records);

        // It drives right through the first building: a dynamic record joins no cluster.
        records.back() = box(3, 31, 5, 1, 6, 2, true, 2);
        CHECK(hashes(records) == before);
        // The scan's hash covers it, so whoever polls the stamp for movers hears of it.
        CHECK(scanHash(records) != scan);
    }

    TEST_CASE("skcensusclusterhash: one changed signature moves exactly its cluster", "[skcensusclusterhash]")
    {
        std::vector<RecordPtr> records = twoBuildings();
        const std::map<LLUUID, U64> before = hashes(records);
        const U64 scan = scanHash(records);

        records[4] = box(2, 22, 54, 0, 58, 4, false, 7);
        const std::map<LLUUID, U64> after = hashes(records);
        REQUIRE(after.size() == 2);
        CHECK(after.at(id(1)) == before.at(id(1)));
        CHECK(after.at(id(2)) != before.at(id(2)));
        CHECK(scanHash(records) != scan);

        // Put back, it hashes as it did.
        records[4] = box(2, 22, 54, 0, 58, 4);
        CHECK(hashes(records) == before);
        CHECK(scanHash(records) == scan);
    }

    TEST_CASE("skcensusclusterhash: the delta names added removed and changed parts", "[skcensusclusterhash]")
    {
        const std::vector<RecordPtr> before = twoBuildings();
        std::vector<RecordPtr> after = before;
        after.erase(after.begin() + 1);                             // part 12 gone
        after.push_back(box(4, 41, 30, 30, 31, 31));                // part 41 come
        after[3] = box(2, 22, 54, 0, 58, 4, false, 9);             // part 22 re-signed
        after[4] = box(2, 23, 58, 0, 62, 4, true);                  // part 23 turned dynamic
        // A part re-recorded with the same content is no change.
        after[0] = box(1, 11, 0, 0, 4, 4);

        std::vector<RecordPtr> added, removed, changed;
        diffRecords(recordsByPart(before), recordsByPart(after), added, removed, changed);
        REQUIRE(added.size() == 1);
        CHECK(added[0]->mPart == id(41));
        REQUIRE(removed.size() == 1);
        CHECK(removed[0]->mPart == id(12));
        REQUIRE(changed.size() == 2);
        CHECK(changed[0]->mPart < changed[1]->mPart);
        CHECK(std::set<LLUUID>{ changed[0]->mPart, changed[1]->mPart } == std::set<LLUUID>{ id(22), id(23) });
        // A changed part is given as it is now.
        for (const RecordPtr& rec : changed)
        {
            CHECK(rec->mDynamic == (rec->mPart == id(23)));
        }

        // Against nothing, everything came; against itself, nothing changed.
        diffRecords(RecordsByPart(), recordsByPart(after), added, removed, changed);
        CHECK(added.size() == after.size());
        CHECK(removed.empty());
        CHECK(changed.empty());
        diffRecords(recordsByPart(after), recordsByPart(after), added, removed, changed);
        CHECK(added.empty());
        CHECK(removed.empty());
        CHECK(changed.empty());
    }

    TEST_CASE("skcensusclusterhash: features doors the window and the gap move the scan hash", "[skcensusclusterhash]")
    {
        const std::vector<RecordPtr> records = twoBuildings();

        Feature vehicle;
        vehicle.mKind = FeatureKind::VEHICLE;
        vehicle.mRoots = { id(5), id(6) };
        vehicle.mMin.set(20.0, 20.0, 20.0);
        vehicle.mMax.set(24.0, 22.0, 22.0);
        Feature shaft;
        shaft.mKind = FeatureKind::ELEVATOR;
        shaft.mRoots = { id(7) };
        shaft.mMin.set(30.0, 0.0, 20.0);
        shaft.mMax.set(32.0, 2.0, 40.0);
        const std::vector<Feature> features{ vehicle, shaft };

        DoorEntity door;
        door.mId = id(12);
        door.mLeaves = { id(12) };
        door.mMin.set(4.0, 0.0, 20.0);
        door.mMax.set(4.1, 1.0, 22.0);
        door.mSill = 20.0;
        const std::vector<DoorEntity> doors{ door };

        ClusterWindow window;
        window.mMin.set(0.0, 0.0, 0.0);
        window.mMax.set(256.0, 256.0, 0.0);

        const U64 scan = censusHash(records, features, doors, window, 0.5);
        CHECK(censusHash(records, features, doors, window, 0.5) == scan);

        // The same roots in another order are the same feature.
        std::vector<Feature> f = features;
        std::reverse(f[0].mRoots.begin(), f[0].mRoots.end());
        CHECK(censusHash(records, f, doors, window, 0.5) == scan);

        // A vehicle drives: only its feature's box says so.
        f = features;
        f[0].mMin.mdV[VX] += 0.25;
        f[0].mMax.mdV[VX] += 0.25;
        CHECK(censusHash(records, f, doors, window, 0.5) != scan);

        // A car arrives in the shaft.
        f = features;
        f[1].mWorking = true;
        CHECK(censusHash(records, f, doors, window, 0.5) != scan);

        // A door with no rest pose swings.
        std::vector<DoorEntity> d = doors;
        d[0].mMax.mdV[VX] += 0.5;
        CHECK(censusHash(records, features, d, window, 0.5) != scan);

        // Another region loads, so the clusters are cut to another window.
        ClusterWindow wider = window;
        wider.mMax.mdV[VX] = 512.0;
        CHECK(censusHash(records, features, doors, wider, 0.5) != scan);

        // Another gap links the records differently.
        CHECK(censusHash(records, features, doors, window, 1.0) != scan);
    }
}
