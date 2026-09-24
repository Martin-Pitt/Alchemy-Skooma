/**
 * @file sknavfeatures_test.cpp
 * @brief Tests for the feature rules: doors, ladders, elevators, vehicles and seats.
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

#include "../sknavfeatures.h"

#include <algorithm>

namespace
{
    using namespace sknav;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        return u;
    }

    // An axis-aligned part spanning the box, global metres.
    FeaturePart part(U32 root, U32 n, F64 x0, F64 y0, F64 z0, F64 x1, F64 y1, F64 z1)
    {
        FeaturePart p;
        p.mRoot = id(root);
        p.mPart = id(n);
        p.mMin.set(x0, y0, z0);
        p.mMax.set(x1, y1, z1);
        p.mCenter = (p.mMin + p.mMax) * 0.5;
        p.mAxes[0].set(1.f, 0.f, 0.f);
        p.mAxes[1].set(0.f, 1.f, 0.f);
        p.mAxes[2].set(0.f, 0.f, 1.f);
        p.mHalf.set(static_cast<F32>(x1 - x0) * 0.5f, static_cast<F32>(y1 - y0) * 0.5f, static_cast<F32>(z1 - z0) * 0.5f);
        return p;
    }

    FeatureRoot root(U32 n, const std::string& name = std::string())
    {
        FeatureRoot r;
        r.mRoot = id(n);
        r.mName = name;
        return r;
    }

    std::vector<const Feature*> ofKind(const std::vector<Feature>& features, FeatureKind kind)
    {
        std::vector<const Feature*> out;
        for (const Feature& f : features)
        {
            if (f.mKind == kind)
            {
                out.push_back(&f);
            }
        }
        return out;
    }

    bool owns(const Feature& f, U32 n)
    {
        return std::find(f.mParts.begin(), f.mParts.end(), id(n)) != f.mParts.end();
    }
}

namespace tut
{
    struct sknavfeatures_data {};
    typedef test_group<sknavfeatures_data> sknavfeatures_group;
    typedef sknavfeatures_group::object sknavfeatures_object;
    tut::sknavfeatures_group g("sknavfeatures");

    // A thin upright panel is a door only with something to say for it.
    template<> template<>
    void sknavfeatures_object::test<1>()
    {
        FeaturePart door = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        door.mName = "Door";
        door.mScripted = true;
        const FeaturePart wall = part(2, 2, 5, 0, 0, 5.125, 1.06, 3.0);
        const std::vector<Feature> f = classifyFeatures({ door, wall }, { root(1), root(2) }, FeatureParams());
        const auto doors = ofKind(f, FeatureKind::DOOR);
        ensure_equals("one door", doors.size(), 1u);
        ensure("the named scripted leaf", owns(*doors[0], 1));
        ensure("not the bare panel", !owns(*doors[0], 2));
        ensure("normal along x", std::fabs(doors[0]->mNormal.mV[VX]) > 0.99f);
    }

    // Two leaves side by side in one plane are one double door; a leaf in a
    // parallel wall a metre away is a door of its own.
    template<> template<>
    void sknavfeatures_object::test<2>()
    {
        FeaturePart left = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        FeaturePart right = part(1, 2, 0, 1.06, 0, 0.125, 2.12, 3.0);
        FeaturePart other = part(3, 3, 1.0, 0, 0, 1.125, 1.06, 3.0);
        left.mTouch = right.mTouch = other.mTouch = true;
        left.mScripted = right.mScripted = other.mScripted = true;
        const std::vector<Feature> f = classifyFeatures({ left, right, other }, { root(1), root(3) }, FeatureParams());
        const auto doors = ofKind(f, FeatureKind::DOOR);
        ensure_equals("two doors", doors.size(), 2u);
        const Feature* pair = owns(*doors[0], 1) ? doors[0] : doors[1];
        ensure("left and right pair", owns(*pair, 1) && owns(*pair, 2) && pair->mParts.size() == 2);
    }

    // The torus ladder: two pieces in a named linkset, two stacked above them
    // in an unnamed one, all on one footprint, union to one span. A record
    // beside it and the landing deck its top meets stay out.
    template<> template<>
    void sknavfeatures_object::test<3>()
    {
        const std::vector<FeaturePart> parts = {
            part(1, 1, 103.8, 52.5, 2986.3, 104.1, 53.6, 2991.1),
            part(1, 2, 103.8, 52.5, 2989.9, 104.1, 53.6, 2994.8),
            part(2, 3, 103.8, 52.5, 2992.3, 104.1, 53.6, 2997.1),
            part(2, 4, 103.8, 52.5, 2995.9, 104.1, 53.6, 3000.73),
            part(3, 5, 104.5, 52.5, 2986.3, 104.8, 53.6, 3000.0),     // beside it
            part(4, 6, 98.0, 48.0, 3000.4, 110.0, 58.0, 3000.77),     // the landing deck
        };
        const std::vector<FeatureRoot> roots = { root(1, "[ Rusty Ladder ]"), root(2), root(3), root(4) };
        const std::vector<Feature> f = classifyFeatures(parts, roots, FeatureParams());
        const auto ladders = ofKind(f, FeatureKind::LADDER);
        ensure_equals("one ladder", ladders.size(), 1u);
        const Feature& l = *ladders[0];
        ensure_equals("four pieces", l.mParts.size(), 4u);
        ensure("not the record beside it", !owns(l, 5));
        ensure("not the deck", !owns(l, 6));
        ensure_distance("foot", l.mFoot, 2986.3, 0.001);
        ensure_distance("top", l.mTop, 3000.73, 0.001);
        ensure_equals("two linksets", l.mRoots.size(), 2u);

        // Unnamed, it is nothing: the name is the detector.
        const std::vector<Feature> none = classifyFeatures(parts, { root(1), root(2), root(3), root(4) }, FeatureParams());
        ensure("no ladder without the name", ofKind(none, FeatureKind::LADDER).empty());
    }

    // Three landing doors stacked on one line make a shaft; a scripted car
    // named as an elevator in its column makes it work.
    template<> template<>
    void sknavfeatures_object::test<4>()
    {
        std::vector<FeaturePart> parts;
        std::vector<FeatureRoot> roots;
        for (U32 storey = 0; storey < 3; ++storey)
        {
            const F64 z = 100.0 + 8.75 * storey;
            FeaturePart a = part(10 + storey, 20 + storey * 2, 50.0, 40.0, z, 50.125, 41.06, z + 3.0);
            FeaturePart b = part(10 + storey, 21 + storey * 2, 50.0, 41.06, z, 50.125, 42.12, z + 3.0);
            a.mName = "door2";
            a.mScripted = b.mScripted = true;
            b.mTouch = true;
            parts.push_back(a);
            parts.push_back(b);
            roots.push_back(root(10 + storey));
        }
        const std::vector<Feature> dead = classifyFeatures(parts, roots, FeatureParams());
        ensure_equals("three double doors", ofKind(dead, FeatureKind::DOOR).size(), 3u);
        const auto shafts = ofKind(dead, FeatureKind::ELEVATOR);
        ensure_equals("one shaft", shafts.size(), 1u);
        ensure("no car, not working", !shafts[0]->mWorking);
        ensure_equals("three doors on it", shafts[0]->mDoors.size(), 3u);

        // The car, behind the doors on the +x side, at the middle storey.
        parts.push_back(part(30, 30, 50.2, 39.9, 108.6, 52.7, 42.4, 112.6));
        FeatureRoot car = root(30, "SmallOfficeElevator");
        car.mScripted = TriState::YES;
        roots.push_back(car);
        const std::vector<Feature> live = classifyFeatures(parts, roots, FeatureParams());
        const auto working = ofKind(live, FeatureKind::ELEVATOR);
        ensure_equals("still one shaft", working.size(), 1u);
        ensure("working", working[0]->mWorking);
        ensure("owns the car", owns(*working[0], 30));
        ensure("column is the car's footprint", working[0]->mMin.mdV[VX] > 50.1 && working[0]->mMax.mdV[VX] < 52.8);
        ensure_distance("foot at the lowest door", working[0]->mFoot, 100.0, 0.001);

        // Unscripted, the car is not one.
        roots.back().mScripted = TriState::NO;
        ensure("unscripted car does not work", !ofKind(classifyFeatures(parts, roots, FeatureParams()), FeatureKind::ELEVATOR)[0]->mWorking);
    }

    // Sat on and moved is a vehicle, and damageable by its description or
    // its hover text; sat on and never moved is a seat.
    template<> template<>
    void sknavfeatures_object::test<5>()
    {
        const std::vector<FeaturePart> parts = { part(1, 1, 0, 0, 0, 6, 10, 3), part(2, 2, 20, 0, 0, 21, 1, 1) };
        FeatureRoot tank = root(1);
        tank.mEverCrewed = tank.mEverMoved = true;
        tank.mTravel = 12.f;
        FeatureRoot chair = root(2);
        chair.mEverCrewed = true;

        std::vector<Feature> f = classifyFeatures(parts, { tank, chair }, FeatureParams());
        auto vehicles = ofKind(f, FeatureKind::VEHICLE);
        ensure_equals("one vehicle", vehicles.size(), 1u);
        ensure("the tank", owns(*vehicles[0], 1) && !vehicles[0]->mCombat);
        const auto seats = ofKind(f, FeatureKind::SEAT);
        ensure("the chair is a seat", seats.size() == 1 && owns(*seats[0], 2));

        tank.mDescription = "LBA.v.L.2.31,47,50";
        ensure("described damageable", ofKind(classifyFeatures(parts, { tank }, FeatureParams()), FeatureKind::VEHICLE)[0]->mCombat);
        tank.mDescription.clear();
        tank.mHover = "[LBA Light]\n[47/50]";
        ensure("hover damageable", ofKind(classifyFeatures(parts, { tank }, FeatureParams()), FeatureKind::VEHICLE)[0]->mCombat);

        tank.mClearance = 45.f;
        ensure("flying", ofKind(classifyFeatures(parts, { tank }, FeatureParams()), FeatureKind::VEHICLE)[0]->mAir);

        // Never crewed, however much it moves, is neither.
        tank.mEverCrewed = false;
        const std::vector<Feature> empty = classifyFeatures(parts, { tank }, FeatureParams());
        ensure("uncrewed mover is nothing", ofKind(empty, FeatureKind::VEHICLE).empty() && ofKind(empty, FeatureKind::SEAT).empty());
    }

    // Health in hover text: two numbers around a slash, and nothing else.
    template<> template<>
    void sknavfeatures_object::test<6>()
    {
        ensure("47/50", hoverShowsHealth("47/50"));
        ensure("HP 47 / 50", hoverShowsHealth("HP 47 / 50"));
        ensure("not a path", !hoverShowsHealth("see /help"));
        ensure("not a fraction of words", !hoverShowsHealth("on/off"));
        ensure("a number on one side only", !hoverShowsHealth("page 2/x"));
        ensure("case-insensitive name", containsNoCase("[Ash] Ladder that works", "ladder"));
        ensure("masks", excludeFor(FeatureKind::VEHICLE) == (EXCLUDE_AVATAR | EXCLUDE_WEATHER) && excludeFor(FeatureKind::SEAT) == 0);
    }

    // A door needs a door's width and real evidence: a name, or two of its
    // own script, touch and motion. A scripted root alone is never enough.
    template<> template<>
    void sknavfeatures_object::test<7>()
    {
        auto isDoor = [](FeaturePart p, FeatureRoot r) {
            p.mRoot = r.mRoot;
            return !ofKind(classifyFeatures({ p }, { r }, FeatureParams()), FeatureKind::DOOR).empty();
        };
        const FeaturePart leaf = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        FeatureRoot scripted_root = root(1);
        scripted_root.mScripted = TriState::YES;
        ensure("a scripted root alone", !isDoor(leaf, scripted_root));

        FeaturePart p = leaf;
        p.mScripted = true;
        ensure("its own script alone", !isDoor(p, root(1)));
        ensure("its own script and a scripted root", !isDoor(p, scripted_root));
        p.mTouch = true;
        ensure("script and touch", isDoor(p, root(1)));

        FeatureRoot moved = root(1);
        moved.mEverMoved = true;
        p = leaf;
        p.mTouch = true;
        ensure("touch and motion", isDoor(p, moved));

        p = leaf;
        p.mName = "Front Door";
        ensure("named", isDoor(p, root(1)));
        ensure("a named wall 10 m wide", !isDoor(part(1, 1, 0, 0, 0, 0.125, 10.0, 3.0), root(1, "door")));
        ensure("a named post 0.3 m wide", !isDoor(part(1, 1, 0, 0, 0, 0.1, 0.3, 3.0), root(1, "door")));
        ensure("a named 5 m gate", isDoor(part(1, 1, 0, 0, 0, 0.2, 5.0, 3.0), root(1, "Yard Door")));
    }

    // A pane mostly seen through is never a door, whatever its evidence and
    // with no name to call it a window.
    template<> template<>
    void sknavfeatures_object::test<8>()
    {
        FeaturePart pane = part(1, 1, 0, 0, 0, 0.018, 1.98, 3.15);
        pane.mScripted = pane.mTouch = true;
        pane.mSeeThrough = 0.8f;
        ensure("glass", ofKind(classifyFeatures({ pane }, { root(1) }, FeatureParams()), FeatureKind::DOOR).empty());
        pane.mSeeThrough = 0.2f;
        ensure("a door with a small window", ofKind(classifyFeatures({ pane }, { root(1) }, FeatureParams()), FeatureKind::DOOR).size() == 1);
    }

    // Crewed is a vehicle only once it has travelled or is physical: a swing
    // turns and a sofa gets nudged, and both stay seats.
    template<> template<>
    void sknavfeatures_object::test<9>()
    {
        const std::vector<FeaturePart> parts = { part(1, 1, 0, 0, 0, 2, 2, 2) };
        FeatureRoot swing = root(1);
        swing.mEverCrewed = swing.mEverMoved = true;
        swing.mTravel = 0.4f;
        auto kind = [&](const FeatureRoot& r) { return classifyFeatures(parts, { r }, FeatureParams()).front().mKind; };
        ensure("turned and nudged is a seat", kind(swing) == FeatureKind::SEAT);
        swing.mTravel = 1.5f;
        ensure("travelled is a vehicle", kind(swing) == FeatureKind::VEHICLE);
        swing.mTravel = 0.f;
        swing.mEverMoved = false;
        swing.mPhysical = true;
        ensure("physical is a vehicle", kind(swing) == FeatureKind::VEHICLE);
    }

    // A working shaft's height is its door stack's wherever the car is, so
    // the span holds still while the car rides.
    template<> template<>
    void sknavfeatures_object::test<10>()
    {
        std::vector<FeaturePart> parts;
        std::vector<FeatureRoot> roots;
        for (U32 storey = 0; storey < 3; ++storey)
        {
            const F64 z = 100.0 + 8.75 * storey;
            FeaturePart a = part(10 + storey, 20 + storey, 50.0, 40.0, z, 50.125, 41.06, z + 3.0);
            a.mName = "door";
            parts.push_back(a);
            roots.push_back(root(10 + storey));
        }
        FeatureRoot car = root(30, "Lift Car");
        car.mScripted = TriState::YES;
        roots.push_back(car);
        for (F64 floor : { 99.0, 108.6, 117.5 })
        {
            std::vector<FeaturePart> with_car = parts;
            with_car.push_back(part(30, 30, 50.2, 39.9, floor, 52.7, 42.4, floor + 4.0));
            const std::vector<Feature> found = classifyFeatures(with_car, roots, FeatureParams());
            const auto shafts = ofKind(found, FeatureKind::ELEVATOR);
            ensure("one working shaft", shafts.size() == 1 && shafts[0]->mWorking);
            ensure_distance("foot at the lowest door", shafts[0]->mFoot, 100.0, 0.001);
            ensure_distance("top at the highest door", shafts[0]->mTop, 120.5, 0.001);
        }
    }

    // A named linkset's pieces join only where they meet, and a car's name is
    // matched as a word.
    template<> template<>
    void sknavfeatures_object::test<11>()
    {
        const std::vector<FeaturePart> parts = {
            part(1, 1, 0.0, 0.0, 0.0, 0.3, 0.6, 4.0),
            part(1, 2, 0.0, 0.0, 3.95, 0.3, 0.6, 8.0),
            part(1, 3, 20.0, 0.0, 0.0, 20.3, 0.6, 4.0),
        };
        const std::vector<Feature> found = classifyFeatures(parts, { root(1, "Jacob's Ladder Club") }, FeatureParams());
        const auto ladders = ofKind(found, FeatureKind::LADDER);
        ensure_equals("two ladders", ladders.size(), 2u);
        const Feature& tall = owns(*ladders[0], 1) ? *ladders[0] : *ladders[1];
        ensure("the stacked pair is one", owns(tall, 2) && !owns(tall, 3));

        ensure("Lift", containsWordNoCase("Lift Car", "lift"));
        ensure("camel case", containsWordNoCase("SmallOfficeElevator", "elevator"));
        ensure("plural", containsWordNoCase("elevators", "elevator"));
        ensure("not forklift", !containsWordNoCase("forklift", "lift"));
        ensure("not airlift", !containsWordNoCase("Airlift Pad", "lift"));
        ensure("not shoplifter", !containsWordNoCase("shoplifter", "lift"));
    }
}
