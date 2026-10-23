/**
 * @file skmapplan.h
 * @brief Where someone stands in the rooms layer's air, for the mini-map and the overview.
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

#ifndef SK_MAPPLAN_H
#define SK_MAPPLAN_H

#include "llmath.h"
#include "v3dmath.h"
#include "lluuid.h"
#include "skrooms.h"

namespace skmap
{
namespace plan
{
    enum class Bucket : U8 { OUTDOORS, SHELTERED, INDOORS, UNKNOWN };

    /// A space by its mesh and id; mId 0 for none.
    struct SpaceRef
    {
        LLUUID mMesh;
        U32 mId = 0;
        bool operator==(const SpaceRef& o) const { return mMesh == o.mMesh && mId == o.mId; }
        bool operator!=(const SpaceRef& o) const { return !(*this == o); }
    };

    /// Where a global position is: the fine air gap holding it (SKWorldView::airGapAt) with its space, bucket,
    /// floor and building. Main thread.
    struct Whereabouts
    {
        bool mFound = false;
        SpaceRef mSpace;            ///< mId 0 for a gap in no space
        bool mIndoor = false;       ///< mBucket is INDOORS
        Bucket mBucket = Bucket::UNKNOWN; ///< the gap's bucket; never UNKNOWN when mFound
        F32 mFloor = 0.f;           ///< global
        F32 mCeiling = 1e30f;       ///< global; 1e30 under open sky
        bool mFine = false;         ///< read from detail air; always true when mFound
        U32 mBuilding = 0;          ///< the space's building, 0 in none
        bool mRoof = false;         ///< the space is a building's roof
    };
    /// Where someone standing with their feet at feet is: the air just over the feet, which must be fine, known
    /// and floored within band_below under to band_above over the feet, since a foot sunk into a roof reads the
    /// room under it; a gap holding the point is taken however far below its floor is, as in a jump. Not found otherwise: coarse air, an unloaded region or unsettled rooms give no guess.
    Whereabouts whereabouts(const LLVector3d& feet, F32 band_below = 1.0f, F32 band_above = 1.0f);
    /// whereabouts judged from a column already read at the feet's plan, in world.
    Whereabouts whereabouts(const SKWorldView& world, const SKWorldView::AirColumn& column, const LLVector3d& feet,
                            F32 band_below = 1.0f, F32 band_above = 1.0f);
    /// Whether the body standing at feet is in outdoor air: the first known gap 0.3, 0.8 or 1.3 m over the feet,
    /// as enclosureAt reads them, so a sunk foot does not read the room under it. UNKNOWN is never outdoors.
    bool outdoorsIn(const SKWorldView::AirColumn& column, const LLVector3d& feet);
    /// The floor under the agent standing at at with its feet at feet: a known fine air gap's floored at or just
    /// under the feet, else, off the floor in a jump or a fall, the covered fine gap's holding them. False with no
    /// answer. Main thread.
    bool agentFloor(const LLVector3d& at, F64 feet, F32& floor_out);
}
}

#endif // SK_MAPPLAN_H
