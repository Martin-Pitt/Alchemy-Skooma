/**
 * @file lldrawpoolterrain.h
 * @brief LLDrawPoolTerrain class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLDRAWPOOLTERRAIN_H
#define LL_LLDRAWPOOLTERRAIN_H

#include "lldrawpool.h"

#include <functional> // <SK:Nexii>

class LLGLSLShader;
class LLTerrainMaterials; // <SK:Nexii>

class LLDrawPoolTerrain : public LLFacePool
{
    LLPointer<LLViewerTexture> mTexturep;
public:
    // Four corners per patch; the surface itself comes from the region's maps.
    enum
    {
        VERTEX_DATA_MASK = LLVertexBuffer::MAP_VERTEX
    };

    virtual U32 getVertexDataMask();
    LLDrawPoolTerrain(LLViewerTexture *texturep);
    virtual ~LLDrawPoolTerrain();

    /*virtual*/ S32 getNumDeferredPasses() { return 1; }
    /*virtual*/ void beginDeferredPass(S32 pass);
    /*virtual*/ void endDeferredPass(S32 pass);
    /*virtual*/ void renderDeferred(S32 pass);

    /*virtual*/ S32 getNumShadowPasses() { return 1; }
    /*virtual*/ void beginShadowPass(S32 pass);
    /*virtual*/ void endShadowPass(S32 pass);
    /*virtual*/ void renderShadow(S32 pass);
    /*virtual*/ void addShadowCasters(ALSpotShadowCache::Casters& casters);

    /*virtual*/ void prerender();
    /*virtual*/ void dirtyTextures(const std::set<LLViewerFetchedTexture*>& textures);
    /*virtual*/ LLViewerTexture *getTexture();
    /*virtual*/ LLViewerTexture *getDebugTexture();
    /*virtual*/ LLColor3 getDebugColor() const; // For AGP debug display

    LLPointer<LLViewerTexture> mAlphaRampImagep;
    LLPointer<LLViewerTexture> m2DAlphaRampImagep;
    LLPointer<LLViewerTexture> mAlphaNoiseImagep;

    static S32 sPBRDetailMode;
    static F32 sDetailScale; // textures per meter
    static F32 sPBRDetailScale; // textures per meter
    static F32 sLODFactor; // RenderTerrainLODFactor squared; scales the tessellation density

    // <SK:Nexii> Distant regions draw through the live terrain's uniforms and looks.
    // The tessellation and placement uniforms bindSurface sets, for a surface whose south-west corner is at
    // region_origin_agent.
    static void bindSurfaceUniforms(LLGLSLShader* shader, const LLVector3& region_origin_agent, F32 meters_per_grid,
                                    F32 region_width);
    // A look's detail textures or materials bound around draw, for a surface whose corner is at origin_global.
    static void renderLookTextures(LLGLSLShader* shader, LLTerrainMaterials& look, const LLVector3d& origin_global,
                                   LLViewerTexture* alpha_ramp, const std::function<void()>& draw);
    static void renderLookPBR(LLGLSLShader* shader, LLTerrainMaterials& look, U32 paint_type, LLViewerTexture* paint_map,
                              const LLVector3d& origin_global, LLViewerTexture* alpha_ramp,
                              const std::function<void()>& draw);
    // </SK:Nexii>

protected:
    void boostTerrainDetailTextures();
    // The region's surface maps and the tessellation uniforms, for the bound
    // program; every pass that draws terrain binds the same ones.
    void bindSurface(LLGLSLShader* shader);
    void unbindSurface(LLGLSLShader* shader);
    void bindParcelOverlay(LLGLSLShader* shader);

    void renderFullShader();
    void renderFullShaderTextures();
    void renderFullShaderPBR(bool use_local_materials = false);
    void drawLoop();

    // Each projection's uv origin's place on the world's hex lattice (pbrterrainUtilF's terrain_hex_place), from
    // delta, three to a material (the top, the x slice, the y slice): the world's uv less the region's.
    static void uploadHexPlaces(LLGLSLShader* shader, const F64 (*delta)[2], U32 materials);
};

#endif // LL_LLDRAWPOOLSIMPLE_H
