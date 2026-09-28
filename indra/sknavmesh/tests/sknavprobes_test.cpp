/**
 * @file sknavprobes_test.cpp
 * @brief Tests for rooms probe parts, placement, placed-probe clearance, budget and churn.
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

#include "../sknavprobes.h"

#include <cmath>
#include <map>

namespace
{
    using namespace sknav;

    U64 key(S32 x, S32 y, S32 z)
    {
        const S64 bias = 1ll << 20;
        return (static_cast<U64>(x + bias) << 42) | (static_cast<U64>(y + bias) << 21) | static_cast<U64>(z + bias);
    }

    /// An indoor space over boxes of the 0.5 m raster, in metres, floor at 0.
    struct Room
    {
        std::map<U64, U32> mCells;

        Room& box(F32 x0, F32 y0, F32 x1, F32 y1)
        {
            for (S32 y = static_cast<S32>(y0 * 2.f); y < static_cast<S32>(y1 * 2.f); ++y)
            {
                for (S32 x = static_cast<S32>(x0 * 2.f); x < static_cast<S32>(x1 * 2.f); ++x)
                {
                    mCells[key(x, y, 0)] = 1;
                }
            }
            return *this;
        }

        Space space(F32 ceiling = 3.f) const
        {
            Space s;
            s.mBucket = EnclosureBucket::INDOORS;
            s.mCeilingMean = ceiling;
            s.mRaster.assign(mCells.begin(), mCells.end());
            return s;
        }
    };
}

namespace tut
{
    struct sknavprobes_data
    {
    };
    typedef test_group<sknavprobes_data> sknavprobes_group;
    typedef sknavprobes_group::object sknavprobes_object;
    tut::sknavprobes_group sknavprobes_tests("sknavprobes");

    template<> template<>
    void sknavprobes_object::test<1>()
    {
        set_test_name("an L-shaped room gives two parts");
        const ProbeParams params;
        const std::vector<ProbePart> parts = spaceProbes(Room().box(0.f, 0.f, 12.f, 4.f).box(0.f, 4.f, 4.f, 10.f).space(), params);
        ensure_equals("two parts", parts.size(), 2u);
        ensure("each has a sphere", parts[0].mRadius > params.mMinRadiusM && parts[1].mRadius > params.mMinRadiusM);
        F32 area = 0.f;
        for (const ProbePart& p : parts)
        {
            area += p.mArea;
            ensure("half way up", std::fabs(p.mOrigin.mV[VZ] - 1.5f) < 1e-3f);
            ensure("inside its part", p.mOrigin.mV[VX] > p.mMin.mV[VX] && p.mOrigin.mV[VX] < p.mMax.mV[VX] &&
                                          p.mOrigin.mV[VY] > p.mMin.mV[VY] && p.mOrigin.mV[VY] < p.mMax.mV[VY]);
        }
        ensure("the parts cover the floor", std::fabs(area - 72.f) < 1e-3f);
    }

    template<> template<>
    void sknavprobes_object::test<2>()
    {
        set_test_name("a plain room is one part, a nick in its wall folds in, and an open one gets none");
        const ProbeParams params;
        const std::vector<ProbePart> one = spaceProbes(Room().box(0.f, 0.f, 8.f, 6.f).box(3.f, 6.f, 4.f, 7.f).space(), params);
        ensure_equals("one part", one.size(), 1u);
        ensure("at the middle", std::fabs(one[0].mOrigin.mV[VX] - 4.f) <= 0.5f && std::fabs(one[0].mOrigin.mV[VY] - 3.f) <= 0.5f);
        ensure("reaching its corners", one[0].mRadius >= 5.f);

        Space open = Room().box(0.f, 0.f, 8.f, 6.f).space();
        open.mOpenShare = 0.9f;
        ensure("no ceiling, no probe", spaceProbes(open, params).empty());
        Space outdoors = Room().box(0.f, 0.f, 8.f, 6.f).space();
        outdoors.mBucket = EnclosureBucket::OUTDOORS;
        ensure("outdoors, no probe", spaceProbes(outdoors, params).empty());
    }

    template<> template<>
    void sknavprobes_object::test<3>()
    {
        set_test_name("a placed probe's influence is never intersected");
        ProbeParams params;
        std::vector<ProbeInfluence> placed(2);
        placed[0].mCentre.set(10.f, 0.f, 0.f);
        placed[0].mRadius = 3.f;
        placed[1].mBox = true;
        placed[1].mCentre.set(0.f, 9.f, 1.f);
        const F32 c = std::cos(0.6f), s = std::sin(0.6f);
        placed[1].mAxes[0].set(c, s, 0.f);
        placed[1].mAxes[1].set(-s, c, 0.f);
        placed[1].mAxes[2].set(0.f, 0.f, 1.f);
        placed[1].mHalf.set(2.f, 1.f, 1.5f);

        ensure("shrinks to the sphere", std::fabs(clearRadius(LLVector3(0.f, 0.f, 0.f), 12.f, placed, params) - 7.f) < 1e-3f);
        ensure("dropped inside one", clearRadius(LLVector3(10.f, 1.f, 0.f), 12.f, placed, params) == 0.f);
        ensure("dropped under 2 m", clearRadius(LLVector3(8.f, 4.f, 0.f), 12.f, placed, params) == 0.f);

        for (S32 i = 0; i < 400; ++i)
        {
            const LLVector3 o(-8.f + (i % 20) * 1.1f, -6.f + (i / 20) * 1.05f, 0.5f);
            const F32 r = clearRadius(o, 12.f, placed, params);
            if (r == 0.f)
            {
                continue;
            }
            for (S32 k = 0; k < 64; ++k)
            {
                const F32 a = k * 0.39f, b = (k % 8) * 0.4f - 1.4f;
                const LLVector3 p = o + LLVector3(std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), std::sin(b)) * (r * 0.999f);
                for (const ProbeInfluence& inf : placed)
                {
                    ensure("outside every placed influence", distanceTo(inf, p) > 0.f);
                }
            }
        }
    }

    template<> template<>
    void sknavprobes_object::test<4>()
    {
        set_test_name("the cap is respected, the far and small going first");
        std::vector<ProbeSlot> slots;
        for (U32 i = 0; i < 300; ++i)
        {
            ProbeSlot s;
            s.mKey = i;
            s.mDistance = static_cast<F32>((i * 37) % 300);
            s.mArea = static_cast<F32>(i % 7);
            slots.push_back(s);
        }
        keepBudget(slots, 255);
        ensure_equals("capped", slots.size(), 255u);
        for (const ProbeSlot& s : slots)
        {
            ensure("the nearest kept", s.mDistance < 255.f);
        }
        std::vector<ProbeSlot> tie(2);
        tie[0].mArea = 4.f;
        tie[1].mArea = 9.f;
        tie[1].mKey = 1;
        keepBudget(tie, 1);
        ensure_equals("the larger of two at one distance kept", tie[0].mKey, 1ull);
    }

    template<> template<>
    void sknavprobes_object::test<5>()
    {
        set_test_name("an unchanged space gives no re-placement");
        const ProbeParams params;
        const Space a = Room().box(0.f, 0.f, 12.f, 4.f).box(0.f, 4.f, 4.f, 10.f).space();
        const std::vector<ProbePart> first = spaceProbes(a, params), again = spaceProbes(a, params);
        ensure_equals("same parts", first.size(), again.size());
        for (size_t i = 0; i < first.size(); ++i)
        {
            ensure("held", !shouldReplace(first[i].mOrigin, first[i].mRadius, again[i].mOrigin, again[i].mRadius, params));
        }
        const std::vector<ProbePart> moved = spaceProbes(Room().box(3.f, 0.f, 15.f, 4.f).box(3.f, 4.f, 7.f, 10.f).space(), params);
        ensure("a room moved 3 m re-places", shouldReplace(first[0].mOrigin, first[0].mRadius, moved[0].mOrigin, moved[0].mRadius, params));
        ensure("a radius off by a quarter re-places", shouldReplace(first[0].mOrigin, 8.f, first[0].mOrigin, 10.f, params));
        ensure("a radius off by a tenth holds", !shouldReplace(first[0].mOrigin, 8.f, first[0].mOrigin, 8.8f, params));
    }

    template<> template<>
    void sknavprobes_object::test<6>()
    {
        set_test_name("a hall wider than one sphere reaches is tiled");
        const ProbeParams params;
        const std::vector<ProbePart> parts = spaceProbes(Room().box(0.f, 0.f, 40.f, 10.f).space(6.f), params);
        ensure("several parts", parts.size() >= 3);
        for (const ProbePart& p : parts)
        {
            ensure("capped", p.mRadius <= params.mMaxRadiusM + 1e-3f);
        }
    }
}
