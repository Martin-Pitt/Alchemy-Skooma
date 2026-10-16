/**
 * @file skoverviewcapture.cpp
 * @brief The overview: the census as styled shapes over the terrain, for the mini-map's base and
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

#include "llsmoothstep.h"

#include "llagent.h"
#include "llvoavatarself.h"
#include "llagentcamera.h"
#include "llviewerjoystick.h"
#include "llworld.h"
#include "sknetmap.h"
#include "skmappeople.h"
#include "skcensusavatarshape.h"
#include "skmaplabels.h"
#include "skmapmenu.h"
#include "llrootview.h"
#include "skpopuppanes.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llviewerinput.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llcharacter.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llsurface.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llvoavatar.h"
#include "llviewerobjectlist.h"
#include "pipeline.h"
#include "sknavbuild.h"
#include "sknavmesh.h"
#include "skmappalette.h"
#include "skworldcensus.h"

#include <algorithm>
#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    LLRenderTarget sCapture;      // depth, and a class code in R (skoverview::Code), an invisible face in B
    LLRenderTarget sCaptureNear;  // the same for the agent's neighbours, at half the resolution
    LLRenderTarget sCaptureFar;   // and for regions further off, at a quarter
    LLRenderTarget sTacticalCapture;
    LLRenderTarget sTacticalResolved;
    LLRenderTarget sTacticalWork;   // the static capture copied, with the movers drawn over it
    LLGLSLShader sResolve;
    LLGLSLShader sComposite;
    LLGLSLShader sGeom;
    LLGLSLShader sGeomCut;    // the census with a cut height per vertex, for the tactical view
    LLGLSLShader sMarker;     // the tactical avatar markers, depth-tested by hand       // the census: class code and the face's own normal
    GLuint sQueries[2] = { 0, 0 };

    const F32 CENSUS_RANGE_M = 128.f;
    const size_t SK_MAX_TERRITORY = 16;       // regions styled as inside, at most (the resolve's array)
    const F32 TACTICAL_MIN_PITCH = 20.f * DEG_TO_RAD;   // alt-cam tilts the tactical camera no flatter than this
    const F32 TACTICAL_MAX_PITCH = 89.f * DEG_TO_RAD;   // nor past straight down
    const F32 AVATAR_HALF_HEIGHT_M = 0.9f;     // an avatar's position over its feet, near enough
    const F32 MARKER_RADIUS_M = 0.35f;         // the avatar marker's cylinder at true size
    const F32 MARKER_HEIGHT_M = 1.8f;
    const F32 MARKER_COARSE_HEIGHT_M = 0.5f;   // a coarse-only avatar: a short plain puck, no heading
    const S32 MARKER_SIDES = 16;
    const F32 MARKER_GHOST_ALPHA = 0.3f;       // its silhouette where the census hides it
    const F32 HEADING_TIP_PER_RADIUS = 1.f;    // the minimap's heading triangle (skmap::palette::HEADING_*_PX) over
    const F32 HEADING_BASE_PER_RADIUS = 0.25f; // its 6 px dot, as fractions of the body's radius beyond it
    const F32 HEADING_HALF_PER_RADIUS = 0.42f;
    const F32 HEADING_THICK_PER_RADIUS = 0.3f; // and extruded this thick
    const F32 TACTICAL_NEAR_M = 0.5f;
    const F32 TACTICAL_MIN_M = 8.f;
    const F32 TACTICAL_MAX_M = 400.f;
    const F64 TACTICAL_TAP_S = 0.2;
    const F32 LEVEL_BELOW_M = 4.f;  // another region's map keeps the clusters reaching this far below the agent's floor
    const F32 LEVEL_ABOVE_M = 8.f;  // and this far above it
    const F32 ON_GROUND_M = 8.f;    // an agent's floor this close to the land is at ground level

    void uniformColour(GLint prog, const char* name, const skmap::PaletteColour& c)
    {
        glUniform3f(glGetUniformLocation(prog, name), c.mR, c.mG, c.mB);
    }

}

struct SKOverviewCapture::RegionMap
{
    U64 mHandle = 0;
    LLViewerRegion* mRegion = nullptr; // this frame's, set before any capture
    LLRenderTarget mResolved; // the map, RGBA8
    LLVector3d mOrigin;
    F32 mWidth = 0.f;
    F64 mLast = -1.0;
    U32 mRing = 0;            // 0 the agent's region, 1 a neighbour, more the further off
    bool mCut = false;        // whether it was taken through the agent's cut
    bool mValid = false;
};

SKOverviewCapture::SKOverviewCapture() = default;
SKOverviewCapture::~SKOverviewCapture() = default;

void SKOverviewCapture::onFrame()
{
    updateCapsLockCapture();
    // The world census runs slowly only while the overview hides the world (navmesh freshness matters little then),
    // and back at its normal cadence the frame it closes.
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus::instance().setSlowCadence(worldHidden());
    }
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
    if (instanceExists() && instance().mTacticalCam.mActive)
    {
        TacticalCamera& cam = instance().mTacticalCam;
        cam.mDistance = llclamp(cam.mDistance * std::pow(1.15f, clicks), TACTICAL_MIN_M, TACTICAL_MAX_M);
    }
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
    sCaptureNear.release();
    sCaptureFar.release();
    sTacticalCapture.release();
    sTacticalResolved.release();
    sTacticalWork.release();
    sResolve.unload();
    sComposite.unload();
    sGeom.unload();
    sGeomCut.unload();
    sMarker.unload();
}

void SKOverviewCapture::releaseGL()
{
    sCapture.release();
    sCaptureNear.release();
    sCaptureFar.release();
    sTacticalCapture.release();
    sTacticalResolved.release();
    sTacticalWork.release();
    sResolve.unload();
    sComposite.unload();
    sGeom.unload();
    sGeomCut.unload();
    sMarker.unload();
    if (sQueries[0])
    {
        glDeleteQueries(2, sQueries);
        sQueries[0] = sQueries[1] = 0;
    }
    mCensus.release();
    mMaps.clear();
    mTacticalKey.mValid = false;
    mTiming = false;
    mGLFailed = false;
}

void SKOverviewCapture::updateDemand(bool wanted)
{
    if (!SKWorldCensus::instanceExists())
    {
        return;
    }
    // The region maps want the census over the agent's neighbours as well; the tactical view only around it.
    static LLCachedControl<bool> maps(gSavedSettings, "SKMapCaptureBase", false);
    static LLCachedControl<F32> maps_range(gSavedSettings, "SKOverviewCensusRange", 384.f);
    F32 range = maps ? llmax((F32)maps_range, CENSUS_RANGE_M) : CENSUS_RANGE_M;
    // The tactical view shows the neighbours too: the census reaches as far as it looks, in 64 m steps so a zoom
    // does not restart the demand every frame.
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    static LLCachedControl<F32> tactical_distance(gSavedSettings, "SKTacticalDistance", 60.f);
    if (tactical)
    {
        const F32 reach = llclamp((F32)tactical_distance, TACTICAL_MIN_M, TACTICAL_MAX_M) + CENSUS_RANGE_M;
        range = llmax(range, std::ceil(reach / 64.f) * 64.f);
    }
    if (wanted == mDemanding && (!wanted || range == mDemandRange))
    {
        return;
    }
    if (wanted)
    {
        SKWorldCensus::instance().addDemand("overview", range);
        mDemandRange = range;
    }
    else
    {
        SKWorldCensus::instance().removeDemand("overview");
    }
    mDemanding = wanted;
}

std::vector<SKOverviewCapture::Tile> SKOverviewCapture::tiles() const
{
    std::vector<Tile> out;
    const RegionMap* agent = nullptr;
    for (const std::unique_ptr<RegionMap>& map : mMaps)
    {
        if (!map->mValid || !map->mResolved.isComplete())
        {
            continue;
        }
        if (map->mRing == 0)
        {
            agent = map.get();
            continue;
        }
        out.push_back({ map->mResolved.getTexture(), map->mOrigin, map->mWidth });
    }
    if (agent)
    {
        out.push_back({ agent->mResolved.getTexture(), agent->mOrigin, agent->mWidth });
    }
    return out;
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
    if (ok && !sGeom.mProgramObject)
    {
        sGeom.mName = "SK Overview Geometry";
        sGeom.mShaderFiles.clear();
        sGeom.clearPermutations();
        sGeom.mShaderFiles.push_back(std::make_pair("skooma/overviewGeomV.glsl", GL_VERTEX_SHADER));
        sGeom.mShaderFiles.push_back(std::make_pair("skooma/overviewGeomF.glsl", GL_FRAGMENT_SHADER));
        sGeom.mShaderLevel = 1;
        ok = sGeom.createShader();
        LL_INFOS_ONCE("SKOverview") << "overview geometry shader " << (ok ? "loaded" : "FAILED") << LL_ENDL;
    }
    if (ok && !sGeomCut.mProgramObject)
    {
        sGeomCut.mName = "SK Overview Geometry Cut";
        sGeomCut.mShaderFiles.clear();
        sGeomCut.clearPermutations();
        sGeomCut.addPermutation("SK_CUT_ATTRIBUTE", "1");
        sGeomCut.mShaderFiles.push_back(std::make_pair("skooma/overviewGeomV.glsl", GL_VERTEX_SHADER));
        sGeomCut.mShaderFiles.push_back(std::make_pair("skooma/overviewGeomF.glsl", GL_FRAGMENT_SHADER));
        sGeomCut.mShaderLevel = 1;
        ok = sGeomCut.createShader();
        LL_INFOS_ONCE("SKOverview") << "overview cut geometry shader " << (ok ? "loaded" : "FAILED") << LL_ENDL;
    }
    if (ok && !sMarker.mProgramObject)
    {
        sMarker.mName = "SK Overview Marker";
        sMarker.mShaderFiles.clear();
        sMarker.clearPermutations();
        sMarker.mShaderFiles.push_back(std::make_pair("skooma/overviewMarkerV.glsl", GL_VERTEX_SHADER));
        sMarker.mShaderFiles.push_back(std::make_pair("skooma/overviewMarkerF.glsl", GL_FRAGMENT_SHADER));
        sMarker.mShaderLevel = 1;
        ok = sMarker.createShader();
        LL_INFOS_ONCE("SKOverview") << "overview marker shader " << (ok ? "loaded" : "FAILED") << LL_ENDL;
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
    }
    if (resolved.isComplete() && ((S32)resolved.getWidth() != w || (S32)resolved.getHeight() != h))
    {
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

void SKOverviewCapture::cutHeights(const LLVector3d& at, F32 fallback_z, F32& floor_z, F32& cut_z) const
{
    static LLCachedControl<F32> cut(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    // The avatar's feet are the floor; the navmesh only refines it, since its nearest mesh can be the ground under a street.
    F32 feet = fallback_z;
    if (isAgentAvatarValid())
    {
        feet = gAgentAvatarp->getPositionAgent().mV[VZ] - gAgentAvatarp->getPelvisToFoot();
    }
    F32 nav_floor = 0.f;
    const bool near_feet = SKNavMesh::instanceExists() &&
                           SKNavMesh::instance().floorUnder(at, sknav::AgentClass::AVATAR, nav_floor) &&
                           nav_floor > feet - 0.75f && nav_floor < feet + 0.5f;
    floor_z = near_feet ? nav_floor : feet;
    cut_z = floor_z + (F32)cut;
}

namespace
{
    // The water surface over a global point, from its region (else the agent's); very low where there is none.
    F32 waterAt(const LLVector3d& pos)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos);
        region = region ? region : gAgent.getRegion();
        return region ? region->getWaterHeight() : -1.0e6f;
    }
}

void SKOverviewCapture::takeTacticalCamera()
{
    if (!mTacticalCam.mActive)
    {
        static LLCachedControl<F32> distance_setting(gSavedSettings, "SKTacticalDistance", 60.f);
        mTacticalCam.mActive = true;
        mTacticalCam.mFocus = gAgent.getPosGlobalFromAgent(mTacticalTarget);
        mTacticalCam.mYaw = mTacticalYaw;
        mTacticalCam.mPitch = mTacticalPitch;
        mTacticalCam.mDistance = llclamp((F32)distance_setting, TACTICAL_MIN_M, TACTICAL_MAX_M);
    }
}

bool SKOverviewCapture::tacticalFocusAt(S32 x, S32 y)
{
    if (!tacticalShown())
    {
        return false;
    }
    SKOverviewCapture& self = instance();
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - self.mTacticalViewport[0], y * scale.mV[VY] - self.mTacticalViewport[1]);
    LLVector3d world;
    if (!self.groundAt(at, world))
    {
        return false;
    }
    self.takeTacticalCamera();
    TacticalCamera& cam = self.mTacticalCam;
    const F64 ANIM_EPSILON_SQUARED = 0.0001;
    if ((world - cam.mFocus).magVecSquared() > ANIM_EPSILON_SQUARED)
    {
        // LLAgentCamera::startCameraAnimation and setAnimationDuration: from where it is now, over ZoomTime, never
        // cutting short an ease already under way.
        const F32 duration = gSavedSettings.getF32("ZoomTime");
        cam.mAnimDuration = cam.mAnimating ? llmax(duration, cam.mAnimDuration - cam.mAnimTimer.getElapsedTimeF32().value()) : duration;
        cam.mStartEye = self.mTacticalEyeGlobal;
        cam.mStartFocus = self.mTacticalFocusGlobal;
        cam.mAnimTimer.reset();
        cam.mAnimating = true;
    }
    cam.mFocus = world;
    return true;
}

void SKOverviewCapture::tacticalOrbitAround(F32 radians)
{
    instance().takeTacticalCamera();
    instance().mTacticalCam.mYaw += radians;
}

void SKOverviewCapture::tacticalOrbitOver(F32 radians)
{
    SKOverviewCapture& self = instance();
    self.takeTacticalCamera();
    self.mTacticalCam.mPitch = llclamp(self.mTacticalCam.mPitch + radians, TACTICAL_MIN_PITCH, TACTICAL_MAX_PITCH);
}

void SKOverviewCapture::tacticalZoomIn(F32 fraction)
{
    SKOverviewCapture& self = instance();
    self.takeTacticalCamera();
    self.mTacticalCam.mDistance = llclamp(self.mTacticalCam.mDistance * fraction, TACTICAL_MIN_M, TACTICAL_MAX_M);
}

void SKOverviewCapture::tacticalPanLeft(F32 metres)
{
    SKOverviewCapture& self = instance();
    self.takeTacticalCamera();
    self.mTacticalCam.mFocus += LLVector3d(self.mTacticalLeft * metres);
}

void SKOverviewCapture::tacticalPanUp(F32 metres)
{
    SKOverviewCapture& self = instance();
    self.takeTacticalCamera();
    self.mTacticalCam.mFocus += LLVector3d(self.mTacticalUp * metres);
}

F32 SKOverviewCapture::tacticalFocusDistance()
{
    return instanceExists() ? (F32)(instance().mTacticalEyeGlobal - instance().mTacticalFocusGlobal).magVec() : 0.f;
}

void SKOverviewCapture::resetTacticalCamera()
{
    if (instanceExists())
    {
        instance().mTacticalCam = TacticalCamera();
    }
}

void SKOverviewCapture::tacticalCentre(LLVector3d& centre) const
{
    // Follow: the avatar. Alt-cam: the camera's focus target. Flycam: the surface under the camera.
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        centre = gAgent.getPosGlobalFromAgent(eye);
        F32 ground = LLWorld::getInstance()->resolveLandHeightGlobal(centre);
        LLVector4a start, end, hit;
        start.load3(eye.mV);
        end.set(eye.mV[VX], eye.mV[VY], llmin(ground, eye.mV[VZ]) - 1.f, 1.f);
        S32 face = -1;
        if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, &hit))
        {
            ground = llmax(ground, hit.getF32ptr()[VZ]);
        }
        centre.mdV[VZ] = llmax(ground, waterAt(centre)); // over water the surface, not the seabed
        return;
    }
    centre = gAgentCamera.getFocusOnAvatar() ? gAgent.getPositionGlobal() : gAgentCamera.getFocusTargetGlobal();
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
    if (enabled)
    {
        captureMaps(LLFrameTimer::getUptimeSeconds().value());
    }
    else
    {
        mMaps.clear();
    }
    if (tactical_on)
    {
        tactical();
    }
}

void SKOverviewCapture::resolve(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot)
{
    const S32 w = capture.getWidth();
    const S32 h = capture.getHeight();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview resolve");
        LL_PROFILE_GPU_ZONE("sk overview resolve");
        LLTimer timer;
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
        glUniform3f(glGetUniformLocation(prog, "sk_focus"), shot.mFocus.mV[VX], shot.mFocus.mV[VY], shot.mFocus.mV[VZ]);
        glUniform1f(glGetUniformLocation(prog, "sk_cut_flag"), shot.mPartCuts ? 1.f : 0.f);
        glUniform1f(glGetUniformLocation(prog, "sk_cut"), shot.mCut);
        glUniform1f(glGetUniformLocation(prog, "sk_water_z"), shot.mWater);
        glUniform1f(glGetUniformLocation(prog, "sk_water_depth"), skmap::palette::WATER_DEPTH_M);
        uniformColour(prog, "sk_invisible", skmap::palette::DROP_EDGE);
        static LLCachedControl<U32> debug_view(gSavedSettings, "SKOverviewDebugView", 0);
        glUniform1i(glGetUniformLocation(prog, "sk_debug_view"), (GLint)(U32)debug_view);
        {
            // The territory: the regions styled as inside, at most SK_MAX_TERRITORY of them.
            std::vector<LLVector4> rects = shot.mTerritory.empty() ? std::vector<LLVector4>{ shot.mRegion } : shot.mTerritory;
            if (rects.size() > SK_MAX_TERRITORY)
            {
                rects.resize(SK_MAX_TERRITORY);
            }
            glUniform4fv(glGetUniformLocation(prog, "sk_regions"), (GLsizei)rects.size(), rects.front().mV);
            glUniform1i(glGetUniformLocation(prog, "sk_region_count"), (GLint)rects.size());
        }
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTextureSlot(1)->unbind();
        gGL.getTextureSlot(0)->unbind();
        sResolve.unbind();
        resolved.flush();
        mCost.mResolveMs += timer.getElapsedTimeF32() * 1000.f;
    }

    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
}

void SKOverviewCapture::drawMovers(const Shot& shot)
{
    LL_PROFILE_ZONE_SCOPED;
    const S32 w = sTacticalCapture.getWidth();
    const S32 h = sTacticalCapture.getHeight();
    if (!sTacticalWork.isComplete() || (S32)sTacticalWork.getWidth() != w || (S32)sTacticalWork.getHeight() != h)
    {
        sTacticalWork.release();
        if (!sTacticalWork.allocate(w, h, GL_RGBA8, true, false, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                                    LLRenderTarget::DEPTH_FMT_32F))
        {
            LL_WARNS_ONCE("SKOverview") << "tactical mover target did not allocate" << LL_ENDL;
            return;
        }
    }
    {
        LLGLDisable scissor(GL_SCISSOR_TEST); // a blit is scissored too
        sTacticalWork.copyContents(sTacticalCapture, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }

    sTacticalWork.bindTarget();
    glViewport(0, 0, w, h);
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(shot.mProj);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    {
        // Whole: no cut plane, and the class code that styles them green.
        LLMatrix4a inverse_view;
        inverse_view.setInverse(shot.mView);
        const F32* m = inverse_view.getF32ptr();
        const F32 eye_to_world[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
        const F32 no_cut[4] = { 0.f, 0.f, 0.f, 1.f };
        sGeom.bind();
        glUniform4fv(glGetUniformLocation(sGeom.mProgramObject, "clip_plane"), 1, no_cut);
        glUniformMatrix3fv(glGetUniformLocation(sGeom.mProgramObject, "sk_eye_to_world"), 1, GL_FALSE, eye_to_world);
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
        LLGLDisable cull_face(GL_CULL_FACE);
        gGL.setColorMask(true, true, false, true);
        mMovers.draw(shot.mView);
        gGL.setColorMask(true, true);
        sGeom.unbind();
    }
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
    sTacticalWork.flush();
    resolve(sTacticalWork, sTacticalResolved, shot);
}

U32 SKOverviewCapture::render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, bool resolve_now)
{
    const S32 w = capture.getWidth();
    const S32 h = capture.getHeight();
    S32 saved_viewport[4];
    memcpy(saved_viewport, gGLViewport, sizeof(saved_viewport));
    const U32 saved_res_x = LLRenderTarget::sCurResX;
    const U32 saved_res_y = LLRenderTarget::sCurResY;
    gGLViewport[0] = gGLViewport[1] = 0;
    gGLViewport[2] = w;
    gGLViewport[3] = h;

    LLCamera cull = shot.mCull;
    LLViewerCamera::updateFrustumPlanes(cull, false, false, true);

    // The census, region by region, a class a range, nearer than the terrain where they coincide.
    const std::vector<SKOverviewCensus::Batch>& batches = mCensus.batches();
    std::function<void()> census;
    if (!batches.empty())
    {
        census = [&batches, &shot]()
        {
            // The census writes its class code and the triangle's own normal (G, A), so flush parts never crease.
            LLVector4a down, normal_eye, origin_eye, zero;
            down.set(0.f, 0.f, -1.f, 0.f);
            shot.mView.rotate(down, normal_eye);
            zero.clear();
            shot.mView.affineTransform(zero, origin_eye);
            const F32 plane[4] = { normal_eye.getF32ptr()[0], normal_eye.getF32ptr()[1], normal_eye.getF32ptr()[2],
                                   shot.mCut - normal_eye.dot3(origin_eye).getF32() };
            const F32 no_cut[4] = { 0.f, 0.f, 0.f, 1.f };
            // The tactical view cuts each part at its own height (SKOverviewCensus::applyCut); a map at the one plane.
            LLGLSLShader& geom = shot.mPartCuts ? sGeomCut : sGeom;
            LLMatrix4a inverse_view;
            inverse_view.setInverse(shot.mView);
            const F32* m = inverse_view.getF32ptr();
            const F32 eye_to_world[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
            // A class's range whole, or only the groups the shot's level keeps.
            auto drawClass = [&shot](const SKOverviewCensus::Batch& batch, U32 c)
            {
                if (shot.mLevel.mAll)
                {
                    batch.mBuffer->drawArrays(LLRender::TRIANGLES, batch.mStart[c], batch.mStart[c + 1] - batch.mStart[c]);
                    return;
                }
                for (const SKOverviewCensus::Group& group : batch.mGroups)
                {
                    if (group.mCount[c] && shot.mLevel.keeps(group))
                    {
                        batch.mBuffer->drawArrays(LLRender::TRIANGLES, group.mStart[c], group.mCount[c]);
                    }
                }
            };
            geom.bind();
            glUniform4fv(glGetUniformLocation(geom.mProgramObject, "clip_plane"), 1, shot.mPartCuts ? no_cut : plane);
            glUniformMatrix3fv(glGetUniformLocation(geom.mProgramObject, "sk_eye_to_world"), 1, GL_FALSE, eye_to_world);
            // B carries a cut part's cap flag; the invisible pass after it raises it to 1 where it draws.
            gGL.setColorMask(true, true, shot.mPartCuts, true);
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
                        if (!count || c == skoverview::INVISIBLE)
                        {
                            continue;
                        }
                        const U8 code = pass == 0 ? skoverview::codeOf((skoverview::CensusClass)c) : (U8)skoverview::CODE_SECTION;
                        gGL.diffuseColor4f(code / 255.f, 0.f, 0.f, 1.f);
                        // Coplanar parts of different classes resolve the same way every frame: floor over ramp over
                        // cover over wall over the rest, by a class's own depth offset rather than draw order.
                        static const F32 PRIORITY[skoverview::NUM_CENSUS_CLASSES] = { 8.f, 2.f, 4.f, 6.f, 1.f, 1.f, 1.f, 0.f, 0.f };
                        gGL.setPolygonOffset(-1.f, -4.f - PRIORITY[c]);
                        drawClass(batch, c);
                    }
                }
                glCullFace(GL_BACK);
            }
            {
                // Transparent parts small enough to be invisible walls: the hatch flag, hiding nothing.
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
                LLGLDisable cull_face(GL_CULL_FACE);
                gGL.setColorMask(false, false, true, false);
                for (const SKOverviewCensus::Batch& batch : batches)
                {
                    const U32 start = batch.mStart[skoverview::INVISIBLE];
                    const U32 count = batch.mStart[skoverview::INVISIBLE + 1] - start;
                    if (!count)
                    {
                        continue;
                    }
                    const LLVector3 offset_agent = gAgent.getPosAgentFromGlobal(batch.mOrigin);
                    gGL.loadMatrix(shot.mView);
                    gGL.translatef(offset_agent.mV[VX], offset_agent.mV[VY], offset_agent.mV[VZ]);
                    gGLLastMatrix = NULL;
                    batch.mBuffer->setBuffer();
                    gGL.diffuseColor4f(0.f, 0.f, 1.f, 1.f);
                    drawClass(batch, skoverview::INVISIBLE);
                }
                gGL.setColorMask(true, false, false, false);
            }
            gGL.setPolygonOffset(0.f, 0.f);
            geom.unbind();
        };
    }

    U32 draws = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview capture");
        LLTimer timer;
        capture.bindTarget();
        glViewport(0, 0, w, h);
        GLfloat clear_colour[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_colour);
        GLfloat clear_depth = 1.f;
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClearDepth(al_window_far()); // the cleared depth must be the far plane of the current convention
        {
            // A clear obeys the write masks: whatever pass ran last could have left them on one channel, and the
            // terrain writes only R, so its texels kept the last capture's normals (G, A) and invisible flag (B)
            // and smeared as the camera turned. Every channel and the depth are cleared, every capture.
            LLGLDepthTest depth_write(GL_TRUE, GL_TRUE, GL_ALWAYS);
            LLGLDisable scissor(GL_SCISSOR_TEST); // and a scissor left on would clear only part of it
            gGL.setColorMask(true, true);
            capture.clear();
        }
        glClearDepth(clear_depth);
        glClearColor(clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3]);
        draws = gPipeline.skRenderOverview(shot.mView, shot.mProj, cull, shot.mCut, census);
        capture.flush();
        mCost.mCaptureMs += timer.getElapsedTimeF32() * 1000.f;
    }

    if (resolve_now)
    {
        resolve(capture, resolved, shot);
    }

    memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;
    return draws;
}

void SKOverviewCapture::captureMaps(F64 now)
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    const auto& regions = LLWorld::getInstance()->getRegionList();
    // Regions that left the world take their maps with them.
    mMaps.erase(std::remove_if(mMaps.begin(), mMaps.end(),
                               [&regions](const std::unique_ptr<RegionMap>& map)
                               {
                                   return std::none_of(regions.begin(), regions.end(), [&map](LLViewerRegion* r)
                                                       { return r->getHandle() == map->mHandle; });
                               }),
                 mMaps.end());

    // A region's ring: 0 the agent's, 1 one touching it, then one more per region width of gap between them.
    const LLVector3d agent_min = agent_region->getOriginGlobal();
    const F64 agent_w = agent_region->getWidth();
    auto ringOf = [&](LLViewerRegion* region) -> U32
    {
        if (region == agent_region)
        {
            return 0;
        }
        const LLVector3d min = region->getOriginGlobal();
        const F64 w = region->getWidth();
        const F64 gap_x = llmax(min.mdV[VX] - (agent_min.mdV[VX] + agent_w), agent_min.mdV[VX] - (min.mdV[VX] + w), 0.0);
        const F64 gap_y = llmax(min.mdV[VY] - (agent_min.mdV[VY] + agent_w), agent_min.mdV[VY] - (min.mdV[VY] + w), 0.0);
        return 1 + (U32)(llmax(gap_x, gap_y) / llmax(agent_w, 1.0) + 0.001);
    };

    static LLCachedControl<F32> refresh(gSavedSettings, "SKOverviewRefresh", 1.f);
    static LLCachedControl<F32> near_refresh(gSavedSettings, "SKOverviewNeighbourRefresh", 5.f);
    static LLCachedControl<F32> far_refresh(gSavedSettings, "SKOverviewFarRefresh", 30.f);
    static LLCachedControl<F32> slot(gSavedSettings, "SKOverviewQueueInterval", 0.25f);
    RegionMap* missing = nullptr;
    LLViewerRegion* missing_region = nullptr;
    for (LLViewerRegion* region : regions)
    {
        auto it = std::find_if(mMaps.begin(), mMaps.end(),
                               [region](const std::unique_ptr<RegionMap>& map) { return map->mHandle == region->getHandle(); });
        if (it == mMaps.end())
        {
            mMaps.push_back(std::make_unique<RegionMap>());
            mMaps.back()->mHandle = region->getHandle();
            it = std::prev(mMaps.end());
        }
        RegionMap& map = **it;
        map.mRegion = region;
        map.mRing = ringOf(region);
        if (map.mRing == 0)
        {
            // The agent's region on its own timer, and at once when the agent has just crossed into it uncut.
            if (map.mLast < 0.0 || !map.mCut || now - map.mLast >= llmax((F32)refresh, 0.05f))
            {
                if (!captureMap(map, region, true))
                {
                    return; // a GL failure releases the maps
                }
                map.mLast = now;
            }
            continue;
        }
        // A region with no map yet, or still showing the cut it had as the agent's, goes first, nearest first.
        if ((map.mLast < 0.0 || map.mCut) && (!missing || map.mRing < missing->mRing))
        {
            missing = &map;
            missing_region = region;
        }
    }
    if (missing)
    {
        // One a frame, so a ring coming into view does not cost a burst.
        if (captureMap(*missing, missing_region, false))
        {
            missing->mLast = now;
        }
        return;
    }
    if (now < mNextSlot || mMaps.empty())
    {
        return;
    }
    // The rest take turns: one capture a slot, round robin from where the last turn stopped, each region only once
    // its own period has passed: neighbours every few seconds, regions further off much less often.
    const size_t n = mMaps.size();
    for (size_t step = 0; step < n; ++step)
    {
        const size_t at = (mCursor + step) % n;
        RegionMap& map = *mMaps[at];
        if (map.mRing == 0 || !map.mRegion)
        {
            continue;
        }
        const F64 period = map.mRing == 1 ? llmax((F32)near_refresh, 0.1f) : llmax((F32)far_refresh, 1.f) * (map.mRing - 1);
        if (now - map.mLast < period)
        {
            continue;
        }
        mCursor = (at + 1) % n;
        mNextSlot = now + llmax((F32)slot, 0.f);
        if (captureMap(map, map.mRegion, false))
        {
            map.mLast = now;
        }
        return;
    }
}

bool SKOverviewCapture::captureMap(RegionMap& map, LLViewerRegion* region, bool agent)
{
    static LLCachedControl<U32> res_setting(gSavedSettings, "SKOverviewResolution", 1024);
    const S32 full = llclamp((S32)(U32)res_setting, 64, 4096);
    const S32 res = agent ? full : llmax(full >> (map.mRing == 1 ? 1 : 2), 64);
    LLRenderTarget& capture = agent ? sCapture : map.mRing == 1 ? sCaptureNear : sCaptureFar;
    if (!ensureGL(capture, map.mResolved, res, res))
    {
        return false;
    }

    LLTimer cpu_timer;
    const F32 width = region->getWidth();
    const LLVector3 origin = region->getOriginAgent();
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    Shot shot;
    // The agent's own region is cut at its storey. The others are not cut: they keep only the clusters at the agent's
    // level, the ground's when the agent is on it, and show them whole, roofs and all.
    if (!agent)
    {
        const F32 land = LLWorld::getInstance()->resolveLandHeightGlobal(gAgent.getPositionGlobal());
        shot.mLevel.mAll = false;
        shot.mLevel.mGround = floor_z - land <= ON_GROUND_M;
        shot.mLevel.mLo = floor_z - LEVEL_BELOW_M;
        shot.mLevel.mHi = floor_z + LEVEL_ABOVE_M;
    }
    const F32 top = agent ? cut_z : llmax(mCensus.levelTop(shot.mLevel, cut_z), region->getLand().getMaxZ()) + 1.f;
    const F32 bottom = llmin(top - 64.f, -32.f);
    const LLVector3 centre(origin.mV[VX] + width * 0.5f, origin.mV[VY] + width * 0.5f, top);

    // Straight down, east to the right and north up; the ortho near plane at the eye is the cut as well.
    LLVector4a eye4, dir4, up4;
    eye4.load3(centre.mV);
    dir4.set(0.f, 0.f, -1.f, 0.f);
    up4.set(0.f, 1.f, 0.f, 0.f);
    shot.mView = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 half = width * 0.5f;
    shot.mProj = al_ortho(-half, half, -half, half, 0.f, top - bottom);
    shot.mEye = centre + LLVector3(0.f, 0.f, 1000.f); // far above: outline tolerance as for a distant camera
    shot.mCut = top;
    shot.mFocus = LLVector3(centre.mV[VX], centre.mV[VY], floor_z);
    shot.mWater = region->getWaterHeight();
    // Another region is neither dimmed as outside the agent's nor edged as it is: its rectangle is unbounded.
    shot.mRegion = agent ? LLVector4(origin.mV[VX], origin.mV[VY], origin.mV[VX] + width, origin.mV[VY] + width)
                         : LLVector4(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f);
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

    const bool time = agent && !mTiming;
    if (time)
    {
        glQueryCounter(sQueries[0], GL_TIMESTAMP);
    }
    const U32 draws = render(capture, map.mResolved, shot);
    if (time)
    {
        glQueryCounter(sQueries[1], GL_TIMESTAMP);
        mTiming = true;
    }

    map.mOrigin = region->getOriginGlobal();
    map.mWidth = width;
    map.mCut = agent;
    map.mValid = true;
    if (agent)
    {
        mStats.resolution = res;
        mStats.draws = draws;
        mStats.census_tris = mCensus.triangles();
        mStats.cpu_ms = cpu_timer.getElapsedTimeF32() * 1000.f;
        ++mStats.captures;
    }
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

    // Orbit the centre the camera follows at the main camera's heading; the cut and the shading stay the avatar's.
    LLVector3d centre;
    tacticalCentre(centre);
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    LLVector3 target = gAgent.getPosAgentFromGlobal(centre);
    if (gAgentCamera.getFocusOnAvatar() && !LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        target.mV[VZ] = floor_z;
    }
    LLVector3 heading = LLViewerCamera::getInstance()->getAtAxis();
    heading.mV[VZ] = 0.f;
    if (heading.normVec() < 1.0e-3f)
    {
        heading.setVec(1.f, 0.f, 0.f);
    }
    F32 pitch = llclamp((F32)pitch_setting, 5.f, 90.f) * DEG_TO_RAD;
    if (mTacticalCam.mActive)
    {
        // Alt-cam has taken the tactical camera: its own focus, heading and tilt.
        target = gAgent.getPosAgentFromGlobal(mTacticalCam.mFocus);
        heading.setVec(std::cos(mTacticalCam.mYaw), std::sin(mTacticalCam.mYaw), 0.f);
        pitch = mTacticalCam.mPitch;
    }
    mTacticalTarget = target;
    mTacticalYaw = std::atan2(heading.mV[VY], heading.mV[VX]);
    mTacticalPitch = pitch;
    const F32 distance = mTacticalCam.mActive ? mTacticalCam.mDistance : llclamp((F32)distance_setting, TACTICAL_MIN_M, TACTICAL_MAX_M);
    const F32 pitch_safe = llmin(pitch, 89.5f * DEG_TO_RAD);
    LLVector3 eye = target - heading * (distance * std::cos(pitch_safe)) + LLVector3(0.f, 0.f, distance * std::sin(pitch_safe));
    if (mTacticalCam.mAnimating)
    {
        // An alt+click's ease, as LLAgentCamera::updateCamera: linear in time, smoothstepped, eye and focus together.
        const F32 fraction = mTacticalCam.mAnimDuration > 0.f ? mTacticalCam.mAnimTimer.getElapsedTimeF32().value() / mTacticalCam.mAnimDuration : 1.f;
        if (fraction < 1.f)
        {
            const F32 smooth = llsmoothstep(0.f, 1.f, fraction);
            eye = lerp(gAgent.getPosAgentFromGlobal(mTacticalCam.mStartEye), eye, smooth);
            target = lerp(gAgent.getPosAgentFromGlobal(mTacticalCam.mStartFocus), target, smooth);
        }
        else
        {
            mTacticalCam.mAnimating = false;
        }
    }
    mTacticalEyeGlobal = gAgent.getPosGlobalFromAgent(eye);
    mTacticalFocusGlobal = gAgent.getPosGlobalFromAgent(target);
    LLVector3 dir = target - eye;
    dir.normVec();
    LLVector3 left = LLVector3::z_axis % dir;
    left.normVec();
    const LLVector3 up = dir % left;
    mTacticalLeft = left;
    mTacticalUp = up;

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
    shot.mFocus = target;
    shot.mPartCuts = true;
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLVector3 region_min = region->getOriginAgent();
        const F32 region_width = region->getWidth();
        shot.mWater = region->getWaterHeight();
        shot.mRegion = LLVector4(region_min.mV[VX], region_min.mV[VY], region_min.mV[VX] + region_width,
                                 region_min.mV[VY] + region_width);
        // Every connected region is territory: the agent's first, then its neighbours.
        shot.mTerritory.push_back(shot.mRegion);
        for (LLViewerRegion* other : LLWorld::getInstance()->getRegionList())
        {
            if (other == region || shot.mTerritory.size() >= SK_MAX_TERRITORY)
            {
                continue;
            }
            const LLVector3 o = other->getOriginAgent();
            const F32 w = other->getWidth();
            shot.mTerritory.push_back(LLVector4(o.mV[VX], o.mV[VY], o.mV[VX] + w, o.mV[VY] + w));
        }
    }
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
        // Captured again only when what it shows changed: the camera beyond a small epsilon, the cut, the census
        // buffers, the region or the view's size. Otherwise last frame's image is composited again.
        const LLVector3 at_axis = dir;
        static LLCachedControl<bool> force(gSavedSettings, "SKOverviewForceRecapture", false);
        // Cut by building: the agent's own at its level, and each other building in its region that holds someone
        // the overview draws a marker for at its highest occupant's level; the rest, and other regions, whole.
        {
            static LLCachedControl<F32> cut_height(gSavedSettings, "SKOverviewCutHeight", 2.5f);
            std::vector<SKOverviewCensus::Occupant> occupants;
            SKOverviewCensus::Occupant self;
            self.mFloor = gAgent.getPosGlobalFromAgent(LLVector3(gAgent.getPositionAgent().mV[VX], gAgent.getPositionAgent().mV[VY], floor_z));
            self.mCut = cut_z;
            occupants.push_back(self);
            for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
            {
                if (p.mVisibility == skmap::Visibility::HIDDEN || !p.mZKnown)
                {
                    continue;
                }
                SKOverviewCensus::Occupant o;
                o.mFloor = p.mPos;
                o.mFloor.mdV[VZ] = p.mWhere.mFound ? (F64)p.mWhere.mFloor : p.mPos.mdV[VZ] - AVATAR_HALF_HEIGHT_M;
                o.mCut = (F32)o.mFloor.mdV[VZ] + (F32)cut_height;
                occupants.push_back(o);
            }
            mCensus.applyCut(gAgent.getRegion(), occupants);
        }
        const bool same = !force && mTacticalKey.mValid && (eye - mTacticalKey.mEye).magVecSquared() < 1e-4f &&
                          at_axis * mTacticalKey.mDir > 0.999999f && fov == mTacticalKey.mFov && vw == mTacticalKey.mWidth &&
                          vh == mTacticalKey.mHeight && std::fabs(cut_z - mTacticalKey.mCut) < 1e-3f &&
                          mCensus.signature() == mTacticalKey.mCensus &&
                          mCensus.cutStamp() == mTacticalKey.mCutStamp && gAgent.getRegion() == mTacticalKey.mRegion;
        // Movers draw over a copy of the static capture each frame; the static capture itself is redone only on change.
        const bool movers = mMovers.update();
        if (!same)
        {
            LL_PROFILE_ZONE_NAMED("sk tactical render");
            render(sTacticalCapture, sTacticalResolved, shot, !movers);
            mTacticalKey.mValid = true;
            mTacticalKey.mEye = eye;
            mTacticalKey.mDir = at_axis;
            mTacticalKey.mFov = fov;
            mTacticalKey.mWidth = vw;
            mTacticalKey.mHeight = vh;
            mTacticalKey.mCut = cut_z;
            mTacticalKey.mCensus = mCensus.signature();
            mTacticalKey.mCutStamp = mCensus.cutStamp();
            mTacticalKey.mRegion = gAgent.getRegion();
            ++mCost.mRenders;
        }
        else
        {
            ++mCost.mReuses;
        }
        mSceneDepth = movers ? &sTacticalWork : &sTacticalCapture;
        if (movers)
        {
            drawMovers(shot);
        }
        else if (mMoversDrawn && same)
        {
            resolve(sTacticalCapture, sTacticalResolved, shot); // the last mover left: the static map again
        }
        mMoversDrawn = movers;
    }

    mTacticalView = shot.mView;
    mTacticalProj = shot.mProj;
    mTacticalCentre = target;
    mTacticalReach = distance + CENSUS_RANGE_M;
    mTacticalDistance = distance;
    mTacticalViewport[0] = vx;
    mTacticalViewport[1] = vy;
    mTacticalViewport[2] = vw;
    mTacticalViewport[3] = vh;
    mTacticalReady = true;
    LL_INFOS_ONCE("SKOverview") << "tactical overview rendered " << vw << "x" << vh << ", census tris " << mCensus.triangles()
                                << LL_ENDL;
}


// The tactical overview over the finished frame: after renderFinalize, which would otherwise paint the world over it.
bool SKOverviewCapture::worldHidden()
{
    // The overview was composited last frame (or this one) and is still on: it will cover the world view again, opaque.
    static LLCachedControl<bool> tactical_on(gSavedSettings, "SKTacticalOverview", false);
    return tactical_on && !gCubeSnapshot && instanceExists() &&
           instance().mLastCompositeFrame + 1 >= LLFrameTimer::getFrameCount();
}

bool SKOverviewCapture::tacticalShown()
{
    return instanceExists() && instance().mLastCompositeFrame == LLFrameTimer::getFrameCount();
}

// static
bool SKOverviewCapture::handleRightClick(S32 x, S32 y)
{
    // Input runs at the top of the frame after the one that composited.
    return instanceExists() && instance().mLastCompositeFrame + 1 >= LLFrameTimer::getFrameCount() && instance().rightClick(x, y);
}

bool SKOverviewCapture::rightClick(S32 x, S32 y)
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mTacticalViewport[0], y * scale.mV[VY] - mTacticalViewport[1]);
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= mTacticalViewport[2] || at.mV[VY] >= mTacticalViewport[3])
    {
        return false;
    }
    // The mini-map's pick: every marker within its hover radius, nearest first.
    const F32 radius = skmap::HOVER_ENTER_PX * llmax(scale.mV[VX], scale.mV[VY]);
    std::vector<std::pair<F32, LLUUID>> near_click;
    for (const TacticalMark& mark : mTacticalMarks)
    {
        const F32 dist = dist_vec(mark.mAt, at);
        if (dist <= radius)
        {
            near_click.emplace_back(dist, mark.mId);
        }
    }
    std::sort(near_click.begin(), near_click.end());
    std::vector<LLUUID> ids;
    for (const auto& n : near_click)
    {
        ids.push_back(n.second);
    }
    LLVector3d world;
    if (!groundAt(at, world))
    {
        world = gAgent.getPosGlobalFromAgent(mTacticalCentre);
    }
    if (SKPopupPanes* old = mPopup.get())
    {
        old->closeAll();
    }
    mPopup = SKMapMenu::open(gViewerWindow->getRootView(), x, y, world, SKMapMenu::freeze(ids));
    return true;
}

bool SKOverviewCapture::groundAt(const LLVector2& at, LLVector3d& world) const
{
    LLMatrix4a vp, inv_vp, inv_view;
    vp.setMul(mTacticalView, mTacticalProj);
    if (!inv_vp.setInverse(vp) || !inv_view.setInverse(mTacticalView))
    {
        return false;
    }
    LLVector4a origin, eye, ndc, far_pt;
    origin.set(0.f, 0.f, 0.f, 1.f);
    inv_view.transform4(origin, eye);
    ndc.set(at.mV[VX] / mTacticalViewport[2] * 2.f - 1.f, at.mV[VY] / mTacticalViewport[3] * 2.f - 1.f, 0.5f, 1.f);
    inv_vp.transform4(ndc, far_pt);
    const F32* e = eye.getF32ptr();
    const F32* f = far_pt.getF32ptr();
    if (std::fabs(e[3]) < 1e-6f || std::fabs(f[3]) < 1e-6f)
    {
        return false;
    }
    const LLVector3 from(e[0] / e[3], e[1] / e[3], e[2] / e[3]);
    const LLVector3 dir = LLVector3(f[0] / f[3], f[1] / f[3], f[2] / f[3]) - from;
    const F32 plane_z = mTacticalCentre.mV[VZ];
    if (dir.mV[VZ] > -1e-6f || from.mV[VZ] < plane_z)
    {
        return false;
    }
    const F32 t = (plane_z - from.mV[VZ]) / dir.mV[VZ];
    world = gAgent.getPosGlobalFromAgent(from + dir * t);
    return true;
}

void SKOverviewCapture::onComposite()
{
    static LLCachedControl<bool> tactical_on(gSavedSettings, "SKTacticalOverview", false);
    if (!tactical_on || !instanceExists())
    {
        return;
    }
    instance().composite();
}


template <typename Project>
void SKOverviewCapture::drawMarkers3D(const Project& project, S32 vw, S32 vh, std::vector<std::pair<size_t, LLVector2>>& drawn)
{
    LL_PROFILE_ZONE_NAMED("sk tactical markers 3d");
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    LLMatrix4a inverse_view;
    inverse_view.setInverse(mTacticalView);
    const LLVector3 eye(inverse_view.getF32ptr()[12], inverse_view.getF32ptr()[13], inverse_view.getF32ptr()[14]);
    const LLVector3 light = LLVector3(-0.45f, 0.45f, 0.77f) / LLVector3(-0.45f, 0.45f, 0.77f).length();

    struct Marker
    {
        LLVector3 mFloor;   // agent space, the avatar's feet
        F32 mHeading = 0.f;
        bool mHasHeading = false;
        LLColor4 mColour;
        size_t mPerson = (size_t)-1;
        LLUUID mId;
        bool mCoarse = false;   // known only from the region's coarse locations: no rotation, a coarse height
        F32 mHeight = 0.f;      // the avatar's own height when known, else the default
        F32 mRadius = 0.f;      // half the larger of its x and y size when known, else in proportion to the height
        bool mHasShape = false; // drawn as its physics shape, from its agent size, pose and rotation
        LLVector3 mCentre;      // agent space, the avatar's position: the shape's origin
        LLQuaternion mRotation;
        skcensus::AvatarPose mPose = skcensus::AvatarPose::STANDING;
        bool mHasHead = false;  // its head joint, the height the heading wedge is drawn at
        LLVector3 mHead;
    };
    // A loaded avatar's own size, as the sim has it: getScale() is the agent size from its object update (what
    // llGetAgentSize reports), its box centred on the avatar's position, so its base is the floor the sim stands it
    // on. The position is the object's, not the render position, which carries the hover offset. Sitting keeps the
    // same size. Only when no size has come is the shape's body height used, on its pelvis-to-foot.
    auto sized = [](Marker& m, const LLUUID& id)
    {
        LLViewerObject* object = gObjectList.findObject(id);
        LLVOAvatar* avatar = object && object->isAvatar() ? static_cast<LLVOAvatar*>(object) : nullptr;
        if (!avatar || avatar->isDead())
        {
            return;
        }
        const LLVector3 size = avatar->getScale();
        const LLVector3 centre = avatar->getPositionAgent();
        if (size.mV[VZ] > 0.1f)
        {
            m.mFloor = centre - LLVector3(0.f, 0.f, size.mV[VZ] * 0.5f);
            m.mHeight = size.mV[VZ];
            m.mRadius = llmax(size.mV[VX], size.mV[VY]) * 0.5f;
            // The census's physics shape for its pose, about its position, turned as the sim has the avatar.
            m.mHasShape = true;
            m.mCentre = centre;
            m.mRotation = avatar->isSelf() ? avatar->getRenderRotation() : avatar->skPredictedRotationRegion();
            m.mPose = skcensus::avatarPose(avatar);
        }
        else if (avatar->mBodySize.mV[VZ] > 0.1f)
        {
            m.mFloor = avatar->getRenderPosition() - LLVector3(0.f, 0.f, avatar->getPelvisToFoot());
            m.mHeight = avatar->mBodySize.mV[VZ];
        }
        if (avatar->mHeadp)
        {
            m.mHead = avatar->mHeadp->getWorldPosition();
            m.mHasHead = true;
        }
    };
    std::vector<Marker> markers;
    for (size_t i = 0; i < people.size(); ++i)
    {
        const SKMapPeople::Person& p = people[i];
        Marker m;
        // Tiers by what is known (SKMapPeople's own flags): the avatar object (live position and rotation), or only a
        // coarse location, whose height may be the capped value, in which case it stands on the land under it.
        m.mCoarse = !p.mLoaded;
        LLVector3d where = p.mPos;
        if (!p.mZKnown)
        {
            where.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(where) + AVATAR_HALF_HEIGHT_M;
        }
        m.mFloor = gAgent.getPosAgentFromGlobal(where) - LLVector3(0.f, 0.f, AVATAR_HALF_HEIGHT_M);
        m.mHeading = p.mHeading;
        m.mHasHeading = p.mHasHeading && !m.mCoarse;
        const skmap::PaletteColour& coded = skmap::palette::DOT_COMBAT;
        m.mColour = p.mVisibility == skmap::Visibility::DOT_CODED ? LLColor4(coded.mR, coded.mG, coded.mB, 1.f)
                                                                  : LLColor4(p.mColour.mV[0], p.mColour.mV[1], p.mColour.mV[2], 1.f);
        if (m.mCoarse)
        {
            m.mColour = LLColor4(m.mColour.mV[0] * 0.7f, m.mColour.mV[1] * 0.7f, m.mColour.mV[2] * 0.7f, 0.85f);
        }
        else
        {
            sized(m, p.mId);
        }
        m.mPerson = i;
        m.mId = p.mId;
        markers.push_back(m);
    }
    if (isAgentAvatarValid())
    {
        Marker m;
        m.mFloor = gAgent.getPositionAgent() - LLVector3(0.f, 0.f, AVATAR_HALF_HEIGHT_M);
        sized(m, gAgent.getID());
        const LLVector3 fwd = gAgent.getAtAxis();
        m.mHeading = std::atan2(fwd.mV[VY], fwd.mV[VX]);
        m.mHasHeading = true;
        const skmap::PaletteColour& self = skmap::palette::SELF_DOT;
        m.mColour = LLColor4(self.mR, self.mG, self.mB, 1.f);
        markers.push_back(m);
    }
    if (markers.empty() || !sMarker.mProgramObject || !mSceneDepth || !mSceneDepth->isComplete())
    {
        return;
    }

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(mTacticalProj);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(mTacticalView);
    gGLLastMatrix = NULL;
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    LLGLDisable cull_face(GL_CULL_FACE);
    sMarker.bind();
    gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, mSceneDepth->getDepth(), gGL.getSampler(ALSamplers::PointClamp));
    glUniform1i(glGetUniformLocation(sMarker.mProgramObject, "sceneDepth"), 0);
    glUniform4f(glGetUniformLocation(sMarker.mProgramObject, "sk_viewport"), (F32)mTacticalViewport[0], (F32)mTacticalViewport[1],
                (F32)vw, (F32)vh);
    glUniform1f(glGetUniformLocation(sMarker.mProgramObject, "sk_zo"), LLRender::sReverseZ ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(sMarker.mProgramObject, "sk_ghost_alpha"), MARKER_GHOST_ALPHA);

    gGL.begin(LLRender::TRIANGLES);
    // A triangle facing the camera, lit by the overview's key light; those facing away are left out (no depth buffer).
    auto tri = [&](const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& colour)
    {
        LLVector3 n = (b - a) % (c - a);
        if (n.normVec() < 1e-8f)
        {
            return;
        }
        if (n * (eye - a) < 0.f)
        {
            return;
        }
        const F32 shade = 0.45f + 0.55f * llmax(n * light, 0.f);
        gGL.color4f(colour.mV[0] * shade, colour.mV[1] * shade, colour.mV[2] * shade, colour.mV[3]);
        gGL.vertex3fv(a.mV);
        gGL.vertex3fv(b.mV);
        gGL.vertex3fv(c.mV);
    };
    for (const Marker& m : markers)
    {
        // True size at every zoom: the sim's agent size, never grown for the screen.
        LLVector2 at0;
        if (!project(m.mFloor, at0))
        {
            continue;
        }
        // A known body sets the height, the radius in proportion.
        const F32 body = m.mHeight > 0.f ? m.mHeight / MARKER_HEIGHT_M : 1.f;
        const F32 r = (m.mRadius > 0.f ? m.mRadius : MARKER_RADIUS_M * body);
        const F32 h = (m.mCoarse ? MARKER_COARSE_HEIGHT_M : MARKER_HEIGHT_M) * body;
        const LLColor4 bright(llmin(1.f, m.mColour.mV[0] * 1.25f + 0.1f), llmin(1.f, m.mColour.mV[1] * 1.25f + 0.1f),
                              llmin(1.f, m.mColour.mV[2] * 1.25f + 0.1f), 1.f);
        LLVector3 top_c = m.mFloor + LLVector3(0.f, 0.f, h);
        if (m.mHasShape && !m.mCoarse)
        {
            // The avatar's own physics shape, about its position.
            const std::vector<LLVector3>& shape = skcensus::avatarShape(m.mPose, m.mHeight);
            F32 top_z = -F32_MAX;
            for (size_t v = 0; v + 2 < shape.size(); v += 3)
            {
                const LLVector3 a = m.mCentre + shape[v] * m.mRotation;
                const LLVector3 b = m.mCentre + shape[v + 1] * m.mRotation;
                const LLVector3 c = m.mCentre + shape[v + 2] * m.mRotation;
                tri(a, b, c, m.mColour);
                top_z = llmax(top_z, llmax(a.mV[VZ], llmax(b.mV[VZ], c.mV[VZ])));
            }
            top_c = LLVector3(m.mCentre.mV[VX], m.mCentre.mV[VY], top_z);
        }
        else
        {
            for (S32 k = 0; k < MARKER_SIDES; ++k)
            {
                const F32 a0 = F_TWO_PI * k / MARKER_SIDES, a1 = F_TWO_PI * (k + 1) / MARKER_SIDES;
                const LLVector3 d0(std::cos(a0) * r, std::sin(a0) * r, 0.f), d1(std::cos(a1) * r, std::sin(a1) * r, 0.f);
                const LLVector3 b0 = m.mFloor + d0, b1 = m.mFloor + d1, t0 = top_c + d0, t1 = top_c + d1;
                tri(b0, b1, t1, m.mColour);
                tri(b0, t1, t0, m.mColour);
                tri(top_c, t0, t1, m.mColour);
            }
        }
        if (m.mHasShape && !m.mCoarse && m.mHasHead && m.mHasHeading)
        {
            // The minimap's heading wedge as a flat slab: a solid triangle just clear of the body at head height, along
            // the body's heading, in the minimap's proportions to the body's radius.
            {
                const LLVector3 ahead(std::cos(m.mHeading), std::sin(m.mHeading), 0.f);
                const LLVector3 across(-ahead.mV[VY], ahead.mV[VX], 0.f);
                const LLVector3 from(m.mCentre.mV[VX], m.mCentre.mV[VY], m.mCentre.mV[VZ] + (m.mHead.mV[VZ] - m.mCentre.mV[VZ]));
                const F32 body_r = m.mRadius;
                const LLVector3 tip = from + ahead * (body_r * (1.f + HEADING_TIP_PER_RADIUS));
                const LLVector3 base = from + ahead * (body_r * (1.f + HEADING_BASE_PER_RADIUS));
                const LLVector3 l = base + across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 rr = base - across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 up(0.f, 0.f, body_r * HEADING_THICK_PER_RADIUS * 0.5f);
                tri(tip + up, l + up, rr + up, m.mColour);
                tri(tip - up, rr - up, l - up, m.mColour);
                tri(tip - up, l - up, l + up, m.mColour);
                tri(tip - up, l + up, tip + up, m.mColour);
                tri(rr - up, tip - up, tip + up, m.mColour);
                tri(rr - up, tip + up, rr + up, m.mColour);
                tri(l - up, rr - up, rr + up, m.mColour);
                tri(l - up, rr + up, l + up, m.mColour);
            }
        }
        else if (m.mHasHeading)
        {
            // A wedge on the top, pointing along the heading, brighter than the body.
            const LLVector3 ahead(std::cos(m.mHeading), std::sin(m.mHeading), 0.f);
            const LLVector3 across(-ahead.mV[VY], ahead.mV[VX], 0.f);
            const LLVector3 lift(0.f, 0.f, 0.03f);
            const LLVector3 tip = top_c + lift + ahead * (r * 1.7f);
            const LLVector3 back = top_c + lift - ahead * (r * 0.35f);
            tri(back + across * (r * 0.75f), tip, back - across * (r * 0.75f), bright);
            tri(back - across * (r * 0.75f), tip, back + across * (r * 0.75f), bright);
        }
        // Labels, hover, picks and menus take the projected top of the marker.
        LLVector2 top;
        if (project(top_c, top))
        {
            if (m.mPerson != (size_t)-1)
            {
                mTacticalMarks.push_back({ m.mId, top });
                drawn.emplace_back(m.mPerson, top);
            }
        }
    }
    gGL.end();
    gGL.flush();
    gGL.getTextureSlot(0)->unbind();
    sMarker.unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
    // The 2D pass the caller resumes in.
    gUIProgram.bind();
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
    LLTimer composite_timer;
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
        // The mini-map's own markers and visibility rules (SKMapPeople, skmapdraw::personMarker), at a constant pixel
        // size; headings are the avatars' facings laid on the ground and projected through the tilted camera.
        LL_PROFILE_ZONE_NAMED("sk tactical markers");
        LLMatrix4a vp;
        vp.setMul(mTacticalView, mTacticalProj);
        auto project = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
        {
            LLVector4a p, clip;
            p.set(agent_pos.mV[VX], agent_pos.mV[VY], agent_pos.mV[VZ], 1.f);
            vp.transform4(p, clip);
            const F32 w = clip.getF32ptr()[3];
            if (w <= 1e-4f)
            {
                return false;
            }
            out.set((clip.getF32ptr()[0] / w * 0.5f + 0.5f) * vw, (clip.getF32ptr()[1] / w * 0.5f + 0.5f) * vh);
            return out.mV[VX] > -32.f && out.mV[VY] > -32.f && out.mV[VX] < vw + 32.f && out.mV[VY] < vh + 32.f;
        };
        auto ground_ahead = [&](const LLVector3& agent_pos, const LLVector2& at, F32 heading, LLVector2& ahead) -> bool
        {
            LLVector2 tip;
            if (!project(agent_pos + LLVector3(std::cos(heading), std::sin(heading), 0.f), tip))
            {
                return false;
            }
            ahead = tip - at;
            return ahead.normVec() > 1e-4f;
        };

        glViewport(vx, vy, vw, vh);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, (F32)vw, 0.f, (F32)vh, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGLLastMatrix = NULL;
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gUIProgram.bind();
        gGL.getTextureSlot(0)->unbind();

        static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
        const F32 px = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
        const LLVector3d centre = gAgent.getPosGlobalFromAgent(mTacticalCentre);
        const F64 reach = mTacticalReach;
        SKMapPeople::instance().noteDrawn(centre - LLVector3d(reach, reach, reach), centre + LLVector3d(reach, reach, reach));
        mTacticalMarks.clear();
        std::vector<std::pair<size_t, LLVector2>> drawn;
        drawMarkers3D(project, vw, vh, drawn);
        gGL.flush();
        drawTacticalLabels(drawn, vw, vh);
        gGL.flush();
        gUIProgram.unbind();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGLLastMatrix = NULL;
    }
    mLastCompositeFrame = LLFrameTimer::getFrameCount();
    mCost.mCompositeMs += composite_timer.getElapsedTimeF32() * 1000.f;
    ++mCost.mFrames;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (now - mCost.mSince >= 1.0)
    {
        const bool census_slow = SKWorldCensus::instanceExists() && SKWorldCensus::instance().slowCadence();
        const U32 scans_now = SKWorldCensus::instanceExists() ? SKWorldCensus::instance().scansBegun() : 0;
        const U32 census_scans = scans_now - mCost.mScansAtStart;
        if (mCost.mSince > 0.0 && mCost.mFrames)
        {
            const F32 n = (F32)mCost.mFrames;
            LL_INFOS("SKOverviewCost") << "tactical per frame over " << mCost.mFrames << " frames: capture "
                                       << mCost.mCaptureMs / n << " ms, resolve " << mCost.mResolveMs / n << " ms, composite+markers "
                                       << mCost.mCompositeMs / n << " ms; rendered " << mCost.mRenders << ", reused " << mCost.mReuses
                                       << ", world hidden " << (worldHidden() ? 1 : 0) << "; census "
                                       << (census_slow ? "slow" : "normal") << ", scans " << census_scans << LL_ENDL;
        }
        mCost = Cost();
        mCost.mSince = now;
        mCost.mScansAtStart = scans_now;
    }
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LL_INFOS_ONCE("SKOverview") << "tactical overview composited at " << vx << "," << vy << " " << vw << "x" << vh << LL_ENDL;
}

void SKOverviewCapture::drawTacticalLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn, S32 vw, S32 vh)
{
    static LLCachedControl<F32> enter(gSavedSettings, "SKMapHoverEnterPx", skmap::HOVER_ENTER_PX);
    static LLCachedControl<F32> exit(gSavedSettings, "SKMapHoverExitPx", skmap::HOVER_EXIT_PX);
    static LLCachedControl<U32> max_width(gSavedSettings, "SKMapNametagMaxWidth", 190);
    LL_PROFILE_ZONE_NAMED("sk tactical labels");
    // Labels work in UI pixels, as on the mini-map, so they keep its sizes under any display scale.
    const LLVector2 scale(LLFontGL::sScaleX, LLFontGL::sScaleY);
    const F32 ui_w = vw / scale.mV[VX], ui_h = vh / scale.mV[VY];
    auto ui = [&](const LLVector2& px) { return LLVector2(px.mV[VX] / scale.mV[VX], px.mV[VY] / scale.mV[VY]); };
    auto down = [&](const LLVector2& at) { return LLVector2(at.mV[VX], ui_h - at.mV[VY]); };
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();

    const LLFontGL::UIOrigin origin = LLFontGL::sCurOrigin;
    const F32 depth = LLFontGL::sCurDepth, cur_sx = LLFontGL::sCurScaleX, cur_sy = LLFontGL::sCurScaleY;
    LLFontGL::sCurOrigin.set(0.f, 0.f);
    LLFontGL::sCurDepth = 0.f;
    LLFontGL::sCurScaleX = LLFontGL::sCurScaleY = 1.f;
    gGL.pushUIMatrix();
    gGL.loadUIIdentity();
    gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);

    for (const auto& [index, at] : drawn)
    {
        if (index < people.size())
        {
            skmapdraw::personCode(people[index], ui(at), skmap::FloorRelation::SAME);
        }
    }

    // Who is under the pointer, by the mini-map's hover rules.
    const LLVector2 pointer(static_cast<F32>(gViewerWindow->getCurrentMouseX()) - mTacticalViewport[0] / scale.mV[VX],
                            static_cast<F32>(gViewerWindow->getCurrentMouseY()) - mTacticalViewport[1] / scale.mV[VY]);
    mTacticalHover.notePointer(pointer);
    const bool esc = gKeyboard && gKeyboard->getKeyDown(KEY_ESCAPE);
    if (esc && !mTacticalEscDown && !mTacticalHover.empty())
    {
        mTacticalHover.suppress(pointer);
    }
    mTacticalEscDown = esc;
    std::vector<skmap::HoverDot> dots;
    for (const auto& [index, at] : drawn)
    {
        if (index < people.size())
        {
            dots.push_back({ people[index].mId, ui(at) });
        }
    }
    if (gFocusMgr.getMouseCapture() || mTacticalHover.suppressed())
    {
        mTacticalHover.clear();
    }
    else
    {
        mTacticalHover.update(dots, pointer, enter, exit);
    }
    if (mTacticalHover.empty())
    {
        mTacticalLabels.clear();
        mTacticalTagTexts.clear();
    }
    else
    {
        auto same_floor = [](const LLUUID&) { return skmap::FloorRelation::SAME; };
        mTacticalTagTexts = skmapdraw::tagTexts(mTacticalHover.members(), same_floor, skmapdraw::FloorLabels(),
                                                skmap::tagMaxWidth(ui_w, static_cast<F32>(max_width)));
        skmap::LabelLayout::Input in;
        in.mPointer = down(pointer);
        in.mWidth = ui_w;
        in.mHeight = ui_h;
        for (const skmapdraw::TagText& t : mTacticalTagTexts)
        {
            for (const skmap::HoverDot& d : dots)
            {
                if (d.mId == t.mId)
                {
                    in.mMembers.push_back({ t.mId, down(d.mPos), t.mWidth, t.mHeight });
                    break;
                }
            }
        }
        for (const skmap::HoverDot& d : dots)
        {
            const LLVector2 p = down(d.mPos);
            in.mObstacles.push_back({ p.mV[VX] - 5.0, p.mV[VY] - 5.0, 10.0, 10.0 });
        }
        mTacticalLabels.update(in, LLFrameTimer::getFrameDeltaTimeF32());
    }

    // Connectors, then the tags over them.
    gGL.getTextureSlot(0)->unbind();
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::CONNECTOR);
    for (const skmap::LabelLayout::Tag& tag : mTacticalLabels.tags())
    {
        for (const skmap::HoverDot& d : dots)
        {
            if (d.mId == tag.mId)
            {
                const LLVector2 end = skmap::LabelLayout::connectorEnd(tag, down(d.mPos));
                skmapdraw::line(d.mPos, down(end), 1.f);
                break;
            }
        }
    }
    gGL.end();
    for (const skmap::LabelLayout::Tag& tag : mTacticalLabels.tags())
    {
        for (const skmapdraw::TagText& t : mTacticalTagTexts)
        {
            if (t.mId == tag.mId)
            {
                const S32 left = ll_round(static_cast<F32>(tag.mX)), top = ll_round(ui_h - static_cast<F32>(tag.mY));
                skmapdraw::drawTag(LLRect(left, top, left + ll_round(static_cast<F32>(tag.mW)), top - ll_round(static_cast<F32>(tag.mH))), t);
                break;
            }
        }
    }

    gGL.popUIMatrix();
    LLFontGL::sCurOrigin = origin;
    LLFontGL::sCurDepth = depth;
    LLFontGL::sCurScaleX = cur_sx;
    LLFontGL::sCurScaleY = cur_sy;
}
