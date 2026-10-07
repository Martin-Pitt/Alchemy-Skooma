/**
 * @file skmappolicy_test.cpp
 * @brief Mini-map rules: visibility, events, seeding, the Moderate gate, parcel facts, presets.
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

#include "../skmappolicy.h"

#include "llsdutil.h"

#include <cmath>
#include <set>

namespace
{
    using namespace skmap;

    const LLUUID AGENT("aaaaaaaa-0000-0000-0000-000000000001");
    const LLUUID GROUP("bbbbbbbb-0000-0000-0000-000000000002");
    const LLUUID STRANGER("cccccccc-0000-0000-0000-000000000003");
    const LLUUID REGION("dddddddd-0000-0000-0000-000000000004");
    const LLUUID REGION2("eeeeeeee-0000-0000-0000-000000000005");

    SKCombatEvent event(SKCombatEvent::Kind kind, F64 time, bool actor_allied, bool target_allied)
    {
        SKCombatEvent e = {};
        e.kind = kind;
        e.time = time;
        e.actorAllied = actor_allied;
        e.targetAllied = target_allied;
        return e;
    }

    ModerationFacts facts(ParcelClass cls, FactsState state)
    {
        ModerationFacts f;
        f.mAgent = AGENT;
        f.mClass = cls;
        f.mFacts = state;
        return f;
    }

    ParcelFacts parcel(const LLUUID& region, S32 local_id, const std::vector<std::pair<S32, S32>>& cells, F64 time = 0.0)
    {
        ParcelFacts p;
        p.mRegion = region;
        p.mLocalId = local_id;
        p.mOwner = STRANGER;
        p.mTime = time;
        for (const auto& c : cells)
        {
            p.mCells.set(static_cast<size_t>(c.first + 64 * c.second));
        }
        return p;
    }
}

namespace tut
{
    struct skmappolicy_data {};
    typedef test_group<skmappolicy_data> skmappolicy_group;
    typedef skmappolicy_group::object skmappolicy_object;
    tut::skmappolicy_group g("skmappolicy");

    // People and vehicles by mode.
    template<> template<>
    void skmappolicy_object::test<1>()
    {
        ensure("general self", visibilityOf(false, true, false) == Visibility::SELF);
        ensure("general ally", visibilityOf(false, false, true) == Visibility::DOT);
        ensure("general anyone", visibilityOf(false, false, false) == Visibility::DOT);
        ensure("combat self", visibilityOf(true, true, false) == Visibility::SELF);
        ensure("combat ally", visibilityOf(true, false, true) == Visibility::DOT_CODED);
        ensure("combat anyone else hidden", visibilityOf(true, false, false) == Visibility::HIDDEN);

        for (bool ally : { false, true })
        {
            for (bool crewed : { false, true })
            {
                ensure("general is civilian", vehicleLook(false, ally, crewed) == VehicleLook::CIVILIAN);
            }
        }
        ensure("combat ally aboard", vehicleLook(true, true, true) == VehicleLook::ALLY);
        ensure("combat uncrewed", vehicleLook(true, false, false) == VehicleLook::NEUTRAL);
        ensure("combat crewed by no ally", vehicleLook(true, false, true) == VehicleLook::HIDDEN);
    }

    // The event ring wraps, fades lines at 0.5 s and icons at 2 s, and filters in combat.
    template<> template<>
    void skmappolicy_object::test<2>()
    {
        EventRing ring;
        for (S32 i = 0; i < 300; ++i)
        {
            ring.add(event(SKCombatEvent::HIT, i, true, true));
        }
        ensure_equals("wraps at 256", ring.size(), static_cast<size_t>(256));
        std::vector<EventRing::Draw> out;
        ring.visible(299.0, 0.5f, 2.f, false, out);
        ensure_equals("only the newest line", out.size(), static_cast<size_t>(1));
        ensure("its time", out.front().mEvent->time == 299.0);

        EventRing kill;
        kill.add(event(SKCombatEvent::KILL, 10.0, true, true));
        kill.visible(10.25, 0.5f, 2.f, false, out);
        ensure("line half faded", out.size() == 1 && std::fabs(out[0].mLineAlpha - 0.5f) < 1e-4f && out[0].mIconAlpha == 1.f);
        kill.visible(10.6, 0.5f, 2.f, false, out);
        ensure("line gone, icon held", out.size() == 1 && out[0].mLineAlpha == 0.f && out[0].mIconAlpha == 1.f);
        kill.visible(11.9, 0.5f, 2.f, false, out);
        ensure("icon fading", out.size() == 1 && std::fabs(out[0].mIconAlpha - 0.5f) < 1e-3f);
        kill.visible(12.01, 0.5f, 2.f, false, out);
        ensure("icon gone", out.empty());

        EventRing combat;
        combat.add(event(SKCombatEvent::KILL, 0.0, false, true));
        combat.add(event(SKCombatEvent::DEATH, 0.0, true, false));
        combat.visible(0.1, 0.5f, 2.f, true, out);
        ensure_equals("both drawn in part", out.size(), static_cast<size_t>(2));
        ensure("no line from a non-ally", out[0].mLineAlpha == 0.f && out[0].mIconAlpha > 0.f);
        ensure("no icon on a non-ally", out[1].mLineAlpha > 0.f && out[1].mIconAlpha == 0.f);
        EventRing none;
        none.add(event(SKCombatEvent::HIT, 0.0, false, false));
        none.visible(0.1, 0.5f, 2.f, true, out);
        ensure("nothing of non-allies", out.empty());
    }

    // Seeding from the classic map.
    template<> template<>
    void skmappolicy_object::test<3>()
    {
        LLSD classic;
        classic["MiniMapScale"] = 128.0;
        classic["MiniMapRotate"] = true;
        classic["MiniMapAutoCenter"] = false;
        classic["MiniMapShowPropertyLines"] = true;
        classic["AlchemyMiniMapForSaleParcels"] = true;
        classic["AlchemyMiniMapCollisionParcels"] = false;
        classic["AlchemyMinimapChatRings"] = true;
        classic["AlchemyMinimapGuideLine"] = true;
        classic["AlchemyMinimapRenderObjects"] = false;
        const LLSD seeded = seedFromClassic(classic, 200.f);
        ensure("128 at 200 px is 400 m", std::fabs(seeded["SKMapZoomMeters"].asReal() - 400.0) < 1e-6);
        ensure("rotate is not seeded: the map always turns", !seeded.has("SKMapRotate"));
        ensure("auto-centre", !seeded["SKMapAutoCenter"].asBoolean());
        ensure("parcels", seeded["SKMapLayerParcels"].asBoolean() && seeded["SKMapLayerParcelsForSale"].asBoolean());
        ensure("collision", !seeded["SKMapLayerParcelsCollision"].asBoolean());
        ensure("chat rings and guide line", seeded["SKMapLayerChatRings"].asBoolean() && seeded["SKMapLayerGuideLine"].asBoolean());
        ensure_equals("objects off", seeded["SKMapLayerObjects"].asInteger(), 0);
        classic["MiniMapScale"] = 8.0;
        classic["AlchemyMinimapRenderObjects"] = true;
        const LLSD again = seedFromClassic(classic, 200.f);
        ensure("clamped at 2048", std::fabs(again["SKMapZoomMeters"].asReal() - 2048.0) < 1e-6);
        ensure_equals("objects where no floorplan", again["SKMapLayerObjects"].asInteger(), 1);
        ensure("idempotent", llsd_equals(seedFromClassic(classic, 200.f), again));
        ensure("a key missing stays missing", !seedFromClassic(LLSD::emptyMap(), 200.f).has("SKMapAutoCenter"));
    }

    // The Moderate gate, every branch.
    template<> template<>
    void skmappolicy_object::test<4>()
    {
        ModerationFacts god = facts(ParcelClass::OTHER, FactsState::NONE);
        god.mGod = true;
        ModerationVisibility v = moderationVisibility(god);
        ensure("god enables all", v.mFreeze == GateState::ENABLED && v.mEject == GateState::ENABLED && v.mEstate == GateState::ENABLED);
        ModerationFacts estate = facts(ParcelClass::OTHER, FactsState::NONE);
        estate.mEstate = true;
        v = moderationVisibility(estate);
        ensure("estate enables all", v.mFreeze == GateState::ENABLED && v.mEstate == GateState::ENABLED);
        ensure("estate power needs a target not the owner", estatePower(true, false, false) && estatePower(false, true, false)
                                                                 && !estatePower(true, true, true) && !estatePower(false, false, false));

        v = moderationVisibility(facts(ParcelClass::SELF, FactsState::NONE));
        ensure("own land enables freeze and eject", v.mFreeze == GateState::ENABLED && v.mEject == GateState::ENABLED);
        ensure("not the estate items", v.mEstate == GateState::HIDDEN);
        v = moderationVisibility(facts(ParcelClass::OTHER, FactsState::KNOWN));
        ensure("someone else's land hides them", !v.row());

        ModerationFacts group = facts(ParcelClass::GROUP, FactsState::PENDING);
        v = moderationVisibility(group);
        ensure("checking while pending", v.mFreeze == GateState::CHECKING && v.mEject == GateState::CHECKING && v.row());
        group.mFacts = FactsState::KNOWN;
        group.mOwner = GROUP;
        group.mGroupOwned = true;
        group.mOwnerPower = true;
        ensure("group power enables", moderationVisibility(group).mEject == GateState::ENABLED);
        group.mOwnerPower = false;
        ensure("no power hides", moderationVisibility(group).mEject == GateState::HIDDEN);
        group.mGroupOwned = false;
        group.mOwner = AGENT;
        ensure("the owner enables", moderationVisibility(group).mFreeze == GateState::ENABLED);
        group.mFacts = FactsState::FAILED;
        ensure("failed hides", !moderationVisibility(group).row());
        group.mFacts = FactsState::NONE;
        ensure("none hides", !moderationVisibility(group).row());

        ModerationFacts mine = facts(ParcelClass::OTHER, FactsState::KNOWN);
        mine.mInAgentParcel = true;
        mine.mOwner = AGENT;
        ensure("the agent's own parcel path", moderationVisibility(mine).mEject == GateState::ENABLED);
        mine.mOwner = STRANGER;
        ensure("the agent's parcel, owned by another", !moderationVisibility(mine).row());

        // Nothing is enabled while the facts are pending, whatever else holds, but god or estate.
        for (S32 cls = 0; cls < 3; ++cls)
        {
            for (S32 bits = 0; bits < 16; ++bits)
            {
                ModerationFacts f = facts(static_cast<ParcelClass>(cls), FactsState::PENDING);
                f.mInAgentParcel = bits & 1;
                f.mGroupOwned = bits & 2;
                f.mOwnerPower = bits & 4;
                f.mOwner = bits & 8 ? AGENT : STRANGER;
                const ModerationVisibility p = moderationVisibility(f);
                if (f.mClass != ParcelClass::SELF)
                {
                    ensure("never enabled while pending", p.mFreeze != GateState::ENABLED && p.mEject != GateState::ENABLED);
                }
                ensure("estate never without power", p.mEstate == GateState::HIDDEN);
            }
        }
    }

    // The ban option on Eject.
    template<> template<>
    void skmappolicy_object::test<5>()
    {
        ModerationFacts god = facts(ParcelClass::OTHER, FactsState::NONE);
        god.mGod = true;
        ensure("god", ejectOffersBan(god));
        ensure("own land", ejectOffersBan(facts(ParcelClass::SELF, FactsState::NONE)));
        ModerationFacts owner = facts(ParcelClass::GROUP, FactsState::KNOWN);
        owner.mOwner = AGENT;
        ensure("the owner", ejectOffersBan(owner));
        ModerationFacts group = facts(ParcelClass::GROUP, FactsState::KNOWN);
        group.mOwner = GROUP;
        group.mGroupOwned = true;
        group.mOwnerBanPower = true;
        ensure("the group's ban power", ejectOffersBan(group));
        ModerationFacts estate = facts(ParcelClass::OTHER, FactsState::NONE);
        estate.mEstate = true;
        ensure("not estate power alone", !ejectOffersBan(estate));
        group.mOwnerBanPower = false;
        group.mOwnerPower = true;
        ensure("not the eject power alone", !ejectOffersBan(group));
        group.mOwnerBanPower = true;
        for (FactsState s : { FactsState::PENDING, FactsState::FAILED, FactsState::NONE })
        {
            group.mFacts = s;
            ensure("not without known facts", !ejectOffersBan(group));
        }
    }

    // The parcel bitmap and cells.
    template<> template<>
    void skmappolicy_object::test<6>()
    {
        U8 bits[512] = {};
        const std::vector<std::pair<S32, S32>> set_cells = { { 0, 0 }, { 7, 0 }, { 8, 0 }, { 63, 63 }, { 20, 30 } };
        for (const auto& c : set_cells)
        {
            const S32 i = c.first + 64 * c.second;
            bits[i >> 3] |= static_cast<U8>(1 << (i & 7));
        }
        for (const auto& c : set_cells)
        {
            ensure("set", parcelBitmapHas(bits, c.first, c.second));
        }
        ensure("neighbours clear", !parcelBitmapHas(bits, 1, 0) && !parcelBitmapHas(bits, 6, 0) && !parcelBitmapHas(bits, 9, 0)
                                       && !parcelBitmapHas(bits, 0, 1) && !parcelBitmapHas(bits, 62, 63) && !parcelBitmapHas(bits, 21, 30)
                                       && !parcelBitmapHas(bits, 20, 29));
        ensure("0.0", parcelCellOf(LLVector3(0.f, 0.f, 0.f)) == std::make_pair(0, 0));
        ensure("3.99", parcelCellOf(LLVector3(3.99f, 3.99f, 0.f)) == std::make_pair(0, 0));
        ensure("4.0", parcelCellOf(LLVector3(4.f, 4.f, 0.f)) == std::make_pair(1, 1));
        ensure("255.9", parcelCellOf(LLVector3(255.9f, 255.9f, 0.f)) == std::make_pair(63, 63));
        ensure("clamped", parcelCellOf(LLVector3(-3.f, 300.f, 0.f)) == std::make_pair(0, 63));
    }

    // The parcel facts cache is exact by cell.
    template<> template<>
    void skmappolicy_object::test<7>()
    {
        ParcelFactsCache cache;
        // An L: a column and a row; its bounding box holds cells it does not.
        std::vector<std::pair<S32, S32>> l_shape;
        for (S32 i = 0; i < 8; ++i)
        {
            l_shape.emplace_back(0, i);
            l_shape.emplace_back(i, 0);
        }
        cache.insert(parcel(REGION, 1, l_shape));
        ensure("a set cell hits", cache.find(REGION, 0, 7, 0.0) != nullptr);
        ensure("a clear cell in the box misses", cache.find(REGION, 5, 5, 0.0) == nullptr);
        ensure("another region misses", cache.find(REGION2, 0, 7, 0.0) == nullptr);
        // A newer parcel takes cells from the older.
        cache.insert(parcel(REGION, 2, { { 0, 7 }, { 1, 7 } }));
        const ParcelFacts* hit = cache.find(REGION, 0, 7, 1.0);
        ensure("the newer wins", hit && hit->mLocalId == 2);
        ensure("the older keeps the rest", cache.find(REGION, 0, 6, 1.0)->mLocalId == 1);
        // A single-cell entry hits only its cell.
        cache.insert(parcel(REGION2, 5, { { 10, 10 } }));
        ensure("single cell", cache.find(REGION2, 10, 10, 0.0) && !cache.find(REGION2, 11, 10, 0.0));
        cache.dropRegion(REGION);
        ensure("dropped", !cache.find(REGION, 0, 6, 1.0) && cache.find(REGION2, 10, 10, 0.0));
        ensure("expires after a day", !cache.find(REGION2, 10, 10, 24.0 * 3600.0 + 1.0));
    }

    // The sequence id band.
    template<> template<>
    void skmappolicy_object::test<8>()
    {
        for (S32 id : { 0, 1, -1, -10000, -20000, -30000, -40000, -50000, -59999, -70000 })
        {
            ensure("outside the band", !isMapParcelSeq(id));
        }
        ensure("first", isMapParcelSeq(-60000));
        ensure("last", isMapParcelSeq(-69999));
        ensure_equals("steps down", nextMapParcelSeq(-60000), -60001);
        ensure_equals("wraps", nextMapParcelSeq(-69999), -60000);
        ensure_equals("out of band starts at the first", nextMapParcelSeq(5), -60000);
    }

    // Presets write only their own keys.
    template<> template<>
    void skmappolicy_object::test<9>()
    {
        auto keys = [](Preset p) {
            std::set<std::string> out;
            for (const auto& kv : presetValues(p))
            {
                out.insert(kv.first);
            }
            return out;
        };
        const std::set<std::string> common = { "SKMapLayerCover", "SKMapLayerStairs", "SKMapLayerContours", "SKMapLayerDoors",
                                               "SKMapLayerParcels", "SKMapLayerParcelsForSale", "SKMapLayerParcelsCollision",
                                               "SKMapLayerChatRings", "SKMapLayerGuideLine", "SKMapLayerVehicles",
                                               "SKMapLayerFiringLines", "SKMapLayerObjects" };
        ensure("combat", keys(Preset::COMBAT) == common);
        ensure("explore", keys(Preset::EXPLORE) == common);
        std::set<std::string> minimal = common;
        minimal.insert("SKMapLayerFloorplan");
        ensure("minimal", keys(Preset::MINIMAL) == minimal);
        for (const auto& kv : presetValues(Preset::COMBAT))
        {
            if (kv.first == "SKMapLayerContours")
            {
                ensure("combat drops contours", !kv.second.asBoolean());
            }
            if (kv.first == "SKMapLayerFiringLines")
            {
                ensure("combat keeps firing lines", kv.second.asBoolean());
            }
        }
    }
}
