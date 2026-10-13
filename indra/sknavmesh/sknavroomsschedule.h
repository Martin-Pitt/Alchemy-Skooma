/**
 * @file sknavroomsschedule.h
 * @brief When the per-mesh rooms pass runs: an old layout always beats none.
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

#ifndef SK_NAVROOMSSCHEDULE_H
#define SK_NAVROOMSSCHEDULE_H

#include "stdtypes.h"

namespace sknav
{
    /// What the scheduler knows of one mesh's rooms pass.
    struct RoomsSchedule
    {
        bool mPublished = false;    ///< it has landed a product
        bool mDirty = false;        ///< its air or doors moved since the last post
        bool mInFlight = false;
        F64 mChanged = 0.0;         ///< when its air last moved, seconds
        F64 mDirtySince = 0.0;      ///< when it last went dirty
        F64 mLastPosted = 0.0;
        U8 mPriority = 3;           ///< the navmesh's order: the agent's region first
    };

    /// A refresh waits at most this many settle periods under churn.
    constexpr F64 ROOMS_MAX_SETTLES = 4.0;

    /// Whether a pass may post now. A mesh never published runs at once on
    /// whatever air it has; churn only delays a refresh, and only boundedly.
    inline bool roomsReady(const RoomsSchedule& s, F64 now, F64 settle_s)
    {
        if (!s.mDirty || s.mInFlight)
        {
            return false;
        }
        return !s.mPublished || now - s.mChanged >= settle_s || now - s.mDirtySince >= ROOMS_MAX_SETTLES * settle_s;
    }

    /// Posting order: first publications, then the navmesh's order, then the longest unserved.
    inline bool roomsBefore(const RoomsSchedule& a, const RoomsSchedule& b)
    {
        if (a.mPublished != b.mPublished)
        {
            return !a.mPublished;
        }
        if (a.mPriority != b.mPriority)
        {
            return a.mPriority < b.mPriority;
        }
        return a.mLastPosted < b.mLastPosted;
    }

    /// Whether a better tier's pass may cancel this one mid-flight: never a first publication.
    inline bool roomsPreemptible(const RoomsSchedule& s, U8 best)
    {
        return s.mInFlight && s.mPublished && s.mPriority > best;
    }
}

#endif // SK_NAVROOMSSCHEDULE_H
