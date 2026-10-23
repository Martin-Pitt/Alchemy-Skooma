/**
 * @file skroomscensus.cpp
 * @brief The rooms layer's section of the census file.
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

#include "skroomscensus.h"

#include "llsd.h"

#include <ostream>

namespace skworld
{
    namespace
    {
        F32 real(const LLSD& map, const char* key, F32 fallback)
        {
            return map.has(key) ? static_cast<F32>(map[key].asReal()) : fallback;
        }
    }

    void writeEnclosureParams(std::ostream& out, const EnclosureParams& e)
    {
        out << "{\"sky_decay\":" << e.mSkyDecayM << ",\"depth_full\":" << e.mDepthFullM << ",\"climb\":" << e.mClimbM << ",\"walk_head\":"
            << e.mWalkHeadM << ",\"walk_overlap\":" << e.mWalkOverlapM << ",\"carry_overlap\":" << e.mCarryOverlapM << ",\"carry_rise\":"
            << e.mCarryRiseM << ",\"sky_reach\":" << e.mSkyReachM << ",\"hysteresis\":" << e.mHysteresis << ",\"shelter_cover\":"
            << e.mShelterCover << ",\"shelter_open\":" << e.mShelterOpen << ",\"sealed_indoors\":" << (e.mSealedIndoors ? "true" : "false") << '}';
    }

    bool readEnclosureParams(const LLSD& v, U32, EnclosureParams& e)
    {
        if (!v.isMap())
        {
            return false;
        }
        e.mSkyDecayM = real(v, "sky_decay", e.mSkyDecayM);
        e.mDepthFullM = real(v, "depth_full", e.mDepthFullM);
        e.mClimbM = real(v, "climb", e.mClimbM);
        e.mWalkHeadM = real(v, "walk_head", e.mWalkHeadM);
        e.mWalkOverlapM = real(v, "walk_overlap", e.mWalkOverlapM);
        e.mCarryOverlapM = real(v, "carry_overlap", e.mCarryOverlapM);
        e.mCarryRiseM = real(v, "carry_rise", e.mCarryRiseM);
        e.mSkyReachM = real(v, "sky_reach", e.mSkyReachM);
        e.mHysteresis = real(v, "hysteresis", e.mHysteresis);
        e.mShelterCover = real(v, "shelter_cover", e.mShelterCover);
        e.mShelterOpen = real(v, "shelter_open", e.mShelterOpen);
        e.mSealedIndoors = v.has("sealed_indoors") ? v["sealed_indoors"].asBoolean() : e.mSealedIndoors;
        return true;
    }

    CensusSection enclosureParamsSection(EnclosureParams& params)
    {
        return { "rooms.enclosure", [&params](std::ostream& out) { writeEnclosureParams(out, params); },
                 [&params](const LLSD& v, U32 version) { return readEnclosureParams(v, version, params); } };
    }
}
