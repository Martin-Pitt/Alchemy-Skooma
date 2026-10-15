/**
 * @file skoverviewcensus.h
 * @brief The census's physical shapes as vertex buffers, a range per map class, for the overview.
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

#ifndef SK_OVERVIEWCENSUS_H
#define SK_OVERVIEWCENSUS_H

#include "llpointer.h"
#include "v3dmath.h"
#include "v3math.h"

#include <memory>
#include <vector>

class LLVertexBuffer;

namespace sknav
{
    struct Record;
}

namespace skoverview
{
    /// The class code each overview layer writes into the capture's R channel; overviewResolveF.glsl reads the same.
    enum Code : U8
    {
        CODE_NONE = 0,
        CODE_TERRAIN = 255,     ///< the region's land: its shadow program writes white
        CODE_GHOST = 2,         ///< rendered geometry, front face on top
        CODE_GHOST_SECTION = 3, ///< rendered geometry the cut went through
        CODE_FLOOR = 16,        ///< census: walkable slope, facing up or down
        CODE_WALL = 17,         ///< census: steep faces of parts taller than cover
        CODE_COVER = 18,        ///< census: steep faces of parts no taller than COVER_TOP_M
        CODE_RAMP = 19,         ///< census: between floor and wall
        CODE_DOOR = 20,
        CODE_LADDER = 21,
        CODE_VEHICLE = 22,
        CODE_SECTION = 23,      ///< census: a solid the cut went through
    };

    /// The census classes, in vertex buffer order.
    enum CensusClass : U8 { FLOOR, WALL, COVER, RAMP, DOOR, LADDER, VEHICLE, INVISIBLE, NUM_CENSUS_CLASSES };

    constexpr F32 FLOOR_NZ = 0.866f;    ///< steeper than 30 degrees is no longer floor
    constexpr F32 WALL_NZ = 0.5f;       ///< steeper than 60 degrees is wall
    constexpr F32 COVER_TOP_M = 1.6f;   ///< a part no taller than this is cover, not wall
    constexpr F32 INVISIBLE_WALL_M = 64.f; ///< a fully transparent part larger than this on any axis is no wall

    U8 codeOf(CensusClass cls);

    /// Whether the overview draws a record at all, and the class of its parts that is not per triangle (door, ladder,
    /// vehicle); NUM_CENSUS_CLASSES when each triangle is classed by its slope.
    bool keepRecord(const sknav::Record& rec, CensusClass& fixed);

    /// A triangle's class from its unit normal's z and its part's height.
    CensusClass classifyTriangle(F32 nz, F32 part_height);
}

/// The census's shapes uploaded once per snapshot that changes them: one vertex buffer per region, its triangles
/// grouped by class. Main thread only.
class SKOverviewCensus
{
public:
    struct Batch
    {
        LLVector3d mOrigin;     ///< region origin, global; vertices are region-local
        LLPointer<LLVertexBuffer> mBuffer;
        U32 mStart[skoverview::NUM_CENSUS_CLASSES + 1] = {}; ///< vertex ranges per class
    };

    SKOverviewCensus();
    ~SKOverviewCensus();

    /// Rebuilds the buffers when the census's geometry changed; true when there is anything to draw.
    bool update();
    const std::vector<Batch>& batches() const { return mBatches; }
    U32 triangles() const { return mTriangles; }
    void release();

private:
    std::vector<Batch> mBatches;
    U64 mStamp = 0;
    U64 mSignature = 0;
    std::shared_ptr<const struct SKCensusSnapshot> mSnapshot;
    U32 mTriangles = 0;
};

#endif // SK_OVERVIEWCENSUS_H
