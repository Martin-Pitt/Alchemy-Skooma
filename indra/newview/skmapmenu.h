/**
 * @file skmapmenu.h
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

#ifndef SK_MAPMENU_H
#define SK_MAPMENU_H

#include "llhandle.h"
#include "lluuid.h"
#include "v3dmath.h"

#include "skmappolicy.h"

#include <memory>
#include <string>
#include <vector>

class LLView;
class SKPopupPanes;
struct SKPopupItem;
struct SKPopupPaneSpec;

/// Builds the panes from values frozen at the click: nothing in them follows the people after.
class SKMapMenu
{
public:
    /// A person as they were when the menu opened.
    struct Person
    {
        LLUUID mId;
        std::string mLabel;             ///< as shown, RLVa-anonymised when names are hidden
        LLVector3d mPos;
        bool mNameHidden = false;
    };

    /// Freeze the people by id, in order, as they are now.
    static std::vector<Person> freeze(const std::vector<LLUUID>& ids);

    /// Open the map pane at the spawner's point (x, y), and the people pane beside it when anyone
    /// was frozen; a mini-map spawner also gets Recentre.
    static LLHandle<SKPopupPanes> open(LLView* spawner, S32 x, S32 y, const LLVector3d& world, const std::vector<Person>& people);

    /// A label from panel_sk_map_popup.xml.
    static std::string label(const std::string& name);

    /// What the Moderate gate knows of a person now: powers live, parcel facts cached or asked for.
    /// seq carries the person's parcel request from one refresh to the next.
    static skmap::ModerationFacts moderationFacts(const Person& person, S32& seq);

private:
    /// Where a person's Moderate rows sit, and their parcel request.
    struct Gate
    {
        Person mPerson;
        S32 mPane = -1;             ///< the pane holding the person's rows
        S32 mRow = -1;              ///< the Moderate row in it
        S32 mModerate = -1;         ///< the Moderate pane
        S32 mSeq = 0;
        F64 mRefusedAt = -1.0;      ///< when the request was refused at the in-flight cap
    };
    typedef std::shared_ptr<std::vector<Gate>> GatesPtr;

    static void personItems(std::vector<SKPopupPaneSpec>& panes, S32 pane, const Person& person, std::vector<Gate>& gates);
    /// Show, hide or mark checking each Moderate row by the gate, never enabling one on a guess.
    static void applyGates(std::vector<SKPopupPaneSpec>& panes, std::vector<Gate>& gates);
};

#endif // SK_MAPMENU_H
