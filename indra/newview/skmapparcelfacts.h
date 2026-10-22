/**
 * @file skmapparcelfacts.h
 * @brief Parcel owners for the mini-map's Moderate check, from the map's own parcel requests.
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

#ifndef SK_MAPPARCELFACTS_H
#define SK_MAPPARCELFACTS_H

#include "llsingleton.h"

#include "skmappolicy.h"

#include <boost/signals2.hpp>
#include <map>

class LLMessageSystem;
class LLViewerRegion;

/// Main thread only. Requests carry sequence ids from the map's own band, so their replies come
/// here and never touch the parcel manager's hover, selected, agent or collision parcel.
class SKMapParcelFacts : public LLSingleton<SKMapParcelFacts>
{
    LLSINGLETON(SKMapParcelFacts);
    ~SKMapParcelFacts() override;

public:
    void initSingleton() override;
    void cleanupSingleton() override;

    /// The parcel the sim said holds this 4 m cell, or null.
    const skmap::ParcelFacts* find(const LLUUID& region, S32 x, S32 y) const;
    /// Ask the sim about the parcel holding the cell. Returns the request's sequence id, the one
    /// already in flight for the same cell, or 0 when too many are in flight.
    S32 request(LLViewerRegion* region, S32 x, S32 y);
    /// Whether a request still waits for its reply; false after 5 s.
    bool pending(S32 seq) const;

    /// Called from LLViewerParcelMgr::processParcelProperties for every reply. True when the id
    /// is in the map's band: the reply is the map's, whatever becomes of it.
    static bool handleParcelProperties(LLMessageSystem* msg, S32 seq, S32 result);

    static void onIdle(void*);

private:
    void expire();
    void take(LLMessageSystem* msg, S32 seq, S32 result);

    struct Pending
    {
        LLUUID mRegion;
        S32 mX = 0;
        S32 mY = 0;
        F64 mSent = 0.0;
    };
    std::map<S32, Pending> mPending;
    S32 mLastSeq = 0;
    skmap::ParcelFactsCache mCache;
    boost::signals2::connection mRegionRemoved;
};

#endif // SK_MAPPARCELFACTS_H
