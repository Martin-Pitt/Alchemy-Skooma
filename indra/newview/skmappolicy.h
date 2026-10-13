/**
 * @file skmappolicy.h
 * @brief Mini-map rules: who is drawn, combat events, classic seeding, the Moderate gate and presets.
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

#ifndef SK_MAPPOLICY_H
#define SK_MAPPOLICY_H

#include "llsd.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <bitset>
#include <map>
#include <string>
#include <utility>
#include <vector>

/// A hit, kill or death the combat log reports, for the mini-map to draw.
struct SKCombatEvent { enum Kind { HIT, KILL, DEATH } kind; LLUUID actor, target; LLVector3d actorPos, targetPos; F64 time; bool actorAllied, targetAllied; };

namespace skmap
{
    // Who is drawn. In combat only the user and resolved allies are: nothing marks an enemy,
    // and nothing relaxes this.
    enum class Visibility : U8 { HIDDEN, SELF, DOT, DOT_CODED };
    Visibility visibilityOf(bool combat, bool self, bool ally);

    /// How long combat outlasts the last engagement. Fixed, so nobody can shorten it.
    constexpr F64 ENGAGEMENT_HOLD_S = 240.0;

    /// Combat on damage land is earned: mouselook there, or a hit to or from the user, holds it
    /// for ENGAGEMENT_HOLD_S. Leaving damage land suspends it but does not reset the hold.
    class Engagement
    {
    public:
        /// Whether the map is in combat now.
        bool update(bool damage_land, bool mouselook, F64 now);
        /// A hit to or from the user.
        void engage(F64 now) { mLast = now; }
        bool combat() const { return mCombat; }
        /// When combat last started or ended, for the brief notice; negative before any change.
        F64 changedAt() const { return mChanged; }

    private:
        F64 mLast = -1.0e9;
        F64 mChanged = -1.0e9;
        bool mCombat = false;
    };

    enum class VehicleLook : U8 { HIDDEN, CIVILIAN, ALLY, NEUTRAL };
    /// General: every vehicle civilian. Combat: ally aboard, uncrewed neutral, crewed by
    /// nobody allied hidden.
    VehicleLook vehicleLook(bool combat, bool ally_aboard, bool crewed);

    /// Combat events as a ring that overwrites its oldest entry.
    class EventRing
    {
    public:
        static constexpr size_t CAPACITY = 256;
        void add(const SKCombatEvent& event);
        size_t size() const { return mCount; }
        void clear();

        struct Draw
        {
            const SKCombatEvent* mEvent = nullptr;
            F32 mLineAlpha = 0.f;           ///< actor to target, 0 when not drawn
            F32 mIconAlpha = 0.f;           ///< the cross at the target, 0 when not drawn
        };
        /// What is still drawn at now. In combat a line needs an allied actor and an icon an
        /// allied target.
        void visible(F64 now, F32 line_seconds, F32 icon_seconds, bool combat, std::vector<Draw>& out) const;

    private:
        SKCombatEvent mEvents[CAPACITY];
        size_t mNext = 0;
        size_t mCount = 0;
    };

    constexpr F32 EVENT_ICON_FADE_S = 0.2f;

    /// The classic map's settings to the new ones, once. Keys missing from classic are left
    /// out; the zoom is the metres the classic scale showed across the widget's short side.
    LLSD seedFromClassic(const LLSD& classic, F32 widget_short_px);

    // The Moderate gate. Only certain powers show; nothing is ever enabled on a guess.
    enum class ParcelClass : U8 { SELF, GROUP, OTHER };
    enum class FactsState : U8 { NONE, PENDING, KNOWN, FAILED };
    enum class GateState : U8 { HIDDEN, CHECKING, ENABLED };

    struct ModerationFacts
    {
        bool mGod = false;                  ///< a real god, not the admin-menu override
        bool mEstate = false;               ///< estate owner or manager, the target not the owner
        ParcelClass mClass = ParcelClass::OTHER;
        bool mInAgentParcel = false;
        FactsState mFacts = FactsState::NONE;
        LLUUID mAgent;
        LLUUID mOwner;                      ///< KNOWN only
        bool mGroupOwned = false;
        bool mOwnerPower = false;           ///< GP_LAND_ADMIN in the owning group
        bool mOwnerBanPower = false;        ///< GP_LAND_MANAGE_BANNED in the owning group
    };

    struct ModerationVisibility
    {
        GateState mFreeze = GateState::HIDDEN;
        GateState mEject = GateState::HIDDEN;
        GateState mEstate = GateState::HIDDEN;  ///< kick, teleport home and ban together
        bool row() const { return mFreeze != GateState::HIDDEN || mEject != GateState::HIDDEN || mEstate != GateState::HIDDEN; }
    };

    /// Estate power over a target: the user owns or manages the estate, and the target is not its owner.
    bool estatePower(bool agent_owns_region, bool agent_manages_estate, bool target_owns_region);
    ModerationVisibility moderationVisibility(const ModerationFacts& facts);
    /// Whether Eject offers to ban: the user's own land, or the owning group's ban power. Estate
    /// power alone does not, as upstream.
    bool ejectOffersBan(const ModerationFacts& facts);

    // Parcel facts from the map's own requests, cached by the cells the sim said they hold.
    constexpr S32 PARCEL_CELLS = 64;        ///< a side, 4 m each
    constexpr F64 PARCEL_FACTS_EXPIRY_S = 24.0 * 3600.0;

    struct ParcelFacts
    {
        LLUUID mRegion;
        S32 mLocalId = 0;
        LLUUID mOwner;
        bool mGroupOwned = false;
        LLUUID mGroup;
        std::bitset<PARCEL_CELLS * PARCEL_CELLS> mCells;
        F64 mTime = 0.0;
    };

    class ParcelFactsCache
    {
    public:
        /// Replaces an older entry of the parcel and takes its cells off every other parcel of the region.
        void insert(const ParcelFacts& facts);
        /// The parcel of the region whose cells hold the cell, or null; exact, no bounding box.
        const ParcelFacts* find(const LLUUID& region, S32 x, S32 y, F64 now) const;
        void dropRegion(const LLUUID& region);
        size_t size() const { return mEntries.size(); }
        void clear() { mEntries.clear(); }

    private:
        std::map<std::pair<LLUUID, S32>, ParcelFacts> mEntries;
    };

    /// The 4 m parcel cell a region-local position lies in, clamped to the region.
    std::pair<S32, S32> parcelCellOf(const LLVector3& local);
    /// Bit x + 64 y of a 512-byte parcel bitmap, least significant bit first.
    bool parcelBitmapHas(const U8* bits, S32 x, S32 y);

    /// The map's ParcelPropertiesRequest sequence ids, clear of upstream's -10000 to -50000.
    constexpr S32 MAP_PARCEL_SEQ_FIRST = -60000;
    constexpr S32 MAP_PARCEL_SEQ_LAST = -69999;
    bool isMapParcelSeq(S32 id);
    /// The next id: one down, wrapping from the last to the first.
    S32 nextMapParcelSeq(S32 previous);

    enum class Preset : U8 { COMBAT, EXPLORE, MINIMAL };
    /// The settings a preset writes, and only those.
    std::vector<std::pair<std::string, LLSD>> presetValues(Preset preset);
}

#endif // SK_MAPPOLICY_H
