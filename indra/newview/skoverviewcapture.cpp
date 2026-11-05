/**
 * @file skoverviewcapture.cpp
 * @brief The overview capture: the census as styled shapes over the terrain, as region maps and a camera view.
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

#include "skoverviewcapture.h"

#include "llagent.h"
#include "llbase64.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llregionhandle.h"
#include "llsurface.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"
#include "skoverviewprograms.h"
#include "skrooms.h"
#include "skworldcensus.h"

#include <algorithm>
#include <cmath>
#include <limits>

extern bool gCubeSnapshot;

namespace
{
    LLRenderTarget sCapture;      // depth, and a class code in R (skoverview::Code), an invisible face in B
    LLRenderTarget sCaptureNear;  // the same for the agent's neighbours, at half the resolution
    LLRenderTarget sCaptureFar;   // and for regions further off, at a quarter
    LLRenderTarget sViewCapture;
    LLRenderTarget sViewResolved;
    LLRenderTarget sViewWork;   // the static capture copied, with the movers drawn over it
    LLGLSLShader sResolve;
    LLGLSLShader sComposite;
    LLGLSLShader sGeom;       // the census: class code and the face's own normal
    LLGLSLShader sGeomCut;    // the census with a cut height per vertex, for the camera view
    LLGLSLShader sGeomCutDissolve;  // the same with the see-through circles, the only permutation that discards
    LLGLSLShader sGeomCapLift;      // the cut census's section pass, the only permutation that writes depth
    LLGLSLShader sGeomCapLiftDissolve;
    LLGLSLShader sGeomId;           // the probes' id render: the linkset id, no cut, no discard, no depth write
    LLGLSLShader sMarker;     // markers over the camera view, depth-tested by hand
    LLGLSLShader sCommands;   // the census pages' command lists, where the session computes

    const F32 CENSUS_RANGE_M = 128.f;
    const size_t SK_MAX_TERRITORY = 16;       // regions styled as inside, at most (the resolve's array)
    const size_t SK_MAX_DISSOLVE = 16;        // see-through circles, at most (overviewGeomF.glsl's array)
    const F32 AVATAR_HALF_HEIGHT_M = 0.9f;     // a coarse-only avatar's position over its feet, having no box from the sim
    const F32 VIEW_NEAR_M = 0.5f;
    const F32 VIEW_MIN_M = 8.f;

    const F64 MAPS_WANTED_S = 2.0;  // the region maps keep capturing this long after a map last drew them
    F64 sMapsWantedAt = -1.0;
    F64 sViewWantedAt = -1.0;
    F32 sViewReachM = 0.f;
    skoverview::MapParams sMapParams;

    const F32 LEVEL_BELOW_M = 4.f;  // another region's map keeps the clusters reaching this far below the agent's floor
    const F32 LEVEL_ABOVE_M = 8.f;  // and this far above it
    const F32 ON_GROUND_M = 8.f;    // an agent's floor this close to the land is at ground level

    void uniformColour(LLGLSLShader& prog, const LLStaticHashedString& name, const LLColor3& c)
    {
        prog.uniform3f(name, c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE]);
    }

    // The overview's own uniforms, looked up in each program's map once a bind rather than by string in GL: thirty
    // names would be a heavier edit to LLShaderMgr's reserved table than a lookup here. Its samplers are reserved names.
    const LLStaticHashedString SK_INV_VP("sk_inv_vp");
    const LLStaticHashedString SK_SLOT_COUNT("sk_slot_count");
    const LLStaticHashedString SK_LEVEL("sk_level");
    const LLStaticHashedString SK_PLANES("sk_planes");
    const LLStaticHashedString SK_PATCH_ON("sk_patch_on");
    const LLStaticHashedString SK_VIEW_PROJ("sk_view_proj");
    const LLStaticHashedString SK_OFFSET_AGENT("sk_offset_agent");
    const LLStaticHashedString SK_SIZE("sk_size");
    const LLStaticHashedString SK_PATCH("sk_patch");
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
                gGL.getScissor(mSaved);
                gGL.setScissor(rect->mLeft, rect->mBottom, rect->getWidth(), rect->getHeight());
                mSet = true;
            }
        }
        ~ScissorTo()
        {
            // As LLGLSScissor: only a box that was known is put back.
            if (mSet && mSaved[2] >= 0 && mSaved[3] >= 0)
            {
                gGL.setScissor(mSaved[0], mSaved[1], mSaved[2], mSaved[3]);
            }
        }

    private:
        LLGLState mState;
        S32 mSaved[4] = { 0, 0, -1, -1 };
        bool mSet = false;
    };

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

    /// The territory the resolve styles as inside, in agent space: every connected region, the agent's first, so the
    /// void beyond them dims over its grid and their outer edge glows wherever a capture looks.
    std::vector<LLVector4> territory()
    {
        std::vector<LLVector4> out;
        LLViewerRegion* region = gAgent.getRegion();
        auto add = [&out](LLViewerRegion* r)
        {
            const LLVector3 o = r->getOriginAgent();
            const F32 w = r->getWidth();
            out.push_back(LLVector4(o.mV[VX], o.mV[VY], o.mV[VX] + w, o.mV[VY] + w));
        };
        if (region)
        {
            add(region);
        }
        for (LLViewerRegion* other : LLWorld::getInstance()->getRegionList())
        {
            if (other != region && out.size() < SK_MAX_TERRITORY)
            {
                add(other);
            }
        }
        return out;
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
    LLViewerRegion* mRegion = nullptr; // this frame's, set before any capture; none for a void tile
    LLRenderTarget mResolved; // the map, RGBA8
    LLVector3d mOrigin;
    F32 mWidth = 0.f;
    F64 mLast = -1.0;
    U32 mRing = 0;            // 0 the agent's region, 1 a neighbour, more the further off
    bool mCut = false;        // whether it was taken through the agent's cut
    U64 mCutStamp = 0;        // the census cut it was taken with
    bool mVoid = false;       // a square with no region in it near the agent's (MapParams::mVoidTiles)
    bool mValid = false;
};

SKOverviewCapture::SKOverviewCapture()
{
    // The overview's last look per record, in the census file's record order, so an offline reference can judge
    // invisible parts as the overview did: base64 of one byte a record, 255 for a part it never looked at.
    skworld::CensusSection seen;
    seen.mName = "overview.seen";
    seen.mWrite = [this](std::ostream& out)
    {
        std::vector<U8> bytes;
        if (const SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
        {
            bytes.reserve(snap->mRecords.size());
            for (const skworld::RecordPtr& r : snap->mRecords)
            {
                bytes.push_back(r ? mCensus.lastSeen(r->mPart) : 255);
            }
        }
        out << '"' << LLBase64::encode(bytes.data(), bytes.size()) << '"';
    };
    seen.mRead = [](const LLSD&, U32) { return true; };
    SKWorldCensus::instance().addCensusSection(seen);
}

SKOverviewCapture::~SKOverviewCapture()
{
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus::getInstance()->removeCensusSection("overview.seen");
    }
}

void SKOverviewCapture::onFrame()
{
    // The census is kept only for maps or a camera view; the setting alone draws nothing.
    if (!mapsWanted() && !viewWanted())
    {
        if (instanceExists())
        {
            instance().updateDemand(false);
        }
        return;
    }
    instance().frame();
}

// static
void SKOverviewCapture::setWorldHidden(bool hidden)
{
    // The world census runs slowly only while the view hides the world (census freshness matters little then).
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus::instance().setSlowCadence(hidden);
    }
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
    sViewCapture.release();
    sViewResolved.release();
    sViewWork.release();
}

void SKOverviewCapture::releaseGL()
{
    sCapture.release();
    sCaptureNear.release();
    sCaptureFar.release();
    sViewCapture.release();
    sViewResolved.release();
    sViewWork.release();
    mSpans.clear();
    mProbes.release();
    mCensus.release();
    mMaps.clear();
    mViewKey.mValid = false;
    mWorkValid = false;
    mGLFailed = false;
}

void SKOverviewCapture::updateDemand(bool wanted)
{
    if (!SKWorldCensus::instanceExists())
    {
        return;
    }
    // The region maps want the census over the agent's neighbours as well; the camera view as far as it looks, in
    // 64 m steps so a zoom does not restart the demand every frame.
    static LLCachedControl<F32> maps_range(gSavedSettings, "SKOverviewCensusRange", 384.f);
    F32 range = mapsWanted() ? llmax((F32)maps_range, CENSUS_RANGE_M) : CENSUS_RANGE_M;
    if (viewWanted())
    {
        range = llmax(range, std::ceil(llmax(sViewReachM, CENSUS_RANGE_M) / 64.f) * 64.f);
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
void SKOverviewCapture::noteMapsWanted(const skoverview::MapParams& params)
{
    sMapsWantedAt = LLFrameTimer::getUptimeSeconds().value();
    sMapParams = params;
}

// static
bool SKOverviewCapture::mapsWanted()
{
    return sMapsWantedAt >= 0.0 && LLFrameTimer::getUptimeSeconds().value() - sMapsWantedAt < MAPS_WANTED_S;
}

// static
void SKOverviewCapture::noteViewWanted()
{
    sViewWantedAt = LLFrameTimer::getUptimeSeconds().value();
}

// static
bool SKOverviewCapture::viewWanted()
{
    return sViewWantedAt >= 0.0 && LLFrameTimer::getUptimeSeconds().value() - sViewWantedAt < MAPS_WANTED_S;
}

// static
void SKOverviewCapture::setViewReach(F32 reach_m)
{
    sViewReachM = llmax(reach_m, 0.f);
}

// static
skoverview::ViewShotParams skoverview::ViewShotParams::fromViewerCamera(const LLVector3d& centre)
{
    ViewShotParams params;
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    params.mEye = camera->getOrigin();
    params.mAt = camera->getAtAxis();
    params.mLeft = camera->getLeftAxis();
    params.mUp = camera->getUpAxis();
    params.mFov = camera->getView();
    params.mDrawDistance = camera->getFar();
    params.mCentre = centre;
    std::copy(std::begin(gGLViewport), std::end(gGLViewport), params.mViewport);
    return params;
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
    // The command list's storage blocks (overviewCommandsC.glsl), past the reserved blocks' points.
    const U32 SLOT_TABLE_BINDING = 6;
    const U32 COMMANDS_BINDING = 7;

    /// The frustum planes (agent space, a*x + b*y + c*z + d <= 0 inside) in the frame of a page whose origin lies at
    /// offset_agent, which both command builders test its slots' boxes in.
    void pagePlanes(const F32 agent[6][4], const LLVector3& offset_agent, F32 out[6][4])
    {
        for (U32 i = 0; i < 6; ++i)
        {
            out[i][0] = agent[i][0];
            out[i][1] = agent[i][1];
            out[i][2] = agent[i][2];
            out[i][3] = (F32)((F64)agent[i][3] + (F64)agent[i][0] * offset_agent.mV[VX] + (F64)agent[i][1] * offset_agent.mV[VY] +
                              (F64)agent[i][2] * offset_agent.mV[VZ]);
        }
    }

    /// The program each of skoverview::PROGRAMS builds, in its order.
    LLGLSLShader* const OVERVIEW_SHADERS[] = { &sResolve,     &sComposite,           &sGeom,   &sGeomCut, &sGeomCutDissolve,
                                               &sGeomCapLift, &sGeomCapLiftDissolve, &sGeomId, &sMarker };
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
        // The cut table is a storage block where the session computes, a buffer texture elsewhere.
        const bool ssbo = program.mCutTable && SKOverviewCutTable::useStorageBlock();
        if (ssbo)
        {
            shader.addPermutation("SK_CUT_TABLE_SSBO", "1");
        }
        shader.mFeatures.mMinGLSLVersion = ssbo ? 430 : 0;
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
    // The census's command lists are built on the GPU where the session computes, on the CPU elsewhere.
    if (ok && LLRender::sCompute)
    {
        sCommands.mName = skoverview::COMMANDS_NAME;
        sCommands.mShaderFiles.clear();
        sCommands.mShaderFiles.push_back(std::make_pair(skoverview::COMMANDS_COMPUTE, GL_COMPUTE_SHADER));
        sCommands.mShaderLevel = llmax(level, 1);
        sCommands.clearPermutations();
        if (!sCommands.createShader())
        {
            LL_WARNS("Shader") << "The overview's command list program did not build; the census culls on the CPU" << LL_ENDL;
            sCommands.unload();
        }
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
    sCommands.unload();
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
        case SpanKind::View:
            mStats.gpu_view_ms = ms;
            mCost.mGpuViewMs += ms;
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
    // The avatar's feet are the floor; the air cells only refine it, since their nearest floor can be the ground under a street.
    F32 feet = fallback_z;
    if (isAgentAvatarValid())
    {
        feet = gAgentAvatarp->getPositionAgent().mV[VZ] - 0.5f * gAgentAvatarp->getScale().mV[VZ];
    }
    F32 floor = 0.f;
    floor_z = skoverview::place::agentFloor(at, feet, floor) ? floor : feet;
    cut_z = floor_z + (F32)cut;
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
    if (mapsWanted() || viewWanted())
    {
        LL_PROFILE_ZONE_NAMED("sk overview census update");
        mCensus.update();
    }

    if (mapsWanted())
    {
        // A view rendered last frame works out the cut in its own render, which the maps then take.
        if (LLFrameTimer::getFrameCount() - mViewFrame > 1)
        {
            // The cut's roofs, rooms and columns are GPU probes, so the maps run a probe frame of their own.
            static LLCachedControl<U32> probe_people(gSavedSettings, "SKOverviewProbeBudget", 8);
            mProbes.beginFrame(sGeomId, llmax((U32)probe_people, 1u));
            updateCut(gAgent.getPositionGlobal(), sMapParams.mOccupants);
            mProbes.endFrame();
        }
        captureMaps(LLFrameTimer::getUptimeSeconds().value());
    }
    else
    {
        mMaps.clear();
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
        // The camera view's resolves are timed; the maps' fall inside their capture's span.
        ALQuerySpan span = shot.mTopDown ? ALQuerySpan() : gGL.queries().beginSpan();
        resolved.bindTarget();
        // The viewport stays whole with a rect: the resolve addresses its inputs by window pixel.
        gGL.setViewport(0, 0, w, h);
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
        prog.uniformMatrix4fv(SK_INV_VP, 1, GL_FALSE, inv_vp.getF32ptr());
        prog.uniform1f(SK_ZO, LLRender::sReverseZ ? 1.f : 0.f);
        prog.uniform1f(SK_FAR_DEPTH, al_window_far());
        prog.uniform3f(SK_EYE, shot.mEye.mV[VX], shot.mEye.mV[VY], shot.mEye.mV[VZ]);
        prog.uniform1f(SK_TOP_DOWN, shot.mTopDown ? 1.f : 0.f);
        {
            // World up on screen (world north looking straight down), so the hatch keeps its angle when the camera rolls.
            LLVector4a up;
            shot.mView.rotate(LLVector4a(0.f, 0.f, 1.f), up);
            if (up[0] * up[0] + up[1] * up[1] < 0.01f)
            {
                shot.mView.rotate(LLVector4a(0.f, 1.f, 0.f), up);
            }
            const F32 len = llmax(sqrtf(up[0] * up[0] + up[1] * up[1]), 1e-6f);
            prog.uniform2f(SK_SCREEN_UP, up[0] / len, up[1] / len);
        }
        prog.uniform3f(SK_FOCUS, shot.mFocus.mV[VX], shot.mFocus.mV[VY], shot.mFocus.mV[VZ]);
        prog.uniform1f(SK_CUT_FLAG, shot.mPartCuts ? 1.f : 0.f);
        prog.uniform1f(SK_LEVEL_TOP, shot.mLevelTop);
        prog.uniform1f(SK_CUT, shot.mCut);
        prog.uniform1f(SK_WATER_Z, shot.mWater);
        prog.uniform1f(SK_WATER_DEPTH, mPalette.mWaterDepthM);
        uniformColour(prog, SK_INVISIBLE, mPalette.mInvisible);
        static LLCachedControl<U32> debug_view(gSavedSettings, "SKOverviewDebugView", 0);
        prog.uniform1i(SK_DEBUG_VIEW, (GLint)(U32)debug_view);
        prog.uniform1i(SK_CLASS_MASK, shot.mTopDown ? (GLint)sMapParams.mClassMask : -1);
        {
            // The territory: the regions styled as inside, at most SK_MAX_TERRITORY of them.
            std::vector<LLVector4> rects = shot.mTerritory.empty() ? std::vector<LLVector4>{ shot.mRegion } : shot.mTerritory;
            if (rects.size() > SK_MAX_TERRITORY)
            {
                rects.resize(SK_MAX_TERRITORY);
            }
            prog.uniform4fv(SK_REGIONS, (GLsizei)rects.size(), rects.front().mV);
            prog.uniform1i(SK_REGION_COUNT, (GLint)rects.size());
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

    gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
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
        prog.uniform4fv(SK_CEILING, 1, plane);
    }
}

void SKOverviewCapture::drawMovers(const Shot& shot)
{
    LL_PROFILE_ZONE_SCOPED;
    const S32 w = sViewWork.getWidth();
    const S32 h = sViewWork.getHeight();
    ALQuerySpan span = gGL.queries().beginSpan();
    sViewWork.bindTarget();
    gGL.setViewport(0, 0, w, h);
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
        sGeom.uniform4fv(CLIP_PLANE, 1, no_cut);
        sGeom.uniformMatrix3fv(SK_EYE_TO_WORLD, 1, GL_FALSE, eye_to_world);
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
    sViewWork.flush();
    endSpan(SpanKind::Movers, span);
}

bool SKOverviewCapture::ensureWork()
{
    const S32 w = sViewCapture.getWidth();
    const S32 h = sViewCapture.getHeight();
    if (sViewWork.isComplete() && (S32)sViewWork.getWidth() == w && (S32)sViewWork.getHeight() == h)
    {
        return true;
    }
    sViewWork.release();
    mWorkValid = false;
    if (!sViewWork.allocate(w, h, GL_RGBA8, true, true, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                                LLRenderTarget::DEPTH_FMT_32F))
    {
        LL_WARNS_ONCE("SKOverview") << "camera view work target did not allocate" << LL_ENDL;
        return false;
    }
    return true;
}

void SKOverviewCapture::updateWork(const Shot& shot, bool static_new, U64 patch_key, const LLRect& patch_rect, bool movers)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!ensureWork())
    {
        mSceneDepth = &sViewCapture;
        resolve(sViewCapture, sViewResolved, shot);
        return;
    }
    const S32 w = sViewWork.getWidth();
    const S32 h = sViewWork.getHeight();
    const bool whole = static_new || !mWorkValid;
    LLGLDisable scissor(GL_SCISSOR_TEST); // a blit is scissored too
    if (whole)
    {
        sViewWork.copyContents(sViewCapture, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        mWorkValid = true;
        mPatchRect = LLRect();
        mMoverRect = LLRect();
    }
    mSceneDepth = &sViewWork;

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
        sViewWork.copyContents(sViewCapture, restore.mLeft, restore.mBottom, restore.mRight, restore.mTop, restore.mLeft,
                                   restore.mBottom, restore.mRight, restore.mTop, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                                   GL_NEAREST);
    }
    if (redraw_patch)
    {
        LL_PROFILE_ZONE_NAMED("sk overview view patch");
        ALQuerySpan span = gGL.queries().beginSpan();
        render(sViewWork, sViewResolved, shot, false, &patch_rect);
        endSpan(SpanKind::Patch, span);
    }
    if (movers && mover_rect.notEmpty())
    {
        drawMovers(shot);
    }
    if (whole)
    {
        resolve(sViewWork, sViewResolved, shot);
    }
    else
    {
        LLRect dirty = rectUnion(restore, redraw_patch ? patch_rect : LLRect());
        if (dirty.notEmpty())
        {
            LL_PROFILE_ZONE_NAMED("sk overview view dirty resolve");
            dirty.stretch(RESOLVE_REACH_PX);
            dirty.intersectWith(LLRect(0, h, w, 0));
            resolve(sViewWork, sViewResolved, shot, &dirty);
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

    // The census, region by region and page by page, a page a multi-draw, nearer than the terrain where they coincide.
    const std::vector<SKOverviewCensus::Region>& regions = mCensus.regions();
    // Each page's slots the shot sees, merged into runs: on the level the shot keeps, in the frustum, and with a patch,
    // on screen inside it. A page's slots in vertex order, so adjacent kept ones draw as one run.
    struct Runs
    {
        std::vector<GLint> mFirst;
        std::vector<GLsizei> mCount;
    };
    std::vector<std::vector<Runs>> runs(regions.size());
    // Where the session computes, the lists are built on the GPU, a dispatch a page, a patch's on-screen test there
    // too; a context without compute builds them here.
    bool gpu_commands = LLRender::sCompute && sCommands.isComplete();
    if (gpu_commands)
    {
        LL_PROFILE_ZONE_NAMED("sk overview command tables");
        for (const SKOverviewCensus::Region& region : regions)
        {
            for (const SKOverviewCensus::Page& page : region.mPages)
            {
                const size_t n = page.mSlots.size();
                if (!n || !page.mTableDirty)
                {
                    continue;
                }
                const size_t table_bytes = n * sizeof(skoverview::SlotCull);
                const size_t command_bytes = n * sizeof(ALDrawElementsIndirectCommand);
                if (page.mSlotTable.size() == table_bytes)
                {
                    // Instrumented, not fixed: a write into a buffer the last capture's dispatch read may wait on Mesa.
                    LL_PROFILE_ZONE_NAMED("sk overview slot table upload");
                    page.mSlotTable.upload(0, page.mSlots.data(), table_bytes);
                }
                else if (!page.mSlotTable.allocate(table_bytes, ALGPUBuffer::DYNAMIC, page.mSlots.data(), "SK census slots"))
                {
                    gpu_commands = false;
                }
                if (page.mCommands.size() != command_bytes &&
                    !page.mCommands.allocate(command_bytes, 0, nullptr, "SK census commands"))
                {
                    gpu_commands = false;
                }
                page.mTableDirty = !gpu_commands;
            }
        }
    }
    {
        LL_PROFILE_ZONE_NAMED("sk overview cull groups");
        LLMatrix4a view_proj;
        view_proj.setMul(shot.mView, shot.mProj);
        // The frustum's sides and near plane, agent space; nothing is culled by the far plane.
        F32 agent_planes[6][4];
        for (U32 i = 0; i < 6; ++i)
        {
            const LLPlane& plane = cull.getAgentPlane(i);
            for (U32 k = 0; k < 4; ++k)
            {
                agent_planes[i][k] = i == LLCamera::AGENT_PLANE_FAR ? (k == 3 ? -1.f : 0.f) : plane[k];
            }
        }
        std::vector<U32> first, count;
        for (size_t r = 0; r < regions.size(); ++r)
        {
            const SKOverviewCensus::Region& region = regions[r];
            const LLVector3 offset_agent = gAgent.getPosAgentFromGlobal(region.mOrigin);
            F32 planes[6][4];
            pagePlanes(agent_planes, offset_agent, planes);
            runs[r].resize(region.mPages.size());
            if (gpu_commands)
            {
                // The page's slots in vertex order, kept or not, one command each.
                LL_PROFILE_ZONE_NAMED("sk overview commands");
                sCommands.bind();
                const F32 level[4] = { shot.mLevel.mAll ? 1.f : 0.f, shot.mLevel.mGround ? 1.f : 0.f, shot.mLevel.mLo,
                                       shot.mLevel.mHi };
                sCommands.uniform4fv(SK_LEVEL, 1, level);
                sCommands.uniform4fv(SK_PLANES, 6, &planes[0][0]);
                sCommands.uniform1ui(SK_PATCH_ON, patch ? 1u : 0u);
                if (patch)
                {
                    const GLint size[4] = { w, h, 0, 0 };
                    const GLint rect[4] = { patch->mLeft, patch->mBottom, patch->mRight, patch->mTop };
                    sCommands.uniformMatrix4fv(SK_VIEW_PROJ, 1, GL_FALSE, view_proj.getF32ptr());
                    sCommands.uniform3f(SK_OFFSET_AGENT, offset_agent.mV[VX], offset_agent.mV[VY], offset_agent.mV[VZ]);
                    sCommands.uniform4iv(SK_SIZE, 1, size);
                    sCommands.uniform4iv(SK_PATCH, 1, rect);
                }
                for (const SKOverviewCensus::Page& page : region.mPages)
                {
                    if (page.mSlots.empty())
                    {
                        continue;
                    }
                    sCommands.uniform1ui(SK_SLOT_COUNT, (GLuint)page.mSlots.size());
                    gGL.bindStorageBuffer(SLOT_TABLE_BINDING, page.mSlotTable);
                    gGL.bindStorageBuffer(COMMANDS_BINDING, page.mCommands);
                    sCommands.dispatchThreads((U32)page.mSlots.size());
                }
                sCommands.unbind();
                continue;
            }
            for (size_t p = 0; p < region.mPages.size(); ++p)
            {
                const SKOverviewCensus::Page& page = region.mPages[p];
                skoverview::buildCommands(page.mSlots, shot.mLevel, planes, first, count);
                for (size_t i = 0; patch && i < count.size(); ++i)
                {
                    if (!count[i])
                    {
                        continue;
                    }
                    const skoverview::SlotCull& slot = page.mSlots[i];
                    const LLVector3 lo_agent = offset_agent + LLVector3(slot.mLo[0], slot.mLo[1], slot.mLo[2]);
                    const LLVector3 hi_agent = offset_agent + LLVector3(slot.mHi[0], slot.mHi[1], slot.mHi[2]);
                    LLVector4a lo, hi;
                    lo.load3(lo_agent.mV);
                    hi.load3(hi_agent.mV);
                    LLRect on_screen;
                    if (!(SKOverviewMovers::screenRectOf(view_proj, lo, hi, w, h, on_screen) && on_screen.overlaps(*patch)))
                    {
                        count[i] = 0;
                    }
                }
                skoverview::mergeRuns(first, count, runs[r][p].mFirst, runs[r][p].mCount);
            }
        }
    }
    if (gpu_commands)
    {
        // The draws read the commands the dispatches wrote; the slot and command points are left empty.
        gGL.memoryBarrier(LLRender::BARRIER_COMMAND);
        gGL.unbindStorageBuffer(SLOT_TABLE_BINDING);
        gGL.unbindStorageBuffer(COMMANDS_BINDING);
    }
    U32 census_draws = 0;
    std::function<void()> census;
    if (!regions.empty())
    {
        census = [&regions, &runs, &census_draws, gpu_commands, &shot, patch, &cut_table = mCensus.cutTable()]()
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
            if (shot.mPartCuts)
            {
                // A build that landed since the last cut is drawn whole until applyCut writes its cuts.
                cut_table.flush();
            }
            // The camera view cuts each part at its own height (SKOverviewCensus::applyCut); a map at the one plane.
            // A table whose store could not be allocated has nothing to read, so its parts are drawn whole.
            const bool cuts = shot.mPartCuts && cut_table.publishedRows() > 0;
            // Circles bind the one permutation that discards; without them every pass keeps early depth.
            const bool dissolve = cuts && !shot.mDissolve.empty();
            LLGLSLShader& geom = !cuts ? sGeom : dissolve ? sGeomCutDissolve : sGeomCut;
            // The section pass alone writes depth, lifting each fragment onto its part's cut.
            LLGLSLShader& lift = dissolve ? sGeomCapLiftDissolve : sGeomCapLift;
            LLMatrix4a inverse_view;
            inverse_view.setInverse(shot.mView);
            const F32* m = inverse_view.getF32ptr();
            const F32 eye_to_world[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
            // A page's kept runs in one call: the commands the GPU built, or the runs the CPU merged.
            auto drawPage = [&runs, &census_draws, gpu_commands](size_t r, size_t p, const SKOverviewCensus::Page& page)
            {
                if (gpu_commands)
                {
                    if (!page.mSlots.empty())
                    {
                        page.mBuffer->setBuffer();
                        page.mBuffer->multiDrawElementsIndirect(LLRender::TRIANGLES, page.mCommands, 0, (GLsizei)page.mSlots.size());
                        ++census_draws;
                    }
                    return;
                }
                const Runs& kept = runs[r][p];
                if (kept.mFirst.empty())
                {
                    return;
                }
                page.mBuffer->setBuffer();
                page.mBuffer->multiDrawElements(LLRender::TRIANGLES, kept.mFirst.data(), kept.mCount.data(),
                                                (GLsizei)kept.mFirst.size());
                ++census_draws;
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
                prog.uniform4fv(CLIP_PLANE, 1, shot.mPartCuts ? no_cut : plane);
                prog.uniformMatrix3fv(SK_EYE_TO_WORLD, 1, GL_FALSE, eye_to_world);
                uniformCeiling(prog, shot.mView);
                static LLCachedControl<U32> debug_view(gSavedSettings, "SKOverviewDebugView", 0);
                prog.uniform1i(SK_DEBUG_CUT, cuts && debug_view == 5 ? 1 : 0);
                const GLsizei n = (GLsizei)llmin(shot.mDissolve.size(), (size_t)SK_MAX_DISSOLVE);
                if (dissolve && n)
                {
                    prog.uniform1i(SK_DISSOLVE_COUNT, n);
                    prog.uniform1f(SK_DISSOLVE_SOFT, shot.mDissolveSoft);
                    prog.uniform4fv(SK_DISSOLVE, n, shot.mDissolve.front().mV);
                    prog.uniform1fv(SK_DISSOLVE_FLOOR, n, shot.mDissolveFloor.data());
                }
                if (cuts)
                {
                    cut_table.bind(prog);
                }
            };
            // The clip plane, the ceiling and the cuts are clip distances every overview geometry program writes.
            LLGLEnable clip0(GL_CLIP_DISTANCE0);
            LLGLEnable clip1(GL_CLIP_DISTANCE0 + 1);
            LLGLEnable clip2(GL_CLIP_DISTANCE0 + 2);
            LLGLEnable clip3(GL_CLIP_DISTANCE0 + 3);
            bindGeom(geom);
            // B carries a cut part's cap flag; the invisible pass after it raises it to 1 where it draws.
            gGL.setColorMask(true, true, cuts, true);
            LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
            gGL.setPolygonOffset(-1.f, -4.f);
            // Front faces, then the back faces of solids the cut went through, which stand for their section. Where
            // each part carries its own cut, a back face is the section only where the pixel's ray from the cut plane
            // is inside a cut solid: more of the cut parts' back faces past it than front, every one of which lies below the
            // cut. There it is lifted onto the cut plane (overviewGeomF.glsl), so a floor running through the solid
            // does not hide it, and the outside faces in front of a far wall's inside are left alone.
            // An open part (a physics mesh, wound as uploaded) is a surface with no inside: its back faces are drawn
            // with depth and class as its front faces are, and it takes no part in the section passes.
            enum { CAP_FRONT, CAP_BACK, CAP_PARITY, CAP_FILL, CAP_OPEN_BACK };
            std::vector<S32> passes = { CAP_FRONT };
            if (cuts)
            {
                passes.push_back(CAP_OPEN_BACK);
            }
            passes.push_back(CAP_BACK);
            if (cuts)
            {
                passes.push_back(CAP_PARITY);
                passes.push_back(CAP_FILL);
                LLGLState scissor(GL_SCISSOR_TEST, patch != nullptr); // a patch clears its own stencil only
                // GL's clear stencil, 0, which nothing in the viewer sets but the transient pool, which puts it back.
                gGL.setStencilMask(0xFF);
                gGL.clear(GL_STENCIL_BUFFER_BIT);
            }
            for (S32 pass : passes)
            {
                bindGeom(pass == CAP_FILL ? lift : geom);
                bound->uniform1i(SK_CAP_PASS, pass);
                const bool parity = pass == CAP_PARITY;
                LLGLDepthTest depth(GL_TRUE, pass == CAP_FRONT || pass == CAP_OPEN_BACK || pass == CAP_FILL ? GL_TRUE : GL_FALSE,
                                    parity ? GL_ALWAYS : GL_LEQUAL);
                LLGLState cull_face(GL_CULL_FACE, !parity);
                gGL.setCullFace(pass == CAP_FRONT ? GL_BACK : GL_FRONT);
                LLGLState stencil(GL_STENCIL_TEST, pass == CAP_PARITY || pass == CAP_FILL);
                if (parity)
                {
                    // A winding count, not parity: overlapping solids (walls meeting at corners) and mirrored ones
                    // stay inside, where an odd/even count turns their overlap and its surroundings inside out.
                    gGL.setStencilFunc(GL_ALWAYS, 0, 0xFF);
                    gGL.setStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_INCR_WRAP);
                    gGL.setStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_DECR_WRAP);
                    gGL.setStencilMask(0xFF);
                    gGL.setColorMask(false, false);
                }
                else if (pass == CAP_FILL)
                {
                    gGL.setStencilFunc(GL_NOTEQUAL, 0, 0xFF);
                    gGL.setStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                    gGL.setStencilMask(0x00);
                    gGL.setColorMask(true, true, true, true);
                }
                for (size_t r = 0; r < regions.size(); ++r)
                {
                    const SKOverviewCensus::Region& region = regions[r];
                    const LLVector3 offset_agent = gAgent.getPosAgentFromGlobal(region.mOrigin);
                    gGL.loadMatrix(shot.mView);
                    gGL.translatef(offset_agent.mV[VX], offset_agent.mV[VY], offset_agent.mV[VZ]);
                    gGLLastMatrix = NULL;
                    for (size_t p = 0; p < region.mPages.size(); ++p)
                    {
                        const SKOverviewCensus::Page& page = region.mPages[p];
                        const U32 c = page.mClass;
                        if (c == skoverview::INVISIBLE)
                        {
                            continue;
                        }
                        // Where each part carries its own cut, an uncut or open part's back face is no section, only a
                        // single-sided face seen from behind (a roof plane wound downwards): it keeps its class.
                        const bool section = pass == CAP_PARITY || pass == CAP_FILL || (pass == CAP_BACK && !shot.mPartCuts);
                        const U8 code = section ? (U8)skoverview::CODE_SECTION : skoverview::codeOf((skoverview::CensusClass)c);
                        gGL.diffuseColor4f(code / 255.f, 0.f, 0.f, 1.f);
                        // Coplanar parts of different classes resolve the same way every frame: floor over ramp over
                        // cover over wall over the rest, by a class's own depth offset rather than draw order.
                        static const F32 PRIORITY[skoverview::NUM_CENSUS_CLASSES] = { 8.f, 2.f, 4.f, 6.f, 1.f, 1.f, 1.f, 0.f, 0.f };
                        gGL.setPolygonOffset(-1.f, -4.f - PRIORITY[c]);
                        drawPage(r, p, page);
                    }
                }
                gGL.setCullFace(GL_BACK);
            }
            // The stencil state back at GL's defaults, as ALGLStencil leaves it.
            gGL.setStencilFunc(GL_ALWAYS, 0, ~0u);
            gGL.setStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            gGL.setStencilMask(~0u);
            bindGeom(geom);
            bound->uniform1i(SK_CAP_PASS, CAP_FRONT);
            {
                // Barriers, already drawn as their class: the hatch flag over them, hiding nothing, offset past every
                // class's own so it passes over the copy it lies on.
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
                LLGLDisable cull_face(GL_CULL_FACE);
                gGL.setColorMask(false, false, true, false);
                gGL.setPolygonOffset(-1.f, -16.f);
                for (size_t r = 0; r < regions.size(); ++r)
                {
                    const SKOverviewCensus::Region& region = regions[r];
                    const LLVector3 offset_agent = gAgent.getPosAgentFromGlobal(region.mOrigin);
                    gGL.loadMatrix(shot.mView);
                    gGL.translatef(offset_agent.mV[VX], offset_agent.mV[VY], offset_agent.mV[VZ]);
                    gGLLastMatrix = NULL;
                    gGL.diffuseColor4f(0.f, 0.f, 1.f, 1.f);
                    for (size_t p = 0; p < region.mPages.size(); ++p)
                    {
                        if (region.mPages[p].mClass == skoverview::INVISIBLE)
                        {
                            drawPage(r, p, region.mPages[p]);
                        }
                    }
                }
                gGL.setColorMask(true, false, false, false);
            }
            gGL.setPolygonOffset(0.f, 0.f);
            cut_table.unbind();
            bound->unbind();
        };
    }

    U32 draws = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("sk overview capture");
        LLTimer timer;
        capture.bindTarget();
        gGL.setViewport(0, 0, w, h);
        ScissorTo scissor_to_patch(patch);
        // The clear depth is always the convention's far plane (LLRender::applyDepthConvention), so it is not set here.
        const LLColor4 saved_clear = gGL.getClearColor();
        gGL.setClearColor(LLColor4::transparent);
        {
            // A clear obeys the write masks: whatever pass ran last could have left them on one channel, and the
            // terrain writes only R, so its texels kept the last capture's normals (G, A) and invisible flag (B)
            // and smeared as the camera turned. Every channel and the depth are cleared, every capture.
            LLGLDepthTest depth_write(GL_TRUE, GL_TRUE, GL_ALWAYS);
            LLGLState scissor(GL_SCISSOR_TEST, patch != nullptr); // a scissor left on would clear only part; a patch clears its own
            gGL.setColorMask(true, true);
            capture.clear();
        }
        gGL.setClearColor(saved_clear);
        draws = gPipeline.skRenderOverview(shot.mView, shot.mProj, cull, shot.mCut, census) + census_draws;
        LL_PROFILE_PLOT("census indirect draws", (int64_t)(gpu_commands ? census_draws : 0));
        capture.flush();
        mCost.mCaptureMs += timer.getElapsedTimeF32() * 1000.f;
    }

    if (resolve_now)
    {
        resolve(capture, resolved, shot);
    }

    memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
    gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;
    return draws;
}

void SKOverviewCapture::captureMaps(F64 now)
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    const auto& regions = LLWorld::getInstance()->getRegionList();
    const LLVector3d agent_min = agent_region->getOriginGlobal();
    const F64 agent_w = agent_region->getWidth();

    // The void tiles: the squares of the agent region's grid that no region covers, over the loaded regions' bounds
    // and one square past them, so the void between and around them is mapped and nothing beside a region is a hole.
    const bool void_tiles = sMapParams.mVoidTiles;
    const S32 VOID_RINGS_MAX = 8; // a 17 x 17 square at most, however far the regions reach
    std::vector<LLVector3d> voids;
    if (void_tiles)
    {
        S32 lo_x = 0, lo_y = 0, hi_x = 0, hi_y = 0;
        for (LLViewerRegion* r : regions)
        {
            const LLVector3d o = r->getOriginGlobal() - agent_min;
            const F64 w = r->getWidth();
            lo_x = llmin(lo_x, (S32)std::floor(o.mdV[VX] / agent_w + 0.001));
            lo_y = llmin(lo_y, (S32)std::floor(o.mdV[VY] / agent_w + 0.001));
            hi_x = llmax(hi_x, (S32)std::ceil((o.mdV[VX] + w) / agent_w - 0.001) - 1);
            hi_y = llmax(hi_y, (S32)std::ceil((o.mdV[VY] + w) / agent_w - 0.001) - 1);
        }
        lo_x = llmax(lo_x - 1, -VOID_RINGS_MAX);
        lo_y = llmax(lo_y - 1, -VOID_RINGS_MAX);
        hi_x = llmin(hi_x + 1, VOID_RINGS_MAX);
        hi_y = llmin(hi_y + 1, VOID_RINGS_MAX);
        for (S32 dy = lo_y; dy <= hi_y; ++dy)
        {
            for (S32 dx = lo_x; dx <= hi_x; ++dx)
            {
                const LLVector3d min = agent_min + LLVector3d(dx * agent_w, dy * agent_w, 0.0);
                const bool covered = std::any_of(regions.begin(), regions.end(),
                                                 [&](LLViewerRegion* r)
                                                 {
                                                     const LLVector3d o = r->getOriginGlobal();
                                                     const F64 w = r->getWidth();
                                                     return o.mdV[VX] < min.mdV[VX] + agent_w - 0.5 && o.mdV[VX] + w > min.mdV[VX] + 0.5 &&
                                                            o.mdV[VY] < min.mdV[VY] + agent_w - 0.5 && o.mdV[VY] + w > min.mdV[VY] + 0.5;
                                                 });
                if (!covered)
                {
                    voids.push_back(min);
                }
            }
        }
    }
    auto voidAt = [&voids](const RegionMap& map)
    { return std::any_of(voids.begin(), voids.end(), [&map](const LLVector3d& v) { return to_region_handle(v) == map.mHandle; }); };

    // Regions that left the world take their maps with them, and void tiles a region now covers or the agent left.
    mMaps.erase(std::remove_if(mMaps.begin(), mMaps.end(),
                               [&](const std::unique_ptr<RegionMap>& map)
                               {
                                   if (map->mVoid)
                                   {
                                       return !voidAt(*map);
                                   }
                                   return std::none_of(regions.begin(), regions.end(), [&map](LLViewerRegion* r)
                                                       { return r->getHandle() == map->mHandle; });
                               }),
                 mMaps.end());
    for (const LLVector3d& min : voids)
    {
        const U64 handle = to_region_handle(min);
        auto it = std::find_if(mMaps.begin(), mMaps.end(),
                               [handle](const std::unique_ptr<RegionMap>& map) { return map->mVoid && map->mHandle == handle; });
        if (it == mMaps.end())
        {
            mMaps.push_back(std::make_unique<RegionMap>());
            RegionMap& map = *mMaps.back();
            map.mHandle = handle;
            map.mVoid = true;
            map.mOrigin = min;
            map.mWidth = (F32)agent_w;
            it = std::prev(mMaps.end());
        }
        const S32 dx = ll_round((F32)((min.mdV[VX] - agent_min.mdV[VX]) / agent_w));
        const S32 dy = ll_round((F32)((min.mdV[VY] - agent_min.mdV[VY]) / agent_w));
        (*it)->mRing = (U32)llmax(std::abs(dx), std::abs(dy));
        (*it)->mRegion = nullptr;
    }

    // A region's ring: 0 the agent's, 1 one touching it, then one more per region width of gap between them.
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
    if (sMapParams.mClassMask != mClassMask)
    {
        mClassMask = sMapParams.mClassMask;
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
                               [region](const std::unique_ptr<RegionMap>& map) { return !map->mVoid && map->mHandle == region->getHandle(); });
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
    for (std::unique_ptr<RegionMap>& map : mMaps)
    {
        if (map->mVoid && map->mLast < 0.0 && (!missing || map->mRing < missing->mRing))
        {
            missing = map.get();
            missing_region = nullptr;
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
        if (map.mRing == 0 || (!map.mRegion && !map.mVoid))
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
    // A void tile has no region of its own: its square is the map's, its water the agent region's, and it has no land.
    const F32 width = region ? region->getWidth() : map.mWidth;
    const LLVector3 origin = region ? region->getOriginAgent() : gAgent.getPosAgentFromGlobal(map.mOrigin);
    const F32 water = (region ? region : gAgent.getRegion())->getWaterHeight();
    const F32 land_max = region ? region->getLand().getMaxZ() : water;
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
        for (const SKOverviewCensus::Region& census_region : mCensus.regions())
        {
            if ((census_region.mOrigin - region_origin).magVecSquared() < 1.0)
            {
                for (const SKOverviewCensus::Group& group : census_region.mGroups)
                {
                    region_top = llmax(region_top, (F32)group.mMax.mdV[VZ]);
                }
            }
        }
    }
    const F32 top = agent ? llmax(region_top, land_max) + 1.f : llmax(mCensus.levelTop(shot.mLevel, cut_z), land_max) + 1.f;
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
    shot.mWater = water;
    // Styled as the camera view is: the void dimmed over its grid, the regions' outer edge glowing and the
    // borders between them dashed, the same in every map so the tiles meet.
    shot.mRegion = LLVector4(origin.mV[VX], origin.mV[VY], origin.mV[VX] + width, origin.mV[VY] + width);
    shot.mTerritory = territory();
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

    if (region)
    {
        map.mOrigin = region->getOriginGlobal();
    }
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

std::shared_ptr<const SKOverviewCensus::RoomFootprint> SKOverviewCapture::roomAt(const LLVector3d& floor)
{
    const SKWorldViewPtr world = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
    if (!world)
    {
        mRooms.clear();
        return {};
    }
    const U64 revision = world->airRevision();
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    mRooms.erase(std::remove_if(mRooms.begin(), mRooms.end(),
                                [revision, now](const HeldRoom& r) { return r.mRevision != revision || now - r.mAt > 5.0; }),
                 mRooms.end());
    // The room's floor is the floor of the air column the point is in, not the point's own height.
    SKWorldView::SpaceId start_id;
    skworld::Enclosure start_gap;
    // The gap of a space below stands in only within a metre: further down it is another storey's (a spaceless pocket).
    SKWorldView::AirColumn start_column;
    world->columnAt(floor, start_column);
    bool start_found = start_column.airGapAt(floor.mdV[VZ] + 0.3, start_id, start_gap);
    if (start_found && start_gap.mFloor < floor.mdV[VZ] - 1.0)
    {
        LLUUID mesh;
        start_found = start_column.enclosureAt(floor.mdV[VZ] + 0.3, start_gap, &mesh);
        start_id.mMesh = mesh;
        start_id.mId = start_gap.mSpaceId;
    }
    if (!start_found || start_id.mId == 0 ||
        (start_gap.mBucket != skworld::EnclosureBucket::INDOORS && start_gap.mBucket != skworld::EnclosureBucket::SHELTERED))
    {
        return {};
    }
    // The start space's room on its own mesh, or none (the street, a roof, a flight, shelter with no building over it):
    // then the column is cut instead.
    const SKWorldView::Mesh* mesh = nullptr;
    const skworld::Room* room = world->roomOf(start_id, &mesh);
    if (!room || !mesh)
    {
        return {};
    }
    const SKWorldView::RoomId id{ mesh->mId, room->mId };
    for (HeldRoom& held : mRooms)
    {
        if (held.mId == id && held.mHash == room->mHash)
        {
            held.mAt = now;
            return held.mFootprint;
        }
    }
    auto footprint = std::make_shared<SKOverviewCensus::RoomFootprint>();
    footprint->mMesh = id.mMesh;
    footprint->mId = id.mId;
    footprint->mHash = room->mHash;
    footprint->mOrigin = world->roomCellOrigin(*mesh, *room) - LLVector3d(skworld::RASTER_XY_M, skworld::RASTER_XY_M, 0.0);
    footprint->mW = room->mCellW + 2;
    footprint->mH = room->mCellH + 2;
    if (!room->mTruncated && room->mMask.size() == (size_t)room->mCellW * room->mCellH)
    {
        footprint->mMask.assign((size_t)footprint->mW * footprint->mH, 0);
        for (S32 y = 0; y < room->mCellH; ++y)
        {
            for (S32 x = 0; x < room->mCellW; ++x)
            {
                if (!room->mMask[(size_t)y * room->mCellW + x])
                {
                    continue;
                }
                for (S32 dy = 0; dy <= 2; ++dy)
                {
                    for (S32 dx = 0; dx <= 2; ++dx)
                    {
                        footprint->mMask[(size_t)(y + dy) * footprint->mW + x + dx] = 1;
                    }
                }
            }
        }
    }
    mRooms.push_back({ id, room->mHash, revision, now, footprint });
    return footprint;
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
    const SKWorldViewPtr world = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
    const SKWorldView::AirColumn* column = columnOf(world, id, feet, now);
    const bool out = column && skoverview::place::outdoorsIn(*column, feet);
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

const SKWorldView::AirColumn* SKOverviewCapture::columnOf(const SKWorldViewPtr& world, const LLUUID& id,
                                                             const LLVector3d& feet, F64 now)
{
    if (!world)
    {
        return nullptr;
    }
    HeldColumn& held = mColumns[id];
    // The dissolve passes the feet back through agent coordinates, so the plan matches to a millimetre, not exactly.
    if (held.mAt != now || held.mRevision != world->airRevision() || std::fabs(held.mXY.mdV[VX] - feet.mdV[VX]) > 1e-3 ||
        std::fabs(held.mXY.mdV[VY] - feet.mdV[VY]) > 1e-3)
    {
        held.mAt = now;
        held.mRevision = world->airRevision();
        held.mXY = feet;
        world->columnAt(feet, held.mColumn);
    }
    return &held.mColumn;
}

void SKOverviewCapture::updateCut(const LLVector3d& centre, const std::vector<skoverview::Occupant>& occupants_in)
{
    // Cut by building, for every view that draws the census: the agent's own at its level, and each other building
    // in its region that holds an occupant at its highest occupant's level; the rest, and other regions, whole. What
    // stands between the camera view's eye and someone is dissolved round them instead.
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    static LLCachedControl<F32> cut_height(gSavedSettings, "SKOverviewCutHeight", 2.5f);
    std::vector<SKOverviewCensus::Occupant> occupants;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
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
                         const skoverview::place::Whereabouts& where)
    {
        SKOverviewCensus::Occupant o;
        o.mId = id;
        o.mSelf = self;
        o.mFloor = floor;
        o.mFeet = feet;
        o.mCut = cut;
        const bool enclosed = where.mFound && (where.mIndoor || where.mBucket == skoverview::place::Bucket::SHELTERED);
        if (enclosed && where.mCeiling < 1e29f)
        {
            // Indoors the storey's own air bounds the cut: just under its ceiling, so the floor above is gone.
            o.mCut = where.mCeiling - 0.1f; // floor + 2.5 stands when the ceiling is unknown
        }
        o.mPriority = -(F32)dist_vec(feet, centre);
        o.mOutdoors = outdoorsAt(id, feet, now);
        if (!o.mOutdoors && enclosed)
        {
            o.mRoom = roomAt(floor);
            o.mRoomCellM = skworld::RASTER_XY_M;
        }
        mixPos(floor);
        mixPos(feet);
        mix((U64)(S64)std::llround(o.mCut * 4.f));
        mix(id.getCRC32());
        mix(o.mOutdoors);
        mix(o.mRoom ? o.mRoom->mHash : 0);
        mix(o.mRoom ? o.mRoom->mId : 0);
        mix(o.mRoom ? o.mRoom->mMesh.getCRC32() : 0);
        occupants.push_back(std::move(o));
    };
    const LLVector3d agent_floor = gAgent.getPosGlobalFromAgent(
        LLVector3(gAgent.getPositionAgent().mV[VX], gAgent.getPositionAgent().mV[VY], floor_z));
    LLVector3d agent_feet = agent_floor; // the feet as they stand, which a hop lifts off the floor
    if (isAgentAvatarValid())
    {
        // Avatars stand upright, so the box's half height is all of it.
        agent_feet.mdV[VZ] = gAgentAvatarp->getPositionGlobal().mdV[VZ] - 0.5 * (F64)gAgentAvatarp->getScale().mV[VZ];
    }
    skoverview::place::Whereabouts agent_where;
    const SKWorldViewPtr world = SKRooms::instanceExists() ? SKRooms::instance().view() : nullptr;
    if (const SKWorldView::AirColumn* column = columnOf(world, gAgent.getID(), agent_feet, now))
    {
        agent_where = skoverview::place::whereabouts(*column, agent_feet);
    }
    addPerson(gAgent.getID(), true, agent_floor, agent_feet, cut_z, agent_where);
    for (const skoverview::Occupant& p : occupants_in)
    {
        if (p.mHidden || !p.mZKnown || p.mId == gAgent.getID())
        {
            continue;
        }
        LLVector3d floor = p.mFeet;
        floor.mdV[VZ] = p.mWhere.mFound ? (F64)p.mWhere.mFloor : p.mFeet.mdV[VZ];
        addPerson(p.mId, false, floor, p.mFeet, (F32)floor.mdV[VZ] + (F32)cut_height, p.mWhere);
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
    mCensus.applyCut(gAgent.getRegion(), occupants, key, &mProbes);
}

U64 SKOverviewCapture::viewDissolve(Shot& shot, const skoverview::ViewShotParams& params, S32 w, S32 h, LLRect& rect)
{
    LL_PROFILE_ZONE_SCOPED;
    const skoverview::DissolveParams& dissolve = params.mDissolve;
    const F32 MIN_PX = 40.f;
    const F32 MAX_PX = 320.f;
    const F32 EASE_S = 0.2f;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const F32 step = mDissolveTime < 0.0 ? 1.f : llclamp((F32)(now - mDissolveTime) / EASE_S, 0.f, 1.f);
    mDissolveTime = now;

    // Who wants one: the agent, and every occupant shown.
    std::map<LLUUID, LLVector3> wanted;
    if (dissolve.mEnabled)
    {
        // Only close up, and only for someone in outdoor air: indoors the building's cut already shows them.
        const LLVector3 eye = params.mEye;
        auto want = [&](const LLUUID& id, const LLVector3& feet)
        {
            if (dist_vec(eye, feet + LLVector3(0.f, 0.f, 0.9f)) <= dissolve.mReach &&
                outdoorsAt(id, gAgent.getPosGlobalFromAgent(feet), now))
            {
                wanted[id] = feet;
            }
        };
        // Anchored to the feet as they stand, as markers are, never the floor under them.
        LLVector3 self = gAgent.getPositionAgent();
        self.mV[VZ] = isAgentAvatarValid()
                          ? self.mV[VZ] - 0.5f * gAgentAvatarp->getScale().mV[VZ]
                          : self.mV[VZ] - AVATAR_HALF_HEIGHT_M;
        want(gAgent.getID(), self);
        for (const skoverview::Occupant& p : params.mOccupants)
        {
            if (p.mHidden || !p.mZKnown || p.mId == gAgent.getID())
            {
                continue;
            }
            LLVector3 feet = gAgent.getPosAgentFromGlobal(p.mFeet);
            feet.mV[VZ] = (F32)p.mFeet.mdV[VZ]; // global z is agent z
            want(p.mId, feet);
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
        if (cw <= VIEW_NEAR_M || feet_clip.getF32ptr()[3] <= 0.f || head_clip.getF32ptr()[3] <= 0.f)
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
        const F32 radius = llclamp(tall * dissolve.mRadius, MIN_PX, MAX_PX) * d.mAmount * d.mAmount * (3.f - 2.f * d.mAmount);
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
    shot.mDissolveSoft = llclamp(dissolve.mSoftness, 0.f, 1.f);
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
        mix(std::llround(shot.mDissolveSoft * 1000.f));
    }
    return key;
}

bool SKOverviewCapture::renderView(const skoverview::ViewShotParams& params)
{
    LL_PROFILE_ZONE_SCOPED;
    mViewReady = false;
    const S32 vx = params.mViewport[0], vy = params.mViewport[1], vw = params.mViewport[2], vh = params.mViewport[3];
    if (vw < 16 || vh < 16)
    {
        LL_WARNS_ONCE("SKOverview") << "camera view skipped: viewport " << vw << "x" << vh << LL_ENDL;
        return false;
    }
    if (gCubeSnapshot || !LLPipeline::sRenderDeferred || !gPipeline.shadersLoaded() || !gAgent.getRegion())
    {
        return false;
    }
    if (!ensureGL(sViewCapture, sViewResolved, vw, vh))
    {
        LL_WARNS_ONCE("SKOverview") << "camera view skipped: targets or shaders unavailable" << LL_ENDL;
        return false;
    }

    // The caller's camera; the cut and the shading stay the avatar's.
    F32 floor_z, cut_z;
    cutHeights(gAgent.getPositionGlobal(), gAgent.getPositionAgent().mV[VZ], floor_z, cut_z);
    const LLVector3 eye = params.mEye;
    const LLVector3 dir = params.mAt;
    const LLVector3 left = params.mLeft;
    const LLVector3 up = params.mUp;
    const LLVector3 target = gAgent.getPosAgentFromGlobal(params.mCentre);
    const F32 distance = llmax(dist_vec(eye, target), VIEW_MIN_M);

    Shot shot;
    LLVector4a eye4, dir4, up4;
    eye4.load3(eye.mV);
    dir4.load3(dir.mV);
    up4.load3(up.mV);
    shot.mView = LLMatrix4a::lookDir(eye4, dir4, up4);
    const F32 fov = params.mFov;
    const F32 aspect = (F32)vw / (F32)vh;
    // The cull reaches past the focus by the draw distance, so the surrounds stay whole however far out it zooms;
    // under reverse-Z the projection clips nothing far.
    const F32 far_m = distance + llmax(params.mDrawDistance, 2.f * CENSUS_RANGE_M);
    shot.mProj = al_perspective(fov, aspect, VIEW_NEAR_M, LLRender::sReverseZ ? std::numeric_limits<F32>::infinity() : far_m);
    shot.mEye = eye;
    shot.mCut = cut_z;
    // The fade follows the focus, but the height colours stay on the agent's floor, eased so a climb or teleport never flickers.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (mViewShadeTime < 0.0 || now - mViewShadeTime > 1.0)
    {
        mViewShadeZ = floor_z;
    }
    else
    {
        mViewShadeZ += (floor_z - mViewShadeZ) * (F32)(1.0 - std::exp(-(now - mViewShadeTime) / 0.25));
    }
    mViewShadeTime = now;
    shot.mFocus = LLVector3(target.mV[VX], target.mV[VY], mViewShadeZ);
    shot.mPartCuts = true;
    shot.mLevelTop = cut_z;
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLVector3 region_min = region->getOriginAgent();
        const F32 region_width = region->getWidth();
        shot.mWater = region->getWaterHeight();
        shot.mRegion = LLVector4(region_min.mV[VX], region_min.mV[VY], region_min.mV[VX] + region_width,
                                 region_min.mV[VY] + region_width);
        shot.mTerritory = territory();
    }
    shot.mTopDown = false;
    shot.mCull = *LLViewerCamera::getInstance();
    shot.mCull.setView(fov);
    shot.mCull.setAspect(aspect);
    shot.mCull.setNear(VIEW_NEAR_M);
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
        static LLCachedControl<U32> view_debug_view(gSavedSettings, "SKOverviewDebugView", 0);
        static LLCachedControl<U32> probe_people(gSavedSettings, "SKOverviewProbeBudget", 8);
        mProbes.beginFrame(sGeomId, llmax((U32)probe_people, 1u));
        updateCut(params.mCentre, params.mOccupants);
        LLRect patch_rect;
        const U64 dissolve_key = viewDissolve(shot, params, vw, vh, patch_rect);
        const bool same = !force && mViewKey.mValid && (eye - mViewKey.mEye).magVecSquared() < 1e-4f &&
                          at_axis * mViewKey.mDir > 0.999999f && fov == mViewKey.mFov && vw == mViewKey.mWidth &&
                          vh == mViewKey.mHeight && std::fabs(cut_z - mViewKey.mCut) < 1e-3f &&
                          mCensus.signature() == mViewKey.mCensus &&
                          mCensus.cutStamp() == mViewKey.mCutStamp &&
                          (gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0) == mViewKey.mRegionHandle &&
                          (U32)view_debug_view == mViewKey.mDebugView;
        // The circles and the movers repair a copy of the static capture each frame; it is redone only on change.
        mMovers.setLoose(mCensus.restlessParts());
        const bool movers = mMovers.update();
        if (!same)
        {
            LL_PROFILE_ZONE_NAMED("sk overview view render");
            Shot scene = shot;
            scene.mDissolve.clear();
            scene.mDissolveFloor.clear();
            ALQuerySpan span = gGL.queries().beginSpan();
            render(sViewCapture, sViewResolved, scene, false);
            endSpan(SpanKind::View, span);
            mViewKey.mValid = true;
            mViewKey.mEye = eye;
            mViewKey.mDir = at_axis;
            mViewKey.mFov = fov;
            mViewKey.mWidth = vw;
            mViewKey.mHeight = vh;
            mViewKey.mCut = cut_z;
            mViewKey.mCensus = mCensus.signature();
            mViewKey.mCutStamp = mCensus.cutStamp();
            mViewKey.mDebugView = view_debug_view;
            mViewKey.mRegionHandle = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
            ++mCost.mRenders;
        }
        else
        {
            ++mCost.mReuses;
        }
        updateWork(shot, !same, patch_rect.notEmpty() ? dissolve_key : 0, patch_rect, movers);
    }

    mViewView = shot.mView;
    mViewProj = shot.mProj;
    mViewCentre = target;
    mViewReach = distance + CENSUS_RANGE_M;
    mViewViewport[0] = vx;
    mViewViewport[1] = vy;
    mViewViewport[2] = vw;
    mViewViewport[3] = vh;
    updatePick();
    mProbes.endFrame();
    mViewReady = true;
    mViewPickable = true;
    mViewFrame = LLFrameTimer::getFrameCount();
    LL_INFOS_ONCE("SKOverview") << "camera view rendered " << vw << "x" << vh << ", census tris " << mCensus.triangles()
                                << LL_ENDL;
    return true;
}

void SKOverviewCapture::updatePick()
{
    // The texels read in earlier frames, oldest first: the newest that landed is the one a pick reads.
    for (auto it = mPicksInFlight.begin(); it != mPicksInFlight.end();)
    {
        F32 depth = 0.f;
        const SKOverviewProbes::Status status = mProbes.pickResult(it->mFrame, depth);
        if (status == SKOverviewProbes::Status::Pending)
        {
            ++it;
            continue;
        }
        if (status == SKOverviewProbes::Status::Ready)
        {
            mPick = *it;
            mPick.mDepth = depth;
        }
        it = mPicksInFlight.erase(it);
    }
    // This frame's: the shown frame's depth (the static capture, or the work target with the circles and the movers).
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const S32 x = (S32)std::floor(gViewerWindow->getCurrentMouseX() * scale.mV[VX] - mViewViewport[0]);
    const S32 y = (S32)std::floor(gViewerWindow->getCurrentMouseY() * scale.mV[VY] - mViewViewport[1]);
    if (!mSceneDepth || mPicksInFlight.size() >= 4 || !mProbes.readDepth(*mSceneDepth, x, y))
    {
        return;
    }
    PickSample sample;
    sample.mX = x;
    sample.mY = y;
    sample.mView = mViewView;
    sample.mProj = mViewProj;
    sample.mViewport[2] = mViewViewport[2];
    sample.mViewport[3] = mViewViewport[3];
    sample.mAgentOrigin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    sample.mFrame = mProbes.frame();
    sample.mValid = true;
    mPicksInFlight.push_back(sample);
}

bool SKOverviewCapture::groundAt(const LLVector2& at, LLVector3d& world) const
{
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (now - mPicksSince >= 1.0)
    {
        if (mPicksByTexel || mPicksByFallback)
        {
            LL_INFOS("SKOverview") << "pick: " << mPicksByTexel << " by texel, " << mPicksByFallback << " by terrain or plane" << LL_ENDL;
        }
        mPicksByTexel = 0;
        mPicksByFallback = 0;
        mPicksSince = now;
    }
    // What the pixel shows, cut, dissolved and with the movers: the frame's depth under the cursor, while the cursor is
    // within two pixels of where it was read and the read is at most three frames old, and not the cleared depth.
    const PickSample& pick = mPick;
    if (pick.mValid && std::fabs(at.mV[VX] - (pick.mX + 0.5f)) <= 2.5f && std::fabs(at.mV[VY] - (pick.mY + 0.5f)) <= 2.5f &&
        mProbes.frame() - pick.mFrame <= 3 && pick.mDepth != al_window_far())
    {
        LLVector4a win, agent;
        win.set(pick.mX + 0.5f, pick.mY + 0.5f, pick.mDepth, 1.f);
        agent = al_unproject(win, pick.mView, pick.mProj, pick.mViewport);
        const F32* p = agent.getF32ptr();
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
        {
            world = pick.mAgentOrigin + LLVector3d(p[0], p[1], p[2]);
            ++mPicksByTexel;
            return true;
        }
    }
    ++mPicksByFallback;
    // Otherwise the view camera's ray through the pixel: the terrain, then the agent's floor plane.
    LLVector3 from, dir;
    if (!cursorRay(mViewView, mViewProj, mViewViewport, at, from, dir))
    {
        return false;
    }
    const F32 PICK_RANGE_M = 4096.f;
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

bool SKOverviewCapture::compositeView(const LLRect& rect, F32 tilt_px)
{
    if (!mViewReady || !sComposite.mProgramObject || !sViewResolved.isComplete())
    {
        LL_WARNS_ONCE("SKOverview") << "camera view composite skipped: rendered " << mViewReady << ", shader "
                                    << (sComposite.mProgramObject != 0) << ", target " << sViewResolved.isComplete()
                                    << LL_ENDL;
        return false;
    }
    mViewReady = false;
    LL_PROFILE_ZONE_SCOPED;
    const S32 vx = rect.mLeft, vy = rect.mBottom, vw = rect.getWidth(), vh = rect.getHeight();
    LLGLDisable scissor(GL_SCISSOR_TEST);
    gGL.setColorMask(true, true);
    LLTimer composite_timer;
    {
        LL_PROFILE_ZONE_NAMED("sk overview view composite");
        gGL.setViewport(vx, vy, vw, vh);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull_face(GL_CULL_FACE);
        sComposite.bind();
        sComposite.bindTexture(LLShaderMgr::DIFFUSE_MAP, &sViewResolved, ALSamplers::BilinearClamp);
        sComposite.uniform4f(SK_VIEWPORT, (F32)vx, (F32)vy, (F32)vw, (F32)vh);
        sComposite.uniform1f(SK_TILT_PX, llmax(tilt_px, 0.f));
        uniformColour(sComposite, SK_GROUND, mPalette.mGround);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sComposite.unbindTexture(LLShaderMgr::DIFFUSE_MAP);
        sComposite.unbind();
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
            LL_INFOS("SKOverviewCost") << "camera view per frame over " << mCost.mFrames << " frames: capture "
                                       << mCost.mCaptureMs / n << " ms, resolve " << mCost.mResolveMs / n << " ms, composite "
                                       << mCost.mCompositeMs / n << " ms; rendered " << mCost.mRenders << ", reused " << mCost.mReuses
                                       << "; gpu view " << mCost.mGpuViewMs / n << " ms, patch " << mCost.mGpuPatchMs / n
                                       << " ms, movers " << mCost.mGpuMoversMs / n << " ms, resolve " << mCost.mGpuResolveMs / n
                                       << " ms; census " << (census_slow ? "slow" : "normal") << ", scans " << census_scans
                                       << LL_ENDL;
        }
        mCost = Cost();
        mCost.mSince = now;
        mCost.mScansAtStart = scans_now;
    }
    gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LL_INFOS_ONCE("SKOverview") << "camera view composited at " << vx << "," << vy << " " << vw << "x" << vh << LL_ENDL;
    return true;
}

// static
LLGLSLShader& SKOverviewCapture::markerProgram()
{
    return sMarker;
}
