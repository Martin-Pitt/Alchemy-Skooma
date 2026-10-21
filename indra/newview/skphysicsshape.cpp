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
#include "skmeshhavokshape.h"

#include <algorithm>
#include <unordered_map>

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

namespace
{
    // Fitted to the server's physics weight (GetObjectCost) over 24k objects.
    // The server charges every analytic shape 0.1; the steps keep Havok's order visible on the ramp.
    constexpr F32 SPHERE_COST = 0.1f;
    constexpr F32 BOX_COST = 0.15f;
    constexpr F32 CYLINDER_COST = 0.2f;
    // The server charges a hull 0.04 a vertex plus one, and a decomposition the sum with no floor per hull.
    constexpr F32 HULL_VERTEX_COST = 0.04f;
    constexpr F32 HULL_MIN_COST = 0.2f;
    constexpr S32 HULL_UNKNOWN_VERTICES = 8;
    constexpr F32 PRIM_HULL_BASE_COST = 0.28f;
    constexpr F32 PRIM_HULL_VERTEX_COST = 0.01f;
    constexpr S32 PRIM_HULL_UNKNOWN_VERTICES = 8;
    constexpr F32 MESH_MIN_COST = 0.5f;

    F32 hullCost(S32 vertices)
    {
        return HULL_VERTEX_COST * (F32)(vertices + 1);
    }

    constexpr F32 MESH_TRIANGLE_COST = 0.05f;
    constexpr F32 MESH_MIN_WIDTH = 0.001f;
    constexpr F32 MESH_MAX_WIDTH = 20.f;

    // A triangle mesh, uploaded or a prim's, by the published formula (wiki Mesh/Mesh_physics):
    // 0.05 x triangles / their harmonic mean width, so a 0.5 m triangle costs 0.1. Narrow
    // triangles are real collision work. For prims the server instead charges thinness (about
    // smallest^-0.5 x largest^0.5, the same at any uniform scale); the heatmap keeps the width.
    // Sums one triangle into a mesh's count and inverse widths. A collapsed triangle is no
    // collision work: it is skipped rather than charged as the narrowest width.
    void addTriangle(const LLVector3& p0, const LLVector3& p1, const LLVector3& p2, const LLVector3& scale,
                     size_t& count, F32& inverse_widths)
    {
        const LLVector3 a = p0.scaledVec(scale);
        const LLVector3 b = p1.scaledVec(scale);
        const LLVector3 c = p2.scaledVec(scale);
        const F32 longest = llmax((b - a).length(), (c - b).length(), (a - c).length());
        // The triangle's height over its longest side: the narrow side of its enclosing rectangle.
        const F32 width = longest > 0.f ? ((b - a) % (c - a)).length() / longest : 0.f;
        if (width <= 0.f)
        {
            return;
        }
        ++count;
        inverse_widths += 1.f / width;
    }

    F32 meshCostFrom(size_t count, F32 inverse_widths)
    {
        if (count == 0)
        {
            return MESH_MIN_COST;
        }
        const F32 width = llclamp((F32)count / inverse_widths, MESH_MIN_WIDTH, MESH_MAX_WIDTH);
        return llmax(MESH_MIN_COST, MESH_TRIANGLE_COST * (F32)count / width);
    }

    F32 meshCost(const std::vector<LLVector3>& tris, const LLVector3& scale)
    {
        size_t count = 0;
        F32 inverse_widths = 0.f;
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
        {
            addTriangle(tris[i], tris[i + 1], tris[i + 2], scale, count, inverse_widths);
        }
        return meshCostFrom(count, inverse_widths);
    }

    F32 meshCost(const LLVolume* volume, const LLVector3& scale)
    {
        size_t count = 0;
        F32 inverse_widths = 0.f;
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& face = volume->getVolumeFace(f);
            if (!face.mPositions || !face.mIndices)
            {
                continue;
            }
            for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
            {
                addTriangle(LLVector3(face.mPositions[face.mIndices[i]].getF32ptr()),
                            LLVector3(face.mPositions[face.mIndices[i + 1]].getF32ptr()),
                            LLVector3(face.mPositions[face.mIndices[i + 2]].getF32ptr()), scale, count, inverse_widths);
            }
        }
        return meshCostFrom(count, inverse_widths);
    }

    // The overlay asks every frame; a triangle walk is redone only when its source or the scale changes.
    struct CachedMeshCost
    {
        const void* mSource = nullptr;
        size_t mSize = 0;
        LLVector3 mScale;
        F32 mCost = 0.f;
    };

    template <typename Compute>
    F32 cachedMeshCost(const LLUUID& id, const void* source, size_t size, const LLVector3& scale, Compute compute)
    {
        static std::unordered_map<LLUUID, CachedMeshCost> cache;
        CachedMeshCost& entry = cache[id];
        if (entry.mSource != source || entry.mSize != size || entry.mScale != scale)
        {
            entry.mSource = source;
            entry.mSize = size;
            entry.mScale = scale;
            entry.mCost = compute();
        }
        return entry.mCost;
    }

    F32 primHullCost(LLVOVolume* volume)
    {
        LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(volume->getVolume()->getParams(), physicsDetail(volume->getScale()));
        const S32 points = phys && phys->mHullPoints ? phys->mNumHullPoints : PRIM_HULL_UNKNOWN_VERTICES;
        LLPrimitive::sVolumeManager->unrefVolume(phys);
        return PRIM_HULL_BASE_COST + PRIM_HULL_VERTEX_COST * (F32)points;
    }

    F32 baseHullCost(const LLModel::Decomposition* decomp)
    {
        return hullCost(decomp && !decomp->mBaseHull.empty() ? (S32)decomp->mBaseHull.size() : HULL_UNKNOWN_VERTICES);
    }
}

