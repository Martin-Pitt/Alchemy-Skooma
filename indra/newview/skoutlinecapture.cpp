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
#include "llface.h"
#include "llgl.h"
#include "llgltfmaterial.h"
#include "llglslshader.h"
#include "llimagepng.h"
#include "llmaterial.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llselectmgr.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
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
    constexpr S32 AVATAR_CENTER_POINT = 40;
    constexpr F64 NAME_RETRY_SECONDS = 30.0; // a name that never came is asked for again after this

    LLRenderTarget sTarget; // depth (and a scratch colour) at twice a slot's resolution
    LLRenderTarget sAtlas;  // R8 coverage, one slot per layer
    LLGLSLShader sResolve;
    U32 sQueries[2] = { 0, 0 };
    LLUUID sTimedRoot; // the capture whose render the queries are timing
    View sTimedView = View::PLAN;

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

    // A frame in agent space: a point p in it is at p * rot + pos.
    struct Pose
    {
        LLVector3 pos;
        LLQuaternion rot;
    };

    Pose poseOf(const LLViewerObject* obj)
    {
        return { obj->getRenderPosition(), obj->getRenderRotation() };
    }

    LLVector3 intoFrame(const LLVector3& agent, const Pose& frame)
    {
        return (agent - frame.pos) * ~frame.rot;
    }

    void grow(Box& box, const LLVector3& p)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            box.min[k] = llmin(box.min[k], p.mV[k]);
            box.max[k] = llmax(box.max[k], p.mV[k]);
        }
    }

    Box emptyBox()
    {
        Box b;
        for (S32 k = 0; k < 3; ++k)
        {
            b.min[k] = F32_MAX;
            b.max[k] = -F32_MAX;
        }
        return b;
    }

    bool isEmpty(const Box& b)
    {
        return b.min[0] > b.max[0];
    }

    // Every face that shows is alpha blended or masked, and at least one shows.
    bool alphaFacesOnly(LLViewerObject* part, F32 alpha_cutoff)
    {
        LLDrawable* drawable = part->mDrawable;
        bool shows = false;
        for (U8 t = 0; t < part->getNumTEs(); ++t)
        {
            const LLTextureEntry* te = part->getTE(t);
            const F32 alpha = te ? te->getColor().mV[VALPHA] : 0.f;
            if (!te || alpha <= alpha_cutoff)
            {
                continue;
            }
            shows = true;
            bool blended = alpha < 0.999f;
            const LLFace* face = drawable && t < drawable->getNumFaces() ? drawable->getFace(t) : nullptr;
            blended = blended || (face && face->isInAlphaPool());
            if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
            {
                blended = blended || gltf->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE;
            }
            if (const LLMaterial* mat = te->getMaterialParams().get())
            {
                const U8 mode = mat->getDiffuseAlphaMode();
                blended = blended || mode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND || mode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK;
            }
            if (!blended)
            {
                return false;
            }
        }
        return shows;
    }

    // A pilot aid drawn on a near-zero-thickness plane: left out of the glyph and of its box.
    bool leftOut(LLViewerObject* part, F32 alpha_cutoff)
    {
        const LLVector3 s = part->getScale();
        return isThinPlane(s.mV[0], s.mV[1], s.mV[2], alphaFacesOnly(part, alpha_cutoff));
    }

    // The linkset's bounds in a frame, from each part's own box. Parts whose shape is not their own box (rigged, flexible)
    // take the bridge's agent-space box instead: loose, but covering.
    Box linksetBox(const std::vector<LLViewerObject*>& parts, const Pose& frame, F32 alpha_cutoff)
    {
        Box box = emptyBox();
        bool loose = false;
        for (LLViewerObject* part : parts)
        {
            LLVOVolume* vol = dynamic_cast<LLVOVolume*>(part);
            const bool own_shape = vol && !vol->isRiggedMesh() && !vol->isFlexible() && part->mDrawable &&
                                   !part->mDrawable->isState(LLDrawable::RIGGED);
            if (!own_shape)
            {
                loose = loose || part->mDrawable.notNull();
                continue;
            }
            if (leftOut(part, alpha_cutoff))
            {
                continue;
            }
            LLVector3 lo(-0.5f, -0.5f, -0.5f), hi(0.5f, 0.5f, 0.5f);
            const LLVolume* volume = vol->getVolume();
            if (volume && volume->getNumVolumeFaces() > 0)
            {
                lo.setVec(F32_MAX, F32_MAX, F32_MAX);
                hi.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
                for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
                {
                    const LLVector4a* ext = volume->getVolumeFace(f).mExtents;
                    for (S32 k = 0; k < 3; ++k)
                    {
                        lo.mV[k] = llmin(lo.mV[k], ext[0][k]);
                        hi.mV[k] = llmax(hi.mV[k], ext[1][k]);
                    }
                }
            }
            const LLVector3 scale = part->getScale();
            const Pose pose = poseOf(part);
            for (U32 c = 0; c < 8; ++c)
            {
                const LLVector3 corner((c & 1 ? hi : lo).mV[0] * scale.mV[0], (c & 2 ? hi : lo).mV[1] * scale.mV[1],
                                       (c & 4 ? hi : lo).mV[2] * scale.mV[2]);
                grow(box, intoFrame(corner * pose.rot + pose.pos, frame));
            }
        }
        LLSpatialBridge* bridge = parts.empty() || !parts[0]->mDrawable ? nullptr : parts[0]->mDrawable->getSpatialBridge();
        if (loose && bridge)
        {
            const LLVector4a* ext = bridge->getSpatialExtents();
            for (U32 c = 0; c < 8; ++c)
            {
                const LLVector3 p(c & 1 ? ext[1][0] : ext[0][0], c & 2 ? ext[1][1] : ext[0][1], c & 4 ? ext[1][2] : ext[0][2]);
                grow(box, intoFrame(p, frame));
            }
        }
        return box;
    }

    // What a linkset looks like for change tracking: its parts, their poses in the linkset, scale, faces and LOD.
    U64 hashLinkset(const std::vector<LLViewerObject*>& parts, bool& top, bool& loading)
    {
        U64 h = mix(0, parts.size());
        for (LLViewerObject* part : parts)
        {
            h = mix(h, std::hash<LLUUID>{}(part->getID()));
            if (part != parts.front())
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
        return h;
    }

    // The index ranges of the faces a glyph leaves out, matched against draw infos by buffer and range.
    struct SkipRange
    {
        const LLVertexBuffer* buffer = nullptr;
        U32 start = 0;
        U32 end = 0;
    };

    void gatherSkips(const std::vector<LLViewerObject*>& parts, F32 alpha_cutoff, std::vector<SkipRange>& out)
    {
        for (LLViewerObject* part : parts)
        {
            LLDrawable* drawable = part->mDrawable;
            if (!drawable || !leftOut(part, alpha_cutoff))
            {
                continue;
            }
            for (S32 f = 0; f < drawable->getNumFaces(); ++f)
            {
                const LLFace* face = drawable->getFace(f);
                if (face && face->getVertexBuffer() && face->getIndicesCount() > 0)
                {
                    const U32 start = (U32)face->getIndicesStart();
                    out.push_back({ face->getVertexBuffer(), start, start + face->getIndicesCount() });
                }
            }
        }
    }

    // A rectangle of a layer's plan in the vehicle root's frame, from its own rect and pose.
    Rect planRect(const SKOutlineLayer& l)
    {
        Rect r { F32_MAX, F32_MAX, -F32_MAX, -F32_MAX };
        for (U32 c = 0; c < 4; ++c)
        {
            const LLVector3 p = LLVector3(c & 1 ? l.x1 : l.x0, c & 2 ? l.y1 : l.y0, 0.f) * l.rel_rot + l.rel_pos;
            r.x0 = llmin(r.x0, p.mV[0]);
            r.y0 = llmin(r.y0, p.mV[1]);
            r.x1 = llmax(r.x1, p.mV[0]);
            r.y1 = llmax(r.y1, p.mV[1]);
        }
        return r;
    }

    // Covered area in square centimetres, so layers with different cells compare.
    S32 coveredArea(const SKOutlineLayer& l)
    {
        return (S32)llmin(std::lround((F64)l.covered_cells * l.cell * l.cell * 1.0e4), (long)S32_MAX);
    }
}

