/**
 * @file skphysicsshape.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skphysicsshape.h"

#include "llmeshrepository.h"
#include "llmodel.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"

namespace
{
    typedef LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification Spec;

    // The physics tessellation detail the overlay uses (llspatialpartition.cpp
    // get_physics_detail): only the object's size matters.
    S32 physicsDetail(const LLVector3& scale)
    {
        const F32 avg = (scale[0] + scale[1] + scale[2]) / 3.f;
        S32 detail = 1;
        if (avg > 5.f)
        {
            detail += 1;
            if (avg > 25.f)
            {
                detail += 1;
            }
        }
        return detail;
    }

    void appendFaces(const LLVolume* volume, std::vector<LLVector3>& out)
    {
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& face = volume->getVolumeFace(f);
            if (!face.mPositions || !face.mIndices)
            {
                continue;
            }
            out.reserve(out.size() + face.mNumIndices);
            for (S32 i = 0; i < face.mNumIndices; ++i)
            {
                out.emplace_back(face.mPositions[face.mIndices[i]].getF32ptr());
            }
        }
    }

    void appendHull(const LLVolume* volume, std::vector<LLVector3>& out)
    {
        out.reserve(out.size() + volume->mNumHullIndices);
        for (S32 i = 0; i < volume->mNumHullIndices; ++i)
        {
            out.emplace_back(volume->mHullPoints[volume->mHullIndices[i]].getF32ptr());
        }
    }

    void appendSoup(const std::vector<LLVector3>& positions, std::vector<LLVector3>& out)
    {
        // A PhysicsMesh is three positions per triangle already.
        out.insert(out.end(), positions.begin(), positions.end() - (positions.size() % 3));
    }

    void boundingBox(const LLVOVolume* volume, SKShapeGeometry& out)
    {
        out.mShape = sknav::Shape::BOX;
        out.mProvenance = sknav::Provenance::BOX;
        out.mCenter.setZero();
        out.mHalf.set(0.5f, 0.5f, 0.5f);
    }
}

bool skPhysicsShapeClassify(LLVOVolume* volume, SKPhysicsShape& out)
{
    out.mPhysicsType = volume->getPhysicsShapeType();
    if (out.mPhysicsType == LLViewerObject::PHYSICS_SHAPE_NONE || volume->isFlexible())
    {
        return false;
    }
    const LLPhysicsVolumeParams physics_params(volume->getVolume()->getParams(),
                                               out.mPhysicsType == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
    LLPhysicsShapeBuilderUtil::determinePhysicsShape(physics_params, volume->getScale(), out.mSpec);
    return true;
}

bool skPhysicsShapeGeometry(LLVOVolume* volume, const SKPhysicsShape& shape, SKShapeGeometry& out)
{
    out.mTriangles.clear();
    out.mShape = sknav::Shape::TRIANGLES;
    out.mProvenance = sknav::Provenance::EXACT;

    Spec spec = shape.mSpec;
    const LLVolumeParams& params = volume->getVolume()->getParams();
    const LLVector3 scale = volume->getScale();

    switch (spec.getType())
    {
        case Spec::USER_MESH:
        {
            LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(params.getSculptID());
            if (!decomp)
            {
                boundingBox(volume, out);
                return true;
            }
            if (!decomp->mHull.empty())
            {
                if (decomp->mMesh.empty())
                {
                    gMeshRepo.buildPhysicsMesh(*decomp);
                }
                out.mProvenance = sknav::Provenance::HULL;
                for (const LLModel::PhysicsMesh& hull : decomp->mMesh)
                {
                    appendSoup(hull.mPositions, out.mTriangles);
                }
            }
            else if (!decomp->mPhysicsShapeMesh.empty())
            {
                appendSoup(decomp->mPhysicsShapeMesh.mPositions, out.mTriangles);
            }
            else
            {
                if (decomp->mBaseHullMesh.empty())
                {
                    gMeshRepo.buildPhysicsMesh(*decomp);
                }
                gMeshRepo.fetchPhysicsShape(params.getSculptID());
                if (decomp->mBaseHullMesh.empty())
                {
                    boundingBox(volume, out);
                    return true;
                }
                out.mProvenance = sknav::Provenance::HULL;
                appendSoup(decomp->mBaseHullMesh.mPositions, out.mTriangles);
            }
            break;
        }
        case Spec::USER_CONVEX:
        case Spec::PRIM_CONVEX:
        {
            if (volume->isMesh())
            {
                LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(params.getSculptID());
                if (!decomp || (decomp->mBaseHullMesh.empty() && (gMeshRepo.buildPhysicsMesh(*decomp), decomp->mBaseHullMesh.empty())))
                {
                    boundingBox(volume, out);
                    return true;
                }
                out.mProvenance = sknav::Provenance::HULL;
                appendSoup(decomp->mBaseHullMesh.mPositions, out.mTriangles);
            }
            else
            {
                // The overlay builds a single hull lazily and caches it on the
                // volume; take it when it is there, and the faces meanwhile.
                LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(params, physicsDetail(scale));
                if (phys->mHullPoints && phys->mHullIndices)
                {
                    out.mProvenance = sknav::Provenance::HULL;
                    appendHull(phys, out.mTriangles);
                }
                else
                {
                    appendFaces(phys, out.mTriangles);
                }
                LLPrimitive::sVolumeManager->unrefVolume(phys);
            }
            break;
        }
        case Spec::BOX:
        {
            // The spec's scale is the world-size box; back it into the unscaled
            // frame as the overlay does before drawing under the object transform.
            out.mShape = sknav::Shape::BOX;
            out.mCenter = spec.getCenter();
            const LLVector3 world = spec.getScale();
            out.mHalf.set(world[0] / (2.f * scale[0]), world[1] / (2.f * scale[1]), world[2] / (2.f * scale[2]));
            break;
        }
        case Spec::SPHERE:
        case Spec::CYLINDER:
        {
            out.mShape = spec.getType() == Spec::SPHERE ? sknav::Shape::SPHERE : sknav::Shape::CYLINDER;
            out.mCenter.setZero();
            out.mHalf.set(0.5f, 0.5f, 0.5f);
            break;
        }
        case Spec::PRIM_MESH:
        {
            LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(params, physicsDetail(scale));
            appendFaces(phys, out.mTriangles);
            LLPrimitive::sVolumeManager->unrefVolume(phys);
            break;
        }
        case Spec::SCULPT:
        {
            // The overlay draws nothing for a classic sculpt; the physics engine
            // uses its convex hull. The rendered faces are the closest thing we have.
            out.mProvenance = sknav::Provenance::RENDER;
            appendFaces(volume->getVolume(), out.mTriangles);
            break;
        }
        default:
            return false;
    }
    return out.mShape != sknav::Shape::TRIANGLES || !out.mTriangles.empty();
}

bool skRenderShapeGeometry(LLVOVolume* volume, SKShapeGeometry& out)
{
    out.mTriangles.clear();
    out.mShape = sknav::Shape::TRIANGLES;
    out.mProvenance = sknav::Provenance::RENDER;
    const LLVolume* v = volume->getVolume();
    if (!v)
    {
        return false;
    }
    appendFaces(v, out.mTriangles);
    return !out.mTriangles.empty();
}
