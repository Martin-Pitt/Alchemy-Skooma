/**
 * @file sksurroundsrenderer.cpp
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
    constexpr S32 FAR_BUILDS_PER_FRAME = 2;
    constexpr S32 FAR_CHECKS_PER_FRAME = 16;
    constexpr S32 FAR_PATCHES = SKSurrounds::FAR_PATCHES_PER_EDGE;
    constexpr U32 TILE_KEEP_FRAMES = 600;
    constexpr S32 MAP_LEVELS = 8; // the map server's coarsest level
    constexpr F32 NO_HEIGHT = -1.0e30f; // below surroundsTC's SK_SURROUNDS_NO_HEIGHT: the patch is discarded

    S32 floorDiv(S32 a, S32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    U64 tileKey(S32 level, S32 x, S32 y) { return ((U64)(U32)level << 48) | ((U64)((U32)x & 0xffffff) << 24) | (U64)((U32)y & 0xffffff); }

    U64 nodeKey(const SKSurrounds::Node& node) { return tileKey(node.mLevel, node.mX, node.mY); }

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
        // Float data has no alpha to analyse, and asking would warn on every upload.
        image->setNeedsAlphaAndPickMask(false);
        image->setExplicitFormat(internal_format, primary_format, GL_FLOAT);
        image->createGLTexture(0, reinterpret_cast<const U8*>(data.data()));
    }
} // namespace

SKSurroundsRenderer::SKSurroundsRenderer() = default;

SKSurroundsRenderer::~SKSurroundsRenderer() = default;

void SKSurroundsRenderer::release()
{
    mCells.clear();
    mFar.clear();
    mTiles.clear();
    for (LLPointer<LLVertexBuffer>& patches : mFarPatches)
    {
        patches = nullptr;
    }
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
        if (!mCells.empty() || !mFar.empty() || !mTiles.empty() || mPatches.notNull())
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
            drawRingTiles();
        }
    }
    // Past the ring only an infinite projection reaches; a finite one ends inside it.
    if (ALFarPlane::isInfinite(LLViewerCamera::getInstance()->getProjectionFar()))
    {
        LL_PROFILE_ZONE_NAMED("surrounds far draw");
        {
            LL_PROFILE_GPU_ZONE("surrounds far");
            drawFar();
        }
    }
    // Tiles nothing has asked for a while are let go.
    std::erase_if(mTiles, [this](const auto& entry) { return mFrame - entry.second.mSeen > TILE_KEEP_FRAMES; });
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
        mFar.clear();
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
        std::copy(std::begin(node.mNeighbour), std::end(node.mNeighbour), (*it)->mNeighbour);
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

    {
        LL_PROFILE_ZONE_NAMED("surrounds far select");
        selectFar(mSelection.nodes());
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
    cell.mHasHeights = false;
    const SKTerrainPyramid* own = terrainOf(cell.mRegion);
    if (!own)
    {
        return false;
    }
    cell.mMinZ = own->mMin;
    cell.mMaxZ = own->mMax;

    ALTerrainSurfaceMaps::fill(mHeightStaging, GRIDS_PER_EDGE, 1,
                               [&](S32 gx, S32 gy, F32* texel) { texel[0] = heightAt(cell, *own, gx, gy); });
    {
        LL_PROFILE_ZONE_NAMED("surrounds upload");
        upload(cell.mHeight, GL_R32F, GL_RED, 1, mHeightStaging);
        gGL.getTextureSlot(0)->unbind();
    }
    cell.mHasHeights = true;
    // Without a look the map tile draws the cell.
    if (!parseLook(cell))
    {
        cell.mLook.reset();
        return false;
    }

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
        cell.mLookReady = false;
        if (!cell.mDrawable)
        {
            continue;
        }
        // Not ready is not drawn, but asked for again: the call boosts what the look still fetches. The map tile
        // stands in meanwhile.
        const bool ready = cell.mPBR ? cell.mLook->makeMaterialsReady(true, true) : cell.mLook->makeTexturesReady(true, true);
        if (!ready)
        {
            continue;
        }
        cell.mLookReady = true;
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

void SKSurroundsRenderer::selectFar(const std::vector<SKSurrounds::Node>& nodes)
{
    for (const SKSurrounds::Node& node : nodes)
    {
        if (node.mBand != SKSurrounds::Band::Far)
        {
            continue;
        }
        const U64 key = nodeKey(node);
        auto it = std::find_if(mFar.begin(), mFar.end(), [key](const std::unique_ptr<FarNode>& n) { return nodeKey(n->mNode) == key; });
        if (it == mFar.end())
        {
            mFar.push_back(std::make_unique<FarNode>());
            it = mFar.end() - 1;
        }
        (*it)->mNode = node; // the neighbours' levels move with the selection
        (*it)->mSeen = mFrame;
    }
    std::erase_if(mFar, [this](const std::unique_ptr<FarNode>& n) { return n->mSeen != mFrame; });
    if (mFar.empty())
    {
        return;
    }

    // A node is built again once a region under it or beside it changes in the cache: its signature, taken in turns.
    // A node not yet signed is checked before the round goes on.
    S32 checks = 0;
    for (const std::unique_ptr<FarNode>& node : mFar)
    {
        if (!node->mSigned && checks < FAR_CHECKS_PER_FRAME)
        {
            ++checks;
            node->mSignature = farSignature(*node);
            node->mSigned = true;
            node->mDirty = true;
        }
    }
    for (S32 round = 0; checks < FAR_CHECKS_PER_FRAME && round < (S32)mFar.size(); ++checks, ++round)
    {
        FarNode& node = *mFar[mFarCheck++ % mFar.size()];
        const size_t signature = farSignature(node);
        if (signature != node.mSignature)
        {
            node.mSignature = signature;
            node.mDirty = true;
        }
    }

    S32 builds = 0;
    for (const std::unique_ptr<FarNode>& node : mFar)
    {
        if (builds >= FAR_BUILDS_PER_FRAME)
        {
            break;
        }
        if (node->mSigned && node->mDirty)
        {
            ++builds;
            LL_PROFILE_ZONE_NAMED("surrounds far build");
            buildFar(*node);
            node->mDirty = false;
        }
    }
}

size_t SKSurroundsRenderer::farSignature(const FarNode& node_far)
{
    // The regions under the node and the ring around it, whose edge feeds its buffer row; asking also starts their reads.
    SKTerrainCache& cache = SKTerrainCache::instance();
    const S32 n = node_far.mNode.sizeCells();
    size_t signature = 0;
    for (S32 y = -1; y <= n; ++y)
    {
        for (S32 x = -1; x <= n; ++x)
        {
            const U64 handle = SKTerrain::handleFromGrid((U32)(node_far.mNode.mX + x), (U32)(node_far.mNode.mY + y));
            const size_t region = (size_t)cache.get(handle).get();
            signature = (signature * 1099511628211ull) ^ region;
        }
    }
    return signature;
}

void SKSurroundsRenderer::buildFar(FarNode& node_far)
{
    const S32 level = node_far.mNode.mLevel;
    const S32 n = node_far.mNode.sizeCells();
    const S32 step = 1 << level;                 // metres per grid
    const S32 per_region = CELL_METRES >> level; // grids a region gives at this level
    const S32 table_w = n + 2;

    struct Source
    {
        const SKTerrainPyramid::Level* mTerrain = nullptr;
        const SKTerrainPyramid::Level* mSurface = nullptr;
    };
    std::vector<Source> sources((size_t)table_w * table_w);
    SKTerrainCache& cache = SKTerrainCache::instance();
    std::vector<SKTerrainCache::region_ptr> held; // keeps what the table points into for the build
    held.reserve(sources.size());
    node_far.mMinZ = std::numeric_limits<F32>::max();
    node_far.mMaxZ = -std::numeric_limits<F32>::max();
    bool any = false;
    for (S32 y = -1; y <= n; ++y)
    {
        for (S32 x = -1; x <= n; ++x)
        {
            SKTerrainCache::region_ptr region = cache.get(SKTerrain::handleFromGrid((U32)(node_far.mNode.mX + x), (U32)(node_far.mNode.mY + y)));
            const SKTerrainPyramid* terrain = region ? region->mPyramids[SKTerrain::LAYER_TERRAIN].get() : nullptr;
            if (!terrain || !terrain->hasData() || terrain->mLevels[level].mSize != (U32)per_region)
            {
                continue;
            }
            Source& source = sources[(size_t)(y + 1) * table_w + (x + 1)];
            source.mTerrain = &terrain->mLevels[level];
            const SKTerrainPyramid* surface = region->mPyramids[SKTerrain::LAYER_SURFACE].get();
            const bool has_surface = surface && surface->hasData() && surface->mLevels[level].mSize == (U32)per_region;
            if (has_surface)
            {
                source.mSurface = &surface->mLevels[level];
            }
            if (x >= 0 && y >= 0 && x < n && y < n)
            {
                any = true;
                node_far.mMinZ = llmin(node_far.mMinZ, terrain->mMin);
                node_far.mMaxZ = llmax(node_far.mMaxZ, has_surface ? llmax(terrain->mMax, surface->mMax) : terrain->mMax);
            }
            held.push_back(std::move(region));
        }
    }
    node_far.mHeight = nullptr;
    node_far.mMask = 0;
    if (!any)
    {
        return; // nothing cached under it: flat masked tiles are 8c
    }

    // The node's grid in grids from its corner. A region's lattice 0 is its west neighbour's buffer column, so where that
    // region is missing the neighbour's last sample repeats, as an unconnected live surface's buffer does.
    const auto sample = [&](S32 gx, S32 gy) -> F32
    {
        const S32 mx = gx * step;
        const S32 my = gy * step;
        const S32 rx = floorDiv(mx, CELL_METRES);
        const S32 ry = floorDiv(my, CELL_METRES);
        const S32 lx = (mx - rx * CELL_METRES) / step;
        const S32 ly = (my - ry * CELL_METRES) / step;
        for (S32 attempt = 0; attempt < 4; ++attempt)
        {
            const S32 dx = (attempt & 1) ? 1 : 0;
            const S32 dy = (attempt & 2) ? 1 : 0;
            if ((dx && lx != 0) || (dy && ly != 0))
            {
                continue;
            }
            const S32 tx = rx - dx + 1;
            const S32 ty = ry - dy + 1;
            if (tx < 0 || ty < 0 || tx >= table_w || ty >= table_w)
            {
                continue;
            }
            const Source& source = sources[(size_t)ty * table_w + tx];
            if (!source.mTerrain)
            {
                continue;
            }
            const size_t i = (size_t)(dy ? per_region - 1 : ly) * per_region + (dx ? per_region - 1 : lx);
            F32 h = source.mTerrain->mHeight[i];
            // The surface where it is known, never below the ground.
            if (source.mSurface && source.mSurface->mValid[i])
            {
                h = llmax(h, source.mSurface->mHeight[i]);
            }
            return h;
        }
        return NO_HEIGHT;
    };
    ALTerrainSurfaceMaps::fill(mHeightStaging, GRIDS_PER_EDGE, 1, [&](S32 gx, S32 gy, F32* texel) { texel[0] = sample(gx, gy); });

    // The apron outside the node takes the nearest grid point inside it where it has no height of its own.
    const S32 apron = ALTerrainSurfaceMaps::APRON;
    const S32 size = GRIDS_PER_EDGE + 2 * apron;
    const auto at = [&](S32 gx, S32 gy) -> F32& { return mHeightStaging[(size_t)(gy + apron) * size + (gx + apron)]; };
    for (S32 gy = -apron; gy < GRIDS_PER_EDGE + apron; ++gy)
    {
        for (S32 gx = -apron; gx < GRIDS_PER_EDGE + apron; ++gx)
        {
            const bool inside = gx >= 0 && gy >= 0 && gx < GRIDS_PER_EDGE && gy < GRIDS_PER_EDGE;
            if (!inside && at(gx, gy) == NO_HEIGHT)
            {
                at(gx, gy) = at(llclamp(gx, 0, GRIDS_PER_EDGE - 1), llclamp(gy, 0, GRIDS_PER_EDGE - 1));
            }
        }
    }

    // A patch is drawn only where every grid point it spans has a height.
    const S32 grids_per_patch = CELL_METRES / FAR_PATCHES;
    for (S32 py = 0; py < FAR_PATCHES; ++py)
    {
        for (S32 px = 0; px < FAR_PATCHES; ++px)
        {
            bool whole = true;
            for (S32 gy = py * grids_per_patch; whole && gy <= (py + 1) * grids_per_patch; ++gy)
            {
                for (S32 gx = px * grids_per_patch; gx <= (px + 1) * grids_per_patch; ++gx)
                {
                    if (at(gx, gy) == NO_HEIGHT)
                    {
                        whole = false;
                        break;
                    }
                }
            }
            if (whole)
            {
                node_far.mMask |= 1ull << (py * FAR_PATCHES + px);
            }
        }
    }
    if (node_far.mMask == 0)
    {
        return;
    }
    {
        LL_PROFILE_ZONE_NAMED("surrounds far upload");
        upload(node_far.mHeight, GL_R32F, GL_RED, 1, mHeightStaging);
        gGL.getTextureSlot(0)->unbind();
    }
}

LLViewerFetchedTexture* SKSurroundsRenderer::tile(S32 level, S32 x, S32 y)
{
    if (level > MAP_LEVELS)
    {
        return nullptr;
    }
    Tile& entry = mTiles[tileKey(level, x, y)];
    if (entry.mTexture.isNull())
    {
        // The map's own URL, as LLWorldMipmap::loadObjectsTile builds it, so the map floater's fetch is the same texture.
        static LLCachedControl<std::string> map_server(gSavedSettings, "CurrentMapServerURL", "");
        const std::string url = (std::string)map_server + llformat("map-%d-%d-%d-objects.jpg", level, x, y);
        entry.mTexture = LLViewerTextureManager::getFetchedTextureFromUrl(url, FTT_MAP_TILE, true, LLGLTexture::BOOST_NONE,
                                                                          LLViewerTexture::LOD_TEXTURE);
    }
    entry.mSeen = mFrame;
    if (entry.mTexture.notNull())
    {
        entry.mTexture->setBoostLevel(LLGLTexture::BOOST_MAP);
    }
    return entry.mTexture.get();
}

LLViewerFetchedTexture* SKSurroundsRenderer::tileFor(const SKSurrounds::Node& node, F32 uv[4])
{
    const auto ready = [](LLViewerFetchedTexture* tex) { return tex && !tex->isMissingAsset() && tex->getDiscardLevel() >= 0; };

    const SKSurrounds::MapTile own = SKSurrounds::mapTile(node);
    LLViewerFetchedTexture* tex = tile(own.mLevel, own.mX, own.mY);
    // The parent is asked for too, so it is there while the next split's tiles load.
    const S32 parent_level = own.mLevel + 1;
    const S32 span = 1 << (parent_level - 1);
    const S32 parent_x = floorDiv(node.mX, span) * span;
    const S32 parent_y = floorDiv(node.mY, span) * span;
    LLViewerFetchedTexture* parent = tile(parent_level, parent_x, parent_y);
    if (ready(tex))
    {
        uv[0] = uv[1] = 1.f;
        uv[2] = uv[3] = 0.f;
        return tex;
    }
    if (ready(parent))
    {
        const F32 inv = 1.f / (F32)span;
        uv[0] = uv[1] = (F32)node.sizeCells() * inv;
        uv[2] = (F32)(node.mX - parent_x) * inv;
        uv[3] = (F32)(node.mY - parent_y) * inv;
        return parent;
    }
    return nullptr;
}

void SKSurroundsRenderer::drawTiled(LLGLSLShader* shader, LLImageGL* height, LLViewerFetchedTexture* tile, const F32 uv[4],
                                    const S32 edges[4], U64 mask, F32 coarse_ratio, const LLVector3& origin_agent, F32 grid_scale,
                                    F32 width, LLVertexBuffer* patches, U32 corners)
{
    const S32 height_slot = shader->enableTexture(LLShaderMgr::TERRAIN_HEIGHT_MAP);
    if (height_slot >= 0)
    {
        gGL.getTextureSlot(height_slot)->bindSampled(height, ALSamplers::PointClamp);
    }
    shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tile, ALSamplers::BilinearClamp);
    LLDrawPoolTerrain::bindSurfaceUniforms(shader, origin_agent, grid_scale, width);
    shader->uniform4fv(LLShaderMgr::SK_SURROUNDS_TILE, 1, uv);
    shader->uniform4iv(LLShaderMgr::SK_SURROUNDS_EDGES, 1, edges);
    const GLuint mask_words[4] = { (GLuint)(mask & 0xffffffffull), (GLuint)(mask >> 32), 0u, 0u };
    shader->uniform4uiv(LLShaderMgr::SK_SURROUNDS_MASK, 1, mask_words);
    const F32 params[4] = { coarse_ratio, 0.f, 0.f, 0.f };
    shader->uniform4fv(LLShaderMgr::SK_SURROUNDS_PARAMS, 1, params);
    patches->setBuffer();
    patches->drawArrays(LLRender::PATCHES, 0, corners);
    shader->disableTexture(LLShaderMgr::TERRAIN_HEIGHT_MAP);
    shader->unbindTexture(LLShaderMgr::DIFFUSE_MAP);
}

void SKSurroundsRenderer::drawRingTiles()
{
    if (mCells.empty() || mPatches.isNull())
    {
        return;
    }
    LLGLSLShader* shader = nullptr;
    LLViewerCamera& camera = *LLViewerCamera::getInstance();
    for (const std::unique_ptr<Cell>& cellp : mCells)
    {
        Cell& cell = *cellp;
        if (!cell.mHasHeights || cell.mLookReady)
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
        SKSurrounds::Node node;
        node.mX = cell.mX;
        node.mY = cell.mY;
        node.mLevel = SKSurrounds::LEVEL_SPECIAL;
        node.mBand = SKSurrounds::Band::Ring;
        F32 uv[4];
        LLViewerFetchedTexture* tex = tileFor(node, uv);
        if (!tex)
        {
            continue;
        }
        if (!shader)
        {
            shader = &gSKSurroundsRingProgram;
            shader->bind();
            // A program that did not build binds the stand-in, which has no tessellation stages to take a patch.
            if (!shader->isComplete())
            {
                shader->unbind();
                return;
            }
            gGL.setPatchVertices(PATCH_CORNERS);
        }
        // A ring cell is the fine half where a far node of level 0 meets it, and never meets a coarser one.
        S32 edges[SKSurrounds::SIDE_COUNT];
        for (S32 s = 0; s < SKSurrounds::SIDE_COUNT; ++s)
        {
            edges[s] = cell.mNeighbour[s] == 0 ? 1 : 0;
        }
        LLRenderPass::applyModelMatrix(&cell.mModel);
        drawTiled(shader, cell.mHeight.get(), tex, uv, edges, ~0ull, 1.f, origin_agent, 1.f, (F32)CELL_METRES, mPatches,
                  PATCHES_PER_EDGE * PATCHES_PER_EDGE * PATCH_CORNERS);
    }
    if (shader)
    {
        shader->unbind();
        LLVertexBuffer::unbind();
        gGLLastMatrix = nullptr;
        gGL.loadMatrix(camera.getModelview());
    }
}

void SKSurroundsRenderer::drawFar()
{
    if (mFar.empty())
    {
        return;
    }
    LLGLSLShader* shader = &gSKSurroundsFarProgram;
    LLViewerCamera& camera = *LLViewerCamera::getInstance();
    const bool pinned = gPipeline.shaderInputs().mLocal.mTerrainTessPinned;
    bool bound = false;
    for (const std::unique_ptr<FarNode>& node_farp : mFar)
    {
        FarNode& node_far = *node_farp;
        if (node_far.mMask == 0 || node_far.mHeight.isNull())
        {
            continue;
        }
        const SKSurrounds::Node& node = node_far.mNode;
        const F32 metres = (F32)node.sizeMetres();
        const LLVector3 origin_agent = gAgent.getPosAgentFromGlobal(originGlobal(node.mX, node.mY));
        const F32 half = 0.5f * metres;
        const LLVector4a center(origin_agent.mV[VX] + half, origin_agent.mV[VY] + half, 0.5f * (node_far.mMinZ + node_far.mMaxZ));
        const LLVector4a extent(half, half, 0.5f * (node_far.mMaxZ - node_far.mMinZ) + 1.f);
        if (!camera.AABBInFrustumNoFarClip(center, extent))
        {
            continue;
        }
        F32 uv[4];
        LLViewerFetchedTexture* tex = tileFor(node, uv);
        if (!tex)
        {
            continue;
        }

        LLPointer<LLVertexBuffer>& patches = mFarPatches[node.mLevel];
        if (patches.isNull())
        {
            // The node's 8x8 patches in its own metres, row by row, so a patch's index is its bit in the mask.
            patches = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
            if (!patches->allocateBuffer(FAR_PATCHES * FAR_PATCHES * PATCH_CORNERS, 0))
            {
                patches = nullptr;
                continue;
            }
            LLStrider<LLVector3> vert;
            patches->getVertexStrider(vert);
            const F32 edge = metres / FAR_PATCHES;
            for (S32 y = 0; y < FAR_PATCHES; ++y)
            {
                for (S32 x = 0; x < FAR_PATCHES; ++x)
                {
                    const F32 x0 = x * edge, y0 = y * edge;
                    const F32 x1 = x0 + edge, y1 = y0 + edge;
                    *vert++ = LLVector3(x0, y0, 0.f);
                    *vert++ = LLVector3(x1, y0, 0.f);
                    *vert++ = LLVector3(x1, y1, 0.f);
                    *vert++ = LLVector3(x0, y1, 0.f);
                }
            }
            patches->unmapBuffer();
        }

        if (!bound)
        {
            shader->bind();
            if (!shader->isComplete())
            {
                shader->unbind();
                return;
            }
            gGL.setPatchVertices(PATCH_CORNERS);
            bound = true;
        }

        // Per side: the same size, the fine half of a coarser neighbour's edge, or the coarse side of a finer one,
        // whose fine half is fractional odd where it is a ring cell at the live spacing.
        S32 edges[SKSurrounds::SIDE_COUNT];
        for (S32 s = 0; s < SKSurrounds::SIDE_COUNT; ++s)
        {
            const S8 neighbour = node.mNeighbour[s];
            if (neighbour == SKSurrounds::NO_NEIGHBOUR || neighbour == node.mLevel)
            {
                edges[s] = 0;
            }
            else if (neighbour > node.mLevel)
            {
                edges[s] = 1;
            }
            else
            {
                edges[s] = (neighbour == SKSurrounds::LEVEL_SPECIAL && !pinned) ? 3 : 2;
            }
        }
        node_far.mModel.setIdentity();
        node_far.mModel.setTranslation(origin_agent);
        LLRenderPass::applyModelMatrix(&node_far.mModel);
        drawTiled(shader, node_far.mHeight.get(), tex, uv, edges, node_far.mMask, 2.f, origin_agent, (F32)(1 << node.mLevel), metres,
                  patches, FAR_PATCHES * FAR_PATCHES * PATCH_CORNERS);
    }
    if (bound)
    {
        shader->unbind();
        LLVertexBuffer::unbind();
        gGLLastMatrix = nullptr;
        gGL.loadMatrix(camera.getModelview());
    }
}
