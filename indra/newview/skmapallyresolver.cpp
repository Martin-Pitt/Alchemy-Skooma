/**
 * @file skmapallyresolver.cpp
 * @brief Evidence that nearby avatars belong to the user's candidate groups: rosters and probes.
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

#include "llagent.h"
#include "llviewercontrol.h"

namespace
{
    constexpr F64 ROSTER_REFRESH_S = 300.0;     ///< held rosters are asked for again this often in combat
    constexpr F64 ROSTER_GIVE_UP_S = 60.0;      ///< a roster that never completes stops blocking the next
    constexpr F64 ROSTER_DROP_S = 60.0;         ///< after leaving combat
}

SKMapAllyResolver::SKMapAllyResolver()
{
}

SKMapAllyResolver::~SKMapAllyResolver()
{
    dropRosters();
}

void SKMapAllyResolver::clear()
{
    dropRosters();
}

void SKMapAllyResolver::dropRosters()
{
    if (LLGroupMgr::instanceExists())
    {
        for (auto& entry : mObservers)
        {
            LLGroupMgr::getInstance()->removeObserver(entry.first, entry.second.get());
        }
    }
    mObservers.clear();
    mRequested.clear();
    mWaiting.setNull();
}

void SKMapAllyResolver::update(bool active, bool combat, const std::set<LLUUID>& candidates, F64 now)
{
    mCandidates = candidates;
    updateRosters(active, combat, now);
}

void SKMapAllyResolver::updateRosters(bool active, bool combat, F64 now)
{
    static LLCachedControl<bool> use_rosters(gSavedSettings, "SKMapAllyRoster", true);
    if (!use_rosters)
    {
        dropRosters();
        return;
    }
    if (!combat)
    {
        if (mLeftCombat < 0.0)
        {
            mLeftCombat = now;
        }
        if (now - mLeftCombat >= ROSTER_DROP_S)
        {
            dropRosters();
        }
        return;
    }
    mLeftCombat = -1.0;
    if (!active)
    {
        return;
    }

    // Rosters exist only for groups the user is in: those candidates are held observed, so
    // LLGroupMgr keeps them, and the rest are let go.
    LLGroupMgr* groups = LLGroupMgr::getInstance();
    std::set<LLUUID> wanted;
    for (const LLUUID& g : mCandidates)
    {
        if (gAgent.isInGroup(g))
        {
            wanted.insert(g);
        }
    }
    for (auto it = mObservers.begin(); it != mObservers.end();)
    {
        if (!wanted.count(it->first))
        {
            groups->removeObserver(it->first, it->second.get());
            mRequested.erase(it->first);
            it = mObservers.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const LLUUID& g : wanted)
    {
        if (!mObservers.count(g))
        {
            auto observer = std::make_unique<RosterObserver>();
            groups->addObserver(g, observer.get());
            mObservers[g] = std::move(observer);
        }
    }

    // One roster at a time: LLGroupMgr has one member request in flight.
    if (mWaiting.notNull())
    {
        LLGroupMgrGroupData* data = groups->getGroupData(mWaiting);
        const bool complete = data && data->isMemberDataComplete();
        if (!complete && now - mRequested[mWaiting] < ROSTER_GIVE_UP_S && wanted.count(mWaiting))
        {
            return;
        }
        mWaiting.setNull();
    }
    for (const LLUUID& g : wanted)
    {
        LLGroupMgrGroupData* data = groups->getGroupData(g);
        const auto asked = mRequested.find(g);
        const bool stale = asked == mRequested.end() || now - asked->second >= ROSTER_REFRESH_S;
        if (stale || !(data && data->isMemberDataComplete()))
        {
            if (asked != mRequested.end() && !stale)
            {
                continue;   // asked recently and still not complete: give it time
            }
            groups->sendCapGroupMembersRequest(g);
            mRequested[g] = now;
            mWaiting = g;
            return;
        }
    }
}

bool SKMapAllyResolver::rosterHit(const LLUUID& id) const
{
    if (!LLGroupMgr::instanceExists())
    {
        return false;
    }
    for (const auto& entry : mObservers)
    {
        if (!mCandidates.count(entry.first))
        {
            continue;
        }
        LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(entry.first);
        if (data && data->isMemberDataComplete() && data->mMembers.count(id))
        {
            return true;
        }
    }
    return false;
}

skmap::AllyVerdict SKMapAllyResolver::verdict(const LLUUID& id) const
{
    return rosterHit(id) ? skmap::AllyVerdict::ALLY : skmap::AllyVerdict::UNKNOWN;
}
