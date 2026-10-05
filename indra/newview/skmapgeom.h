/**
 * @file skmapgeom.h
 * @brief The mini-map's scene: navmesh polygons stitched into surfaces, components and layers.
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

#ifndef SK_MAPGEOM_H
#define SK_MAPGEOM_H

#include "skmapview.h"
#include "sknavpolys.h"

#include "sknavenclosure.h"
#include "sknavflights.h"
#include "sknavtypes.h"
#include "sknavwalls.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace skmap
{
    constexpr F32 RUNLESS_Z_SPAN_M = 4.f;       ///< a polygon outside a run spanning more is dropped
    constexpr F32 LATTICE_M = 32.f;
    constexpr F32 LATTICE_TOLERANCE_M = 0.02f;
    constexpr F32 STITCH_OVERLAP_M = 0.05f;
    constexpr F32 LAYER_GAP_M = 1.8f;           ///< surfaces closer than this in median z may share a layer
    constexpr F32 INDEX_CELL_M = 4.f;
    constexpr F32 LINK_REACH_M = 1.f;           ///< a ladder or elevator end this close to a polygon joins it
    constexpr F32 STOREY_REACH_M = 5.f;
    constexpr F32 INSIDE_ABOVE_M = 1.f;
    constexpr S32 INTERIOR_CAP_CELLS = 64;
    constexpr F32 FLOOR_SAME_M = 2.f;
    constexpr F32 FLOOR_RAW_M = 2.5f;

    /// One published navmesh tile, as gathered on the main thread.
    struct TileInput
    {
        LLUUID mMesh;
        bool mTerrain = false;
        U64 mRegion = 0;
        LLVector3d mOrigin;                     ///< mesh origin, global
        sknav::TileKey mKey;
        U64 mContentKey = 0;
        LLVector3d mMin, mMax;                  ///< footprint, global
        F32 mWater = -1.0e4f;                   ///< the region's water height, global
        std::shared_ptr<const sknav::TilePolys> mPolys;
        std::shared_ptr<const std::vector<sknav::WallSegment>> mSegments;
        std::shared_ptr<const std::vector<sknav::DropLink>> mLinks;
        std::shared_ptr<const std::vector<sknav::Flight>> mFlights;
    };

    /// A ladder or elevator link between two exits, global.
    struct FeatureLink
    {
        LLVector3d mFrom;
        LLVector3d mTo;
        sknav::LinkKind mKind = sknav::LinkKind::LADDER;
    };

    /// A door as the census found it, global.
    struct DoorInput
    {
        LLVector3d mMin, mMax;
        LLVector3d mPlanePoint;
        bool mPlaneKnown = true;
        LLVector3 mNormal;
        F32 mWidth = 0.f;
        F64 mSill = 0.0;
        bool mDouble = false;
    };

    struct RegionSquare
    {
        LLVector3d mOrigin;                     ///< south-west corner, global
        F64 mWidth = 256.0;
    };

    typedef std::function<sknav::EnclosureBucket(const LLVector3d&)> CoverageFn;

    /// Everything one scene build reads. Immutable once posted to the worker.
    struct SceneInput
    {
        LLVector3d mOrigin;                     ///< the analysis box's minimum corner, z 0: scene-local metres start here
        LLVector3d mMin, mMax;                  ///< the analysis box, global
        std::vector<TileInput> mTiles;
        std::vector<FeatureLink> mFeatureLinks;
        std::vector<DoorInput> mDoors;
        std::vector<RegionSquare> mRegions;
        F32 mClimb = 0.75f;
        CoverageFn mCoverage;                   ///< null reads as UNKNOWN everywhere
    };

    /// One navmesh polygon in scene-local metres.
    struct ScenePoly
    {
        U32 mTile = 0;                          ///< into SceneInput::mTiles
        U16 mIndex = 0;                         ///< within its tile
        U8 mArea = 0;
        U8 mVertCount = 0;
        bool mTerrainTile = false;
        U8 mCoverage = static_cast<U8>(sknav::EnclosureBucket::UNKNOWN);
        LLVector3 mVerts[sknav::TilePolys::MAX_VERTS];
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        F32 mZMean = 0.f;
        LLVector2 mCentroid;
        F32 mPlanArea = 0.f;
        F32 mMinX = 0.f, mMinY = 0.f, mMaxX = 0.f, mMaxY = 0.f;
        U32 mFirstTri = 0;                      ///< into Scene::mTris, three corners each
        U16 mTriCount = 0;
        F32 mWater = -1.0e4f;
        S32 mRun = -1;
        S32 mSurface = -1;
        S32 mLayer = -1;
    };

    /// A stair or ramp, one per (mesh, flight key), scene-local.
    struct SceneRun
    {
        LLUUID mMesh;
        U64 mKey = 0;
        bool mStair = false;
        U8 mTreads = 0;
        LLVector3 mBottomA, mBottomB, mTopA, mTopB;
        F32 mRun = 0.f;
        F32 mRise = 0.f;
        std::vector<U32> mPolys;
        S32 mBottomSurface = -1;
        S32 mTopSurface = -1;
        S32 mLayer = -1;
        S32 mTopLayer = -1;
    };

    struct SceneSurface
    {
        std::vector<U32> mPolys;
        bool mTerrain = false;
        S32 mComponent = -1;
        S32 mLayer = -1;
        F32 mMedianZ = 0.f;
        F32 mArea = 0.f;
    };

    struct SceneLayer
    {
        bool mGround = false;
        S32 mComponent = -1;
        F32 mBaseZ = 0.f;                       ///< area-weighted median z of its polygons
        F32 mArea = 0.f;
        std::vector<U32> mPolys;
        std::vector<S32> mRuns;
        std::set<std::pair<S32, S32>> mFootprint;   ///< global 4 m cells its polygons meet
    };

    /// A polygon's edge joined to another's, inside a tile or across a lattice line.
    struct SceneStitch
    {
        U32 mA = 0;
        U32 mB = 0;
        bool mStep = false;                     ///< more than a climb apart at an end of the overlap
        F32 mZ = 0.f;                           ///< the joint's height
    };

    struct Scene
    {
        LLVector3d mOrigin;
        LLVector3d mMin, mMax;
        F32 mClimb = 0.75f;
        std::vector<ScenePoly> mPolys;
        std::vector<LLVector3> mTris;
        std::vector<SceneRun> mRuns;
        std::vector<SceneSurface> mSurfaces;
        std::vector<SceneStitch> mStitches;
        std::vector<SceneLayer> mLayers;
        std::vector<S32> mOrder;                ///< layers in draw order
        std::vector<std::pair<S32, S32>> mLayerLinks;   ///< layers joined by a run or a ladder or elevator
        S32 mComponents = 0;
        /// Global 4 m cell -> the polygons that meet it; each polygon knows its layer.
        std::map<std::pair<S32, S32>, std::vector<U32>> mIndex;

        LLVector2 toLocal(const LLVector3d& global) const
        {
            return LLVector2(static_cast<F32>(global.mdV[VX] - mOrigin.mdV[VX]), static_cast<F32>(global.mdV[VY] - mOrigin.mdV[VY]));
        }
    };

    typedef std::shared_ptr<const Scene> ScenePtr;

    /// Analyse the input into a scene. Pure and deterministic: the same input gives the same scene.
    ScenePtr buildScene(const SceneInput& input);

    /// The global 4 m cell holding a global position.
    std::pair<S32, S32> indexCell(F64 x, F64 y);
    /// Whether a scene polygon's plan contains a scene-local point.
    bool polyContains(const Scene& scene, const ScenePoly& poly, const LLVector2& p);
    /// The polygon's height at a scene-local point, from its detail triangles.
    F32 polyHeightAt(const Scene& scene, const ScenePoly& poly, const LLVector2& p);

    /// The floor at a global position: the highest polygon over its plan point within the window.
    struct FloorHit
    {
        bool mFound = false;
        S32 mLayer = -1;
        S32 mPoly = -1;
        F32 mZ = 0.f;
    };
    FloorHit floorAt(const Scene& scene, const LLVector3d& pos, F32 below, F32 above);

    /// Where the agent stands, as storey selection and floor relations read it.
    struct AgentFloor
    {
        LLVector3d mPos;                        ///< raw position, global
        bool mFound = false;                    ///< a scene floor (or the navmesh's) was found
        S32 mLayer = -1;                        ///< -1 when the floor came from outside the scene
        F32 mZ = 0.f;
    };

    enum class FloorRelation : U8 { SAME, ABOVE, BELOW, UNKNOWN };
    /// Another avatar's floor against the agent's. z_known is false for a coarse avatar.
    FloorRelation floorRelation(const Scene& scene, const AgentFloor& agent, const LLVector3d& other, bool z_known);

    /// The layers to draw and the cells cut out of them.
    struct LayerSelection
    {
        bool mInside = false;
        std::vector<S32> mDrawn;                ///< in draw order
        std::map<S32, std::set<std::pair<S32, S32>>> mCut;  ///< per layer, global 4 m cells left out
        std::set<S32> mStorey;                  ///< the set S of an inside selection
    };
    /// Storey selection: everything within the vertical fade outdoors; inside, the floors over
    /// the interior that are not on the agent's storey are cut.
    LayerSelection selectLayers(const Scene& scene, const AgentFloor& agent, sknav::EnclosureBucket bucket, F32 fade);
}

#endif // SK_MAPGEOM_H
