/**
 * @file skoverviewmovers.cpp
 * @brief Moving objects and vehicles for the tactical overview, at their live pose.
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

#include "skoverviewmovers.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"
#include "skmappolicy.h"
#include "skphysicsshape.h"
#include "skoverviewcensus.h"
#include "skworldcensus.h"

#include <set>

namespace
{
    // A root whose linkset someone sits on.
    bool crewed(const LLViewerObject* root, bool& ally_aboard)
    {
        bool any = false;
        ally_aboard = false;
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            if (!child->isAvatar())
            {
                continue;
            }
            any = true;
            for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
            {
                if (p.mId == child->getID() && p.mAlly)
                {
                    ally_aboard = true;
                }
            }
        }
        return any;
    }

    // Volume space to agent space, from the volume's own transform of the unit axes.
    LLMatrix4a modelOf(const LLVOVolume* volume)
    {
        const LLVector3 o = volume->volumePositionToAgent(LLVector3::zero);
        const LLVector3 x = volume->volumePositionToAgent(LLVector3::x_axis) - o;
        const LLVector3 y = volume->volumePositionToAgent(LLVector3::y_axis) - o;
        const LLVector3 z = volume->volumePositionToAgent(LLVector3::z_axis) - o;
        const F32 m[16] = { x.mV[VX], x.mV[VY], x.mV[VZ], 0.f, y.mV[VX], y.mV[VY], y.mV[VZ], 0.f,
                            z.mV[VX], z.mV[VY], z.mV[VZ], 0.f, o.mV[VX], o.mV[VY], o.mV[VZ], 1.f };
        return LLMatrix4a(m);
    }
}

SKOverviewMovers::SKOverviewMovers() = default;

SKOverviewMovers::~SKOverviewMovers()
{
    release();
}

void SKOverviewMovers::release()
{
    mMeshes.clear();
    mParts.clear();
    mVehicles.clear();
    mCandidates.clear();
    mStamp = 0;
}

bool SKOverviewMovers::update()
{
    LL_PROFILE_ZONE_SCOPED;
    // Candidates change with the census: what it saw move, its elevator cars, and whatever is physical now.
    if (SKWorldCensus::instanceExists())
    {
        SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
        if (snap && snap->mStamp != mStamp)
        {
            mStamp = snap->mStamp;
            std::set<LLUUID> roots;
            for (const sknav::RecordPtr& rec : snap->mRecords)
            {
                if (rec && rec->mDynamic)
                {
                    roots.insert(rec->mRoot);
                }
            }
            for (const sknav::Feature& f : snap->mFeatures)
            {
                if (f.mKind == sknav::FeatureKind::ELEVATOR || f.mKind == sknav::FeatureKind::VEHICLE)
                {
                    roots.insert(f.mRoots.begin(), f.mRoots.end());
                }
            }
            for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
            {
                LLViewerObject* object = gObjectList.getObject(i);
                if (object && !object->isDead() && object->isRoot() && !object->isAvatar() && !object->isAttachment() &&
                    object->flagUsePhysics())
                {
                    roots.insert(object->getID());
                }
            }
            mCandidates.assign(roots.begin(), roots.end());
        }
    }

    // Vehicles: the mini-map's, then any moving linkset someone sits on or that sits on click.
    mVehicles.clear();
    std::set<LLUUID> vehicle_roots;
    if (SKMapPeople::instanceExists())
    {
        for (const SKMapPeople::Vehicle& v : SKMapPeople::instance().vehicles())
        {
            mVehicles.push_back(v);
            vehicle_roots.insert(v.mRoot);
        }
    }
    const bool combat = SKMapPeople::instanceExists() && SKMapPeople::instance().combat();

    mParts.clear();
    std::set<LLUUID> live;
    for (const LLUUID& id : mCandidates)
    {
        LLViewerObject* root = gObjectList.findObject(id);
        if (!root || root->isDead() || !root->isRoot() || vehicle_roots.count(id))
        {
            continue;
        }
        bool ally_aboard = false;
        const bool seated = crewed(root, ally_aboard);
        if (seated || root->getClickAction() == CLICK_ACTION_SIT)
        {
            SKMapPeople::Vehicle v;
            v.mRoot = id;
            v.mPos = root->getPositionGlobal();
            v.mRot = root->getRotation();
            v.mMin = root->getScale() * -0.5f;
            v.mMax = root->getScale() * 0.5f;
            v.mLook = skmap::vehicleLook(combat, ally_aboard, seated);
            if (v.mLook != skmap::VehicleLook::HIDDEN)
            {
                mVehicles.push_back(v);
            }
            continue;
        }
        addParts(root, skoverview::CODE_MOVER, live);
    }
    // Vehicles draw the same way, in their look's colour.
    for (const SKMapPeople::Vehicle& v : mVehicles)
    {
        LLViewerObject* root = gObjectList.findObject(v.mRoot);
        if (!root || root->isDead())
        {
            continue;
        }
        const U8 code = v.mLook == skmap::VehicleLook::ALLY      ? (U8)skoverview::CODE_VEH_ALLY
                      : v.mLook == skmap::VehicleLook::NEUTRAL   ? (U8)skoverview::CODE_VEH_NEUTRAL
                                                                 : (U8)skoverview::CODE_VEH_CIVIL;
        addParts(root, code, live);
    }
    // Parts the census found restless, each on its own: a door that keeps opening, not its whole building.
    for (const LLUUID& id : mLoose)
    {
        LLViewerObject* object = gObjectList.findObject(id);
        if (object && !object->isDead())
        {
            addPart(object, skoverview::CODE_MOVER, live);
        }
    }
    // Meshes of parts no longer moving go.
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        it = live.count(it->first) ? std::next(it) : mMeshes.erase(it);
    }
    return !mParts.empty();
}

void SKOverviewMovers::addParts(LLViewerObject* root, U8 code, std::set<LLUUID>& live)
{
    // Drawn from its rendered faces, part by part.
    addPart(root, code, live);
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        if (!child->isAvatar())
        {
            addPart(child.get(), code, live);
        }
    }
}

void SKOverviewMovers::addPart(LLViewerObject* object, U8 code, std::set<LLUUID>& live)
{
    LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
    if (!volume || !volume->getVolume() || live.count(volume->getID()))
    {
        return;
    }
    Mesh& mesh = mMeshes[volume->getID()];
    if (mesh.mVolume != volume->getVolume())
    {
        mesh = Mesh();
        // A failed build leaves mVolume null, so the next update tries again.
        SKShapeGeometry geom;
        if (skRenderShapeGeometry(volume, geom))
        {
            LLPointer<LLVertexBuffer> buffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
            LLStrider<LLVector3> verts;
            if (buffer->allocateBuffer((U32)geom.mTriangles.size(), 0) && buffer->getVertexStrider(verts) && verts.get())
            {
                for (const LLVector3& p : geom.mTriangles)
                {
                    *verts++ = p;
                }
                buffer->unmapBuffer();
                mesh.mBuffer = buffer;
                mesh.mCount = (U32)geom.mTriangles.size();
                mesh.mVolume = volume->getVolume();
            }
        }
    }
    live.insert(volume->getID());
    if (mesh.mBuffer.notNull())
    {
        mParts.push_back({ volume->getID(), modelOf(volume), code });
    }
}

void SKOverviewMovers::draw(const LLMatrix4a& view) const
{
    LL_PROFILE_ZONE_SCOPED;
    for (const Part& part : mParts)
    {
        auto it = mMeshes.find(part.mId);
        if (it == mMeshes.end() || it->second.mBuffer.isNull())
        {
            continue;
        }
        gGL.diffuseColor4f((F32)part.mCode / 255.f, 0.f, 0.f, 1.f);
        gGL.loadMatrix(view);
        gGL.multMatrix(part.mModel);
        gGL.syncMatrices();
        it->second.mBuffer->setBuffer();
        it->second.mBuffer->drawArrays(LLRender::TRIANGLES, 0, it->second.mCount);
    }
}
