/**
 * @file sknavstamp.h
 * @brief A tile's walkable spans stamped with the spaces they stand in, so its polygons never cross from one space into another.
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

#ifndef SK_NAVSTAMP_H
#define SK_NAVSTAMP_H

#include "sknavairdistil.h"
#include "sknavflights.h"

#include <vector>

struct rcCompactHeightfield;
struct rcPolyMesh;

namespace sknav
{
    struct TileEnclosure;
    struct RoomsProduct;

    /// A span or polygon in no space.
    constexpr U16 STAMP_NONE = 0xffff;

    /// A portal as a tile's stamp reads it: its plane and ends, mesh-local,
    /// and the tile's components on its two sides.
    struct StampPortal
    {
        LLVector3 mCentre;
        LLVector3 mNormal;          ///< horizontal, from side 0 into side 1
        LLVector3 mA;
        LLVector3 mB;
        U16 mSide[2] = { STAMP_NONE, STAMP_NONE };
        U8 mKind = 0;               ///< PortalKind
        U32 mPortal = 0;            ///< index into the rooms product's portals; binding only
    };

    /// What a tile's stamp is cut from, read off its air and the spaces
    /// product. A component is a space as this tile sees it, numbered in the
    /// order its gaps first appear, so the numbering follows the shape of the
    /// partition and not the spaces' ids: a rename leaves it as it was.
    struct TileStampInput
    {
        std::vector<U16> mGapComponent;     ///< per gap of the air, STAMP_NONE for no space
        std::vector<U32> mComponentSpace;   ///< per component, its space's index in the product; binding only
        std::vector<StampPortal> mPortals;  ///< portals within reach of the tile
        /// Everything the stamp reads but the binding: equal patterns stamp
        /// equal spans, so the same polygons, over the same raster.
        U64 mPattern = 0;
    };

    /// How far from a portal a span with no gap of its own looks for one.
    constexpr F32 STAMP_PORTAL_REACH_M = 1.f;
    /// How far either side of a door's plane its threshold runs: the leaf's floor and the jambs' depth.
    constexpr F32 STAMP_THRESHOLD_M = 0.5f;

    /// The stamp input of one tile: tile_min and tile_max bound it, mesh-local.
    void stampInput(const TileAir& air, const TileEnclosure& enclosure, const RoomsProduct& rooms, const LLVector3& tile_min,
                    const LLVector3& tile_max, TileStampInput& out);

    struct StampParams
    {
        S32 mPaletteMax = 63;       ///< area codes a palette may use: 1 up to this
    };

    /// One code of a palette: the slope partition's code and the colour it is stamped with.
    struct StampCode
    {
        U8 mSlope = 0;
        U8 mColour = 0;
    };

    /// A tile's walkable spans, each with its component and the palette code
    /// that goes into the compact heightfield's area in place of its slope
    /// code. Components that touch get different colours, so a region, which
    /// grows within one area, never holds two of them.
    struct TileStamp
    {
        enum Overflow : U8 { FIT = 0, CODES_MERGED = 1, SPACES_FUSED = 2, ONE_COLOUR = 3 };

        std::vector<U16> mSpanComponent;    ///< per span of the compact heightfield
        std::vector<U8> mSpanCode;          ///< per span, the palette code; 0 for no floor
        std::vector<U8> mComponentColour;   ///< per component, 1 up; colour 0 is no space and the border
        std::vector<StampCode> mPalette;    ///< indexed by code; entry 0 unused
        U8 mOverflow = FIT;                 ///< how far down the cascade the palette had to go
        U32 mDoorSpans = 0;                 ///< spans with no gap that took the side of a portal's plane
        U32 mFloodSpans = 0;                ///< spans with no gap and no portal near that took a neighbour's component
        U32 mUnspacedSpans = 0;             ///< walkable spans of the tile in no space
        U32 mFused = 0;                     ///< components fused into a neighbour's colour
        U32 mPairs = 0;                     ///< palette codes used
    };

    /// Stamp a raster's walkable spans. span_gap is the raster's mSpanGap;
    /// real and flights tell a slope code's area and which codes are flights,
    /// which the cascade never merges.
    void stampTile(const rcCompactHeightfield& chf, S32 border, const std::vector<U32>& span_gap, const TileStampInput& input,
                   const std::vector<U8>& real, const std::vector<Flight>& flights, const StampParams& params, TileStamp& out);

    /// Detour poly flags past the area bits: where a poly is and what it touches.
    enum StampFlag : U16
    {
        POLY_INDOORS = 1 << 10,
        POLY_SHELTERED = 1 << 11,
        POLY_OUTDOORS = 1 << 12,
        POLY_FLIGHT = 1 << 13,
        POLY_DOOR = 1 << 14,
        POLY_BINDING_FLAGS = POLY_INDOORS | POLY_SHELTERED | POLY_OUTDOORS | POLY_DOOR,
    };

    /// A poly edge on a portal: the poly, its edge, the portal's side the poly is on.
    struct StampPortalEdge
    {
        U16 mPoly = 0;
        U8 mEdge = 0;
        U8 mSide = 0;
        U16 mPortal = 0;            ///< into the stamp's portals
    };

    /// What the polygon stage made of a stamp, by component rather than by
    /// space, so it caches under the raster's key and the pattern and binds
    /// to whatever spaces stand there when it is published.
    struct TileStampGeom
    {
        bool mStamped = false;
        U64 mPattern = 0;
        std::vector<U16> mPolyComponent;    ///< per poly, STAMP_NONE for none
        std::vector<U32> mPolyGap;          ///< per poly, the air gap most of its spans stand in, or SPAN_NO_GAP
        std::vector<StampPortalEdge> mPortalEdges;
        std::vector<StampPortal> mPortals;  ///< the stamp input's, sides by component
        U32 mComponents = 0;
        U8 mOverflow = 0;
        U32 mPairs = 0;
        U32 mDoorSpans = 0;
        U32 mFloodSpans = 0;
        U32 mUnspacedSpans = 0;
        U32 mImpurePolys = 0;               ///< polys whose spans hold more than one component; 0 unless the cascade fused
        U32 mJoinedPolys = 0;               ///< polys in no component that took a walkably joined neighbour's

        size_t bytes() const;
    };

    /// Fill a geometry's per-poly tables from the poly mesh, the compact
    /// heightfield it came from (regions still on it) and the stamp.
    void stampGeometry(const rcPolyMesh& pmesh, const rcCompactHeightfield& chf, S32 border, const std::vector<U32>& span_gap,
                       const TileStampInput& input, const TileStamp& stamp, TileStampGeom& out);

    /// A portal's identity across passes: its two spaces' ids, kind and centre to 0.25 m.
    U64 portalKey(U32 space_a, U32 space_b, U8 kind, const LLVector3& centre);

    /// A geometry bound to the spaces standing now: each poly's space id and
    /// the flags past its area bits, and each portal edge's portal key.
    constexpr U32 NO_PORTAL = 0xffffffffu;

    struct TileBinding
    {
        U64 mHash = 0;
        std::vector<U32> mComponentSpaceId;     ///< 0 for a component whose space is gone
        std::vector<U16> mPolyFlags;            ///< POLY_BINDING_FLAGS bits per poly
        std::vector<U64> mPortalKeys;           ///< per portal edge
        std::vector<U32> mPortalIndex;          ///< per portal edge, its portal's index in the rooms product, or NO_PORTAL
    };

    /// Bind a geometry to the input it was stamped with, read again against
    /// the current spaces, and the air's enclosure for each poly's bucket.
    void bindStamp(const TileStampGeom& geom, const TileStampInput& input, const TileEnclosure* enclosure, const RoomsProduct& rooms,
                   TileBinding& out);
}

#endif // SK_NAVSTAMP_H
