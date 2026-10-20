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
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llvoavatarself.h"
#include "llagentcamera.h"
#include "llviewerjoystick.h"
#include "llworld.h"
#include "sknetmap.h"
#include "skmappeople.h"
#include "skcensusavatarshape.h"
#include "skmaplabels.h"
#include "skmapmenu.h"
#include "skoverviewprograms.h"
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
#include "llhudnametag.h"
#include "llviewerobjectlist.h"
#include "pipeline.h"
#include "sknavbuild.h"
#include "sknavmesh.h"
#include "sknavrooms.h"
#include "skmappalette.h"
#include "skworldcensus.h"
#include "skworldmapmath.h"
#include "llstartup.h"
#include "lltool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

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
    LLGLSLShader sGeom;       // the census: class code and the face's own normal
    LLGLSLShader sGeomCut;    // the census with a cut height per vertex, for the tactical view
    LLGLSLShader sGeomCutDissolve;  // the same with the see-through circles, the only permutation that discards
    LLGLSLShader sGeomCapLift;      // the cut census's section pass, the only permutation that writes depth
    LLGLSLShader sGeomCapLiftDissolve;
    LLGLSLShader sMarker;     // the tactical avatar markers, depth-tested by hand

    const F32 CENSUS_RANGE_M = 128.f;
    const size_t SK_MAX_TERRITORY = 16;       // regions styled as inside, at most (the resolve's array)
    const size_t SK_MAX_DISSOLVE = 16;        // see-through circles, at most (overviewGeomF.glsl's array)
    const F32 AVATAR_HALF_HEIGHT_M = 0.9f;     // a coarse-only avatar's position over its feet, having no box from the sim
    const F32 MARKER_HALO_PX = 1.5f;           // the dark ring that sets a flat marker off bright geometry
    const F32 MARKER_GHOST_ALPHA = 0.3f;       // its silhouette where the census hides it
    const F32 HEADING_TIP_PER_RADIUS = 1.f;    // the minimap's heading triangle (skmap::palette::HEADING_*_PX) over
    const F32 HEADING_BASE_PER_RADIUS = 0.25f; // its 6 px dot, as fractions of the body's radius beyond it
    const F32 HEADING_HALF_PER_RADIUS = 0.42f;
    const F32 HEADING_THICK_PER_RADIUS = 0.3f; // and extruded this thick
    const F32 TACTICAL_NEAR_M = 0.5f;
    constexpr F64 ROOM_CELL_M = 0.5;    // the lattice a room is flooded through the air on
    const F32 TACTICAL_MIN_M = 8.f;
    const F32 TACTICAL_MAX_M = 4096.f;
    const F32 BATTLEFIELD_MAX_M = 4096.f;
    const F32 BATTLEFIELD_PITCH_DEG = 89.f;  // the untilted battlefield: short of straight down, so the camera keeps a heading to put north up
    const F32 BATTLEFIELD_MARGIN = 1.05f;
    const F32 FLAT_FADE_S = 0.15f;  // a marker's crossfade between its body and its zoomed-out flat marker
    const size_t MAX_BADGES = 64;           // coded people badged on the tactical view at most, nearest first
    const F32 BADGE_LIFT_PX = 2.f;          // a badge's gap above its marker's top

    /// SKTacticalCameraMode: a fixed north-up view fitting the battlefield, or the orbit that follows the agent.
    bool battlefieldCamera()
    {
        static LLCachedControl<U32> mode(gSavedSettings, "SKTacticalCameraMode", 0);
        return mode == 0;
    }
    const F64 MAPS_WANTED_S = 2.0;  // the region maps keep capturing this long after a map last drew them
    F64 sMapsWantedAt = -1.0;

    /// The mini-map's opt-in layers the region maps style (overviewResolveF.glsl's MASK_*); the tactical view ignores them.
    U32 mapClassMask()
    {
        static LLCachedControl<bool> stairs(gSavedSettings, "SKMapLayerStairs", false);
        static LLCachedControl<bool> doors(gSavedSettings, "SKMapLayerDoors", false);
        static LLCachedControl<bool> ladders(gSavedSettings, "SKMapLayerLadders", false);
        static LLCachedControl<bool> contours(gSavedSettings, "SKMapLayerContours", false);
        return (stairs ? 1u : 0u) | (doors ? 2u : 0u) | (ladders ? 4u : 0u) | (contours ? 8u : 0u);
    }
    const F32 LEVEL_BELOW_M = 4.f;  // another region's map keeps the clusters reaching this far below the agent's floor
    const F32 LEVEL_ABOVE_M = 8.f;  // and this far above it
    const F32 ON_GROUND_M = 8.f;    // an agent's floor this close to the land is at ground level

    void uniformColour(GLint location, const skmap::PaletteColour& c)
    {
        glUniform3f(location, c.mR, c.mG, c.mB);
    }

    // The overview's own uniforms, looked up in each program's map once a bind rather than by string in GL: thirty
    // names would be a heavier edit to LLShaderMgr's reserved table than a lookup here. Its samplers are reserved names.
    const LLStaticHashedString SK_INV_VP("sk_inv_vp");
    const LLStaticHashedString SK_ZO("sk_zo");
    const LLStaticHashedString SK_FAR_DEPTH("sk_far_depth");
    const LLStaticHashedString SK_EYE("sk_eye");
    const LLStaticHashedString SK_TOP_DOWN("sk_top_down");
    const LLStaticHashedString SK_SCREEN_UP("sk_screen_up");
    const LLStaticHashedString SK_FOCUS("sk_focus");
    const LLStaticHashedString SK_CUT_FLAG("sk_cut_flag");
    const LLStaticHashedString SK_LEVEL_TOP("sk_level_top");
    const LLStaticHashedString SK_CUT("sk_cut");
    const LLStaticHashedString SK_WATER_Z("sk_water_z");
    const LLStaticHashedString SK_WATER_DEPTH("sk_water_depth");
    const LLStaticHashedString SK_INVISIBLE("sk_invisible");
    const LLStaticHashedString SK_DEBUG_VIEW("sk_debug_view");
    const LLStaticHashedString SK_CLASS_MASK("sk_class_mask");
    const LLStaticHashedString SK_REGIONS("sk_regions");
    const LLStaticHashedString SK_REGION_COUNT("sk_region_count");
    const LLStaticHashedString CLIP_PLANE("clip_plane");
    const LLStaticHashedString SK_CEILING("sk_ceiling");
    const LLStaticHashedString SK_EYE_TO_WORLD("sk_eye_to_world");
    const LLStaticHashedString SK_DEBUG_CUT("sk_debug_cut");
    const LLStaticHashedString SK_CAP_PASS("sk_cap_pass");
    const LLStaticHashedString SK_DISSOLVE_COUNT("sk_dissolve_count");
    const LLStaticHashedString SK_DISSOLVE_SOFT("sk_dissolve_soft");
    const LLStaticHashedString SK_DISSOLVE("sk_dissolve");
    const LLStaticHashedString SK_DISSOLVE_FLOOR("sk_dissolve_floor");
    const LLStaticHashedString SK_VIEWPORT("sk_viewport");
    const LLStaticHashedString SK_GHOST_ALPHA("sk_ghost_alpha");
    const LLStaticHashedString SK_TILT_PX("sk_tilt_px");
    const LLStaticHashedString SK_GROUND("sk_ground");

    const S32 RESOLVE_REACH_PX = 29;    // how far a resolved pixel reads: the AO taps' 28 px and the neighbours' one

    /// The smallest rect holding both; an empty one adds nothing.
    LLRect rectUnion(const LLRect& a, const LLRect& b)
    {
        if (a.isEmpty())
        {
            return b;
        }
        if (b.isEmpty())
        {
            return a;
        }
        LLRect out = a;
        out.unionWith(b);
        return out;
    }

    /// Limits drawing to a window rect of the bound target while it lives; with no rect it changes nothing.
    class ScissorTo
    {
    public:
        explicit ScissorTo(const LLRect* rect)
        : mState(rect ? GL_SCISSOR_TEST : 0, LLGLState::ENABLED_STATE)
        {
            if (rect)
            {
                glGetIntegerv(GL_SCISSOR_BOX, mSaved);
                glScissor(rect->mLeft, rect->mBottom, rect->getWidth(), rect->getHeight());
                mSet = true;
            }
        }
        ~ScissorTo()
        {
            if (mSet)
            {
                glScissor(mSaved[0], mSaved[1], mSaved[2], mSaved[3]);
            }
        }

    private:
        LLGLState mState;
        GLint mSaved[4] = {};
        bool mSet = false;
    };

    const F64 PAN_MARGIN_M = 512.0;     // how far past the battlefield the view's focus may be dragged

    /// The step a pan may take: zoom-to-cursor leaves the focus at whatever depth it picked, so the limit holds the
    /// ground at the view's centre, on the agent's level, within the margin, and never pulls it back from outside.
    LLVector3d clampedPanStep(const LLVector3d& eye, const LLVector3d& focus, const LLVector3d& step, const LLVector3d& lo,
                              const LLVector3d& hi)
    {
        const LLVector3d dir = focus - eye;
        LLVector3d centre = focus;
        const F64 t = std::fabs(dir.mdV[VZ]) > 1.0e-6 ? (gAgent.getPositionGlobal().mdV[VZ] - eye.mdV[VZ]) / dir.mdV[VZ] : -1.0;
        if (t > 0.0)
        {
            centre = eye + dir * t;
        }
        LLVector3d clamped = step;
        for (S32 i = VX; i <= VY; ++i)
        {
            const F64 min_v = llmin(lo.mdV[i] - PAN_MARGIN_M, centre.mdV[i]);
            const F64 max_v = llmax(hi.mdV[i] + PAN_MARGIN_M, centre.mdV[i]);
            clamped.mdV[i] = llclamp(centre.mdV[i] + step.mdV[i], min_v, max_v) - centre.mdV[i];
        }
        return clamped;
    }

    /// Why a press or drag did not pan, with SKOverviewPanDebug, once a second at most.
    void panTrace(const char* why)
    {
        static LLCachedControl<bool> debug(gSavedSettings, "SKOverviewPanDebug", false);
        static F64 last = -1.0;
        const F64 now = LLFrameTimer::getUptimeSeconds().value();
        if (debug && now - last >= 1.0)
        {
            last = now;
            LL_INFOS("SKOverviewPan") << why << LL_ENDL;
        }
    }

    /// The ray through a viewport pixel of a camera, agent space.
    bool cursorRay(const LLMatrix4a& view, const LLMatrix4a& proj, const S32 viewport[4], const LLVector2& at, LLVector3& from,
                   LLVector3& dir)
    {
        LLMatrix4a vp, inv_vp, inv_view;
        vp.setMul(view, proj);
        if (viewport[2] <= 0 || viewport[3] <= 0 || !inv_vp.setInverse(vp) || !inv_view.setInverse(view))
        {
            return false;
        }
        LLVector4a origin, eye, ndc, far_pt;
        origin.set(0.f, 0.f, 0.f, 1.f);
        inv_view.transform4(origin, eye);
        ndc.set(at.mV[VX] / viewport[2] * 2.f - 1.f, at.mV[VY] / viewport[3] * 2.f - 1.f, 0.5f, 1.f);
        inv_vp.transform4(ndc, far_pt);
        const F32* e = eye.getF32ptr();
        const F32* f = far_pt.getF32ptr();
        if (std::fabs(e[3]) < 1e-6f || std::fabs(f[3]) < 1e-6f)
        {
            return false;
        }
        from.set(e[0] / e[3], e[1] / e[3], e[2] / e[3]);
        dir = LLVector3(f[0] / f[3], f[1] / f[3], f[2] / f[3]) - from;
        return dir.normVec() > 1e-6f;
    }

    /// Where a ray meets the plane z = plane_z ahead of it.
    bool rayPlane(const LLVector3& from, const LLVector3& dir, F32 plane_z, LLVector3& hit)
    {
        if (std::fabs(dir.mV[VZ]) < 1e-6f)
        {
            return false;
        }
        const F32 t = (plane_z - from.mV[VZ]) / dir.mV[VZ];
        if (t <= 0.f)
        {
            return false;
        }
        hit = from + dir * t;
        return true;
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
    U64 mCutStamp = 0;        // the census cut it was taken with
    bool mValid = false;
};

SKOverviewCapture::SKOverviewCapture()
{
    SKModalView::instance().registerMode(this);
}

SKOverviewCapture::~SKOverviewCapture()
{
    if (SKModalView::instanceExists())
    {
        SKModalView::getInstance()->unregisterMode(this);
    }
}

void SKOverviewCapture::onFrame()
{
    updateCapsLockCapture();
    syncTactical();
    // The world census runs slowly only while the overview hides the world (navmesh freshness matters little then),
    // and back at its normal cadence the frame it closes.
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus::instance().setSlowCadence(tacticalShown());
    }
    // The setting alone draws nothing: the census is kept only for maps or an open tactical overview.
    if (!mapsWanted() && !tacticalShown())
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

