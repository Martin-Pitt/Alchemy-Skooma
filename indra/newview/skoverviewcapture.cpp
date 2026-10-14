/**
 * @file skoverviewcapture.cpp
 * @brief A top-down render of the agent's region, shaded as a map, for the mini-map's base layer.
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

#include "skoverviewcapture.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "skmappalette.h"

#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    LLRenderTarget sCapture;  // depth, and the class flags: R terrain, G object, B invisible
    LLRenderTarget sResolved; // the map, RGBA8
    LLGLSLShader sResolve;
    GLuint sQueries[2] = { 0, 0 };
    S32 sResolution = 0;

    void uniformColour(GLint prog, const char* name, const skmap::PaletteColour& c)
    {
        glUniform3f(glGetUniformLocation(prog, name), c.mR, c.mG, c.mB);
    }
}

SKOverviewCapture::SKOverviewCapture() = default;
SKOverviewCapture::~SKOverviewCapture() = default;

void SKOverviewCapture::onFrame()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKMapCaptureBase", false);
    if (!enabled)
    {
        return;
    }
    instance().frame();
}

void SKOverviewCapture::cleanupGL()
{
    if (instanceExists())
    {
        instance().releaseGL();
    }
    else
    {
        sCapture.release();
        sResolved.release();
        sResolve.unload();
    }
}

void SKOverviewCapture::releaseGL()
{
    sCapture.release();
    sResolved.release();
    sResolve.unload();
    if (sQueries[0])
    {
        glDeleteQueries(2, sQueries);
        sQueries[0] = sQueries[1] = 0;
    }
    sResolution = 0;
    mTiming = false;
    mValid = false;
    mGLFailed = false;
}

U32 SKOverviewCapture::texture() const
{
    return mValid && sResolved.isComplete() ? sResolved.getTexture() : 0;
}

bool SKOverviewCapture::ensureGL(S32 res)
{
    if (mGLFailed)
    {
        return false;
    }
    if (res != sResolution)
    {
        sCapture.release();
        sResolved.release();
        mValid = false;
    }
    bool ok = sCapture.isComplete() ||
              sCapture.allocate(res, res, GL_RGBA8, true, false, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                                LLRenderTarget::DEPTH_FMT_32F);
    ok = ok && (sResolved.isComplete() || sResolved.allocate(res, res, GL_RGBA8));
    if (ok && !sResolve.mProgramObject)
    {
        sResolve.mName = "SK Overview Resolve";
        sResolve.mShaderFiles.clear();
        sResolve.clearPermutations();
        sResolve.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        sResolve.mShaderFiles.push_back(std::make_pair("skooma/overviewResolveF.glsl", GL_FRAGMENT_SHADER));
        sResolve.mShaderLevel = 1;
        ok = sResolve.createShader();
    }
    if (ok && !sQueries[0])
    {
        glGenQueries(2, sQueries);
    }
    if (!ok)
    {
        LL_WARNS("SKOverview") << "Overview capture could not allocate its targets or load its resolve shader; it is off until "
                                  "the GL context is rebuilt"
                               << LL_ENDL;
        mGLFailed = true;
        sCapture.release();
        sResolved.release();
        sResolve.unload();
        return false;
    }
    sResolution = res;
    return true;
}

void SKOverviewCapture::readTimer()
{
    if (!mTiming)
    {
        return;
    }
    GLint available = 0;
    glGetQueryObjectiv(sQueries[1], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
    {
        return;
    }
    GLuint64 t0 = 0, t1 = 0;
    glGetQueryObjectui64v(sQueries[0], GL_QUERY_RESULT, &t0);
    glGetQueryObjectui64v(sQueries[1], GL_QUERY_RESULT, &t1);
    mStats.gpu_ms = (F32)((F64)(t1 - t0) / 1.0e6);
    mTiming = false;
}

void SKOverviewCapture::frame()
{
    if (gCubeSnapshot || !LLPipeline::sRenderDeferred || !gPipeline.shadersLoaded() || !gAgent.getRegion())
    {
        return;
    }
    readTimer();
    static LLCachedControl<F32> refresh(gSavedSettings, "SKOverviewRefresh", 1.f);
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (mLastCapture >= 0.0 && now - mLastCapture < llmax((F32)refresh, 0.05f))
    {
        return;
    }
    mLastCapture = now;
    capture();
}

bool SKOverviewCapture::capture()
{
    static LLCachedControl<U32> res_setting(gSavedSettings, "SKOverviewResolution", 1024);
    static LLCachedControl<F32> cut(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    const S32 res = llclamp((S32)(U32)res_setting, 64, 4096);
    if (!ensureGL(res))
    {
        return false;
    }

    LLTimer cpu_timer;
    LLViewerRegion* region = gAgent.getRegion();
    const F32 width = region->getWidth();
    const LLVector3 origin = region->getOriginAgent();
    const F32 agent_z = gAgent.getPositionAgent().mV[VZ];
    const F32 top = agent_z + (F32)cut;
    const F32 bottom = llmin(top - 64.f, -32.f);
    const LLVector3 centre(origin.mV[VX] + width * 0.5f, origin.mV[VY] + width * 0.5f, top);

    // Straight down, east to the right and north up: view space is world space about the eye, so the ortho near
    // plane at the eye is the floor cut.
    LLVector4a eye4, dir4, up4;
    eye4.load3(centre.mV);
    dir4.set(0.f, 0.f, -1.f, 0.f);
    up4.set(0.f, 1.f, 0.f, 0.f);
    const LLMatrix4a view = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 half = width * 0.5f;
    const LLMatrix4a proj = al_ortho(-half, half, -half, half, 0.f, top - bottom);

    S32 saved_viewport[4];
    memcpy(saved_viewport, gGLViewport, sizeof(saved_viewport));
    const U32 saved_res_x = LLRenderTarget::sCurResX;
    const U32 saved_res_y = LLRenderTarget::sCurResY;
    gGLViewport[0] = gGLViewport[1] = 0;
    gGLViewport[2] = gGLViewport[3] = res;

    // The cull camera: ortho planes for the partitions, and a perspective far enough above to hold the region for
    // the bridges, which rebuild their planes from origin and axes.
    const LLVector3 dir(0.f, 0.f, -1.f);
    const LLVector3 up(0.f, 1.f, 0.f);
    const F32 lift = width * 0.75f;
    LLCamera camera = *LLViewerCamera::getInstance();
    camera.setView(F_PI_BY_TWO);
    camera.setAspect(1.f);
    camera.setNear(0.1f);
    camera.setFar(lift + (top - bottom) + 1.f);
    camera.setOrigin(centre + LLVector3(0.f, 0.f, lift));
    camera.setAxes(dir, up % dir, up);
    camera.setModelview(view);
    camera.setProjection(proj);
    LLViewerCamera::updateFrustumPlanes(camera, false, false, true);

    if (!mTiming)
    {
        glQueryCounter(sQueries[0], GL_TIMESTAMP);
    }

    U32 draws = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview capture");
        sCapture.bindTarget();
        glViewport(0, 0, res, res);
        GLfloat clear_colour[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_colour);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        sCapture.clear();
        glClearColor(clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3]);
        draws = gPipeline.skRenderOverview(view, proj, camera);
        sCapture.flush();
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview resolve");
        LL_PROFILE_GPU_ZONE("sk overview resolve");
        sResolved.bindTarget();
        glViewport(0, 0, res, res);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        sResolve.bind();
        const GLint prog = sResolve.mProgramObject;
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, sCapture.getDepth(), gGL.getSampler(ALSamplers::PointClamp));
        gGL.getTextureSlot(1)->bindManual(ALTextureSlot::TT_TEXTURE, sCapture.getTexture(), gGL.getSampler(ALSamplers::PointClamp));
        glUniform1i(glGetUniformLocation(prog, "depthMap"), 0);
        glUniform1i(glGetUniformLocation(prog, "classMap"), 1);
        glUniform4f(glGetUniformLocation(prog, "sk_depth"), al_window_near(), al_window_far(), top, bottom);
        glUniform1f(glGetUniformLocation(prog, "sk_mpp"), width / (F32)res);
        glUniform1f(glGetUniformLocation(prog, "sk_agent_z"), agent_z);
        glUniform1f(glGetUniformLocation(prog, "sk_water_z"), region->getWaterHeight());
        uniformColour(prog, "sk_floor", skmap::palette::MAT_OBJECT);
        uniformColour(prog, "sk_terrain", skmap::palette::MAT_TERRAIN);
        uniformColour(prog, "sk_water_shallow", skmap::palette::WATER_SHALLOW);
        uniformColour(prog, "sk_water_deep", skmap::palette::WATER_DEEP);
        uniformColour(prog, "sk_wall_full", skmap::palette::SECTION_FULL);
        uniformColour(prog, "sk_wall_half", skmap::palette::SECTION_HALF);
        uniformColour(prog, "sk_invisible", skmap::palette::DROP_EDGE);
        glUniform1f(glGetUniformLocation(prog, "sk_water_depth"), skmap::palette::WATER_DEPTH_M);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTextureSlot(1)->unbind();
        gGL.getTextureSlot(0)->unbind();
        sResolve.unbind();
        sResolved.flush();
    }

    if (!mTiming)
    {
        glQueryCounter(sQueries[1], GL_TIMESTAMP);
        mTiming = true;
    }

    memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;

    mOrigin = region->getOriginGlobal();
    mWidth = width;
    mAgentZ = agent_z;
    mTop = top;
    mBottom = bottom;
    mValid = true;
    mStats.resolution = res;
    mStats.draws = draws;
    mStats.cpu_ms = cpu_timer.getElapsedTimeF32() * 1000.f;
    ++mStats.captures;
    return true;
}
