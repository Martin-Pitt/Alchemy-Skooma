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
#include "lldir.h"
#include "llfile.h"
#include "llviewercamera.h"
#include "llviewerparcelmgr.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "pipeline.h"
#include "lljoint.h"
#include "skmappalette.h"
#include "sknetmap.h"
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
    // Every probe record goes with a teleport, and combat with one to another region.
    mTeleportConnection = LLViewerParcelMgr::getInstance()->setTeleportFinishedCallback([this](const LLVector3d&, const bool& local) {
        mResolver.forgetRecords();
        mCodes.clear();
        if (!local)
        {
            mEngagement.disengage();
        }
    });
}

void SKMapPeople::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SKMapPeople::onIdle, nullptr);
    mTeleportConnection.disconnect();
    mResolver.clear();
    mPeople.clear();
}

void SKMapPeople::noteDrawn(const skmap::MapView& view)
{
    LLVector3d min, max;
    view.visibleBox(min, max);
    noteDrawn(min, max);
}

void SKMapPeople::noteDrawn(const LLVector3d& min, const LLVector3d& max)
{
    mLastDrawn = LLFrameTimer::getUptimeSeconds().value();
    mViewMin = min;
    mViewMax = max;
    const LLVector3d pad = (mViewMax - mViewMin) * 0.05;
    mViewMin -= pad;
    mViewMax += pad;
}

bool SKMapPeople::active() const
{
    return LLFrameTimer::getUptimeSeconds().value() - mLastDrawn < DRAWN_RECENTLY_S;
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
            mVehicles.clear();
        }
        // Kept current, so the first frame after the map reopens on damage land already hides what combat hides.
        if (LLWorld::instanceExists() && gAgent.getRegion())
        {
            updateCombat();
        }
        else
        {
            mCombat = false;
        }
        mResolver.update(false, mCandidates);
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    updateCombat();
    updateCandidates();
    mResolver.update(mCombat, mCandidates);
    gatherPeople();
    mResolver.dispatch(LLFrameTimer::getUptimeSeconds().value());
    updateFiringLines();
    updateProjectiles();
    gatherVehicles();
}

void SKMapPeople::gatherVehicles()
{
    LL_PROFILE_ZONE_SCOPED;
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
        // The crew, read live from the seated avatars; a vehicle crewed by nobody allied hides in combat.
        const auto seat = mSeats.find(id);
        v.mLook = skmap::vehicleLook(mCombat, seat != mSeats.end() && seat->second.second, seat != mSeats.end() && seat->second.first);
        if (v.mLook == skmap::VehicleLook::HIDDEN)
        {
            continue;
        }
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

void SKMapPeople::addPing(const LLVector3d& pos)
{
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    mPings.erase(std::remove_if(mPings.begin(), mPings.end(), [now](const Ping& p) { return now - p.mTime > skmap::palette::PING_SECONDS; }),
                 mPings.end());
    mPings.push_back({ pos, now });
    SKNetMap::firePing(pos);
}

void SKMapPeople::updateFiringLines()
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerFiringLines", true);
    if (!show)
    {
        mFiring.clear();
        return;
    }
    constexpr F64 RECAST_S = 0.25;      ///< 4 Hz an avatar
    constexpr S32 RAYS_PER_IDLE = 4;
    constexpr F64 RAY_BUDGET_S = 0.001; ///< what an idle may spend on rays, averaged
    constexpr F64 RAY_BURST_S = 0.004;  ///< how much unspent budget may accumulate
    constexpr F64 RAY_DEBT_S = 0.5;     ///< how far one dear ray may push the next out
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    // A world ray through a hub full of mesh can cost a hundred milliseconds, so the rays are
    // paid for by measured cost: a dear one is followed by as many idles as it takes to earn
    // the next, and a cheap scene never notices.
    mRayAllowance = llmin(mRayAllowance + RAY_BUDGET_S, RAY_BURST_S);
    // Only for people the map draws: in combat, allies; a hidden avatar's aim must not leak.
    std::map<LLUUID, FiringLine> kept;
    std::vector<std::pair<F64, const Person*>> due;
    for (const Person& p : mPeople)
    {
        const bool in_view = p.mPos.mdV[VX] >= mViewMin.mdV[VX] && p.mPos.mdV[VX] <= mViewMax.mdV[VX] && p.mPos.mdV[VY] >= mViewMin.mdV[VY]
                          && p.mPos.mdV[VY] <= mViewMax.mdV[VY];
        if (!p.mLoaded || !in_view)
        {
            continue;
        }
        const auto it = mFiring.find(p.mId);
        if (it != mFiring.end())
        {
            // The aim is cheap and follows the head every idle; only the reach waits for a ray.
            FiringLine line = it->second;
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(p.mId));
            if (LLJoint* head = avatar ? avatar->getJoint("mHead") : nullptr)
            {
                const LLVector3 from = head->getWorldPosition();
                const LLVector3 dir = LLVector3::x_axis * head->getWorldRotation();
                line.mFrom = gAgent.getPosGlobalFromAgent(from);
                line.mTo = gAgent.getPosGlobalFromAgent(from + dir * line.mLength);
            }
            kept[p.mId] = line;
        }
        const F64 last = it != mFiring.end() ? it->second.mCast : -1.0e9;
        if (now - last >= RECAST_S)
        {
            due.emplace_back(last, &p);
        }
    }
    // The longest waiting first, a few rays an idle.
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    S32 rays = 0;
    for (const auto& entry : due)
    {
        if (rays >= RAYS_PER_IDLE || mRayAllowance <= 0.0)
        {
            break;
        }
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(entry.second->mId));
        LLJoint* head = avatar ? avatar->getJoint("mHead") : nullptr;
        if (!head)
        {
            continue;
        }
        ++rays;
        const F64 ray_start = LLFrameTimer::getTotalSeconds();
        const LLVector3 from = head->getWorldPosition();
        const LLVector3 dir = LLVector3::x_axis * head->getWorldRotation();
        const LLVector3 to = from + dir * skmap::palette::FIRING_REACH_M;
        LLVector4a start, end, hit;
        // Past the head, so the shooter's own attachments are not what the ray finds.
        start.load3((from + dir * 0.5f).mV);
        end.load3(to.mV);
        S32 face = -1;
        LLVector3 stop = to;
        if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, false, false, &face, &hit))
        {
            stop.set(hit.getF32ptr());
        }
        mRayAllowance = llmax(mRayAllowance - (LLFrameTimer::getTotalSeconds() - ray_start), -RAY_DEBT_S);
        FiringLine& line = kept[entry.second->mId];
        line.mFrom = gAgent.getPosGlobalFromAgent(from);
        line.mTo = gAgent.getPosGlobalFromAgent(stop);
        line.mLength = dist_vec(from, stop);
        line.mCast = now;
    }
    mFiring.swap(kept);
}