void SKOverviewCapture::handleTacticalKey(bool down)
{
    static F64 pressed_at = -1.0;
    static U32 pressed_frame = 0;
    static bool latched = false;
    static bool closing = false;
    static LLCachedControl<F32> tap_s(gSavedSettings, "SKTacticalTapSeconds", 0.35f);
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (down)
    {
        pressed_at = now;
        pressed_frame = LLFrameTimer::getFrameCount();
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
    // A release within a couple of frames is a tap however long they took: opening the view stalls a frame.
    if (now - pressed_at < (F64)tap_s || LLFrameTimer::getFrameCount() - pressed_frame <= 2)
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
}

void SKOverviewCapture::releaseGL()
{
    sCapture.release();
    sCaptureNear.release();
    sCaptureFar.release();
    sTacticalCapture.release();
    sTacticalResolved.release();
    sTacticalWork.release();
    mSpans.clear();
    mCensus.release();
    mMaps.clear();
    mTacticalKey.mValid = false;
    mWorkValid = false;
    mGLFailed = false;
}

void SKOverviewCapture::updateDemand(bool wanted)
{
    if (!SKWorldCensus::instanceExists())
    {
        return;
    }
    // The region maps want the census over the agent's neighbours as well; the tactical view only around it.
    const bool maps = mapsWanted();
    static LLCachedControl<F32> maps_range(gSavedSettings, "SKOverviewCensusRange", 384.f);
    F32 range = maps ? llmax((F32)maps_range, CENSUS_RANGE_M) : CENSUS_RANGE_M;
    // The tactical view shows the neighbours too: the census reaches as far as it looks, in 64 m steps so a zoom
    // does not restart the demand every frame.
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    static LLCachedControl<F32> tactical_distance(gSavedSettings, "SKTacticalDistance", 60.f);
    if (tactical)
    {
        // Open, the camera's own distance, which alt-cam and the wheel change.
        const F32 distance = mTacticalOpen && mTacticalDistance > 0.f ? mTacticalDistance : (F32)tactical_distance;
        F32 reach = llclamp(distance, TACTICAL_MIN_M, battlefieldCamera() ? BATTLEFIELD_MAX_M : TACTICAL_MAX_M) + CENSUS_RANGE_M;
        LLVector3d lo, hi;
        if (battlefieldCamera() && battlefieldBounds(lo, hi))
        {
            // Battlefield shows its area and no more: the census reaches the area's farthest corner, not the camera's
            // height, which a tilt or a wide area takes to kilometres.
            const LLVector3d agent = gAgent.getPositionGlobal();
            const F64 dx = llmax(std::fabs(lo.mdV[VX] - agent.mdV[VX]), std::fabs(hi.mdV[VX] - agent.mdV[VX]));
            const F64 dy = llmax(std::fabs(lo.mdV[VY] - agent.mdV[VY]), std::fabs(hi.mdV[VY] - agent.mdV[VY]));
            reach = llmin(reach, (F32)std::sqrt(dx * dx + dy * dy));
        }
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

// static
void SKOverviewCapture::noteMapsWanted()
{
    sMapsWantedAt = LLFrameTimer::getUptimeSeconds().value();
}

// static
bool SKOverviewCapture::mapsWanted()
{
    return sMapsWantedAt >= 0.0 && LLFrameTimer::getUptimeSeconds().value() - sMapsWantedAt < MAPS_WANTED_S;
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

namespace
{
    /// The program each of skoverview::PROGRAMS builds, in its order.
    LLGLSLShader* const OVERVIEW_SHADERS[] = { &sResolve,     &sComposite,           &sGeom,  &sGeomCut, &sGeomCutDissolve,
                                               &sGeomCapLift, &sGeomCapLiftDissolve, &sMarker };
    static_assert(sizeof(OVERVIEW_SHADERS) / sizeof(OVERVIEW_SHADERS[0]) == skoverview::PROGRAM_COUNT);
}

// static
bool SKOverviewCapture::loadShaders(S32 level)
{
    unloadShaders();
    bool ok = true;
    for (size_t i = 0; i < skoverview::PROGRAM_COUNT; ++i)
    {
        const skoverview::ProgramSpec& program = skoverview::PROGRAMS[i];
        LLGLSLShader& shader = *OVERVIEW_SHADERS[i];
        shader.mName = program.mName;
        shader.mShaderFiles.clear();
        shader.mShaderFiles.push_back(std::make_pair(program.mVertex, GL_VERTEX_SHADER));
        shader.mShaderFiles.push_back(std::make_pair(program.mFragment, GL_FRAGMENT_SHADER));
        shader.mShaderLevel = llmax(level, 1);
        shader.clearPermutations();
        for (const char* define : program.mDefines)
        {
            if (define)
            {
                shader.addPermutation(define, "1");
            }
        }
        if (!shader.createShader())
        {
            LL_WARNS("Shader") << "The overview's program " << program.mName << " did not build; the overview is off until the "
                                  "shaders are reloaded"
                               << LL_ENDL;
            ok = false;
            break;
        }
    }
    if (ok)
    {
        LL_INFOS("SKOverview") << "overview shaders loaded" << LL_ENDL;
    }
    else
    {
        unloadShaders();
    }
    if (instanceExists())
    {
        instance().mGLFailed = false;
    }
    return ok;
}

// static
void SKOverviewCapture::unloadShaders()
{
    for (LLGLSLShader* shader : OVERVIEW_SHADERS)
    {
        shader->unload();
    }
}

bool SKOverviewCapture::ensureShaders()
{
    for (size_t i = 0; i < skoverview::PROGRAM_COUNT; ++i)
    {
        if (!OVERVIEW_SHADERS[i]->isComplete())
        {
            LL_WARNS_ONCE("SKOverview") << "overview program " << skoverview::PROGRAMS[i].mName << " is not loaded" << LL_ENDL;
            return false;
        }
    }
    return true;
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
              capture.allocate(w, h, GL_RGBA8, true, true, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                               LLRenderTarget::DEPTH_FMT_32F);
    ok = ok && (resolved.isComplete() || resolved.allocate(w, h, GL_RGBA8));
    if (!ok)
    {
        LL_WARNS_ONCE("SKOverview") << "overview target " << w << "x" << h << " did not allocate" << LL_ENDL;
    }
    ok = ok && ensureShaders();
    if (!ok)
    {
        LL_WARNS("SKOverview") << "Overview could not allocate its targets or find its shaders; it is off until the shaders "
                                  "are reloaded or the GL context is rebuilt"
                               << LL_ENDL;
        mGLFailed = true;
        releaseGL();
        mGLFailed = true;
        return false;
    }
    return true;
}

void SKOverviewCapture::endSpan(SpanKind kind, ALQuerySpan& span)
{
    if (span.isValid())
    {
        gGL.queries().endSpan(span);
        mSpans.emplace_back(kind, span);
    }
}

void SKOverviewCapture::readSpans()
{
    const ALQueryPool& queries = gGL.queries();
    std::erase_if(mSpans, [this, &queries](const std::pair<SpanKind, ALQuerySpan>& timed)
    {
        U64 nanoseconds = 0;
        switch (queries.result(timed.second, nanoseconds))
        {
        case ALQueryPool::Status::Ready:
            break;
        case ALQueryPool::Status::Expired:
            return true;
        default:
            return false;
        }
        const F32 ms = (F32)((F64)nanoseconds / 1.0e6);
        switch (timed.first)
        {
        case SpanKind::Map:
            mStats.gpu_ms = ms;
            break;
        case SpanKind::Tactical:
            mStats.gpu_tactical_ms = ms;
            mCost.mGpuTacticalMs += ms;
            break;
        case SpanKind::Patch:
            mStats.gpu_patch_ms = ms;
            mCost.mGpuPatchMs += ms;
            break;
        case SpanKind::Movers:
            mStats.gpu_movers_ms = ms;
            mCost.mGpuMoversMs += ms;
            break;
        case SpanKind::Resolve:
            mStats.gpu_resolve_ms = ms;
            mCost.mGpuResolveMs += ms;
            break;
        }
        return true;
    });
}

void SKOverviewCapture::cutHeights(const LLVector3d& at, F32 fallback_z, F32& floor_z, F32& cut_z) const
{
    static LLCachedControl<F32> cut(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    // The avatar's feet are the floor; the navmesh only refines it, since its nearest mesh can be the ground under a street.
    F32 feet = fallback_z;
    if (isAgentAvatarValid())
    {
        feet = gAgentAvatarp->getPositionAgent().mV[VZ] - 0.5f * gAgentAvatarp->getScale().mV[VZ];
    }
    F32 floor = 0.f;
    floor_z = skmap::plan::agentFloor(at, feet, floor) ? floor : feet;
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

// static
void SKOverviewCapture::syncTactical()
{
    // The setting is the switch, from the key, the menu or anywhere else; the modal view follows it.
    static LLCachedControl<bool> tactical(gSavedSettings, "SKTacticalOverview", false);
    static bool connected = false;
    if (!connected)
    {
        // A change made while another mode covers the world, when no world frame runs onFrame.
        connected = true;
        gSavedSettings.getControl("SKTacticalOverview")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { syncTactical(); });
        // A new camera mode moves an open overview's camera at once.
        auto replace = [](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (instanceExists() && instance().mTacticalOpen && SKModalView::top() == &instance())
            {
                instance().placeTacticalCamera(false);
            }
        };
        gSavedSettings.getControl("SKTacticalCameraMode")->getSignal()->connect(replace);
        gSavedSettings.getControl("SKTacticalBattlefieldNeighbours")->getSignal()->connect(replace);
        auto tilt = [](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (battlefieldCamera() && instanceExists() && instance().mTacticalOpen && SKModalView::top() == &instance())
            {
                instance().placeBattlefieldCamera();
            }
        };
        gSavedSettings.getControl("SKTacticalBattlefieldPitch")->getSignal()->connect(tilt);
    }
    if (tactical == (instanceExists() && instance().mTacticalOpen))
    {
        return;
    }
    if (tactical)
    {
        instance().openTactical();
    }
    else
    {
        instance().closeTactical();
    }
}

void SKOverviewCapture::openTactical()
{
    const bool over_other_mode = SKModalView::worldCovered();
    if (!SKModalView::instance().claim(this, SKModalView::PRIORITY_TACTICAL_OVERVIEW))
    {
        // Logged in, only RLVa holds the view shut: the switch goes off rather than open later by surprise.
        if (LLStartUp::getStartupState() >= STATE_STARTED)
        {
            gSavedSettings.setBOOL("SKTacticalOverview", false);
        }
        return;
    }
    mTacticalOpen = true;
    mTacticalKey.mValid = false;
    if (SKModalView::top() == this)
    {
        placeTacticalCamera(over_other_mode);
    }
}

void SKOverviewCapture::closeTactical()
{
    if (!mTacticalOpen)
    {
        return;
    }
    if (SKModalView::top() == this && !battlefieldCamera())
    {
        // The next open starts as far out as this one ended.
        const F32 distance = (F32)(gAgentCamera.getCameraPositionGlobal() - gAgentCamera.getFocusGlobal()).magVec();
        gSavedSettings.setF32("SKTacticalDistance", llclamp(distance, TACTICAL_MIN_M, TACTICAL_MAX_M));
    }
    mTacticalOpen = false;
    mTacticalReady = false;
    mTacticalPickable = false;
    if (SKPopupPanes* popup = mPopup.get())
    {
        popup->closeAll();
    }
    // The last release puts the world camera back; a map under the overview takes its own back.
    SKModalView::instance().release(this);
}

void SKOverviewCapture::closeModal()
{
    gSavedSettings.setBOOL("SKTacticalOverview", false);
    closeTactical();
}

void SKOverviewCapture::onTopChanged(bool on_top)
{
    if (on_top && mTacticalOpen)
    {
        placeTacticalCamera(false);
    }
}

void SKOverviewCapture::placeTacticalCamera(bool over_other_mode)
{
    if (battlefieldCamera())
    {
        placeBattlefieldCamera();
        return;
    }
    static LLCachedControl<F32> distance_setting(gSavedSettings, "SKTacticalDistance", 60.f);
    static LLCachedControl<F32> pitch_setting(gSavedSettings, "SKTacticalPitch", 55.f);
    // Over the agent's floor when the camera follows it or another mode had moved it; else what alt-cam looked at.
    LLVector3d focus;
    tacticalCentre(focus);
    if (over_other_mode || gAgentCamera.getFocusOnAvatar())
    {
        F32 floor_z, cut_z;
        cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
        focus = gAgent.getPosGlobalFromAgent(LLVector3(gAgent.getPositionAgent().mV[VX], gAgent.getPositionAgent().mV[VY], floor_z));
    }
    LLVector3 heading = LLViewerCamera::getInstance()->getAtAxis();
    heading.mV[VZ] = 0.f;
    if (heading.normVec() < 1.0e-3f)
    {
        heading.setVec(1.f, 0.f, 0.f);
    }
    const F32 pitch = llclamp((F32)pitch_setting, 5.f, 89.5f) * DEG_TO_RAD;
    const F32 distance = llclamp((F32)distance_setting, TACTICAL_MIN_M, TACTICAL_MAX_M);
    const LLVector3d eye = focus - LLVector3d(heading * (distance * std::cos(pitch))) + LLVector3d(0.0, 0.0, distance * std::sin(pitch));
    setTacticalCamera(eye, focus);
}

void SKOverviewCapture::setTacticalCamera(const LLVector3d& eye, const LLVector3d& focus)
{
    // Snapped, not eased, so the overview can be flicked on and off for bearings.
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(eye, focus, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    gAgentCamera.skipCameraSmoothing();
}

bool SKOverviewCapture::battlefieldBounds(LLVector3d& lo, LLVector3d& hi) const
{
    static LLCachedControl<bool> neighbours(gSavedSettings, "SKTacticalBattlefieldNeighbours", false);
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return false;
    }
    // The agent's region, and with SKTacticalBattlefieldNeighbours the connected regions that touch it.
    const LLVector3d origin = region->getOriginGlobal();
    const F64 width = region->getWidth();
    lo = origin;
    hi = origin + LLVector3d(width, width, 0.0);
    if (neighbours)
    {
        const LLVector3d own_lo = lo;
        const LLVector3d own_hi = hi;
        for (LLViewerRegion* other : LLWorld::getInstance()->getRegionList())
        {
            const LLVector3d o = other->getOriginGlobal();
            const F64 w = other->getWidth();
            if (other == region || o.mdV[VX] > own_hi.mdV[VX] || o.mdV[VX] + w < own_lo.mdV[VX] || o.mdV[VY] > own_hi.mdV[VY] ||
                o.mdV[VY] + w < own_lo.mdV[VY])
            {
                continue;
            }
            lo.mdV[VX] = llmin(lo.mdV[VX], o.mdV[VX]);
            lo.mdV[VY] = llmin(lo.mdV[VY], o.mdV[VY]);
            hi.mdV[VX] = llmax(hi.mdV[VX], o.mdV[VX] + w);
            hi.mdV[VY] = llmax(hi.mdV[VY], o.mdV[VY] + w);
        }
    }
    return true;
}

void SKOverviewCapture::placeBattlefieldCamera()
{
    LLVector3d lo, hi;
    if (!battlefieldBounds(lo, hi))
    {
        return;
    }
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    const LLVector3d floor = gAgent.getPosGlobalFromAgent(LLVector3(0.f, 0.f, floor_z));
    const LLVector3d focus((lo.mdV[VX] + hi.mdV[VX]) * 0.5, (lo.mdV[VY] + hi.mdV[VY]) * 0.5, floor.mdV[VZ]);
    // North up, tilted to look north from the south; far enough that every corner of the area lies in the frustum,
    // which a tilt turns into a trapezoid on the ground.
    static LLCachedControl<F32> pitch_setting(gSavedSettings, "SKTacticalBattlefieldPitch", BATTLEFIELD_PITCH_DEG);
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLRect world = gViewerWindow->getWorldViewRectRaw();
    const F32 aspect = world.getHeight() > 0 ? (F32)world.getWidth() / (F32)world.getHeight() : camera->getAspect();
    const F32 tan_half = std::tan(camera->getView() * 0.5f);
    const F32 pitch = llclamp((F32)pitch_setting, 5.f, BATTLEFIELD_PITCH_DEG) * DEG_TO_RAD;
    const LLVector3 forward(0.f, std::cos(pitch), -std::sin(pitch));
    const LLVector3 up(0.f, std::sin(pitch), std::cos(pitch));
    const F32 half_x = (F32)(hi.mdV[VX] - lo.mdV[VX]) * 0.5f * BATTLEFIELD_MARGIN;
    const F32 half_y = (F32)(hi.mdV[VY] - lo.mdV[VY]) * 0.5f * BATTLEFIELD_MARGIN;
    F32 fit = 0.f;
    for (S32 corner = 0; corner < 4; ++corner)
    {
        // A corner a from the focus sits at depth a.forward + d before an eye d back along forward, so it fits once
        // d covers its offsets across and up over the half-angle tangents.
        const LLVector3 a((corner & 1) ? half_x : -half_x, (corner & 2) ? half_y : -half_y, 0.f);
        const F32 depth = a * forward;
        fit = llmax(fit, llmax(std::fabs(a.mV[VX]) / (tan_half * aspect), std::fabs(a * up) / tan_half) - depth);
    }
    const F32 distance = llclamp(fit, TACTICAL_MIN_M, BATTLEFIELD_MAX_M);
    const LLVector3d north(0.0, 1.0, 0.0);
    const LLVector3d eye = focus - north * (distance * std::cos(pitch)) + LLVector3d(0.0, 0.0, distance * std::sin(pitch));
    setTacticalCamera(eye, focus);
}

void SKOverviewCapture::renderModal(const LLRect& world_rect_raw)
{
    // The display loop skips the world, and with it the overview's per-frame work: the maps, the census and the view.
    updateCapsLockCapture();
    applyKeyPan();
    if (mapsWanted() || mTacticalOpen)
    {
        frame();
    }
    composite();
}

bool SKOverviewCapture::pickGround(S32 x, S32 y, LLVector3d& ground) const
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mTacticalViewport[0], y * scale.mV[VY] - mTacticalViewport[1]);
    return groundAt(at, ground);
}

F64 SKOverviewCapture::flycamScale(const LLVector3d& eye) const
{
    return skworldmap::flycamHeightScale(eye.mdV[VZ], gAgent.getPositionGlobal().mdV[VZ]);
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
    readSpans();
    static LLCachedControl<bool> tactical_on(gSavedSettings, "SKTacticalOverview", false);
    if (mapsWanted() || (tactical_on && tacticalShown()))
    {
        LL_PROFILE_ZONE_NAMED("sk overview census update");
        mCensus.update();
    }

    if (mapsWanted())
    {
        if (!tacticalShown())
        {
            updateCut();
        }
        captureMaps(LLFrameTimer::getUptimeSeconds().value());
    }
    else
    {
        mMaps.clear();
    }
    if (tactical_on && tacticalShown())
    {
        tactical();
    }
}

void SKOverviewCapture::resolve(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, const LLRect* rect)
{
    const S32 w = capture.getWidth();
    const S32 h = capture.getHeight();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview resolve");
        LL_PROFILE_GPU_ZONE("sk overview resolve");
        LLTimer timer;
        // The tactical view's resolves are timed; the maps' fall inside their capture's span.
        ALQuerySpan span = shot.mTopDown ? ALQuerySpan() : gGL.queries().beginSpan();
        resolved.bindTarget();
        // The viewport stays whole with a rect: the resolve addresses its inputs by window pixel.
        glViewport(0, 0, w, h);
        ScissorTo scissor(rect);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull_face(GL_CULL_FACE);
        LLGLSLShader& prog = sResolve;
        prog.bind();
        prog.bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, &capture);
        prog.bindTexture(LLShaderMgr::DIFFUSE_MAP, &capture, ALSamplers::PointClamp);
        LLMatrix4a inv_vp;
        inv_vp.setMul(shot.mView, shot.mProj);
        inv_vp.invert();
        glUniformMatrix4fv(prog.getUniformLocation(SK_INV_VP), 1, GL_FALSE, inv_vp.getF32ptr());
        glUniform1f(prog.getUniformLocation(SK_ZO), LLRender::sReverseZ ? 1.f : 0.f);
        glUniform1f(prog.getUniformLocation(SK_FAR_DEPTH), al_window_far());
        glUniform3f(prog.getUniformLocation(SK_EYE), shot.mEye.mV[VX], shot.mEye.mV[VY], shot.mEye.mV[VZ]);
        glUniform1f(prog.getUniformLocation(SK_TOP_DOWN), shot.mTopDown ? 1.f : 0.f);
        {
            // World up on screen (world north looking straight down), so the hatch keeps its angle when the camera rolls.
            LLVector4a up;
            shot.mView.rotate(LLVector4a(0.f, 0.f, 1.f), up);
            if (up[0] * up[0] + up[1] * up[1] < 0.01f)
            {
                shot.mView.rotate(LLVector4a(0.f, 1.f, 0.f), up);
            }
            const F32 len = llmax(sqrtf(up[0] * up[0] + up[1] * up[1]), 1e-6f);
            glUniform2f(prog.getUniformLocation(SK_SCREEN_UP), up[0] / len, up[1] / len);
        }
        glUniform3f(prog.getUniformLocation(SK_FOCUS), shot.mFocus.mV[VX], shot.mFocus.mV[VY], shot.mFocus.mV[VZ]);
        glUniform1f(prog.getUniformLocation(SK_CUT_FLAG), shot.mPartCuts ? 1.f : 0.f);
        glUniform1f(prog.getUniformLocation(SK_LEVEL_TOP), shot.mLevelTop);
        glUniform1f(prog.getUniformLocation(SK_CUT), shot.mCut);
        glUniform1f(prog.getUniformLocation(SK_WATER_Z), shot.mWater);
        glUniform1f(prog.getUniformLocation(SK_WATER_DEPTH), skmap::palette::WATER_DEPTH_M);
        uniformColour(prog.getUniformLocation(SK_INVISIBLE), skmap::palette::DROP_EDGE);
        static LLCachedControl<U32> debug_view(gSavedSettings, "SKOverviewDebugView", 0);
        glUniform1i(prog.getUniformLocation(SK_DEBUG_VIEW), (GLint)(U32)debug_view);
        glUniform1i(prog.getUniformLocation(SK_CLASS_MASK), shot.mTopDown ? (GLint)mapClassMask() : -1);
        {
            // The territory: the regions styled as inside, at most SK_MAX_TERRITORY of them.
            std::vector<LLVector4> rects = shot.mTerritory.empty() ? std::vector<LLVector4>{ shot.mRegion } : shot.mTerritory;
            if (rects.size() > SK_MAX_TERRITORY)
            {
                rects.resize(SK_MAX_TERRITORY);
            }
            glUniform4fv(prog.getUniformLocation(SK_REGIONS), (GLsizei)rects.size(), rects.front().mV);
            glUniform1i(prog.getUniformLocation(SK_REGION_COUNT), (GLint)rects.size());
        }
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        prog.unbindTexture(LLShaderMgr::DIFFUSE_MAP);
        prog.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
        prog.unbind();
        resolved.flush();
        endSpan(SpanKind::Resolve, span);
        mCost.mResolveMs += timer.getElapsedTimeF32() * 1000.f;
    }

    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
}

namespace
{
    // The census's hard ceiling, SKOverviewMaxAbove over the agent's floor, as an eye-space plane: a skybox kilometres up
    // never roofs the view, whatever its base or the cut rules make of it.
    void uniformCeiling(LLGLSLShader& prog, const LLMatrix4a& view)
    {
        static LLCachedControl<F32> max_above(gSavedSettings, "SKOverviewMaxAbove", 500.f);
        const F32 ceiling = gAgent.getPositionAgent().mV[VZ] - AVATAR_HALF_HEIGHT_M + llmax((F32)max_above, 1.f);
        LLVector4a up, normal_eye, origin_eye, zero;
        up.set(0.f, 0.f, -1.f, 0.f);
        view.rotate(up, normal_eye);
        zero.clear();
        view.affineTransform(zero, origin_eye);
        const F32 plane[4] = { normal_eye.getF32ptr()[0], normal_eye.getF32ptr()[1], normal_eye.getF32ptr()[2],
                               ceiling - normal_eye.dot3(origin_eye).getF32() };
        glUniform4fv(prog.getUniformLocation(SK_CEILING), 1, plane);
    }
}

void SKOverviewCapture::drawMovers(const Shot& shot)
{
    LL_PROFILE_ZONE_SCOPED;
    const S32 w = sTacticalWork.getWidth();
    const S32 h = sTacticalWork.getHeight();
    ALQuerySpan span = gGL.queries().beginSpan();
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
        glUniform4fv(sGeom.getUniformLocation(CLIP_PLANE), 1, no_cut);
        glUniformMatrix3fv(sGeom.getUniformLocation(SK_EYE_TO_WORLD), 1, GL_FALSE, eye_to_world);
        uniformCeiling(sGeom, shot.mView);
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
        LLGLDisable cull_face(GL_CULL_FACE);
        LLGLEnable clip0(GL_CLIP_DISTANCE0);
        LLGLEnable clip1(GL_CLIP_DISTANCE0 + 1);
        LLGLEnable clip2(GL_CLIP_DISTANCE0 + 2);
        LLGLEnable clip3(GL_CLIP_DISTANCE0 + 3);
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
    endSpan(SpanKind::Movers, span);
}

bool SKOverviewCapture::ensureWork()
{
    const S32 w = sTacticalCapture.getWidth();
    const S32 h = sTacticalCapture.getHeight();
    if (sTacticalWork.isComplete() && (S32)sTacticalWork.getWidth() == w && (S32)sTacticalWork.getHeight() == h)
    {
        return true;
    }
    sTacticalWork.release();
    mWorkValid = false;
    if (!sTacticalWork.allocate(w, h, GL_RGBA8, true, true, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                                LLRenderTarget::DEPTH_FMT_32F))
    {
        LL_WARNS_ONCE("SKOverview") << "tactical work target did not allocate" << LL_ENDL;
        return false;
    }
    return true;
}

void SKOverviewCapture::updateWork(const Shot& shot, bool static_new, U64 patch_key, const LLRect& patch_rect, bool movers)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!ensureWork())
    {
        mSceneDepth = &sTacticalCapture;
        resolve(sTacticalCapture, sTacticalResolved, shot);
        return;
    }
    const S32 w = sTacticalWork.getWidth();
    const S32 h = sTacticalWork.getHeight();
    const bool whole = static_new || !mWorkValid;
    LLGLDisable scissor(GL_SCISSOR_TEST); // a blit is scissored too
    if (whole)
    {
        sTacticalWork.copyContents(sTacticalCapture, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        mWorkValid = true;
        mPatchRect = LLRect();
        mMoverRect = LLRect();
    }
    mSceneDepth = &sTacticalWork;

    const bool patch_moved = patch_key != mPatchKey;
    LLRect mover_rect;
    if (movers && !mMovers.screenBounds(shot.mView, shot.mProj, w, h, mover_rect))
    {
        mover_rect = LLRect();
    }

    // What differs from the static capture and is stale: last frame's movers, the movers' place now, and a patch that
    // moved. A patch clears and redraws its own rect, so it is redrawn only when it moved or the restore reached it.
    LLRect restore = rectUnion(mMoverRect, mover_rect);
    if (patch_moved)
    {
        restore = rectUnion(restore, mPatchRect);
    }
    const bool redraw_patch = patch_rect.notEmpty() && (whole || patch_moved || (restore.notEmpty() && restore.overlaps(patch_rect)));
    if (!whole && restore.notEmpty())
    {
        sTacticalWork.copyContents(sTacticalCapture, restore.mLeft, restore.mBottom, restore.mRight, restore.mTop, restore.mLeft,
                                   restore.mBottom, restore.mRight, restore.mTop, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                                   GL_NEAREST);
    }
    if (redraw_patch)
    {
        LL_PROFILE_ZONE_NAMED("sk tactical patch");
        ALQuerySpan span = gGL.queries().beginSpan();
        render(sTacticalWork, sTacticalResolved, shot, false, &patch_rect);
        endSpan(SpanKind::Patch, span);
    }
    if (movers && mover_rect.notEmpty())
    {
        drawMovers(shot);
    }
    if (whole)
    {
        resolve(sTacticalWork, sTacticalResolved, shot);
    }
    else
    {
        LLRect dirty = rectUnion(restore, redraw_patch ? patch_rect : LLRect());
        if (dirty.notEmpty())
        {
            LL_PROFILE_ZONE_NAMED("sk tactical dirty resolve");
            dirty.stretch(RESOLVE_REACH_PX);
            dirty.intersectWith(LLRect(0, h, w, 0));
            resolve(sTacticalWork, sTacticalResolved, shot, &dirty);
        }
    }
    mPatchRect = patch_rect;
    mMoverRect = movers ? mover_rect : LLRect();
    mPatchKey = patch_key;
}

