/**
 * @file sknavlevels.h
 * @brief Storeys of a structure and the stairs, ramps and drops between them.
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

#ifndef SK_NAVLEVELS_H
#define SK_NAVLEVELS_H

#include "sknavtypes.h"
#include "sknavwalls.h"

#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace sknav
{
    /// How a storey is left. Stairs and ramps are one kind: Recast merges a
    /// tread with the riser below it into a single sloped polygon, so the
    /// shape of the polygons does not say which is which. Telling them apart
    /// would mean reading the detail mesh's heights for a stepped profile,
    /// which is a lot of work for an icon.
    enum class ConnectorKind : U8 { CLIMB, DROP };

    /// One storey of one structure. Mesh-local metres.
    struct Level
    {
        F32 mZ = 0.f;               ///< the floor height this storey sits at
        LLVector3 mMin;
        LLVector3 mMax;
        U32 mPolys = 0;
        /// Too small a footprint to be a storey of its own: a landing, a
        /// doorstep, the treads of a staircase. Kept, because a connector is
        /// built out of exactly these.
        bool mTransitional = false;
    };

    /// A way from one storey to another. One-way for a drop.
    struct Connector
    {
        U16 mLow = 0;
        U16 mHigh = 0;
        LLVector3 mLowPoint;
        LLVector3 mHighPoint;
        ConnectorKind mKind = ConnectorKind::CLIMB;
        bool mOneWay = false;
    };

    struct LevelParams
    {
        /// A polygon that rises less than this is floor whatever its size, and
        /// only such a polygon can start a storey: a gentle ramp is walkable
        /// like a floor but is not one, and letting it seed would make a
        /// storey of every band's worth of its climb.
        F32 mStepTolerance = 0.3f;
        /// Rise over run above which a polygon is a way up rather than floor.
        /// Absolute rise alone will not do: where a floor meets the bottom
        /// step of a staircase Recast makes one broad polygon that rises the
        /// height of that step, and calling it a way up cuts the floor in two.
        F32 mFlatGradient = 0.2f;
        F32 mBand = 0.75f;          ///< how far in z a storey reaches from where it started
        /// Square metres of footprint below which a storey is only a landing.
        /// Counted as the bounding box, not the polygons: Recast merges a flat
        /// floor into a handful of them, so their number says nothing. A room
        /// four metres across is the smallest thing called a storey, which
        /// also catches the level patch a ramp leaves where its slope eases.
        F32 mMinArea = 16.f;
    };

    /// The storeys of one structure, and which storey each polygon is on.
    struct Levels
    {
        static const U16 NO_LEVEL = 0xffff;

        std::unordered_map<U64, U16> mPolyLevel;    ///< dtPolyRef -> level index
        std::vector<Level> mLevels;
        std::vector<Connector> mConnectors;

        U16 levelOf(U64 poly_ref) const
        {
            const auto it = mPolyLevel.find(poly_ref);
            return it == mPolyLevel.end() ? NO_LEVEL : it->second;
        }
    };

    /// Label every walkable polygon of the mesh with the storey it is on.
    ///
    /// A storey is the polygons you can walk between without changing height:
    /// flat polygons reached from a seed, none of them further than mBand in z
    /// from where that seed started. Banding rather than chaining is what keeps
    /// a staircase from welding two floors into one, since climbing it leaves
    /// the band after a step or two, and it is also why a spiral ramp cannot
    /// join a whole car park.
    ///
    /// What the staircase becomes is a connector: a run of sloped polygons, or
    /// of transitional ones too small to be a storey, that touches two storeys.
    /// Drop links make one-way connectors from the storey they leave to the one
    /// they land on. Read-only over the mesh; the query is used to place the
    /// links' ends.
    void buildLevels(const dtNavMesh& nav, const dtNavMeshQuery& query, const LevelParams& params,
                     const std::vector<DropLink>& links, Levels& out);
}

#endif // SK_NAVLEVELS_H
