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
#include "llpanel.h"
#include "lluictrlfactory.h"

#include "alavataractions.h"
#include "llavataractions.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloaterworldmap.h"
#include "lltracker.h"
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "rlvactions.h"
#include "sknetmap.h"
#include "skpopuppanes.h"

#include <map>

namespace
{
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
        static const char* NAMES[] = { "ping", "about_land", "place_profile", "world_map", "stop_tracking", "recenter", "settings",
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
void SKMapMenu::personItems(std::vector<SKPopupPaneSpec>& panes, S32 pane, const Person& person)
{
    const LLUUID id = person.mId;
    const bool named = !person.mNameHidden;
    // Each check runs once, now; the rows do not change while open.
    std::vector<SKPopupItem>& items = panes[pane].mItems;
    items.push_back(action(label("view_profile"), [id]() { LLAvatarActions::showProfile(id); }, named));
    items.push_back(action(label("cam"), [id]() { ALAvatarActions::zoomIn(id); }, ALAvatarActions::canZoomIn(id)));
    items.push_back(action(label("im"), [id]() { LLAvatarActions::startIM(id); }, named));
    items.push_back(separator());
    items.push_back(action(label("offer_teleport"), [id]() { LLAvatarActions::offerTeleport(id); }, named && LLAvatarActions::canOfferTeleport(id)));
    items.push_back(action(label("request_teleport"), [id]() { LLAvatarActions::teleportRequest(id); }, named));
    items.push_back(action(label("teleport_to"), [id]() { ALAvatarActions::teleportTo(id); }, named && ALAvatarActions::canTeleportTo(id)));
    items.push_back(separator());
    items.push_back(action(label(LLAvatarActions::isBlocked(id) ? "unblock" : "block"), [id]() { LLAvatarActions::toggleBlock(id); },
                           LLAvatarActions::canBlock(id)));
    items.push_back(action(label("derender"), []() {}, true, false));
    items.push_back(action(label("report"), [id]() { ALAvatarActions::reportAbuse(id); }));

    // The Moderate row and its pane; shown once the gate knows what the user may do.
    const S32 moderate = static_cast<S32>(panes.size());
    SKPopupItem row = action(label("moderate"), nullptr, true, false);
    row.mDetail = label("more");
    row.mOpensPane = moderate;
    items.push_back(separator(false));
    items.push_back(row);
    SKPopupPaneSpec spec;
    spec.mName = "moderate";
    spec.mParent = pane;
    panes.push_back(spec);
}

// static
LLHandle<SKPopupPanes> SKMapMenu::open(SKNetMap* map, S32 x, S32 y, const LLVector3d& world, const std::vector<Person>& people)
{
    std::vector<SKPopupPaneSpec> panes;
    SKPopupPaneSpec spec;
    spec.mName = "map";
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
    LLHandle<LLView> handle = map->getHandle();
    spec.mItems.push_back(action(label("recenter"),
                                 [handle]() {
                                     if (SKNetMap* m = dynamic_cast<SKNetMap*>(handle.get()))
                                     {
                                         m->recentre();
                                     }
                                 },
                                 true, map->isPanned()));
    panes.push_back(spec);

    if (!people.empty())
    {
        SKPopupPaneSpec people_pane;
        people_pane.mName = "people";
        panes.push_back(people_pane);
        if (people.size() == 1)
        {
            panes[1].mItems.push_back(heading(people.front().mLabel));
            personItems(panes, 1, people.front());
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
                personItems(panes, pane, person);
            }
        }
    }
    return SKPopupPanes::show(map, x, y, std::move(panes));
}
