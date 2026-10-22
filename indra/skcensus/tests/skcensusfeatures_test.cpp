/**
 * @file skcensusfeatures_test.cpp
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

#include "llmath.h"

#include <catch2/catch_test_macros.hpp>

#include "../skcensusfeatures.h"

#include <algorithm>
#include <memory>
#include <random>

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
        p.mFaceCover = 0.f;     // free-standing: measured, nothing against either face
        return p;
    }

    // A record spanning the box in a region at the origin, as an analytic box
    // or as its twelve triangles.
    RecordPtr slab(U32 n, F64 x0, F64 y0, F64 z0, F64 x1, F64 y1, F64 z1, bool triangles = false)
    {
        auto r = std::make_shared<Record>();
        r->mPart = id(n);
        r->mRoot = id(n);
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        const LLVector3 lo(static_cast<F32>(x0), static_cast<F32>(y0), static_cast<F32>(z0));
        const LLVector3 hi(static_cast<F32>(x1), static_cast<F32>(y1), static_cast<F32>(z1));
        r->mCenter = (lo + hi) * 0.5f;
        r->mHalf = (hi - lo) * 0.5f;
        r->mAxes[0].set(1.f, 0.f, 0.f);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mShape = Shape::BOX;
        if (triangles)
        {
            auto c = [&](S32 i) { return LLVector3(i & 1 ? hi.mV[0] : lo.mV[0], i & 2 ? hi.mV[1] : lo.mV[1], i & 4 ? hi.mV[2] : lo.mV[2]); };
            const S32 quads[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
            auto tris = std::make_shared<std::vector<LLVector3>>();
            for (const auto& q : quads)
            {
                for (S32 k : { q[0], q[1], q[2], q[0], q[2], q[3] })
                {
                    tris->push_back(c(k));
                }
            }
            r->mTriangles = tris;
            r->mShape = Shape::TRIANGLES;
        }
        return r;
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

    void ensure(const char* what, bool ok)
    {
        INFO(what);
        CHECK(ok);
    }

    template<typename A, typename B>
    void ensure_equals(const char* what, const A& actual, const B& expected)
    {
        INFO(what);
        CHECK(actual == static_cast<A>(expected));
    }

    void ensure_distance(const char* what, F64 actual, F64 expected, F64 tolerance)
    {
        INFO(what);
        CAPTURE(actual, expected);
        CHECK(std::fabs(actual - expected) <= tolerance);
    }

    void ensure_approximately_equals(const char* what, F32 actual, F32 expected, U32 frac_bits)
    {
        INFO(what);
        CAPTURE(actual, expected);
        CHECK(is_approx_equal_fraction(actual, expected, frac_bits));
    }
}

namespace
{

    // A thin upright panel is a door only with something to say for it.
    TEST_CASE("skcensusfeatures: a thin upright panel is a door only with something to say for it", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: two leaves side by side in one plane are one double door", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: the torus ladder", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: three landing doors stacked on one line make a shaft", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: sat on and moved is a vehicle and damageable by its description or its hover text", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: health in hover text", "[skcensusfeatures]")
    {
        ensure("47/50", hoverShowsHealth("47/50"));
        ensure("HP 47 / 50", hoverShowsHealth("HP 47 / 50"));
        ensure("not a path", !hoverShowsHealth("see /help"));
        ensure("not a fraction of words", !hoverShowsHealth("on/off"));
        ensure("a number on one side only", !hoverShowsHealth("page 2/x"));
        ensure("case-insensitive name", containsNoCase("[Ash] Ladder that works", "ladder"));
        ensure("masks", excludeFor(FeatureKind::VEHICLE) == (EXCLUDE_AVATAR | EXCLUDE_SOLID) && excludeFor(FeatureKind::SEAT) == 0
               && excludeFor(FeatureKind::ELEVATOR) == EXCLUDE_ALL);
    }

    // A door needs a door's width and real evidence: a name, or two of its
    // own script, touch and motion. A scripted root alone is never enough.
    TEST_CASE("skcensusfeatures: a door needs a door's width and real evidence", "[skcensusfeatures]")
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
        FeaturePart gate = part(1, 1, 0, 0, 0, 0.2, 5.0, 3.0);
        gate.mTouch = true;
        ensure("a named 5 m gate", isDoor(gate, root(1, "Yard Door")));
    }

    // A pane mostly seen through is never a door, whatever its evidence and
    // with no name to call it a window.
    TEST_CASE("skcensusfeatures: a pane mostly seen through is never a door whatever its evidence and with no name to call it a window", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: crewed is a vehicle only once it has travelled or is physical", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: a working shaft's height is its door stack's wherever the car is so the span holds still while the car rides", "[skcensusfeatures]")
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
    TEST_CASE("skcensusfeatures: a named linkset's pieces join only where they meet and a car's name is matched as a word", "[skcensusfeatures]")
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
        ensure("humps off", !containsWordNoCase("ForkLift", "lift", false) && !containsWordNoCase("AirLift", "lift", false));
        ensure("humps off, a word", containsWordNoCase("Lift Car", "lift", false));

        // A ForkLift or an AirLift someone rode is a vehicle, not a car.
        FeatureRoot fork = root(1, "ForkLift");
        fork.mEverCrewed = true;
        fork.mTravel = 5.f;
        ensure("ForkLift", ofKind(classifyFeatures({ part(1, 1, 0, 0, 0, 2, 1, 2) }, { fork }, FeatureParams()), FeatureKind::VEHICLE).size() == 1);
        fork.mName = "AirLift";
        ensure("AirLift", ofKind(classifyFeatures({ part(1, 1, 0, 0, 0, 2, 1, 2) }, { fork }, FeatureParams()), FeatureKind::VEHICLE).size() == 1);
    }

    // Someone riding a car named as an elevator up a storey does not make it
    // a vehicle: it stays the shaft's car.
    TEST_CASE("skcensusfeatures: someone riding a car named as an elevator up a storey does not make it a vehicle", "[skcensusfeatures]")
    {
        std::vector<FeaturePart> parts;
        std::vector<FeatureRoot> roots;
        for (U32 storey = 0; storey < 2; ++storey)
        {
            const F64 z = 100.0 + 8.75 * storey;
            FeaturePart a = part(10 + storey, 20 + storey, 50.0, 40.0, z, 50.125, 41.06, z + 3.0);
            a.mName = "door";
            parts.push_back(a);
            roots.push_back(root(10 + storey));
        }
        parts.push_back(part(30, 30, 50.2, 39.9, 108.6, 52.7, 42.4, 112.6));
        FeatureRoot car = root(30, "Elevator");
        car.mScripted = TriState::YES;
        car.mEverCrewed = car.mEverMoved = true;
        car.mTravel = 8.75f;
        roots.push_back(car);
        const std::vector<Feature> found = classifyFeatures(parts, roots, FeatureParams());
        ensure("not a vehicle", ofKind(found, FeatureKind::VEHICLE).empty() && ofKind(found, FeatureKind::SEAT).empty());
        const auto shafts = ofKind(found, FeatureKind::ELEVATOR);
        ensure("the shaft's car", shafts.size() == 1 && shafts[0]->mWorking && owns(*shafts[0], 30));
    }

    // A linkset's name holding "door" is not a door by itself: not inside
    // another word, and as a word only with one more sign of its own.
    TEST_CASE("skcensusfeatures: a linkset's name holding \"door\" is not a door by itself", "[skcensusfeatures]")
    {
        auto isDoor = [](FeaturePart p, FeatureRoot r) {
            p.mRoot = r.mRoot;
            return !ofKind(classifyFeatures({ p }, { r }, FeatureParams()), FeatureKind::DOOR).empty();
        };
        FeaturePart wall = part(1, 1, 0, 0, 0, 0.2, 4.0, 3.0);
        ensure("Outdoor Cafe", !isDoor(wall, root(1, "Outdoor Cafe")));
        ensure("House w/ doors", !isDoor(wall, root(1, "House w/ doors")));
        wall.mTouch = true;
        ensure("Outdoor Cafe, touched", !isDoor(wall, root(1, "Outdoor Cafe")));
        ensure("Door Set, touched", isDoor(wall, root(1, "Door Set")));
        wall.mTouch = false;
        wall.mName = "Indoor Pool Wall";
        ensure("a leaf named Indoor", !isDoor(wall, root(1)));
    }

    // A lone piece of a build named for a ladder is a ladder only if it is
    // shaped like one; a wall there joins floors to nothing.
    TEST_CASE("skcensusfeatures: a lone piece of a build named for a ladder is a ladder only if it is shaped like one", "[skcensusfeatures]")
    {
        const FeaturePart wall = part(1, 1, 10.0, 10.0, 20.0, 10.3, 18.0, 24.0);
        const FeaturePart rungs = part(1, 2, 30.0, 10.0, 20.0, 30.3, 10.6, 24.0);
        const std::vector<Feature> found = classifyFeatures({ wall, rungs }, { root(1, "Ladder Club") }, FeatureParams());
        const auto ladders = ofKind(found, FeatureKind::LADDER);
        ensure_equals("one ladder", ladders.size(), 1u);
        ensure("the thin one, not the wall", owns(*ladders[0], 2) && !owns(*ladders[0], 1));
    }

    // Twin shafts 3 m apart: with one stack's leaves slid open its box reaches
    // the other car, and each shaft still takes its own, the nearer one.
    TEST_CASE("skcensusfeatures: twin shafts 3 m apart", "[skcensusfeatures]")
    {
        std::vector<FeaturePart> parts;
        std::vector<FeatureRoot> roots;
        for (U32 storey = 0; storey < 2; ++storey)
        {
            const F64 z = 100.0 + 8.75 * storey;
            // Shaft one, open: its leaves' box runs 2.12 m either side of 41.06.
            FeaturePart a = part(10 + storey, 20 + storey, 50.0, 38.94, z, 50.125, 43.18, z + 3.0);
            // Shaft two, closed, on the line 3 m over.
            FeaturePart b = part(12 + storey, 22 + storey, 50.0, 43.3, z, 50.125, 44.82, z + 3.0);
            a.mName = b.mName = "door";
            parts.push_back(a);
            parts.push_back(b);
            roots.push_back(root(10 + storey));
            roots.push_back(root(12 + storey));
        }
        // Car two first in root order, its near edge 1.75 m from door one's
        // line; before both, a car of the next column back, in reach of line one.
        parts.push_back(part(29, 29, 52.9, 39.9, 100.0, 55.4, 42.2, 104.0));
        parts.push_back(part(30, 30, 50.2, 42.81, 100.0, 52.7, 45.31, 104.0));
        parts.push_back(part(31, 31, 50.2, 39.9, 100.0, 52.7, 42.2, 104.0));
        for (U32 n : { 29u, 30u, 31u })
        {
            FeatureRoot car = root(n, "Elevator");
            car.mScripted = TriState::YES;
            roots.push_back(car);
        }
        const std::vector<Feature> found = classifyFeatures(parts, roots, FeatureParams());
        const auto shafts = ofKind(found, FeatureKind::ELEVATOR);
        ensure_equals("two shafts", shafts.size(), 2u);
        for (const Feature* s : shafts)
        {
            ensure("both work", s->mWorking);
            const bool one = s->mMin.mdV[VY] < 41.0;
            ensure("each its own car", owns(*s, one ? 31 : 30) && !owns(*s, one ? 30 : 31));
        }
    }

    // A vehicle seen once stays one: parked non-physical with no travel on
    // record, it does not fall back to a seat.
    TEST_CASE("skcensusfeatures: a vehicle seen once stays one", "[skcensusfeatures]")
    {
        FeatureRoot boat = root(1);
        boat.mEverCrewed = true;
        const std::vector<FeaturePart> parts = { part(1, 1, 0, 0, 0, 6, 3, 2) };
        ensure("a seat until latched", ofKind(classifyFeatures(parts, { boat }, FeatureParams()), FeatureKind::SEAT).size() == 1);
        boat.mEverVehicle = true;
        const std::vector<Feature> f = classifyFeatures(parts, { boat }, FeatureParams());
        ensure("latched, a vehicle", ofKind(f, FeatureKind::VEHICLE).size() == 1 && ofKind(f, FeatureKind::SEAT).empty());
    }

    // A house root nudged is no evidence for its child panels: they stay walls,
    // while a root that is itself a panel is moved by its own motion.
    TEST_CASE("skcensusfeatures: a house root nudged is no evidence for its child panels", "[skcensusfeatures]")
    {
        FeatureRoot house = root(1);
        house.mEverMoved = true;
        FeaturePart base = part(1, 1, 0, 0, 0, 10, 10, 0.5);
        FeaturePart panel = part(1, 2, 20, 0, 0, 20.125, 1.06, 3.0);
        panel.mScripted = true;
        std::vector<Feature> f = classifyFeatures({ base, panel }, { house }, FeatureParams());
        ensure("the child panel stays a wall", ofKind(f, FeatureKind::DOOR).empty());

        FeaturePart leaf = part(1, 1, 20, 0, 0, 20.125, 1.06, 3.0);
        leaf.mScripted = true;
        f = classifyFeatures({ leaf }, { house }, FeatureParams());
        ensure("the root's own leaf moved", ofKind(f, FeatureKind::DOOR).size() == 1);
    }

    // Landing linksets named "Elevator Door" still give leaves and a shaft,
    // and a thin panel named as a lift nearer the line is never the car.
    TEST_CASE("skcensusfeatures: landing linksets named \"Elevator Door\" still give leaves and a shaft and a thin panel named as a lift nearer the line is never the car", "[skcensusfeatures]")
    {
        std::vector<FeaturePart> parts;
        std::vector<FeatureRoot> roots;
        for (U32 storey = 0; storey < 2; ++storey)
        {
            const F64 z = 100.0 + 8.75 * storey;
            FeaturePart a = part(10 + storey, 20 + storey, 50.0, 40.0, z, 50.125, 41.06, z + 3.0);
            a.mScripted = true;
            parts.push_back(a);
            roots.push_back(root(10 + storey, "Elevator Door"));
        }
        parts.push_back(part(30, 30, 50.2, 39.9, 100.0, 52.7, 42.4, 104.0));
        parts.push_back(part(32, 32, 50.3, 40.0, 100.0, 50.4, 41.0, 103.0));
        for (U32 n : { 30u, 32u })
        {
            FeatureRoot car = root(n, n == 30 ? "Elevator" : "Lift");
            car.mScripted = TriState::YES;
            roots.push_back(car);
        }
        const std::vector<Feature> found = classifyFeatures(parts, roots, FeatureParams());
        ensure_equals("two landing doors", ofKind(found, FeatureKind::DOOR).size(), 2u);
        const auto shafts = ofKind(found, FeatureKind::ELEVATOR);
        ensure("one working shaft", shafts.size() == 1 && shafts[0]->mWorking);
        ensure("the car, not the panel", owns(*shafts[0], 30) && !owns(*shafts[0], 32));
    }

    // A single door's entity: its leaf, the opening's size and the sill.
    TEST_CASE("skcensusfeatures: a single door's entity", "[skcensusfeatures]")
    {
        FeaturePart door = part(1, 7, 10.0, 20.0, 30.0, 10.125, 21.0, 33.0);
        door.mName = "Door";
        door.mScripted = true;
        FeatureRoot r = root(1, "House");
        r.mDescription = "LBA.v.1";
        const std::vector<FeatureRoot> roots = { r };
        const std::vector<Feature> f = classifyFeatures({ door }, roots, FeatureParams());
        const std::vector<DoorEntity> doors = buildDoorEntities(f, { door }, roots);
        ensure_equals("one entity", doors.size(), 1u);
        const DoorEntity& d = doors[0];
        ensure("id is the leaf", d.mId == id(7) && d.mLeaves.size() == 1 && !d.mDouble);
        ensure_approximately_equals("width", d.mWidth, 1.f, 8);
        ensure_approximately_equals("height", d.mHeight, 3.f, 8);
        ensure("sill", std::fabs(d.mSill - 30.0) < 1e-6 && std::fabs(d.mPlanePoint.mdV[VZ] - 30.0) < 1e-6);
        ensure("normal along +x", d.mNormal.mV[VX] > 0.99f);
        ensure("damageable", d.mDamageable);
        ensure("leaf's own name", d.mName == "Door");
    }

    // A coplanar double door is one entity with both leaves, as wide as both.
    TEST_CASE("skcensusfeatures: a coplanar double door is one entity with both leaves as wide as both", "[skcensusfeatures]")
    {
        FeaturePart left = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        FeaturePart right = part(1, 2, 0, 1.06, 0, 0.125, 2.12, 3.0);
        left.mTouch = right.mTouch = true;
        left.mScripted = right.mScripted = true;
        const std::vector<FeaturePart> parts = { left, right };
        const std::vector<FeatureRoot> roots = { root(1) };
        const std::vector<DoorEntity> doors = buildDoorEntities(classifyFeatures(parts, roots, FeatureParams()), parts, roots);
        ensure_equals("one entity", doors.size(), 1u);
        ensure("double with two leaves", doors[0].mDouble && doors[0].mLeaves.size() == 2);
        ensure_approximately_equals("both leaves wide", doors[0].mWidth, 2.12f, 8);
        ensure("not damageable", !doors[0].mDamageable);
    }

    // The id, leaves and normal hold whatever order the parts come in.
    TEST_CASE("skcensusfeatures: the id leaves and normal hold whatever order the parts come in", "[skcensusfeatures]")
    {
        FeaturePart left = part(1, 9, 0, 0, 0, 0.125, 1.06, 3.0);
        FeaturePart right = part(1, 4, 0, 1.06, 0, 0.125, 2.12, 3.0);
        left.mTouch = right.mTouch = true;
        left.mScripted = right.mScripted = true;
        const std::vector<FeatureRoot> roots = { root(1) };
        const std::vector<FeaturePart> ab = { left, right }, ba = { right, left };
        const std::vector<DoorEntity> one = buildDoorEntities(classifyFeatures(ab, roots, FeatureParams()), ab, roots);
        const std::vector<DoorEntity> two = buildDoorEntities(classifyFeatures(ba, roots, FeatureParams()), ba, roots);
        ensure("one each", one.size() == 1 && two.size() == 1);
        ensure("smallest leaf id", one[0].mId == id(4));
        ensure("same id", one[0].mId == two[0].mId);
        ensure("same leaves", one[0].mLeaves == two[0].mLeaves);
        ensure("same normal", one[0].mNormal == two[0].mNormal);
    }

    // The shut leaf and the same leaf swung open about a hinge at y = 0.
    FeaturePart shutLeaf(U32 n, F64 y0 = 0.0)
    {
        FeaturePart p = part(1, n, 0, y0, 0, 0.125, y0 + 1.06, 3.0);
        p.mScripted = p.mTouch = true;
        return p;
    }

    FeaturePart openLeaf(U32 n)
    {
        FeaturePart p = part(1, n, 0, -0.125, 0, 1.06, 0, 3.0);
        p.mScripted = p.mTouch = true;
        return p;
    }

    std::vector<DoorEntity> doorsNow(const std::vector<FeaturePart>& parts, LeafHistories& h)
    {
        const std::vector<FeatureRoot> roots = { root(1) };
        return buildDoorEntities(classifyFeatures(parts, roots, FeatureParams()), parts, roots, &h);
    }

    // A leaf seen open briefly and then shut keeps the shut plane, even when
    // looked at while open; while it swings, before its doorway is
    // established, the plane is not known.
    TEST_CASE("skcensusfeatures: a leaf seen open briefly and then shut keeps the shut plane even when looked at while open", "[skcensusfeatures]")
    {
        LeafHistories h;
        F32 t = 0.f;
        for (; t <= 5.f; t += 1.f)
        {
            observeLeaf(h[id(1)], shutLeaf(1), t);
        }
        observeLeaf(h[id(1)], openLeaf(1), t);
        std::vector<DoorEntity> d = doorsNow({ openLeaf(1) }, h);
        ensure("swinging: plane unknown", d.size() == 1 && !d[0].mPlaneKnown);
        t += 1.f;
        observeLeaf(h[id(1)], openLeaf(1), t);
        t += 1.f;
        observeLeaf(h[id(1)], openLeaf(1), t);
        d = doorsNow({ openLeaf(1) }, h);
        ensure("resting open: plane known", d.size() == 1 && d[0].mPlaneKnown);
        ensure("the shut plane", d[0].mNormal.mV[VX] > 0.99f && std::fabs(d[0].mPlanePoint.mdV[VX] - 0.0625) < 1e-4);
        ensure("the shut box", d[0].mMax.mdV[VX] < 0.2 && d[0].mMax.mdV[VY] > 1.0);
    }

    // A double door with one leaf standing open is still one cut: one plane
    // and the width of both leaves shut.
    TEST_CASE("skcensusfeatures: a double door with one leaf standing open is still one cut", "[skcensusfeatures]")
    {
        LeafHistories h;
        for (F32 t = 0.f; t <= 10.f; t += 1.f)
        {
            observeLeaf(h[id(1)], shutLeaf(1), t);
            observeLeaf(h[id(2)], shutLeaf(2, 1.06), t);
        }
        // Leaf 1 swings open and rests there briefly; the classifier still
        // sees two leaves, but not side by side, so it is given them apart.
        for (F32 t = 11.f; t <= 13.f; t += 1.f)
        {
            observeLeaf(h[id(1)], openLeaf(1), t);
            observeLeaf(h[id(2)], shutLeaf(2, 1.06), t);
        }
        std::vector<Feature> features = { Feature() };
        features[0].mKind = FeatureKind::DOOR;
        features[0].mParts = { id(1), id(2) };
        features[0].mNormal.set(0.f, 1.f, 0.f);
        const std::vector<FeaturePart> parts = { openLeaf(1), shutLeaf(2, 1.06) };
        const std::vector<DoorEntity> d = buildDoorEntities(features, parts, { root(1) }, &h);
        ensure("one door", d.size() == 1 && d[0].mDouble && d[0].mPlaneKnown);
        ensure_approximately_equals("both leaves wide", d[0].mWidth, 2.12f, 8);
        ensure("one plane at the doorway", d[0].mNormal.mV[VX] > 0.99f && std::fabs(d[0].mPlanePoint.mdV[VX] - 0.0625) < 1e-4);
    }

    // A double door keeps its id while one leaf drops out and comes back.
    TEST_CASE("skcensusfeatures: a double door keeps its id while one leaf drops out and comes back", "[skcensusfeatures]")
    {
        LeafHistories h;
        std::vector<DoorEntity> d = doorsNow({ shutLeaf(9), shutLeaf(4, 1.06) }, h);
        ensure("pair named for its smallest leaf", d.size() == 1 && d[0].mId == id(4));
        d = doorsNow({ shutLeaf(9) }, h);
        ensure("the leaf left keeps the id", d.size() == 1 && d[0].mId == id(4));
        d = doorsNow({ shutLeaf(9), shutLeaf(4, 1.06) }, h);
        ensure("and the pair again", d.size() == 1 && d[0].mId == id(4) && d[0].mDouble);
    }

    // The classifier sees a double door with one leaf standing open as two
    // doors; their rest poses make it one doorway again, under the pair's id.
    TEST_CASE("skcensusfeatures: the classifier sees a double door with one leaf standing open as two doors", "[skcensusfeatures]")
    {
        LeafHistories h;
        std::vector<DoorEntity> d = doorsNow({ shutLeaf(1), shutLeaf(2, 1.06) }, h);
        ensure("shut: one pair", d.size() == 1 && d[0].mId == id(1));
        for (F32 t = 0.f; t <= 10.f; t += 1.f)
        {
            observeLeaf(h[id(1)], shutLeaf(1), t);
            observeLeaf(h[id(2)], shutLeaf(2, 1.06), t);
        }
        for (F32 t = 11.f; t <= 13.f; t += 1.f)
        {
            observeLeaf(h[id(1)], openLeaf(1), t);
            observeLeaf(h[id(2)], shutLeaf(2, 1.06), t);
        }
        const std::vector<FeaturePart> parts = { openLeaf(1), shutLeaf(2, 1.06) };
        const std::vector<FeatureRoot> roots = { root(1) };
        const std::vector<Feature> features = classifyFeatures(parts, roots, FeatureParams());
        ensure_equals("the classifier sees two doors", ofKind(features, FeatureKind::DOOR).size(), 2u);
        d = buildDoorEntities(features, parts, roots, &h);
        ensure_equals("one entity", d.size(), 1u);
        ensure("the pair's id, both leaves", d[0].mId == id(1) && d[0].mDouble && d[0].mLeaves.size() == 2);
        ensure_approximately_equals("both leaves wide", d[0].mWidth, 2.12f, 8);
        ensure("one plane at the doorway", d[0].mPlaneKnown && d[0].mNormal.mV[VX] > 0.99f
                                               && std::fabs(d[0].mPlanePoint.mdV[VX] - 0.0625) < 1e-4);
    }

    // Script and touch with both faces open is a door; with a face unmeasured
    // it is not, and with something flat against a face it is mounted there.
    TEST_CASE("skcensusfeatures: script and touch with both faces open is a door", "[skcensusfeatures]")
    {
        FeaturePart leaf = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        leaf.mScripted = leaf.mTouch = true;
        auto isDoor = [](const FeaturePart& p, const FeatureRoot& r) {
            return !ofKind(classifyFeatures({ p }, { r }, FeatureParams()), FeatureKind::DOOR).empty();
        };
        const FeatureParams params;

        // A doorway: jambs either side and a header above, nothing on the faces.
        const std::vector<RecordPtr> frame = { slab(2, -0.05, -0.15, 0, 0.175, 0, 3.2), slab(3, -0.05, 1.06, 0, 0.175, 1.21, 3.2),
                                               slab(4, -0.05, -0.15, 3.0, 0.175, 1.21, 3.2), slab(5, -2, -2, -0.2, 2, 3, 0, true) };
        leaf.mFaceCover = faceCover(leaf, frame, params);
        ensure_equals("a frame and a floor cover nothing", leaf.mFaceCover, 0.f);
        ensure("framed, a door", isDoor(leaf, root(1)));
        leaf.mFaceCover = -1.f;
        ensure("unmeasured, script and touch are not enough", !isDoor(leaf, root(1)));

        // A painting on a wall, as triangles and as a box.
        for (const bool triangles : { true, false })
        {
            leaf.mFaceCover = faceCover(leaf, { slab(6, 0.125, -2, 0, 0.425, 3, 4, triangles) }, params);
            ensure("the wall covers the back", leaf.mFaceCover > 0.95f);
            ensure("mounted, not a door", !isDoor(leaf, root(1)));
        }

        // Only a name or motion outweighs it: a leaf seen open into a wall.
        FeaturePart named = leaf;
        named.mName = "Front Door";
        ensure("named", isDoor(named, root(1)));
        FeatureRoot moved = root(1);
        moved.mEverMoved = true;
        ensure("moved", isDoor(leaf, moved));
    }

    // A mattress against a headboard's lower half and a second board flat on
    // its face, as a bed's layered boards are: the boards are not leaves.
    // Bypass leaves overlapping only at an edge still are.
    TEST_CASE("skcensusfeatures: a mattress against a headboard's lower half and a second board flat on its face as a bed's layered boards are", "[skcensusfeatures]")
    {
        const FeatureParams params;
        FeaturePart board = part(1, 1, 0, 0, 0, 0.3, 3.0, 2.1);
        board.mScripted = board.mTouch = true;
        const std::vector<RecordPtr> bed = { slab(2, 0.3, 0, 0.1, 2.5, 3.0, 0.6), slab(3, -0.25, 0, 0, -0.05, 3.0, 2.1) };
        board.mFaceCover = faceCover(board, bed, params);
        ensure("the layered board covers a face", board.mFaceCover >= params.mFaceBlocked);
        ensure("a headboard", ofKind(classifyFeatures({ board }, { root(1) }, params), FeatureKind::DOOR).empty());

        FeaturePart a = part(1, 1, 0, 0, 0, 0.05, 1.0, 2.5);
        FeaturePart b = part(2, 2, 0.1, 0.9, 0, 0.15, 1.9, 2.5);
        a.mFaceCover = faceCover(a, { slab(2, 0.1, 0.9, 0, 0.15, 1.9, 2.5) }, params);
        b.mFaceCover = faceCover(b, { slab(1, 0, 0, 0, 0.05, 1.0, 2.5) }, params);
        ensure("an edge's overlap leaves the face open", a.mFaceCover <= params.mFaceClear && b.mFaceCover <= params.mFaceClear);
    }

    // A leaf that has rested shut long enough keeps its shut plane through a
    // swing, so opening the door moves nothing built over the doorway.
    TEST_CASE("skcensusfeatures: a leaf that has rested shut long enough keeps its shut plane through a swing so opening the door moves nothing built over the doorway", "[skcensusfeatures]")
    {
        LeafHistories h;
        F32 t = 0.f;
        for (; t <= DOORWAY_ESTABLISHED_SECONDS; t += 1.f)
        {
            observeLeaf(h[id(1)], shutLeaf(1), t);
        }
        ensure("shut and established", atEstablishedDoorway(h[id(1)]));
        const std::vector<DoorEntity> shut = doorsNow({ shutLeaf(1) }, h);
        observeLeaf(h[id(1)], openLeaf(1), t);
        ensure("swinging: no longer at it", !atEstablishedDoorway(h[id(1)]));
        const std::vector<DoorEntity> d = doorsNow({ openLeaf(1) }, h);
        ensure("swinging: plane still known", d.size() == 1 && d[0].mPlaneKnown);
        ensure("the same doorway", shut.size() == 1 && d[0].mPlanePoint == shut[0].mPlanePoint && d[0].mWidth == shut[0].mWidth
                                       && d[0].mMin == shut[0].mMin && d[0].mMax == shut[0].mMax);
    }

    // A record region-placed as the census makes them: global box, local geometry.
    const LLVector3d ORIGIN(256000.0, 256256.0, 0.0);

    RecordPtr placed(U32 n, const LLVector3d& centre, const LLVector3& half, F32 turn, bool triangles)
    {
        auto r = std::make_shared<Record>();
        r->mPart = id(n);
        r->mRoot = id(n);
        r->mOrigin = ORIGIN;
        r->mCenter = LLVector3(centre - ORIGIN);
        r->mHalf = half;
        r->mAxes[0].set(std::cos(turn), std::sin(turn), 0.f);
        r->mAxes[1].set(-std::sin(turn), std::cos(turn), 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mShape = Shape::BOX;
        LLVector3 span;
        for (S32 k = 0; k < 3; ++k)
        {
            for (S32 j = 0; j < 3; ++j)
            {
                span.mV[j] += std::fabs(r->mAxes[k].mV[j]) * half.mV[k];
            }
        }
        r->mMin = centre - LLVector3d(span);
        r->mMax = centre + LLVector3d(span);
        if (triangles)
        {
            auto c = [&](S32 i) {
                return r->mCenter + r->mAxes[0] * (i & 1 ? half.mV[0] : -half.mV[0]) + r->mAxes[1] * (i & 2 ? half.mV[1] : -half.mV[1])
                    + r->mAxes[2] * (i & 4 ? half.mV[2] : -half.mV[2]);
            };
            const S32 quads[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
            auto tris = std::make_shared<std::vector<LLVector3>>();
            for (const auto& q : quads)
            {
                for (S32 k : { q[0], q[1], q[2], q[0], q[2], q[3] })
                {
                    tris->push_back(c(k));
                }
            }
            r->mTriangles = tris;
            r->mShape = Shape::TRIANGLES;
        }
        return r;
    }

    // A leaf turned about z, its box the turned box's.
    FeaturePart turnedLeaf(U32 n, const LLVector3d& centre, F32 width, F32 thick, F32 height, F32 turn)
    {
        FeaturePart p;
        p.mPart = id(n);
        p.mRoot = id(n);
        p.mCenter = centre;
        p.mAxes[0].set(std::cos(turn), std::sin(turn), 0.f);
        p.mAxes[1].set(-std::sin(turn), std::cos(turn), 0.f);
        p.mAxes[2].set(0.f, 0.f, 1.f);
        p.mHalf.set(thick * 0.5f, width * 0.5f, height * 0.5f);
        LLVector3 span;
        for (S32 k = 0; k < 3; ++k)
        {
            for (S32 j = 0; j < 3; ++j)
            {
                span.mV[j] += std::fabs(p.mAxes[k].mV[j]) * p.mHalf.mV[k];
            }
        }
        p.mMin = centre - LLVector3d(span);
        p.mMax = centre + LLVector3d(span);
        return p;
    }

    // Leaves among walls, frames and clutter close enough to touch them, some
    // of it triangles, some of it larger than the index buckets.
    struct CoverScene
    {
        std::vector<RecordPtr> mRecords;
        std::vector<FeaturePart> mLeaves;
    };

    CoverScene coverScene(U32 seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<F32> u(0.f, 1.f);
        CoverScene s;
        U32 n = 1;
        for (S32 i = 0; i < 120; ++i)
        {
            const LLVector3d c = ORIGIN + LLVector3d(20.0 + u(rng) * 60.0, 20.0 + u(rng) * 60.0, 22.25 + 1.6);
            const F32 turn = u(rng) < 0.5f ? 0.f : u(rng) * F_TWO_PI;
            const FeaturePart leaf = turnedLeaf(n++, c, 0.6f + u(rng) * 1.5f, 0.02f + u(rng) * 0.3f, 2.1f + u(rng), turn);
            s.mLeaves.push_back(leaf);
            // Up to three things by its faces, near enough to cover them or not.
            for (S32 k = 0; k < i % 4; ++k)
            {
                const F32 side = u(rng) < 0.5f ? -1.f : 1.f;
                const F32 off = leaf.mHalf.mV[0] + u(rng) * 0.5f;
                const LLVector3d at = c + LLVector3d(leaf.mAxes[0] * (side * off) + leaf.mAxes[1] * ((u(rng) - 0.5f) * 2.f));
                const LLVector3 half(0.05f + u(rng) * 0.3f, 0.2f + u(rng), 0.5f + u(rng) * 1.5f);
                const F32 skew = u(rng) < 0.7f ? 0.f : u(rng);
                s.mRecords.push_back(placed(n++, at, half, turn + skew, u(rng) < 0.5f));
            }
        }
        // A megaprim floor under it all and a region-long wall.
        s.mRecords.push_back(placed(n++, ORIGIN + LLVector3d(128.0, 128.0, 22.0), LLVector3(128.f, 128.f, 0.2f), 0.f, true));
        s.mRecords.push_back(placed(n++, ORIGIN + LLVector3d(50.0, 128.0, 23.0), LLVector3(0.1f, 128.f, 3.f), 0.f, false));
        s.mRecords.push_back(nullptr);
        std::shuffle(s.mRecords.begin(), s.mRecords.end(), rng);
        return s;
    }

    TEST_CASE("skcensusfeatures: a record index query holds every record whose plan meets the box in record order", "[skcensusfeatures]")
    {
        const CoverScene s = coverScene(1);
        const RecordIndex index(s.mRecords, 4.0);
        std::mt19937 rng(2);
        std::uniform_real_distribution<F64> u(0.0, 1.0);
        std::vector<const RecordPtr*> found;
        for (S32 q = 0; q < 200; ++q)
        {
            const LLVector3d mn = ORIGIN + LLVector3d(u(rng) * 100.0, u(rng) * 100.0, 0.0);
            const LLVector3d mx = mn + LLVector3d(u(rng) * 6.0, u(rng) * 6.0, 50.0);
            index.query(mn, mx, found);
            CHECK(std::is_sorted(found.begin(), found.end()));
            CHECK(std::adjacent_find(found.begin(), found.end()) == found.end());
            for (const RecordPtr& r : s.mRecords)
            {
                if (r && r->mMin.mdV[VX] <= mx.mdV[VX] && mn.mdV[VX] <= r->mMax.mdV[VX] && r->mMin.mdV[VY] <= mx.mdV[VY]
                    && mn.mdV[VY] <= r->mMax.mdV[VY])
                {
                    CHECK(std::find(found.begin(), found.end(), &r) != found.end());
                }
            }
        }
    }

    TEST_CASE("skcensusfeatures: face cover over the index is the walk over every record", "[skcensusfeatures]")
    {
        const FeatureParams params;
        for (U32 seed = 1; seed <= 4; ++seed)
        {
            const CoverScene s = coverScene(seed);
            const RecordIndex index(s.mRecords);
            S32 covered = 0, open = 0;
            for (const FeaturePart& leaf : s.mLeaves)
            {
                const F32 walked = faceCover(leaf, s.mRecords, params);
                CAPTURE(seed, walked);
                CHECK(faceCover(leaf, index, params) == walked);
                covered += walked > 0.f ? 1 : 0;
                open += walked == 0.f ? 1 : 0;
            }
            // The scene exercises both answers, not only one.
            CHECK(covered > 10);
            CHECK(open > 0);
        }
    }

    TEST_CASE("skcensusfeatures: the face cover cache casts again only when the leaf its neighbours or the parameters change", "[skcensusfeatures]")
    {
        FeatureParams params;
        const FeaturePart leaf = part(1, 1, 0, 0, 0, 0.125, 1.06, 3.0);
        std::vector<RecordPtr> records = { slab(6, 0.125, -2, 0, 0.425, 3, 4, true), slab(7, 40, 40, 0, 41, 41, 3) };
        FaceCoverCache cache;
        const F32 first = cache.measure(leaf, RecordIndex(records), params);
        CHECK(first > 0.95f);
        CHECK(cache.mMisses == 1);

        // Nothing changed, or only something far off: the answer kept.
        CHECK(cache.measure(leaf, RecordIndex(records), params) == first);
        records[1] = slab(7, 40, 40, 0, 42, 42, 3);
        CHECK(cache.measure(leaf, RecordIndex(records), params) == first);
        CHECK(cache.mHits == 2);

        // The wall rebuilt, even as it was: a new record is cast against.
        records[0] = slab(6, 0.125, -2, 0, 0.425, 3, 4, true);
        CHECK(cache.measure(leaf, RecordIndex(records), params) == first);
        CHECK(cache.mMisses == 2);

        // The wall taken away, the leaf moved, a parameter changed.
        records[0] = slab(6, 5, -2, 0, 5.3, 3, 4, true);
        CHECK(cache.measure(leaf, RecordIndex(records), params) == 0.f);
        const FeaturePart moved = part(1, 1, 4.9, 0, 0, 5.025, 1.06, 3.0);
        CHECK(cache.measure(moved, RecordIndex(records), params) == faceCover(moved, records, params));
        params.mFaceReach = 0.01f;
        CHECK(cache.measure(moved, RecordIndex(records), params) == faceCover(moved, records, params));
        CHECK(cache.mMisses == 5);
        CHECK(cache.mHits == 2);

        // A part not measured between two prunes is forgotten.
        CHECK(cache.size() == 1);
        cache.prune();
        CHECK(cache.size() == 1);
        cache.prune();
        CHECK(cache.size() == 0);

        // Not door-shaped: never cast, never kept.
        CHECK(cache.measure(part(2, 2, 0, 0, 0, 1, 1, 1), RecordIndex(records), params) < 0.f);
        CHECK(cache.size() == 0);
    }
}
