/**
 * @file sksurroundsrenderer.cpp
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

#include "llviewerprecompiledheaders.h"

#include "sksurroundsrenderer.h"

#include "alfarplane.h"
#include "alterrainsurfacemaps.h"
#include "llagent.h"
#include "lldrawpool.h"
#include "lldrawpoolterrain.h"
#include "llenvironment.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmateriallist.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llsdserialize.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexturelist.h"
#include "llvlcomposition.h"
#include "llworld.h"
#include "pipeline.h"

namespace
{
    constexpr S32 CELL_METRES     = SKSurrounds::REGION_METRES;
    constexpr S32 GRIDS_PER_EDGE  = CELL_METRES + 1; // the east and north buffer, as LLSurface keeps it
    constexpr S32 PATCH_METRES    = SKSurrounds::LIVE_PATCH_METRES;
    constexpr S32 PATCHES_PER_EDGE = CELL_METRES / PATCH_METRES;
    constexpr U32 PATCH_CORNERS   = 4;
    constexpr S32 BUILDS_PER_FRAME = 1;

    LLVector3d originGlobal(S32 x, S32 y) { return LLVector3d((F64)x * CELL_METRES, (F64)y * CELL_METRES, 0.0); }

    // A region's terrain at its own lattice point, clamped inside it.
    F32 latticeAt(const SKTerrainPyramid& pyramid, S32 x, S32 y)
    {
        const SKTerrainPyramid::Level& base = pyramid.mLevels[0];
        const S32 last = (S32)base.mSize - 1;
        return base.mHeight[(size_t)llclamp(y, 0, last) * base.mSize + llclamp(x, 0, last)];
    }

    const SKTerrainPyramid* terrainOf(const SKTerrainCache::region_ptr& region)
    {
        const SKTerrainPyramid* pyramid = region ? region->mPyramids[SKTerrain::LAYER_TERRAIN].get() : nullptr;
        return pyramid && pyramid->hasData() && pyramid->mLevels[0].mSize == (U32)CELL_METRES ? pyramid : nullptr;
    }

    void upload(LLPointer<LLImageGL>& image, S32 internal_format, U32 primary_format, U8 components, const std::vector<F32>& data)
    {
        const S32 size = GRIDS_PER_EDGE + 2 * ALTerrainSurfaceMaps::APRON;
        image = new LLImageGL(size, size, components, /*usemipmaps=*/false);
        image->setExplicitFormat(internal_format, primary_format, GL_FLOAT);
        image->createGLTexture(0, reinterpret_cast<const U8*>(data.data()));
    }
} // namespace

SKSurroundsRenderer::SKSurroundsRenderer() = default;

SKSurroundsRenderer::~SKSurroundsRenderer() = default;

void SKSurroundsRenderer::release()
{
    mCells.clear();
    mPatches = nullptr;
    mAlphaRamp = nullptr;
    mHeightStaging = {};
    mCompositionStaging = {};
}

void SKSurroundsRenderer::renderDeferred()
{
    static LLCachedControl<U32> surrounds(gSavedSettings, "SKRenderSurrounds", 1);
    if (surrounds == 0)
    {
        if (!mCells.empty() || mPatches.notNull())
        {
            release();
        }
        return;
    }
    ++mFrame;
    {
        LL_PROFILE_ZONE_NAMED("surrounds select");
        select();
    }
    {
        LL_PROFILE_ZONE_NAMED("surrounds draw");
        {
            LL_PROFILE_GPU_ZONE("surrounds");
            draw();
        }
    }
}