F32 skPhysicsShapeCost(LLVOVolume* volume, const SKPhysicsShape& shape)
{
    const LLVector3 scale = volume->getScale();
    Spec spec = shape.mSpec;
    switch (spec.getType())
    {
        case Spec::SPHERE:
            return SPHERE_COST;
        case Spec::BOX:
            return BOX_COST;
        case Spec::CYLINDER:
            return CYLINDER_COST;
        case Spec::PRIM_MESH:
        {
            LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(volume->getVolume()->getParams(), physicsDetail(scale));
            F32 cost = MESH_MIN_COST;
            if (phys)
            {
                cost = cachedMeshCost(volume->getID(), phys, (size_t)phys->getNumVolumeFaces(), scale,
                                      [&] { return meshCost(phys, scale); });
            }
            LLPrimitive::sVolumeManager->unrefVolume(phys);
            return cost;
        }
        case Spec::USER_MESH:
        {
            const LLUUID& mesh_id = volume->getVolume()->getParams().getSculptID();
            const LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
            if (std::shared_ptr<const SKPhysicsHavokShape> sim = skPhysicsShapeSimTriangles(volume))
            {
                return cachedMeshCost(volume->getID(), sim.get(), sim->mTriangles.size(), scale,
                                      [&] { return meshCost(sim->mTriangles, scale); });
            }
            if (decomp && !decomp->mPhysicsShapeMesh.empty() && !skPhysicsMeshTooSmallForTriangles(volume))
            {
                const std::vector<LLVector3>& positions = decomp->mPhysicsShapeMesh.mPositions;
                return cachedMeshCost(volume->getID(), positions.data(), positions.size(), scale,
                                      [&] { return meshCost(positions, scale); });
            }
            if (decomp && !decomp->mHull.empty())
            {
                F32 cost = 0.f;
                for (const LLModel::hull& hull : decomp->mHull)
                {
                    cost += hullCost((S32)hull.size());
                }
                return llmax(HULL_MIN_COST, cost);
            }
            return baseHullCost(decomp);
        }
        default: // USER_CONVEX, PRIM_CONVEX, SCULPT
            if (volume->isMesh())
            {
                return baseHullCost(gMeshRepo.getDecomposition(volume->getVolume()->getParams().getSculptID()));
            }
            return primHullCost(volume);
    }
}

F32 skPhysicsShapeCostFraction(F32 cost)
{
    // Logarithmic over [0.1, 100] physics weight: analytic shapes at the bottom, hulls low, meshes above.
    return llclamp(log10f(llmax(cost, 0.1f) / 0.1f) / 3.f, 0.f, 1.f);
}

bool skPhysicsMeshTooSmallForTriangles(LLVOVolume* volume)
{
    // The server charges every such mesh a hull's cost, triangles or not.
    const LLVector3 scale = volume->getScale();
    return volume->isMesh() && llmin(scale[VX], scale[VY], scale[VZ]) < SHAPE_BUILDER_USER_MESH_CONVEXIFICATION_SIZE;
}

std::shared_ptr<const SKPhysicsHavokShape> skPhysicsShapeSimTriangles(LLVOVolume* volume)
{
    if (!volume || !volume->isMesh() || !volume->getVolume() || volume->getPhysicsShapeType() != LLViewerObject::PHYSICS_SHAPE_PRIM
        || skPhysicsMeshTooSmallForTriangles(volume))
    {
        return nullptr;
    }
    const LLUUID& mesh_id = volume->getVolume()->getParams().getSculptID();
    // Only where the physics mesh is the shape: with hulls, which the sim
    // collides with then is still open, so those keep the hull path.
    const LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
    if (gMeshRepo.physicsMeshSize(mesh_id) <= 0 || !decomp || !decomp->mHull.empty())
    {
        return nullptr;
    }
    std::shared_ptr<const SKPhysicsHavokShape> sim = gMeshRepo.getPhysicsHavok(mesh_id);
    if (!sim || !sim->mHasMopp || sim->mTriangles.size() < 3)
    {
        return nullptr;
    }
    return sim;
}

bool skPhysicsShapeGeometry(LLVOVolume* volume, const SKPhysicsShape& shape, SKShapeGeometry& out,
                            const std::shared_ptr<const SKPhysicsHavokShape>& sim)
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
                if (physics_mesh > 0 && !skPhysicsMeshTooSmallForTriangles(volume))
                {
                    gMeshRepo.fetchPhysicsShape(params.getSculptID());
                    // What the simulator built from the physics mesh is what it collides with.
                    if (sim && decomp && decomp->mHull.empty())
                    {
                        appendSoup(sim->mTriangles, out.mTriangles);
                        out.mProvenance = sknav::Provenance::SIM;
                        out.mSolid = false;
                        break;
                    }
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
            // The fixed-detail volume is shared and only sculpted by whatever
            // draws at that detail, so it can hold a lower-resolution texture
            // or none; a partial sculpt warps, so fall back to the loaded one.
            LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(params, physicsDetail(scale));
            out.mProvenance = sknav::Provenance::RENDER;
            if (phys->getSculptLevel() == 0)
            {
                appendFaces(phys, out.mTriangles);
            }
            else if (volume->getVolume()->getSculptLevel() == 0)
            {
                appendFaces(volume->getVolume(), out.mTriangles);
            }
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
