/**
 * @file skoutlinecapture.cpp
 * @brief Coverage glyphs of vehicles, rendered on the GPU into an atlas and refreshed as they change.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skoutlinecapture.h"

#include "lldrawable.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagepng.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <cmath>

using namespace SKOutlinePolicy;

namespace
{
    constexpr S32 ATLAS_SIZE = 2048;
    constexpr S32 SLOT_SIZE = 256;
    constexpr S32 TARGET_SIZE = SLOT_SIZE * SUPERSAMPLE;
    constexpr F64 CHECK_SECONDS = 0.25;  // how often a capture's parts are looked at for changes
    constexpr F64 MAX_WAIT_SECONDS = 5.0; // parts still loading after this render as they are
    constexpr S32 MIN_ACTIVE_FRAMES = 2;  // frames a freshly made-active root waits for its bridge to fill

    LLRenderTarget sTarget; // depth (and a scratch colour) at twice a slot's resolution
    LLRenderTarget sAtlas;  // R8 coverage, one slot per layer
    LLGLSLShader sResolve;
    U32 sQueries[2] = { 0, 0 };
    LLUUID sTimedRoot; // the capture whose render the queries are timing

    U64 mix(U64 h, U64 v)
    {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }

    U64 mixF(U64 h, F32 v, F32 step)
    {
        return mix(h, (U64)(S64)std::lround(v / step));
    }

    void gatherLinkset(LLViewerObject* obj, std::vector<LLViewerObject*>& out)
    {
        out.push_back(obj);
        for (LLViewerObject* child : obj->getChildren())
        {
            if (child && !child->isAvatar())
            {
                gatherLinkset(child, out);
            }
        }
    }

    // The part loaded at the best LOD it has: prims at high, meshes at the highest their asset holds.
    bool atTopLOD(LLVOVolume* vol)
    {
        if (!vol)
        {
            return true;
        }
        if (vol->isMesh())
        {
            if (!vol->getVolume() || !vol->getVolume()->isMeshAssetLoaded())
            {
                return false;
            }
            const S32 top = gMeshRepo.getActualMeshLOD(vol->getVolume()->getParams(), LLModel::LOD_HIGH);
            return top < 0 || vol->getLOD() >= top;
        }
        return vol->getLOD() >= LLModel::LOD_HIGH;
    }

    const char* reasonName(Reason r)
    {
        switch (r)
        {
            case Reason::FIRST: return "first";
            case Reason::CHANGED: return "changed";
            case Reason::MOVING: return "moving";
            case Reason::CONTEXT: return "context";
            default: return "";
        }
    }
}

struct SKOutlineCapture::Entry
{
    LLUUID root_id;
    bool pinned = false;
    F64 wanted = 0.0;       // last time a consumer asked
    F64 first_asked = 0.0;

    // Change tracking.
    U64 signature = 0;
    U64 rendered_signature = 0;
    F64 next_check = 0.0;
    F64 last_change = 0.0;
    LLVector3 last_pos;
    LLQuaternion last_rot;
    bool moving = false;
    bool top_lod = false;
    bool loading = false;   // a mesh missing or a drawable rebuilding
    S32 parts = 0;
    F32 distance = 0.f;

    // Readiness of a parked root, made active for its own bridge.
    S32 active_frames = -1;

    // The last render.
    bool rendered = false;
    bool lost = false;
    S32 slot = -1;
    Grid grid;
    F64 rendered_at = 0.0;
    U32 renders = 0;
    U32 draws = 0;
    F64 cpu_ms = 0.0;
    F64 gpu_ms = -1.0;
    Reason last_reason = Reason::NONE;

    SKOutlineGlyph glyph;
};

SKOutlineCapture::SKOutlineCapture()
    : mAtlas(ATLAS_SIZE, SLOT_SIZE)
{
}

SKOutlineCapture::~SKOutlineCapture() = default;

const SKOutlineGlyph* SKOutlineCapture::glyphFor(const LLUUID& root)
{
    SKOutlineCapture& self = instance();
    self.request(root);
    const auto it = self.mEntries.find(root);
    if (it == self.mEntries.end() || !it->second->rendered || it->second->lost || it->second->slot < 0)
    {
        return nullptr;
    }
    return &it->second->glyph;
}

void SKOutlineCapture::request(const LLUUID& root, bool pinned)
{
    if (root.isNull())
    {
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    std::unique_ptr<Entry>& entry = mEntries[root];
    if (!entry)
    {
        entry = std::make_unique<Entry>();
        entry->root_id = root;
        entry->first_asked = now;
        entry->glyph.root = root;
    }
    entry->wanted = now;
    entry->pinned = entry->pinned || pinned;
}

void SKOutlineCapture::forget(const LLUUID& root)
{
    const auto it = mEntries.find(root);
    if (it != mEntries.end())
    {
        mAtlas.release(it->second->slot);
        mEntries.erase(it);
    }
}

void SKOutlineCapture::forgetAll()
{
    mEntries.clear();
    mAtlas.clear();
}

void SKOutlineCapture::onFrame()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKOutlineCaptureEnabled", true);
    if (!enabled || !instanceExists())
    {
        return;
    }
    instance().frame();
}

void SKOutlineCapture::cleanupGL()
{
    if (instanceExists())
    {
        instance().releaseGL();
    }
    else
    {
        sTarget.release();
        sAtlas.release();
        sResolve.unload();
    }
}

void SKOutlineCapture::releaseGL()
{
    sTarget.release();
    sAtlas.release();
    sResolve.unload();
    if (sQueries[0])
    {
        glDeleteQueries(2, sQueries);
        sQueries[0] = sQueries[1] = 0;
    }
    sTimedRoot.setNull();
    mAtlas.clear();
    mGLFailed = false;
    for (auto& [id, entry] : mEntries)
    {
        entry->slot = -1;
        entry->lost = true;
    }
}

bool SKOutlineCapture::ensureGL()
{
    if (mGLFailed)
    {
        return false;
    }
    if (sAtlas.isComplete() && sTarget.isComplete() && sResolve.mProgramObject)
    {
        return true;
    }

    bool ok = sTarget.isComplete() ||
              sTarget.allocate(TARGET_SIZE, TARGET_SIZE, GL_R8, true, false, ALTextureSlot::TT_TEXTURE,
                               LLRenderTarget::MIPS_NONE, LLRenderTarget::DEPTH_FMT_32F);
    ok = ok && (sAtlas.isComplete() || sAtlas.allocate(ATLAS_SIZE, ATLAS_SIZE, GL_R8));
    if (ok)
    {
        // Every channel reads the coverage, so a consumer can draw it as a white mask or an alpha.
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, sAtlas.getTexture());
        const GLint swizzle[4] = { GL_RED, GL_RED, GL_RED, GL_RED };
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        gGL.getTextureSlot(0)->unbind();

        // A fresh atlas holds garbage outside the slots written, and the resolve writes whole slots, so clear it once.
        sAtlas.bindTarget();
        sAtlas.clear();
        sAtlas.flush();
    }
    if (ok && !sResolve.mProgramObject)
    {
        sResolve.mName = "SK Outline Coverage Resolve";
        sResolve.mShaderFiles.clear();
        sResolve.clearPermutations();
        sResolve.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        sResolve.mShaderFiles.push_back(std::make_pair("deferred/skOutlineResolveF.glsl", GL_FRAGMENT_SHADER));
        sResolve.mShaderLevel = 1;
        ok = sResolve.createShader();
    }
    if (!ok)
    {
        LL_WARNS("SKOutline") << "Outline capture could not allocate its atlas or load its resolve shader; captures are off "
                                 "until the GL context is rebuilt"
                              << LL_ENDL;
        mGLFailed = true;
        sTarget.release();
        sAtlas.release();
        sResolve.unload();
    }
    return ok;
}

void SKOutlineCapture::frame()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    static LLCachedControl<F32> budget_ms(gSavedSettings, "SKOutlineCaptureBudgetMs", 0.2f);
    static LLCachedControl<S32> max_renders(gSavedSettings, "SKOutlineCaptureMaxPerFrame", 4);
    static LLCachedControl<F32> moving_seconds(gSavedSettings, "SKOutlineCaptureMovingSeconds", 0.5f);
    static LLCachedControl<F32> idle_seconds(gSavedSettings, "SKOutlineCaptureIdleSeconds", 30.f);
    static LLCachedControl<bool> debug(gSavedSettings, "SKOutlineCaptureDebug", false);

    LLTimer frame_timer;
    const F64 now = LLTimer::getTotalSeconds();
    const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();

    // The GPU time of the last timed render, read only once it is ready.
    if (sTimedRoot.notNull())
    {
        GLint available = 0;
        glGetQueryObjectiv(sQueries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available)
        {
            GLuint64 t0 = 0, t1 = 0;
            glGetQueryObjectui64v(sQueries[0], GL_QUERY_RESULT, &t0);
            glGetQueryObjectui64v(sQueries[1], GL_QUERY_RESULT, &t1);
            const auto it = mEntries.find(sTimedRoot);
            if (it != mEntries.end())
            {
                it->second->gpu_ms = t1 > t0 ? (F64)(t1 - t0) / 1.0e6 : 0.0;
            }
            sTimedRoot.setNull();
        }
    }

    std::vector<Candidate> candidates;
    std::vector<Entry*> by_index;
    bool made_active = false;
    for (auto it = mEntries.begin(); it != mEntries.end();)
    {
        Entry& e = *it->second;
        LLViewerObject* root = gObjectList.findObject(e.root_id);
        const bool expired = !e.pinned && now - e.wanted > idle_seconds;
        if (expired || (!e.pinned && (!root || root->isDead())))
        {
            mAtlas.release(e.slot);
            it = mEntries.erase(it);
            continue;
        }
        ++it;
        if (!root || root->isDead() || !root->mDrawable || root->isAvatar())
        {
            continue;
        }

        // Motion is the root's own; parts' poses enter the signature in the root's frame.
        const LLVector3 pos = root->getRenderPosition();
        const LLQuaternion rot = root->getRenderRotation();
        e.moving = (pos - e.last_pos).lengthSquared() > 1.0e-4f || !(rot == e.last_rot);
        e.last_pos = pos;
        e.last_rot = rot;
        e.distance = (pos - eye).length();

        if (now >= e.next_check)
        {
            e.next_check = now + CHECK_SECONDS;
            std::vector<LLViewerObject*> parts;
            gatherLinkset(root, parts);
            U64 h = mix(0, parts.size());
            bool top = true;
            bool loading = false;
            for (LLViewerObject* part : parts)
            {
                h = mix(h, std::hash<LLUUID>{}(part->getID()));
                if (part != root)
                {
                    const LLVector3 p = part->getPosition();
                    const LLQuaternion q = part->getRotation();
                    for (S32 k = 0; k < 3; ++k)
                    {
                        h = mixF(h, p.mV[k], 0.01f);
                    }
                    for (S32 k = 0; k < 4; ++k)
                    {
                        h = mixF(h, q.mQ[k], 0.002f);
                    }
                }
                const LLVector3 s = part->getScale();
                for (S32 k = 0; k < 3; ++k)
                {
                    h = mixF(h, s.mV[k], 0.001f);
                }
                for (U8 t = 0; t < part->getNumTEs(); ++t)
                {
                    if (const LLTextureEntry* te = part->getTE(t))
                    {
                        h = mixF(h, te->getColor().mV[VALPHA], 1.f / 64.f);
                        h = mix(h, std::hash<LLUUID>{}(te->getID()));
                        h = mix(h, te->getGLTFMaterial() ? 1 : 0);
                    }
                }
                LLVOVolume* vol = dynamic_cast<LLVOVolume*>(part);
                if (vol)
                {
                    h = mix(h, (U64)(vol->getLOD() + 1));
                    h = mix(h, vol->getVolume() ? (U64)vol->getVolume()->getNumVolumeFaces() : 0);
                }
                top = top && atTopLOD(vol);
                LLDrawable* drawable = part->mDrawable;
                loading = loading || (vol && vol->isMesh() && (!vol->getVolume() || !vol->getVolume()->isMeshAssetLoaded())) ||
                          (drawable && drawable->isState(LLDrawable::REBUILD_ALL));
            }
            if (h != e.signature)
            {
                e.signature = h;
                e.last_change = now;
            }
            e.top_lod = top;
            e.loading = loading;
            e.parts = (S32)parts.size();
        }

        // A parked root has no bridge of its own: make it active, one per frame, and give it frames to fill.
        if (!root->mDrawable->getSpatialBridge())
        {
            if (!made_active)
            {
                made_active = true;
                LLTimer t;
                root->mDrawable->makeActive();
                e.active_frames = 0;
                LL_DEBUGS("SKOutline") << "made " << e.root_id << " active in " << t.getElapsedTimeF64() * 1000.0 << " ms"
                                       << LL_ENDL;
            }
            continue;
        }
        if (e.active_frames >= 0 && e.active_frames < MIN_ACTIVE_FRAMES)
        {
            ++e.active_frames;
            continue;
        }
        if (e.loading && now - e.last_change < MAX_WAIT_SECONDS)
        {
            continue;
        }

        const Reason reason = reasonFor(e.rendered, e.lost, e.signature != e.rendered_signature, e.moving,
                                        now - e.rendered_at, moving_seconds);
        // A capture with no slot waits while the atlas is full, rather than spending the frame failing.
        const bool no_room = e.slot < 0 && mAtlas.used() >= mAtlas.capacity();
        if (reason != Reason::NONE && !no_room)
        {
            Candidate c;
            c.index = (S32)by_index.size();
            c.reason = reason;
            c.distance = e.distance;
            c.waiting = now - (e.rendered ? e.rendered_at : e.first_asked);
            candidates.push_back(c);
            by_index.push_back(&e);
        }
    }

    order(candidates);
    mBudget.begin(budget_ms, max_renders);
    if (!candidates.empty() && ensureGL())
    {
        LLGLState::checkStates();
        for (const Candidate& c : candidates)
        {
            if (!mBudget.fits())
            {
                break;
            }
            Entry& e = *by_index[c.index];
            LLTimer t;
            // Timestamps rather than GL_TIME_ELAPSED, which cannot nest inside the profiler's own GPU zones.
            const bool time_gpu = debug && sTimedRoot.isNull();
            if (time_gpu)
            {
                if (!sQueries[0])
                {
                    glGenQueries(2, sQueries);
                }
                glQueryCounter(sQueries[0], GL_TIMESTAMP);
            }
            e.last_reason = c.reason;
            const bool ok = render(e);
            if (time_gpu)
            {
                glQueryCounter(sQueries[1], GL_TIMESTAMP);
                sTimedRoot = e.root_id;
            }
            if (ok)
            {
                e.cpu_ms = t.getElapsedTimeF64() * 1000.0;
                mBudget.spend(e.cpu_ms);
            }
        }
        LLGLState::checkStates();
    }
    mLastFrameRenders = mBudget.renders();
    mLastFrameMs = frame_timer.getElapsedTimeF64() * 1000.0;

    static LLCachedControl<F32> settle_seconds(gSavedSettings, "SKOutlineCaptureSettleSeconds", 3.f);
    for (auto& [id, entry] : mEntries)
    {
        entry->glyph.settled = entry->rendered && entry->rendered_signature == entry->signature &&
                               settled(entry->top_lod && !entry->loading, now - entry->last_change, settle_seconds);
    }
}

bool SKOutlineCapture::render(Entry& e)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("sk outline capture");

    static LLCachedControl<S32> target_cells(gSavedSettings, "SKOutlineCaptureCells", 160);
    static LLCachedControl<F32> max_cell(gSavedSettings, "SKOutlineCaptureMaxCellMetres", 0.25f);
    static LLCachedControl<F32> alpha_cutoff(gSavedSettings, "SKOutlineCaptureAlphaCutoff", 0.02f);

    // Whatever happens, this signature is dealt with: a failed render does not retry every frame.
    e.rendered_signature = e.signature;
    e.rendered_at = LLTimer::getTotalSeconds();

    LLViewerObject* root = gObjectList.findObject(e.root_id);
    LLDrawable* drawable = root ? root->mDrawable.get() : nullptr;
    LLSpatialBridge* bridge = drawable ? drawable->getSpatialBridge() : nullptr;
    if (!bridge)
    {
        return false;
    }

    // Plan view in the root's frame: look down its -Z with its +X right and +Y up.
    const LLQuaternion rot = root->getRenderRotation();
    const LLVector3 origin = root->getRenderPosition();
    const LLVector3 dir = LLVector3(0.f, 0.f, -1.f) * rot;
    const LLVector3 up = LLVector3(0.f, 1.f, 0.f) * rot;
    LLVector4a eye4, dir4, up4;
    eye4.load3(origin.mV);
    dir4.load3(dir.mV);
    up4.load3(up.mV);
    const LLMatrix4a view = LLMatrix4a::lookDir(eye4, dir4, up4);

    // The bridge's agent-space box, turned into the view: loose for a turned root, but always covering.
    const LLVector4a* ext = bridge->getSpatialExtents();
    LLVector3 vmin(F32_MAX, F32_MAX, F32_MAX), vmax(-F32_MAX, -F32_MAX, -F32_MAX);
    for (U32 c = 0; c < 8; ++c)
    {
        LLVector4a p(c & 1 ? ext[1][0] : ext[0][0], c & 2 ? ext[1][1] : ext[0][1], c & 4 ? ext[1][2] : ext[0][2], 1.f);
        LLVector4a q;
        view.affineTransform(p, q);
        for (U32 k = 0; k < 3; ++k)
        {
            vmin.mV[k] = llmin(vmin.mV[k], q[k]);
            vmax.mV[k] = llmax(vmax.mV[k], q[k]);
        }
    }

    GridLimits limits;
    limits.target_cells = llclamp((S32)target_cells, 16, SLOT_SIZE - 2);
    limits.max_cell = llmax((F32)max_cell, limits.min_cell);
    limits.max_cells = SLOT_SIZE - 2;
    const Grid g = gridFor(vmin.mV[0], vmin.mV[1], vmax.mV[0], vmax.mV[1], limits);
    if (!g.valid())
    {
        return false;
    }

    if (e.slot < 0)
    {
        e.slot = mAtlas.acquire();
        if (e.slot < 0)
        {
            return false; // atlas full; it asks again when a slot frees
        }
    }

    const F32 zpad = 1.f;
    const LLMatrix4a proj = al_ortho(g.x0, g.x1, g.y0, g.y1, -vmax.mV[2] - zpad, -vmin.mV[2] + zpad);
    const S32 resx = g.cols * SUPERSAMPLE;
    const S32 resy = g.rows * SUPERSAMPLE;

    // The cull camera: ortho planes for the bridge test, and a pulled-back wide perspective for the
    // bridge-local octree cull, which rebuilds its planes from origin and axes.
    const F32 radius = (vmax - vmin).length() * 0.5f + 1.f;
    const LLVector3 center = origin + dir * ((vmin.mV[2] + vmax.mV[2]) * -0.5f);
    LLCamera camera = *LLViewerCamera::getInstance();
    camera.setView(F_PI_BY_TWO);
    camera.setAspect(1.f);
    camera.setNear(0.1f);
    camera.setFar(radius * 4.f + 1.f);
    camera.setOrigin(center - dir * (radius * 2.f));
    camera.setAxes(dir, up % dir, up);
    camera.setModelview(view);
    camera.setProjection(proj);
    LLViewerCamera::updateFrustumPlanes(camera, false, false, true);

    S32 saved_viewport[4];
    memcpy(saved_viewport, gGLViewport, sizeof(saved_viewport));
    const U32 saved_res_x = LLRenderTarget::sCurResX;
    const U32 saved_res_y = LLRenderTarget::sCurResY;

    // Coverage into the scratch target's corner, cleared there only.
    sTarget.bindTarget();
    gGLViewport[0] = gGLViewport[1] = 0;
    gGLViewport[2] = resx;
    gGLViewport[3] = resy;
    glViewport(0, 0, resx, resy);
    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        glScissor(0, 0, resx, resy);
        sTarget.clear();
    }
    F32 clear_depth = 1.f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
    e.draws = gPipeline.skRenderOutlineCoverage(bridge, view, proj, camera, alpha_cutoff);
    sTarget.flush();

    // Resolve: covered subsamples averaged into the slot, which it writes whole so nothing stale survives.
    S32 sx = 0, sy = 0;
    mAtlas.origin(e.slot, sx, sy);
    sAtlas.bindTarget();
    gGLViewport[0] = sx;
    gGLViewport[1] = sy;
    gGLViewport[2] = SLOT_SIZE;
    gGLViewport[3] = SLOT_SIZE;
    glViewport(sx, sy, SLOT_SIZE, SLOT_SIZE);
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        sResolve.bind();
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, sTarget.getDepth(), gGL.getSampler(ALSamplers::PointClamp));
        const GLint prog = sResolve.mProgramObject;
        glUniform1i(glGetUniformLocation(prog, "depthMap"), 0);
        glUniform4i(glGetUniformLocation(prog, "sk_slot"), sx, sy, g.cols, g.rows);
        glUniform1f(glGetUniformLocation(prog, "sk_clear_depth"), clear_depth);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTextureSlot(0)->unbind();
        sResolve.unbind();
    }
    sAtlas.flush();

    memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;

    e.grid = g;
    e.rendered = true;
    e.lost = false;
    ++e.renders;

    SKOutlineLayer layer;
    layer.pose_source = SKOutlineLayer::PoseSource::ROOT;
    layer.pose_id = e.root_id;
    layer.texture = sAtlas.getTexture();
    layer.u0 = (F32)sx / ATLAS_SIZE;
    layer.v0 = (F32)sy / ATLAS_SIZE;
    layer.u1 = (F32)(sx + g.cols) / ATLAS_SIZE;
    layer.v1 = (F32)(sy + g.rows) / ATLAS_SIZE;
    layer.x0 = g.x0;
    layer.y0 = g.y0;
    layer.x1 = g.x1;
    layer.y1 = g.y1;
    layer.cell = g.cell;
    layer.coarsened = g.coarsened;
    e.glyph.layers.assign(1, layer);
    ++e.glyph.version;
    return true;
}

void SKOutlineCapture::forEachCapture(const std::function<void(const Stats&)>& fn) const
{
    const F64 now = LLTimer::getTotalSeconds();
    for (const auto& [id, entry] : mEntries)
    {
        Stats s;
        s.root = id;
        s.parts = entry->parts;
        s.layers = (S32)entry->glyph.layers.size();
        s.renders = entry->renders;
        s.draws = entry->draws;
        s.last_render = entry->rendered ? now - entry->rendered_at : -1.0;
        s.cpu_ms = entry->cpu_ms;
        s.gpu_ms = entry->gpu_ms;
        s.distance = entry->distance;
        s.cols = entry->grid.cols;
        s.rows = entry->grid.rows;
        s.cell = entry->grid.cell;
        s.settled = entry->glyph.settled;
        s.pinned = entry->pinned;
        s.waiting = entry->loading;
        s.last_reason = reasonName(entry->last_reason);
        fn(s);
    }
}

U32 SKOutlineCapture::atlasTexture() const
{
    return sAtlas.isComplete() ? sAtlas.getTexture() : 0;
}

bool SKOutlineCapture::dumpAtlas(const std::string& path)
{
    if (!sAtlas.isComplete())
    {
        return false;
    }
    LLPointer<LLImageRaw> raw = new LLImageRaw(ATLAS_SIZE, ATLAS_SIZE, 1);
    sAtlas.bindTarget();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ATLAS_SIZE, ATLAS_SIZE, GL_RED, GL_UNSIGNED_BYTE, raw->getData());
    sAtlas.flush();
    LLPointer<LLImagePNG> png = new LLImagePNG();
    return png->encode(raw, 0.f) && png->save(path);
}
