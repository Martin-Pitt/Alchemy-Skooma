/**
 * @file skobjectfaces.cpp
 * @brief What a viewer object's faces show: how each blends, and how much of it can be seen through.
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

#include "skobjectfaces.h"

#include "llfetchedgltfmaterial.h"
#include "llgltfmaterial.h"
#include "llmaterial.h"
#include "lltextureentry.h"
#include "llviewerobject.h"
#include "llviewertexture.h"

// The image a face draws: a PBR face its material's base colour, any other its own texture.
const LLViewerTexture* skFaceImage(const LLViewerObject* object, U8 f)
{
    const LLTextureEntry* te = object->getTE(f);
    if (const LLFetchedGLTFMaterial* gltf = te ? dynamic_cast<const LLFetchedGLTFMaterial*>(te->getGLTFRenderMaterial()) : nullptr)
    {
        return gltf->mBaseColorTexture.get();
    }
    return object->getTEImage(f);
}

// Whether one face can be seen through: set to blend whatever its tint,
// faint under blend, or the stock transparent texture.
bool skFaceSeeThrough(const LLViewerObject* object, U8 f)
{
    const LLTextureEntry* te = object->getTE(f);
    if (!te)
    {
        return false;
    }
    F32 alpha = te->getColor().mV[VALPHA];
    bool blended = true;
    bool blend_mode = false;
    if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
    {
        // A PBR face draws its material's base colour texture, not the face's own texture.
        if (gltf->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] == IMG_TRANSPARENT)
        {
            return true;
        }
        alpha *= gltf->mBaseColor.mV[VW];
        blended = gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND;
        blend_mode = blended;
    }
    else if (const LLMaterialPtr material = te->getMaterialParams(); material.notNull())
    {
        // A legacy material defaults to blend, so an opaque door with a
        // normal map reads as blended; only a texture with alpha is glass.
        const LLViewerTexture* image = skFaceImage(object, f);
        blend_mode = material->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND && image &&
                     image->getComponents() == 4;
    }
    return te->getID() == IMG_TRANSPARENT || blend_mode || (blended && alpha < 0.95f);
}

// A PBR material's own mode wins over a legacy material's, and with
// neither the face is blended only if its tint or texture carries alpha.
const char* skFaceAlphaMode(const LLTextureEntry* te)
{
    if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
    {
        switch (gltf->mAlphaMode)
        {
            case LLGLTFMaterial::ALPHA_MODE_BLEND: return "pbr-blend";
            case LLGLTFMaterial::ALPHA_MODE_MASK: return "pbr-mask";
            default: return "pbr-opaque";
        }
    }
    const LLMaterialPtr material = te->getMaterialParams();
    if (material.notNull())
    {
        switch (material->getDiffuseAlphaMode())
        {
            case LLMaterial::DIFFUSE_ALPHA_MODE_NONE: return "none";
            case LLMaterial::DIFFUSE_ALPHA_MODE_BLEND: return "blend";
            case LLMaterial::DIFFUSE_ALPHA_MODE_MASK: return "mask";
            case LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE: return "emissive";
            default: break;
        }
    }
    return "default";
}

F32 skFacePbrAlpha(const LLTextureEntry* te)
{
    const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial();
    return gltf ? gltf->mBaseColor.mV[VW] : 1.f;
}

S32 skLinkNumber(const LLViewerObject* object)
{
    const LLViewerObject* parent = static_cast<const LLViewerObject*>(object->getParent());
    if (!parent)
    {
        return 1;
    }
    S32 link = 2;
    for (const LLPointer<LLViewerObject>& child : parent->getChildren())
    {
        if (child.get() == object)
        {
            break;
        }
        if (!child->isAvatar())
        {
            ++link;
        }
    }
    return link;
}

F32 skSeeThroughShare(const LLViewerObject* object)
{
    const U8 faces = object->getNumTEs();
    if (faces == 0)
    {
        return 0.f;
    }
    U32 clear = 0;
    for (U8 f = 0; f < faces; ++f)
    {
        clear += skFaceSeeThrough(object, f) ? 1 : 0;
    }
    return static_cast<F32>(clear) / faces;
}
