/**
 * @file skmapcomposite.cpp
 * @brief The mini-map plan drawn once into a multisampled offscreen target, then shown as one quad per frame.
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

#include "skmapcomposite.h"

#include "llagent.h"
#include "llglslshader.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "skmapview.h"
#include "sknavmesh.h"
#include "sknavrooms.h"

#include <cmath>

using namespace skmap::plan;

namespace
{
    // The frame covers the widget's diagonal with this much to spare, so a turn never shows its edge and a walk
    // redraws only when the centre has moved an eighth of the diagonal.
    constexpr F32 PLAN_FRAME_MARGIN = 1.25f;
    constexpr U32 MAX_SIDE = 4096;          // the composite's largest side in texels
    constexpr U32 MAX_SAMPLES = 8;          // ruling 19: 4x default, 8x allowed

    // Plan colours (proto2 gl.js COL, rulings 10 and 15).
    const F32 COL_FLOOR[3] = { 48 / 255.f, 47 / 255.f, 46 / 255.f };
    const F32 COL_TERRAIN[3] = { 37 / 255.f, 44 / 255.f, 35 / 255.f };
    const F32 COL_OTHER[3] = { 34 / 255.f, 34 / 255.f, 36 / 255.f };
    const F32 COL_WALL_FULL[4] = { 229 / 255.f, 228 / 255.f, 225 / 255.f, 0.57f };
    const F32 COL_WALL_HALF[4] = { 208 / 255.f, 208 / 255.f, 205 / 255.f, 0.16f };
    const F32 COL_HATCH[4] = { 208 / 255.f, 208 / 255.f, 205 / 255.f, 0.47f };
    constexpr F32 HATCH_SPAN_M = 128.f;     // the half-cover poché hatch shows at spans up to this

    const char* VS_PLAN = R"(#version 330 core
layout(location = 0) in vec3 a_p;
layout(location = 1) in vec4 a_c;
layout(location = 2) in vec4 a_k;
uniform vec4 u_frame;
out vec3 v_p;
out vec4 v_c;
flat out vec4 v_k;
void main()
{
    v_p = a_p;
    v_c = a_c;
    v_k = a_k;
    // Depth by height inside a rank, in [0, 1] so either clip-control mode keeps its order.
    gl_Position = vec4((a_p.xy - u_frame.xy) / u_frame.z * 2.0 - 1.0, clamp(0.5 - a_p.z / 1200.0, 0.001, 0.999), 1.0);
}
)";

    const char* FS_PLAN = R"(#version 330 core
in vec3 v_p;
in vec4 v_c;
flat in vec4 v_k;
out vec4 o;
uniform vec4 u_frame;
uniform int u_mode;
uniform float u_only;
uniform float u_vz;
uniform float u_shade;
uniform float u_hatchOn;
uniform vec3 u_floor;
uniform vec3 u_terrain;
uniform vec3 u_other;
uniform vec4 u_wf;
uniform vec4 u_wh;
uniform vec4 u_hatch;
// Hatch anchored to the plan: the fragment's position, pitch snapped to a power of two in metres near the texel pitch.
bool hatch(float pitchPx, float onPx, float dir)
{
    float mpp = u_frame.w;
    vec2 w = u_frame.xy + gl_FragCoord.xy * mpp;
    float pitch = exp2(round(log2(pitchPx * mpp)));
    return fract((w.x + dir * w.y) / (pitch * 1.41421356)) * pitch / mpp < onPx;
}
void main()
{
    if (u_mode == 0)
    {
        if (v_k.x == 4.0)
        {
            o = hatch(6.0, 1.5, -1.0) ? vec4(1.0, 0.0, 1.0, 0.55) : vec4(1.0, 0.0, 1.0, 0.06);
            return;
        }
        vec3 c = v_k.x == 1.0 ? u_terrain : v_k.x == 2.0 ? u_other : u_floor;
        if (u_shade > 0.5 && v_k.x != 2.0)
        {
            c *= clamp(0.72 + 0.26 * tanh((v_p.z - u_vz) / 10.0), 0.35, 1.0) / 0.72;
        }
        if (v_k.y > 0.5 && v_p.z < v_k.z)
        {
            float k = clamp((v_k.z - v_p.z) / 6.0, 0.0, 1.0);
            c = mix(c, mix(vec3(0.55, 0.72, 0.82), vec3(0.12, 0.20, 0.32), k), 0.15 + 0.30 * k);
        }
        o = vec4(c, v_c.a);
    }
    else if (u_mode == 1)
    {
        if (u_only >= 0.0 && v_k.x != u_only)
        {
            discard;
        }
        vec4 c = v_k.x == 1.0 ? u_wh : u_wf;
        if (v_k.x == 1.0 && u_hatchOn > 0.5 && hatch(5.0, 1.0, 1.0))
        {
            c = u_hatch;
        }
        o = vec4(c.rgb, c.a * v_k.y);
    }
    else if (u_mode == 2)
    {
        if (v_c.a < 0.0)
        {
            if (!hatch(5.0, 1.5, 1.0))
            {
                discard;
            }
            o = vec4(v_c.rgb, -v_c.a);
            return;
        }
        o = v_c;
    }
    else
    {
        o = vec4(0.0);
    }
}
)";

    const char* VS_SHADE = R"(#version 330 core
layout(location = 0) in vec3 a_p;
out vec2 v_t;
void main()
{
    v_t = a_p.xy * 0.5 + 0.5;
    gl_Position = vec4(a_p.xy, 0.0, 1.0);
}
)";

    const char* FS_SHADE = R"(#version 330 core
in vec2 v_t;
out vec4 o;
uniform sampler2D u_t;
void main()
{
    o = vec4(0.0, 0.0, 0.0, texture(u_t, v_t).r);
}
)";

    U32 compile(const char* vs, const char* fs)
    {
        const auto stage = [](GLenum type, const char* src) -> GLuint
        {
            const GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[2048] = {};
                glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
                LL_WARNS("SKMap") << "Plan shader failed to compile: " << log << LL_ENDL;
                glDeleteShader(s);
                return 0;
            }
            return s;
        };
        const GLuint v = stage(GL_VERTEX_SHADER, vs), f = stage(GL_FRAGMENT_SHADER, fs);
        if (!v || !f)
        {
            if (v) glDeleteShader(v);
            if (f) glDeleteShader(f);
            return 0;
        }
        const GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[2048] = {};
            glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
            LL_WARNS("SKMap") << "Plan shader failed to link: " << log << LL_ENDL;
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    /// The GL state the composite touches, read on entry and put back on exit, so the UI pass and the
    /// renderer's caches see what they left.
    struct SavedState
    {
        GLint mProgram = 0, mVao = 0, mArray = 0, mDraw = 0, mRead = 0;
        GLint mViewport[4] = {};
        GLboolean mBlend = 0, mDepth = 0, mStencil = 0, mScissor = 0, mCull = 0;
        GLboolean mDepthMask = 0, mColourMask[4] = {};
        GLint mDepthFunc = 0, mStencilWrite = 0, mStencilFunc = 0, mStencilRef = 0, mStencilMask = 0;
        GLint mStencilFail = 0, mStencilZFail = 0, mStencilZPass = 0;
        GLint mSrcRgb = 0, mDstRgb = 0, mSrcA = 0, mDstA = 0, mEqRgb = 0, mEqA = 0;
        GLfloat mClearColour[4] = {}, mClearDepth = 1.f;
        GLint mClearStencil = 0;
        LLGLSLShader* mShader = nullptr;

        void save()
        {
            mShader = LLGLSLShader::sCurBoundShaderPtr;
            glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &mVao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArray);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &mDraw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &mRead);
            glGetIntegerv(GL_VIEWPORT, mViewport);
            mBlend = glIsEnabled(GL_BLEND);
            mDepth = glIsEnabled(GL_DEPTH_TEST);
            mStencil = glIsEnabled(GL_STENCIL_TEST);
            mScissor = glIsEnabled(GL_SCISSOR_TEST);
            mCull = glIsEnabled(GL_CULL_FACE);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &mDepthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK, mColourMask);
            glGetIntegerv(GL_DEPTH_FUNC, &mDepthFunc);
            glGetIntegerv(GL_STENCIL_WRITEMASK, &mStencilWrite);
            glGetIntegerv(GL_STENCIL_FUNC, &mStencilFunc);
            glGetIntegerv(GL_STENCIL_REF, &mStencilRef);
            glGetIntegerv(GL_STENCIL_VALUE_MASK, &mStencilMask);
            glGetIntegerv(GL_STENCIL_FAIL, &mStencilFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &mStencilZFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &mStencilZPass);
            glGetIntegerv(GL_BLEND_SRC_RGB, &mSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &mDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &mSrcA);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &mDstA);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &mEqRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &mEqA);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, mClearColour);
            glGetFloatv(GL_DEPTH_CLEAR_VALUE, &mClearDepth);
            glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &mClearStencil);
        }

        static void cap(GLenum c, GLboolean on) { on ? glEnable(c) : glDisable(c); }

        void restore()
        {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDraw);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, mRead);
            glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
            cap(GL_BLEND, mBlend);
            cap(GL_DEPTH_TEST, mDepth);
            cap(GL_STENCIL_TEST, mStencil);
            cap(GL_SCISSOR_TEST, mScissor);
            cap(GL_CULL_FACE, mCull);
            glDepthMask(mDepthMask);
            glColorMask(mColourMask[0], mColourMask[1], mColourMask[2], mColourMask[3]);
            glDepthFunc(mDepthFunc);
            glStencilMask(mStencilWrite);
            glStencilFunc(mStencilFunc, mStencilRef, mStencilMask);
            glStencilOp(mStencilFail, mStencilZFail, mStencilZPass);
            glBlendFuncSeparate(mSrcRgb, mDstRgb, mSrcA, mDstA);
            glBlendEquationSeparate(mEqRgb, mEqA);
            glClearColor(mClearColour[0], mClearColour[1], mClearColour[2], mClearColour[3]);
            glClearDepth(mClearDepth);
            glClearStencil(mClearStencil);
            glBindVertexArray(mVao);
            glBindBuffer(GL_ARRAY_BUFFER, mArray);
            LLVertexBuffer::unbind();
            glUseProgram(mProgram);
            if (mShader)
            {
                mShader->bind();
            }
        }
    };

    struct Range
    {
        U32 mFirst = 0;
        U32 mCount = 0;
    };

    void attribs()
    {
        const GLsizei stride = sizeof(Vertex);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, mP)));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, mC)));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, mK)));
    }

    // Draw ranges of the uploaded geometry, in the order the frame draws them.
    struct Layout
    {
        struct RankRanges
        {
            Range mFloors, mWalls, mWallLines, mLines;
        };
        std::vector<RankRanges> mRanks;
        Range mShadowClip, mShadowBand, mTop, mQuad;
    };
    Layout sLayout;
}

SKMapComposite::~SKMapComposite()
{
    release();
}

void SKMapComposite::release()
{
    const auto del_fbo = [](U32& f) { if (f) { glDeleteFramebuffers(1, &f); f = 0; } };
    const auto del_rb = [](U32& r) { if (r) { glDeleteRenderbuffers(1, &r); r = 0; } };
    const auto del_tex = [](U32& t) { if (t) { glDeleteTextures(1, &t); t = 0; } };
    const auto del_buf = [](U32& b) { if (b) { glDeleteBuffers(1, &b); b = 0; } };
    del_fbo(mMsaaFbo);
    del_rb(mMsaaColour);
    del_rb(mMsaaDepth);
    del_fbo(mResolveFbo);
    del_tex(mResolveTex);
    del_fbo(mShadeFbo);
    del_tex(mShadeTex);
    del_rb(mShadeDepth);
    del_buf(mVbo);
    del_buf(mTerrainVbo);
    del_buf(mTerrainIbo);
    if (mVao)
    {
        glDeleteVertexArrays(1, &mVao);
        mVao = 0;
    }
    if (mProgram)
    {
        glDeleteProgram(mProgram);
        mProgram = 0;
    }
    if (mShadeProgram)
    {
        glDeleteProgram(mShadeProgram);
        mShadeProgram = 0;
    }
    mSide = 0;
    mSamples = 0;
    mTerrainData = nullptr;
    mTerrainCount = 0;
    mGeometryDirty = true;
    mRenderDirty = true;
}

bool SKMapComposite::ensureGL(U32 side, U32 samples)
{
    if (mGLFailed)
    {
        return false;
    }
    if (!mProgram)
    {
        mProgram = compile(VS_PLAN, FS_PLAN);
        mShadeProgram = compile(VS_SHADE, FS_SHADE);
        if (!mProgram || !mShadeProgram)
        {
            mGLFailed = true;
            return false;
        }
        glGenVertexArrays(1, &mVao);
        glGenBuffers(1, &mVbo);
    }
    if (side == mSide && samples == mSamples && mMsaaFbo)
    {
        return true;
    }
    const U32 vao = mVao, vbo = mVbo, program = mProgram, shade = mShadeProgram;
    mVao = mVbo = mProgram = mShadeProgram = 0;     // keep the programs and buffers through the target rebuild
    release();
    mVao = vao;
    mVbo = vbo;
    mProgram = program;
    mShadeProgram = shade;

    const auto storage = [](U32& rb, U32 n, GLenum format, U32 s)
    {
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, n, format, s, s);
    };
    const auto texture = [](U32& t, U32 s)
    {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    // The texture is bound outside the slot cache; the slot forgets what it held.
    GLint old_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    storage(mMsaaColour, samples, GL_RGBA8, side);
    storage(mMsaaDepth, samples, GL_DEPTH24_STENCIL8, side);
    glGenFramebuffers(1, &mMsaaFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, mMsaaFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, mMsaaColour);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mMsaaDepth);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    texture(mResolveTex, side);
    glGenFramebuffers(1, &mResolveFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, mResolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mResolveTex, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    texture(mShadeTex, side);
    storage(mShadeDepth, 0, GL_DEPTH24_STENCIL8, side);
    glGenFramebuffers(1, &mShadeFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, mShadeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mShadeTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mShadeDepth);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindTexture(GL_TEXTURE_2D, old_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    mSide = side;
    mSamples = samples;
    mRenderDirty = true;
    if (!ok)
    {
        LL_WARNS("SKMap") << "Plan composite target incomplete at " << side << " texels, " << samples << " samples" << LL_ENDL;
        mGLFailed = true;
    }
    return ok;
}

void SKMapComposite::buildTerrain()
{
    mTerrainData = mData.get();
    std::vector<Vertex> verts;
    std::vector<U32> idx;
    for (const Region& r : mData->mRegions)
    {
        const U32 base = static_cast<U32>(verts.size()), n = r.mSide + 1;
        for (S32 y = 0; y <= r.mSide; ++y)
        {
            for (S32 x = 0; x <= r.mSide; ++x)
            {
                verts.push_back({ { r.mMin.mV[VX] + x, r.mMin.mV[VY] + y, r.mHeights[y * n + x] }, { 0.f, 0.f, 0.f, 1.f }, { 1.f, 1.f, r.mWater, 0.f } });
            }
        }
        for (S32 y = 0; y < r.mSide; ++y)
        {
            for (S32 x = 0; x < r.mSide; ++x)
            {
                const U32 a = base + y * n + x, b = a + 1, c = a + n + 1, d = a + n;
                idx.insert(idx.end(), { a, b, c, a, c, d });
            }
        }
    }
    if (!mTerrainVbo)
    {
        glGenBuffers(1, &mTerrainVbo);
        glGenBuffers(1, &mTerrainIbo);
    }
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mTerrainVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mTerrainIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(U32), idx.data(), GL_STATIC_DRAW);
    mTerrainCount = static_cast<U32>(idx.size());
}

void SKMapComposite::render(F32 vz)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.Composite");
    LLTimer timer;
    const GLint loc_frame = glGetUniformLocation(mProgram, "u_frame");
    const GLint loc_mode = glGetUniformLocation(mProgram, "u_mode");
    const GLint loc_only = glGetUniformLocation(mProgram, "u_only");
    glUseProgram(mProgram);
    glUniform4f(loc_frame, mFrameMin.mV[VX], mFrameMin.mV[VY], mFrameSize, mMpp);
    glUniform1f(glGetUniformLocation(mProgram, "u_vz"), vz);
    glUniform1f(glGetUniformLocation(mProgram, "u_shade"), mOptions.mShade ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(mProgram, "u_hatchOn"), mSpan <= HATCH_SPAN_M ? 1.f : 0.f);
    glUniform3fv(glGetUniformLocation(mProgram, "u_floor"), 1, COL_FLOOR);
    glUniform3fv(glGetUniformLocation(mProgram, "u_terrain"), 1, COL_TERRAIN);
    glUniform3fv(glGetUniformLocation(mProgram, "u_other"), 1, COL_OTHER);
    glUniform4fv(glGetUniformLocation(mProgram, "u_wf"), 1, COL_WALL_FULL);
    glUniform4fv(glGetUniformLocation(mProgram, "u_wh"), 1, COL_WALL_HALF);
    glUniform4fv(glGetUniformLocation(mProgram, "u_hatch"), 1, COL_HATCH);
    glUniform1f(loc_only, -1.f);
    const auto mode = [&](S32 m) { glUniform1i(loc_mode, m); };
    const auto draw = [](const Range& r) { if (r.mCount) glDrawArrays(GL_TRIANGLES, r.mFirst, r.mCount); };

    glBindFramebuffer(GL_FRAMEBUFFER, mMsaaFbo);
    glViewport(0, 0, mSide, mSide);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // Clears obey the write masks; set them before every clear.
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindVertexArray(mVao);

    if (mOptions.mTerrain && mTerrainCount)
    {
        mode(0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glBindBuffer(GL_ARRAY_BUFFER, mTerrainVbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mTerrainIbo);
        attribs();
        glDrawElements(GL_TRIANGLES, mTerrainCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    attribs();
    // Ranks low to high; each starts with a clear depth buffer, so a higher drawn storey covers everything below it.
    for (const Layout::RankRanges& g : sLayout.mRanks)
    {
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        glClear(GL_DEPTH_BUFFER_BIT);
        if (g.mFloors.mCount)
        {
            mode(0);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            draw(g.mFloors);
        }
        if (g.mWalls.mCount)
        {
            // Poché at the run's floor; each pixel takes the wall colour once, full walls before half.
            mode(1);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glStencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(1);
            glStencilFunc(GL_EQUAL, 0, 1);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            for (F32 k : { 0.f, 1.f, 2.f })
            {
                glUniform1f(loc_only, k);
                draw(g.mWalls);
            }
            glUniform1f(loc_only, -1.f);
            glDisable(GL_STENCIL_TEST);
        }
        if (g.mWallLines.mCount)
        {
            // Each pixel of the rank's wall lines once.
            mode(2);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glStencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(2);
            glStencilFunc(GL_NOTEQUAL, 2, 2);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            draw(g.mWallLines);
            glStencilMask(0xff);
            glDisable(GL_STENCIL_TEST);
        }
        if (g.mLines.mCount)
        {
            mode(2);
            glDisable(GL_DEPTH_TEST);
            draw(g.mLines);
        }
    }
    // Roof inner shadow: the falloff max-blended into its own target clipped to the roofs, then darkening by it.
    if (sLayout.mShadowBand.mCount && sLayout.mShadowClip.mCount)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mShadeFbo);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        mode(3);
        draw(sLayout.mShadowClip);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_EQUAL, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glBlendEquationSeparate(GL_MAX, GL_MAX);
        glBlendFunc(GL_ONE, GL_ONE);
        mode(2);
        draw(sLayout.mShadowBand);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_STENCIL_TEST);

        glBindFramebuffer(GL_FRAMEBUFFER, mMsaaFbo);
        glUseProgram(mShadeProgram);
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, mShadeTex, 0);
        glUniform1i(glGetUniformLocation(mShadeProgram, "u_t"), 0);
        draw(sLayout.mQuad);
        gGL.getTextureSlot(0)->unbind();
        glUseProgram(mProgram);
    }
    // Region edges over everything.
    mode(2);
    glDisable(GL_DEPTH_TEST);
    draw(sLayout.mTop);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, mMsaaFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mResolveFbo);
    glBlitFramebuffer(0, 0, mSide, mSide, 0, 0, mSide, mSide, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    mStats.mRenderMs = timer.getElapsedTimeF32() * 1000.f;
    ++mStats.mRedraws;
}

void SKMapComposite::draw(const skmap::MapView& view)
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<U32> overlap(gSavedSettings, "SKMapPlanOverlap", 0);
    static LLCachedControl<U32> outside(gSavedSettings, "SKMapPlanOutside", 0);
    static LLCachedControl<U32> msaa(gSavedSettings, "SKMapPlanMSAA", 4);
    static LLCachedControl<bool> gaps(gSavedSettings, "SKMapPlanGaps", true);
    static LLCachedControl<bool> ledges(gSavedSettings, "SKMapPlanLedges", true);
    static LLCachedControl<bool> other(gSavedSettings, "SKMapPlanOtherLevel", true);
    static LLCachedControl<bool> roof_outline(gSavedSettings, "SKMapPlanRoofOutlines", true);
    static LLCachedControl<bool> terrain(gSavedSettings, "SKMapPlanTerrain", true);
    static LLCachedControl<bool> contours(gSavedSettings, "SKMapLayerContours", true);
    static LLCachedControl<bool> doors(gSavedSettings, "SKMapLayerDoors", true);
    static LLCachedControl<bool> region_edges(gSavedSettings, "SKMapLayerVoid", true);
    if (!SKNavMesh::instanceExists() || mGLFailed)
    {
        return;
    }
    Options opts;
    opts.mOverlap = static_cast<Overlap>(llmin<U32>(overlap, 2));
    opts.mOutside = static_cast<Outside>(llmin<U32>(outside, 2));
    opts.mGaps = gaps;
    opts.mLedges = ledges;
    opts.mOther = other;
    opts.mRoofOutline = roof_outline;
    opts.mTerrain = terrain;
    opts.mContours = contours;
    opts.mDoors = doors;
    opts.mRegionEdges = region_edges;
    const bool options_moved = !(opts == mOptions);
    mOptions = opts;

    // The frame: north-up, the widget's diagonal with margin, in window texels.
    const F32 ui = gGL.getUIScale().mV[VX];
    GLint max_rb = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    const F32 diag = std::sqrt(view.width() * view.width() + view.height() * view.height());
    const U32 side = llmin(static_cast<U32>(std::ceil(diag * PLAN_FRAME_MARGIN * ui)), MAX_SIDE, static_cast<U32>(llmax(max_rb, 1)));
    const F32 mpp = view.mpp() / ui;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    const U32 samples = llmin(static_cast<U32>(msaa), MAX_SAMPLES, static_cast<U32>(llmax(max_samples, 0)));

    SKNavMesh& nav = SKNavMesh::instance();
    const LLVector3d origin_of_view = view.centre();
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        return;
    }
    // The plan's origin is the agent region's corner at z 0, so plan z is global z and the hatch is region-anchored.
    LLVector3d origin = agent_region->getOriginGlobal();
    origin.mdV[VZ] = 0.0;
    const F32 size_m = side * mpp;
    const LLVector2 centre_local(static_cast<F32>(origin_of_view.mdV[VX] - origin.mdV[VX]), static_cast<F32>(origin_of_view.mdV[VY] - origin.mdV[VY]));
    const bool origin_moved = mData && (mData->mOrigin - origin).lengthSquared() > 0.0;
    const LLVector2 frame_centre = mFrameMin + LLVector2(mFrameSize, mFrameSize) * 0.5f;
    const F32 slack = (size_m - diag * view.mpp()) * 0.5f;
    const bool recentre = origin_moved || side != mSide || mpp != mMpp || std::fabs(centre_local.mV[VX] - frame_centre.mV[VX]) > slack
                          || std::fabs(centre_local.mV[VY] - frame_centre.mV[VY]) > slack;
    if (recentre)
    {
        // Snapped to the texel lattice so a recentre does not move what is drawn.
        mFrameMin = LLVector2(std::floor((centre_local.mV[VX] - size_m * 0.5f) / mpp) * mpp, std::floor((centre_local.mV[VY] - size_m * 0.5f) / mpp) * mpp);
        mFrameSize = size_m;
        mRenderDirty = true;
    }
    if (view.span() != mSpan)
    {
        mSpan = view.span();
        mRenderDirty = true;
    }
    if (mpp != mMpp)
    {
        mMpp = mpp;
        mGeometryDirty = true;
    }

    // Data: the region meshes meeting the frame, gathered again only when what they are built from moves.
    const SKNavWorldViewPtr world = nav.view();
    const bool nav_moved = nav.revision() != mNavRevision || world.get() != mViewSeen || (world && world->revision() != mViewRevision);
    if (nav_moved || recentre || !mData)
    {
        mNavRevision = nav.revision();
        mViewSeen = world.get();
        mViewRevision = world ? world->revision() : 0;
        const LLVector3d gmin = origin + LLVector3d(mFrameMin.mV[VX], mFrameMin.mV[VY], 0.0);
        const LLVector3d gmax = gmin + LLVector3d(mFrameSize, mFrameSize, 0.0);
        const U64 key = gatherKey(gmin, gmax);
        if (!mData || key != mDataKey || origin_moved)
        {
            LLTimer t;
            mData = gather(origin, gmin, gmax);
            mDataKey = key;
            mStats.mGatherMs = t.getElapsedTimeF32() * 1000.f;
            mHasSelection = false;
            mViewerSpace = -2;
        }
    }

    const LLVector3d agent = gAgent.getPositionGlobal();
    const S32 vs = viewerSpace(*mData, agent);
    if (!mHasSelection || vs != mViewerSpace || options_moved)
    {
        LLTimer t;
        mSelection = select(*mData, vs, mOptions);
        mStats.mSelectMs = t.getElapsedTimeF32() * 1000.f;
        mViewerSpace = vs;
        mHasSelection = true;
        if (mSelection.mHash != mGeometryKey || options_moved)
        {
            mGeometryKey = mSelection.mHash;
            mGeometryDirty = true;
        }
    }
    F32 vz = static_cast<F32>(agent.mdV[VZ]);
    nav.floorUnder(agent, sknav::AgentClass::AVATAR, vz);

    gGL.flush();
    SavedState saved;
    saved.save();
    LLGLSLShader::unbind();
    if (!ensureGL(side, samples))
    {
        saved.restore();
        return;
    }
    if (mTerrainData != mData.get())
    {
        buildTerrain();
        mRenderDirty = true;
    }
    if (mGeometryDirty)
    {
        LLTimer t;
        buildGeometry(*mData, mSelection, mOptions, mMpp, mGeometry);
        mStats.mGeometryMs = t.getElapsedTimeF32() * 1000.f;
        std::vector<Vertex> all;
        sLayout.mRanks.clear();
        const auto add = [&all](const std::vector<Vertex>& v) { Range r{ static_cast<U32>(all.size()), static_cast<U32>(v.size()) }; all.insert(all.end(), v.begin(), v.end()); return r; };
        for (const Geometry::Rank& g : mGeometry.mRanks)
        {
            sLayout.mRanks.push_back({ add(g.mFloors), add(g.mWalls), add(g.mWallLines), add(g.mLines) });
        }
        sLayout.mShadowClip = add(mGeometry.mShadowClip);
        sLayout.mShadowBand = add(mGeometry.mShadowBand);
        sLayout.mTop = add(mGeometry.mTop);
        const Vertex q[6] = { { { -1, -1, 0 }, {}, {} }, { { 1, -1, 0 }, {}, {} }, { { 1, 1, 0 }, {}, {} },
                              { { -1, -1, 0 }, {}, {} }, { { 1, 1, 0 }, {}, {} }, { { -1, 1, 0 }, {}, {} } };
        sLayout.mQuad = add(std::vector<Vertex>(q, q + 6));
        glBindVertexArray(mVao);
        glBindBuffer(GL_ARRAY_BUFFER, mVbo);
        glBufferData(GL_ARRAY_BUFFER, all.size() * sizeof(Vertex), all.data(), GL_STATIC_DRAW);
        mStats.mTriangles = static_cast<U32>(all.size() / 3 + mTerrainCount / 3);
        mStats.mRanks = static_cast<U32>(mGeometry.mRanks.size());
        mStats.mWallRuns = mGeometry.mWallRuns;
        mGeometryDirty = false;
        mRenderDirty = true;
    }
    if (vz != mDrawnVz)
    {
        mRenderDirty = true;
    }
    if (mRenderDirty)
    {
        render(vz);
        mDrawnVz = vz;
        mRenderDirty = false;
    }
    saved.restore();
    mStats.mSpaces = static_cast<U32>(mSelection.mRank.size());
    mStats.mPolys = static_cast<U32>(mData->mPolys.size());
    mStats.mSamples = mSamples;
    mStats.mSide = mSide;

    // The one quad: each widget corner's texel in the north-up frame.
    LLVector2 uv[4];
    const F32 corner[4][2] = { { 0.f, 0.f }, { view.width(), 0.f }, { view.width(), view.height() }, { 0.f, view.height() } };
    for (S32 k = 0; k < 4; ++k)
    {
        const LLVector3d g = view.toGlobal(corner[k][0], corner[k][1]);
        uv[k] = LLVector2((static_cast<F32>(g.mdV[VX] - origin.mdV[VX]) - mFrameMin.mV[VX]) / mFrameSize,
                          (static_cast<F32>(g.mdV[VY] - origin.mdV[VY]) - mFrameMin.mV[VY]) / mFrameSize);
    }
    gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, mResolveTex, 0);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 k : { 0, 1, 2, 0, 2, 3 })
    {
        gGL.texCoord2f(uv[k].mV[VX], uv[k].mV[VY]);
        gGL.vertex2f(corner[k][0], corner[k][1]);
    }
    gGL.end();
    gGL.getTextureSlot(0)->unbind();
}

std::string SKMapComposite::statsLine() const
{
    return llformat("plan: gather %.1f select %.2f geometry %.1f render %.2f ms | %u tris, %u ranks, %u spaces, %u walls, %u polys | %u px %ux, %u redraws",
                    mStats.mGatherMs, mStats.mSelectMs, mStats.mGeometryMs, mStats.mRenderMs, mStats.mTriangles, mStats.mRanks, mStats.mSpaces,
                    mStats.mWallRuns, mStats.mPolys, mStats.mSide, mStats.mSamples, mStats.mRedraws);
}
