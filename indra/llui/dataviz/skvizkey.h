/**
 * @file skvizkey.h
 * @brief What a hovered legend row, ramp value, chart point or world mark stands for.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_VIZKEY_H
#define SK_VIZKEY_H

#include "llsd.h"
#include "v4color.h"

#include <cmath>
#include <string>

// Data, never pointers: a scale id plus whichever of class, value and thing it names.
struct SKVizKey
{
    static constexpr U32 NO_CATEGORY = ~0u;

    std::string scale;                   // SKColorScale::id or a chart series
    U32         category = NO_CATEGORY;  // class, bin or row index
    F64         value    = std::nan(""); // a value on the scale
    U64         id       = 0;            // the thing in the world, 0 for none
    std::string state;                   // a named state row, "" for none
    LLColor4    colour;                  // optional, alpha 0 for none
    bool        hasColour = false;

    bool empty() const { return scale.empty() && category == NO_CATEGORY && std::isnan(value) && id == 0 && state.empty(); }
    bool hasCategory() const { return category != NO_CATEGORY; }
    bool hasValue() const { return !std::isnan(value); }

    // Same scale and the same class or state: how a row is matched, never by colour.
    bool matchesRow(const SKVizKey& other) const
    {
        if (scale != other.scale)
        {
            return false;
        }
        if (hasCategory() || other.hasCategory())
        {
            return category == other.category;
        }
        return !state.empty() && state == other.state;
    }

    bool operator==(const SKVizKey& o) const
    {
        const bool values = (std::isnan(value) && std::isnan(o.value)) || value == o.value;
        return scale == o.scale && category == o.category && values && id == o.id && state == o.state && hasColour == o.hasColour &&
               (!hasColour || colour == o.colour);
    }
    bool operator!=(const SKVizKey& o) const { return !(*this == o); }

    LLSD toLLSD() const
    {
        LLSD sd = LLSD::emptyMap();
        sd["scale"] = scale;
        if (hasCategory())
        {
            sd["category"] = (LLSD::Integer)category;
        }
        if (hasValue())
        {
            sd["value"] = value;
        }
        if (id)
        {
            sd["id"] = std::to_string(id);
        }
        if (!state.empty())
        {
            sd["state"] = state;
        }
        if (hasColour)
        {
            sd["colour"] = colour.getValue();
        }
        return sd;
    }

    static SKVizKey fromLLSD(const LLSD& sd)
    {
        SKVizKey k;
        k.scale = sd["scale"].asString();
        if (sd.has("category"))
        {
            k.category = (U32)sd["category"].asInteger();
        }
        if (sd.has("value"))
        {
            k.value = sd["value"].asReal();
        }
        if (sd.has("id"))
        {
            k.id = std::strtoull(sd["id"].asString().c_str(), nullptr, 10);
        }
        k.state = sd["state"].asString();
        if (sd.has("colour"))
        {
            k.colour.setValue(sd["colour"]);
            k.hasColour = true;
        }
        return k;
    }
};

#endif // SK_VIZKEY_H
