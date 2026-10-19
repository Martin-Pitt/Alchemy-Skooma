/**
 * @file skmapmenu.cpp
 * @brief The mini-map's right-click panes: the map, the people under the cursor, and their actions.
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

#include "llviewerprecompiledheaders.h"

#include "skmapmenu.h"

#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llpanel.h"
#include "lluictrlfactory.h"

#include "alavataractions.h"
#include "alderenderlist.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "pipeline.h"
#include "llparcel.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "roles_constants.h"
#include "skmapparcelfacts.h"
#include "llavataractions.h"
#include "llavatarnamecache.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloaterworldmap.h"
#include "lltracker.h"
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "llviewercontrol.h"
#include "skmappeople.h"
#include "sknetmap.h"
#include "skpopuppanes.h"

#include <map>

namespace
{
    // Frames an avatar as the third-person camera frames the agent: the rear-view offset's distance and pitch,
    // behind the avatar's facing, turned round it until the line of sight is clear.
    void camOnAvatar(const LLUUID& id)
    {
        LLViewerObject* avatar = gObjectList.findObject(id);
        if (!avatar || !avatar->isAvatar())
        {
            handle_zoom_to_object(id);
            return;
        }

        const LLVector3 eye = gAgentCamera.getCameraOffsetInitial() * gAgentCamera.getCameraOffsetScale();
        const LLVector3 look = LLVector3(gAgentCamera.getFocusOffsetInitial());
        const F32 back = llmax(0.5f, look.mV[VX] - eye.mV[VX]);
        const F32 up = eye.mV[VZ] - look.mV[VZ];

        const LLVector3 focus = avatar->getRenderPosition() + LLVector3(0.f, 0.f, look.mV[VZ]);
        LLVector3 facing = LLVector3::x_axis * avatar->getRenderRotation();
        facing.mV[VZ] = 0.f;
        if (facing.normVec() < 0.01f)
        {
            facing = LLVector3::x_axis;
        }

        // The ray starts clear of the avatar and its attachments, and stops short of the camera's near plane.
        constexpr F32 SELF_CLEARANCE = 0.75f;
        constexpr F32 WALL_CLEARANCE = 0.3f;
        constexpr S32 YAW_STEPS = 12;

        LLVector3 best_cam;
        F32 best_clear = -1.f;
        for (S32 step = 0; step < YAW_STEPS && best_clear < 1.f; ++step)
        {
            // 0, +30, -30, +60, -60, ... 180 degrees round from behind.
            const S32 k = (step + 1) / 2;
            const F32 yaw = (step % 2 ? 1.f : -1.f) * k * F_TWO_PI / YAW_STEPS;
            const LLVector3 dir = -facing * LLQuaternion(yaw, LLVector3::z_axis);
            LLVector3 offset = dir * back + LLVector3(0.f, 0.f, up);
            const F32 length = offset.normVec();
            const LLVector3 cam = focus + offset * length;

            if (cam.mV[VZ] < LLWorld::getInstance()->resolveLandHeightAgent(cam) + WALL_CLEARANCE)
            {
                continue;
            }

            LLVector4a start, end, hit;
            start.load3((focus + offset * SELF_CLEARANCE).mV);
            end.load3(cam.mV);
            F32 reach = length;
            if (length > SELF_CLEARANCE &&
                gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, nullptr, &hit))
            {
                reach = (LLVector3(hit.getF32ptr()) - focus).length() - WALL_CLEARANCE;
            }

            const F32 clear = reach / length;
            if (clear > best_clear)
            {
                best_clear = clear;
                best_cam = focus + offset * llmax(reach, SELF_CLEARANCE);
            }
        }

        if (best_clear < 0.f)
        {
            // Every side is under the ground: look down from above.
            best_cam = focus + LLVector3(0.f, 0.f, back + up);
        }

        // A jump across the region warps rather than sweeping the camera through everything between.
        gAgentCamera.setFocusOnAvatar(false, ANIMATE);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(best_cam), gAgent.getPosGlobalFromAgent(focus), id);
        gAgentCamera.startCameraWarp(SKCameraWarp::AVATAR);
    }

    SKPopupItem action(const std::string& label, std::function<void()> fn, bool enabled = true, bool visible = true)
    {
        SKPopupItem item;
        item.mLabel = label;
        item.mAction = std::move(fn);
        item.mEnabled = enabled;
        item.mVisible = visible;
        return item;
    }

    SKPopupItem separator(bool visible = true)
    {
        SKPopupItem item;
        item.mKind = SKPopupItem::Kind::SEPARATOR;
        item.mVisible = visible;
        return item;
    }

    SKPopupItem heading(const std::string& label)
    {
        SKPopupItem item;
        item.mKind = SKPopupItem::Kind::HEADING;
        item.mLabel = label;
        return item;
    }
}

// static
std::string SKMapMenu::label(const std::string& name)
{
    // Read once from the panel's strings, so labels stay in XUI and translatable.
    static std::map<std::string, std::string> labels;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        static const char* NAMES[] = { "ping", "about_land", "place_profile", "world_map", "stop_tracking", "recenter", "settings", "tactical_settings",
                                       "view_profile", "cam", "im", "offer_teleport", "request_teleport", "teleport_to", "block",
                                       "unblock", "derender", "report", "moderate", "freeze", "parcel_eject", "estate_kick",
                                       "estate_teleport_home", "estate_ban", "checking", "more" };
        LLPanel::Params params;
        LLPanel* panel = LLUICtrlFactory::create<LLPanel>(params);
        if (panel->buildFromFile("panel_sk_map_popup.xml"))
        {
            for (const char* name : NAMES)
            {
                if (panel->hasString(name))
                {
                    labels[name] = panel->getString(name);
                }
            }
        }
        delete panel;
    }
    const auto it = labels.find(name);
    return it != labels.end() ? it->second : name;
}

// static
skmap::ModerationFacts SKMapMenu::moderationFacts(const Person& person, S32& seq)
{
    skmap::ModerationFacts f;
    f.mAgent = gAgent.getID();
    // Not isGodlike(): that counts the admin menu's override.
    f.mGod = gAgent.isGodlikeWithoutAdminMenuFakery();
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(person.mPos);
    if (!region)
    {
        f.mFacts = skmap::FactsState::FAILED;
        return f;
    }
    const LLVector3 local = region->getPosRegionFromGlobal(person.mPos);
    f.mEstate = skmap::estatePower(region->getOwner() == gAgent.getID(), region->isEstateManager(), person.mId == region->getOwner());
    f.mClass = region->isOwnedSelf(local) ? skmap::ParcelClass::SELF
             : region->isOwnedGroup(local) ? skmap::ParcelClass::GROUP
                                           : skmap::ParcelClass::OTHER;
    f.mInAgentParcel = LLViewerParcelMgr::getInstance()->inAgentParcel(person.mPos);

    const skmap::ParcelFacts* known = nullptr;
    skmap::ParcelFacts agent_parcel;
    if (f.mInAgentParcel)
    {
        // The sim keeps the agent's own parcel current: no request.
        if (LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel())
        {
            agent_parcel.mOwner = parcel->getOwnerID();
            agent_parcel.mGroupOwned = parcel->getIsGroupOwned();
            agent_parcel.mGroup = parcel->getGroupID();
            known = &agent_parcel;
        }
    }
    else if (f.mClass == skmap::ParcelClass::GROUP)
    {
        const std::pair<S32, S32> cell = skmap::parcelCellOf(local);
        SKMapParcelFacts& facts = SKMapParcelFacts::instance();
        known = facts.find(region->getRegionID(), cell.first, cell.second);
        if (!known)
        {
            if (seq == 0)
            {
                seq = facts.request(region, cell.first, cell.second);
                f.mFacts = seq ? skmap::FactsState::PENDING : skmap::FactsState::FAILED;
                seq = seq ? seq : -1;   // refused: failed, and not asked again
            }
            else
            {
                f.mFacts = skmap::isMapParcelSeq(seq) && facts.pending(seq) ? skmap::FactsState::PENDING : skmap::FactsState::FAILED;
            }
        }
    }
    if (known)
    {
        f.mFacts = skmap::FactsState::KNOWN;
        f.mOwner = known->mOwner;
        f.mGroupOwned = known->mGroupOwned;
        f.mOwnerPower = gAgent.hasPowerInGroup(known->mOwner, GP_LAND_ADMIN);
        f.mOwnerBanPower = gAgent.hasPowerInGroup(known->mOwner, GP_LAND_MANAGE_BANNED);
    }
    return f;
}

// static
void SKMapMenu::applyGates(std::vector<SKPopupPaneSpec>& panes, std::vector<Gate>& gates)
{
    // A request refused because eight were in flight is asked again a second later; one that
    // timed out or failed is not, so a sim that never answers is not asked over and over.
    constexpr F64 REFUSED_RETRY_S = 1.0;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const std::string checking = label("checking");
    for (Gate& gate : gates)
    {
        if (gate.mSeq == -1 && gate.mRefusedAt >= 0.0 && now - gate.mRefusedAt >= REFUSED_RETRY_S)
        {
            gate.mSeq = 0;
        }
        const bool asking = gate.mSeq == 0;
        const skmap::ModerationVisibility v = skmap::moderationVisibility(moderationFacts(gate.mPerson, gate.mSeq));
        if (asking)
        {
            gate.mRefusedAt = gate.mSeq == -1 ? now : -1.0;
        }
        const skmap::GateState states[5] = { v.mFreeze, v.mEject, v.mEstate, v.mEstate, v.mEstate };
        std::vector<SKPopupItem>& rows = panes[gate.mModerate].mItems;
        for (size_t i = 0; i < rows.size() && i < 5; ++i)
        {
            rows[i].mVisible = states[i] != skmap::GateState::HIDDEN;
            rows[i].mEnabled = states[i] == skmap::GateState::ENABLED;
            rows[i].mDetail = states[i] == skmap::GateState::CHECKING ? checking : std::string();
        }
        std::vector<SKPopupItem>& person_rows = panes[gate.mPane].mItems;
        person_rows[gate.mRow].mVisible = v.row();
        person_rows[gate.mRow - 1].mVisible = v.row();
    }
}

// static
void SKMapMenu::personItems(std::vector<SKPopupPaneSpec>& panes, S32 pane, const Person& person, std::vector<Gate>& gates)
{
    const LLUUID id = person.mId;
    const bool named = !person.mNameHidden;
    // Each check runs once, now; the rows do not change while open.
    std::vector<SKPopupItem>& items = panes[pane].mItems;
    items.push_back(action(label("view_profile"), [id]() { LLAvatarActions::showProfile(id); }, named));
    items.push_back(action(label("cam"), [id]() { camOnAvatar(id); }, ALAvatarActions::canZoomIn(id)));
    items.push_back(action(label("im"), [id]() { LLAvatarActions::startIM(id); }, named));
    items.push_back(separator());
    items.push_back(action(label("offer_teleport"), [id]() { LLAvatarActions::offerTeleport(id); }, named && LLAvatarActions::canOfferTeleport(id)));
    items.push_back(action(label("request_teleport"), [id]() { LLAvatarActions::teleportRequest(id); }, named));
    items.push_back(action(label("teleport_to"), [id]() { ALAvatarActions::teleportTo(id); }, named && ALAvatarActions::canTeleportTo(id)));
    items.push_back(separator());
    items.push_back(action(label(LLAvatarActions::isBlocked(id) ? "unblock" : "block"), [id]() { LLAvatarActions::toggleBlock(id); },
                           LLAvatarActions::canBlock(id)));
    const std::string name = person.mLabel;
    items.push_back(action(label("derender"), [id, name]() { ALDerenderList::instance().addAvatar(id, name); }));
    items.push_back(action(label("report"), [id]() { ALAvatarActions::reportAbuse(id); }));

    // The Moderate row and its pane; the gate shows the row and its items only for powers the
    // user certainly holds.
    const S32 moderate = static_cast<S32>(panes.size());
    SKPopupItem row = action(label("moderate"), nullptr, true, false);
    row.mDetail = label("more");
    row.mOpensPane = moderate;
    items.push_back(separator(false));
    items.push_back(row);
    Gate gate;
    gate.mPerson = person;
    gate.mPane = pane;
    gate.mRow = static_cast<S32>(items.size()) - 1;
    gate.mModerate = moderate;
    gates.push_back(gate);

    SKPopupPaneSpec spec;
    spec.mName = "moderate";
    spec.mParent = pane;
    spec.mItems.push_back(action(label("freeze"), [id]() { ALAvatarActions::parcelFreeze(id); }, false, false));
    // The ban option from the gate's own facts, re-read when chosen: no parcel is selected.
    spec.mItems.push_back(action(label("parcel_eject"),
                                 [person]() {
                                     // -1: cached facts only, never a fresh request; uncached reads FAILED, so no ban.
                                     S32 seq = -1;
                                     const skmap::ModerationFacts facts = moderationFacts(person, seq);
                                     ALAvatarActions::parcelEjectWithBan({ person.mId }, skmap::ejectOffersBan(facts));
                                 },
                                 false, false));
    spec.mItems.push_back(action(label("estate_kick"), [id]() { ALAvatarActions::estateKick(id); }, false, false));
    spec.mItems.push_back(action(label("estate_teleport_home"), [id]() { ALAvatarActions::estateTeleportHome(id); }, false, false));
    spec.mItems.push_back(action(label("estate_ban"), [id]() { ALAvatarActions::estateBan(id); }, false, false));
    panes.push_back(spec);
}

// static
std::vector<SKMapMenu::Person> SKMapMenu::freeze(const std::vector<LLUUID>& ids)
{
    std::vector<Person> people;
    for (const LLUUID& id : ids)
    {
        Person person;
        person.mId = id;
        person.mNameHidden = !RlvActions::canShowName(RlvActions::SNC_DEFAULT, id);
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(id, &av_name))
        {
            person.mLabel = person.mNameHidden ? RlvStrings::getAnonym(av_name) : av_name.getDisplayName();
        }
        for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
        {
            if (p.mId == id)
            {
                person.mPos = p.mPos;
                break;
            }
        }
        people.push_back(person);
    }
    return people;
}

// static
LLHandle<SKPopupPanes> SKMapMenu::open(LLView* spawner, S32 x, S32 y, const LLVector3d& world, const std::vector<Person>& people)
{
    std::vector<SKPopupPaneSpec> panes;
    SKPopupPaneSpec spec;
    spec.mName = "map";
    // Ping: a local marker for the user's own eyes, in combat only.
    static LLCachedControl<bool> ping(gSavedSettings, "SKMapPing", true);
    if (ping && SKMapPeople::instance().combat())
    {
        spec.mItems.push_back(action(label("ping"), [world]() { SKMapPeople::instance().addPing(world); }));
        spec.mItems.push_back(separator());
    }
    const bool location = RlvActions::canShowLocation();
    spec.mItems.push_back(action(label("about_land"),
                                 [world]() {
                                     // As the classic map: select the parcel there, then show it.
                                     LLViewerParcelMgr::getInstance()->deselectLand();
                                     LLParcelSelectionHandle selection = LLViewerParcelMgr::getInstance()->selectParcelAt(world);
                                     gMenuHolder->setParcelSelection(selection);
                                     LLFloaterReg::showInstance("about_land", LLSD(), false);
                                 },
                                 true, location));
    spec.mItems.push_back(action(label("place_profile"),
                                 [world]() {
                                     LLSD key;
                                     key["type"] = "remote_place";
                                     key["x"] = world.mdV[VX];
                                     key["y"] = world.mdV[VY];
                                     key["z"] = world.mdV[VZ];
                                     LLFloaterSidePanelContainer::showPanel("places", key);
                                 },
                                 true, location));
    spec.mItems.push_back(action(label("world_map"),
                                 [world]() {
                                     if (LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance())
                                     {
                                         map->trackLocation(world);
                                     }
                                     LLFloaterReg::showInstance("world_map");
                                 },
                                 true, location));
    spec.mItems.push_back(action(label("stop_tracking"), []() { LLTracker::stopTracking(false); }, true, LLTracker::isTracking(nullptr)));
    if (SKNetMap* map = dynamic_cast<SKNetMap*>(spawner))
    {
        LLHandle<LLView> handle = map->getHandle();
        spec.mItems.push_back(action(label("recenter"),
                                     [handle]() {
                                         if (SKNetMap* m = dynamic_cast<SKNetMap*>(handle.get()))
                                         {
                                             m->recentre();
                                         }
                                     },
                                     true, map->isPanned()));
    }
    spec.mItems.push_back(separator());
    // The tactical overview opens its own options; the mini-map its own.
    if (dynamic_cast<SKNetMap*>(spawner))
    {
        spec.mItems.push_back(action(label("settings"), []() { LLFloaterReg::showInstance("sk_map_settings"); }));
    }
    else
    {
        spec.mItems.push_back(action(label("tactical_settings"), []() { LLFloaterReg::showInstance("sk_tactical_options"); }));
    }
    panes.push_back(spec);

    auto gates = std::make_shared<std::vector<Gate>>();
    if (!people.empty())
    {
        SKPopupPaneSpec people_pane;
        people_pane.mName = "people";
        panes.push_back(people_pane);
        if (people.size() == 1)
        {
            panes[1].mItems.push_back(heading(people.front().mLabel));
            personItems(panes, 1, people.front(), *gates);
        }
        else
        {
            // One row per person, each opening a pane of that person's actions.
            for (const Person& person : people)
            {
                const S32 pane = static_cast<S32>(panes.size());
                SKPopupItem row = action(person.mLabel, nullptr);
                row.mDetail = label("more");
                row.mOpensPane = pane;
                panes[1].mItems.push_back(row);
                SKPopupPaneSpec person_pane;
                person_pane.mName = "person";
                person_pane.mParent = 1;
                panes.push_back(person_pane);
                personItems(panes, pane, person, *gates);
            }
        }
    }
    applyGates(panes, *gates);
    LLHandle<SKPopupPanes> shown = SKPopupPanes::show(spawner, x, y, std::move(panes));
    if (SKPopupPanes* view = shown.get())
    {
        // Only the Moderate rows change while open: "checking" turns enabled or goes.
        if (!gates->empty())
        {
            view->setRefresh([gates](std::vector<SKPopupPaneSpec>& specs) { applyGates(specs, *gates); });
        }
    }
    return shown;
}