void SKMapPeople::updateProjectiles()
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerProjectiles", true);
    static LLCachedControl<F32> fast_speed(gSavedSettings, "SKMapProjectileMinSpeed", 20.f);
    static LLCachedControl<F32> max_size(gSavedSettings, "SKMapProjectileMaxSize", 3.f);
    if (!show)
    {
        mProjectiles.clear();
        mFastTrails.clear();
        mTrails.clear();
        mShooters.clear();
        return;
    }
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    // Avatar trails for tracing shooters keep their own cadence; the shots themselves are gathered every frame.
    if (now - mProjectilesAt >= 0.1)
    {
        mProjectilesAt = now;
        updateTrails(now);
    }
    static LLCachedControl<F32> trail_slow(gSavedSettings, "SKMapTrailMinSpeed", 80.f);
    static LLCachedControl<F32> trail_fast(gSavedSettings, "SKMapTrailMaxSpeed", 200.f);
    static LLCachedControl<F32> alpha_slow(gSavedSettings, "SKMapTrailAlphaSlow", 0.05f);
    static LLCachedControl<F32> alpha_fast(gSavedSettings, "SKMapTrailAlphaFast", 0.35f);
    static LLCachedControl<F32> life_slow(gSavedSettings, "SKMapTrailLifeSlow", 0.02f);
    static LLCachedControl<F32> life_fast(gSavedSettings, "SKMapTrailLifeFast", 0.08f);
    std::vector<Projectile> found;
    std::map<LLUUID, LLUUID> shooters;
    std::map<LLUUID, FastTrail> trails;
    // The whole object list: the map objects hold only large or owned prims, so a bullet never reached it.
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!object || object->isDead() || !object->isRoot() || object->isAttachment() || object->isAvatar())
        {
            continue;
        }
        const LLVector3 velocity = object->getVelocity();
        const F32 speed = velocity.length();
        if (speed < llmin(llmax(static_cast<F32>(fast_speed), 0.5f), static_cast<F32>(trail_slow)))
        {
            continue;
        }
        const LLVector3d pos = object->getPositionGlobal();
        if (pos.mdV[VX] < mViewMin.mdV[VX] || pos.mdV[VX] > mViewMax.mdV[VX] || pos.mdV[VY] < mViewMin.mdV[VY] || pos.mdV[VY] > mViewMax.mdV[VY])
        {
            continue;
        }
        if (speed >= static_cast<F32>(trail_slow))
        {
            // Any fast object keeps a short history, longer and stronger the faster it goes.
            const F32 t = llclamp((speed - trail_slow) / llmax(static_cast<F32>(trail_fast - trail_slow), 1.f), 0.f, 1.f);
            FastTrail& trail = trails[object->getID()];
            trail.mAlpha = lerp(static_cast<F32>(alpha_slow), static_cast<F32>(alpha_fast), t);
            trail.mLife = lerp(static_cast<F32>(life_slow), static_cast<F32>(life_fast), t);
            trail.mPoints.emplace_back(now, pos);
            const auto old = mFastTrails.find(object->getID());
            if (old != mFastTrails.end())
            {
                for (const auto& point : old->second.mPoints)
                {
                    if (now - point.first > trail.mLife)
                    {
                        break;
                    }
                    if (point.first < now)
                    {
                        trail.mPoints.push_back(point);
                    }
                }
            }
        }
        const LLVector3 scale = object->getScale();
        if (!object->flagUsePhysics() || speed < llmax(static_cast<F32>(fast_speed), 0.5f) || scale.length() > static_cast<F32>(max_size))
        {
            continue;
        }
        Projectile p;
        p.mPos = pos;
        p.mDir = velocity / speed;
        // The box's extent along and across the direction of travel, from its scale and rotation.
        const LLQuaternion rot = object->getRotationRegion();
        const LLVector3 axes[3] = { LLVector3::x_axis * rot, LLVector3::y_axis * rot, LLVector3::z_axis * rot };
        F32 across = 0.f;
        for (S32 k = 0; k < 3; ++k)
        {
            const F32 along = std::fabs(axes[k] * p.mDir);
            p.mLength += along * scale.mV[k];
            across = llmax(across, std::sqrt(llmax(0.f, 1.f - along * along)) * scale.mV[k]);
        }
        p.mWidth = across;
        // Traced once, when first seen, while the trails still hold where everyone was.
        const auto traced = mShooters.find(object->getID());
        p.mShooter = traced != mShooters.end() ? traced->second : traceShooter(pos, velocity);
        shooters[object->getID()] = p.mShooter;
        found.push_back(p);
    }
    mProjectiles.swap(found);
    mFastTrails.swap(trails);
    mShooters.swap(shooters);
}

