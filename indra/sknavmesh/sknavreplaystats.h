/**
 * @file sknavreplaystats.h
 * @brief A measured replay written out: per-stage times, sizes, how polygons fall across spaces, pathing and edits.
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

#ifndef SK_NAVREPLAYSTATS_H
#define SK_NAVREPLAYSTATS_H

#include "sknavreplay.h"

#include <iosfwd>
#include <vector>

namespace sknav
{
    /// One prim edit: a record taken out, moved 0.5 m along x, or copied 3 m along y.
    struct ReplayEdit
    {
        enum Op : U8 { REMOVE = 0, SHIFT = 1, COPY = 2 };
        U32 mRecord = 0;        ///< index into the census's records
        Op mOp = REMOVE;
    };

    /// The replay's measurements as JSON: every tile's stages and sizes, where
    /// its AVATAR spans and polys fall among the spaces, each mesh's rooms
    /// passes, pathing and air lookups, and the 1 m air of the tiles past the
    /// air radius. The replay must have run with ReplayOptions::mStats.
    void writeReplayStats(std::ostream& out, const CensusFile& census, const ReplayResult& result);

    /// What each edit reaches in the tiles holding its record, and each such tile rebuilt with it.
    void writeReplayEdits(std::ostream& out, const CensusFile& census, const ReplayResult& result, const std::vector<ReplayEdit>& edits);
}

#endif // SK_NAVREPLAYSTATS_H
