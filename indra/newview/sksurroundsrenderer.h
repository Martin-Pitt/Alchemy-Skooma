/**
 * @file sksurroundsrenderer.h
 * @brief The distant regions drawn into the G-buffer: the ring of regions next to the loaded ones, from the terrain
 * cache, through the live terrain's programs and with each region's own look.
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
#include <vector>

class LLImageGL;
class LLTerrainMaterials;
class LLVertexBuffer;
class LLViewerTexture;

// Owned by LLPipeline. Each main view selects the ring (SKSurrounds::Selection), asks the cache for its regions without
// waiting, builds a region's maps in ALTerrainSurfaceMaps' layout once per cached revision, and draws the regions whose
// look is ready as the live terrain draws a region: four-corner patches through the same programs and uniforms.
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
        F32                                mMinZ = 0.f;
        F32                                mMaxZ = 0.f;
        U32                                mSeen = 0;
        LLMatrix4a                         mModel;
    };

    void select();
    bool build(Cell& cell);
    bool parseLook(Cell& cell);
    void draw();
    F32  heightAt(const Cell& cell, const SKTerrainPyramid& own, S32 gx, S32 gy);

    SKSurrounds::Selection          mSelection;
    std::vector<SKSurrounds::Cell>  mLive;
    std::vector<std::unique_ptr<Cell>> mCells;
    U32                             mFrame = 0;
    LLPointer<LLVertexBuffer>       mPatches;
    LLPointer<LLViewerTexture>      mAlphaRamp;
    std::vector<F32>                mHeightStaging;
    std::vector<F32>                mCompositionStaging;
};

#endif // SK_SURROUNDSRENDERER_H
