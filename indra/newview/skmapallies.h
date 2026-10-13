/**
 * @file skmapallies.h
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

#ifndef SK_MAPALLIES_H
#define SK_MAPALLIES_H

#include "llsd.h"
#include "lluuid.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace skmap
{
    /// One SLMC faction: its main, militia and land groups. Civilian groups are not part of it.
    struct Faction
    {
        std::string mName;
        std::string mShort;
        std::set<LLUUID> mGroups;
    };

    /// The faction table shipped as data. Each faction's g, m and l keys hold a uuid, a string
    /// or an array of either; null and invalid ids are dropped, and no other key is read.
    class FactionTable
    {
    public:
        static FactionTable fromLLSD(const LLSD& sd);
        /// False when the file is missing, does not parse or holds no faction.
        bool load(const std::string& path);

        const std::vector<Faction>& factions() const { return mFactions; }
        bool empty() const { return mFactions.empty(); }
        /// The faction holding the group, or null.
        const Faction* factionOf(const LLUUID& group) const;
        /// The user's active group's candidates: nothing for none, its faction's whole set when
        /// it is in one, else just itself.
        std::set<LLUUID> candidatesFor(const LLUUID& active_group) const;

    private:
        std::vector<Faction> mFactions;
    };

    constexpr F32 PROBE_RATE = 4.f;             ///< requests a second
    constexpr S32 PROBE_IN_FLIGHT = 4;
    constexpr F32 PROBE_TIMEOUT_S = 5.f;
    constexpr S32 PROBE_PENDING = 64;
    constexpr F32 PROBE_RETRY_S = 30.f;
    constexpr F32 RECORD_FORGET_S = 600.f;

    /// What the attachment probe knows about one avatar's active group. There is no enemy.
    enum class ProbeState : U8 { UNKNOWN, UNTITLED, PENDING, RESOLVED, NONE };

    struct AllyRecord
    {
        ProbeState mState = ProbeState::UNKNOWN;
        LLUUID mGroup;                          ///< RESOLVED only
        std::string mTitle;                     ///< the nametag title at the last probe, or as first seen
        bool mSeenTitle = false;
        std::vector<LLUUID> mSignature;         ///< the attachment roots, sorted
        std::set<LLUUID> mAsked;                ///< roots asked since the title last changed
        bool mInFlight = false;
        LLUUID mTarget;                         ///< the root asked
        F64 mSentAt = 0.0;
        F64 mRetryAt = 0.0;
        F64 mLastSeen = 0.0;
        bool mNeedProbe = false;                ///< a title or signature change asks again
        bool mFresh = false;                    ///< the next request must not be answered from the cache
    };

    /// Note what is seen of the avatar now: its title and its attachment roots. A title change
    /// clears the group at once and asks again with a fresh request; a new root set re-probes
    /// while a resolved group holds.
    void observe(AllyRecord& record, const std::string& title, const std::vector<LLUUID>& roots, F64 now, bool probe_untitled);
    /// Whether to send a probe now.
    bool wantsProbe(const AllyRecord& record, F64 now, bool has_roots);
    /// The root to ask: the first not asked yet, else the first.
    LLUUID chooseTarget(const std::set<LLUUID>& asked, const std::vector<LLUUID>& roots);
    /// A request went out to target, with the roots it was chosen from.
    void probeSent(AllyRecord& record, const LLUUID& target, const std::vector<LLUUID>& roots, F64 now);
    /// The answer for root came in; one for a root not asked changes nothing.
    void probeReply(AllyRecord& record, const LLUUID& root, const LLUUID& group, F64 now);
    /// After a reply that is no candidate: ask the next root at once while any is left, since one
    /// attachment rezzed under another group must not hide an ally.
    void askNextRoot(AllyRecord& record, const std::vector<LLUUID>& roots, const std::set<LLUUID>& candidates);
    /// Give up on a request older than the timeout.
    void probeTimeout(AllyRecord& record, F64 now);

    enum class AllyVerdict : U8 { UNKNOWN, ALLY };
    /// ALLY only when the active group read off the attachments is a candidate; membership alone is not enough.
    AllyVerdict allyVerdict(const AllyRecord& record, const std::set<LLUUID>& candidates);

    /// The probe's rate limit: a token bucket, an in-flight cap with a timeout, and a bounded queue.
    class ProbeBudget
    {
    public:
        void setRate(F32 per_second) { mRate = per_second; }
        /// Queue an avatar for a probe; false when the queue is full or it is queued already.
        bool enqueue(const LLUUID& id);
        /// The next queued avatar a request may go out for now, or null.
        LLUUID next(F64 now);
        void sent(const LLUUID& id, F64 now);
        void done(const LLUUID& id);
        /// The requests older than the timeout, dropped.
        std::vector<LLUUID> expire(F64 now);
        size_t inFlight() const { return mInFlight.size(); }
        size_t queued() const { return mQueue.size(); }
        void clear();

    private:
        void refill(F64 now);

        F32 mRate = PROBE_RATE;
        F64 mTokens = PROBE_RATE;
        F64 mLastRefill = -1.0;
        std::map<LLUUID, F64> mInFlight;
        std::deque<LLUUID> mQueue;
    };
}

#endif // SK_MAPALLIES_H
