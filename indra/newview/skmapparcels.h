/**
 * @file skmapparcels.h
 * @brief The mini-map's parcel lines and for-sale and collision fills, as vertex arrays.
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

#ifndef SK_MAPPARCELS_H
#define SK_MAPPARCELS_H

#include "v3dmath.h"
#include "v4coloru.h"

#include <boost/signals2.hpp>
#include <vector>

namespace skmap
{
    class MapView;
}

/// Rebuilt when a region's parcel overlay or the collision bitmap changes, or a region arrives,
/// leaves or dies; never per frame. Nothing is drawn for damage: no parcel is tinted.
class SKMapParcels
{
public:
    SKMapParcels();
    ~SKMapParcels();

    void draw(const skmap::MapView& view);

private:
    void rebuild();
    /// The region set and each region's liveness: a change rebuilds, whatever the overlay says.
    static U64 regionsKey();

    struct Line
    {
        LLVector3d mA, mB;          ///< global
        LLColor4U mColour;
    };
    struct Fill
    {
        LLVector3d mMin, mMax;      ///< global
        LLColor4U mColour;
    };
    std::vector<Line> mLines;
    std::vector<Fill> mFills;
    bool mDirty = true;
    U64 mRegionsKey = 0;
    bool mLastSale = false;
    bool mLastCollision = false;
    boost::signals2::connection mOverlayConnection;
    boost::signals2::connection mCollisionConnection;
};

#endif // SK_MAPPARCELS_H