U32 SKOverviewCapture::render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, bool resolve_now,
                              const LLRect* patch)
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

    // The census, region by region, a class a multi-draw, nearer than the terrain where they coincide.
    const std::vector<SKOverviewCensus::Batch>& batches = mCensus.batches();
    // Each class's groups the shot sees, merged into ranges: in the frustum, on the level the shot keeps, and with a
    // patch, on screen inside it. A class's groups tile its range, so with all of them kept it is one range, as before.
    struct Ranges
    {
        std::vector<GLint> mFirst;
        std::vector<GLsizei> mCount;
    };
    std::vector<std::array<Ranges, skoverview::NUM_CENSUS_CLASSES>> ranges(batches.size());
    {
        LL_PROFILE_ZONE_NAMED("sk overview cull groups");
        LLMatrix4a view_proj;
        view_proj.setMul(shot.mView, shot.mProj);
        std::vector<std::pair<U32, U32>> spans;
        std::vector<bool> kept;
        for (size_t b = 0; b < batches.size(); ++b)
        {
            const SKOverviewCensus::Batch& batch = batches[b];
            kept.assign(batch.mGroups.size(), false);
            for (size_t g = 0; g < batch.mGroups.size(); ++g)
            {
                const SKOverviewCensus::Group& group = batch.mGroups[g];
                if (!shot.mLevel.keeps(group))
                {
                    continue;
                }
                const LLVector3 lo_agent = gAgent.getPosAgentFromGlobal(group.mMin);
                const LLVector3 hi_agent = gAgent.getPosAgentFromGlobal(group.mMax);
                LLVector4a lo, hi, centre, half;
                lo.load3(lo_agent.mV);
                hi.load3(hi_agent.mV);
                centre.setAdd(lo, hi);
                centre.mul(0.5f);
                half.setSub(hi, lo);
                half.mul(0.5f);
                if (!cull.AABBInFrustumNoFarClip(centre, half))
                {
                    continue;
                }
                LLRect on_screen;
                if (patch && !(SKOverviewMovers::screenRectOf(view_proj, lo, hi, w, h, on_screen) && on_screen.overlaps(*patch)))
                {
                    continue;
                }
                kept[g] = true;
            }
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                spans.clear();
                for (size_t g = 0; g < batch.mGroups.size(); ++g)
                {
                    if (kept[g] && batch.mGroups[g].mCount[c])
                    {
                        spans.emplace_back(batch.mGroups[g].mStart[c], batch.mGroups[g].mCount[c]);
                    }
                }
                std::sort(spans.begin(), spans.end());
                Ranges& out = ranges[b][c];
                for (const std::pair<U32, U32>& span : spans)
                {
                    if (!out.mFirst.empty() && (U32)(out.mFirst.back() + out.mCount.back()) == span.first)
                    {
                        out.mCount.back() += (GLsizei)span.second;
                    }
                    else
                    {
                        out.mFirst.push_back((GLint)span.first);
                        out.mCount.push_back((GLsizei)span.second);
                    }
                }
            }
        }
    }
    std::function<void()> census;
    if (!batches.empty())
    {
        census = [&batches, &ranges, &shot, patch]()
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
            // Circles bind the one permutation that discards; without them every pass keeps early depth.
            const bool dissolve = shot.mPartCuts && !shot.mDissolve.empty();
            LLGLSLShader& geom = !shot.mPartCuts ? sGeom : dissolve ? sGeomCutDissolve : sGeomCut;
            // The section pass alone writes depth, lifting each fragment onto its part's cut.
            LLGLSLShader& lift = dissolve ? sGeomCapLiftDissolve : sGeomCapLift;
            LLMatrix4a inverse_view;
            inverse_view.setInverse(shot.mView);
            const F32* m = inverse_view.getF32ptr();
            const F32 eye_to_world[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
            // A class's kept ranges in one call.
            auto drawClass = [&batches, &ranges](const SKOverviewCensus::Batch& batch, U32 c)
            {
                const Ranges& kept = ranges[&batch - batches.data()][c];
                batch.mBuffer->multiDrawArrays(LLRender::TRIANGLES, kept.mFirst.data(), kept.mCount.data(),
                                               (GLsizei)kept.mFirst.size());
            };
            // Binds a geometry program with the shot's uniforms; the cap pass is set per pass.
            LLGLSLShader* bound = nullptr;
            auto bindGeom = [&](LLGLSLShader& prog)
            {
                if (bound == &prog)
                {
                    return;
                }
                bound = &prog;
                prog.bind();
                glUniform4fv(prog.getUniformLocation(CLIP_PLANE), 1, shot.mPartCuts ? no_cut : plane);
                glUniformMatrix3fv(prog.getUniformLocation(SK_EYE_TO_WORLD), 1, GL_FALSE, eye_to_world);
                uniformCeiling(prog, shot.mView);
                static LLCachedControl<U32> debug_view(gSavedSettings, "SKOverviewDebugView", 0);
                glUniform1i(prog.getUniformLocation(SK_DEBUG_CUT), shot.mPartCuts && debug_view == 5 ? 1 : 0);
                const GLsizei n = (GLsizei)llmin(shot.mDissolve.size(), (size_t)SK_MAX_DISSOLVE);
                if (dissolve && n)
                {
                    static LLCachedControl<F32> soft(gSavedSettings, "SKTacticalDissolveSoftness", 0.25f);
                    glUniform1i(prog.getUniformLocation(SK_DISSOLVE_COUNT), n);
                    glUniform1f(prog.getUniformLocation(SK_DISSOLVE_SOFT), llclamp((F32)soft, 0.f, 1.f));
                    glUniform4fv(prog.getUniformLocation(SK_DISSOLVE), n, shot.mDissolve.front().mV);
                    glUniform1fv(prog.getUniformLocation(SK_DISSOLVE_FLOOR), n, shot.mDissolveFloor.data());
                }
            };
            // The clip plane, the ceiling and the cuts are clip distances every overview geometry program writes.
            LLGLEnable clip0(GL_CLIP_DISTANCE0);
            LLGLEnable clip1(GL_CLIP_DISTANCE0 + 1);
            LLGLEnable clip2(GL_CLIP_DISTANCE0 + 2);
            LLGLEnable clip3(GL_CLIP_DISTANCE0 + 3);
            bindGeom(geom);
            // B carries a cut part's cap flag; the invisible pass after it raises it to 1 where it draws.
            gGL.setColorMask(true, true, shot.mPartCuts, true);
            LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
            gGL.setPolygonOffset(-1.f, -4.f);
            // Front faces, then the back faces of solids the cut went through, which stand for their section. Where
            // each part carries its own cut, a back face is the section only where the pixel's ray from the cut plane
            // is inside a cut solid: more of the cut parts' back faces past it than front, every one of which lies below the
            // cut. There it is lifted onto the cut plane (overviewGeomF.glsl), so a floor running through the solid
            // does not hide it, and the outside faces in front of a far wall's inside are left alone.
            enum { CAP_FRONT, CAP_BACK, CAP_PARITY, CAP_FILL };
            std::vector<S32> passes = { CAP_FRONT, CAP_BACK };
            if (shot.mPartCuts)
            {
                passes.push_back(CAP_PARITY);
                passes.push_back(CAP_FILL);
                LLGLState scissor(GL_SCISSOR_TEST, patch != nullptr); // a patch clears its own stencil only
                glStencilMask(0xFF);
                glClearStencil(0);
                glClear(GL_STENCIL_BUFFER_BIT);
            }
            for (S32 pass : passes)
            {
                bindGeom(pass == CAP_FILL ? lift : geom);
                glUniform1i(bound->getUniformLocation(SK_CAP_PASS), pass);
                const bool parity = pass == CAP_PARITY;
                LLGLDepthTest depth(GL_TRUE, pass == CAP_FRONT || pass == CAP_FILL ? GL_TRUE : GL_FALSE,
                                    parity ? GL_ALWAYS : GL_LEQUAL);
                LLGLState cull_face(GL_CULL_FACE, !parity);
                glCullFace(pass == CAP_FRONT ? GL_BACK : GL_FRONT);
                LLGLState stencil(GL_STENCIL_TEST, pass >= CAP_PARITY);
                if (parity)
                {
                    // A winding count, not parity: overlapping solids (walls meeting at corners) and mirrored ones
                    // stay inside, where an odd/even count turns their overlap and its surroundings inside out.
                    glStencilFunc(GL_ALWAYS, 0, 0xFF);
                    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_INCR_WRAP);
                    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_DECR_WRAP);
                    glStencilMask(0xFF);
                    gGL.setColorMask(false, false);
                }
                else if (pass == CAP_FILL)
                {
                    glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                    glStencilMask(0x00);
                    gGL.setColorMask(true, true, true, true);
                }
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
                        const U8 code = pass == CAP_FRONT ? skoverview::codeOf((skoverview::CensusClass)c) : (U8)skoverview::CODE_SECTION;
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
            glStencilMask(0xFF);
            bindGeom(geom);
            glUniform1i(bound->getUniformLocation(SK_CAP_PASS), CAP_FRONT);
            {
                // Barriers, already drawn as their class: the hatch flag over them, hiding nothing, offset past every
                // class's own so it passes over the copy it lies on.
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
                LLGLDisable cull_face(GL_CULL_FACE);
                gGL.setColorMask(false, false, true, false);
                gGL.setPolygonOffset(-1.f, -16.f);
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
            bound->unbind();
        };
    }

    U32 draws = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview capture");
        LLTimer timer;
        capture.bindTarget();
        glViewport(0, 0, w, h);
        ScissorTo scissor_to_patch(patch);
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
            LLGLState scissor(GL_SCISSOR_TEST, patch != nullptr); // a scissor left on would clear only part; a patch clears its own
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

    // A layer switched on or off restyles every map, nearest first.
    if (mapClassMask() != mClassMask)
    {
        mClassMask = mapClassMask();
        for (std::unique_ptr<RegionMap>& map : mMaps)
        {
            map->mLast = -1.0;
        }
    }
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
            if (map.mLast < 0.0 || !map.mCut || map.mCutStamp != mCensus.cutStamp() || now - map.mLast >= llmax((F32)refresh, 0.05f))
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
    // The agent's region is cut part by part (updateCut), so its eye stands over everything in it; the others keep
    // their level under a near plane at its top.
    F32 region_top = cut_z;
    if (agent)
    {
        const LLVector3d region_origin = region->getOriginGlobal();
        for (const SKOverviewCensus::Batch& batch : mCensus.batches())
        {
            if ((batch.mOrigin - region_origin).magVecSquared() < 1.0)
            {
                for (const SKOverviewCensus::Group& group : batch.mGroups)
                {
                    region_top = llmax(region_top, (F32)group.mMax.mdV[VZ]);
                }
            }
        }
    }
    const F32 top = agent ? llmax(region_top, region->getLand().getMaxZ()) + 1.f
                          : llmax(mCensus.levelTop(shot.mLevel, cut_z), region->getLand().getMaxZ()) + 1.f;
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
    shot.mCut = agent ? cut_z : top;
    shot.mPartCuts = agent;
    shot.mFocus = LLVector3(centre.mV[VX], centre.mV[VY], floor_z);
    shot.mLevelTop = cut_z;
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

    ALQuerySpan span = agent ? gGL.queries().beginSpan() : ALQuerySpan();
    const U32 draws = render(capture, map.mResolved, shot);
    endSpan(SpanKind::Map, span);

    map.mOrigin = region->getOriginGlobal();
    map.mWidth = width;
    map.mCut = agent;
    map.mCutStamp = agent ? mCensus.cutStamp() : 0;
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