void SKSurroundsRenderer::select()
{
    static LLCachedControl<U32> near_cells(gSavedSettings, "SKRenderSurroundsNearCells", 1);

    mLive.clear();
    for (const LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (regionp && regionp->getWidth() == (F32)CELL_METRES)
        {
            const U64 handle = regionp->getHandle();
            mLive.push_back({ (S32)SKTerrain::gridX(handle), (S32)SKTerrain::gridY(handle) });
        }
    }
    if (mLive.empty())
    {
        mCells.clear();
        return;
    }

    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 camera_global[3] = { camera.mdV[VX], camera.mdV[VY], camera.mdV[VZ] };
    SKSurrounds::SelectParams params;
    params.mNearCells = (S32)llclamp((U32)near_cells, 1u, 8u);
    mSelection.select(camera_global, mLive, params);

    SKTerrainCache& cache = SKTerrainCache::instance();
    for (const SKSurrounds::Node& node : mSelection.nodes())
    {
        if (node.mBand != SKSurrounds::Band::Ring)
        {
            continue;
        }
        const U64 handle = SKTerrain::handleFromGrid((U32)node.mX, (U32)node.mY);
        auto it = std::find_if(mCells.begin(), mCells.end(), [handle](const std::unique_ptr<Cell>& c) { return c->mHandle == handle; });
        if (it == mCells.end())
        {
            auto cell = std::make_unique<Cell>();
            cell->mHandle = handle;
            cell->mX = node.mX;
            cell->mY = node.mY;
            mCells.push_back(std::move(cell));
            it = mCells.end() - 1;
        }
        (*it)->mSeen = mFrame;
    }
    // What left the ring lets its maps and its look go.
    std::erase_if(mCells, [this](const std::unique_ptr<Cell>& c) { return c->mSeen != mFrame; });

    S32 builds = 0;
    for (const std::unique_ptr<Cell>& cell : mCells)
    {
        SKTerrainCache::region_ptr region = cache.get(cell->mHandle);
        if (region && region != cell->mRegion && builds < BUILDS_PER_FRAME)
        {
            ++builds;
            LL_PROFILE_ZONE_NAMED("surrounds build");
            cell->mRegion = region;
            cell->mDrawable = build(*cell);
        }
        // The agent region's frame moves with every crossing; the cell's corner is a whole number of regions from it.
        const LLVector3 origin_agent = gAgent.getPosAgentFromGlobal(originGlobal(cell->mX, cell->mY));
        cell->mModel.setIdentity();
        cell->mModel.setTranslation(origin_agent);
    }
}

bool SKSurroundsRenderer::parseLook(Cell& cell)
{
    LLSD look;
    std::istringstream in(cell.mRegion->mCell.mSidecar);
    if (cell.mRegion->mCell.mSidecar.empty() || LLSDSerialize::fromNotation(look, in, cell.mRegion->mCell.mSidecar.size()) <= 0 ||
        !look.isMap())
    {
        return false;
    }
    cell.mPBR = look["type"].asString() == "pbr";
    cell.mPaintType = (U32)look["paint_type"].asInteger();
    // A painted region's map is not cached; it waits for the map tiles (8b).
    if (cell.mPBR && cell.mPaintType != TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE)
    {
        return false;
    }
    cell.mLook = std::make_unique<LLTerrainMaterials>();
    cell.mAlphaMask = false;
    for (S32 i = 0; i < LLTerrainMaterials::ASSET_COUNT; ++i)
    {
        const LLUUID id = look["assets"][i].asUUID();
        if (id.isNull())
        {
            return false;
        }
        cell.mLook->setDetailAssetID(i, id);
        if (cell.mPBR)
        {
            const LLFetchedGLTFMaterial* material = gGLTFMaterialList.getMaterial(id);
            cell.mAlphaMask = cell.mAlphaMask || (material && material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK);
        }
    }
    cell.mLook->setPaintType(cell.mPaintType);
    return true;
}

F32 SKSurroundsRenderer::heightAt(const Cell& cell, const SKTerrainPyramid& own, S32 gx, S32 gy)
{
    // A neighbour's lattice 0 is this cell's 256th point, as ALTerrainSurfaceMaps::resolve has it.
    const S32 dx = gx < 0 ? -1 : (gx >= CELL_METRES ? 1 : 0);
    const S32 dy = gy < 0 ? -1 : (gy >= CELL_METRES ? 1 : 0);
    if (dx == 0 && dy == 0)
    {
        return latticeAt(own, gx, gy);
    }
    const S32 lx = gx - dx * CELL_METRES;
    const S32 ly = gy - dy * CELL_METRES;
    const U64 handle = SKTerrain::handleFromGrid((U32)(cell.mX + dx), (U32)(cell.mY + dy));
    if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(handle))
    {
        const LLSurface& land = regionp->getLand();
        if (land.hasZData() && land.getGridsPerEdge() == GRIDS_PER_EDGE)
        {
            return land.getZ(llclamp(lx, 0, CELL_METRES), llclamp(ly, 0, CELL_METRES));
        }
    }
    else if (const SKTerrainPyramid* neighbour = terrainOf(SKTerrainCache::instance().get(handle)))
    {
        return latticeAt(*neighbour, lx, ly);
    }
    // No neighbour: the own edge repeats, as an unconnected live surface's buffer does.
    return latticeAt(own, gx, gy);
}

