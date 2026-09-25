/**
 * @file skphysicsshape.h
 * @brief What a volume's physics shape is, decided once for the debug renderer and the navmesh census.
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

#ifndef SK_PHYSICSSHAPE_H
#define SK_PHYSICSSHAPE_H

#include "llphysicsshapebuilderutil.h"
#include "sknavtypes.h"

#include <vector>

class LLVOVolume;

/// The decision: the object's physics type and the shape specification the
/// physics engine would build from it.
struct SKPhysicsShape
{
    U8 mPhysicsType = 0;
    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification mSpec;
};

/// False when the volume has no physics shape at all (type NONE, or flexible).
/// Otherwise fills the decision. This is the only place that decision is made,
/// so the physics-shape overlay and the census cannot disagree.
bool skPhysicsShapeClassify(LLVOVolume* volume, SKPhysicsShape& out);

/// Geometry in the volume's own frame, unscaled: apply scale, rotation and
/// position to get region space. Analytic shapes carry a centre and half
/// extents (in that same unscaled frame, so scale applies to them too);
/// TRIANGLES carry three vertices per triangle.
struct SKShapeGeometry
{
    sknav::Shape mShape = sknav::Shape::TRIANGLES;
    sknav::Provenance mProvenance = sknav::Provenance::EXACT;
    LLVector3 mCenter;
    LLVector3 mHalf;
    std::vector<LLVector3> mTriangles;
    /// mTriangles as several separate convex bodies end to end (a
    /// decomposition): the triangle count of each, in order. The build fills
    /// a closed body to make it solid, and the union of a door frame's sill,
    /// posts and lintel is not a body - filled as one it walls off the
    /// opening they exist to leave. Empty means one body.
    std::vector<S32> mConvexTris;
    /// The geometry closes a body the sim collides with as a volume - a prim,
    /// a hull, a sculpt's hull - rather than a surface it collides with from
    /// either side, which is what a mesh's physics mesh is. The build fills
    /// a solid's inside so no floor is offered there.
    bool mSolid = false;
};

/// The physics geometry for a classified shape. Asks the mesh repository for
/// what is missing exactly as the overlay does. False while it is missing and
/// when there is nothing to emit: nothing stands in for a fetch.
bool skPhysicsShapeGeometry(LLVOVolume* volume, const SKPhysicsShape& shape, SKShapeGeometry& out);

/// The rendered geometry: the volume's faces at whatever LOD is loaded, with
/// provenance RENDER. What sim surrounds contribute to the weather class.
bool skRenderShapeGeometry(LLVOVolume* volume, SKShapeGeometry& out);

#endif // SK_PHYSICSSHAPE_H
