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
#include <iostream>
#include <limits>
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
        set_test_name("a plain room is one part and outdoor space also gets full probes");
        const ProbeParams params;
        const std::vector<ProbePart> one = spaceProbes(Room().box(0.f, 0.f, 8.f, 6.f).box(3.f, 6.f, 4.f, 7.f).space(), params);
        ensure_equals("one part", one.size(), 1u);
        ensure("at the middle", std::fabs(one[0].mOrigin.mV[VX] - 4.f) <= 0.5f && std::fabs(one[0].mOrigin.mV[VY] - 3.f) <= 0.5f);
        ensure("reaching its corners", one[0].mRadius >= 5.f);

        Space open = Room().box(0.f, 0.f, 8.f, 6.f).space();
        open.mOpenShare = 0.9f;
        ensure("open space gets a probe above its floor", !spaceProbes(open, params).empty());
        Space outdoors = Room().box(0.f, 0.f, 8.f, 6.f).space();
        outdoors.mBucket = EnclosureBucket::OUTDOORS;
        ensure("outdoors gets full probes", !spaceProbes(outdoors, params).empty());
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

    template<> template<>
    void sknavprobes_object::test<7>()
    {
        set_test_name("new spheres match the nearest held one, each at most once");
        const std::vector<LLVector3> held = { LLVector3(0.f, 0.f, 1.f), LLVector3(10.f, 0.f, 1.f) };
        const std::vector<LLVector3> origins = { LLVector3(10.5f, 0.f, 1.f), LLVector3(0.3f, 0.f, 1.f), LLVector3(0.f, 1.f, 1.f), LLVector3(40.f, 0.f, 1.f) };
        const std::vector<S32> m = matchNearest(held, origins, 4.f);
        ensure_equals("renumbered first", m[0], 1);
        ensure_equals("renumbered second", m[1], 0);
        ensure_equals("a held one is used once", m[2], -1);
        ensure_equals("too far is new", m[3], -1);
    }

    template<> template<>
    void sknavprobes_object::test<8>()
    {
        set_test_name("a held sphere keeps its place in the budget against a slightly nearer one");
        std::vector<ProbeSlot> slots(2);
        slots[0].mKey = 0;
        slots[0].mDistance = 20.f;
        slots[0].mHeld = true;
        slots[1].mKey = 1;
        slots[1].mDistance = 15.f;
        keepBudget(slots, 1, 8.f);
        ensure_equals("held kept", slots[0].mKey, 0ull);
        slots.resize(2);
        slots[0].mKey = 0;
        slots[0].mDistance = 30.f;
        slots[0].mHeld = true;
        slots[1].mKey = 1;
        slots[1].mDistance = 15.f;
        slots[1].mHeld = false;
        keepBudget(slots, 1, 8.f);
        ensure_equals("a much nearer one wins", slots[0].mKey, 1ull);
    }

    template<> template<>
    void sknavprobes_object::test<9>()
    {
        set_test_name("tile rebuilds that renumber parts move few probes");
        // A hall with an alcove, and a second alcove larger than the first coming and going,
        // which renumbers the parts; then ragged cells at the edges.
        std::vector<Room> steps(5);
        steps[0].box(0.f, 0.f, 10.f, 10.f).box(10.f, 0.f, 16.f, 2.f);
        steps[1].box(0.f, 0.f, 10.f, 10.f).box(10.f, 0.f, 16.f, 2.f).box(0.f, 10.f, 3.f, 15.f);
        steps[2].box(0.f, 0.f, 10.f, 10.f).box(10.f, 0.f, 16.f, 2.f);
        steps[3].box(0.f, 0.f, 10.f, 10.f).box(10.f, 0.f, 16.f, 2.f).box(10.f, 5.f, 10.5f, 6.f);
        steps[4].box(0.f, 0.f, 10.f, 10.f).box(10.f, 0.f, 16.f, 2.f).box(0.f, 10.f, 3.f, 15.f);
        const ProbeParams params;

        // Each strategy: count spheres created or moved past the thresholds.
        auto run = [&](bool nearest)
        {
            std::vector<ProbePart> held = spaceProbes(steps[0].space(), params);
            U32 churn = 0;
            for (size_t i = 1; i < steps.size(); ++i)
            {
                const std::vector<ProbePart> parts = spaceProbes(steps[i].space(), params);
                std::vector<LLVector3> ho, po;
                for (const ProbePart& p : held)
                {
                    ho.push_back(p.mOrigin);
                }
                for (const ProbePart& p : parts)
                {
                    po.push_back(p.mOrigin);
                }
                const std::vector<S32> m = matchNearest(ho, po, params.mMatchM);
                std::vector<ProbePart> next;
                for (size_t k = 0; k < parts.size(); ++k)
                {
                    const S32 h = nearest ? m[k] : (k < held.size() ? static_cast<S32>(k) : -1);
                    if (h < 0 || shouldReplace(held[h].mOrigin, held[h].mRadius, parts[k].mOrigin, parts[k].mRadius, params))
                    {
                        ++churn;
                        next.push_back(parts[k]);
                    }
                    else
                    {
                        next.push_back(held[h]);
                    }
                }
                held = next;
            }
            return churn;
        };
        const U32 by_key = run(false), by_nearest = run(true);
        std::cerr << "rebuild sequence: " << by_key << " regenerations by part index, " << by_nearest << " by nearest origin" << std::endl;
        ensure("nearest never churns more", by_nearest <= by_key);
        ensure("and churns less here", by_nearest < by_key);
    }

    template<> template<>
    void sknavprobes_object::test<10>()
    {
        set_test_name("census tests the collision shape, not its enclosing box");
        Record r;
        r.mSolid = true;
        r.mShape = Shape::BOX;
        r.mOrigin = LLVector3d(1000000.0, -1000000.0, 20.0);
        r.mMin = r.mOrigin - LLVector3d(3, 3, 3);
        r.mMax = r.mOrigin + LLVector3d(3, 3, 3);
        r.mHalf = LLVector3(2, 1, 1);
        r.mAxes[0] = LLVector3(0, 1, 0);
        r.mAxes[1] = LLVector3(-1, 0, 0);
        r.mAxes[2] = LLVector3(0, 0, 1);
        ensure("rotated box inside", probeInsideSolid(r, r.mOrigin + LLVector3d(0, 1.5, 0)));
        ensure("AABB corner outside", !probeInsideSolid(r, r.mOrigin + LLVector3d(1.5, 0, 0)));
        r.mProvenance = Provenance::BOX;
        ensure("placeholder is not evidence", !probeInsideSolid(r, r.mOrigin));
        r.mProvenance = Provenance::EXACT;
        r.mRole = Role::SURROUND;
        ensure("visual surround not collision", !probeInsideSolid(r, r.mOrigin));
        r.mRole = Role::NAV;
        r.mSolid = false;
        ensure("open surface not filled", !probeInsideSolid(r, r.mOrigin));
        r.mSolid = true;
        r.mShape = Shape::SPHERE;
        ensure("ellipsoid center", probeInsideSolid(r, r.mOrigin));
        ensure("ellipsoid corner", !probeInsideSolid(r, r.mOrigin + LLVector3d(0.9, 1.8, 0)));
        r.mShape = Shape::CYLINDER;
        ensure("cylinder inside", probeInsideSolid(r, r.mOrigin + LLVector3d(0, 1, 0.9)));
        ensure("cylinder above cap", !probeInsideSolid(r, r.mOrigin + LLVector3d(0, 0, 1.1)));
    }

    template<> template<>
    void sknavprobes_object::test<11>()
    {
        set_test_name("triangle solids preserve cavities and union overlapping hulls");
        Record r;
        r.mSolid = true;
        r.mMin = LLVector3d(-2, -2, -2);
        r.mMax = LLVector3d(2, 2, 2);
        auto tris = std::make_shared<std::vector<LLVector3>>();
        auto plane = [&](F32 z)
        {
            tris->insert(tris->end(), { LLVector3(-2,-2,z), LLVector3(2,-2,z), LLVector3(2,2,z),
                                      LLVector3(-2,-2,z), LLVector3(2,2,z), LLVector3(-2,2,z) });
        };
        // Horizontal cross-sections of a closed hollow body. Vertical faces
        // do not cross this ray; shared diagonals must count only once.
        plane(-2); plane(-1); plane(1); plane(2);
        r.mTriangles = tris;
        ensure("cavity remains empty", !probeInsideSolid(r, LLVector3d(0,0,0)));
        ensure("body below cavity", probeInsideSolid(r, LLVector3d(0,0,-1.5)));
        ensure("body above cavity", probeInsideSolid(r, LLVector3d(0,0,1.5)));
        tris->clear();
        plane(-2); plane(1); plane(-1); plane(2);
        r.mConvexTris = {4, 4};
        ensure("overlapping hulls form union", probeInsideSolid(r, LLVector3d(0,0,0)));
    }

    template<> template<>
    void sknavprobes_object::test<12>()
    {
        set_test_name("open sloping space captures above the chosen cell's floor");
        ProbeParams params;
        FloorPlan plan;
        plan.mW = plan.mH = 12;
        plan.mCell = 0.5f;
        plan.mCeiling = -100.f;
        plan.mFloor.resize(144);
        for (S32 y = 0; y < 12; ++y)
            for (S32 x = 0; x < 12; ++x)
                plan.mFloor[y * 12 + x] = x * 0.5f;
        const auto parts = placeParts(plan, params);
        ensure("has probes", !parts.empty());
        for (const auto& part : parts)
        {
            const S32 x = static_cast<S32>(part.mOrigin.mV[0] / plan.mCell);
            const S32 y = static_cast<S32>(part.mOrigin.mV[1] / plan.mCell);
            ensure("above local floor, not lowest corner", fabsf(part.mOrigin.mV[2] - plan.floorAt(x, y) - 1.5f) < 1e-4f);
        }
    }
    template<> template<>
    void sknavprobes_object::test<13>()
    {
        set_test_name("residual grid coverage remains above full probes and yields to manual probes");
        const ProbeParams params;
        ProbeInfluence nav;
        nav.mCentre = LLVector3(0, 0, 2);
        nav.mRadius = 12.f;
        ensure_equals("inside full probe has no grid", clearRadius(LLVector3(0,0,8), 13.f, {nav}, params), 0.f);
        ensure_equals("high unwalkable air keeps coverage", clearRadius(LLVector3(0,0,40), 13.f, {nav}, params), 13.f);
        ProbeInfluence manual;
        manual.mCentre = LLVector3(0,0,40);
        manual.mRadius = 8.f;
        ensure_equals("manual probe overrides high grid too", clearRadius(LLVector3(0,0,40), 13.f, {nav, manual}, params), 0.f);
    }

    template<> template<>
    void sknavprobes_object::test<14>()
    {
        set_test_name("a tight budget keeps one sphere per space, crowded ones out");
        ProbeParams params;
        params.mMaxPerSpace = 1;
        params.mSpacingM = 8.f;
        auto slot = [](U32 key, U32 space, U32 part, F32 distance, F32 area, F32 x)
        {
            ProbeSlot s;
            s.mKey = key;
            s.mSpace = space;
            s.mPart = part;
            s.mDistance = distance;
            s.mArea = area;
            s.mOrigin.set(x, 0.f, 0.f);
            return s;
        };
        std::vector<ProbeSlot> slots =
        {
            slot(0, 0, 1, 2.f, 4.f, 0.f),    // space 0's near small part
            slot(1, 0, 0, 5.f, 20.f, 12.f),  // and its far large one
            slot(2, 1, 0, 6.f, 20.f, 5.f),   // space 1 near space 0's kept sphere
            slot(3, 1, 1, 30.f, 4.f, 40.f),  // and its far one, clear of it
        };
        budgetProbes(slots, 3, params); // below twice the candidates, the budget is tight
        ensure_equals("one per space, the crowded dropped", slots.size(), 2u);
        ensure("space 0's near part stands", slots[0].mKey == 0ull);
        ensure("space 1's only clear part stands", slots[1].mKey == 3ull);

        slots =
        {
            slot(0, 0, 1, 2.f, 4.f, 0.f),
            slot(1, 0, 0, 5.f, 20.f, 12.f),
            slot(2, 1, 0, 6.f, 20.f, 5.f),
            slot(3, 1, 1, 30.f, 4.f, 40.f),
        };
        slots[1].mHeld = true; // a held part outranks its nearer sibling and keeps its ground
        budgetProbes(slots, 3, params);
        ensure_equals("the held part kept", slots[0].mKey, 1ull);
        ensure("the space's other part still out", slots[1].mKey == 3ull);

        slots =
        {
            slot(0, 0, 1, 2.f, 4.f, 0.f),
            slot(1, 0, 0, 5.f, 20.f, 12.f),
            slot(2, 1, 0, 6.f, 20.f, 5.f),
            slot(3, 1, 1, 30.f, 4.f, 40.f),
        };
        budgetProbes(slots, 8, params); // room for twice the candidates, so every part places
        ensure_equals("plenty places every part", slots.size(), 4u);
    }

    template<> template<>
    void sknavprobes_object::test<15>()
    {
        set_test_name("a tight budget caps across spaces, the far dropping first");
        const ProbeParams params;
        std::vector<ProbeSlot> slots(5);
        for (U32 i = 0; i < 5; ++i)
        {
            slots[i].mKey = i;
            slots[i].mSpace = i;
            slots[i].mDistance = 10.f * i;
            slots[i].mOrigin.set(100.f * i, 0.f, 0.f);
        }
        budgetProbes(slots, 2, params);
        ensure_equals("the cap holds", slots.size(), 2u);
        ensure("the nearest two kept", slots[0].mKey == 0ull && slots[1].mKey == 1ull);
    }

    template<> template<>
    void sknavprobes_object::test<16>()
    {
        set_test_name("the solid lattice answers by tile, the oversized bodies apart");

        auto box = [](bool solid, F32 cx, F32 cy, F32 cz, F32 hx, F32 hy, F32 hz)
        {
            std::shared_ptr<Record> r = std::make_shared<Record>();
            r->mSolid = solid;
            r->mShape = Shape::BOX;
            r->mCenter.set(cx, cy, cz);
            r->mAxes[0] = LLVector3::x_axis;
            r->mAxes[1] = LLVector3::y_axis;
            r->mAxes[2] = LLVector3::z_axis;
            r->mHalf.set(hx, hy, hz);
            r->mMin.set(cx - hx, cy - hy, cz - hz);
            r->mMax.set(cx + hx, cy + hy, cz + hz);
            return r;
        };

        std::vector<RecordPtr> records;
        records.push_back(box(true, 4.f, 4.f, 1.f, 4.f, 4.f, 1.f));
        // Reaching over more than a screen of 32 m tiles: kept separate, and
        // reached by every point test.
        records.push_back(box(true, 2100.f, 2100.f, 1.f, 1100.f, 1100.f, 1.f));
        records.push_back(box(false, 20.f, 4.f, 1.f, 2.f, 4.f, 1.f));

        SolidLattice lattice;
        lattice.rebuild(records);
        ensure("inside the small solid", lattice.contains(LLVector3d(4.f, 4.f, 1.f)));
        ensure("outside every solid", !lattice.contains(LLVector3d(40.f, 40.f, 1.f)));
        ensure("a body that is not solid is invisible", !lattice.contains(LLVector3d(20.f, 4.f, 1.f)));
        ensure("the oversized one answers anywhere in it", lattice.contains(LLVector3d(2100.f, 2100.f, 1.f)));

        lattice.rebuild({});
        ensure("a rebuild from nothing empties it", !lattice.contains(LLVector3d(4.f, 4.f, 1.f)));
    }

    template<> template<>
    void sknavprobes_object::test<17>()
    {
        set_test_name("the blend floor widens a corner pair's thin zone");
        // Two 2 m squares touching at a corner, ceiling 3: each placed sphere
        // reaches its farthest corner, so the pair intersects, but the zone
        // between them is the sliver the corner leaves, well under the owed
        // blend width. The floor grows them until they share a real zone.
        FloorPlan plan;
        plan.mX0 = plan.mY0 = 0;
        plan.mW = 12;
        plan.mH = 8;
        plan.mCell = 0.5f;
        plan.mCeiling = 3.f;
        plan.mFloor.assign(96, std::numeric_limits<F32>::quiet_NaN());
        for (S32 y = 0; y < 4; ++y)
        {
            for (S32 x = 0; x < 4; ++x)
            {
                plan.mFloor[y * 12 + x] = 3.f;              // the first square
            }
        }
        for (S32 y = 4; y < 8; ++y)
        {
            for (S32 x = 4; x < 12; ++x)
            {
                plan.mFloor[y * 12 + x] = 3.f;              // the second, at its corner
            }
        }
        const ProbeParams params;
        const std::vector<ProbePart> parts = placeParts(plan, params);
        ensure_equals("two parts", parts.size(), 2u);
        const F32 d = dist_vec(parts[0].mOrigin, parts[1].mOrigin);
        const F32 w = std::max(params.mBlendZoneM, params.mBlendShare * std::min(parts[0].mRadius, parts[1].mRadius));
        ensure("the spheres share a real blend zone", d < parts[0].mRadius + parts[1].mRadius - w * 0.5f);
        ensure("growth marked if it happened", parts[0].mGrowth == ProbePart::PLACED ||
               parts[0].mGrowth == ProbePart::GROWN || parts[0].mGrowth == ProbePart::WEAK);
        for (const ProbePart& p : parts)
        {
            ensure("still capped", p.mRadius <= params.mMaxRadiusM + 1e-3f);
        }
    }

    template<> template<>
    void sknavprobes_object::test<18>()
    {
        set_test_name("a chain of corner-touching parts converges inside the pass cap");
        // Six 2 m squares zigzagging on their corners: growth must propagate
        // down the chain, one hop a pass, until every corner pair shares a
        // zone worth blending across.
        FloorPlan plan;
        plan.mX0 = plan.mY0 = 0;
        plan.mW = 28;
        plan.mH = 8;
        plan.mCell = 0.5f;
        plan.mCeiling = 3.f;
        plan.mFloor.assign(224, std::numeric_limits<F32>::quiet_NaN());
        for (S32 k = 0; k < 6; ++k)
        {
            const S32 x0 = k * 4;
            const S32 y0 = (k % 2) * 4;
            for (S32 y = y0; y < y0 + 4; ++y)
            {
                for (S32 x = x0; x < x0 + 4; ++x)
                {
                    plan.mFloor[y * 28 + x] = 3.f;
                }
            }
        }
        const ProbeParams params;
        const std::vector<ProbePart> parts = placeParts(plan, params);
        ensure_equals("six parts", parts.size(), 6u);
        for (size_t a = 0; a < parts.size(); ++a)
        {
            for (size_t b = a + 1; b < parts.size(); ++b)
            {
                const F32 gap_x = std::max(std::max(parts[a].mMin.mV[VX] - parts[b].mMax.mV[VX], parts[b].mMin.mV[VX] - parts[a].mMax.mV[VX]), 0.f);
                const F32 gap_y = std::max(std::max(parts[a].mMin.mV[VY] - parts[b].mMax.mV[VY], parts[b].mMin.mV[VY] - parts[a].mMax.mV[VY]), 0.f);
                if (gap_x > plan.mCell + 0.01f || gap_y > plan.mCell + 0.01f)
                {
                    continue;   // these two never touch
                }
                const F32 d = dist_vec(parts[a].mOrigin, parts[b].mOrigin);
                const F32 w = std::max(params.mBlendZoneM, params.mBlendShare * std::min(parts[a].mRadius, parts[b].mRadius));
                ensure("every corner pair blends", d < parts[a].mRadius + parts[b].mRadius - w * 0.5f);
            }
        }
        for (const ProbePart& p : parts)
        {
            ensure("still capped", p.mRadius <= params.mMaxRadiusM + 1e-3f);
        }
    }

    template<> template<>
    void sknavprobes_object::test<19>()
    {
        set_test_name("a tight budget tiles a street with spheres, not one per space");
        const ProbeParams params;
        // One long street, its parts every 5 m, and a row of small rooms
        // beside it nearer the camera: the rooms must not starve the street.
        std::vector<ProbeSlot> slots;
        for (U32 i = 0; i < 40; ++i)
        {
            ProbeSlot s;
            s.mKey = i;
            s.mSpace = 0;
            s.mPart = i;
            s.mArea = 60.f;
            s.mOrigin.set(5.f * i, 0.f, 1.5f);
            s.mDistance = 10.f + 0.1f * i;
            slots.push_back(s);
        }
        for (U32 i = 0; i < 10; ++i)
        {
            ProbeSlot s;
            s.mKey = 100 + i;
            s.mSpace = 1 + i;
            s.mArea = 12.f;
            s.mOrigin.set(20.f * i, 12.f, 1.5f);
            s.mDistance = 1.f;
            slots.push_back(s);
        }
        std::vector<std::pair<U64, ProbeDrop>> dropped;
        budgetProbes(slots, 24, params, &dropped);   // under twice the 50 candidates: tight
        U32 street = 0;
        for (const ProbeSlot& s : slots)
        {
            street += s.mSpace == 0;
            for (const ProbeSlot& t : slots)
            {
                ensure("kept spheres keep their spacing", &s == &t || dist_vec(s.mOrigin, t.mOrigin) >= params.mSpacingM);
            }
        }
        ensure_equals("every room kept", static_cast<U32>(slots.size()) - street, 10u);
        ensure("the street holds several spheres", street >= 10u);
        ensure("the street's spheres stop at the per-space limit", street <= params.mMaxPerSpace);
        ensure_equals("every candidate kept or named dropped", slots.size() + dropped.size(), 50u);
        U32 spacing = 0;
        for (const auto& d : dropped)
        {
            spacing += d.second == ProbeDrop::SPACING;
        }
        ensure("street parts closer than the spacing dropped for it", spacing > 0u);
    }
}