std::shared_ptr<const std::set<std::pair<S32, S32>>> SKOverviewCapture::roomAt(const LLVector3d& floor, U64& hash)
{
    LL_PROFILE_ZONE_SCOPED;
    hash = 0;
    // The air's rooms need no navmesh: flood the indoor air gaps whose floor is the point's gap's, out from the point.
    const F64 SAME_FLOOR_M = 0.6;
    const S32 MAX_CELLS = 16384;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const std::pair<S32, S32> start((S32)std::floor(floor.mdV[VX] / ROOM_CELL_M), (S32)std::floor(floor.mdV[VY] / ROOM_CELL_M));
    mRooms.erase(std::remove_if(mRooms.begin(), mRooms.end(), [now](const Room& r) { return now - r.mAt > 5.0; }), mRooms.end());
    const SKNavWorldViewPtr world = SKNavMesh::instanceExists() ? SKNavMesh::instance().view() : nullptr;
    if (!world)
    {
        return {};
    }
    // The room's floor is the floor of the air column the point is in, not the point's own height.
    SKNavWorldView::SpaceId start_id;
    sknav::Enclosure start_gap;
    // The gap of a space below stands in only within a metre: further down it is another storey's (a spaceless pocket).
    SKNavWorldView::AirColumn start_column;
    world->columnAt(floor, start_column);
    bool start_found = start_column.airGapAt(floor.mdV[VZ] + 0.3, start_id, start_gap);
    if (start_found && start_gap.mFloor < floor.mdV[VZ] - 1.0)
    {
        LLUUID mesh;
        start_found = start_column.enclosureAt(floor.mdV[VZ] + 0.3, start_gap, &mesh);
        start_id.mMesh = mesh;
        start_id.mId = start_gap.mSpaceId;
    }
    if (!start_found ||
        (start_gap.mBucket != sknav::EnclosureBucket::INDOORS && start_gap.mBucket != sknav::EnclosureBucket::SHELTERED))
    {
        return {};
    }
    const F64 start_floor = start_gap.mFloor;
    const LLViewerRegion* at_region = LLWorld::getInstance()->getRegionFromPosGlobal(floor);
    const U64 region = at_region ? at_region->getHandle() : 0;
    for (Room& room : mRooms)
    {
        if (room.mRevision == world->revision() && room.mRegion == region && std::fabs(room.mFloor - start_floor) < SAME_FLOOR_M &&
            room.mCells->count(start))
        {
            room.mAt = now; // a room in use stays cached, not flooded afresh every few seconds
            hash = room.mHash;
            return room.mCells;
        }
    }
    auto indoorAt = [&](const std::pair<S32, S32>& cell)
    {
        const LLVector3d at((cell.first + 0.5) * ROOM_CELL_M, (cell.second + 0.5) * ROOM_CELL_M, start_floor + 1.0);
        SKNavWorldView::SpaceId id;
        sknav::Enclosure gap;
        return world->airGapAt(at, id, gap) && (gap.mBucket == sknav::EnclosureBucket::INDOORS || gap.mBucket == sknav::EnclosureBucket::SHELTERED) &&
               std::fabs(gap.mFloor - start_floor) < SAME_FLOOR_M;
    };
    Room room;
    room.mCells = std::make_shared<std::set<std::pair<S32, S32>>>();
    room.mAt = now;
    room.mFloor = start_floor;
    room.mRevision = world->revision();
    room.mRegion = region;
    std::vector<std::pair<S32, S32>> todo{ start };
    room.mCells->insert(start);
    // The space the point is in already holds the room's floor cells: they seed the flood, which then only
    // walks its edge and on through portals. Its raster cells are ROOM_CELL_M square on the same lattice.
    static_assert(sknav::RASTER_XY_M == ROOM_CELL_M, "the space raster and the room lattice must share cells");
    const SKNavWorldView::Mesh* mesh = nullptr;
    const sknav::Space* space = start_id.mId ? world->space(start_id, &mesh) : nullptr;
    // Only a building's space: an open one, the ground round every building, would seed sheltered pockets region-wide.
    if (space && mesh && !space->mFrozen && space->mBuilding != 0)
    {
        for (const auto& r : space->mRaster)
        {
            const sknav::PlanCell c = sknav::rasterCell(r.first);
            const F64 z0 = mesh->mOrigin.mdV[VZ] + c.mZ;
            if (start_floor < z0 - SAME_FLOOR_M || start_floor >= z0 + sknav::RASTER_Z_M + SAME_FLOOR_M ||
                (S32)room.mCells->size() >= MAX_CELLS)
            {
                continue;
            }
            // The cell's middle, so a mesh origin off the lattice by rounding still lands in the right cell.
            const std::pair<S32, S32> cell((S32)std::floor((mesh->mOrigin.mdV[VX] + c.mX + 0.5 * ROOM_CELL_M) / ROOM_CELL_M),
                                           (S32)std::floor((mesh->mOrigin.mdV[VY] + c.mY + 0.5 * ROOM_CELL_M) / ROOM_CELL_M));
            if (room.mCells->insert(cell).second)
            {
                todo.push_back(cell);
            }
        }
    }
    while (!todo.empty() && (S32)room.mCells->size() < MAX_CELLS)
    {
        const std::pair<S32, S32> cell = todo.back();
        todo.pop_back();
        static const S32 STEPS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const auto& step : STEPS)
        {
            const std::pair<S32, S32> next(cell.first + step[0], cell.second + step[1]);
            if (!room.mCells->count(next) && indoorAt(next))
            {
                room.mCells->insert(next);
                todo.push_back(next);
            }
        }
    }
    room.mHash = 1469598103934665603ull;
    for (const auto& cell : *room.mCells)
    {
        room.mHash = (room.mHash ^ (U32)cell.first) * 1099511628211ull;
        room.mHash = (room.mHash ^ (U32)cell.second) * 1099511628211ull;
    }
    mRooms.push_back(room);
    hash = room.mHash;
    return room.mCells;
}

