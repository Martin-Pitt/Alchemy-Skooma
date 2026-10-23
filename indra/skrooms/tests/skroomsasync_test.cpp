/**
 * @file skroomsasync_test.cpp
 * @brief Immutable navigation snapshots shared with background preparation jobs.
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
#include "skairlayout.h"
#include "../skroomsprobes.h"
#include "skroomstestscene.h"
#include <future>
#include <thread>

namespace
{

    TEST_CASE("skroomsasync: worker sky replacement leaves the published map and its readers intact", "[skroomsasync]")
    {
        skworld::TileTops air;        // one 32 m tile: a roof from 8 to 9 m over every metre column
        air.mCell = skrooms_test::CELL;
        air.mUnit = skrooms_test::UNIT;
        air.mTopCell = static_cast<S32>(1.f / skrooms_test::CELL);
        air.mTopWidth = air.mTopHeight = 32;
        air.mTopStart.assign(static_cast<size_t>(air.mTopWidth) * air.mTopHeight + 1, 0);
        for (S32 c = 0; c < air.mTopWidth * air.mTopHeight; ++c)
        {
            air.mTop.push_back({ static_cast<U16>(8.f / skrooms_test::UNIT), static_cast<U16>(9.f / skrooms_test::UNIT) });
            air.mTopStart[c + 1] = static_cast<U32>(air.mTop.size());
        }
        skworld::SkyMap published;
        published.set(1, LLVector3d(), air);
        skworld::SkyMap replacement = published;
        const skworld::SkyView reader(published, 0., 0., 32., 32.);
        const LLVector3d point(16., 16., 2.);
        const F32 cover = reader.coverAbove(point, 10.f);
        { INFO("the fixture has a roof within reach"); REQUIRE((!published.skyOpenAt(point, 10.f))); }
        std::promise<void> changed, release;
        auto ready = changed.get_future();
        auto gate = release.get_future();
        std::thread worker([&]()
        {
            replacement.remove(1);
            changed.set_value();
            gate.wait();
        });
        ready.wait();
        const bool retained = !published.skyOpenAt(point, 10.f) &&
            std::fabs(reader.coverAbove(point, 10.f) - cover) < 1e-5f;
        // Air nobody published reads closed, so the roof's going shows as its tile's going and no top over the point.
        F64 top = 0.0;
        const bool removed = !replacement.has(1) && !replacement.topAt(point.mdV[VX], point.mdV[VY], top);
        release.set_value();
        worker.join();
        { INFO("the worker result removes the roof"); REQUIRE((removed)); }
        { INFO("the old snapshot and dense view still have the roof"); REQUIRE((retained)); }
    }

    TEST_CASE("skroomsasync: layout and solid-index jobs retain records after the producer releases its snapshot", "[skroomsasync]")
    {
        using namespace skworld;
        auto record = std::make_shared<Record>();
        record->mRoot.generate();
        record->mPart = record->mRoot;
        record->mShape = Shape::BOX;
        record->mSolid = true;
        record->mCenter.set(16.f, 16.f, 20.f);
        record->mHalf.set(2.f, 2.f, 2.f);
        record->mAxes[0] = LLVector3::x_axis;
        record->mAxes[1] = LLVector3::y_axis;
        record->mAxes[2] = LLVector3::z_axis;
        record->mMin.set(14., 14., 18.);
        record->mMax.set(18., 18., 22.);
        auto snapshot = std::make_shared<const ClusterSet>(cluster({record}, 0.5f));
        std::promise<void> release;
        auto gate = release.get_future();
        std::packaged_task<std::pair<size_t, bool>()> task([snapshot, gate = std::move(gate)]() mutable
        {
            gate.wait();
            std::vector<LaidTile> laid;
            layAirborne(*snapshot->mClusters.front(), LLVector3d(), 1.f, laid);
            SolidLattice solids;
            solids.rebuild(snapshot->mClusters.front()->mRecords);
            return std::make_pair(laid.size(), solids.contains(LLVector3d(16., 16., 20.)));
        });
        auto result = task.get_future();
        std::thread worker(std::move(task));
        snapshot.reset();
        record.reset();
        const bool pending = result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
        release.set_value();
        worker.join();
        const auto built = result.get();
        { INFO("polling an unfinished job does not wait"); REQUIRE((pending)); }
        { INFO("the retained cluster produces its tile"); REQUIRE((built.first) == (size_t(1))); }
        { INFO("the retained body is indexed"); REQUIRE((built.second)); }
    }
    TEST_CASE("skroomsasync: census clustering retains detached records and matches synchronous output", "[skroomsasync]")
    {
        using namespace skworld;
        auto record = std::make_shared<Record>();
        record->mRoot.generate();
        record->mPart = record->mRoot;
        record->mMin.set(30., -2., 0.);
        record->mMax.set(66., 34., 5.);
        record->mSignature = 1234;
        std::vector<RecordPtr> records { record };
        auto expected = cluster(records, 0.5);
        const auto expected_id = expected.mClusters.front()->mId;
        const auto expected_tiles = expected.mClusters.front()->mTiles;
        const auto expected_index_size = expected.mByTile.size();
        expected = {};
        std::promise<void> release;
        auto gate = release.get_future();
        std::packaged_task<ClusterSet()> task([records, gate = std::move(gate)]() mutable
        {
            gate.wait();
            return cluster(records, 0.5);
        });
        auto result = task.get_future();
        std::thread worker(std::move(task));
        records.clear();
        record.reset();
        const bool pending = result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
        release.set_value();
        worker.join();
        const auto built = result.get();
        { INFO("the caller can poll without waiting for clustering"); REQUIRE((pending)); }
        { INFO("cluster count"); REQUIRE((built.mClusters.size()) == (size_t(1))); }
        { INFO("stable identity"); REQUIRE((built.mClusters.front()->mId == expected_id)); }
        { INFO("same tile coverage and signatures"); REQUIRE((built.mClusters.front()->mTiles == expected_tiles)); }
        { INFO("same spatial index coverage"); REQUIRE((built.mByTile.size()) == (expected_index_size)); }
    }

}
