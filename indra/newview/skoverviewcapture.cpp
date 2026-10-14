/**
 * @file skoverviewcapture.cpp
 * @brief The overview: the census as styled shapes over a ghost of the rendered world, for the mini-map's base and
 *        the tactical view.
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
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llviewerinput.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llcharacter.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llvoavatar.h"
#include "pipeline.h"
#include "sknavbuild.h"
#include "sknavmesh.h"
#include "skmappalette.h"
#include "skworldcensus.h"

#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    LLRenderTarget sCapture;  // depth, and a class code in R (skoverview::Code), an invisible face in B
    LLRenderTarget sResolved; // the map, RGBA8
    LLRenderTarget sTacticalCapture;
    LLRenderTarget sTacticalResolved;
    LLGLSLShader sResolve;
    LLGLSLShader sComposite;
    GLuint sQueries[2] = { 0, 0 };

    const F32 CENSUS_RANGE_M = 128.f;
    const F32 TACTICAL_NEAR_M = 0.5f;
    const F32 TACTICAL_MIN_M = 8.f;
    const F32 TACTICAL_MAX_M = 400.f;
    const F32 DOT_PX = 4.f;
    const F64 TACTICAL_TAP_S = 0.2;

    void uniformColour(GLint prog, const char* name, const skmap::PaletteColour& c)
    {
        glUniform3f(glGetUniformLocation(prog, name), c.mR, c.mG, c.mB);
    }

    LLVector3 toVec3(const LLVector4a& v)
    {
        return LLVector3(v.getF32ptr());
    }
}

SKOverviewCapture::SKOverviewCapture() = default;
SKOverviewCapture::~SKOverviewCapture() = default;

void SKOverviewCapture::onFrame()
{
    updateCapsLockCapture();
    static LLCachedControl<bool> enabled(gSavedSettings, "SKMapCaptureBase", false);
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    if (!enabled && !tactical)
    {
        if (instanceExists())
        {
            instance().updateDemand(false);
        }
        return;
    }
    instance().frame();
}

void SKOverviewCapture::updateCapsLockCapture()
{
    // Take Caps Lock from the OS only while the world has the keyboard and the tactical action sits on it bare, so
    // chat and editors still type capitals with it.
    static const std::string caps = LLKeyboard::stringFromAccelerator(MASK_NONE, KEY_CAPSLOCK);
    static U32 frame = 0;
    static bool bound = false;
    if (frame++ % 30 == 0)
    {
        const std::string keys = gViewerInput.getKeyBindingAsString("", "tactical_overview");
        bound = false;
        for (size_t at = 0; at <= keys.size();)
        {
            const size_t end = keys.find(" | ", at);
            if (keys.substr(at, end == std::string::npos ? std::string::npos : end - at) == caps)
            {
                bound = true;
                break;
            }
            if (end == std::string::npos)
            {
                break;
            }
            at = end + 3;
        }
    }
    const bool want = bound && gFocusMgr.getAppHasFocus() && !gFocusMgr.getKeyboardFocus();
    if (LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr)
    {
        window->skSetCapsLockCapture(want);
    }
}

bool SKOverviewCapture::handleScrollWheel(F32 clicks)
{
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    if (!tactical || clicks == 0.f)
    {
        return false;
    }
    const F32 distance = gSavedSettings.getF32("SKTacticalDistance");
    gSavedSettings.setF32("SKTacticalDistance", llclamp(distance * std::pow(1.15f, clicks), TACTICAL_MIN_M, TACTICAL_MAX_M));
    return true;
}

void SKOverviewCapture::handleTacticalKey(bool down)
{
    static F64 pressed_at = -1.0;
    static bool latched = false;
    static bool closing = false;
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (down)
    {
        pressed_at = now;
        closing = latched;
        latched = false;
        gSavedSettings.setBOOL("SKTacticalOverview", !closing);
        return;
    }
    if (closing || pressed_at < 0.0)
    {
        closing = false;
        return;
    }
    if (now - pressed_at < TACTICAL_TAP_S)
    {
        latched = true;
        return;
    }
    gSavedSettings.setBOOL("SKTacticalOverview", false);
}

void SKOverviewCapture::cleanupGL()
{
    if (instanceExists())
    {
        instance().releaseGL();
        return;
    }
    sCapture.release();
    sResolved.release();
    sTacticalCapture.release();
    sTacticalResolved.release();
    sResolve.unload();
    sComposite.unload();
}

void SKOverviewCapture::releaseGL()
{
    sCapture.release();
    sResolved.release();
    sTacticalCapture.release();
    sTacticalResolved.release();
    sResolve.unload();
    sComposite.unload();
    if (sQueries[0])
    {
        glDeleteQueries(2, sQueries);
        sQueries[0] = sQueries[1] = 0;
    }
    mCensus.release();
    mTiming = false;
    mValid = false;
    mGLFailed = false;
}

void SKOverviewCapture::updateDemand(bool wanted)
{
    if (wanted == mDemanding || !SKWorldCensus::instanceExists())
    {
        return;
    }
    if (wanted)
    {
        SKWorldCensus::instance().addDemand("overview", CENSUS_RANGE_M);
    }
    else
    {
        SKWorldCensus::instance().removeDemand("overview");
    }
    mDemanding = wanted;
}

U32 SKOverviewCapture::texture() const
{
    return mValid && sResolved.isComplete() ? sResolved.getTexture() : 0;
}

bool SKOverviewCapture::ensureShaders()
{
    bool ok = true;
    if (!sResolve.mProgramObject)
    {
        sResolve.mName = "SK Overview Resolve";
        sResolve.mShaderFiles.clear();
        sResolve.clearPermutations();
        sResolve.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        sResolve.mShaderFiles.push_back(std::make_pair("skooma/overviewResolveF.glsl", GL_FRAGMENT_SHADER));
        sResolve.mShaderLevel = 1;
        ok = sResolve.createShader();
        LL_INFOS_ONCE("SKOverview") << "overview resolve shader " << (ok ? "loaded" : "FAILED") << LL_ENDL;
    }
    if (ok && !sComposite.mProgramObject)
    {
        sComposite.mName = "SK Overview Composite";
        sComposite.mShaderFiles.clear();
        sComposite.clearPermutations();
        sComposite.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        sComposite.mShaderFiles.push_back(std::make_pair("skooma/overviewCompositeF.glsl", GL_FRAGMENT_SHADER));
        sComposite.mShaderLevel = 1;
        ok = sComposite.createShader();
        LL_INFOS_ONCE("SKOverview") << "overview composite shader " << (ok ? "loaded" : "FAILED") << LL_ENDL;
    }
    if (ok && !sQueries[0])
    {
        glGenQueries(2, sQueries);
    }
    return ok;
}

bool SKOverviewCapture::ensureGL(LLRenderTarget& capture, LLRenderTarget& resolved, S32 w, S32 h)
{
    if (mGLFailed)
    {
        return false;
    }
    if (capture.isComplete() && ((S32)capture.getWidth() != w || (S32)capture.getHeight() != h))
    {
        capture.release();
        resolved.release();
    }
    bool ok = capture.isComplete() ||
              capture.allocate(w, h, GL_RGBA8, true, false, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                               LLRenderTarget::DEPTH_FMT_32F);
    ok = ok && (resolved.isComplete() || resolved.allocate(w, h, GL_RGBA8));
    if (!ok)
    {
        LL_WARNS_ONCE("SKOverview") << "overview target " << w << "x" << h << " did not allocate" << LL_ENDL;
    }
    ok = ok && ensureShaders();
    if (!ok)
    {
        LL_WARNS("SKOverview") << "Overview could not allocate its targets or load its shaders; it is off until the GL "
                                  "context is rebuilt"
                               << LL_ENDL;
        mGLFailed = true;
        releaseGL();
        mGLFailed = true;
        return false;
    }
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

void SKOverviewCapture::cutHeights(F32& floor_z, F32& cut_z) const
{
    static LLCachedControl<F32> cut(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    const F32 agent_z = gAgent.getPositionAgent().mV[VZ];
    F32 nav_floor = 0.f;
    if (SKNavMesh::instanceExists() &&
        SKNavMesh::instance().floorUnder(gAgent.getPositionGlobal(), sknav::AgentClass::AVATAR, nav_floor))
    {
        // Global and agent heights are the same number.
        floor_z = nav_floor;
        cut_z = nav_floor + (F32)cut;
        return;
    }
    floor_z = agent_z;
    cut_z = agent_z + (F32)cut;
}

void SKOverviewCapture::frame()
{
    if (gCubeSnapshot || !LLPipeline::sRenderDeferred || !gPipeline.shadersLoaded() || !gAgent.getRegion())
    {
        LL_INFOS_ONCE("SKOverview") << "overview waits: cube snapshot " << gCubeSnapshot << ", deferred "
                                    << LLPipeline::sRenderDeferred << ", shaders " << gPipeline.shadersLoaded() << ", region "
                                    << (gAgent.getRegion() != nullptr) << LL_ENDL;
        return;
    }
    LL_INFOS_ONCE("SKOverview") << "overview running" << LL_ENDL;
    updateDemand(true);
    readTimer();
    {
        LL_PROFILE_ZONE_NAMED("sk overview census update");
        mCensus.update();
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "SKMapCaptureBase", false);
    static LLCachedControl<bool> tactical_on(gSavedSettings, "SKTacticalOverview", false);
    static LLCachedControl<F32> refresh(gSavedSettings, "SKOverviewRefresh", 1.f);
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (enabled && (mLastCapture < 0.0 || now - mLastCapture >= llmax((F32)refresh, 0.05f)))
    {
        mLastCapture = now;
        captureMap();
    }
    if (tactical_on)
    {
        tactical();
    }
}

U32 SKOverviewCapture::render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot)
{
    static LLCachedControl<bool> ghost(gSavedSettings, "SKOverviewGhost", true);
    const S32 w = capture.getWidth();
    const S32 h = capture.getHeight();
    LLViewerRegion* region = gAgent.getRegion();

    S32 saved_viewport[4];
    memcpy(saved_viewport, gGLViewport, sizeof(saved_viewport));
    const U32 saved_res_x = LLRenderTarget::sCurResX;
    const U32 saved_res_y = LLRenderTarget::sCurResY;
    gGLViewport[0] = gGLViewport[1] = 0;
    gGLViewport[2] = w;
    gGLViewport[3] = h;

    LLCamera cull = shot.mCull;
    LLViewerCamera::updateFrustumPlanes(cull, false, false, true);

    // The census, region by region, a class a range, nearer than the ghost where they coincide.
    const std::vector<SKOverviewCensus::Batch>& batches = mCensus.batches();
    std::function<void()> census;
    if (!batches.empty())
    {
        census = [&batches, &shot]()
        {
            LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
            gGL.setPolygonOffset(-1.f, -4.f);
            for (S32 pass = 0; pass < 2; ++pass)
            {
                // Front: every face, depth written; back: inside a solid the cut went through.
                LLGLDepthTest depth(GL_TRUE, pass == 0 ? GL_TRUE : GL_FALSE, GL_LEQUAL);
                LLGLState cull_face(GL_CULL_FACE, pass == 1);
                glCullFace(pass == 1 ? GL_FRONT : GL_BACK);
                for (const SKOverviewCensus::Batch& batch : batches)
                {
                    const LLVector3 offset_agent = gAgent.getPosAgentFromGlobal(batch.mOrigin);
                    gGL.loadMatrix(shot.mView);
                    gGL.translatef(offset_agent.mV[VX], offset_agent.mV[VY], offset_agent.mV[VZ]);
                    gGLLastMatrix = NULL;
                    batch.mBuffer->setBuffer();
                    for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
                    {
                        const U32 count = batch.mStart[c + 1] - batch.mStart[c];
                        if (!count)
                        {
                            continue;
                        }
                        const U8 code = pass == 0 ? skoverview::codeOf((skoverview::CensusClass)c) : (U8)skoverview::CODE_SECTION;
                        gGL.diffuseColor4f(code / 255.f, 0.f, 1.f, 1.f);
                        batch.mBuffer->drawArrays(LLRender::TRIANGLES, batch.mStart[c], count);
                    }
                }
                glCullFace(GL_BACK);
            }
            gGL.setPolygonOffset(0.f, 0.f);
        };
    }

    U32 draws = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview capture");
        capture.bindTarget();
        glViewport(0, 0, w, h);
        GLfloat clear_colour[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_colour);
        GLfloat clear_depth = 1.f;
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClearDepth(al_window_far()); // the cleared depth must be the far plane of the current convention
        capture.clear();
        glClearDepth(clear_depth);
        glClearColor(clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3]);
        draws = gPipeline.skRenderOverview(shot.mView, shot.mProj, cull, shot.mCut, ghost, census);
        capture.flush();
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview resolve");
        LL_PROFILE_GPU_ZONE("sk overview resolve");
        resolved.bindTarget();
        glViewport(0, 0, w, h);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull_face(GL_CULL_FACE);
        sResolve.bind();
        const GLint prog = sResolve.mProgramObject;
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, capture.getDepth(), gGL.getSampler(ALSamplers::PointClamp));
        gGL.getTextureSlot(1)->bindManual(ALTextureSlot::TT_TEXTURE, capture.getTexture(), gGL.getSampler(ALSamplers::PointClamp));
        glUniform1i(glGetUniformLocation(prog, "depthMap"), 0);
        glUniform1i(glGetUniformLocation(prog, "classMap"), 1);
        LLMatrix4a inv_vp;
        inv_vp.setMul(shot.mView, shot.mProj);
        inv_vp.invert();
        glUniformMatrix4fv(glGetUniformLocation(prog, "sk_inv_vp"), 1, GL_FALSE, inv_vp.getF32ptr());
        glUniform1f(glGetUniformLocation(prog, "sk_zo"), LLRender::sReverseZ ? 1.f : 0.f);
        glUniform1f(glGetUniformLocation(prog, "sk_far_depth"), al_window_far());
        glUniform3f(glGetUniformLocation(prog, "sk_eye"), shot.mEye.mV[VX], shot.mEye.mV[VY], shot.mEye.mV[VZ]);
        glUniform1f(glGetUniformLocation(prog, "sk_top_down"), shot.mTopDown ? 1.f : 0.f);
        glUniform1f(glGetUniformLocation(prog, "sk_agent_z"), shot.mFloor);
        glUniform1f(glGetUniformLocation(prog, "sk_cut"), shot.mCut);
        glUniform1f(glGetUniformLocation(prog, "sk_water_z"), region->getWaterHeight());
        glUniform1f(glGetUniformLocation(prog, "sk_water_depth"), skmap::palette::WATER_DEPTH_M);
        uniformColour(prog, "sk_invisible", skmap::palette::DROP_EDGE);
        const LLVector3 region_min = region->getOriginAgent();
        const F32 region_width = region->getWidth();
        glUniform4f(glGetUniformLocation(prog, "sk_region"), region_min.mV[VX], region_min.mV[VY],
                    region_min.mV[VX] + region_width, region_min.mV[VY] + region_width);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTextureSlot(1)->unbind();
        gGL.getTextureSlot(0)->unbind();
        sResolve.unbind();
        resolved.flush();
    }

    memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;
    return draws;
}

bool SKOverviewCapture::captureMap()
{
    static LLCachedControl<U32> res_setting(gSavedSettings, "SKOverviewResolution", 1024);
    const S32 res = llclamp((S32)(U32)res_setting, 64, 4096);
    if (!ensureGL(sCapture, sResolved, res, res))
    {
        return false;
    }

    LLTimer cpu_timer;
    LLViewerRegion* region = gAgent.getRegion();
    const F32 width = region->getWidth();
    const LLVector3 origin = region->getOriginAgent();
    F32 floor_z, top;
    cutHeights(floor_z, top);
    const F32 bottom = llmin(top - 64.f, -32.f);
    const LLVector3 centre(origin.mV[VX] + width * 0.5f, origin.mV[VY] + width * 0.5f, top);

    // Straight down, east to the right and north up; the ortho near plane at the eye is the cut as well.
    Shot shot;
    LLVector4a eye4, dir4, up4;
    eye4.load3(centre.mV);
    dir4.set(0.f, 0.f, -1.f, 0.f);
    up4.set(0.f, 1.f, 0.f, 0.f);
    shot.mView = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 half = width * 0.5f;
    shot.mProj = al_ortho(-half, half, -half, half, 0.f, top - bottom);
    shot.mEye = centre + LLVector3(0.f, 0.f, 1000.f); // far above: outline tolerance as for a distant camera
    shot.mCut = top;
    shot.mFloor = floor_z;
    shot.mTopDown = true;

    // The cull camera: ortho planes for the partitions, and a perspective far enough above to hold the region for
    // the bridges, which rebuild their planes from origin and axes.
    const LLVector3 dir(0.f, 0.f, -1.f);
    const LLVector3 up(0.f, 1.f, 0.f);
    const F32 lift = width * 0.75f;
    shot.mCull = *LLViewerCamera::getInstance();
    shot.mCull.setView(F_PI_BY_TWO);
    shot.mCull.setAspect(1.f);
    shot.mCull.setNear(0.1f);
    shot.mCull.setFar(lift + (top - bottom) + 1.f);
    shot.mCull.setOrigin(centre + LLVector3(0.f, 0.f, lift));
    shot.mCull.setAxes(dir, up % dir, up);
    shot.mCull.setModelview(shot.mView);
    shot.mCull.setProjection(shot.mProj);

    if (!mTiming)
    {
        glQueryCounter(sQueries[0], GL_TIMESTAMP);
    }
    const U32 draws = render(sCapture, sResolved, shot);
    if (!mTiming)
    {
        glQueryCounter(sQueries[1], GL_TIMESTAMP);
        mTiming = true;
    }

    mOrigin = region->getOriginGlobal();
    mWidth = width;
    mAgentZ = floor_z;
    mTop = top;
    mBottom = bottom;
    mValid = true;
    mStats.resolution = res;
    mStats.draws = draws;
    mStats.census_tris = mCensus.triangles();
    mStats.cpu_ms = cpu_timer.getElapsedTimeF32() * 1000.f;
    ++mStats.captures;
    return true;
}

void SKOverviewCapture::tactical()
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<F32> distance_setting(gSavedSettings, "SKTacticalDistance", 60.f);
    static LLCachedControl<F32> pitch_setting(gSavedSettings, "SKTacticalPitch", 55.f);
    const S32 vx = gGLViewport[0], vy = gGLViewport[1], vw = gGLViewport[2], vh = gGLViewport[3];
    if (vw < 16 || vh < 16)
    {
        LL_WARNS_ONCE("SKOverview") << "tactical overview skipped: world viewport " << vw << "x" << vh << LL_ENDL;
        return;
    }
    if (!ensureGL(sTacticalCapture, sTacticalResolved, vw, vh))
    {
        LL_WARNS_ONCE("SKOverview") << "tactical overview skipped: targets or shaders unavailable" << LL_ENDL;
        return;
    }

    // Orbit the agent's floor at the main camera's heading.
    F32 floor_z, cut_z;
    cutHeights(floor_z, cut_z);
    LLVector3 target = gAgent.getPositionAgent();
    target.mV[VZ] = floor_z;
    LLVector3 heading = LLViewerCamera::getInstance()->getAtAxis();
    heading.mV[VZ] = 0.f;
    if (heading.normVec() < 1.0e-3f)
    {
        heading.setVec(1.f, 0.f, 0.f);
    }
    const F32 distance = llclamp((F32)distance_setting, TACTICAL_MIN_M, TACTICAL_MAX_M);
    const F32 pitch = llclamp((F32)pitch_setting, 5.f, 90.f) * DEG_TO_RAD;
    const F32 pitch_safe = llmin(pitch, 89.5f * DEG_TO_RAD);
    const LLVector3 eye = target - heading * (distance * std::cos(pitch_safe)) + LLVector3(0.f, 0.f, distance * std::sin(pitch_safe));
    LLVector3 dir = target - eye;
    dir.normVec();
    LLVector3 left = LLVector3::z_axis % dir;
    left.normVec();
    const LLVector3 up = dir % left;

    Shot shot;
    LLVector4a eye4, dir4, up4;
    eye4.load3(eye.mV);
    dir4.load3(dir.mV);
    up4.load3(up.mV);
    shot.mView = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 fov = LLViewerCamera::getInstance()->getView();
    const F32 aspect = (F32)vw / (F32)vh;
    const F32 far_m = distance + 2.f * CENSUS_RANGE_M;
    shot.mProj = al_perspective(fov, aspect, TACTICAL_NEAR_M, far_m);
    shot.mEye = eye;
    shot.mCut = cut_z;
    shot.mFloor = floor_z;
    shot.mTopDown = false;
    shot.mCull = *LLViewerCamera::getInstance();
    shot.mCull.setView(fov);
    shot.mCull.setAspect(aspect);
    shot.mCull.setNear(TACTICAL_NEAR_M);
    shot.mCull.setFar(far_m);
    shot.mCull.setOrigin(eye);
    shot.mCull.setAxes(dir, left, up);
    shot.mCull.setModelview(shot.mView);
    shot.mCull.setProjection(shot.mProj);

    {
        LL_PROFILE_ZONE_NAMED("sk tactical render");
        render(sTacticalCapture, sTacticalResolved, shot);
    }

    mTacticalView = shot.mView;
    mTacticalProj = shot.mProj;
    mTacticalViewport[0] = vx;
    mTacticalViewport[1] = vy;
    mTacticalViewport[2] = vw;
    mTacticalViewport[3] = vh;
    mTacticalReady = true;
    LL_INFOS_ONCE("SKOverview") << "tactical overview rendered " << vw << "x" << vh << ", census tris " << mCensus.triangles()
                                << LL_ENDL;
}


// The tactical overview over the finished frame: after renderFinalize, which would otherwise paint the world over it.
void SKOverviewCapture::onComposite()
{
    static LLCachedControl<bool> tactical_on(gSavedSettings, "SKTacticalOverview", false);
    if (!tactical_on || !instanceExists())
    {
        return;
    }
    instance().composite();
}

void SKOverviewCapture::composite()
{
    if (!mTacticalReady || !sComposite.mProgramObject || !sTacticalResolved.isComplete())
    {
        LL_WARNS_ONCE("SKOverview") << "tactical composite skipped: rendered " << mTacticalReady << ", shader "
                                    << (sComposite.mProgramObject != 0) << ", target " << sTacticalResolved.isComplete()
                                    << LL_ENDL;
        return;
    }
    mTacticalReady = false;
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<F32> tilt_setting(gSavedSettings, "SKTacticalTiltShift", 4.f);
    const S32 vx = mTacticalViewport[0], vy = mTacticalViewport[1], vw = mTacticalViewport[2], vh = mTacticalViewport[3];
    if (LLRenderTarget* bound = LLRenderTarget::getCurrentBoundTarget())
    {
        LL_WARNS_ONCE("SKOverview") << "tactical composite found a render target still bound after renderFinalize ("
                                    << bound->getWidth() << "x" << bound->getHeight() << "); drawing into it" << LL_ENDL;
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0); // the window's back buffer
    }
    LLGLDisable scissor(GL_SCISSOR_TEST);
    gGL.setColorMask(true, true);
    {
        LL_PROFILE_ZONE_NAMED("sk tactical composite");
        glViewport(vx, vy, vw, vh);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull_face(GL_CULL_FACE);
        sComposite.bind();
        const GLint prog = sComposite.mProgramObject;
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, sTacticalResolved.getTexture(),
                                          gGL.getSampler(ALSamplers::BilinearClamp));
        glUniform1i(glGetUniformLocation(prog, "overviewMap"), 0);
        glUniform4f(glGetUniformLocation(prog, "sk_viewport"), (F32)vx, (F32)vy, (F32)vw, (F32)vh);
        glUniform1f(glGetUniformLocation(prog, "sk_tilt_px"), llmax((F32)tilt_setting, 0.f));
        uniformColour(prog, "sk_ground", skmap::palette::CODE_INK);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTextureSlot(0)->unbind();
        sComposite.unbind();
    }

    {
        // Avatars as dots, the agent white; vehicles are left to the census's vehicle class.
        LL_PROFILE_ZONE_NAMED("sk tactical dots");
        LLMatrix4a vp;
        vp.setMul(mTacticalView, mTacticalProj);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGLLastMatrix = NULL;
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        gDebugProgram.bind();
        const F32 sx = 2.f * DOT_PX / (F32)vw, sy = 2.f * DOT_PX / (F32)vh;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->isUIAvatar())
            {
                continue;
            }
            LLVector4a pos, clip;
            pos.load3(avatar->getPositionAgent().mV);
            vp.perspectiveTransform(pos, clip);
            const LLVector3 ndc = toVec3(clip);
            if (ndc.mV[VX] < -1.f || ndc.mV[VX] > 1.f || ndc.mV[VY] < -1.f || ndc.mV[VY] > 1.f)
            {
                continue;
            }
            const skmap::PaletteColour& c = avatar->isSelf() ? skmap::palette::SELF_DOT : skmap::palette::DOT_GENERAL;
            gGL.diffuseColor4f(c.mR, c.mG, c.mB, 1.f);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex3f(ndc.mV[VX] - sx, ndc.mV[VY] - sy, 0.f);
            gGL.vertex3f(ndc.mV[VX] + sx, ndc.mV[VY] - sy, 0.f);
            gGL.vertex3f(ndc.mV[VX] + sx, ndc.mV[VY] + sy, 0.f);
            gGL.vertex3f(ndc.mV[VX] - sx, ndc.mV[VY] - sy, 0.f);
            gGL.vertex3f(ndc.mV[VX] + sx, ndc.mV[VY] + sy, 0.f);
            gGL.vertex3f(ndc.mV[VX] - sx, ndc.mV[VY] + sy, 0.f);
            gGL.end();
            gGL.flush();
        }
        gDebugProgram.unbind();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGLLastMatrix = NULL;
    }
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LL_INFOS_ONCE("SKOverview") << "tactical overview composited at " << vx << "," << vy << " " << vw << "x" << vh << LL_ENDL;
}
