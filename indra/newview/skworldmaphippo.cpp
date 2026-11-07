/**
 * @file skworldmaphippo.cpp
 * @brief What carries the 3D world map's disc: a mesh asset hung beneath it, seen only past the rim.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "skworldmaphippo.h"

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llfetchedgltfmaterial.h"
#include "llglslshader.h"
#include "llgltfmateriallist.h"
#include "llmodel.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexturelist.h"

#include <sstream>

using namespace skworldmap;

namespace
{
    constexpr F64 RETRY_SECONDS = 60.0;
    constexpr F64 LENGTH_OVER_DIAMETER = 1.25;     ///< head and tail reach past the rim
    constexpr F32 TEXTURE_PIXELS = 2048.f * 2048.f; ///< asked for whole: it can fill the screen past the rim
    // Sky-blue ambient light, the only light it gets, so the shading painted into its texture carries the form.
    const LLColor4 AMBIENT(0.8f, 0.85f, 1.f, 1.f);
}

SKWorldMapHippo::SKWorldMapHippo() = default;

SKWorldMapHippo::~SKWorldMapHippo()
{
    release();
}

void SKWorldMapHippo::release()
{
    if (mSelf)
    {
        *mSelf = nullptr;
        mSelf.reset();
    }
    mBuffer = nullptr;
    mTexture = nullptr;
    mMaterial = nullptr;
    mIndexCount = 0;
    mState = State::Idle;
}

void SKWorldMapHippo::fetch(const LLUUID& mesh_id)
{
    LLViewerRegion* region = gAgent.getRegion();
    const std::string cap = region ? region->getViewerAssetUrl() : std::string();
    if (cap.empty())
    {
        mRetryAt = LLFrameTimer::getTotalSeconds() + RETRY_SECONDS;
        return;
    }

    LL_INFOS("WorldMap") << "Fetching the world map's hippo mesh " << mesh_id << LL_ENDL;
    mState = State::Fetching;
    mSelf = std::make_shared<SKWorldMapHippo*>(this);
    std::weak_ptr<SKWorldMapHippo*> self = mSelf;
    const std::string url = cap + "/?mesh_id=" + mesh_id.asString();
    LLCoros::instance().launch("SKWorldMapHippo::fetch", [self, url]()
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("SKWorldMapHippo", policy);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        const LLSD result = adapter->getRawAndSuspend(request, url);

        std::shared_ptr<SKWorldMapHippo*> owner = self.lock();
        if (!owner || !*owner)
        {
            return;
        }
        SKWorldMapHippo* hippo = *owner;
        const LLCore::HttpStatus status =
            LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        if (status && hippo->build(std::string(raw.begin(), raw.end())))
        {
            hippo->mState = State::Ready;
            LL_INFOS("WorldMap") << "Loaded the world map's hippo mesh, " << hippo->mIndexCount / 3 << " triangles" << LL_ENDL;
            return;
        }
        LL_WARNS("WorldMap") << "Could not load the world map's hippo mesh: " << status.toString() << LL_ENDL;
        hippo->mState = State::Failed;
        hippo->mRetryAt = LLFrameTimer::getTotalSeconds() + RETRY_SECONDS;
    });
}

bool SKWorldMapHippo::build(const std::string& asset)
{
    LLPointer<LLModel> model = new LLModel(LLVolumeParams(), (F32)LLModel::LOD_HIGH);
    std::istringstream is(asset);
    if (!model->loadModel(is) || model->getNumVolumeFaces() == 0)
    {
        return false;
    }

    U32 vertices = 0, indices = 0;
    for (S32 f = 0; f < model->getNumVolumeFaces(); ++f)
    {
        vertices += model->getVolumeFace(f).mNumVertices;
        indices += model->getVolumeFace(f).mNumIndices;
    }
    if (vertices == 0 || vertices > 65535 || indices == 0)
    {
        return false;
    }

    LLPointer<LLVertexBuffer> buffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXCOORD0);
    if (!buffer->allocateBuffer(vertices, indices))
    {
        return false;
    }
    LLStrider<LLVector3> pos;
    LLStrider<LLVector2> uv;
    LLStrider<LLColor4U> colour;
    LLStrider<U16> idx;
    buffer->getVertexStrider(pos);
    buffer->getTexCoord0Strider(uv);
    buffer->getColorStrider(colour);
    const LLColor4U ambient(AMBIENT);
    buffer->getIndexStrider(idx);
    U16 base = 0;
    mMin.set(F32_MAX, F32_MAX, F32_MAX);
    mMax.set(-F32_MAX, -F32_MAX, -F32_MAX);
    for (S32 f = 0; f < model->getNumVolumeFaces(); ++f)
    {
        const LLVolumeFace& face = model->getVolumeFace(f);
        for (S32 v = 0; v < face.mNumVertices; ++v)
        {
            // The upload keeps glTF's axes, y up, and its origin on the paw plane: turn y up into z up.
            const F32* p = face.mPositions[v].getF32ptr();
            const LLVector3 turned(p[VX], -p[VZ], p[VY]);
            mMin.set(llmin(mMin.mV[VX], turned.mV[VX]), llmin(mMin.mV[VY], turned.mV[VY]), llmin(mMin.mV[VZ], turned.mV[VZ]));
            mMax.set(llmax(mMax.mV[VX], turned.mV[VX]), llmax(mMax.mV[VY], turned.mV[VY]), llmax(mMax.mV[VZ], turned.mV[VZ]));
            *pos++ = turned;
            *uv++ = face.mTexCoords ? face.mTexCoords[v] : LLVector2::zero;
            *colour++ = ambient;
        }
        for (S32 i = 0; i < face.mNumIndices; ++i)
        {
            *idx++ = base + face.mIndices[i];
        }
        base += static_cast<U16>(face.mNumVertices);
    }
    buffer->unmapBuffer();

    mBuffer = buffer;
    mIndexCount = indices;
    return true;
}

void SKWorldMapHippo::draw(const LLVector3d& eye, const WorldDisc& disc)
{
    static LLCachedControl<std::string> mesh_setting(gSavedSettings, "SKWorldMapHippoMesh", "");
    static LLCachedControl<std::string> texture_setting(gSavedSettings, "SKWorldMapHippoTexture", "");
    static LLCachedControl<std::string> material_setting(gSavedSettings, "SKWorldMapHippoMaterial", "");
    static LLCachedControl<LLVector3> size_setting(gSavedSettings, "SKWorldMapHippoSize", LLVector3(58.37f, 25.08f, 37.60f));

    const LLUUID mesh_id(mesh_setting());
    if (mesh_id.isNull() || disc.mRadius <= 0.0)
    {
        return;
    }
    if (mesh_id != mMeshId)
    {
        release();
        mMeshId = mesh_id;
    }

    // The mesh asset is stored in a unit box; its size restores the proportions, then it is scaled to the disc,
    // its origin, the middle of the paw plane, just under the disc's centre.
    const LLVector3 size = size_setting;
    if (size.mV[VX] <= 0.f || size.mV[VY] <= 0.f || size.mV[VZ] <= 0.f)
    {
        return;
    }
    const F64 metres = LENGTH_OVER_DIAMETER * 2.0 * disc.mRadius / size.mV[VX];
    const LLVector3d extent(size.mV[VX] * metres, size.mV[VY] * metres, size.mV[VZ] * metres);
    const LLVector3d centre(disc.mCentreX, disc.mCentreY, disc.mZ - 0.001 * disc.mRadius);
    if (mState == State::Ready)
    {
        const LLVector3d lo(centre.mdV[VX] + mMin.mV[VX] * extent.mdV[VX], centre.mdV[VY] + mMin.mV[VY] * extent.mdV[VY],
                            centre.mdV[VZ] + mMin.mV[VZ] * extent.mdV[VZ]);
        const LLVector3d hi(centre.mdV[VX] + mMax.mV[VX] * extent.mdV[VX], centre.mdV[VY] + mMax.mV[VY] * extent.mdV[VY],
                            centre.mdV[VZ] + mMax.mV[VZ] * extent.mdV[VZ]);
        if (hiddenUnderDisc(eye, disc, lo, hi))
        {
            return;
        }
    }
    else if (hiddenUnderDisc(eye, disc, centre - LLVector3d(extent.mdV[VX], extent.mdV[VY], 2.0 * extent.mdV[VZ]),
                             centre + LLVector3d(extent.mdV[VX], extent.mdV[VY], 0.0)))
    {
        // Before the mesh is in, a box twice its size round the paw plane's middle: fetched a little early, never late.
        return;
    }

    if (mState == State::Idle || (mState == State::Failed && LLFrameTimer::getTotalSeconds() >= mRetryAt))
    {
        if (LLFrameTimer::getTotalSeconds() >= mRetryAt)
        {
            fetch(mesh_id);
        }
    }
    if (mState != State::Ready || !mBuffer)
    {
        return;
    }
    // The material's base colour once it has loaded; the plain texture until then, or without one.
    const LLUUID material_id(material_setting());
    if (material_id.notNull() && mMaterial.isNull())
    {
        mMaterial = gGLTFMaterialList.getMaterial(material_id);
    }
    if (mMaterial.notNull() && mMaterial->isLoaded() && mMaterial->mBaseColorTexture.notNull()
        && mTexture != mMaterial->mBaseColorTexture)
    {
        mTexture = mMaterial->mBaseColorTexture;
        mTexture->setBoostLevel(LLGLTexture::BOOST_UI);
    }
    if (mTexture.isNull())
    {
        const LLUUID texture_id(texture_setting());
        if (texture_id.notNull())
        {
            mTexture = LLViewerTextureManager::getFetchedTexture(texture_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
        }
    }

    LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
    LLGLDisable blend(GL_BLEND);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    const LLVector3d at = centre - eye;
    gGL.translatef(static_cast<F32>(at.mdV[VX]), static_cast<F32>(at.mdV[VY]), static_cast<F32>(at.mdV[VZ]));
    gGL.scalef(static_cast<F32>(extent.mdV[VX]), static_cast<F32>(extent.mdV[VY]), static_cast<F32>(extent.mdV[VZ]));

    gUIProgram.bind();
    if (mTexture.notNull())
    {
        // Nothing else asks for it at a size, and a texture no one sizes stays at its smallest decode.
        mTexture->addTextureStats(TEXTURE_PIXELS);
        gGL.getTextureSlot(0)->bindSampled(mTexture.get(), ALSamplers::AnisoWrap);
    }
    else
    {
        gGL.getTextureSlot(0)->unbind();
    }
    gGL.syncMatrices();
    mBuffer->setBuffer();
    mBuffer->draw(LLRender::TRIANGLES, mIndexCount, 0);
    gGL.getTextureSlot(0)->unbind();
    gUIProgram.unbind();
    gGL.popMatrix();
}
