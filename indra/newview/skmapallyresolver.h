/**
 * @file skmapallyresolver.h
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

#ifndef SK_MAPALLYRESOLVER_H
#define SK_MAPALLYRESOLVER_H

#include "lluuid.h"

#include "skmapallies.h"

#include <boost/signals2.hpp>
#include <map>
#include <memory>
#include <set>
#include <vector>

class LLVOAvatar;

/// Main thread only, a member of SKMapPeople. Asks for nothing unless the map is shown in combat.
class SKMapAllyResolver
{
public:
    SKMapAllyResolver();
    ~SKMapAllyResolver();

    /// Once an idle: active is combat with a map shown; candidates are the user's ally groups.
    void update(bool active, const std::set<LLUUID>& candidates);
    /// ALLY or UNKNOWN, never anything else.
    skmap::AllyVerdict verdict(const LLUUID& id) const;
    /// Forget every probe record.
    void clear();

    /// A loaded avatar seen this idle, nearest first; in_view says the map shows it (with 10%
    /// to spare), which is where probes are spent.
    void noteAvatar(LLVOAvatar* avatar, bool in_view, F64 now);
    /// After the avatars are noted: send what the budget allows.
    void dispatch(F64 now);
    /// The user teleported: every record goes.
    void forgetRecords();
    U32 probesInFlight() const { return static_cast<U32>(mBudget.inFlight()); }
    U32 probesQueued() const { return static_cast<U32>(mBudget.queued()); }

private:
    /// An object's properties came in; only a root we asked about is read.
    void onProperties(const LLUUID& object);

    std::set<LLUUID> mCandidates;
    bool mActive = false;
    std::map<LLUUID, skmap::AllyRecord> mRecords;
    std::map<LLUUID, std::vector<LLUUID>> mRoots;   ///< per avatar, as last noted
    std::map<LLUUID, LLUUID> mPendingRoots;         ///< root asked -> its wearer
    std::set<LLUUID> mProbedRoots;                  ///< roots asked about before: the cache no longer answers them
    skmap::ProbeBudget mBudget;
    boost::signals2::connection mPropertiesConnection;
};

#endif // SK_MAPALLYRESOLVER_H
