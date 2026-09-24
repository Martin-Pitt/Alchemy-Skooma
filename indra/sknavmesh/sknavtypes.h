/**
 * @file sknavtypes.h
 * @brief Shared types of the client-side navmesh: census records, roles, the tile lattice.
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

#ifndef SK_NAVTYPES_H
#define SK_NAVTYPES_H

#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace sknav
{
    /// The global lattice every mesh and every hash grid is aligned to, in metres.
    constexpr F64 TILE_M = 32.0;
    /// Recast cell size, metres, both horizontal and vertical.
    constexpr F32 CELL_M = 0.125f;

    /// What a census record is for.
    enum class Role : U8
    {
        NAV,        ///< a declared physics shape: floor and obstacle for every class
        SURROUND,   ///< a sim surround: its rendered shape, WEATHER class only
        TERRAIN,    ///< generated terrain beyond the region edge (later)
    };

    /// How exact the record's geometry is.
    enum class Provenance : U8
    {
        EXACT,      ///< the physics shape itself (analytic, physics mesh, prim volume)
        HULL,       ///< a convex decomposition hull
        BOX,        ///< a placeholder while the asset is still fetching
        RENDER,     ///< the rendered volume faces (surrounds)
        BASE_HULL,  ///< the single base hull, when there is no decomposition
    };

    /// The sim's navmesh role for a linkset, when known.
    enum class NavRole : U8
    {
        UNKNOWN,
        WALKABLE,
        STATIC_OBSTACLE,
        MATERIAL_VOLUME,
        EXCLUSION_VOLUME,
        DYNAMIC,        ///< a mover or a character: never in a cluster
    };

    /// What a part was recognised as by the feature rules (sknavfeatures.h).
    enum class FeatureKind : U8
    {
        NONE,
        DOOR,       ///< a leaf of a door
        LADDER,     ///< part of a ladder's span
        ELEVATOR,   ///< an elevator's car; a feature of this kind is its shaft
        VEHICLE,    ///< part of a linkset people sit on that moves
        SEAT,       ///< part of a linkset people sit on that never moves
    };

    /// The classes a record is left out of, one bit per AgentClass index.
    constexpr U8 EXCLUDE_AVATAR = 1 << 0;
    constexpr U8 EXCLUDE_WEATHER = 1 << 1;

    /// Analytic shape or triangle soup. Soups are region-local metres.
    enum class Shape : U8 { BOX, SPHERE, CYLINDER, TRIANGLES };

    /// One part of one linkset as the navmesh sees it. Positions are region-local
    /// (metres from the owning region's origin); mOrigin carries that origin so a
    /// consumer can re-base to any lattice tile without losing precision.
    struct Record
    {
        LLUUID mRoot;
        LLUUID mPart;
        Role mRole = Role::NAV;
        Provenance mProvenance = Provenance::EXACT;
        NavRole mNavRole = NavRole::UNKNOWN;
        Shape mShape = Shape::TRIANGLES;
        bool mDynamic = false;
        bool mSolid = false;        ///< a closed body the sim collides with as a volume: the build fills its inside
        FeatureKind mFeature = FeatureKind::NONE;
        U8 mExclude = 0;            ///< EXCLUDE_* bits: classes that build as if it were not there

        LLVector3d mOrigin;         ///< region origin, global
        LLVector3 mCenter;          ///< analytic shapes: centre, region-local
        LLVector3 mAxes[3];         ///< analytic shapes: unit axes in region space
        LLVector3 mHalf;            ///< analytic shapes: half extents along mAxes
        std::shared_ptr<const std::vector<LLVector3>> mTriangles; ///< TRIANGLES: 3 per triangle, region-local
        std::vector<S32> mConvexTris;       ///< TRIANGLES as separate convex bodies: triangle count of each, in order

        LLVector3d mMin;            ///< global AABB
        LLVector3d mMax;

        U64 mSignature = 0;         ///< what shaped the geometry; equal signature, equal geometry
    };

    typedef std::shared_ptr<const Record> RecordPtr;

    /// A lattice tile, by its index on the global 32 m grid.
    struct TileKey
    {
        S32 mX = 0;
        S32 mY = 0;
        bool operator==(const TileKey& o) const { return mX == o.mX && mY == o.mY; }
        bool operator<(const TileKey& o) const { return mX < o.mX || (mX == o.mX && mY < o.mY); }
    };

    inline S32 tileIndex(F64 metres)
    {
        return static_cast<S32>(std::floor(metres / TILE_M));
    }

    inline TileKey tileOf(const LLVector3d& p)
    {
        return { tileIndex(p.mdV[VX]), tileIndex(p.mdV[VY]) };
    }

    /// The tile's minimum corner, global.
    inline LLVector3d tileOrigin(const TileKey& k)
    {
        return LLVector3d(k.mX * TILE_M, k.mY * TILE_M, 0.0);
    }

    struct TileKeyHash
    {
        size_t operator()(const TileKey& k) const
        {
            return (static_cast<size_t>(static_cast<U32>(k.mX)) * 73856093u) ^ (static_cast<size_t>(static_cast<U32>(k.mY)) * 19349663u);
        }
    };

    /// 64-bit FNV-1a, the signature mixer everywhere in the navmesh.
    inline U64 mix(U64 h, U64 v)
    {
        h ^= v;
        h *= 1099511628211ull;
        return h;
    }
    constexpr U64 SEED = 14695981039346656037ull;

    inline U64 mixF32(U64 h, F32 f)
    {
        U32 bits;
        static_assert(sizeof(bits) == sizeof(f), "float width");
        std::memcpy(&bits, &f, sizeof(bits));
        return mix(h, bits);
    }

    inline U64 mixF64(U64 h, F64 f)
    {
        U64 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return mix(h, bits);
    }
}

#endif // SK_NAVTYPES_H
