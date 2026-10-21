/**
 * @file skoverviewprobes_test.cpp
 * @brief The tactical cut's probes: what a landed answer does, without GL.
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

#include "../skoverviewprobes.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    using skoverview::ProbeSlot;
    using Outcome = skoverview::ProbeSlot::Outcome;

    TEST_CASE("skoverviewprobes: a stale-signature feet sample is dropped", "[skoverviewprobes]")
    {
        ProbeSlot feet;
        feet.issue(11, 100, 1);
        CHECK(feet.settle(false, false, 100, true) == Outcome::Waiting);
        // The buffers were replaced between the render and the collect: the id it read may name another linkset now.
        CHECK(feet.settle(true, false, 101, true) == Outcome::Dropped);
        CHECK_FALSE(feet.mValid);
        CHECK(feet.wants(11));
        // A membership probe names extents by root at issue, so the signature does not decide it.
        ProbeSlot marks;
        marks.issue(11, 100, 1);
        CHECK(marks.settle(true, false, 101, false) == Outcome::Apply);
        CHECK(marks.mValid);
        CHECK(marks.mAppliedKey == 11);
    }

    TEST_CASE("skoverviewprobes: a result for an older key is applied and the newest key reissued", "[skoverviewprobes]")
    {
        ProbeSlot slot;
        CHECK(slot.wants(1));
        slot.issue(1, 7, 1);
        // The person moved while it was in flight: nothing more is issued, and what stands is kept.
        CHECK_FALSE(slot.wants(2));
        CHECK(slot.settle(false, false, 7, true) == Outcome::Waiting);
        CHECK_FALSE(slot.mValid);
        CHECK(slot.settle(true, false, 7, true) == Outcome::Apply);
        // Newer than nothing, so it stands; then the newest key goes out.
        CHECK(slot.mValid);
        CHECK(slot.mAppliedKey == 1);
        CHECK(slot.wants(2));
        CHECK_FALSE(slot.wants(1));
        slot.issue(2, 7, 2);
        CHECK(slot.settle(false, true, 7, true) == Outcome::Reissue);
        // A lost answer leaves the old one standing and asks again.
        CHECK(slot.mAppliedKey == 1);
        CHECK(slot.wants(2));
        slot.invalidate();
        CHECK(slot.wants(1));
        CHECK(slot.settle(true, false, 7, true) == Outcome::Idle);
    }

    TEST_CASE("skoverviewprobes: the agent's probes are issued first", "[skoverviewprobes]")
    {
        const std::vector<size_t> order = skoverview::probeOrder({ false, false, true, false });
        REQUIRE(order.size() == 4);
        CHECK(order[0] == 2);
        CHECK(order[1] == 0);
        CHECK(order[2] == 1);
        CHECK(order[3] == 3);
        // One person a frame: the agent, met first, takes it, and the next person waits for the next frame.
        skoverview::ProbeBudget budget(1, 256);
        CHECK(budget.admit((U32)order[0], 10));
        CHECK(budget.admit((U32)order[0], 5)); // the agent's second probe of the frame
        CHECK_FALSE(budget.admit((U32)order[1], 1));
        CHECK(budget.people() == 1);
        CHECK(budget.queries() == 15);
    }

    TEST_CASE("skoverviewprobes: a frame admits its first issue past the query budget and no more", "[skoverviewprobes]")
    {
        skoverview::ProbeBudget budget(8, 16);
        CHECK(budget.admit(0, 40));
        CHECK_FALSE(budget.admit(1, 1));
        skoverview::ProbeBudget roomy(8, 16);
        CHECK(roomy.admit(0, 10));
        CHECK(roomy.admit(1, 6));
        CHECK_FALSE(roomy.admit(2, 1));
    }

    TEST_CASE("skoverviewprobes: a membership answer keeps the candidates that passed and are still their root's", "[skoverviewprobes]")
    {
        LLUUID a, b, c;
        a.generate();
        b.generate();
        c.generate();
        const std::vector<std::pair<U32, LLUUID>> candidates = { { 4, a }, { 5, b }, { 6, c } };
        const std::map<LLUUID, U32> roots = { { a, 4 }, { b, 9 } };
        // a passed and is still extent 4; b passed but its root is another extent now; c passed and is gone.
        const std::vector<U32> passed = skoverview::passedExtents(candidates, { 1, 3, 1 }, roots);
        REQUIRE(passed.size() == 1);
        CHECK(passed[0] == 4);
        CHECK(skoverview::passedExtents(candidates, { 0, 0, 0 }, roots).empty());
    }

    TEST_CASE("skoverviewprobes: a feet texel decodes to its linkset and its height in either depth convention", "[skoverviewprobes]")
    {
        U32 id = 0;
        F32 z = 0.f;
        CHECK_FALSE(skoverview::feetSurface(0, 0.5f, 1.4f, -1.6f, false, id, z));
        // Looking down from 1.4 m to -1.6 m, a quarter of the way: 0.65 m.
        REQUIRE(skoverview::feetSurface(8, 0.25f, 1.4f, -1.6f, false, id, z));
        CHECK(id == 7);
        CHECK(std::fabs(z - 0.65f) < 1.0e-5f);
        REQUIRE(skoverview::feetSurface(8, 0.75f, 1.4f, -1.6f, true, id, z));
        CHECK(std::fabs(z - 0.65f) < 1.0e-5f);
    }
}
