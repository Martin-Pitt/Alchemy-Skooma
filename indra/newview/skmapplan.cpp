/**
 * @file skmapplan.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skmapplan.h"

using namespace skmap::plan;

namespace
{
    Bucket bucketOf(skworld::EnclosureBucket b)
    {
        switch (b)
        {
            case skworld::EnclosureBucket::OUTDOORS: return Bucket::OUTDOORS;
            case skworld::EnclosureBucket::SHELTERED: return Bucket::SHELTERED;
            case skworld::EnclosureBucket::INDOORS: return Bucket::INDOORS;
            default: return Bucket::UNKNOWN;
        }
    }

    // Whether the gap holds height z, rather than standing in for it from below.
    bool holds(const skworld::Enclosure& gap, F64 z)
    {
        return gap.mFloor <= z && z <= gap.mCeiling;
    }

    // airGapAt, but its stand-in from a space below only within a metre of the feet: further down it is another
    // storey's, and the gap holding the point answers instead.
    bool airGapNear(const SKWorldView::AirColumn& column, F64 feet, F64 z, SKWorldView::SpaceId& space, skworld::Enclosure& gap)
    {
        if (!column.airGapAt(z, space, gap))
        {
            return false;
        }
        if (gap.mFloor < feet - 1.0)
        {
            LLUUID mesh;
            if (!column.enclosureAt(z, gap, &mesh))
            {
                return false;
            }
            space.mMesh = mesh;
            space.mId = gap.mSpaceId;
        }
        return true;
    }

}

skmap::plan::Whereabouts skmap::plan::whereabouts(const LLVector3d& feet, F32 band_below, F32 band_above)
{
    const SKWorldViewPtr world = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
    if (!world)
    {
        return {};
    }
    SKWorldView::AirColumn column;
    world->columnAt(feet, column);
    return whereabouts(*world, column, feet, band_below, band_above);
}

skmap::plan::Whereabouts skmap::plan::whereabouts(const SKWorldView& world, const SKWorldView::AirColumn& column,
                                                  const LLVector3d& feet, F32 band_below, F32 band_above)
{
    const F64 LIFT_M = 0.3;
    Whereabouts out;
    SKWorldView::SpaceId id;
    skworld::Enclosure gap;
    // The band holds even here: airGapAt can answer from the gap below. A gap holding the point is where the body
    // is however far its floor lies under the feet, so a jump keeps its room.
    const F64 z = feet.mdV[VZ] + LIFT_M;
    if (!airGapNear(column, feet.mdV[VZ], z, id, gap) || gap.mBucket == skworld::EnclosureBucket::UNKNOWN || !gap.mFine ||
        (gap.mFloor < feet.mdV[VZ] - band_below && !holds(gap, z)) || gap.mFloor > feet.mdV[VZ] + band_above)
    {
        return out;
    }
    out.mFound = true;
    out.mSpace.mMesh = id.mMesh;
    out.mSpace.mId = id.mId;
    out.mBucket = bucketOf(gap.mBucket);
    out.mIndoor = out.mBucket == Bucket::INDOORS;
    out.mFloor = gap.mFloor;
    out.mCeiling = gap.mCeiling;
    out.mFine = true;
    if (const skworld::Space* space = id.mId ? world.space(id) : nullptr)
    {
        out.mBuilding = space->mBuilding;
        out.mRoof = space->mRoof;
    }
    return out;
}

bool skmap::plan::outdoorsIn(const SKWorldView::AirColumn& column, const LLVector3d& feet)
{
    for (F64 lift : { 0.3, 0.8, 1.3 })
    {
        skworld::Enclosure gap;
        if (column.enclosureAt(feet.mdV[VZ] + lift, gap) && gap.mBucket != skworld::EnclosureBucket::UNKNOWN)
        {
            return gap.mBucket == skworld::EnclosureBucket::OUTDOORS;
        }
    }
    return false;
}

bool skmap::plan::agentFloor(const LLVector3d& at, F64 feet, F32& floor_out)
{
    const SKWorldViewPtr world = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
    SKWorldView::SpaceId space;
    skworld::Enclosure gap;
    SKWorldView::AirColumn column;
    if (world)
    {
        world->columnAt(at, column);
    }
    if (!world || !airGapNear(column, feet, feet + 0.3, space, gap) ||
        gap.mBucket == skworld::EnclosureBucket::UNKNOWN || !gap.mFine || gap.mFloor > feet + 0.1)
    {
        return false;
    }
    // Off the floor, in a jump or a fall, only a covered gap holding the feet still knows the floor under them.
    const bool standing = gap.mFloor >= feet - 0.75;
    const bool airborne = gap.mBucket != skworld::EnclosureBucket::OUTDOORS && holds(gap, feet + 0.3);
    if (!standing && !airborne)
    {
        return false;
    }
    floor_out = (F32)gap.mFloor;
    return true;
}
