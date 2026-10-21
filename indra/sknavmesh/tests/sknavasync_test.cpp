/**
 * @file sknavasync_test.cpp
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
#include "../test/lltut.h"
#include "../sknavlayout.h"
#include "../sknavprobes.h"
#include "sknavtestscene.h"
#include <future>
#include <thread>

namespace tut
{
    struct sknavasync_data {};
    typedef test_group<sknavasync_data> sknavasync_group;
    typedef sknavasync_group::object sknavasync_object;
    sknavasync_group g("sknavasync");

    template<> template<>
    void sknavasync_object::test<1>()
    {
        set_test_name("worker sky replacement leaves the published map and its readers intact");
        sknav::TileAir air;         // one 32 m tile: a roof from 8 to 9 m over every metre column
        air.mCell = sknav_test::CELL;
        air.mUnit = sknav_test::UNIT;
        air.mTopCell = static_cast<S32>(1.f / sknav_test::CELL);
        air.mTopWidth = air.mTopHeight = 32;
        air.mTopStart.assign(static_cast<size_t>(air.mTopWidth) * air.mTopHeight + 1, 0);
        for (S32 c = 0; c < air.mTopWidth * air.mTopHeight; ++c)
        {
            air.mTop.push_back({ static_cast<U16>(8.f / sknav_test::UNIT), static_cast<U16>(9.f / sknav_test::UNIT) });
            air.mTopStart[c + 1] = static_cast<U32>(air.mTop.size());
        }
        sknav::SkyMap published;
        published.set(1, LLVector3d(), air);
        sknav::SkyMap replacement = published;
        const sknav::SkyView reader(published, 0., 0., 32., 32.);
        const LLVector3d point(16., 16., 2.);
        const F32 cover = reader.coverAbove(point, 10.f);
        ensure("the fixture has a roof within reach", !published.skyOpenAt(point, 10.f));
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
        const bool removed = replacement.skyOpenAt(point, 10.f);
        release.set_value();
        worker.join();
        ensure("the worker result removes the roof", removed);
        ensure("the old snapshot and dense view still have the roof", retained);
    }

    template<> template<>
    void sknavasync_object::test<2>()
    {
        set_test_name("layout and solid-index jobs retain records after the producer releases its snapshot");
        using namespace sknav;
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
        ensure("polling an unfinished job does not wait", pending);
        ensure_equals("the retained cluster produces its tile", built.first, size_t(1));
        ensure("the retained body is indexed", built.second);
    }
    template<> template<>
    void sknavasync_object::test<3>()
    {
        set_test_name("census clustering retains detached records and matches synchronous output");
        using namespace sknav;
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
        ensure("the caller can poll without waiting for clustering", pending);
        ensure_equals("cluster count", built.mClusters.size(), size_t(1));
        ensure("stable identity", built.mClusters.front()->mId == expected_id);
        ensure("same tile coverage and signatures", built.mClusters.front()->mTiles == expected_tiles);
        ensure_equals("same spatial index coverage", built.mByTile.size(), expected_index_size);
    }

}
