/**
 * @file sknavmarkobjects.cpp
 * @brief The client-only bodies of navmesh annotation marks: prims a click selects and the gizmos drag.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sknavmarkobjects.h"

#include "llagent.h"
#include "lllocalmesh.h"
#include "llprimitive.h"
#include "llquaternion.h"
#include "llselectmgr.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolumemgr.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "m3math.h"
#include "object_flags.h"
#include "pipeline.h"
#include "sknavannotator.h"

#include <cmath>

namespace
{
    constexpr F32 HANDLE_SIZE = 0.12f;      ///< a vertex or axis handle's diameter
    constexpr F32 POINT_SIZE = 0.16f;       ///< a one-point mark's handle
    constexpr F32 EXIT_SIZE = 0.10f;        ///< a way-up handle
    constexpr F32 BEAM_WIDTH = 0.07f;       ///< a straight stretch's cross section
    constexpr F64 MOVED2 = 1e-8;            ///< squared distance a handle may drift before it counts
    constexpr F64 SPIRAL_RADIUS_MIN = 0.5;  ///< a helix tighter than this cannot be walked
    constexpr F64 SPIRAL_RISE_MIN = 0.25;   ///< below this a climb is a step, not a flight

    LLVector3 agentOf(const LLVector3d& global)
    {
        return gAgent.getPosAgentFromGlobal(global);
    }

    LLVector3d globalOf(const LLVector3& agent)
    {
        return gAgent.getPosGlobalFromAgent(agent);
    }

    void paintAll(LLVOVolume* vol, const LLColor4& color)
    {
        if (!vol)
        {
            return;
        }
        const S32 tes = vol->getNumTEs();
        for (S32 te = 0; te < tes; ++te)
        {
            vol->setTEColor((U8)te, color);
            vol->setTEFullbright((U8)te, 1);
        }
    }

    std::string markName(U32 id)
    {
        return llformat("navmesh annotation #%u", id);
    }
}

SKNavMarkObjects::SKNavMarkObjects() = default;

SKNavMarkObjects::~SKNavMarkObjects()
{
    for (auto& entry : mProxies)
    {
        despawnPrims(entry.second);
    }
}

void SKNavMarkObjects::wantFor(const sknav::Mark& mark, size_t& handles, size_t& beams, bool& centre) const
{
    handles = mark.mVerts.size();
    beams = mark.mSpiral ? 0 : (mark.mVerts.size() > 1 ? mark.mVerts.size() - 1 : 0);
    centre = mark.mSpiral && mark.mVerts.size() >= 2;
}

LLViewerObject* SKNavMarkObjects::makeHandle(const sknav::Mark& mark, F32 size)
{
    if (!gAgent.getRegion())
    {
        return nullptr;
    }
    LLViewerObject* obj = gObjectList.createObjectViewer(LL_PCODE_VOLUME, gAgent.getRegion());
    LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
    if (!vol)
    {
        if (obj)
        {
            obj->markDead();
        }
        return nullptr;
    }
    // Client-only, selectable and movable so the tools treat it as an
    // object of the agent's own, phantom so a body across a doorway never
    // blocks anyone.
    vol->mbCanSelect = true;
    vol->mIsLocalOnly = true;
    vol->setFlagsWithoutUpdate(FLAGS_OBJECT_YOU_OWNER | FLAGS_OBJECT_MODIFY | FLAGS_OBJECT_MOVE | FLAGS_OBJECT_COPY
                                   | FLAGS_OBJECT_TRANSFER | FLAGS_PHANTOM,
                               true);
    // Build the drawable and force a valid LOD before setVolume, the way a
    // local mesh preview is rezzed; a NO_LOD drawable would build a
    // placeholder cube instead of the shape asked for.
    gPipeline.createObject(vol);
    vol->setLOD(LLVolumeLODGroup::NUM_LODS - 1);
    LLVolumeParams params;
    params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE);
    vol->setVolume(params, LLVolumeLODGroup::NUM_LODS - 1);
    vol->setScale(LLVector3(size, size, size), false);
    paintAll(vol, skNavMarkColor(mark.mType));
    vol->markForUpdate();
    return vol;
}

LLViewerObject* SKNavMarkObjects::makeBeam(const sknav::Mark& mark)
{
    if (!gAgent.getRegion())
    {
        return nullptr;
    }
    LLViewerObject* obj = gObjectList.createObjectViewer(LL_PCODE_VOLUME, gAgent.getRegion());
    LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
    if (!vol)
    {
        if (obj)
        {
            obj->markDead();
        }
        return nullptr;
    }
    vol->mbCanSelect = true;
    vol->mIsLocalOnly = true;
    vol->setFlagsWithoutUpdate(FLAGS_OBJECT_YOU_OWNER | FLAGS_OBJECT_MODIFY | FLAGS_OBJECT_MOVE | FLAGS_OBJECT_COPY
                                   | FLAGS_OBJECT_TRANSFER | FLAGS_PHANTOM,
                               true);
    gPipeline.createObject(vol);
    vol->setLOD(LLVolumeLODGroup::NUM_LODS - 1);
    LLVolumeParams params;
    params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
    vol->setVolume(params, LLVolumeLODGroup::NUM_LODS - 1);
    vol->setScale(LLVector3(0.02f, BEAM_WIDTH, BEAM_WIDTH), false);
    paintAll(vol, skNavMarkColor(mark.mType));
    vol->markForUpdate();
    return vol;
}

void SKNavMarkObjects::spawn(const sknav::Mark& mark, MarkPrims& prims)
{
    size_t handles = 0, beams = 0;
    bool centre = false;
    wantFor(mark, handles, beams, centre);

    LLViewerObject* root = nullptr;
    auto link = [&root](LLViewerObject* prim) {
        if (!prim)
        {
            return;
        }
        if (root)
        {
            root->addChild(prim);
            // Parent the drawable too: a client-only linkset establishes it
            // itself, as a local mesh preview does.
            prim->setDrawableParent(root->mDrawable);
        }
        else
        {
            root = prim;
        }
    };

    const F32 handle_size = mark.mKind == sknav::MarkKind::POINT ? POINT_SIZE : HANDLE_SIZE;
    for (size_t i = 0; i < handles; ++i)
    {
        LLViewerObject* prim = makeHandle(mark, handle_size);
        prims.mHandles.emplace_back(prim);
        link(prim);
        if (prim)
        {
            Ref ref;
            ref.mMarkId = mark.mId;
            ref.mRole = i == 0 ? ROOT_HANDLE : VERTEX_HANDLE;
            ref.mIndex = (S32)i;
            mByUUID[prim->getID()] = ref;
        }
    }
    for (size_t i = 0; i < mark.mExits.size(); ++i)
    {
        LLViewerObject* prim = makeHandle(mark, EXIT_SIZE);
        prims.mExits.emplace_back(prim);
        link(prim);
        if (prim)
        {
            Ref ref;
            ref.mMarkId = mark.mId;
            ref.mRole = EXIT_HANDLE;
            ref.mIndex = (S32)i;
            mByUUID[prim->getID()] = ref;
        }
    }
    if (centre)
    {
        LLViewerObject* prim = makeHandle(mark, HANDLE_SIZE);
        prims.mCentre = prim;
        link(prim);
        if (prim)
        {
            Ref ref;
            ref.mMarkId = mark.mId;
            ref.mRole = CENTRE_HANDLE;
            mByUUID[prim->getID()] = ref;
        }
    }
    for (size_t i = 0; i < beams; ++i)
    {
        LLViewerObject* prim = makeBeam(mark);
        prims.mBeams.emplace_back(prim);
        link(prim);
        if (prim)
        {
            Ref ref;
            ref.mMarkId = mark.mId;
            ref.mRole = BEAM;
            ref.mIndex = (S32)i;
            mByUUID[prim->getID()] = ref;
        }
    }
}

void SKNavMarkObjects::despawnPrims(MarkPrims& prims)
{
    std::vector<LLPointer<LLViewerObject>> all = prims.mHandles;
    all.insert(all.end(), prims.mExits.begin(), prims.mExits.end());
    all.push_back(prims.mCentre);
    all.insert(all.end(), prims.mBeams.begin(), prims.mBeams.end());
    for (LLPointer<LLViewerObject>& prim : all)
    {
        if (prim.isNull())
        {
            continue;
        }
        // A selected body leaves the selection first: a dead prim in the
        // selection is a ghost the manager keeps until told otherwise.
        if (prim->isSelected() && LLSelectMgr::instanceExists())
        {
            LLSelectMgr::getInstance()->deselectObjectAndFamily(prim);
        }
        mByUUID.erase(prim->getID());
        prim->markDead();
    }
    prims.mHandles.clear();
    prims.mExits.clear();
    prims.mCentre = nullptr;
    prims.mBeams.clear();
}

void SKNavMarkObjects::despawnMark(U32 mark_id)
{
    const auto it = mProxies.find(mark_id);
    if (it != mProxies.end())
    {
        despawnPrims(it->second);
        mProxies.erase(it);
    }
}

void SKNavMarkObjects::despawnAll()
{
    for (auto& entry : mProxies)
    {
        despawnPrims(entry.second);
    }
    mProxies.clear();
    mByUUID.clear();
}

void SKNavMarkObjects::placeBeam(LLViewerObject* beam, LLViewerObject* root, const LLVector3d& a, const LLVector3d& b)
{
    if (!beam || !root)
    {
        return;
    }
    const LLVector3 aa = agentOf(a);
    const LLVector3 bb = agentOf(b);
    LLVector3 dir = bb - aa;
    const F32 len = dir.normalize();
    if (len < 0.02f)
    {
        // A stretch that is barely there: keep the beam findable to select.
        beam->setPositionAgent(aa);
        beam->setScale(LLVector3(0.02f, BEAM_WIDTH, BEAM_WIDTH), false);
        return;
    }
    // A frame with the run along X: across is horizontal where the run is
    // steep, and the third axis completes the right hand.
    const LLVector3 x_axis = dir;
    LLVector3 y_axis = (fabsf(x_axis.mV[VZ]) > 0.9f ? LLVector3::y_axis : LLVector3::z_axis) % x_axis;
    y_axis.normalize();
    const LLVector3 z_axis = x_axis % y_axis;
    LLMatrix3 frame;
    frame.setRows(x_axis, y_axis, z_axis);
    beam->setRotation(~root->getRotation() * LLQuaternion(frame));
    beam->setPositionAgent((aa + bb) * 0.5f);
    beam->setScale(LLVector3(len, BEAM_WIDTH, BEAM_WIDTH), false);
}

void SKNavMarkObjects::reassert(const sknav::Mark& mark, MarkPrims& prims)
{
    const LLColor4 color = skNavMarkColor(mark.mType);
    for (size_t i = 0; i < prims.mHandles.size() && i < mark.mVerts.size(); ++i)
    {
        prims.mHandles[i]->setPositionAgent(agentOf(mark.mVerts[i]));
    }
    if (prims.mCentre.notNull() && mark.mSpiral && mark.mVerts.size() >= 2)
    {
        // The axis stands at the foot's height; its handle's height says
        // nothing of its own.
        LLVector3d centre(mark.mSpiralCentre);
        centre.mdV[VZ] = mark.mVerts[0].mdV[VZ];
        prims.mCentre->setPositionAgent(agentOf(centre));
    }
    for (size_t i = 0; i < prims.mExits.size() && i < mark.mExits.size(); ++i)
    {
        prims.mExits[i]->setPositionAgent(agentOf(mark.mExits[i]));
    }
    if (!mark.mSpiral && !prims.mHandles.empty() && prims.mHandles.front().notNull())
    {
        for (size_t j = 0; j + 1 < mark.mVerts.size() && j < prims.mBeams.size(); ++j)
        {
            placeBeam(prims.mBeams[j], prims.mHandles.front(), mark.mVerts[j], mark.mVerts[j + 1]);
        }
    }
    for (LLPointer<LLViewerObject>& prim : prims.mHandles)
    {
        if (prim.notNull())
        {
            paintAll(static_cast<LLVOVolume*>(prim.get()), color);
        }
    }
    for (LLPointer<LLViewerObject>& prim : prims.mExits)
    {
        if (prim.notNull())
        {
            paintAll(static_cast<LLVOVolume*>(prim.get()), color);
        }
    }
    if (prims.mCentre.notNull())
    {
        paintAll(static_cast<LLVOVolume*>(prims.mCentre.get()), color);
    }
    for (LLPointer<LLViewerObject>& prim : prims.mBeams)
    {
        if (prim.notNull())
        {
            paintAll(static_cast<LLVOVolume*>(prim.get()), color);
        }
    }
}

bool SKNavMarkObjects::derive(sknav::Mark& mark, const MarkPrims& prims)
{
    bool changed = false;
    const LLVector3d old_foot = mark.mVerts.size() > 0 ? mark.mVerts[0] : LLVector3d();
    const LLVector3d old_top = mark.mVerts.size() > 1 ? mark.mVerts[1] : LLVector3d();
    const LLVector3d old_centre = mark.mSpiralCentre;

    // Wherever a handle was dragged, that is where the point stands.
    for (size_t i = 0; i < prims.mHandles.size() && i < mark.mVerts.size(); ++i)
    {
        const LLVector3d at = globalOf(prims.mHandles[i]->getPositionAgent());
        if (dist_vec_squared(at, mark.mVerts[i]) > MOVED2)
        {
            mark.mVerts[i] = at;
            changed = true;
        }
    }
    for (size_t i = 0; i < prims.mExits.size() && i < mark.mExits.size(); ++i)
    {
        const LLVector3d at = globalOf(prims.mExits[i]->getPositionAgent());
        if (dist_vec_squared(at, mark.mExits[i]) > MOVED2)
        {
            mark.mExits[i] = at;
            changed = true;
        }
    }

    if (!mark.mSpiral || mark.mVerts.size() < 2 || prims.mCentre.isNull())
    {
        return changed;
    }

    // A spiral's handles mean more than their point: the axis stands at the
    // foot's height, the foot sets the radius, the top re-aims the wind.
    const LLVector3d centre_at(globalOf(prims.mCentre->getPositionAgent()));
    const LLVector3d centre(centre_at.mdV[VX], centre_at.mdV[VY], mark.mVerts[0].mdV[VZ]);
    if (dist_vec_squared(centre, old_centre) > MOVED2)
    {
        // The axis moves and the wind goes with it: foot and top keep their
        // angles about it and the radius with them.
        const F64 theta0 = std::atan2(old_foot.mdV[VY] - old_centre.mdV[VY], old_foot.mdV[VX] - old_centre.mdV[VX]);
        mark.mSpiralCentre = centre;
        mark.mVerts[0].set(centre.mdV[VX] + mark.mSpiralRadius * std::cos(theta0),
                           centre.mdV[VY] + mark.mSpiralRadius * std::sin(theta0),
                           mark.mVerts[0].mdV[VZ]);
        const LLVector3d wound = mark.helixAt(1.0);
        mark.mVerts[1].set(wound.mdV[VX], wound.mdV[VY], mark.mVerts[1].mdV[VZ]);
        changed = true;
    }
    else if (dist_vec_squared(mark.mVerts[0], old_foot) > MOVED2)
    {
        // The foot was dragged: the radius is what the drag says, and the
        // top winds to stay on the helix at the height it already had.
        const LLVector3d about(mark.mVerts[0] - mark.mSpiralCentre);
        mark.mSpiralRadius = std::max(SPIRAL_RADIUS_MIN, sqrt(about.mdV[VX] * about.mdV[VX] + about.mdV[VY] * about.mdV[VY]));
        const LLVector3d wound = mark.helixAt(1.0);
        mark.mVerts[1].set(wound.mdV[VX], wound.mdV[VY], mark.mVerts[1].mdV[VZ]);
        changed = true;
    }
    else if (dist_vec_squared(mark.mVerts[1], old_top) > MOVED2)
    {
        // The top was dragged: the swing re-aims the whole helix, the
        // height sets the climb, and both ends snap onto the circle.
        const F64 theta_top = std::atan2(mark.mVerts[1].mdV[VY] - mark.mSpiralCentre.mdV[VY],
                                         mark.mVerts[1].mdV[VX] - mark.mSpiralCentre.mdV[VX]);
        const F64 theta_foot = theta_top - mark.mSpiralHand * mark.mSpiralTurns * 2.0 * F_PI;
        mark.mVerts[0].set(mark.mSpiralCentre.mdV[VX] + mark.mSpiralRadius * std::cos(theta_foot),
                           mark.mSpiralCentre.mdV[VY] + mark.mSpiralRadius * std::sin(theta_foot),
                           mark.mVerts[0].mdV[VZ]);
        const F64 rise = std::max(SPIRAL_RISE_MIN, mark.mVerts[1].mdV[VZ] - mark.mVerts[0].mdV[VZ]);
        mark.mVerts[1].set(mark.mSpiralCentre.mdV[VX] + mark.mSpiralRadius * std::cos(theta_top),
                           mark.mSpiralCentre.mdV[VY] + mark.mSpiralRadius * std::sin(theta_top),
                           mark.mVerts[0].mdV[VZ] + rise);
        changed = true;
    }
    return changed;
}

void SKNavMarkObjects::sync(SKNavAnnotator& annotator)
{
    // A crossing or a teleport re-rezzes every body: their positions live
    // in the region they were rezzed in, and the agent's has moved under
    // them, so nothing a stale prim says about a mark can be trusted.
    LLViewerRegion* region = gAgent.getRegion();
    if (region != mRegion)
    {
        mRegion = region;
        for (auto& entry : mProxies)
        {
            const sknav::Mark* mark = annotator.find(entry.first);
            if (!mark)
            {
                continue;
            }
            despawnPrims(entry.second);
            if (mRegion)
            {
                spawn(*mark, entry.second);
                reassert(*mark, entry.second);
            }
        }
        mSeenRevision = annotator.revision();
        return;
    }

    const bool revision_changed = annotator.revision() != mSeenRevision;
    bool derived = false;
    for (const sknav::Mark& mark : annotator.marks().marks())
    {
        MarkPrims& prims = mProxies[mark.mId];
        size_t handles = 0, beams = 0;
        bool centre = false;
        wantFor(mark, handles, beams, centre);
        const bool stale = prims.mHandles.empty() || prims.mHandles.front().isNull() || prims.mHandles.front()->isDead()
                           || prims.mHandles.size() != handles || prims.mBeams.size() != beams
                           || (prims.mCentre.notNull() != centre);
        if (stale)
        {
            despawnPrims(prims);
            spawn(mark, prims);
            reassert(mark, prims);
            continue;
        }
        sknav::Mark* mutable_mark = annotator.find(mark.mId);
        derived = derive(*mutable_mark, prims) || derived;
        if (revision_changed)
        {
            reassert(mark, prims);
        }
    }
    // A mark removed since the last frame takes its body with it.
    for (auto it = mProxies.begin(); it != mProxies.end();)
    {
        if (!annotator.find(it->first))
        {
            despawnPrims(it->second);
            it = mProxies.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (derived || revision_changed)
    {
        if (derived)
        {
            annotator.touch();
        }
        mSeenRevision = annotator.revision();
    }
    // A note or a type changed on the marks: the selected bodies' picker
    // fields follow, since no sim reply ever refreshes them here.
    if (revision_changed && LLSelectMgr::instanceExists())
    {
        for (LLObjectSelection::iterator iter = LLSelectMgr::getInstance()->getSelection()->begin();
             iter != LLSelectMgr::getInstance()->getSelection()->end(); ++iter)
        {
            LLSelectNode* node = *iter;
            LLViewerObject* object = node ? node->getObject() : nullptr;
            if (!object || !isMarkProxy(object))
            {
                continue;
            }
            std::string name, description;
            if (proxyDisplay(object, name, description))
            {
                node->mName = name;
                node->mDescription = description;
            }
        }
    }
}

bool SKNavMarkObjects::isMarkProxy(const LLViewerObject* object) const
{
    return object && mByUUID.count(object->getID()) > 0;
}

U32 SKNavMarkObjects::markIdOf(const LLViewerObject* object) const
{
    const auto it = object ? mByUUID.find(object->getID()) : mByUUID.end();
    return it != mByUUID.end() ? it->second.mMarkId : 0;
}

bool SKNavMarkObjects::isVertexHandle(const LLViewerObject* object) const
{
    const auto it = object ? mByUUID.find(object->getID()) : mByUUID.end();
    return it != mByUUID.end() && it->second.mRole != BEAM;
}

LLViewerObject* SKNavMarkObjects::rootProxy(U32 mark_id) const
{
    const auto it = mProxies.find(mark_id);
    return it != mProxies.end() && !it->second.mHandles.empty() && it->second.mHandles.front().notNull()
               ? it->second.mHandles.front()
               : nullptr;
}

bool SKNavMarkObjects::proxyDisplay(const LLViewerObject* object, std::string& name, std::string& description) const
{
    const auto it = object ? mByUUID.find(object->getID()) : mByUUID.end();
    if (it == mByUUID.end())
    {
        return false;
    }
    const SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const sknav::Mark* mark = annotator.marks().find(it->second.mMarkId);
    if (!mark)
    {
        return false;
    }
    switch (it->second.mRole)
    {
        case ROOT_HANDLE:
            name = markName(mark->mId);
            break;
        case VERTEX_HANDLE:
            name = llformat("%s, vertex %u", markName(mark->mId).c_str(), (U32)it->second.mIndex + 1);
            break;
        case EXIT_HANDLE:
            name = llformat("%s, exit %u", markName(mark->mId).c_str(), (U32)it->second.mIndex + 1);
            break;
        case CENTRE_HANDLE:
            name = llformat("%s, spiral axis", markName(mark->mId).c_str());
            break;
        case BEAM:
            name = llformat("%s, segment %u", markName(mark->mId).c_str(), (U32)it->second.mIndex + 1);
            break;
    }
    description = mark->mText;
    return true;
}