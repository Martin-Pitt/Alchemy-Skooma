/**
 * @file sknavoverlay.cpp
 * @brief Gathered debug geometry for the navmesh overlays, drawn in one batch per primitive type.
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

#include "sknavoverlay.h"

#include "llglslshader.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"

#include <cmath>

namespace
{
    /// One uploaded batch. Nothing to draw without a buffer, and
    /// drawArrays asserts the buffer is the one bound.
    void drawBatch(const LLPointer<LLVertexBuffer>& vb, U32 mode)
    {
        if (vb.isNull() || vb->getNumVerts() == 0)
        {
            return;
        }
        vb->setBuffer();
        vb->drawArrays(mode, 0, vb->getNumVerts());
    }
}

SKNavOverlayGeom::~SKNavOverlayGeom() = default;

void SKNavOverlayGeom::line(const LLVector3& a, const LLVector3& b, const LLColor4& color)
{
    mUploaded = false;
    mLineVerts.push_back(a);
    mLineVerts.push_back(b);
    mLineColors.push_back(color);
    mLineColors.push_back(color);
}

void SKNavOverlayGeom::setEdgeWidth(F32 pixels, const LLVector3& camera_agent, F32 fov_y, S32 viewport_height)
{
    mUploaded = false;
    mEdgeWidth = pixels;
    mCamera = camera_agent;
    // How many world units a pixel covers, per metre of depth.
    mPixelScale = (viewport_height > 0) ? (2.f * std::tan(fov_y * 0.5f) / static_cast<F32>(viewport_height)) : 0.f;
}

void SKNavOverlayGeom::setFade(const LLVector3& centre, F32 radius, F32 fade_from)
{
    mUploaded = false;
    mFadeCentre = centre;
    mFadeRadius = radius;
    mFadeFrom = llclamp(fade_from, 0.f, radius);
}

F32 SKNavOverlayGeom::faded(F32 alpha, const LLVector3& at) const
{
    if (mFadeRadius <= 0.f)
    {
        return alpha;
    }
    const F32 span = mFadeRadius - mFadeFrom;
    if (span <= 0.f)
    {
        return alpha;
    }
    const F32 d = std::sqrt(dist_vec_squared2D(at, mFadeCentre));
    return alpha * llclamp((mFadeRadius - d) / span, 0.f, 1.f);
}

void SKNavOverlayGeom::edge(const LLVector3& a, const LLVector3& b, const LLColor4& color)
{
    mUploaded = false;
    if (mEdgeWidth <= 1.f || mPixelScale <= 0.f)
    {
        mEdgeVerts.push_back(a);
        mEdgeVerts.push_back(b);
        mEdgeColors.push_back(color);
        mEdgeColors.push_back(color);
        return;
    }

    LLVector3 along = b - a;
    const F32 length = along.normalize();
    if (length <= 0.f)
    {
        return;
    }
    // A ribbon that faces the camera, half a width to each side of the edge,
    // widened with depth so it holds its width on screen.
    auto offset = [&](const LLVector3& p) {
        LLVector3 to_camera = mCamera - p;
        const F32 depth = to_camera.normalize();
        LLVector3 side = along % to_camera;
        if (side.normalize() <= 0.f)
        {
            side = LLVector3(0.f, 0.f, 1.f) % along;
            side.normalize();
        }
        return side * (mEdgeWidth * 0.5f * llmax(depth, 0.1f) * mPixelScale);
    };
    const LLVector3 oa = offset(a), ob = offset(b);
    const LLVector3 quad[4] = { a - oa, a + oa, b + ob, b - ob };
    static const S32 ORDER[6] = { 0, 1, 2, 0, 2, 3 };
    for (S32 i : ORDER)
    {
        mEdgeVerts.push_back(quad[i]);
        mEdgeColors.push_back(color);
    }
}

void SKNavOverlayGeom::tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color)
{
    mUploaded = false;
    mTriVerts.push_back(a);
    mTriVerts.push_back(b);
    mTriVerts.push_back(c);
    mTriColors.push_back(color);
    mTriColors.push_back(color);
    mTriColors.push_back(color);
}

void SKNavOverlayGeom::box(const LLVector3& min, const LLVector3& max, const LLColor4& color)
{
    // Corner n has axis a at max when bit a is set, so an edge is a pair of
    // corners differing in exactly one bit.
    static const S32 EDGES[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                      { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    LLVector3 corner[8];
    for (S32 n = 0; n < 8; ++n)
    {
        corner[n].set((n & 1) ? max.mV[VX] : min.mV[VX], (n & 2) ? max.mV[VY] : min.mV[VY], (n & 4) ? max.mV[VZ] : min.mV[VZ]);
    }
    for (const auto& edge : EDGES)
    {
        line(corner[edge[0]], corner[edge[1]], color);
    }
}

void SKNavOverlayGeom::clear()
{
    mUploaded = false;
    mTriVerts.clear();
    mTriColors.clear();
    mLineVerts.clear();
    mLineColors.clear();
    mEdgeVerts.clear();
    mEdgeColors.clear();
}

void SKNavOverlayGeom::uploadBatch(const std::vector<LLVector3>& verts, const std::vector<LLColor4>& colors, LLPointer<LLVertexBuffer>& vb)
{
    if (verts.empty())
    {
        vb = nullptr;
        return;
    }
    // What bindShader() binds: gUIProgram reads position, texcoord0 and
    // diffuse_color, and setBuffer() refuses a buffer missing an attribute
    // the shader asks for.
    const U32 mask = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;
    if (vb.isNull())
    {
        vb = new LLVertexBuffer(mask);
    }
    // Same count, same size: allocateBuffer keeps the buffer it has, so a
    // rebuild that did not change the shape costs a map and a copy.
    if (!vb->allocateBuffer(static_cast<U32>(verts.size()), 0))
    {
        vb = nullptr;
        return;
    }
    LLStrider<LLVector3> positions;
    LLStrider<LLVector2> texcoords;
    LLStrider<LLColor4U> vertex_colors;
    if (!vb->getVertexStrider(positions) || !vb->getTexCoord0Strider(texcoords) || !vb->getColorStrider(vertex_colors))
    {
        vb = nullptr;
        return;
    }
    const U32 count = static_cast<U32>(verts.size());
    for (U32 i = 0; i < count; ++i)
    {
        positions[i] = verts[i];
        // The white texture samples anywhere; the shader reads the coord.
        texcoords[i] = LLVector2(0.f, 0.f);
        LLColor4 c = colors[i];
        c.mV[VALPHA] = faded(c.mV[VALPHA], verts[i]);
        vertex_colors[i].setVecScaleClamp(c);
    }
    vb->unmapBuffer();
}

void SKNavOverlayGeom::upload()
{
    uploadBatch(mTriVerts, mTriColors, mTriVB);
    uploadBatch(mEdgeVerts, mEdgeColors, mEdgeVB);
    uploadBatch(mLineVerts, mLineColors, mLineVB);
    mUploaded = true;
}

void SKNavOverlayGeom::bindShader()
{
    gGL.flush();
    gUIProgram.bind();
    gGL.getTextureSlot(0)->bindSampled(LLViewerFetchedTexture::sWhiteImagep, ALSamplers::AnisoWrap);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.texCoord2f(0.f, 0.f);
}

void SKNavOverlayGeom::unbindShader()
{
    gGL.flush();
    gUIProgram.unbind();
}

void SKNavOverlayGeom::draw(F32 shade, F32 alpha) const
{
    const bool ribboned = mEdgeWidth > 1.f && mPixelScale > 0.f;
    // Anything scaled is the x-ray pass, and gUIProgram has no batch uniform
    // to scale a whole draw by, so that pass re-emits the gathered colours as
    // it always did. At full strength the buffers upload() built are one call
    // each and nothing per vertex on the CPU.
    if (mUploaded && shade == 1.f && alpha == 1.f)
    {
        drawBatch(mTriVB, LLRender::TRIANGLES);
        drawBatch(mEdgeVB, ribboned ? LLRender::TRIANGLES : LLRender::LINES);
        drawBatch(mLineVB, LLRender::LINES);
        return;
    }

    auto emit = [&](const std::vector<LLVector3>& verts, const std::vector<LLColor4>& colors, U32 mode) {
        if (verts.empty())
        {
            return;
        }
        gGL.begin(mode);
        for (size_t i = 0; i < verts.size(); ++i)
        {
            const LLColor4& c = colors[i];
            gGL.color4f(c.mV[VRED] * shade, c.mV[VGREEN] * shade, c.mV[VBLUE] * shade,
                        faded(c.mV[VALPHA] * alpha, verts[i]));
            gGL.vertex3fv(verts[i].mV);
        }
        gGL.end();
        gGL.flush();
    };
    emit(mTriVerts, mTriColors, LLRender::TRIANGLES);
    emit(mEdgeVerts, mEdgeColors, ribboned ? LLRender::TRIANGLES : LLRender::LINES);
    emit(mLineVerts, mLineColors, LLRender::LINES);
}
