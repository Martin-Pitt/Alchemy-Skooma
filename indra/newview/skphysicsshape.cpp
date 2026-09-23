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

#include <algorithm>

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

    void appendSoup(const std::vector<LLVector3>& positions, std::vector<LLVector3>& out)
    {
        // A PhysicsMesh is three positions per triangle already.
        out.insert(out.end(), positions.begin(), positions.end() - (positions.size() % 3));
    }
}

bool skPhysicsShapeClassify(LLVOVolume* volume, SKPhysicsShape& out)
{
    // The type is a per-prim setting: only a child can be none, and a root
    // that should not collide is made phantom instead. A part's own type is
    // what decides its shape. The sim answers the linkset's request rather
    // than a child's own, though, so ask the root as well - a child whose
    // type has not arrived reads as prim, which is how a hologram child of a
    // prim linkset became a solid.
    volume->getRootEdit()->getPhysicsShapeType();
    out.mPhysicsType = volume->getPhysicsShapeType();
    // A volume arrives before its full update gives it an LLVolume; nothing to say yet.
    if (out.mPhysicsType == LLViewerObject::PHYSICS_SHAPE_NONE || volume->isFlexible() || !volume->getVolume())
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
    // Every shape below is a body the sim collides with as a volume, except a
    // mesh's own physics mesh, which is a surface it collides with from
    // either side.
    out.mSolid = true;

    Spec spec = shape.mSpec;
    if (!volume->getVolume())
    {
        return false;
    }
    const LLVolumeParams& params = volume->getVolume()->getParams();
    const LLVector3 scale = volume->getScale();

    switch (spec.getType())
    {
        case Spec::USER_MESH:
        {
            LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(params.getSculptID());
            // A Prim mesh collides as the physics mesh it was uploaded with
            // when the asset has one, and as its hull decomposition when it
            // has none. Nothing stands in for the wait: a hull or a box the
            // sim never collided with is a floor and a wall it never had, and
            // the census asks again next pass, so the fetch costs a few
            // passes of silence and no rebuild. The rendered shape is not an
            // option either - a mesh LOD can be hundreds of thousands of
            // triangles, and only a megasculpty's surround is ever meshed
            // from what the eye sees.
            if (shape.mPhysicsType == LLViewerObject::PHYSICS_SHAPE_PRIM)
            {
                const S32 physics_mesh = gMeshRepo.physicsMeshSize(params.getSculptID());
                if (physics_mesh < 0)
                {
                    return false;   // header on its way: which shape it collides as is unknown
                }
                if (physics_mesh > 0)
                {
                    gMeshRepo.fetchPhysicsShape(params.getSculptID());
                    if (!decomp || decomp->mPhysicsShapeMesh.empty())
                    {
                        return false;   // the physics mesh is on its way
                    }
                    appendSoup(decomp->mPhysicsShapeMesh.mPositions, out.mTriangles);
                    out.mSolid = false;
                    break;
                }
            }
            if (!decomp)
            {
                return false;   // the decomposition is on its way
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
                    const size_t before = out.mTriangles.size();
                    appendSoup(hull.mPositions, out.mTriangles);
                    out.mConvexTris.push_back(static_cast<S32>((out.mTriangles.size() - before) / 3));
                }
                if (out.mTriangles.empty())
                {
                    // The hull list held nothing buildable - a point cloud
                    // with no area at all, which the sim cannot collide with
                    // either. The asset has arrived and has nothing to say, so
                    // say nothing: the object's box is no stand-in for a large
                    // sparse mesh, whose box top is a floor in mid-air.
                    return false;
                }
            }
            else
            {
                if (decomp->mBaseHullMesh.empty())
                {
                    gMeshRepo.buildPhysicsMesh(*decomp);
                }
                if (decomp->mBaseHullMesh.empty())
                {
                    return false;
                }
                // The single hull of a shape with no decomposition.
                out.mProvenance = sknav::Provenance::BASE_HULL;
                appendSoup(decomp->mBaseHullMesh.mPositions, out.mTriangles);
                if (out.mTriangles.empty())
                {
                    return false;
                }
            }
            break;
        }
        case Spec::USER_CONVEX:
        case Spec::PRIM_CONVEX:
        {
            if (volume->isMesh())
            {
                LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(params.getSculptID());
                if (!decomp)
                {
                    return false;   // the hull is on its way; nothing stands in
                }
                if (decomp->mBaseHullMesh.empty() || decomp->mMesh.empty())
                {
                    gMeshRepo.buildPhysicsMesh(*decomp);
                }
                // The hull is the shape, and the sim has none where the asset
                // stores none: a convex-hull mesh with no hull is walked
                // straight through, so no box stand-in - it would be a floor
                // in mid-air over whatever the object draws.
                if (decomp->mBaseHullMesh.empty())
                {
                    return false;
                }
                out.mProvenance = sknav::Provenance::HULL;
                appendSoup(decomp->mBaseHullMesh.mPositions, out.mTriangles);
                if (out.mTriangles.empty())
                {
                    return false;
                }
            }
            else
            {
                // The physics-detail volume, not the loaded one: the faces sit
                // inside the single hull the overlay draws, and taking them at
                // a fixed detail is what keeps the shape - and so the tiles
                // built from it - from moving with the camera.
                LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(params, physicsDetail(scale));
                appendFaces(phys, out.mTriangles);
                LLPrimitive::sVolumeManager->unrefVolume(phys);
            }
            break;
        }
        case Spec::BOX:
        case Spec::SPHERE:
        case Spec::CYLINDER:
        {
            // The spec's centre and size are in world units (a cut cylinder is
            // shorter than its object and sits off-centre); back both into the
            // unscaled frame so the object transform puts them right.
            out.mShape = spec.getType() == Spec::BOX ? sknav::Shape::BOX
                       : spec.getType() == Spec::SPHERE ? sknav::Shape::SPHERE : sknav::Shape::CYLINDER;
            const LLVector3 centre = spec.getCenter();
            const LLVector3 size = spec.getScale();
            for (S32 i = 0; i < 3; ++i)
            {
                const F32 s = std::max(scale[i], 1e-4f);
                out.mCenter.mV[i] = centre[i] / s;
                out.mHalf.mV[i] = size[i] / (2.f * s);
            }
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
            // The sim collides with the sculpt's convex hull, which the viewer
            // never builds; the faces at a fixed detail are the closest stable
            // stand-in. The loaded volume would make the shape follow the
            // camera, rebuilding every tile the sculpt reaches.
            LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(params, physicsDetail(scale));
            out.mProvenance = sknav::Provenance::RENDER;
            appendFaces(phys, out.mTriangles);
            LLPrimitive::sVolumeManager->unrefVolume(phys);
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
    out.mSolid = false;
    const LLVolume* v = volume->getVolume();
    if (!v)
    {
        return false;
    }
    appendFaces(v, out.mTriangles);
    return !out.mTriangles.empty();
}