void SKMapPeople::updateTrails(F64 now)
{
    constexpr F64 TRAIL_S = 2.0;
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, gAgent.getPositionGlobal(), PEOPLE_REACH_M);
    ids.push_back(gAgentID);
    positions.push_back(gAgent.getPositionGlobal());
    std::map<LLUUID, std::deque<std::pair<F64, LLVector3d>>> trails;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        auto& trail = trails[ids[i]];
        const auto old = mTrails.find(ids[i]);
        if (old != mTrails.end())
        {
            trail.swap(old->second);
        }
        trail.emplace_back(now, positions[i]);
        while (!trail.empty() && now - trail.front().first > TRAIL_S)
        {
            trail.pop_front();
        }
    }
    mTrails.swap(trails);
}

LLUUID SKMapPeople::traceShooter(const LLVector3d& pos, const LLVector3& velocity) const
{
    // Whose chest the shot's line passed nearest, run backwards over the trails' two seconds; sideways distance
    // alone, as 10 Hz samples put a 200 m/s shot 20 m off along its track.
    constexpr F64 CHEST_M = 0.5;        ///< above the avatar's reported position
    constexpr F64 FIT_M = 2.0;          ///< recorded shots pass within 1.8 m, p90
    const F64 speed = velocity.length();
    if (speed < 1e-3)
    {
        return LLUUID::null;
    }
    const LLVector3d back = LLVector3d(velocity) * (-1.0 / speed);
    const F64 reach = speed * 2.0;
    LLUUID best;
    F64 best_d = FIT_M;
    for (const auto& trail : mTrails)
    {
        for (const auto& sample : trail.second)
        {
            const LLVector3d w = sample.second + LLVector3d(0.0, 0.0, CHEST_M) - pos;
            const F64 along = llclamp(w * back, 0.0, reach);
            const F64 d = (w - back * along).magVec();
            if (d < best_d)
            {
                best_d = d;
                best = trail.first;
            }
        }
    }
    return best;
}

void SKMapPeople::updateCombat()
{
    // The check behind the health display: the agent's own parcel, nothing else. Damage land is
    // combat only once the user engages there. The debug switch can only turn combat on, never
    // show anyone it would hide.
    static LLCachedControl<bool> force(gSavedSettings, "SKMapDebugForceCombat", false);
    LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();
    const bool damage = parcels->allowAgentDamage(gAgent.getRegion(), parcels->getAgentParcel());
    const bool engaged = mEngagement.update(damage, gAgentCamera.cameraMouselook(), LLFrameTimer::getUptimeSeconds().value());
    mCombat = force || engaged;
}

void SKMapPeople::addEvent(const SKCombatEvent& event)
{
    if (event.actor == gAgentID || event.target == gAgentID)
    {
        mEngagement.engage(LLFrameTimer::getUptimeSeconds().value());
    }
    mEvents.add(event);
}

