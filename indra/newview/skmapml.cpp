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

    // The overlay's mask over each layer (skooma/mapMask*.glsl); built on the first frame that wants it.
    LLGLSLShader sMask;
    const LLStaticHashedString SK_RECT("sk_rect");
    const LLStaticHashedString SK_MODE("sk_mode");
    const LLStaticHashedString SK_SIDE("sk_side");
    const LLStaticHashedString SK_OUTER("sk_outer");
    const LLStaticHashedString SK_CENTRE("sk_centre");
    const LLStaticHashedString SK_SELF("sk_self");

    /// Built again after a shader load unloads it, as no loader asks for it.
    bool ensureMask()
    {
        if (sMask.isComplete())
        {
            return true;
        }
        sMask.mName = "SK Mouselook Map Mask";
        sMask.mShaderFiles.clear();
        sMask.clearPermutations();
        sMask.mShaderFiles.push_back(std::make_pair("skooma/mapMaskV.glsl", GL_VERTEX_SHADER));
        sMask.mShaderFiles.push_back(std::make_pair("skooma/mapMaskF.glsl", GL_FRAGMENT_SHADER));
        sMask.mShaderLevel = 1;
        if (!sMask.createShader())
        {
            LL_WARNS("SKMap") << "Mouselook map shader failed to build" << LL_ENDL;
            sMask.unload();
            return false;
        }
        return true;
    }

    struct State
    {
        LLRenderTarget mTarget;
        bool mFailed = false;       ///< the shader failed to build; nothing will draw
        U32 mDrawnFrame = 0;        ///< the frame the overlay last drew in, plus one
        F32 mHeading = 0.f;         ///< the last heading the camera gave, held while it looks straight up or down
        bool mIdleAdded = false;

        ~State()
        {
            mTarget.release();
            sMask.unload();
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
    void blit(State& s, const S32* viewport, F32 side, S32 mode, const Fades& fades)
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable depth(GL_DEPTH_TEST);
        LLGLDisable stencil(GL_STENCIL_TEST);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth_write(GL_FALSE, GL_FALSE);
        gGL.setColorMask(true, true);
        gGL.flush();
        gGL.setBlendEquation(GL_FUNC_ADD, GL_FUNC_ADD);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

        const F32 hw = side / static_cast<F32>(viewport[2]);
        const F32 hh = side / static_cast<F32>(viewport[3]);
        sMask.bind();
        sMask.uniform4f(SK_RECT, -hw, -hh, hw, hh);
        sMask.uniform1i(SK_MODE, mode);
        sMask.uniform1f(SK_SIDE, side);
        sMask.uniform1f(SK_OUTER, fades.mOuter);
        sMask.uniform1f(SK_CENTRE, fades.mCentre);
        sMask.uniform1f(SK_SELF, fades.mSelf);
        const S32 unit = sMask.bindTexture(LLShaderMgr::DIFFUSE_MAP, &s.mTarget, ALSamplers::BilinearClamp);
        gGL.drawArraysInstanced(LLRender::TRIANGLE_STRIP, 0, 4, 1);
        if (unit >= 0)
        {
            gGL.getTextureSlot(unit)->unbind();
        }
        sMask.unbind();
    }

    /// One frame of the overlay; false when nothing was drawn.
    bool drawOverlay()
    {
        static LLCachedControl<F32> span(gSavedSettings, "SKMapZoomMeters", skmap::ZOOM_DEFAULT_M);
        static LLCachedControl<bool> rotate(gSavedSettings, "SKMapMLRotate", false);
        static LLCachedControl<F32> size(gSavedSettings, "SKMapMLSize", 0.8f);
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
        // The UI's, set by setup2DViewport before the UI draws.
        S32 viewport[4] = {};
        gGL.getViewport(viewport);
        if (viewport[2] <= 0 || viewport[3] <= 0)
        {
            return false;
        }
        // Sized and faded against the window's height, so any aspect keeps the same map.
        const F32 height = static_cast<F32>(viewport[3]);
        // Stepped, so dragging the window's height reallocates the target and the plan only every SIDE_STEP pixels.
        const U32 side = llclamp(static_cast<U32>(ll_round(height * llclamp(static_cast<F32>(size), 0.2f, 1.5f) / SIDE_STEP)) * SIDE_STEP,
                                 SIDE_STEP, MAX_SIDE);
        if (!ensureMask())
        {
            s.mFailed = true;
            return false;
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
        const LLColor4 saved_clear = gGL.getClearColor();
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
        gGL.setClearColor(LLColor4::transparent);

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
            map->drawOverlay(layer, metres, rotation, side_ui, llclamp(static_cast<F32>(surface), 0.f, 1.f));
            gGL.flush();
            // Back to whatever the UI draws into; the viewport set again in case that was a target of another size.
            s.mTarget.flush();
            gGL.setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            blit(s, viewport, static_cast<F32>(side), mode, fades);
        }

        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.popUIMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.setClearColor(saved_clear);
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
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    if (drawOverlay())
    {
        state().mDrawnFrame = LLFrameTimer::getFrameCount() + 1;
    }
}

// static
void SKMapML::cleanup()
{
    if (sState && sState->mIdleAdded)
    {
        gIdleCallbacks.deleteFunction(onIdle, nullptr);
    }
    delete sState;
    sState = nullptr;
}