bool SKOverviewCapture::outdoorsAt(const LLUUID& id, const LLVector3d& feet, F64 now)
{
    const F64 HOLD_S = 0.75;
    // The cut and the dissolve both ask each frame; the second asks the same and must not step the hold again.
    const auto seen = mOutdoors.find(id);
    if (seen != mOutdoors.end() && seen->second.mSeen == now)
    {
        return seen->second.mOut;
    }
    const SKNavWorldViewPtr world = SKNavMesh::instanceExists() ? SKNavMesh::instance().view() : nullptr;
    const SKNavWorldView::AirColumn* column = columnOf(world, id, feet, now);
    const bool out = column && skmap::plan::outdoorsIn(*column, feet);
    auto inserted = mOutdoors.emplace(id, Outdoors());
    Outdoors& held = inserted.first->second;
    held.mSeen = now;
    if (inserted.second)
    {
        held.mOut = out;
    }
    else if (out == held.mOut)
    {
        held.mSince = -1.0;
    }
    else if (held.mSince < 0.0)
    {
        held.mSince = now;
    }
    else if (now - held.mSince >= HOLD_S)
    {
        held.mOut = out;
        held.mSince = -1.0;
    }
    return held.mOut;
}

const SKNavWorldView::AirColumn* SKOverviewCapture::columnOf(const SKNavWorldViewPtr& world, const LLUUID& id,
                                                             const LLVector3d& feet, F64 now)
{
    if (!world)
    {
        return nullptr;
    }
    HeldColumn& held = mColumns[id];
    // The dissolve passes the feet back through agent coordinates, so the plan matches to a millimetre, not exactly.
    if (held.mAt != now || held.mRevision != world->revision() || std::fabs(held.mXY.mdV[VX] - feet.mdV[VX]) > 1e-3 ||
        std::fabs(held.mXY.mdV[VY] - feet.mdV[VY]) > 1e-3)
    {
        held.mAt = now;
        held.mRevision = world->revision();
        held.mXY = feet;
        world->columnAt(feet, held.mColumn);
    }
    return &held.mColumn;
}

void SKOverviewCapture::updateCut()
{
    // Cut by building, for every view that draws the census: the agent's own at its level, and each other building
    // in its region that holds someone the overview draws a marker for at its highest occupant's level; the rest,
    // and other regions, whole. What stands between the tactical eye and someone is dissolved round them instead.
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    static LLCachedControl<F32> cut_height(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    std::vector<SKOverviewCensus::Occupant> occupants;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    LLVector3d centre;
    tacticalCentre(centre);
    // The inputs, coarsely: the cut is worked out again only once the eye or someone has moved a little.
    U64 key = 1469598103934665603ull;
    auto mix = [&key](U64 v) { key = (key ^ v) * 1099511628211ull; };
    auto mixPos = [&mix](const LLVector3d& p)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            mix((U64)(S64)std::llround(p.mdV[k] * 4.0));
        }
    };
    auto addPerson = [&](const LLUUID& id, bool self, const LLVector3d& floor, const LLVector3d& feet, F32 cut,
                         const skmap::plan::Whereabouts& where)
    {
        SKOverviewCensus::Occupant o;
        o.mId = id;
        o.mSelf = self;
        o.mFloor = floor;
        o.mFeet = feet;
        o.mCut = cut;
        if (where.mFound && (where.mIndoor || where.mBucket == skmap::plan::Bucket::SHELTERED) && where.mCeiling < 1e29f)
        {
            // Indoors the storey's own air bounds the cut: just under its ceiling, so the floor above is gone.
            o.mCut = where.mCeiling - 0.1f; // floor + 2.5 stands when the ceiling is unknown
        }
        o.mPriority = -(F32)dist_vec(feet, centre);
        o.mOutdoors = outdoorsAt(id, feet, now);
        U64 room_hash = 0;
        if (!o.mOutdoors && where.mFound && (where.mIndoor || where.mBucket == skmap::plan::Bucket::SHELTERED))
        {
            o.mRoomCells = roomAt(floor, room_hash);
            o.mRoomCellM = (F32)ROOM_CELL_M;
        }
        mixPos(floor);
        mixPos(feet);
        mix((U64)(S64)std::llround(o.mCut * 4.f));
        mix(id.getCRC32());
        mix(o.mOutdoors);
        mix(room_hash);
        occupants.push_back(std::move(o));
    };
    const LLVector3d agent_floor = gAgent.getPosGlobalFromAgent(
        LLVector3(gAgent.getPositionAgent().mV[VX], gAgent.getPositionAgent().mV[VY], floor_z));
    LLVector3d agent_feet = agent_floor; // the feet as they stand, which a hop lifts off the floor
    if (isAgentAvatarValid())
    {
        agent_feet.mdV[VZ] = SKMapPeople::feetZ(gAgentAvatarp);
    }
    skmap::plan::Whereabouts agent_where;
    const SKNavWorldViewPtr world = SKNavMesh::instanceExists() ? SKNavMesh::instance().view() : nullptr;
    if (const SKNavWorldView::AirColumn* column = columnOf(world, gAgent.getID(), agent_feet, now))
    {
        agent_where = skmap::plan::whereabouts(*world, *column, agent_feet);
    }
    addPerson(gAgent.getID(), true, agent_floor, agent_feet, cut_z, agent_where);
    for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
    {
        if (p.mVisibility == skmap::Visibility::HIDDEN || !p.mZKnown)
        {
            continue;
        }
        LLVector3d floor = p.mPos;
        floor.mdV[VZ] = p.mWhere.mFound ? (F64)p.mWhere.mFloor : p.mFeetZ;
        LLVector3d feet = p.mPos;
        feet.mdV[VZ] = p.mFeetZ;
        addPerson(p.mId, false, floor, feet, (F32)floor.mdV[VZ] + (F32)cut_height, p.mWhere);
    }
    for (auto it = mOutdoors.begin(); it != mOutdoors.end();)
    {
        it = now - it->second.mSeen > 10.0 ? mOutdoors.erase(it) : std::next(it);
    }
    for (auto it = mColumns.begin(); it != mColumns.end();)
    {
        it = now - it->second.mAt > 10.0 ? mColumns.erase(it) : std::next(it);
    }
    mixPos(centre);
    mix(gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0);
    mCensus.applyCut(gAgent.getRegion(), occupants, key);
}

