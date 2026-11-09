/**
 * @file skoverviewplace.h
 * @brief Where someone stands in the rooms layer's air, for the overview capture's cut.
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

#ifndef SK_OVERVIEWPLACE_H
#define SK_OVERVIEWPLACE_H

#include "llmath.h"
#include "v3dmath.h"
#include "lluuid.h"
#include "skrooms.h"

namespace skoverview
{
namespace place
{
    enum class Bucket : U8 { OUTDOORS, SHELTERED, INDOORS, UNKNOWN };

    /// Where a global position is: the fine air gap holding it (SKWorldView::airGapAt) with its bucket and floor.
    /// Main thread.
    struct Whereabouts
    {
        bool mFound = false;
        bool mIndoor = false;       ///< mBucket is INDOORS
        Bucket mBucket = Bucket::UNKNOWN; ///< the gap's bucket; never UNKNOWN when mFound
        F32 mFloor = 0.f;           ///< global
        F32 mCeiling = 1e30f;       ///< global; 1e30 under open sky
    };
    /// Where someone standing with their feet at feet is: the air just over the feet, which must be fine, known
    /// and floored within band_below under to band_above over the feet, since a foot sunk into a roof reads the
    /// room under it; a gap holding the point is taken however far below its floor is, as in a jump. Not found
    /// otherwise: coarse air, an unloaded region or unsettled rooms give no guess.
    Whereabouts whereabouts(const LLVector3d& feet, F32 band_below = 1.0f, F32 band_above = 1.0f);
    /// whereabouts judged from a column already read at the feet's plan.
    Whereabouts whereabouts(const SKWorldView::AirColumn& column, const LLVector3d& feet, F32 band_below = 1.0f,
                            F32 band_above = 1.0f);
    /// Whether the body standing at feet is in outdoor air: the first known gap 0.3, 0.8 or 1.3 m over the feet,
    /// as enclosureAt reads them, so a sunk foot does not read the room under it. UNKNOWN is never outdoors.
    bool outdoorsIn(const SKWorldView::AirColumn& column, const LLVector3d& feet);
    /// The floor under the agent standing at at with its feet at feet: a known fine air gap's floored at or just
    /// under the feet, else, off the floor in a jump or a fall, the covered fine gap's holding them. False with no
    /// answer. Main thread.
    bool agentFloor(const LLVector3d& at, F64 feet, F32& floor_out);
}
}

#endif // SK_OVERVIEWPLACE_H
