/**
 * @file skroomsprobesasync_test.cpp
 * @brief The solid index keeps the records it was built from after their scan is released.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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
#include "../skroomsprobes.h"
#include "skcensusrecord.h"
#include <future>
#include <thread>

namespace
{
    TEST_CASE("skroomsprobesasync: a solid-index job retains records after the producer releases them", "[skroomsprobesasync]")
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
        auto records = std::make_shared<const std::vector<RecordPtr>>(std::vector<RecordPtr>{ record });
        std::promise<void> release;
        auto gate = release.get_future();
        std::packaged_task<bool()> task([records, gate = std::move(gate)]() mutable
        {
            gate.wait();
            SolidLattice solids;
            solids.rebuild(*records);
            return solids.contains(LLVector3d(16., 16., 20.));
        });
        auto result = task.get_future();
        std::thread worker(std::move(task));
        records.reset();
        record.reset();
        release.set_value();
        worker.join();
        { INFO("the retained body is indexed"); REQUIRE((result.get())); }
    }
}
