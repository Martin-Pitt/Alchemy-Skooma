/**
 * @file sksurroundsrenderer.h
 * @brief The distant regions drawn into the G-buffer: the ring of regions next to the loaded ones, from the terrain
 * cache, through the live terrain's programs and with each region's own look, and the far bands beyond with map tiles.
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

#ifndef SK_SURROUNDSRENDERER_H
#define SK_SURROUNDSRENDERER_H

#include "llmatrix4a.h"
#include "llpointer.h"
#include "skterraincache.h"
#include "sksurroundsfield.h"

#include <memory>
#include <unordered_map>
#include <vector>

class LLImageGL;
class LLTerrainMaterials;
class LLGLSLShader;
class LLVertexBuffer;
class LLViewerFetchedTexture;
class LLViewerTexture;

// Owned by LLPipeline. Each main view selects the ring (SKSurrounds::Selection), asks the cache for its regions without
// waiting, builds a region's maps in ALTerrainSurfaceMaps' layout once per cached revision, and draws the regions whose
// look is ready as the live terrain draws a region: four-corner patches through the same programs and uniforms. A ring
// cell with no look to draw, and the far nodes out to TERRAIN_REACH under an infinite projection, draw their map tile,
// unlit, on heights decimated from the cache (gSKSurroundsRingProgram, gSKSurroundsFarProgram).
class SKSurroundsRenderer
{
public:
    SKSurroundsRenderer();
    ~SKSurroundsRenderer();

    // Selects, builds and draws the ring; the main view's G-buffer pass calls it after the opaque pools.
    void renderDeferred();
    // Lets every map and the patches' buffer go, as SKRenderSurrounds 0 or a lost context does.
    void release();

private:
    struct Cell
    {
        U64                                mHandle = 0;
        S32                                mX = 0;
        S32                                mY = 0;
        SKTerrainCache::region_ptr         mRegion; // what the maps were built from
        LLPointer<LLImageGL>               mHeight;
        LLPointer<LLImageGL>               mComposition;
        std::unique_ptr<LLTerrainMaterials> mLook;
        bool                               mPBR = false;
        bool                               mAlphaMask = false;
        U32                                mPaintType = 0;
        bool                               mDrawable = false; // built, with a look 8a can draw
        bool                               mHasHeights = false; // its height map is built
        bool                               mLookReady = false;  // its look drew this frame
        S8                                 mNeighbour[SKSurrounds::SIDE_COUNT] = {};
        F32                                mMinZ = 0.f;
        F32                                mMaxZ = 0.f;
        U32                                mSeen = 0;
        LLMatrix4a                         mModel;
    };

    // A far node: its heights at its level's decimation, which of its 8x8 patches have them, and what they were built from.
    struct FarNode
    {
        SKSurrounds::Node    mNode;
        LLPointer<LLImageGL> mHeight;
        U64                  mMask = 0;
        F32                  mMinZ = 0.f;
        F32                  mMaxZ = 0.f;
        size_t               mSignature = 0;
        bool                 mSigned = false; // mSignature has been taken
        bool                 mDirty = true;
        U32                  mSeen = 0;
        LLMatrix4a           mModel;
    };
    struct Tile
    {
        LLPointer<LLViewerFetchedTexture> mTexture;
        U32                               mSeen = 0;
    };

    void select();
    void selectFar(const std::vector<SKSurrounds::Node>& nodes);
    size_t farSignature(const FarNode& node);
    void buildFar(FarNode& node);
    void drawFar();
    void drawRingTiles();
    // The node's map tile, or its parent's quarter while it loads, as the tile and its uv transform; null if neither has landed.
    LLViewerFetchedTexture* tileFor(const SKSurrounds::Node& node, F32 uv[4]);
    LLViewerFetchedTexture* tile(S32 level, S32 x, S32 y);
    void drawTiled(LLGLSLShader* shader, LLImageGL* height, LLViewerFetchedTexture* tile, const F32 uv[4], const S32 edges[4],
                   U64 mask, F32 coarse_ratio, const LLVector3& origin_agent, F32 grid_scale, F32 width, LLVertexBuffer* patches,
                   U32 corners);
    bool build(Cell& cell);
    bool parseLook(Cell& cell);
    void draw();
    F32  heightAt(const Cell& cell, const SKTerrainPyramid& own, S32 gx, S32 gy);

    SKSurrounds::Selection          mSelection;
    std::vector<SKSurrounds::Cell>  mLive;
    std::vector<std::unique_ptr<Cell>> mCells;
    std::vector<std::unique_ptr<FarNode>> mFar;
    std::unordered_map<U64, Tile>   mTiles;
    size_t                          mFarCheck = 0; // where the round of signature checks has got to
    LLPointer<LLVertexBuffer>       mFarPatches[SKSurrounds::TOP_LEVEL + 1];
    U32                             mFrame = 0;
    LLPointer<LLVertexBuffer>       mPatches;
    LLPointer<LLViewerTexture>      mAlphaRamp;
    std::vector<F32>                mHeightStaging;
    std::vector<F32>                mCompositionStaging;
};

#endif // SK_SURROUNDSRENDERER_H