// One layer's render state: its slot, grid and the count of its covered cells.
struct SKOutlineLayerState
{
    LLUUID id;           // the root of the layer's linkset
    S32 slot = -1;
    Grid grid;
    U32 query = 0;
    bool counting = false; // a count is in flight
    bool rendered = false;
    SKOutlineLayer out;
};

// A crew attachment that belongs to the vehicle.
struct SKOutlineCrewRef
{
    LLUUID id;
    LLUUID wearer;
    S32 point = 0;

    bool operator==(const SKOutlineCrewRef& o) const { return id == o.id && wearer == o.wearer && point == o.point; }
};

struct SKOutlineCapture::Entry
{
    LLUUID root_id;
    View view = View::PLAN;
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

    // Crew attachments that belong to the vehicle, found at each check.
    std::vector<SKOutlineCrewRef> crew;

    // The last render: the base layer first, then the crew layers.
    bool rendered = false;
    bool lost = false;
    std::vector<SKOutlineLayerState> layers;
    Grid grid;              // the base layer's
    F64 rendered_at = 0.0;
    U32 renders = 0;
    U32 draws = 0;
    F64 cpu_ms = 0.0;
    F64 gpu_ms = -1.0;
    Reason last_reason = Reason::NONE;

    SKOutlineGlyph glyph;