void SKMapPeople::loadFactions()
{
    mFactionsLoaded = true;
    // The user's own copy wins over the shipped one, when it holds a faction.
    const std::string user = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "sk_slmc_factions.xml");
    if (LLFile::isfile(user) && mFactions.load(user))
    {
        return;
    }
    if (!mFactions.load(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "sk_slmc_factions.xml")))
    {
        LL_WARNS("SKMap") << "no faction table: allies are the active group's members only" << LL_ENDL;
    }
}

void SKMapPeople::updateCandidates()
{
    const LLUUID group = gAgent.getGroupID();
    if (mGroupKnown && group == mActiveGroup)
    {
        return;
    }
    if (!mFactionsLoaded)
    {
        loadFactions();
    }
    mGroupKnown = true;
    mActiveGroup = group;
    mCandidates = mFactions.candidatesFor(group);
    const skmap::Faction* faction = mFactions.factionOf(group);
    LL_INFOS("SKMap") << "active group " << group << ": " << (faction ? faction->mName : std::string("no faction")) << ", "
                      << mCandidates.size() << " ally groups" << LL_ENDL;
}

void SKMapPeople::gatherPeople()
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<bool> avatar_colours(gSavedSettings, "SKMapAvatarColors", true);
    const LLVector3d agent = gAgent.getPositionGlobal();
    const F64 camera_z = gAgentCamera.getCameraPositionGlobal().mdV[VZ];
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, agent, PEOPLE_REACH_M);

    // The probe sees loaded avatars nearest first, so the nearest are asked about first.
    if (mCombat)
    {
        std::vector<std::pair<F64, LLVOAvatar*>> loaded;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            LLVOAvatar* avatar = ids[i] == gAgent.getID() ? nullptr : dynamic_cast<LLVOAvatar*>(gObjectList.findObject(ids[i]));
            if (avatar && !avatar->isDead())
            {
                loaded.emplace_back(dist_vec_squared(positions[i], agent), avatar);
            }
        }
        std::sort(loaded.begin(), loaded.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const F64 now = LLFrameTimer::getUptimeSeconds().value();
        for (const auto& entry : loaded)
        {
            const LLVector3d p = entry.second->getPositionGlobal();
            const bool in_view = p.mdV[VX] >= mViewMin.mdV[VX] && p.mdV[VX] <= mViewMax.mdV[VX] && p.mdV[VY] >= mViewMin.mdV[VY]
                              && p.mdV[VY] <= mViewMax.mdV[VY];
            mResolver.noteAvatar(entry.second, in_view, now);
        }
    }

    static LLCachedControl<F32> code_hold(gSavedSettings, "SKMapMarkerCodeHold", skmap::CODE_HOLD_S);
    mCodes.setHold(code_hold);
    const F64 code_now = LLFrameTimer::getUptimeSeconds().value();
    mCodes.release(code_now);

    const LLColor4 general(skmap::palette::DOT_GENERAL.mR, skmap::palette::DOT_GENERAL.mG, skmap::palette::DOT_GENERAL.mB, 1.f);
    mPeople.clear();
    mPeople.reserve(ids.size());
    mSeats.clear();
    // The user crews their own vehicle as an ally would, so it never reads as empty or hostile.
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting())
    {
        if (LLViewerObject* seat = static_cast<LLViewerObject*>(gAgentAvatarp->getParent()))
        {
            mSeats[seat->getRootEdit()->getID()] = { true, true };
        }
    }
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
            // Ours is the body rotation we send; anyone else's is the sim's, extrapolated, not the drawn body's.
            const LLVector3 forward = LLVector3::x_axis * (avatar->isSelf() ? avatar->getRenderRotation() : avatar->skPredictedRotationRegion());
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
        else
        {
            // Their air gap, space and storey, the viewer's way; the map compares them with what it draws.
            p.mWhere = skmap::plan::whereabouts(p.mPos);
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
        p.mAlly = mCombat && mResolver.verdict(p.mId) == skmap::AllyVerdict::ALLY;
        p.mVisibility = skmap::visibilityOf(mCombat, false, p.mAlly);
        if (p.mSitting)
        {
            std::pair<bool, bool>& seat = mSeats[p.mSeatRoot];
            seat.first = true;
            seat.second = seat.second || p.mAlly;
        }
        // Hidden means hidden everywhere: not drawn, picked, listed, crewing or aiming.
        if (p.mVisibility == skmap::Visibility::HIDDEN)
        {
            continue;
        }
        if (p.mVisibility == skmap::Visibility::DOT_CODED)
        {
            p.mCode = skmap::codeString(mCodes.codeFor(p.mId, code_now));
        }
        mPeople.push_back(p);
    }
    // Friends draw last, on top.
    std::stable_sort(mPeople.begin(), mPeople.end(), [](const Person& a, const Person& b) { return !a.mFriend && b.mFriend; });
}