bool SKSurroundsRenderer::build(Cell& cell)
{
    cell.mHeight = nullptr;
    cell.mComposition = nullptr;
    cell.mLook.reset();
    const SKTerrainPyramid* own = terrainOf(cell.mRegion);
    if (!own || !parseLook(cell))
    {
        return false;
    }
    cell.mMinZ = own->mMin;
    cell.mMaxZ = own->mMax;

    LLSD look;
    {
        std::istringstream in(cell.mRegion->mCell.mSidecar);
        LLSDSerialize::fromNotation(look, in, cell.mRegion->mCell.mSidecar.size());
    }
    F32 start[LLVLComposition::CORNER_COUNT];
    F32 range[LLVLComposition::CORNER_COUNT];
    for (S32 c = 0; c < LLVLComposition::CORNER_COUNT; ++c)
    {
        start[c] = (F32)look["start"][c].asReal();
        range[c] = (F32)look["range"][c].asReal();
        if (range[c] == 0.f)
        {
            range[c] = 1.f;
        }
    }

    const LLVector3d origin = originGlobal(cell.mX, cell.mY);
    ALTerrainSurfaceMaps::fill(mHeightStaging, GRIDS_PER_EDGE, 1,
                               [&](S32 gx, S32 gy, F32* texel) { texel[0] = heightAt(cell, *own, gx, gy); });
    // The composition is only sampled inside the region, as the live maps have it, so its apron repeats the edge.
    const F32 inv_width = 1.f / (F32)CELL_METRES;
    ALTerrainSurfaceMaps::fill(mCompositionStaging, GRIDS_PER_EDGE, 2,
                               [&](S32 gx, S32 gy, F32* texel)
                               {
                                   const S32 cx = llclamp(gx, 0, GRIDS_PER_EDGE - 1);
                                   const S32 cy = llclamp(gy, 0, GRIDS_PER_EDGE - 1);
                                   texel[0] = LLVLComposition::compositionValue(origin, cx, cy, 1.f, inv_width,
                                                                                heightAt(cell, *own, cx, cy), start, range);
                                   texel[1] = terrain_composition_noise(origin.mdV[VX] + gx, origin.mdV[VY] + gy);
                               });
    {
        LL_PROFILE_ZONE_NAMED("surrounds upload");
        upload(cell.mHeight, GL_R32F, GL_RED, 1, mHeightStaging);
        upload(cell.mComposition, GL_RG16F, GL_RG, 2, mCompositionStaging);
        gGL.getTextureSlot(0)->unbind();
    }
    return true;
}

