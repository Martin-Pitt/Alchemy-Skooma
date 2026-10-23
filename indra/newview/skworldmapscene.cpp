/**
 * @file skworldmapscene.cpp
 * @brief The 3D world map's own pass: the ground of map tiles, a light tilt-shift, and the agent dots.
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

#include "skworldmapscene.h"

#include "llagent.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llregionhandle.h"
#include "llui.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworldmap.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

using namespace skworldmap;

namespace
{
    // The map server's open water, so missing tiles read as sea rather than as holes.
    const LLColor4 SEA_COLOUR(0.114f, 0.278f, 0.373f, 1.f);

    // The combat mini-map's people dots, so both maps read the same.
    const LLColor4 DOT_COLOUR(0x60 / 255.f, 0xdf / 255.f, 0x72 / 255.f, 1.f);
    const LLColor4 SELF_COLOUR(1.f, 1.f, 1.f, 1.f);
    const LLColor4 SHADOW_COLOUR(0.f, 0.f, 0.f, 0.55f);
    constexpr F32 DOT_RADIUS_PX = 3.f;
    constexpr F32 SELF_RADIUS_PX = 3.4f;
    constexpr F32 SHADOW_EXTRA_PX = 1.6f;
    constexpr F32 STACK_STEP_PX = 5.5f;

    // Tilt-shift: a sharp band through the focus, blurring over the ramp beyond it.
    constexpr F32 SHARP_HALF_BAND = 0.10f;
    constexpr F32 BLUR_RAMP = 0.32f;
    constexpr F32 BLUR_STRENGTH = 0.9f;
    constexpr S32 BLUR_BANDS = 32;

    struct Projector
    {
        LLVector3d mEye, mForward, mRight, mUp;
        F64 mFocal = 1.0;
        F64 mHalfW = 0.0, mHalfH = 0.0;

        Projector(const SKWorldMapFrame& frame, S32 width, S32 height)
        {
            viewBasis(frame.mYaw, frame.mPitch, mForward, mRight, mUp);
            mEye = frame.mEye;
            mHalfW = width * 0.5;
            mHalfH = height * 0.5;
            mFocal = mHalfH / std::tan(frame.mFovY * 0.5);
        }

        /// Window pixels from the world view's bottom left, and the depth along the look; false behind the eye.
        bool project(const LLVector3d& p, F32& x, F32& y, F64& depth) const
        {
            const LLVector3d rel = p - mEye;
            depth = rel * mForward;
            if (depth <= 1.0e-3)
            {
                return false;
            }
            x = static_cast<F32>(mHalfW + mFocal * (rel * mRight) / depth);
            y = static_cast<F32>(mHalfH + mFocal * (rel * mUp) / depth);
            return true;
        }

        /// World up in view space (x right, y up, z toward the viewer).
        LLVector3d upInView() const { return LLVector3d(mRight.mdV[VZ], mUp.mdV[VZ], -mForward.mdV[VZ]); }
    };

    void disc(F32 x, F32 y, F32 radius, const LLColor4& colour)
    {
        constexpr S32 STEPS = 14;
        gGL.color4fv(colour.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS;
            const F32 a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.vertex2f(x, y);
            gGL.vertex2f(x + radius * std::cos(a0), y + radius * std::sin(a0));
            gGL.vertex2f(x + radius * std::cos(a1), y + radius * std::sin(a1));
        }
        gGL.end();
    }

    void ring(F32 x, F32 y, F32 radius, const LLColor4& colour)
    {
        constexpr S32 STEPS = 14;
        gGL.color4fv(colour.mV);
        gGL.begin(LLRender::LINES);
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS;
            const F32 a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.vertex2f(x + radius * std::cos(a0), y + radius * std::sin(a0));
            gGL.vertex2f(x + radius * std::cos(a1), y + radius * std::sin(a1));
        }
        gGL.end();
    }

    void quad01(F32 alpha)
    {
        gGL.color4f(1.f, 1.f, 1.f, alpha);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(0.f, 0.f);
        gGL.texCoord2f(1.f, 0.f);
        gGL.vertex2f(1.f, 0.f);
        gGL.texCoord2f(0.f, 1.f);
        gGL.vertex2f(0.f, 1.f);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(1.f, 1.f);
        gGL.end();
    }

    void pushOrtho(F32 width, F32 height)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, width, 0.f, height, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
    }

    void popMatrices()
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
    }
} // namespace

bool SKWorldMapScene::allocateTargets(S32 width, S32 height)
{
    const U32 w = static_cast<U32>(width);
    const U32 h = static_cast<U32>(height);
    if (mScene.getWidth() != w || mScene.getHeight() != h)
    {
        if (!mScene.allocate(w, h, GL_RGBA8)
            || !mBlurA.allocate(std::max(w / 2, 1u), std::max(h / 2, 1u), GL_RGBA8)
            || !mBlurB.allocate(std::max(w / 2, 1u), std::max(h / 2, 1u), GL_RGBA8))
        {
            release();
            return false;
        }
    }
    return true;
}

void SKWorldMapScene::render(const SKWorldMapFrame& frame, const LLRect& rect, F64 now)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("world map");

    const S32 width = rect.getWidth();
    const S32 height = rect.getHeight();
    if (width <= 0 || height <= 0 || !allocateTargets(width, height))
    {
        return;
    }

    View view;
    view.mEye = frame.mEye;
    view.mYaw = frame.mYaw;
    view.mPitch = frame.mPitch;
    view.mFovY = frame.mFovY;
    view.mAspect = static_cast<F64>(width) / height;
    view.mScreenHeight = height;
    mSelected = selectTiles(view);

    pollAgents(frame, now);

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);

    drawGround(frame, width, height, now);
    mTiles.update(now);
    tiltShift(frame, rect);
    drawAgents(frame, rect);

    // Leave the viewport as the UI expects it.
    gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
}

void SKWorldMapScene::drawGround(const SKWorldMapFrame& frame, S32 width, S32 height, F64 now)
{
    LLVector3d f, r, u;
    viewBasis(frame.mYaw, frame.mPitch, f, r, u);
    const F64 height_above = std::max(frame.mEye.mdV[VZ] - GROUND_Z, 1.0);
    const F32 z_near = static_cast<F32>(std::max(0.5, 0.25 * height_above));

    mScene.bindTarget();
    const LLColor4 saved_clear = gGL.getClearColor();
    gGL.setClearColor(LLColor4(SEA_COLOUR.mV[VRED], SEA_COLOUR.mV[VGREEN], SEA_COLOUR.mV[VBLUE], 1.f));
    mScene.clear(GL_COLOR_BUFFER_BIT);
    gGL.setClearColor(saved_clear);

    LLGLDisable blend(GL_BLEND);

    // Vertices are relative to the eye, so single precision holds across the whole grid.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(al_perspective(static_cast<F32>(frame.mFovY), static_cast<F32>(width) / height, z_near, 1.0e7f));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    LLVector4a eye, at, up;
    eye.clear();
    at.load3(LLVector3(f).mV);
    up.load3(LLVector3(u).mV);
    gGL.loadMatrix(LLMatrix4a::lookAt(eye, at, up));

    gUIProgram.bind();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    for (const SelectedTile& tile : mSelected)
    {
        SKWorldMapTiles::Draw draw;
        bool found = false;
        if (mSurfaces && tile.mKey.mLevel == 1)
        {
            const U64 handle = to_region_handle(static_cast<U32>(tile.mKey.mGridX * REGION_METRES), static_cast<U32>(tile.mKey.mGridY * REGION_METRES));
            draw.mTexture = mSurfaces->surfaceFor(handle);
            found = draw.mTexture != nullptr;
        }
        if (!found)
        {
            found = mTiles.resolve(tile, now, draw);
        }
        if (!found)
        {
            continue;
        }

        const F64 x0 = tile.mKey.mGridX * REGION_METRES - frame.mEye.mdV[VX];
        const F64 y0 = tile.mKey.mGridY * REGION_METRES - frame.mEye.mdV[VY];
        const F64 w = tile.mKey.sideMetres();
        const F32 z = static_cast<F32>(GROUND_Z - frame.mEye.mdV[VZ]);
        const F32 xa = static_cast<F32>(x0), xb = static_cast<F32>(x0 + w);
        const F32 ya = static_cast<F32>(y0), yb = static_cast<F32>(y0 + w);

        gGL.getTextureSlot(0)->bindSampled(draw.mTexture, ALSamplers::AnisoClamp);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.texCoord2f(draw.mUV[0], draw.mUV[1]);
        gGL.vertex3f(xa, ya, z);
        gGL.texCoord2f(draw.mUV[2], draw.mUV[1]);
        gGL.vertex3f(xb, ya, z);
        gGL.texCoord2f(draw.mUV[0], draw.mUV[3]);
        gGL.vertex3f(xa, yb, z);
        gGL.texCoord2f(draw.mUV[2], draw.mUV[3]);
        gGL.vertex3f(xb, yb, z);
        gGL.end();
    }
    gGL.flush();
    gGL.getTextureSlot(0)->unbind();
    gUIProgram.unbind();

    popMatrices();
    mScene.flush();
}

void SKWorldMapScene::tiltShift(const SKWorldMapFrame& frame, const LLRect& rect)
{
    // Alchemy's depth of field is driven by scene depth and the world camera's optics inside the deferred
    // post chain, on targets renderFinalize owns; the map has neither. A tilt-shift is a band in screen
    // space, so this blurs once with the probe gaussian and blends the sharp map over it by height.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();
    {
        LLGLDisable blend(GL_BLEND);
        gGaussianProgram.bind();
        const S32 channel = gGaussianProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        const F32 bw = static_cast<F32>(mBlurA.getWidth());
        const F32 bh = static_cast<F32>(mBlurA.getHeight());
        struct Pass
        {
            LLRenderTarget* mFrom;
            LLRenderTarget* mTo;
            F32 mDx, mDy, mScale;
        };
        const Pass passes[] = {
            { &mScene, &mBlurA, 1.f, 0.f, 2.f / mScene.getWidth() },
            { &mBlurA, &mBlurB, 0.f, 1.f, 1.5f / bh },
            { &mBlurB, &mBlurA, 1.f, 0.f, 1.5f / bw },
            { &mBlurA, &mBlurB, 0.f, 1.f, 1.5f / bh },
        };
        for (const Pass& p : passes)
        {
            gGaussianProgram.uniform1f(LLShaderMgr::RES_SCALE, p.mScale);
            gGaussianProgram.uniform2f(LLShaderMgr::DIRECTION, p.mDx, p.mDy);
            gGL.getTextureSlot(channel)->bind(p.mFrom);
            p.mTo->bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            p.mTo->flush();
        }
        gGL.getTextureSlot(channel)->unbind();
        gGaussianProgram.unbind();
    }
    popMatrices();

    // The band follows the orbit's focus on screen, the middle of the screen for the fly camera.
    F32 focus_y = 0.5f;
    if (frame.mHasFocus)
    {
        const Projector proj(frame, rect.getWidth(), rect.getHeight());
        F32 x, y;
        F64 depth;
        if (proj.project(frame.mFocus, x, y, depth))
        {
            focus_y = llclamp(y / rect.getHeight(), 0.15f, 0.85f);
        }
    }
    // Looking straight down there is no near and far to tell apart, so the effect fades out.
    const F32 strength = BLUR_STRENGTH * llclamp(2.f * static_cast<F32>(std::cos(frame.mPitch)), 0.f, 1.f);

    gGL.setViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    pushOrtho(1.f, 1.f);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    {
        LLGLDisable blend(GL_BLEND);
        gGL.getTextureSlot(0)->bind(&mBlurB);
        quad01(1.f);
        gGL.flush();
    }
    {
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.getTextureSlot(0)->bind(&mScene);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        for (S32 i = 0; i <= BLUR_BANDS; ++i)
        {
            const F32 y = static_cast<F32>(i) / BLUR_BANDS;
            const F32 t = llclamp((std::fabs(y - focus_y) - SHARP_HALF_BAND) / BLUR_RAMP, 0.f, 1.f);
            const F32 blur = strength * t * t * (3.f - 2.f * t);
            gGL.color4f(1.f, 1.f, 1.f, 1.f - blur);
            gGL.texCoord2f(0.f, y);
            gGL.vertex2f(0.f, y);
            gGL.texCoord2f(1.f, y);
            gGL.vertex2f(1.f, y);
        }
        gGL.end();
        gGL.flush();
    }
    gGL.getTextureSlot(0)->unbind();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    popMatrices();
}

void SKWorldMapScene::pollAgents(const SKWorldMapFrame& frame, F64 now)
{
    // The nearest regions under fine enough tiles, as LLWorldMapView polls the regions it shows.
    std::vector<std::pair<F64, U64>> regions;
    S32 x0 = S32_MAX, y0 = S32_MAX, x1 = S32_MIN, y1 = S32_MIN;
    for (const SelectedTile& t : mSelected)
    {
        if (t.mKey.mLevel > POLL_MAX_LEVEL)
        {
            continue;
        }
        const S32 n = t.mKey.regionsPerSide();
        for (S32 i = 0; i < n; ++i)
        {
            for (S32 j = 0; j < n; ++j)
            {
                const S32 gx = t.mKey.mGridX + i;
                const S32 gy = t.mKey.mGridY + j;
                const LLVector3d centre((gx + 0.5) * REGION_METRES, (gy + 0.5) * REGION_METRES, GROUND_Z);
                regions.emplace_back((centre - frame.mEye).magVecSquared(),
                                     to_region_handle(static_cast<U32>(gx * REGION_METRES), static_cast<U32>(gy * REGION_METRES)));
            }
        }
    }
    const size_t keep = std::min(regions.size(), static_cast<size_t>(POLLED_REGIONS));
    std::partial_sort(regions.begin(), regions.begin() + keep, regions.end());
    regions.resize(keep);

    mPolled.clear();
    for (const auto& [d2, handle] : regions)
    {
        U32 x, y;
        from_region_handle(handle, &x, &y);
        const S32 gx = static_cast<S32>(x / REGION_METRES);
        const S32 gy = static_cast<S32>(y / REGION_METRES);
        x0 = std::min(x0, gx);
        y0 = std::min(y0, gy);
        x1 = std::max(x1, gx);
        y1 = std::max(y1, gy);
        mPolled.push_back(handle);
    }
    if (mPolled.empty())
    {
        return;
    }

    LLWorldMap* world_map = LLWorldMap::getInstance();
    world_map->updateRegions(x0, y0, x1, y1);
    for (U64 handle : mPolled)
    {
        if (LLSimInfo* info = world_map->simInfoFromHandle(handle))
        {
            info->updateAgentCount(now);
        }
    }
}

void SKWorldMapScene::drawAgents(const SKWorldMapFrame& frame, const LLRect& rect)
{
    const Projector proj(frame, rect.getWidth(), rect.getHeight());
    const LLVector2 dir = stackDirection(proj.upInView());
    const F32 scale = LLUI::getScaleFactor().mV[VY];
    const F32 step = STACK_STEP_PX * scale;

    struct Cluster
    {
        F32 mX, mY;
        S32 mCount;
    };
    std::vector<Cluster> clusters;
    std::vector<F64> distances;
    std::vector<S32> counts;
    LLWorldMap* world_map = LLWorldMap::getInstance();
    for (U64 handle : mPolled)
    {
        const LLSimInfo* info = world_map->simInfoFromHandle(handle);
        if (!info || info->isDown())
        {
            continue;
        }
        for (const LLItemInfo& item : info->getAgentLocation())
        {
            LLVector3d p = item.getGlobalPosition();
            p.mdV[VZ] = GROUND_Z;
            F32 x, y;
            F64 depth;
            if (!proj.project(p, x, y, depth) || x < -8.f || y < -8.f || x > rect.getWidth() + 8.f || y > rect.getHeight() + 8.f)
            {
                continue;
            }
            clusters.push_back({ x, y, item.getCount() });
            distances.push_back(depth);
            counts.push_back(item.getCount());
        }
    }
    const std::vector<S32> shown = stackBudget(distances, counts, DOTS_PER_STACK, DOT_BUDGET);

    gGL.setViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    pushOrtho(static_cast<F32>(rect.getWidth()), static_cast<F32>(rect.getHeight()));
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    gGL.getTextureSlot(0)->unbind();

    // Farthest first, so nearer stacks draw over them.
    std::vector<size_t> order(clusters.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distances[a] > distances[b]; });
    for (size_t i : order)
    {
        const Cluster& c = clusters[i];
        for (S32 k = 0; k < shown[i]; ++k)
        {
            const F32 x = c.mX + dir.mV[VX] * step * k;
            const F32 y = c.mY + dir.mV[VY] * step * k;
            disc(x, y, (DOT_RADIUS_PX + SHADOW_EXTRA_PX) * scale, SHADOW_COLOUR);
            disc(x, y, DOT_RADIUS_PX * scale, DOT_COLOUR);
        }
        if (shown[i] > 0 && c.mCount > shown[i])
        {
            const S32 top = shown[i] - 1;
            ring(c.mX + dir.mV[VX] * step * top, c.mY + dir.mV[VY] * step * top, (DOT_RADIUS_PX + 1.5f) * scale, SELF_COLOUR);
        }
    }

    // You, in white, wherever the camera is.
    LLVector3d self = gAgent.getPositionGlobal();
    self.mdV[VZ] = GROUND_Z;
    F32 sx, sy;
    F64 depth;
    if (proj.project(self, sx, sy, depth))
    {
        disc(sx, sy, (SELF_RADIUS_PX + SHADOW_EXTRA_PX) * scale, SHADOW_COLOUR);
        disc(sx, sy, SELF_RADIUS_PX * scale, SELF_COLOUR);
    }
    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    popMatrices();
}

void SKWorldMapScene::release()
{
    mScene.release();
    mBlurA.release();
    mBlurB.release();
    mTiles.dropPriorities();
    mPolled.clear();
    mSelected.clear();
}

void SKWorldMapScene::shutdown()
{
    release();
    mTiles.clear();
}
