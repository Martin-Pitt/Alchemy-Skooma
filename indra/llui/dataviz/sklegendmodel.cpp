/**
 * @file sklegendmodel.cpp
 * @brief What one legend says: ramps, keys, states, fields, channels and readouts.
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

#include "linden_common.h"

#include "sklegendmodel.h"

#include "skticks.h"

SKLegendSection SKLegendSection::key(const SKColorScale& s, std::string heading)
{
    SKLegendSection sec;
    sec.type    = Type::Key;
    sec.heading = std::move(heading);
    for (S32 i = 0; i < (S32)s.stops().size(); ++i)
    {
        SKLegendRow row;
        row.marks.push_back(SKLegendMark::swatch(s.classColour(i)));
        row.label = s.className(i);
        if (s.kind() == SKColorScale::Kind::Binned && i + 1 < (S32)s.edges().size())
        {
            const std::string range = SKTicks::format(s.edges()[i], 0) + " - " + SKTicks::format(s.edges()[i + 1], 0);
            row.label               = row.label.empty() ? range : row.label;
            row.value               = row.label == range ? std::string() : range;
            if (!s.unit().empty())
            {
                (row.value.empty() ? row.label : row.value) += " " + s.unit();
            }
        }
        row.key.scale    = s.id();
        row.key.category = (U32)i;
        sec.rows.push_back(std::move(row));
    }
    return sec;
}
