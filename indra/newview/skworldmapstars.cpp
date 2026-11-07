/**
 * @file skworldmapstars.cpp
 * @brief The stars past the 3D world map's edge: the sky's own star field, fixed in the map's frame.
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

#include "skworldmapstars.h"

#include "alstarfield.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"

namespace
{
    // Far past everything the map draws and inside its far plane; only their direction matters.
    constexpr F32 STAR_DISTANCE = 5.0e6f;
}

SKWorldMapStars::SKWorldMapStars() = default;

SKWorldMapStars::~SKWorldMapStars()
{
    release();
}

void SKWorldMapStars::release()
{
    mBuffer = nullptr;
    mCount = 0;
    mVertices = 0;
}

bool SKWorldMapStars::build(U32 count)
{
    // The dome's radius only places the tiers; the directions are what is kept.
    const std::vector<ALStarField::Star> stars = ALStarField::generate(count, 1.f);
    if (stars.empty())
    {
        return false;
    }

    LLPointer<LLVertexBuffer> buffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_COLOR);
    const U32 vertices = static_cast<U32>(stars.size()) * 2;
    if (!buffer->allocateBuffer(vertices, 0))
    {
        return false;
    }
    LLStrider<LLVector3> pos;
    LLStrider<LLColor4U> colour;
    buffer->getVertexStrider(pos);
    buffer->getColorStrider(colour);
    for (const ALStarField::Star& star : stars)
    {
        LLVector3 d = star.mPosition;
        d.normalize();
        // A point is one pixel here, so a star's intensity goes into its brightness.
        const F32 k = 0.25f + 0.75f * star.mColour.mV[VALPHA] / 255.f;
        const LLColor4U c(static_cast<U8>(star.mColour.mV[VRED] * k), static_cast<U8>(star.mColour.mV[VGREEN] * k),
                          static_cast<U8>(star.mColour.mV[VBLUE] * k), 255);
        *pos++ = d * STAR_DISTANCE;
        *colour++ = c;
        *pos++ = LLVector3(d.mV[VX], d.mV[VY], -d.mV[VZ]) * STAR_DISTANCE;
        *colour++ = c;
    }
    buffer->unmapBuffer();

    mBuffer = buffer;
    mVertices = vertices;
    return true;
}

void SKWorldMapStars::draw()
{
    static LLCachedControl<S32> star_count(gSavedSettings, "RenderStarCount", (S32)ALStarField::DEFAULT_COUNT);
    const U32 count = static_cast<U32>(llclamp((S32)star_count, 0, (S32)ALStarField::MAX_COUNT));
    if (count == 0)
    {
        release();
        return;
    }
    if (count != mCount)
    {
        release();
        mCount = count;
        build(count);
    }
    if (mBuffer.isNull())
    {
        return;
    }

    gUIProgram.bind();
    gGL.getTextureSlot(0)->unbind();
    gGL.syncMatrices();
    mBuffer->setBuffer();
    mBuffer->drawArrays(LLRender::POINTS, 0, mVertices);
    gUIProgram.unbind();
}