void SKSurroundsRenderer::draw()
{
    if (mCells.empty())
    {
        return;
    }
    if (mPatches.isNull())
    {
        // Every cell's patches are the same 16 m squares, region-local, wound as LLVOSurfacePatch winds them.
        mPatches = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        if (!mPatches->allocateBuffer(PATCHES_PER_EDGE * PATCHES_PER_EDGE * PATCH_CORNERS, 0))
        {
            mPatches = nullptr;
            return;
        }
        LLStrider<LLVector3> vert;
        mPatches->getVertexStrider(vert);
        for (S32 y = 0; y < PATCHES_PER_EDGE; ++y)
        {
            for (S32 x = 0; x < PATCHES_PER_EDGE; ++x)
            {
                const F32 x0 = (F32)(x * PATCH_METRES), y0 = (F32)(y * PATCH_METRES);
                const F32 x1 = x0 + PATCH_METRES, y1 = y0 + PATCH_METRES;
                *vert++ = LLVector3(x0, y0, 0.f);
                *vert++ = LLVector3(x1, y0, 0.f);
                *vert++ = LLVector3(x1, y1, 0.f);
                *vert++ = LLVector3(x0, y1, 0.f);
            }
        }
        mPatches->unmapBuffer();
    }
    if (mAlphaRamp.isNull())
    {
        mAlphaRamp = LLViewerTextureManager::getFetchedTexture(IMG_ALPHA_GRAD_2D);
    }

    LLViewerCamera& camera = *LLViewerCamera::getInstance();
    gGL.setPatchVertices(PATCH_CORNERS);
    gGLLastMatrix = nullptr;
    gGL.loadMatrix(camera.getModelview());

    for (const std::unique_ptr<Cell>& cellp : mCells)
    {
        Cell& cell = *cellp;
        if (!cell.mDrawable)
        {
            continue;
        }
        // Not ready is not drawn, but asked for again: the call boosts what the look still fetches.
        const bool ready = cell.mPBR ? cell.mLook->makeMaterialsReady(true, true) : cell.mLook->makeTexturesReady(true, true);
        if (!ready)
        {
            continue;
        }
        const LLVector3 origin_agent = gAgent.getPosAgentFromGlobal(originGlobal(cell.mX, cell.mY));
        const F32 half = 0.5f * CELL_METRES;
        const LLVector4a center(origin_agent.mV[VX] + half, origin_agent.mV[VY] + half, 0.5f * (cell.mMinZ + cell.mMaxZ));
        const LLVector4a extent(half, half, 0.5f * (cell.mMaxZ - cell.mMinZ) + 1.f);
        if (!camera.AABBInFrustumNoFarClip(center, extent))
        {
            continue;
        }

        LLGLSLShader* programs = cell.mAlphaMask ? gDeferredPBRTerrainAlphaMaskProgram : gDeferredPBRTerrainProgram;
        LLGLSLShader* shader = cell.mPBR ? programs[llclamp(cell.mPaintType, 0u, (U32)TERRAIN_PAINT_TYPE_COUNT - 1)].selectVariant()
                                         : gDeferredTerrainProgram.selectVariant();
        shader->bind();
        // A program that did not build binds the stand-in, which has no tessellation stages to take a patch.
        if (!shader->isComplete())
        {
            shader->unbind();
            continue;
        }
        const S32 height_slot = shader->enableTexture(LLShaderMgr::TERRAIN_HEIGHT_MAP);
        const S32 composition_slot = shader->enableTexture(LLShaderMgr::TERRAIN_COMPOSITION_MAP);
        if (height_slot >= 0)
        {
            gGL.getTextureSlot(height_slot)->bindSampled(cell.mHeight.get(), ALSamplers::PointClamp);
        }
        if (composition_slot >= 0)
        {
            gGL.getTextureSlot(composition_slot)->bindSampled(cell.mComposition.get(), ALSamplers::BilinearClamp);
        }
        LLDrawPoolTerrain::bindSurfaceUniforms(shader, origin_agent, 1.f, (F32)CELL_METRES);
        shader->uniform1i(LLShaderMgr::TERRAIN_SHOW_PARCEL_OWNERS, 0);

        const auto draw_patches = [&]()
        {
            LLRenderPass::applyModelMatrix(&cell.mModel);
            mPatches->setBuffer();
            mPatches->drawArrays(LLRender::PATCHES, 0, PATCHES_PER_EDGE * PATCHES_PER_EDGE * PATCH_CORNERS);
        };
        const LLVector3d origin_global = originGlobal(cell.mX, cell.mY);
        if (cell.mPBR)
        {
            LLDrawPoolTerrain::renderLookPBR(shader, *cell.mLook, cell.mPaintType, nullptr, origin_global, mAlphaRamp, draw_patches);
        }
        else
        {
            LLDrawPoolTerrain::renderLookTextures(shader, *cell.mLook, origin_global, mAlphaRamp, draw_patches);
        }
        shader->disableTexture(LLShaderMgr::TERRAIN_HEIGHT_MAP);
        shader->disableTexture(LLShaderMgr::TERRAIN_COMPOSITION_MAP);
        shader->unbind();
    }

    LLVertexBuffer::unbind();
    gGLLastMatrix = nullptr;
    gGL.loadMatrix(camera.getModelview());
}
