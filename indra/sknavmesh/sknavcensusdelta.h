/**
 * @file sknavcensusdelta.h
 * @brief What a census scan holds, as one hash, and what changed between two scans, by part.
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

#ifndef SK_NAVCENSUSDELTA_H
#define SK_NAVCENSUSDELTA_H

#include "sknavcluster.h"
#include "sknavfeatures.h"
#include "sknavtypes.h"

#include <unordered_map>
#include <vector>

namespace sknav
{
    /// The splitmix64 finaliser, so hashes summed over a set do not cancel in equal pairs.
    U64 finalise(U64 h);

    /// A record's content: its part, its signature and whether it is dynamic.
    U64 recordHash(const Record& rec);

    /// What a scan holds, blind to order: every record (the dynamic too), every feature and door, and the window and
    /// gap its clusters are made with. Equal hashes, equal census.
    U64 censusHash(const std::vector<RecordPtr>& records, const std::vector<Feature>& features,
                   const std::vector<DoorEntity>& doors, const ClusterWindow& window, F64 gap);

    typedef std::unordered_map<LLUUID, RecordPtr> RecordsByPart;
    RecordsByPart recordsByPart(const std::vector<RecordPtr>& records);

    /// The parts that came, went and changed (the same part with another signature or dynamic flag) from before to
    /// after, each list in part order; a changed part is given as it is after.
    void diffRecords(const RecordsByPart& before, const RecordsByPart& after, std::vector<RecordPtr>& added,
                     std::vector<RecordPtr>& removed, std::vector<RecordPtr>& changed);
}

#endif // SK_NAVCENSUSDELTA_H
