/**
 * @file skcensusrecord.h
 * @brief A census record's geometry and what keys it: tessellation, signatures, content keys.
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


#ifndef SK_CENSUSRECORD_H
#define SK_CENSUSRECORD_H

#include "skcensustypes.h"

#include <vector>

namespace skworld
{
    /// The tail the census gives a placeholder part made from a region's object cache, named by its local id ('SKNV').
    constexpr U32 PLACEHOLDER_TAG = 0x534b4e56;

    /// Positions and extents are keyed to the census's own movement epsilon,
    /// so a part that did not move by its measure keys the same.
    constexpr F32 KEY_POSITION_M = 0.01f;
    /// Unit axes, and anything else dimensionless.
    constexpr F32 KEY_UNIT = 1e-3f;

    /// splitmix64's finaliser. FNV only carries a change upwards, so a key whose last input moved in its top bits
    /// would keep its low bits, and hashes summed over a set would cancel in equal pairs.
    U64 finalise(U64 h);

    /// What a set of records is signed by, in whatever order they come: a sum, so two equal parts do not cancel.
    U64 recordSignature(const std::vector<RecordPtr>& records);

    /// What a record is built from, and nothing that lives only for a session:
    /// its part id (unless it is a placeholder keyed by a local id), role,
    /// flags, shape, region origin, quantised transform, bounds and triangles.
    U64 recordContentKey(const Record& record);

    /// Tessellate an analytic record into triangles (mesh-local, given the
    /// mesh origin), or copy its soup.
    void emitRecord(const Record& record, const LLVector3d& mesh_origin, std::vector<LLVector3>& out);
}

#endif // SK_CENSUSRECORD_H
