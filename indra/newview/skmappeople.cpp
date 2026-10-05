/**
 * @file skmappeople.cpp
 * @brief The mini-map's people: a snapshot of nearby avatars taken once an idle, for the draw.
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

#include "skmappeople.h"

#include "alavatargroups.h"
#include "alobjectproperties.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llavataractions.h"
#include "llcallbacklist.h"
#include "llgroupmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "pipeline.h"
#include "skmappalette.h"
#include "skmapscene.h"
#include "sknavmesh.h"
#include "skworldcensus.h"

namespace
{
    constexpr F64 DRAWN_RECENTLY_S = 2.0;
    constexpr F64 COARSE_CEILING_M = 1020.0;   ///< coarse locations stop telling heights here
    constexpr F32 PEOPLE_REACH_M = 1024.f;
}

SKMapPeople::SKMapPeople()
{
}

SKMapPeople::~SKMapPeople()
{
}

void SKMapPeople::initSingleton()
{
    // Taking these here makes them outlive us at teardown.
    SKNavMesh::instance();
    ALObjectPropertiesCache::instance();
    LLGroupMgr::instance();
    gIdleCallbacks.addFunction(&SKMapPeople::onIdle, nullptr);
}

void SKMapPeople::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SKMapPeople::onIdle, nullptr);
    mPeople.clear();
}

void SKMapPeople::noteDrawn()
{
    mLastDrawn = LLFrameTimer::getElapsedSeconds();
}

bool SKMapPeople::active() const
{
    return LLFrameTimer::getElapsedSeconds() - mLastDrawn < DRAWN_RECENTLY_S;
}

// static
void SKMapPeople::onIdle(void*)
{
    if (instanceExists())
    {
        getInstance()->idle();
    }
}

void SKMapPeople::idle()
{
    if (!active() || !LLWorld::instanceExists() || !gAgent.getRegion())
    {
        if (!mPeople.empty())
        {
            mPeople.clear();
        }
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    gatherPeople();
    updateCoverAhead();
    gatherVehicles();
}

void SKMapPeople::gatherVehicles()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerVehicles", true);
    mVehicles.clear();
    if (!show || !SKWorldCensus::instanceExists())
    {
        return;
    }
    // The census latches a linkset as a vehicle once it was crewed while physical or travelled,
    // so a parked one stays; the roots are read again only when a new census lands.
    if (const SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        if (snap->mStamp != mVehicleStamp)
        {
            mVehicleStamp = snap->mStamp;
            mVehicleRoots.clear();
            for (const sknav::Feature& f : snap->mFeatures)
            {
                if (f.mKind == sknav::FeatureKind::VEHICLE)
                {
                    mVehicleRoots.insert(mVehicleRoots.end(), f.mRoots.begin(), f.mRoots.end());
                }
            }
        }
    }
    std::map<LLUUID, BoxCache> boxes;
    for (const LLUUID& id : mVehicleRoots)
    {
        LLViewerObject* root = gObjectList.findObject(id);
        if (!root || root->isDead() || !root->isRoot())
        {
            continue;
        }
        Vehicle v;
        v.mRoot = id;
        v.mPos = root->getPositionGlobal();
        v.mRot = root->getRotation();
        // The union of the parts' boxes in the root's frame, cached by the parts' local ids.
        U64 key = sknav::mix(sknav::SEED, root->getChildren().size());
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            if (!child->isAvatar())
            {
                key = sknav::mix(key, child->getLocalID());
            }
        }
        auto cached = mBoxes.find(id);
        if (cached == mBoxes.end() || cached->second.mKey != key)
        {
            BoxCache box;
            box.mKey = key;
            box.mMin = root->getScale() * -0.5f;
            box.mMax = root->getScale() * 0.5f;
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (child->isAvatar())
                {
                    continue;
                }
                const LLVector3 half = child->getScale() * 0.5f;
                for (S32 k = 0; k < 8; ++k)
                {
                    const LLVector3 corner((k & 1) ? half.mV[VX] : -half.mV[VX], (k & 2) ? half.mV[VY] : -half.mV[VY], (k & 4) ? half.mV[VZ] : -half.mV[VZ]);
                    const LLVector3 p = child->getPosition() + corner * child->getRotation();
                    box.mMin.set(llmin(box.mMin.mV[VX], p.mV[VX]), llmin(box.mMin.mV[VY], p.mV[VY]), llmin(box.mMin.mV[VZ], p.mV[VZ]));
                    box.mMax.set(llmax(box.mMax.mV[VX], p.mV[VX]), llmax(box.mMax.mV[VY], p.mV[VY]), llmax(box.mMax.mV[VZ], p.mV[VZ]));
                }
            }
            cached = mBoxes.insert_or_assign(id, box).first;
        }
        v.mMin = cached->second.mMin;
        v.mMax = cached->second.mMax;
        boxes[id] = cached->second;
        // The crew, read live: loaded avatars seated on it.
        for (const Person& p : mPeople)
        {
            if (p.mSitting && p.mSeatRoot == id)
            {
                v.mCrew.push_back(p.mId);
            }
        }
        mVehicles.push_back(v);
    }
    mBoxes.swap(boxes);
}

void SKMapPeople::updateCoverAhead()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapCoverAhead", true);
    if (!show)
    {
        mCoverAhead = sknav::Cover::NONE;
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now - mCoverAt < 0.1)
    {
        return;
    }
    mCoverAt = now;
    // Along the camera in plan, within a metre and a half, against the agent's own eyes.
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 eye = isAgentAvatarValid() ? gAgentAvatarp->mBodySize.mV[VZ] * 0.94f : 1.8f;
    sknav::Cover cover = sknav::Cover::NONE;
    F32 height = 0.f;
    if (!SKNavMesh::instance().coverAt(gAgent.getPositionGlobal(), LLVector3(at.mV[VX], at.mV[VY], 0.f), sknav::AgentClass::AVATAR, eye, cover,
                                       height, 1.5f))
    {
        cover = sknav::Cover::NONE;
    }
    mCoverAhead = cover;
}

void SKMapPeople::gatherPeople()
{
    static LLCachedControl<bool> avatar_colours(gSavedSettings, "SKMapAvatarColors", true);
    const LLVector3d agent = gAgent.getPositionGlobal();
    const F64 camera_z = gAgentCamera.getCameraPositionGlobal().mdV[VZ];
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, agent, PEOPLE_REACH_M);

    // Floors from the scene when there is one; the raw height difference otherwise.
    const skmap::ScenePtr scene = SKMapScene::instanceExists() ? SKMapScene::instance().scene() : nullptr;
    const LLColor4 general(skmap::palette::DOT_GENERAL.mR, skmap::palette::DOT_GENERAL.mG, skmap::palette::DOT_GENERAL.mB, 1.f);
    mPeople.clear();
    mPeople.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == gAgent.getID())
        {
            continue;
        }
        Person p;
        p.mId = ids[i];
        p.mPos = positions[i];
        p.mFriend = LLAvatarActions::isFriend(p.mId);
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(p.mId));
        if (avatar && !avatar->isDead())
        {
            p.mLoaded = true;
            p.mPos = avatar->getPositionGlobal();
            const LLVector3 forward = LLVector3::x_axis * avatar->getRenderRotation();
            p.mHeading = std::atan2(forward.mV[VY], forward.mV[VX]);
            p.mHasHeading = true;
            p.mSitting = avatar->isSitting();
            if (p.mSitting)
            {
                if (LLViewerObject* seat = static_cast<LLViewerObject*>(avatar->getParent()))
                {
                    p.mSeatRoot = seat->getRootEdit()->getID();
                }
            }
        }
        // A coarse location past the ceiling has no height, unless the camera is below it.
        p.mZKnown = p.mLoaded || !(p.mPos.mdV[VZ] >= COARSE_CEILING_M && camera_z >= COARSE_CEILING_M);
        if (!p.mZKnown)
        {
            p.mFloor = skmap::FloorRelation::UNKNOWN;
        }
        else if (scene && SKMapScene::instance().agentFloorCurrent())
        {
            p.mFloor = skmap::floorRelation(*scene, SKMapScene::instance().agentFloor(), p.mPos, true);
        }
        else
        {
            const F64 dz = p.mPos.mdV[VZ] - agent.mdV[VZ];
            p.mFloor = dz >= skmap::FLOOR_RAW_M ? skmap::FloorRelation::ABOVE
                     : dz <= -skmap::FLOOR_RAW_M ? skmap::FloorRelation::BELOW
                                                 : skmap::FloorRelation::SAME;
        }
        if (avatar_colours)
        {
            p.mColour = p.mLoaded && avatar ? ALAvatarGroups::instance().getAvatarColor(avatar, general, ALAvatarGroups::COLOR_MINIMAP)
                                            : ALAvatarGroups::instance().getAvatarColor(p.mId, general, ALAvatarGroups::COLOR_MINIMAP);
        }
        else
        {
            p.mColour = general;
        }
        mPeople.push_back(p);
    }
    // Friends draw last, on top.
    std::stable_sort(mPeople.begin(), mPeople.end(), [](const Person& a, const Person& b) { return !a.mFriend && b.mFriend; });
}