    bool hasBase() const { return !layers.empty() && layers[0].rendered && layers[0].slot >= 0; }

    void release(SlotAtlas& atlas, bool gl)
    {
        for (SKOutlineLayerState& l : layers)
        {
            atlas.release(l.slot);
            l.slot = -1;
            if (l.query && gl)
            {
                glDeleteQueries(1, &l.query);
            }
            l.query = 0;
            l.counting = false;
        }
    }
};

namespace
{
    // Shows the base layer and every counted crew layer that is neither a speck nor inside a larger layer.
    bool publish(SKOutlineCapture::Entry& e)
    {
        std::vector<SKOutlineLayer> shown;
        if (e.hasBase())
        {
            shown.push_back(e.layers[0].out);
        }
        for (size_t i = 1; i < e.layers.size(); ++i)
        {
            const SKOutlineLayer& l = e.layers[i].out;
            if (!e.layers[i].rendered || e.layers[i].slot < 0 || l.covered_cells < 0 || isSpeck(l.covered_cells))
            {
                continue;
            }
            bool merged = false;
            for (size_t j = 0; j < e.layers.size() && !merged; ++j)
            {
                const SKOutlineLayer& o = e.layers[j].out;
                merged = j != i && e.layers[j].rendered && o.covered_cells >= 0 &&
                         mergedInto(planRect(l), coveredArea(l), planRect(o), coveredArea(o));
            }
            if (!merged)
            {
                shown.push_back(l);
            }
        }
        bool same = shown.size() == e.glyph.layers.size();
        for (size_t i = 0; same && i < shown.size(); ++i)
        {
            same = shown[i].pose_id == e.glyph.layers[i].pose_id && shown[i].u0 == e.glyph.layers[i].u0 &&
                   shown[i].v0 == e.glyph.layers[i].v0 && shown[i].x0 == e.glyph.layers[i].x0 &&
                   shown[i].x1 == e.glyph.layers[i].x1 && shown[i].covered_cells == e.glyph.layers[i].covered_cells;
        }
        e.glyph.layers.swap(shown);
        return !same;
    }
}

SKOutlineCapture::SKOutlineCapture()
    : mAtlas(ATLAS_SIZE, SLOT_SIZE)
{
}

SKOutlineCapture::~SKOutlineCapture() = default;

const SKOutlineGlyph* SKOutlineCapture::glyphFor(const LLUUID& root, View view)
{
    SKOutlineCapture& self = instance();
    self.request(root, false, view);
    const auto it = self.mEntries.find({ root, view });
    if (it == self.mEntries.end() || !it->second->rendered || it->second->lost || !it->second->hasBase())
    {
        return nullptr;
    }
    return &it->second->glyph;
}

