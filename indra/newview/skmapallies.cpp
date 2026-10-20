/**
 * @file skmapallies.cpp
 * @brief Mini-map ally rules: the SLMC faction table, candidate groups, probe records and budget.
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

#include "linden_common.h"

#include "skmapallies.h"

#include "llfile.h"
#include "llsdserialize.h"

#include <algorithm>

namespace skmap
{
    namespace
    {
        void addIds(const LLSD& value, std::set<LLUUID>& out)
        {
            if (value.isArray())
            {
                for (LLSD::array_const_iterator it = value.beginArray(); it != value.endArray(); ++it)
                {
                    addIds(*it, out);
                }
                return;
            }
            LLUUID id;
            if (value.isUUID())
            {
                id = value.asUUID();
            }
            else if (value.isString() && LLUUID::validate(value.asString()))
            {
                id.set(value.asString());
            }
            if (id.notNull())
            {
                out.insert(id);
            }
        }
    }

    FactionTable FactionTable::fromLLSD(const LLSD& sd)
    {
        FactionTable table;
        const LLSD& factions = sd["factions"];
        if (!factions.isArray())
        {
            return table;
        }
        for (LLSD::array_const_iterator it = factions.beginArray(); it != factions.endArray(); ++it)
        {
            const LLSD& entry = *it;
            if (!entry.isMap())
            {
                continue;
            }
            Faction faction;
            faction.mName = entry["name"].asString();
            faction.mShort = entry["short"].asString();
            for (const char* key : { "g", "m", "l" })
            {
                if (entry.has(key))
                {
                    addIds(entry[key], faction.mGroups);
                }
            }
            if (!faction.mGroups.empty())
            {
                table.mFactions.push_back(faction);
            }
        }
        return table;
    }

    bool FactionTable::load(const std::string& path)
    {
        llifstream file(path.c_str());
        if (!file.is_open())
        {
            return false;
        }
        LLSD sd;
        if (LLSDSerialize::fromXML(sd, file) == LLSDParser::PARSE_FAILURE)
        {
            return false;
        }
        FactionTable table = fromLLSD(sd);
        if (table.empty())
        {
            return false;
        }
        *this = table;
        return true;
    }

    const Faction* FactionTable::factionOf(const LLUUID& group) const
    {
        for (const Faction& faction : mFactions)
        {
            if (faction.mGroups.count(group))
            {
                return &faction;
            }
        }
        return nullptr;
    }

    std::set<LLUUID> FactionTable::candidatesFor(const LLUUID& active_group) const
    {
        if (active_group.isNull())
        {
            return {};
        }
        if (const Faction* faction = factionOf(active_group))
        {
            return faction->mGroups;
        }
        return { active_group };
    }

    void observe(AllyRecord& record, const std::string& title, const std::vector<LLUUID>& roots, F64 now, bool probe_untitled)
    {
        record.mLastSeen = now;
        const bool untitled = title.empty() && !probe_untitled;
        if (!record.mSeenTitle)
        {
            record.mSeenTitle = true;
            record.mTitle = title;
            record.mSignature = roots;
            if (untitled)
            {
                record.mState = ProbeState::UNTITLED;
            }
            return;
        }
        if (title != record.mTitle)
        {
            // The tag switched: the group it implied is gone at once, and the attachments are
            // asked again, never answered from the cache that holds the old group.
            record.mTitle = title;
            record.mGroup.setNull();
            record.mInFlight = false;
            if (untitled)
            {
                record.mState = ProbeState::UNTITLED;
                record.mNeedProbe = false;
            }
            else
            {
                record.mState = ProbeState::UNKNOWN;
                record.mNeedProbe = true;
                record.mFresh = true;
                record.mRetryAt = now;
            }
        }
        else if (record.mState == ProbeState::UNTITLED && !untitled)
        {
            record.mState = ProbeState::UNKNOWN;
            record.mRetryAt = now;
        }
        if (roots != record.mSignature && (record.mState == ProbeState::RESOLVED || record.mState == ProbeState::NONE))
        {
            record.mNeedProbe = true;
        }
    }

    bool wantsProbe(const AllyRecord& record, F64 now, bool has_roots)
    {
        if (!has_roots || record.mInFlight)
        {
            return false;
        }
        switch (record.mState)
        {
            case ProbeState::UNKNOWN:
                return now >= record.mRetryAt;
            case ProbeState::NONE:
                return record.mNeedProbe || now >= record.mRetryAt;
            case ProbeState::RESOLVED:
                return record.mNeedProbe;
            default:
                return false;
        }
    }

    LLUUID chooseTarget(const std::vector<LLUUID>& previous, const std::vector<LLUUID>& roots)
    {
        for (const LLUUID& root : roots)
        {
            if (std::find(previous.begin(), previous.end(), root) == previous.end())
            {
                return root;
            }
        }
        return roots.empty() ? LLUUID::null : roots.front();
    }

    void probeSent(AllyRecord& record, const LLUUID& target, const std::vector<LLUUID>& roots, F64 now)
    {
        record.mInFlight = true;
        record.mTarget = target;
        record.mSentAt = now;
        record.mSignature = roots;
        record.mNeedProbe = false;
        record.mFresh = false;
        if (record.mState != ProbeState::RESOLVED)
        {
            record.mState = ProbeState::PENDING;
        }
    }

    void probeReply(AllyRecord& record, const LLUUID& root, const LLUUID& group, F64 now)
    {
        if (!record.mInFlight || root != record.mTarget)
        {
            return;
        }
        record.mInFlight = false;
        if (group.isNull())
        {
            record.mState = ProbeState::NONE;
            record.mGroup.setNull();
            record.mRetryAt = now + PROBE_RETRY_S;
        }
        else
        {
            record.mState = ProbeState::RESOLVED;
            record.mGroup = group;
        }
    }

    void probeTimeout(AllyRecord& record, F64 now)
    {
        if (!record.mInFlight || now - record.mSentAt < PROBE_TIMEOUT_S)
        {
            return;
        }
        record.mInFlight = false;
        if (record.mState == ProbeState::PENDING)
        {
            record.mState = ProbeState::UNKNOWN;
            record.mRetryAt = now + PROBE_RETRY_S;
        }
    }

    AllyVerdict allyVerdict(const AllyRecord& record, const std::set<LLUUID>& candidates, bool roster_hit)
    {
        if (roster_hit)
        {
            return AllyVerdict::ALLY;
        }
        if (record.mState == ProbeState::RESOLVED && candidates.count(record.mGroup))
        {
            return AllyVerdict::ALLY;
        }
        return AllyVerdict::UNKNOWN;
    }

    void ProbeBudget::refill(F64 now)
    {
        if (mLastRefill < 0.0)
        {
            mLastRefill = now;
        }
        const F64 cap = llmax(1.0, static_cast<F64>(mRate));
        mTokens = llmin(cap, mTokens + (now - mLastRefill) * mRate);
        mLastRefill = now;
    }

    bool ProbeBudget::enqueue(const LLUUID& id)
    {
        if (mQueue.size() >= static_cast<size_t>(PROBE_PENDING) || mInFlight.count(id)
            || std::find(mQueue.begin(), mQueue.end(), id) != mQueue.end())
        {
            return false;
        }
        mQueue.push_back(id);
        return true;
    }

    LLUUID ProbeBudget::next(F64 now)
    {
        refill(now);
        if (mTokens < 1.0 || mInFlight.size() >= static_cast<size_t>(PROBE_IN_FLIGHT) || mQueue.empty())
        {
            return LLUUID::null;
        }
        const LLUUID id = mQueue.front();
        mQueue.pop_front();
        return id;
    }

    void ProbeBudget::sent(const LLUUID& id, F64 now)
    {
        refill(now);
        mTokens -= 1.0;
        mInFlight[id] = now;
    }

    void ProbeBudget::done(const LLUUID& id)
    {
        mInFlight.erase(id);
    }

    std::vector<LLUUID> ProbeBudget::expire(F64 now)
    {
        std::vector<LLUUID> out;
        for (auto it = mInFlight.begin(); it != mInFlight.end();)
        {
            if (now - it->second >= PROBE_TIMEOUT_S)
            {
                out.push_back(it->first);
                it = mInFlight.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return out;
    }

    void ProbeBudget::clear()
    {
        mInFlight.clear();
        mQueue.clear();
    }
}
