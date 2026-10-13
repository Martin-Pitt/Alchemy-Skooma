/**
 * @file sknavroomsschedule_test.cpp
 * @brief The rooms scheduler publishes under air that never settles.
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

#include "../sknavroomsschedule.h"

#include <algorithm>
#include <vector>

namespace
{
    using namespace sknav;

    /// SKNavRooms::update's loop over one thread, a frame at a time: a pass
    /// lands the whole mesh on the air it was posted with.
    struct Sim
    {
        struct Mesh
        {
            RoomsSchedule mS;
            F64 mFirstLanded = -1.0;
            F64 mLastLanded = -1.0;
            U32 mLanded = 0;
            F64 mPassS = 0.3;
        };
        std::vector<Mesh> mMeshes;
        S32 mRunning = -1;
        F64 mEnds = 0.0;
        F64 mSettleS = 0.5;

        void change(S32 m, F64 now)
        {
            RoomsSchedule& s = mMeshes[m].mS;
            if (!s.mDirty)
            {
                s.mDirtySince = now;
            }
            s.mDirty = true;
            s.mChanged = now;
        }

        void frame(F64 now)
        {
            if (mRunning >= 0 && now >= mEnds)
            {
                Mesh& m = mMeshes[mRunning];
                m.mS.mInFlight = false;
                m.mS.mPublished = true;
                m.mFirstLanded = m.mFirstLanded < 0.0 ? now : m.mFirstLanded;
                m.mLastLanded = now;
                ++m.mLanded;
                mRunning = -1;
            }
            std::vector<S32> ready;
            for (S32 i = 0; i < static_cast<S32>(mMeshes.size()); ++i)
            {
                if (roomsReady(mMeshes[i].mS, now, mSettleS))
                {
                    ready.push_back(i);
                }
            }
            std::stable_sort(ready.begin(), ready.end(), [this](S32 a, S32 b) { return roomsBefore(mMeshes[a].mS, mMeshes[b].mS); });
            if (!ready.empty() && mRunning >= 0 && roomsPreemptible(mMeshes[mRunning].mS, mMeshes[ready[0]].mS.mPriority))
            {
                Mesh& m = mMeshes[mRunning];
                m.mS.mInFlight = false;
                change(mRunning, m.mS.mChanged);
                mRunning = -1;
            }
            if (!ready.empty() && mRunning < 0)
            {
                Mesh& m = mMeshes[ready[0]];
                m.mS.mDirty = false;
                m.mS.mInFlight = true;
                m.mS.mLastPosted = now;
                mRunning = ready[0];
                mEnds = now + m.mPassS;
            }
        }
    };
}

namespace tut
{
    struct sknavroomsschedule_data
    {
    };
    typedef test_group<sknavroomsschedule_data> sknavroomsschedule_group;
    typedef sknavroomsschedule_group::object sknavroomsschedule_object;
    tut::sknavroomsschedule_group sknavroomsschedule_tests("sknavroomsschedule");

    template<> template<>
    void sknavroomsschedule_object::test<1>()
    {
        set_test_name("air changing every 100 ms in the agent's region: it and its neighbour first publish within a pass each, and refresh boundedly");
        Sim sim;
        sim.mMeshes.resize(2);
        sim.mMeshes[0].mS.mPriority = 0;    // the agent's region, churning
        sim.mMeshes[1].mS.mPriority = 1;
        constexpr F64 STEP = 0.01;
        sim.change(0, 0.0);
        sim.change(1, 0.0);
        F64 a_change = 0.0, b_change = -1.0, b_refreshed = -1.0;
        for (F64 now = 0.0; now < 60.0; now += STEP)
        {
            if (now - a_change >= 0.1)
            {
                sim.change(0, now);
                a_change = now;
            }
            if (b_change < 0.0 && now >= 20.0)
            {
                sim.change(1, now);
                b_change = now;
            }
            sim.frame(now);
            if (b_change >= 0.0 && b_refreshed < 0.0 && sim.mMeshes[1].mLastLanded > b_change)
            {
                b_refreshed = sim.mMeshes[1].mLastLanded;
            }
        }
        const F64 pass = 0.3 + 2 * STEP;
        ensure("the churning region publishes within one pass", sim.mMeshes[0].mFirstLanded >= 0.0 && sim.mMeshes[0].mFirstLanded <= pass);
        ensure("its neighbour within the next", sim.mMeshes[1].mFirstLanded >= 0.0 && sim.mMeshes[1].mFirstLanded <= 2 * pass);
        ensure("the churning region keeps refreshing", sim.mMeshes[0].mLanded >= 60.0 / (ROOMS_MAX_SETTLES * sim.mSettleS + pass) - 1.0);
        ensure("and the neighbour's later change still lands", b_refreshed > 0.0 && b_refreshed - b_change <= sim.mSettleS + 3 * pass);
    }

    template<> template<>
    void sknavroomsschedule_object::test<2>()
    {
        set_test_name("a first publication is never preempted, and goes before any refresh");
        RoomsSchedule fresh, old;
        fresh.mInFlight = old.mInFlight = true;
        fresh.mPriority = old.mPriority = 2;
        old.mPublished = true;
        ensure("a first pass runs on", !roomsPreemptible(fresh, 0));
        ensure("a refresh gives way", roomsPreemptible(old, 0));
        RoomsSchedule a, b;
        a.mPriority = 3;
        b.mPriority = 0;
        b.mPublished = true;
        ensure("unpublished first, whatever the tier", roomsBefore(a, b));
        RoomsSchedule churn;
        churn.mDirty = true;
        churn.mChanged = 9.9;
        churn.mDirtySince = 0.0;
        ensure("never published: at once", roomsReady(churn, 10.0, 0.5));
        churn.mPublished = true;
        ensure("published and churning: after the bounded wait", roomsReady(churn, 10.0, 0.5) && !roomsReady(churn, 1.0, 0.5));
    }
}