void SKOutlineCapture::request(const LLUUID& root, bool pinned, View view)
{
    if (root.isNull())
    {
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    std::unique_ptr<Entry>& entry = mEntries[{ root, view }];
    if (!entry)
    {
        entry = std::make_unique<Entry>();
        entry->root_id = root;
        entry->view = view;
        entry->first_asked = now;
        entry->glyph.root = root;
        entry->glyph.view = view;
    }
    entry->wanted = now;
    entry->pinned = entry->pinned || pinned;
}

void SKOutlineCapture::forget(const LLUUID& root)
{
    for (auto it = mEntries.begin(); it != mEntries.end();)
    {
        if (it->first.root == root)
        {
            it->second->release(mAtlas, true);
            it = mEntries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SKOutlineCapture::forgetAll()
{
    for (auto& [id, entry] : mEntries)
    {
        entry->release(mAtlas, true);
    }
    mEntries.clear();
    mAtlas.clear();
    mNames.clear();
    mNameRequests.clear();
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

void SKOutlineCapture::onObjectName(const LLUUID& id, const std::string& name)
{
    if (!instanceExists())
    {
        return;
    }
    SKOutlineCapture& self = instance();
    if (self.mNameRequests.count(id))
    {
        self.mNames[id] = name;
    }
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
    mGLFailed = false;
    for (auto& [id, entry] : mEntries)
    {
        entry->release(mAtlas, true);
        entry->lost = true;
    }
    mAtlas.clear();
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

void SKOutlineCapture::readCounts()
{
    for (auto& [id, entry] : mEntries)
    {
        bool counted = false;
        for (SKOutlineLayerState& l : entry->layers)
        {
            if (!l.counting || !l.query)
            {
                continue;
            }
            GLint available = 0;
            glGetQueryObjectiv(l.query, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available)
            {
                GLuint samples = 0;
                glGetQueryObjectuiv(l.query, GL_QUERY_RESULT, &samples);
                l.out.covered_cells = (S32)samples;
                l.counting = false;
                counted = true;
            }
        }
        if (counted && entry->rendered && publish(*entry))
        {
            ++entry->glyph.version;
        }
    }
}

void SKOutlineCapture::updateCrew(Entry& e, LLViewerObject* root, F64 now)
{
    static LLCachedControl<F32> alpha_cutoff(gSavedSettings, "SKOutlineCaptureAlphaCutoff", 0.02f);

    // Everyone seated on the root or one of its prims.
    std::vector<LLVOAvatar*> crew;
    std::vector<LLViewerObject*> prims { root };
    for (LLViewerObject* child : root->getChildren())
    {
        if (child && !child->isAvatar())
        {
            prims.push_back(child);
        }
    }
    for (LLViewerObject* prim : prims)
    {
        for (LLViewerObject* child : prim->getChildren())
        {
            if (child && child->isAvatar() && !child->isDead())
            {
                crew.push_back(static_cast<LLVOAvatar*>(child));
            }
        }
    }

    std::vector<SKOutlineCrewRef> found;
    if (!crew.empty())
    {
        const Pose frame = poseOf(root);
        std::vector<LLViewerObject*> root_parts;
        gatherLinkset(root, root_parts);
        const Box root_box = linksetBox(root_parts, frame, alpha_cutoff);
        const auto root_name = mNames.find(e.root_id);

        auto ask = [this, now](LLViewerObject* obj)
        {
            const auto it = mNameRequests.find(obj->getID());
            if ((it == mNameRequests.end() || (!mNames.count(obj->getID()) && now - it->second > NAME_RETRY_SECONDS)) &&
                obj->getRegion())
            {
                mNameRequests[obj->getID()] = now;
                LLSelectMgr::getInstance()->requestObjectPropertiesFamily(obj);
            }
        };

        for (LLVOAvatar* avatar : crew)
        {
            const LLVector3 wearer = intoFrame(avatar->getRenderPosition(), frame);
            for (const auto& [point_id, point] : avatar->mAttachmentPoints)
            {
                // A HUD is drawn on the screen, not the avatar.
                if (!point || point->getIsHUDAttachment())
                {
                    continue;
                }
                for (const LLPointer<LLViewerObject>& attached : point->mAttachedObjects)
                {
                    if (attached.isNull() || attached->isDead() || !attached->mDrawable)
                    {
                        continue;
                    }
                    std::vector<LLViewerObject*> parts;
                    gatherLinkset(attached.get(), parts);
                    CrewPiece piece;
                    piece.box = linksetBox(parts, frame, alpha_cutoff);
                    if (isEmpty(piece.box))
                    {
                        continue;
                    }
                    piece.root = isEmpty(root_box) ? piece.box : root_box;
                    piece.wearer[0] = wearer.mV[0];
                    piece.wearer[1] = wearer.mV[1];
                    piece.wearer[2] = wearer.mV[2];
                    piece.avatar_center = point_id == AVATAR_CENTER_POINT;
                    piece.temp = attached->isTempAttachment();
                    const auto name = mNames.find(attached->getID());
                    piece.name_affinity = root_name != mNames.end() && name != mNames.end() &&
                                          nameAffinity(root_name->second, name->second);

                    const CrewVerdict verdict = crewVerdict(piece);
                    if (verdict == CrewVerdict::LAYER)
                    {
                        found.push_back({ attached->getID(), avatar->getID(), point_id });
                    }
                    // Names arrive later and only tip pieces that reach past their wearer.
                    if (verdict != CrewVerdict::PERSONAL)
                    {
                        ask(root);
                        ask(attached.get());
                    }
                }
            }
        }
    }
    e.crew.swap(found);
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
            const auto it = mEntries.find({ sTimedRoot, sTimedView });
            if (it != mEntries.end())
            {
                it->second->gpu_ms = t1 > t0 ? (F64)(t1 - t0) / 1.0e6 : 0.0;
            }
            sTimedRoot.setNull();
        }
    }
    readCounts();

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
            e.release(mAtlas, true);
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
            bool top = true;
            bool loading = false;
            U64 h = hashLinkset(parts, top, loading);
            e.parts = (S32)parts.size();

            // Crew layers: which pieces belong, and their own shapes, not their poses, which are live.
            if (e.view == View::PLAN)
            {
                updateCrew(e, root, now);
            }
            for (const SKOutlineCrewRef& c : e.crew)
            {
                LLViewerObject* worn = gObjectList.findObject(c.id);
                if (!worn)
                {
                    continue;
                }
                std::vector<LLViewerObject*> worn_parts;
                gatherLinkset(worn, worn_parts);
                h = mix(h, hashLinkset(worn_parts, top, loading));
                h = mix(h, (U64)c.point);
                e.parts += (S32)worn_parts.size();
            }
            if (h != e.signature)
            {
                e.signature = h;
                e.last_change = now;
            }
            e.top_lod = top;
            e.loading = loading;
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
        const bool no_room = !e.hasBase() && mAtlas.used() >= mAtlas.capacity();
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
                sTimedView = e.view;
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

namespace
{
    // Renders one linkset's coverage in a view of its root's own frame, into its slot, and starts counting its cells.
    // Returns the draws made, or -1 when there was nothing to render.
    S32 renderLayer(SKOutlineLayerState& l, LLViewerObject* layer_root, const Pose& vehicle, SlotAtlas& atlas, View look)
    {
        static LLCachedControl<S32> target_cells(gSavedSettings, "SKOutlineCaptureCells", 160);
        static LLCachedControl<F32> max_cell(gSavedSettings, "SKOutlineCaptureMaxCellMetres", 0.25f);
        static LLCachedControl<F32> alpha_cutoff(gSavedSettings, "SKOutlineCaptureAlphaCutoff", 0.02f);

        LLDrawable* drawable = layer_root ? layer_root->mDrawable.get() : nullptr;
        LLSpatialBridge* bridge = drawable ? drawable->getSpatialBridge() : nullptr;
        if (!bridge)
        {
            return -1;
        }

        // Look along the view's depth axis of the layer root's frame with its horizontal axis right and vertical axis up
        // (plan: down -Z, +X right, +Y up). View space is then the root's frame with its axes renamed, so the linkset's
        // box in that frame is the view box.
        const Pose frame = poseOf(layer_root);
        const ViewAxes axes = viewAxes(look);
        LLVector3 dir_local, up_local;
        dir_local.mV[axes.d] = axes.depth_sign;
        up_local.mV[axes.v] = 1.f;
        const LLVector3 dir = dir_local * frame.rot;
        const LLVector3 up = up_local * frame.rot;
        LLVector4a eye4, dir4, up4;
        eye4.load3(frame.pos.mV);
        dir4.load3(dir.mV);
        up4.load3(up.mV);
        const LLMatrix4a view = LLMatrix4a::lookDir(eye4, dir4, up4);

        std::vector<LLViewerObject*> parts;
        gatherLinkset(layer_root, parts);
        const Box box = linksetBox(parts, frame, alpha_cutoff);
        if (isEmpty(box))
        {
            return -1;
        }
        const LLVector3 vmin(box.min[0], box.min[1], box.min[2]);
        const LLVector3 vmax(box.max[0], box.max[1], box.max[2]);

        GridLimits limits;
        limits.target_cells = llclamp((S32)target_cells, 16, SLOT_SIZE - 2);
        limits.max_cell = llmax((F32)max_cell, limits.min_cell);
        limits.max_cells = SLOT_SIZE - 2;
        const Grid g = gridFor(vmin.mV[axes.h], vmin.mV[axes.v], vmax.mV[axes.h], vmax.mV[axes.v], limits);
        if (!g.valid())
        {
            return -1;
        }
        if (l.slot < 0)
        {
            l.slot = atlas.acquire();
            if (l.slot < 0)
            {
                return -1; // atlas full; it asks again when a slot frees
            }
        }

        std::vector<SkipRange> skips;
        gatherSkips(parts, alpha_cutoff, skips);
        std::function<bool(const LLDrawInfo*)> skip;
        if (!skips.empty())
        {
            // A draw info batching a left-out face with others is left out whole.
            skip = [skips](const LLDrawInfo* di)
            {
                for (const SkipRange& r : skips)
                {
                    if (di->mVertexBuffer.get() == r.buffer && di->mOffset < r.end && r.start < di->mOffset + di->mCount)
                    {
                        return true;
                    }
                }
                return false;
            };
        }

        const F32 zpad = 1.f;
        // Distances along the view of the box's near and far faces.
        const F32 near_d = axes.depth_sign > 0.f ? vmin.mV[axes.d] : -vmax.mV[axes.d];
        const F32 far_d = axes.depth_sign > 0.f ? vmax.mV[axes.d] : -vmin.mV[axes.d];
        const LLMatrix4a proj = al_ortho(g.x0, g.x1, g.y0, g.y1, near_d - zpad, far_d + zpad);
        const S32 resx = g.cols * SUPERSAMPLE;
        const S32 resy = g.rows * SUPERSAMPLE;

        // The cull camera: ortho planes for the bridge test, and a pulled-back wide perspective for the
        // bridge-local octree cull, which rebuilds its planes from origin and axes.
        const F32 radius = (vmax - vmin).length() * 0.5f + 1.f;
        const LLVector3 center = ((vmin + vmax) * 0.5f) * frame.rot + frame.pos;
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
        const U32 draws = gPipeline.skRenderOutlineCoverage(bridge, view, proj, camera, alpha_cutoff, skip);
        sTarget.flush();

        // Resolve: covered subsamples averaged into the slot, which it writes whole so nothing stale survives. A second
        // pass with colour off counts the covered cells.
        S32 sx = 0, sy = 0;
        atlas.origin(l.slot, sx, sy);
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
            const GLint count_loc = glGetUniformLocation(prog, "sk_count");
            glUniform1i(count_loc, 0);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            if (!l.counting)
            {
                if (!l.query)
                {
                    glGenQueries(1, &l.query);
                }
                glUniform1i(count_loc, 1);
                gGL.setColorMask(false, false);
                glBeginQuery(GL_SAMPLES_PASSED, l.query);
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                glEndQuery(GL_SAMPLES_PASSED);
                gGL.setColorMask(true, true);
                l.counting = true;
            }
            gGL.getTextureSlot(0)->unbind();
            sResolve.unbind();
        }
        sAtlas.flush();

        memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        LLRenderTarget::sCurResX = saved_res_x;
        LLRenderTarget::sCurResY = saved_res_y;

        l.grid = g;
        l.rendered = true;
        SKOutlineLayer& out = l.out;
        out.pose_id = l.id;
        out.texture = sAtlas.getTexture();
        out.u0 = (F32)sx / ATLAS_SIZE;
        out.v0 = (F32)sy / ATLAS_SIZE;
        out.u1 = (F32)(sx + g.cols) / ATLAS_SIZE;
        out.v1 = (F32)(sy + g.rows) / ATLAS_SIZE;
        out.x0 = g.x0;
        out.y0 = g.y0;
        out.x1 = g.x1;
        out.y1 = g.y1;
        out.cell = g.cell;
        out.coarsened = g.coarsened;
        out.rel_rot = frame.rot * ~vehicle.rot;
        out.rel_pos = (frame.pos - vehicle.pos) * ~vehicle.rot;
        return (S32)draws;
    }
}

bool SKOutlineCapture::render(Entry& e)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("sk outline capture");

    // Whatever happens, this signature is dealt with: a failed render does not retry every frame.
    e.rendered_signature = e.signature;
    e.rendered_at = LLTimer::getTotalSeconds();

    LLViewerObject* root = gObjectList.findObject(e.root_id);
    if (!root || !root->mDrawable || !root->mDrawable->getSpatialBridge())
    {
        return false;
    }

    // Match the layer states to the base and the crew found, keeping the slots of layers that stay.
    std::vector<SKOutlineLayerState> layers;
    auto take = [&](const LLUUID& id) -> SKOutlineLayerState
    {
        for (SKOutlineLayerState& l : e.layers)
        {
            if (l.id == id && (l.slot >= 0 || l.query))
            {
                SKOutlineLayerState kept = l;
                l.slot = -1;
                l.query = 0;
                l.counting = false;
                return kept;
            }
        }
        SKOutlineLayerState fresh;
        fresh.id = id;
        return fresh;
    };
    layers.push_back(take(e.root_id));
    layers[0].out.pose_source = SKOutlineLayer::PoseSource::ROOT;
    for (const SKOutlineCrewRef& c : e.crew)
    {
        layers.push_back(take(c.id));
        SKOutlineLayer& out = layers.back().out;
        out.pose_source = SKOutlineLayer::PoseSource::CREW_ATTACHMENT;
        out.attachment_point = c.point;
        out.wearer = c.wearer;
    }
    e.release(mAtlas, true);
    e.layers.swap(layers);

    const Pose vehicle = poseOf(root);
    e.draws = 0;
    for (size_t i = 0; i < e.layers.size(); ++i)
    {
        SKOutlineLayerState& l = e.layers[i];
        LLViewerObject* layer_root = i == 0 ? root : gObjectList.findObject(l.id);
        const S32 draws = renderLayer(l, layer_root, vehicle, mAtlas, e.view);
        if (draws < 0)
        {
            l.rendered = false;
            mAtlas.release(l.slot);
            l.slot = -1;
            if (i == 0)
            {
                return false;
            }
            continue;
        }
        e.draws += (U32)draws;
    }

    e.grid = e.layers[0].grid;
    e.rendered = true;
    e.lost = false;
    ++e.renders;
    publish(e);
    ++e.glyph.version;
    return true;
}

void SKOutlineCapture::forEachCapture(const std::function<void(const Stats&)>& fn) const
{
    const F64 now = LLTimer::getTotalSeconds();
    for (const auto& [id, entry] : mEntries)
    {
        Stats s;
        s.root = id.root;
        s.view = id.view;
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