U64 SKOverviewCapture::tacticalDissolve(Shot& shot, S32 w, S32 h, LLRect& rect)
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<bool> enabled(gSavedSettings, "SKTacticalDissolve", true);
    static LLCachedControl<F32> radius_scale(gSavedSettings, "SKTacticalDissolveRadius", 1.25f);
    const F32 MIN_PX = 40.f;
    const F32 MAX_PX = 320.f;
    const F32 EASE_S = 0.2f;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const F32 step = mDissolveTime < 0.0 ? 1.f : llclamp((F32)(now - mDissolveTime) / EASE_S, 0.f, 1.f);
    mDissolveTime = now;

    // Who wants one: the agent, and everyone the overview marks.
    std::map<LLUUID, LLVector3> wanted;
    if (enabled)
    {
        // Only close up, and only for someone in outdoor air: indoors the building's cut already shows them.
        static LLCachedControl<F32> reach(gSavedSettings, "SKTacticalDissolveReach", 64.f);
        const LLVector3 eye = gAgentCamera.getCameraPositionAgent();
        const F64 seen_at = LLFrameTimer::getUptimeSeconds().value();
        auto want = [&](const LLUUID& id, const LLVector3& feet)
        {
            if (dist_vec(eye, feet + LLVector3(0.f, 0.f, 0.9f)) <= (F32)reach &&
                outdoorsAt(id, gAgent.getPosGlobalFromAgent(feet), seen_at))
            {
                wanted[id] = feet;
            }
        };
        // Anchored to the feet as they stand, as the markers are, never the floor under them.
        LLVector3 self = gAgent.getPositionAgent();
        self.mV[VZ] = isAgentAvatarValid() ? (F32)SKMapPeople::feetZ(gAgentAvatarp) : self.mV[VZ] - AVATAR_HALF_HEIGHT_M;
        want(gAgent.getID(), self);
        for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
        {
            if (p.mVisibility == skmap::Visibility::HIDDEN || !p.mZKnown)
            {
                continue;
            }
            LLVector3 floor = gAgent.getPosAgentFromGlobal(p.mPos);
            floor.mV[VZ] = (F32)p.mFeetZ; // global z is agent z
            want(p.mId, floor);
        }
    }
    for (const auto& want : wanted)
    {
        Dissolve& d = mDissolves[want.first];
        d.mFloor = want.second;
        d.mSelf = want.first == gAgent.getID();
    }
    for (auto it = mDissolves.begin(); it != mDissolves.end();)
    {
        const bool open = wanted.count(it->first) > 0;
        it->second.mAmount = llclamp(it->second.mAmount + (open ? step : -step), 0.f, 1.f);
        it = !open && it->second.mAmount <= 0.f ? mDissolves.erase(it) : std::next(it);
    }

    // Projected into the capture: in front of the eye and near the screen only; the agent first, then the nearest.
    LLMatrix4a vp;
    vp.setMul(shot.mView, shot.mProj);
    struct Circle
    {
        F32 mOrder;
        LLVector4 mAt;
        F32 mFloor;
    };
    std::vector<Circle> circles;
    for (const auto& entry : mDissolves)
    {
        const Dissolve& d = entry.second;
        auto project = [&](const LLVector3& p, LLVector4a& clip, LLVector4a& eye_p)
        {
            LLVector4a at;
            at.set(p.mV[VX], p.mV[VY], p.mV[VZ], 1.f);
            vp.transform4(at, clip);
            shot.mView.affineTransform(at, eye_p);
        };
        LLVector4a feet_clip, head_clip, chest_clip, chest_eye, unused;
        project(d.mFloor, feet_clip, unused);
        project(d.mFloor + LLVector3(0.f, 0.f, 1.8f), head_clip, unused);
        project(d.mFloor + LLVector3(0.f, 0.f, 0.9f), chest_clip, chest_eye);
        const F32 cw = chest_clip.getF32ptr()[3];
        if (cw <= TACTICAL_NEAR_M || feet_clip.getF32ptr()[3] <= 0.f || head_clip.getF32ptr()[3] <= 0.f)
        {
            continue;
        }
        auto window = [w, h](const LLVector4a& c)
        {
            const F32* f = c.getF32ptr();
            return LLVector2((f[0] / f[3] * 0.5f + 0.5f) * w, (f[1] / f[3] * 0.5f + 0.5f) * h);
        };
        const LLVector2 centre = window(chest_clip);
        const F32 tall = dist_vec(window(feet_clip), window(head_clip));
        const F32 radius = llclamp(tall * (F32)radius_scale, MIN_PX, MAX_PX) * d.mAmount * d.mAmount * (3.f - 2.f * d.mAmount);
        if (radius < 1.f || centre.mV[VX] < -radius || centre.mV[VY] < -radius || centre.mV[VX] > w + radius ||
            centre.mV[VY] > h + radius)
        {
            continue;
        }
        circles.push_back({ d.mSelf ? -1.f : cw, LLVector4(centre.mV[VX], centre.mV[VY], -chest_eye.getF32ptr()[2], radius),
                            d.mFloor.mV[VZ] });
    }
    std::sort(circles.begin(), circles.end(), [](const Circle& a, const Circle& b) { return a.mOrder < b.mOrder; });
    if (circles.size() > SK_MAX_DISSOLVE)
    {
        circles.resize(SK_MAX_DISSOLVE);
    }
    // The key moves with a pixel, a decimetre of depth or a pixel of radius: the patch is redrawn only then.
    rect = LLRect();
    U64 key = 1469598103934665603ull;
    auto mix = [&key](S64 v) { key = (key ^ (U64)v) * 1099511628211ull; };
    for (const Circle& c : circles)
    {
        shot.mDissolve.push_back(c.mAt);
        shot.mDissolveFloor.push_back(c.mFloor);
        const S32 cx = (S32)std::llround(c.mAt.mV[0]);
        const S32 cy = (S32)std::llround(c.mAt.mV[1]);
        const S32 cr = (S32)std::llround(c.mAt.mV[3]);
        mix(cx);
        mix(cy);
        mix(std::llround(c.mAt.mV[2] * 10.f));
        mix(cr);
        mix(std::llround(c.mFloor * 10.f));
        // Built from the key's own rounding, a pixel wider for it, so the rect cannot move without the key.
        const S32 r = cr + 2;
        LLRect square(cx - r, cy + r, cx + r, cy - r);
        square.intersectWith(LLRect(0, h, w, 0));
        rect = rectUnion(rect, square);
    }
    if (!circles.empty())
    {
        static LLCachedControl<F32> soft(gSavedSettings, "SKTacticalDissolveSoftness", 0.25f);
        mix(std::llround(llclamp((F32)soft, 0.f, 1.f) * 1000.f));
    }
    return key;
}

void SKOverviewCapture::tactical()
{
    LL_PROFILE_ZONE_SCOPED;
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

    // The viewer's own camera, which alt-cam moves; the cut and the shading stay the avatar's.
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 eye = camera->getOrigin();
    const LLVector3 dir = camera->getAtAxis();
    const LLVector3 left = camera->getLeftAxis();
    const LLVector3 up = camera->getUpAxis();
    LLVector3d centre = gAgentCamera.getFocusGlobal();
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        tacticalCentre(centre); // the flycam leaves the focus behind: the surface under it
    }
    const LLVector3 target = gAgent.getPosAgentFromGlobal(centre);
    const F32 distance = llmax(dist_vec(eye, target), TACTICAL_MIN_M);

    Shot shot;
    LLVector4a eye4, dir4, up4;
    eye4.load3(eye.mV);
    dir4.load3(dir.mV);
    up4.load3(up.mV);
    shot.mView = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 fov = camera->getView();
    const F32 aspect = (F32)vw / (F32)vh;
    // The cull reaches past the focus by the draw distance, so the surrounds stay whole however far out it zooms;
    // under reverse-Z the projection clips nothing far.
    const F32 far_m = distance + llmax(camera->getFar(), 2.f * CENSUS_RANGE_M);
    shot.mProj = al_perspective(fov, aspect, TACTICAL_NEAR_M,
                                LLRender::sReverseZ ? std::numeric_limits<F32>::infinity() : far_m);
    shot.mEye = eye;
    shot.mCut = cut_z;
    // The fade follows the focus, but the height colours stay on the agent's floor, eased so a climb or teleport never flickers.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (mTacticalShadeTime < 0.0 || now - mTacticalShadeTime > 1.0)
    {
        mTacticalShadeZ = floor_z;
    }
    else
    {
        mTacticalShadeZ += (floor_z - mTacticalShadeZ) * (F32)(1.0 - std::exp(-(now - mTacticalShadeTime) / 0.25));
    }
    mTacticalShadeTime = now;
    shot.mFocus = LLVector3(target.mV[VX], target.mV[VY], mTacticalShadeZ);
    shot.mPartCuts = true;
    shot.mLevelTop = cut_z;
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
    shot.mCull = *camera;
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
        static LLCachedControl<U32> tactical_debug_view(gSavedSettings, "SKOverviewDebugView", 0);
        updateCut();
        LLRect patch_rect;
        const U64 dissolve_key = tacticalDissolve(shot, vw, vh, patch_rect);
        const bool same = !force && mTacticalKey.mValid && (eye - mTacticalKey.mEye).magVecSquared() < 1e-4f &&
                          at_axis * mTacticalKey.mDir > 0.999999f && fov == mTacticalKey.mFov && vw == mTacticalKey.mWidth &&
                          vh == mTacticalKey.mHeight && std::fabs(cut_z - mTacticalKey.mCut) < 1e-3f &&
                          mCensus.signature() == mTacticalKey.mCensus &&
                          mCensus.cutStamp() == mTacticalKey.mCutStamp &&
                          (gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0) == mTacticalKey.mRegionHandle &&
                          (U32)tactical_debug_view == mTacticalKey.mDebugView;
        // The circles and the movers repair a copy of the static capture each frame; it is redone only on change.
        mMovers.setLoose(mCensus.restlessParts());
        const bool movers = mMovers.update();
        if (!same)
        {
            LL_PROFILE_ZONE_NAMED("sk tactical render");
            Shot scene = shot;
            scene.mDissolve.clear();
            scene.mDissolveFloor.clear();
            ALQuerySpan span = gGL.queries().beginSpan();
            render(sTacticalCapture, sTacticalResolved, scene, false);
            endSpan(SpanKind::Tactical, span);
            mTacticalKey.mValid = true;
            mTacticalKey.mEye = eye;
            mTacticalKey.mDir = at_axis;
            mTacticalKey.mFov = fov;
            mTacticalKey.mWidth = vw;
            mTacticalKey.mHeight = vh;
            mTacticalKey.mCut = cut_z;
            mTacticalKey.mCensus = mCensus.signature();
            mTacticalKey.mCutStamp = mCensus.cutStamp();
            mTacticalKey.mDebugView = tactical_debug_view;
            mTacticalKey.mRegionHandle = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
            ++mCost.mRenders;
        }
        else
        {
            ++mCost.mReuses;
        }
        updateWork(shot, !same, patch_rect.notEmpty() ? dissolve_key : 0, patch_rect, movers);
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
    mTacticalPickable = true;
    LL_INFOS_ONCE("SKOverview") << "tactical overview rendered " << vw << "x" << vh << ", census tris " << mCensus.triangles()
                                << LL_ENDL;
}


bool SKOverviewCapture::tacticalShown()
{
    return instanceExists() && instance().mTacticalOpen && SKModalView::top() == &instance();
}

// static
bool SKOverviewCapture::handleRightClick(S32 x, S32 y)
{
    return tacticalShown() && instance().rightClick(x, y);
}

bool SKOverviewCapture::rightClick(S32 x, S32 y)
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mTacticalViewport[0], y * scale.mV[VY] - mTacticalViewport[1]);
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= mTacticalViewport[2] || at.mV[VY] >= mTacticalViewport[3])
    {
        return false;
    }
    // The mini-map's pick: every marker within the tactical pick radius, nearest first.
    const F32 radius = skmap::TACTICAL_PICK_PX * llmax(scale.mV[VX], scale.mV[VY]);
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
    // The tactical camera's ray through the pixel, against what the view draws: the census as cut, then the terrain,
    // then the agent's floor plane.
    LLVector3 from, dir;
    if (!cursorRay(mTacticalView, mTacticalProj, mTacticalViewport, at, from, dir))
    {
        return false;
    }
    const F32 PICK_RANGE_M = 4096.f;
    const LLVector3d origin = gAgent.getPosGlobalFromAgent(from);
    if (mCensus.raycast(origin, dir, PICK_RANGE_M, world))
    {
        return true;
    }
    LLVector4a start, end, hit;
    start.load3(from.mV);
    LLVector3 far_pt = from + dir * PICK_RANGE_M;
    end.load3(far_pt.mV);
    bool land = false;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        LLSpatialPartition* part = region->getSpatialPartition(LLViewerRegion::PARTITION_TERRAIN);
        S32 face = -1;
        if (part && part->lineSegmentIntersect(start, end, false, false, true, false, &face, &hit))
        {
            end = hit; // the next region's test only looks nearer
            land = true;
        }
    }
    if (land)
    {
        world = gAgent.getPosGlobalFromAgent(LLVector3(end.getF32ptr()));
        return true;
    }
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    LLVector3 plane_hit;
    if (!rayPlane(from, dir, floor_z, plane_hit))
    {
        return false;
    }
    world = gAgent.getPosGlobalFromAgent(plane_hit);
    return true;
}

