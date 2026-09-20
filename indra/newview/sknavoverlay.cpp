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
#include "llviewershadermgr.h"
#include "llviewertexture.h"

void SKNavOverlayGeom::line(const LLVector3& a, const LLVector3& b, const LLColor4& color)
{
    mLineVerts.push_back(a);
    mLineVerts.push_back(b);
    mLineColors.push_back(color);
    mLineColors.push_back(color);
}

void SKNavOverlayGeom::edge(const LLVector3& a, const LLVector3& b, const LLColor4& color)
{
    mEdgeVerts.push_back(a);
    mEdgeVerts.push_back(b);
    mEdgeColors.push_back(color);
    mEdgeColors.push_back(color);
}

void SKNavOverlayGeom::tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color)
{
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
    mTriVerts.clear();
    mTriColors.clear();
    mLineVerts.clear();
    mLineColors.clear();
    mEdgeVerts.clear();
    mEdgeColors.clear();
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

void SKNavOverlayGeom::draw(F32 width, F32 shade, F32 alpha) const
{
    auto emit = [&](const std::vector<LLVector3>& verts, const std::vector<LLColor4>& colors, U32 mode) {
        if (verts.empty())
        {
            return;
        }
        gGL.begin(mode);
        for (size_t i = 0; i < verts.size(); ++i)
        {
            const LLColor4& c = colors[i];
            gGL.color4f(c.mV[VRED] * shade, c.mV[VGREEN] * shade, c.mV[VBLUE] * shade, c.mV[VALPHA] * alpha);
            gGL.vertex3fv(verts[i].mV);
        }
        gGL.end();
        gGL.flush();
    };
    emit(mTriVerts, mTriColors, LLRender::TRIANGLES);
    // The driver has the last word on width: LLRender clamps to the aliased
    // line range, which is [1, 1] on some of them.
    gGL.setLineWidth(width);
    emit(mEdgeVerts, mEdgeColors, LLRender::LINES);
    gGL.setLineWidth(llmax(1.f, width - 1.f));
    emit(mLineVerts, mLineColors, LLRender::LINES);
    gGL.setLineWidth(1.f);
}
