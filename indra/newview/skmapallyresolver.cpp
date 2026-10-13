/**
 * @file skmapallyresolver.cpp
 * @brief Evidence that nearby avatars wear one of the user's candidate groups active: attachment probes.
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

#include "skmapallyresolver.h"

#include "alobjectproperties.h"
#include "llagent.h"
#include "llnamevalue.h"
#include "llviewerregion.h"
#include "message.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

SKMapAllyResolver::SKMapAllyResolver()
{
    // Family replies come through the properties cache; its signal says which object changed.
    mPropertiesConnection = ALObjectPropertiesCache::instance().setChangeCallback([this](const LLUUID& id) { onProperties(id); });
}

SKMapAllyResolver::~SKMapAllyResolver()
{
    mPropertiesConnection.disconnect();
}

void SKMapAllyResolver::clear()
{
    forgetRecords();
}

void SKMapAllyResolver::forgetRecords()
{
    mRecords.clear();
    mRoots.clear();
    mPendingRoots.clear();
    mProbedRoots.clear();
    mBudget.clear();
}

void SKMapAllyResolver::noteAvatar(LLVOAvatar* avatar, bool in_view, F64 now)
{
    static LLCachedControl<bool> probe_untitled(gSavedSettings, "SKMapAllyProbeUntitled", false);
    if (!avatar)
    {
        return;
    }
    const LLUUID id = avatar->getID();
    // The worn attachment roots: never a HUD, never a temporary attachment someone else rezzed.
    std::vector<LLUUID> roots;
    for (const auto& point : avatar->mAttachmentPoints)
    {
        LLViewerJointAttachment* attachment = point.second;
        if (!attachment || attachment->getIsHUDAttachment())
        {
            continue;
        }
        for (const LLViewerObject* object : attachment->mAttachedObjects)
        {
            if (object && !object->isDead() && !object->isTempAttachment() && object->getID().notNull())
            {
                roots.push_back(object->getID());
            }
        }
    }
    std::sort(roots.begin(), roots.end());
    LLNameValue* title_nv = avatar->getNVPair("Title");
    const std::string title = title_nv && title_nv->getString() ? title_nv->getString() : std::string();
    skmap::AllyRecord& record = mRecords[id];
    skmap::observe(record, title, roots, now, probe_untitled);
    mRoots[id] = roots;
    if (mActive && in_view && skmap::wantsProbe(record, now, !roots.empty()))
    {
        mBudget.enqueue(id);
    }
}

namespace
{
    void selectAndDeselect(LLViewerObject* object)
    {
        LLMessageSystem* msg = gMessageSystem;
        for (const char* name : { _PREHASH_ObjectSelect, _PREHASH_ObjectDeselect })
        {
            msg->newMessageFast(name);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
            msg->sendReliable(object->getRegion()->getHost());
        }
    }
}

void SKMapAllyResolver::dispatch(F64 now)
{
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<F32> rate(gSavedSettings, "SKMapAllyProbeRate", skmap::PROBE_RATE);
    mBudget.setRate(llmax(0.1f, static_cast<F32>(rate)));
    // Requests that never answered: back to unknown, retried later.
    for (const LLUUID& id : mBudget.expire(now))
    {
        auto it = mRecords.find(id);
        if (it != mRecords.end())
        {
            for (auto p = mPendingRoots.begin(); p != mPendingRoots.end();)
            {
                p = p->second == id ? mPendingRoots.erase(p) : std::next(p);
            }
            skmap::probeTimeout(it->second, now);
        }
    }
    // Records of avatars not seen for ten minutes go.
    for (auto it = mRecords.begin(); it != mRecords.end();)
    {
        if (now - it->second.mLastSeen > skmap::RECORD_FORGET_S)
        {
            mRoots.erase(it->first);
            it = mRecords.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (!mActive)
    {
        return;
    }
    for (LLUUID id = mBudget.next(now); id.notNull(); id = mBudget.next(now))
    {
        auto it = mRecords.find(id);
        const auto roots = mRoots.find(id);
        if (it == mRecords.end() || roots == mRoots.end() || !skmap::wantsProbe(it->second, now, !roots->second.empty()))
        {
            continue;
        }
        skmap::AllyRecord& record = it->second;
        const LLUUID target = skmap::chooseTarget(record.mSignature, roots->second);
        // A root never asked about may already be in the cache; a re-probe after a title change
        // must ask afresh, since the cached group is what it replaces.
        const ALObjectPropertiesCache::ServerProps* cached = ALObjectPropertiesCache::instance().get(target);
        if (cached && !record.mFresh && !mProbedRoots.count(target))
        {
            mProbedRoots.insert(target);
            skmap::probeSent(record, target, roots->second, now);
            skmap::probeReply(record, target, cached->mGroupId, now);
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(target);
        // A raw deselect of something the user is editing would desync the sim's selection.
        if (!object || object->isDead() || !object->getRegion() || object->isSelected())
        {
            continue;
        }
        // The sim never answers a family request for an attachment; a select and deselect it does.
        selectAndDeselect(object);
        ALObjectPropertiesCache::instance().markPending(target);
        mBudget.sent(id, now);
        mProbedRoots.insert(target);
        mPendingRoots[target] = id;
        skmap::probeSent(record, target, roots->second, now);
    }
}

void SKMapAllyResolver::onProperties(const LLUUID& object)
{
    const auto pending = mPendingRoots.find(object);
    if (pending == mPendingRoots.end())
    {
        return;     // not ours, or not pending: a pending re-probe's old answer must not be read
    }
    const LLUUID wearer = pending->second;
    mPendingRoots.erase(pending);
    mBudget.done(wearer);
    auto it = mRecords.find(wearer);
    const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(object);
    if (it != mRecords.end() && props)
    {
        skmap::probeReply(it->second, object, props->mGroupId, LLFrameTimer::getUptimeSeconds().value());
    }
}

void SKMapAllyResolver::update(bool active, const std::set<LLUUID>& candidates)
{
    mCandidates = candidates;
    mActive = active;
}

skmap::AllyVerdict SKMapAllyResolver::verdict(const LLUUID& id) const
{
    const auto it = mRecords.find(id);
    return skmap::allyVerdict(it != mRecords.end() ? it->second : skmap::AllyRecord(), mCandidates);
}