bool SKOverviewCapture::canPan() const
{
    return !LLViewerJoystick::getInstance()->getOverrideCamera();
}

bool SKOverviewCapture::beginPan(S32 x, S32 y)
{
    if (!canPan())
    {
        panTrace("press ignored: the flycam holds the camera");
        return false;
    }
    // The composite clears mTacticalReady every frame, so a press between frames tests mTacticalPickable.
    if (!tacticalShown() || !mTacticalPickable)
    {
        panTrace("press ignored: no tactical frame shown yet");
        return false;
    }
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mTacticalViewport[0], y * scale.mV[VY] - mTacticalViewport[1]);
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= mTacticalViewport[2] || at.mV[VY] >= mTacticalViewport[3])
    {
        panTrace("press ignored: outside the tactical viewport");
        return false;
    }
    LLVector3 from, dir;
    if (!cursorRay(mTacticalView, mTacticalProj, mTacticalViewport, at, from, dir) || dir.mV[VZ] > -1e-6f)
    {
        panTrace("press ignored: the cursor's ray does not point down");
        return false;
    }
    LLVector3d grab_global;
    if (!groundAt(at, grab_global))
    {
        panTrace("press ignored: no ground under the cursor");
        return false;
    }
    mPan = Pan();
    mPan.mActive = true;
    mPan.mGrab = gAgent.getPosAgentFromGlobal(grab_global);
    mPan.mView = mTacticalView;
    mPan.mProj = mTacticalProj;
    std::copy(std::begin(mTacticalViewport), std::end(mTacticalViewport), mPan.mViewport);
    mPan.mEye = gAgentCamera.getCameraPositionGlobal();
    mPan.mFocus = gAgentCamera.getFocusGlobal();
    return true;
}

void SKOverviewCapture::pan(S32 x, S32 y)
{
    if (!mPan.mActive)
    {
        return;
    }
    // The camera only translates, so the press's ray through the cursor meets the grab's plane at Q and P - Q puts P
    // back under the cursor; nothing is picked again while dragging.
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const LLVector2 at(x * scale.mV[VX] - mPan.mViewport[0], y * scale.mV[VY] - mPan.mViewport[1]);
    LLVector3 from, dir, q;
    if (!cursorRay(mPan.mView, mPan.mProj, mPan.mViewport, at, from, dir) || !rayPlane(from, dir, mPan.mGrab.mV[VZ], q))
    {
        panTrace("drag step skipped: the cursor's ray misses the grab's plane");
        return;
    }
    LLVector3d step(mPan.mGrab.mV[VX] - q.mV[VX], mPan.mGrab.mV[VY] - q.mV[VY], 0.0);
    LLVector3d lo, hi;
    if (battlefieldBounds(lo, hi))
    {
        step = clampedPanStep(mPan.mEye, mPan.mFocus, step, lo, hi);
    }
    setTacticalCamera(mPan.mEye + step, mPan.mFocus + step);
}

// static
bool SKOverviewCapture::handleKeyPan(const std::string& action, bool fast)
{
    static const std::unordered_map<std::string, std::pair<F32, F32>> steps = {
        {"push_forward", {1.f, 0.f}}, {"run_forward", {1.f, 0.f}},   {"push_backward", {-1.f, 0.f}}, {"run_backward", {-1.f, 0.f}},
        {"slide_left", {0.f, 1.f}},   {"turn_left", {0.f, 1.f}},     {"run_left", {0.f, 1.f}},       {"slide_right", {0.f, -1.f}},
        {"turn_right", {0.f, -1.f}},  {"run_right", {0.f, -1.f}},
    };
    if (!tacticalShown())
    {
        return false;
    }
    const auto it = steps.find(action);
    if (it == steps.end())
    {
        return false;
    }
    // Several keys bound to one direction count once.
    SKOverviewCapture& self = instance();
    self.mKeyForward = llclamp(self.mKeyForward + it->second.first, -1.f, 1.f);
    self.mKeyLeft = llclamp(self.mKeyLeft + it->second.second, -1.f, 1.f);
    self.mKeyFast |= fast || action.compare(0, 4, "run_") == 0;
    return true;
}

