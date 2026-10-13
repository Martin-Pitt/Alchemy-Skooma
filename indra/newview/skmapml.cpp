/**
 * @file skmapml.cpp
 * @brief The Mouselook map: the mini-map's content drawn faintly over the world while in mouselook.
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

#include "skmapml.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llui.h"
#include "llfloaterreg.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llworld.h"
#include "skfloatermap.h"
#include "sknetmap.h"

namespace
{
    constexpr U32 MAX_SIDE = 4096;
    constexpr U32 SIDE_STEP = 32;

    // One quad over the map's square; each layer is masked by its own fade, its colour premultiplied.
    const char* VS_MASK = R"(#version 330 core
uniform vec4 u_rect;
out vec2 v_t;
void main()
{
    v_t = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, v_t), 0.0, 1.0);
}
)";

    // The study's radial gradients: two linear segments meeting at a middle stop.
    const char* FS_MASK = R"(#version 330 core
in vec2 v_t;
out vec4 o;
uniform sampler2D u_t;
uniform int u_mode;
uniform float u_side;
uniform float u_outer;
uniform float u_centre;
uniform float u_self;
float ramp(float t, float a0, float tm, float am, float a1)
{
    t = clamp(t, 0.0, 1.0);
    return t < tm ? mix(a0, am, t / tm) : mix(am, a1, (t - tm) / (1.0 - tm));
}
void main()
{
    vec4 p = texture(u_t, v_t);
    float d = length((v_t - 0.5) * u_side);
    float k = 1.0;
    if (u_mode < 2)
    {
        float r = u_side * 0.45;
        float inner = r * (1.0 - u_outer);
        k *= ramp((d - inner) / max(r - inner, 1e-3), 1.0, 0.48, 0.58, 0.0);
    }
    if (u_mode == 0)
    {
        float r0 = u_centre * 0.18;
        k *= 1.0 - ramp((d - r0) / max(u_centre - r0, 1e-3), 0.92, 0.38, 0.70, 0.0);
    }
    else if (u_mode == 2)
    {
        float r0 = min(4.0, u_self * 0.3);
        k *= 1.0 - ramp((d - r0) / max(u_self - r0, 1e-3), 1.0, 0.5, 0.5, 0.0);
    }
    o = p * k;
}
)";

    GLuint compileStage(GLenum type, const char* src)
    {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
            LL_WARNS("SKMap") << "Mouselook map shader failed to compile: " << log << LL_ENDL;
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    U32 compileProgram()
    {
        const GLuint vs = compileStage(GL_VERTEX_SHADER, VS_MASK);
        const GLuint fs = compileStage(GL_FRAGMENT_SHADER, FS_MASK);
        if (!vs || !fs)
        {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return 0;
        }
        const GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            LL_WARNS("SKMap") << "Mouselook map shader failed to link" << LL_ENDL;
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    struct State
    {
        LLRenderTarget mTarget;
        U32 mProgram = 0;
        U32 mVao = 0;
        bool mFailed = false;       ///< the shader failed to build; nothing will draw
        GLint mLocRect = -1, mLocMode = -1, mLocSide = -1, mLocOuter = -1, mLocCentre = -1, mLocSelf = -1;
        U32 mDrawnFrame = 0;        ///< the frame the overlay last drew in, plus one
        F32 mHeading = 0.f;         ///< the last heading the camera gave, held while it looks straight up or down
        bool mIdleAdded = false;

        ~State()
        {
            mTarget.release();
            if (mProgram)
            {
                glDeleteProgram(mProgram);
            }
            if (mVao)
            {
                glDeleteVertexArrays(1, &mVao);
            }
        }
    };
    State* sState = nullptr;

    State& state()
    {
        if (!sState)
        {
            sState = new State();
        }
        return *sState;
    }

    struct Fades
    {
        F32 mOuter = 0.f;
        F32 mCentre = 0.f;
        F32 mSelf = 0.f;
    };

    bool wanted()
    {
        static LLCachedControl<bool> enable(gSavedSettings, "SKMapMLEnable", true);
        return enable && gAgentCamera.cameraMouselook();
    }

    /// Outside the draw pass: the corner map's floater is built, never opened, so the overlay has its widget.
    void onIdle(void*)
    {
        if (wanted() && !LLFloaterReg::findTypedInstance<SKFloaterMap>("mini_map"))
        {
            LLFloaterReg::getTypedInstance<SKFloaterMap>("mini_map");
        }
    }

    /// The target's layer over the window: viewport is the window's, side the map's, both in pixels.
    void blit(State& s, const GLint* viewport, F32 side, S32 mode, const Fades& fades)
    {
        GLint program = 0, vao = 0, src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, eq_rgb = 0, eq_a = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
        const GLboolean blend = glIsEnabled(GL_BLEND);
        GLboolean colour_mask[4] = {}, depth_mask = GL_TRUE;
        glGetBooleanv(GL_COLOR_WRITEMASK, colour_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        LLGLDisable depth(GL_DEPTH_TEST);
        LLGLDisable stencil(GL_STENCIL_TEST);
        LLGLDisable cull(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);

        const F32 hw = side / static_cast<F32>(viewport[2]);
        const F32 hh = side / static_cast<F32>(viewport[3]);
        glUseProgram(s.mProgram);
        glBindVertexArray(s.mVao);
        glUniform4f(s.mLocRect, -hw, -hh, hw, hh);
        glUniform1i(s.mLocMode, mode);
        glUniform1f(s.mLocSide, side);
        glUniform1f(s.mLocOuter, fades.mOuter);
        glUniform1f(s.mLocCentre, fades.mCentre);
        glUniform1f(s.mLocSelf, fades.mSelf);
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, s.mTarget.getTexture(), 0);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gGL.getTextureSlot(0)->unbind();

        blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        glColorMask(colour_mask[0], colour_mask[1], colour_mask[2], colour_mask[3]);
        glDepthMask(depth_mask);
        glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
        glBlendEquationSeparate(eq_rgb, eq_a);
        glBindVertexArray(vao);
        glUseProgram(program);
    }

    /// One frame of the overlay; false when nothing was drawn.
    bool drawOverlay()
    {
        static LLCachedControl<F32> span(gSavedSettings, "SKMapZoomMeters", skmap::ZOOM_DEFAULT_M);
        static LLCachedControl<bool> rotate(gSavedSettings, "SKMapMLRotate", false);
        static LLCachedControl<F32> size(gSavedSettings, "SKMapMLSize", 0.8f);
        static LLCachedControl<F32> lines(gSavedSettings, "SKMapMLLineStrength", 0.8f);
        static LLCachedControl<F32> surface(gSavedSettings, "SKMapMLSurfaceOpacity", 0.2f);
        static LLCachedControl<F32> centre_fade(gSavedSettings, "SKMapMLCentreFade", 0.18f);
        static LLCachedControl<F32> self_fade(gSavedSettings, "SKMapMLSelfFadePx", 28.f);
        static LLCachedControl<F32> outer_fade(gSavedSettings, "SKMapMLOuterFade", 0.38f);
        State& s = state();
        if (!s.mIdleAdded)
        {
            gIdleCallbacks.addFunction(onIdle, nullptr);
            s.mIdleAdded = true;
        }
        if (s.mFailed || !wanted() || !LLWorld::instanceExists() || !gAgent.getRegion())
        {
            return false;
        }
        SKFloaterMap* floater = LLFloaterReg::findTypedInstance<SKFloaterMap>("mini_map");
        SKNetMap* map = floater ? floater->map() : nullptr;
        if (!map)
        {
            return false;
        }
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        if (viewport[2] <= 0 || viewport[3] <= 0)
        {
            return false;
        }
        // Sized and faded against the window's height, so any aspect keeps the same map.
        const F32 height = static_cast<F32>(viewport[3]);
        // Stepped, so dragging the window's height reallocates the target and the plan only every SIDE_STEP pixels.
        const U32 side = llclamp(static_cast<U32>(ll_round(height * llclamp(static_cast<F32>(size), 0.2f, 1.5f) / SIDE_STEP)) * SIDE_STEP,
                                 SIDE_STEP, MAX_SIDE);
        if (!s.mProgram)
        {
            s.mProgram = compileProgram();
            if (!s.mProgram)
            {
                s.mFailed = true;
                return false;
            }
            glGenVertexArrays(1, &s.mVao);
            s.mLocRect = glGetUniformLocation(s.mProgram, "u_rect");
            s.mLocMode = glGetUniformLocation(s.mProgram, "u_mode");
            s.mLocSide = glGetUniformLocation(s.mProgram, "u_side");
            s.mLocOuter = glGetUniformLocation(s.mProgram, "u_outer");
            s.mLocCentre = glGetUniformLocation(s.mProgram, "u_centre");
            s.mLocSelf = glGetUniformLocation(s.mProgram, "u_self");
            GLint program = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glUseProgram(s.mProgram);
            glUniform1i(glGetUniformLocation(s.mProgram, "u_t"), 0);
            glUseProgram(program);
        }
        if (!s.mTarget.isComplete() || s.mTarget.getWidth() != side)
        {
            // Not latched: a later frame or another size may succeed.
            s.mTarget.release();
            if (!s.mTarget.allocate(side, side, GL_RGBA8))
            {
                return false;
            }
        }
        F32 rotation = 0.f;
        if (rotate)
        {
            const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
            if (at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY] > 0.01f)
            {
                s.mHeading = std::atan2(at.mV[VY], at.mV[VX]);
            }
            rotation = skmap::MapView::rotationForHeading(s.mHeading);
        }

        // The markers keep their UI size: the map is laid out in UI pixels and scaled to the window's.
        const F32 ui = llmax(LLUI::getScaleFactor().mV[VX], 0.01f);
        const F32 side_ui = side / ui;
        Fades fades;
        fades.mOuter = llclamp(static_cast<F32>(outer_fade), 0.05f, 1.f);
        fades.mCentre = height * llclamp(static_cast<F32>(centre_fade), 0.f, 0.5f);
        // As the study: never wider than most of the map's centre fade, while there is one.
        fades.mSelf = llclamp(static_cast<F32>(self_fade), 0.f, 128.f) * ui;
        if (fades.mCentre > 0.f)
        {
            fades.mSelf = llmin(fades.mSelf, fades.mCentre * 0.65f);
        }
        const F32 metres = llclamp(static_cast<F32>(span), skmap::ZOOM_MIN_M, skmap::ZOOM_MAX_M);

        gGL.flush();
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        GLfloat clear[4] = {};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        GLint draw_fbo = 0, read_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, static_cast<F32>(side), 0.f, static_cast<F32>(side), -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.scalef(ui, ui, 1.f);
        gGL.pushUIMatrix();
        gGL.loadUIIdentity();
        gUIProgram.bind();
        glClearColor(0.f, 0.f, 0.f, 0.f);

        for (S32 mode : { 0, 1, 2 })
        {
            s.mTarget.bindTarget();
            s.mTarget.clear(GL_COLOR_BUFFER_BIT);
            gGL.getTextureSlot(0)->unbind();
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            // Onto transparent black: colour by its alpha, alpha accumulated, so the target is premultiplied.
            gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
            const SKNetMap::OverlayLayer layer = mode == 0 ? SKNetMap::OverlayLayer::PLAN
                                                 : mode == 1 ? SKNetMap::OverlayLayer::MARKERS
                                                             : SKNetMap::OverlayLayer::SELF;
            map->drawOverlay(layer, metres, rotation, side_ui, llclamp(static_cast<F32>(lines), 0.f, 1.f),
                             llclamp(static_cast<F32>(surface), 0.f, 1.f));
            gGL.flush();
            s.mTarget.flush();
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            blit(s, viewport, static_cast<F32>(side), mode, fades);
        }

        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.popUIMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        // Unbound first, so the bind below is never skipped as already current.
        LLGLSLShader::unbind();
        if (shader)
        {
            shader->bind();
        }
        return true;
    }
}

// static
bool SKMapML::showing()
{
    // Drawn this frame or the last: the floater draws after the overlay in the same frame.
    return sState && sState->mDrawnFrame >= LLFrameTimer::getFrameCount();
}

// static
void SKMapML::draw()
{
    LL_PROFILE_ZONE_SCOPED;
    if (drawOverlay())
    {
        state().mDrawnFrame = LLFrameTimer::getFrameCount() + 1;
    }
}

// static
void SKMapML::cleanup()
{
    // The corner map's plan holds the overlay's blit too; a restarted context rebuilds both.
    if (SKFloaterMap* floater = LLFloaterReg::findTypedInstance<SKFloaterMap>("mini_map"))
    {
        if (SKNetMap* map = floater->map())
        {
            map->releaseGL();
        }
    }
    if (sState && sState->mIdleAdded)
    {
        gIdleCallbacks.deleteFunction(onIdle, nullptr);
    }
    delete sState;
    sState = nullptr;
}
