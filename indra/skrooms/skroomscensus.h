/**
 * @file skroomscensus.h
 * @brief The rooms layer's section of the census file: the enclosure parameters the answers were built with.
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


#ifndef SK_ROOMSCENSUS_H
#define SK_ROOMSCENSUS_H

#include "skcensusio.h"
#include "skroomsenclosure.h"

#include <iosfwd>

class LLSD;

namespace skworld
{
    /// The "rooms.enclosure" section, written from and read into params. A file older than version 7 hands over
    /// its top-level "enclosure" object, which has the same shape.
    CensusSection enclosureParamsSection(EnclosureParams& params);
    void writeEnclosureParams(std::ostream& out, const EnclosureParams& params);
    bool readEnclosureParams(const LLSD& value, U32 version, EnclosureParams& params);
}

#endif // SK_ROOMSCENSUS_H