void SKOverviewCapture::applyKeyPan()
{
    const F32 forward_step = mKeyForward;
    const F32 left_step = mKeyLeft;
    const bool fast = mKeyFast;
    mKeyForward = mKeyLeft = 0.f;
    mKeyFast = false;
    if ((forward_step == 0.f && left_step == 0.f) || mPan.mActive || !tacticalShown())
    {
        return;
    }
    // Screen-up on the ground from the live camera; looking straight down, its up axis is screen-up.
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLVector3 forward = camera->getAtAxis();
    forward.mV[VZ] = 0.f;
    if (forward.normVec() < 0.05f)
    {
        forward = camera->getUpAxis();
        forward.mV[VZ] = 0.f;
        if (forward.normVec() < 1.0e-3f)
        {
            return;
        }
    }
    const LLVector3 left(-forward.mV[VY], forward.mV[VX], 0.f);
    LLVector3 dir = forward * forward_step + left * left_step;
    dir.normVec();

    // A fraction of the visible ground's width a second, so it feels the same at any zoom.
    static LLCachedControl<F32> speed_setting(gSavedSettings, "SKTacticalKeyPanSpeed", 0.5f);
    const LLVector3d eye = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const F32 distance = llmax((F32)(eye - focus).magVec(), 1.f);
    const F32 ground_width = 2.f * distance * std::tan(camera->getView() * camera->getAspect() * 0.5f);
    const F32 metres = ground_width * llmax((F32)speed_setting, 0.f) * (fast ? 3.f : 1.f) * llmin(gFrameIntervalSeconds.value(), 0.1f);

    LLVector3d step(dir * metres);
    LLVector3d lo, hi;
    if (battlefieldBounds(lo, hi))
    {
        step = clampedPanStep(eye, focus, step, lo, hi);
    }
    setTacticalCamera(eye + step, focus + step);
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
        m.mFloor = gAgent.getPosAgentFromGlobal(where) - LLVector3(0.f, 0.f, (F32)(p.mPos.mdV[VZ] - p.mFeetZ));
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
        m.mFloor = gAgent.getPosAgentFromGlobal(LLVector3d(gAgent.getPositionGlobal().mdV[VX], gAgent.getPositionGlobal().mdV[VY], SKMapPeople::feetZ(gAgentAvatarp)));
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
    sMarker.bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, mSceneDepth);
    glUniform4f(sMarker.getUniformLocation(SK_VIEWPORT), (F32)mTacticalViewport[0], (F32)mTacticalViewport[1], (F32)vw, (F32)vh);
    glUniform1f(sMarker.getUniformLocation(SK_ZO), LLRender::sReverseZ ? 1.f : 0.f);
    glUniform1f(sMarker.getUniformLocation(SK_GHOST_ALPHA), MARKER_GHOST_ALPHA);

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
    // Zoomed out past SKTacticalFlatMarkerPixelsPerMetre the bodies give way, crossfaded, to the mini-map's own flat
    // markers at its constant pixel size, drawn over everything so they read as easily as on the mini-map.
    static LLCachedControl<F32> swap_ppm(gSavedSettings, "SKTacticalFlatMarkerPixelsPerMetre", 10.f);
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const F32 fade_step = mMarkerFadeTime < 0.0 ? 1.f : (F32)(now - mMarkerFadeTime) / FLAT_FADE_S;
    mMarkerFadeTime = now;
    std::map<LLUUID, F32> blends;
    const LLVector3 sideways = LLViewerCamera::getInstance()->getLeftAxis();
    auto faded = [](const LLColor4& c, F32 a) { return LLColor4(c.mV[0], c.mV[1], c.mV[2], c.mV[3] * a); };
    struct Flat
    {
        size_t mPerson = (size_t)-1;   // none for the agent's own
        LLVector2 mAt;
        LLVector2 mAhead;
        bool mHasAhead = false;
    };
    std::vector<Flat> flats;
    // Where a point lands in the window, off it or not; false only behind the camera.
    LLMatrix4a vp;
    vp.setMul(mTacticalView, mTacticalProj);
    auto screen = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
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
        return true;
    };
    for (const Marker& m : markers)
    {
        // Kept while any of it, feet to head, is near the window, so feet just off its edge leave the body drawn.
        LLVector2 at0, head;
        if (!screen(m.mFloor, at0) || !screen(m.mFloor + LLVector3(0.f, 0.f, m.mHeight > 0.f ? m.mHeight : 2.f), head))
        {
            continue;
        }
        const F32 margin = 32.f;
        if (llmax(at0.mV[VX], head.mV[VX]) < -margin || llmax(at0.mV[VY], head.mV[VY]) < -margin ||
            llmin(at0.mV[VX], head.mV[VX]) > vw + margin || llmin(at0.mV[VY], head.mV[VY]) > vh + margin)
        {
            continue;
        }
        LLVector2 side;
        const F32 ppm = screen(m.mFloor + sideways, side) ? (side - at0).length() : 0.f;
        const bool has_body = m.mHasShape && !m.mCoarse;
        // Hysteresis as well as the fade: a flat marker turns back into a body only a fifth past the threshold.
        const auto known = mFlatBlend.find(m.mId);
        F32 blend = known != mFlatBlend.end() ? known->second : (ppm < swap_ppm ? 1.f : 0.f);
        const bool small = ppm < swap_ppm * (blend > 0.5f ? 1.2f : 1.f);
        blend = has_body ? llclamp(blend + (small ? fade_step : -fade_step), 0.f, 1.f) : 1.f;
        blends[m.mId] = blend;
        LLVector3 top_c = m.mFloor;
        if (has_body && blend < 1.f)
        {
            // The avatar's own physics shape, about its position, at true size.
            const LLColor4 colour = faded(m.mColour, 1.f - blend);
            const std::vector<LLVector3>& shape = skcensus::avatarShape(m.mPose, m.mHeight);
            F32 top_z = -F32_MAX;
            for (size_t v = 0; v + 2 < shape.size(); v += 3)
            {
                const LLVector3 a = m.mCentre + shape[v] * m.mRotation;
                const LLVector3 b = m.mCentre + shape[v + 1] * m.mRotation;
                const LLVector3 c = m.mCentre + shape[v + 2] * m.mRotation;
                tri(a, b, c, colour);
                top_z = llmax(top_z, llmax(a.mV[VZ], llmax(b.mV[VZ], c.mV[VZ])));
            }
            if (blend < 0.5f)
            {
                top_c = LLVector3(m.mCentre.mV[VX], m.mCentre.mV[VY], top_z);
            }
            if (m.mHasHeading)
            {
                // The minimap's heading wedge as a flat slab: a solid triangle just clear of the body, its top flush
                // with the shape's, along the body's heading, in the minimap's proportions to the body's radius.
                const LLVector3 ahead(std::cos(m.mHeading), std::sin(m.mHeading), 0.f);
                const LLVector3 across(-ahead.mV[VY], ahead.mV[VX], 0.f);
                const F32 body_r = m.mRadius;
                const LLVector3 from(m.mCentre.mV[VX], m.mCentre.mV[VY], top_z - body_r * HEADING_THICK_PER_RADIUS * 0.5f);
                const LLVector3 tip = from + ahead * (body_r * (1.f + HEADING_TIP_PER_RADIUS));
                const LLVector3 base = from + ahead * (body_r * (1.f + HEADING_BASE_PER_RADIUS));
                const LLVector3 l = base + across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 rr = base - across * (body_r * HEADING_HALF_PER_RADIUS);
                const LLVector3 up(0.f, 0.f, body_r * HEADING_THICK_PER_RADIUS * 0.5f);
                tri(tip + up, l + up, rr + up, colour);
                tri(tip - up, rr - up, l - up, colour);
                tri(tip - up, l - up, l + up, colour);
                tri(tip - up, l + up, tip + up, colour);
                tri(rr - up, tip - up, tip + up, colour);
                tri(rr - up, tip + up, rr + up, colour);
                tri(l - up, rr - up, rr + up, colour);
                tri(l - up, rr + up, l + up, colour);
            }
        }
        // Labels, hover, picks and menus take the projected top of the marker, or the flat marker's top edge.
        LLVector2 top;
        bool placed = false;
        if (blend >= 0.5f)
        {
            Flat f;
            f.mPerson = m.mPerson;
            f.mAt = at0;
            LLVector2 tip;
            if (m.mHasHeading && screen(m.mFloor + LLVector3(std::cos(m.mHeading), std::sin(m.mHeading), 0.f), tip))
            {
                f.mAhead = tip - at0;
                f.mHasAhead = f.mAhead.normVec() > 1e-4f;
            }
            flats.push_back(f);
            const F32 radius = m.mPerson != (size_t)-1 ? skmapdraw::markerRadius(people[m.mPerson], skmap::FloorRelation::SAME)
                                                       : skmap::palette::SELF_DOT_RADIUS_PX;
            top = at0 + LLVector2(0.f, radius);
            placed = true;
        }
        else
        {
            placed = project(top_c, top);
        }
        if (placed)
        {
            if (m.mPerson != (size_t)-1)
            {
                mTacticalMarks.push_back({ m.mId, top });
                drawn.emplace_back(m.mPerson, top);
            }
        }
    }
    mFlatBlend.swap(blends);
    gGL.end();
    gGL.flush();
    sMarker.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sMarker.unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
    // The 2D pass the caller resumes in, where the flat markers draw as the mini-map draws them.
    gUIProgram.bind();
    if (flats.empty())
    {
        return;
    }
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    const F32 px = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
    auto radius_of = [&](const Flat& f)
    {
        return f.mPerson != (size_t)-1 ? skmapdraw::markerRadius(people[f.mPerson], skmap::FloorRelation::SAME)
                                       : skmap::palette::SELF_DOT_RADIUS_PX;
    };
    for (const Flat& f : flats)
    {
        skmapdraw::shadow(f.mAt, radius_of(f));
    }
    gGL.getTextureSlot(0)->unbind();
    gGL.begin(LLRender::TRIANGLES);
    for (const Flat& f : flats)
    {
        const F32 radius = radius_of(f);
        skmapdraw::colour(skmap::palette::CODE_INK);
        skmapdraw::disc(f.mAt, radius + MARKER_HALO_PX);
        if (f.mPerson != (size_t)-1)
        {
            skmapdraw::personMarker(people[f.mPerson], f.mAt, radius, f.mHasAhead ? &f.mAhead : nullptr,
                                    skmap::FloorRelation::SAME, 0.f, px);
            continue;
        }
        skmapdraw::colour(skmap::palette::SELF_DOT);
        if (f.mHasAhead)
        {
            const LLVector2 across(-f.mAhead.mV[VY], f.mAhead.mV[VX]);
            const LLVector2 base = f.mAt + f.mAhead * (radius + skmap::palette::HEADING_BASE_PX);
            skmapdraw::triangle(f.mAt + f.mAhead * (radius + skmap::palette::HEADING_TIP_PX),
                                base + across * skmap::palette::HEADING_HALF_PX, base - across * skmap::palette::HEADING_HALF_PX);
        }
        skmapdraw::disc(f.mAt, radius);
    }
    gGL.end();
    for (const Flat& f : flats)
    {
        if (f.mPerson != (size_t)-1)
        {
            skmapdraw::personCode(people[f.mPerson], f.mAt, skmap::FloorRelation::SAME);
        }
    }
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
        sComposite.bindTexture(LLShaderMgr::DIFFUSE_MAP, &sTacticalResolved, ALSamplers::BilinearClamp);
        glUniform4f(sComposite.getUniformLocation(SK_VIEWPORT), (F32)vx, (F32)vy, (F32)vw, (F32)vh);
        glUniform1f(sComposite.getUniformLocation(SK_TILT_PX), llmax((F32)tilt_setting, 0.f));
        uniformColour(sComposite.getUniformLocation(SK_GROUND), skmap::palette::CODE_INK);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sComposite.unbindTexture(LLShaderMgr::DIFFUSE_MAP);
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
        {
            // Only a mouselook LookAt is precise enough to draw here; a body's facing is the mini-map's rough hint.
            auto clip_project = [&](const LLVector3& agent_pos, LLVector2& out) -> bool
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
                return true;
            };
            gGL.begin(LLRender::TRIANGLES);
            for (const auto& entry : SKMapPeople::instance().firingLines())
            {
                const SKMapPeople::FiringLine& line = entry.second;
                LLVector2 a, b;
                if (!line.mPrecise || !clip_project(gAgent.getPosAgentFromGlobal(line.mFrom), a) ||
                    !clip_project(gAgent.getPosAgentFromGlobal(line.mTo), b))
                {
                    continue;
                }
                LLVector2 d = b - a;
                if (d.normVec() < 1e-3f)
                {
                    continue;
                }
                // The mini-map's line, brighter at the shooter and fainter over its reach.
                const F32 reach = static_cast<F32>(dist_vec(line.mFrom, line.mTo)) / skmap::palette::FIRING_REACH_M;
                const LLColor4 near_c(1.f, 1.f, 1.f, skmap::palette::FIRING_ALPHA_NEAR);
                const LLColor4 far_c(1.f, 1.f, 1.f,
                                     lerp(skmap::palette::FIRING_ALPHA_NEAR, skmap::palette::FIRING_ALPHA_FAR, llclamp(reach, 0.f, 1.f)));
                const LLVector2 n(-d.mV[VY] * 0.5f, d.mV[VX] * 0.5f);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
                gGL.color4fv(far_c.mV);
                gGL.vertex2f((b - n).mV[VX], (b - n).mV[VY]);
                gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
                gGL.color4fv(far_c.mV);
                gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
                gGL.color4fv(near_c.mV);
                gGL.vertex2f((a + n).mV[VX], (a + n).mV[VY]);
            }
            gGL.end();
            gGL.flush();
        }
        {
            // Projectiles over the markers, as on the mini-map: a streak the prim's length, fading to its tail.
            std::map<LLUUID, LLColor4> shooters;
            for (const SKMapPeople::Person& person : SKMapPeople::instance().people())
            {
                shooters[person.mId] = person.mColour;
            }
            const LLColor4 plain = LLColor4(skmap::palette::PROJECTILE.mR, skmap::palette::PROJECTILE.mG, skmap::palette::PROJECTILE.mB,
                                            skmap::palette::PROJECTILE.mA);
            gGL.begin(LLRender::TRIANGLES);
            // Fast movers' recent path, faded from the head's opacity to clear at the oldest point.
            static LLCachedControl<F32> trail_px(gSavedSettings, "SKMapProjectileMinWidthPx", 2.f);
            const F64 now = LLFrameTimer::getUptimeSeconds().value();
            for (const auto& entry : SKMapPeople::instance().fastTrails())
            {
                const SKMapPeople::FastTrail& trail = entry.second;
                const auto& pts = trail.mPoints;
                const F32 half = llmax(static_cast<F32>(trail_px), 0.5f) * 0.5f;
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                {
                    LLVector2 a, b;
                    if (!project(gAgent.getPosAgentFromGlobal(pts[i].second), a) ||
                        !project(gAgent.getPosAgentFromGlobal(pts[i + 1].second), b))
                    {
                        continue;
                    }
                    LLVector2 d = a - b;
                    if (d.normVec() < 1e-4f)
                    {
                        continue;
                    }
                    const F32 life = llmax(trail.mLife, 1e-3f);
                    const F32 alpha_a = trail.mAlpha * (1.f - llclamp(static_cast<F32>(now - pts[i].first) / life, 0.f, 1.f));
                    const F32 alpha_b = trail.mAlpha * (1.f - llclamp(static_cast<F32>(now - pts[i + 1].first) / life, 0.f, 1.f));
                    const LLVector2 n(-d.mV[VY] * half, d.mV[VX] * half);
                    auto at = [&](const LLVector2& v, F32 alpha)
                    {
                        gGL.color4f(plain.mV[VRED], plain.mV[VGREEN], plain.mV[VBLUE], alpha);
                        gGL.vertex2f(v.mV[VX], v.mV[VY]);
                    };
                    at(b - n, alpha_b);
                    at(a - n, alpha_a);
                    at(a + n, alpha_a);
                    at(b - n, alpha_b);
                    at(a + n, alpha_a);
                    at(b + n, alpha_b);
                }
            }
            for (const SKMapPeople::Projectile& p : SKMapPeople::instance().projectiles())
            {
                const LLVector3 head_agent = gAgent.getPosAgentFromGlobal(p.mPos);
                LLVector2 head, back;
                if (!project(head_agent, head) || !project(head_agent - p.mDir, back))
                {
                    continue;
                }
                LLVector2 dir = head - back;
                const F32 ppm = dir.normVec();
                if (ppm < 1e-4f)
                {
                    continue;
                }
                const auto found = p.mShooter.notNull() ? shooters.find(p.mShooter) : shooters.end();
                const LLColor4& colour = found != shooters.end() ? found->second : plain;
                if (!p.mBullet)
                {
                    LLVector3d box[8];
                    p.corners(box);
                    LLVector2 corners[8];
                    bool seen = true;
                    for (S32 k = 0; k < 8 && seen; ++k)
                    {
                        seen = project(gAgent.getPosAgentFromGlobal(box[k]), corners[k]);
                    }
                    if (seen)
                    {
                        skmapdraw::projectileFootprint(corners, dir, llmin(p.mSpeed * 0.15f, 6.f) * ppm, colour);
                    }
                    continue;
                }
                skmapdraw::projectile(head, dir, p.mLength * ppm, p.mWidth * ppm, colour);
            }
            gGL.end();
            gGL.flush();
        }
        // Avatars shown large enough carry their in-world name tag, the one LLVOAvatar keeps, in place of the marker label.
        static LLCachedControl<F32> tag_ppm(gSavedSettings, "SKTacticalNametagPixelsPerMetre", 8.f);
        const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
        const LLVector3 across = LLViewerCamera::getInstance()->getLeftAxis();
        std::vector<LLVOAvatar*> tagged(drawn.size(), nullptr);
        for (size_t i = 0; i < drawn.size(); ++i)
        {
            if (drawn[i].first >= people.size())
            {
                continue;
            }
            LLViewerObject* obj = gObjectList.findObject(people[drawn[i].first].mId);
            LLVOAvatar* av = obj && obj->isAvatar() && !obj->isDead() ? static_cast<LLVOAvatar*>(obj) : nullptr;
            LLVector2 a, b;
            if (av && av->mNameText.notNull() && project(av->getPositionAgent(), a) && project(av->getPositionAgent() + across, b)
                && (a - b).length() >= tag_ppm)
            {
                tagged[i] = av;
            }
        }
        drawTacticalLabels(drawn, tagged, vw, vh);
        gGL.flush();
        gUIProgram.unbind();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGLLastMatrix = NULL;
    }
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
                                       << "; gpu tactical " << mCost.mGpuTacticalMs / n << " ms, patch " << mCost.mGpuPatchMs / n
                                       << " ms, movers " << mCost.mGpuMoversMs / n << " ms, resolve " << mCost.mGpuResolveMs / n
                                       << " ms; census "
                                       << (census_slow ? "slow" : "normal") << ", scans " << census_scans << LL_ENDL;
        }
        mCost = Cost();
        mCost.mSince = now;
        mCost.mScansAtStart = scans_now;
    }
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LL_INFOS_ONCE("SKOverview") << "tactical overview composited at " << vx << "," << vy << " " << vw << "x" << vh << LL_ENDL;
}

void SKOverviewCapture::drawTacticalLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn,
                                           const std::vector<LLVOAvatar*>& tagged, S32 vw, S32 vh)
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

    {
        // Coded people carry the mini-map's badge, a fixed size over their marker's top whatever the zoom; nearest the
        // camera first, a badge mostly covering one already placed is left out, and the nearest draws on top. A flat
        // marker carries its code itself, so it takes no badge.
        const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
        std::vector<std::pair<F32, size_t>> coded;
        for (size_t i = 0; i < drawn.size(); ++i)
        {
            const size_t person = drawn[i].first;
            if (person >= people.size())
            {
                continue;
            }
            const auto blend = mFlatBlend.find(people[person].mId);
            const bool flat = blend != mFlatBlend.end() && blend->second >= 0.5f;
            if (!flat && !tagged[i] && people[person].mVisibility == skmap::Visibility::DOT_CODED &&
                !people[person].mCode.empty())
            {
                coded.emplace_back((gAgent.getPosAgentFromGlobal(people[person].mPos) - camera).lengthSquared(), i);
            }
        }
        std::sort(coded.begin(), coded.end());
        std::vector<std::pair<size_t, LLVector2>> badges;
        std::vector<std::pair<LLVector2, F32>> placed;
        for (const auto& [dist, i] : coded)
        {
            if (badges.size() >= MAX_BADGES)
            {
                break;
            }
            const SKMapPeople::Person& p = people[drawn[i].first];
            const F32 radius = skmapdraw::markerRadius(p, skmap::FloorRelation::SAME);
            const LLVector2 at = ui(drawn[i].second) + LLVector2(0.f, radius + BADGE_LIFT_PX);
            bool covered = false;
            for (const auto& [other, other_r] : placed)
            {
                covered = covered || (other - at).length() < llmax(radius, other_r);
            }
            if (!covered)
            {
                placed.emplace_back(at, radius);
                badges.emplace_back(drawn[i].first, at);
            }
        }
        for (auto it = badges.rbegin(); it != badges.rend(); ++it)
        {
            skmapdraw::personBadge(people[it->first], it->second);
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
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (drawn[i].first < people.size() && !tagged[i])
        {
            dots.push_back({ people[drawn[i].first].mId, ui(drawn[i].second) });
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

    // The in-world tags, nearest the camera last so it draws on top, just above each marker's projected top.
    std::vector<std::pair<F32, size_t>> order;
    const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (tagged[i])
        {
            order.emplace_back((tagged[i]->getPositionAgent() - eye).lengthSquared(), i);
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
    for (const auto& [dist, i] : order)
    {
        const LLVector2 at = ui(drawn[i].second);
        tagged[i]->mNameText->drawFlat(at.mV[VX], at.mV[VY] + 6.f);
    }

    gGL.popUIMatrix();
    LLFontGL::sCurOrigin = origin;
    LLFontGL::sCurDepth = depth;
    LLFontGL::sCurScaleX = cur_sx;
    LLFontGL::sCurScaleY = cur_sy;
}
