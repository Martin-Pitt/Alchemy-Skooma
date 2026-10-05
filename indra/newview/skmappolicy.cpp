/**
 * @file skmappolicy.cpp
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

#include "linden_common.h"

#include "skmappolicy.h"

#include "llmath.h"

namespace skmap
{
    Visibility visibilityOf(bool combat, bool self, bool ally)
    {
        if (self)
        {
            return Visibility::SELF;
        }
        if (!combat)
        {
            return Visibility::DOT;
        }
        return ally ? Visibility::DOT_CODED : Visibility::HIDDEN;
    }

    VehicleLook vehicleLook(bool combat, bool ally_aboard, bool crewed)
    {
        if (!combat)
        {
            return VehicleLook::CIVILIAN;
        }
        if (ally_aboard)
        {
            return VehicleLook::ALLY;
        }
        return crewed ? VehicleLook::HIDDEN : VehicleLook::NEUTRAL;
    }

    void EventRing::add(const SKCombatEvent& event)
    {
        mEvents[mNext] = event;
        mNext = (mNext + 1) % CAPACITY;
        mCount = llmin(mCount + 1, CAPACITY);
    }

    void EventRing::clear()
    {
        mNext = 0;
        mCount = 0;
    }

    void EventRing::visible(F64 now, F32 line_seconds, F32 icon_seconds, bool combat, std::vector<Draw>& out) const
    {
        out.clear();
        for (size_t k = 0; k < mCount; ++k)
        {
            // Oldest first, so newer events draw on top.
            const SKCombatEvent& e = mEvents[(mNext + CAPACITY - mCount + k) % CAPACITY];
            const F64 age = now - e.time;
            if (age < 0.0)
            {
                continue;
            }
            Draw d;
            d.mEvent = &e;
            if (age < line_seconds && line_seconds > 0.f && (!combat || e.actorAllied))
            {
                d.mLineAlpha = static_cast<F32>(1.0 - age / line_seconds);
            }
            if ((e.kind == SKCombatEvent::KILL || e.kind == SKCombatEvent::DEATH) && age < icon_seconds && (!combat || e.targetAllied))
            {
                const F64 left = icon_seconds - age;
                d.mIconAlpha = left >= EVENT_ICON_FADE_S ? 1.f : static_cast<F32>(left / EVENT_ICON_FADE_S);
            }
            if (d.mLineAlpha > 0.f || d.mIconAlpha > 0.f)
            {
                out.push_back(d);
            }
        }
    }

    LLSD seedFromClassic(const LLSD& classic, F32 widget_short_px)
    {
        LLSD out = LLSD::emptyMap();
        if (classic.has("MiniMapScale") && classic["MiniMapScale"].asReal() > 0.0)
        {
            const F64 metres = 256.0 * widget_short_px / classic["MiniMapScale"].asReal();
            out["SKMapZoomMeters"] = llclamp(metres, 4.0, 512.0);
        }
        static const char* TOGGLES[][2] = {
            { "MiniMapRotate", "SKMapRotate" },
            { "MiniMapAutoCenter", "SKMapAutoCenter" },
            { "MiniMapShowPropertyLines", "SKMapLayerParcels" },
            { "AlchemyMiniMapForSaleParcels", "SKMapLayerParcelsForSale" },
            { "AlchemyMiniMapCollisionParcels", "SKMapLayerParcelsCollision" },
            { "AlchemyMinimapChatRings", "SKMapLayerChatRings" },
            { "AlchemyMinimapGuideLine", "SKMapLayerGuideLine" },
        };
        for (const auto& toggle : TOGGLES)
        {
            if (classic.has(toggle[0]))
            {
                out[toggle[1]] = classic[toggle[0]].asBoolean();
            }
        }
        if (classic.has("AlchemyMinimapRenderObjects"))
        {
            out["SKMapLayerObjects"] = classic["AlchemyMinimapRenderObjects"].asBoolean() ? 1 : 0;
        }
        return out;
    }

    bool estatePower(bool agent_owns_region, bool agent_manages_estate, bool target_owns_region)
    {
        return (agent_owns_region || agent_manages_estate) && !target_owns_region;
    }

    ModerationVisibility moderationVisibility(const ModerationFacts& f)
    {
        ModerationVisibility v;
        GateState parcel = GateState::HIDDEN;
        if (f.mGod || f.mEstate || f.mClass == ParcelClass::SELF)
        {
            parcel = GateState::ENABLED;
        }
        else if (f.mClass == ParcelClass::GROUP || f.mInAgentParcel)
        {
            switch (f.mFacts)
            {
                case FactsState::KNOWN:
                    parcel = (f.mOwner == f.mAgent || (f.mGroupOwned && f.mOwnerPower)) ? GateState::ENABLED : GateState::HIDDEN;
                    break;
                case FactsState::PENDING:
                    parcel = GateState::CHECKING;
                    break;
                default:
                    parcel = GateState::HIDDEN;
                    break;
            }
        }
        v.mFreeze = parcel;
        v.mEject = parcel;
        v.mEstate = (f.mGod || f.mEstate) ? GateState::ENABLED : GateState::HIDDEN;
        return v;
    }

    bool ejectOffersBan(const ModerationFacts& f)
    {
        if (f.mGod || f.mClass == ParcelClass::SELF)
        {
            return true;
        }
        return f.mFacts == FactsState::KNOWN && (f.mOwner == f.mAgent || (f.mGroupOwned && f.mOwnerBanPower));
    }

    void ParcelFactsCache::insert(const ParcelFacts& facts)
    {
        // A cell has one parcel, and the newer reply wins.
        for (auto& entry : mEntries)
        {
            if (entry.first.first == facts.mRegion && entry.first.second != facts.mLocalId)
            {
                entry.second.mCells &= ~facts.mCells;
            }
        }
        mEntries[{ facts.mRegion, facts.mLocalId }] = facts;
    }

    const ParcelFacts* ParcelFactsCache::find(const LLUUID& region, S32 x, S32 y, F64 now) const
    {
        if (x < 0 || y < 0 || x >= PARCEL_CELLS || y >= PARCEL_CELLS)
        {
            return nullptr;
        }
        for (const auto& entry : mEntries)
        {
            const ParcelFacts& f = entry.second;
            if (entry.first.first == region && f.mCells.test(static_cast<size_t>(x + PARCEL_CELLS * y))
                && now - f.mTime < PARCEL_FACTS_EXPIRY_S)
            {
                return &f;
            }
        }
        return nullptr;
    }

    void ParcelFactsCache::dropRegion(const LLUUID& region)
    {
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            if (it->first.first == region)
            {
                it = mEntries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::pair<S32, S32> parcelCellOf(const LLVector3& local)
    {
        return { llclamp(static_cast<S32>(local.mV[VX] / 4.f), 0, PARCEL_CELLS - 1),
                 llclamp(static_cast<S32>(local.mV[VY] / 4.f), 0, PARCEL_CELLS - 1) };
    }

    bool parcelBitmapHas(const U8* bits, S32 x, S32 y)
    {
        if (!bits || x < 0 || y < 0 || x >= PARCEL_CELLS || y >= PARCEL_CELLS)
        {
            return false;
        }
        const S32 i = x + PARCEL_CELLS * y;
        return (bits[i >> 3] & (1 << (i & 7))) != 0;
    }

    bool isMapParcelSeq(S32 id)
    {
        return MAP_PARCEL_SEQ_LAST <= id && id <= MAP_PARCEL_SEQ_FIRST;
    }

    S32 nextMapParcelSeq(S32 previous)
    {
        if (!isMapParcelSeq(previous) || previous == MAP_PARCEL_SEQ_LAST)
        {
            return MAP_PARCEL_SEQ_FIRST;
        }
        return previous - 1;
    }

    std::vector<std::pair<std::string, LLSD>> presetValues(Preset preset)
    {
        switch (preset)
        {
            case Preset::COMBAT:
                return { { "SKMapLayerCover", true },           { "SKMapLayerStairs", true },
                         { "SKMapLayerContours", false },       { "SKMapLayerDoors", true },
                         { "SKMapLayerParcels", false },        { "SKMapLayerParcelsForSale", false },
                         { "SKMapLayerParcelsCollision", false }, { "SKMapLayerChatRings", false },
                         { "SKMapLayerGuideLine", false },      { "SKMapLayerVehicles", true },
                         { "SKMapLayerFiringLines", true },     { "SKMapLayerObjects", 1 } };
            case Preset::EXPLORE:
                return { { "SKMapLayerCover", true },           { "SKMapLayerStairs", true },
                         { "SKMapLayerContours", true },        { "SKMapLayerDoors", true },
                         { "SKMapLayerParcels", true },         { "SKMapLayerParcelsForSale", true },
                         { "SKMapLayerParcelsCollision", false }, { "SKMapLayerChatRings", false },
                         { "SKMapLayerGuideLine", false },      { "SKMapLayerVehicles", true },
                         { "SKMapLayerFiringLines", false },    { "SKMapLayerObjects", 1 } };
            case Preset::MINIMAL:
            default:
                return { { "SKMapLayerFloorplan", true },       { "SKMapLayerCover", false },
                         { "SKMapLayerStairs", false },         { "SKMapLayerContours", false },
                         { "SKMapLayerDoors", false },          { "SKMapLayerParcels", false },
                         { "SKMapLayerParcelsForSale", false }, { "SKMapLayerParcelsCollision", false },
                         { "SKMapLayerChatRings", false },      { "SKMapLayerGuideLine", false },
                         { "SKMapLayerVehicles", false },       { "SKMapLayerFiringLines", false },
                         { "SKMapLayerObjects", 0 } };
        }
    }
}
