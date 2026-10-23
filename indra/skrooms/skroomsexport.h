/**
 * @file skroomsexport.h
 * @brief The rooms layer's export JSON: the enclosure per tile, and a mesh's spaces, portals and wind.
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


#ifndef SK_ROOMSEXPORT_H
#define SK_ROOMSEXPORT_H

#include "skroomsenclosure.h"
#include "skroomsspaces.h"
#include "skroomswind.h"

#include <iosfwd>

namespace skworld
{
    /// A tile's per-gap answers as a JSON member, leading comma included.
    void writeExportEnclosure(std::ostream& file, U64 generation, const TileEnclosure& tile);
    /// A mesh's spaces, portals and wind slabs as JSON members, leading comma included, global.
    void writeExportSpaces(std::ostream& file, const LLVector3d& origin, const RoomsProduct& rooms, const WindProduct* wind);
}

#endif // SK_ROOMSEXPORT_H
