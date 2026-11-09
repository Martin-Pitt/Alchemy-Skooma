/**
 * @file skterraincapture.h
 * @brief Copies the live regions' ground into the terrain cache as it arrives.
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


#ifndef SK_SKTERRAINCAPTURE_H
#define SK_SKTERRAINCAPTURE_H

#include "llsingleton.h"

#include <boost/signals2.hpp>

#include <vector>

class LLEventTimer;
class LLViewerRegion;

// Watches each region's land patches; once a region's have stopped changing for a moment, its heights
// are copied on the main thread and handed to SKTerrainCache, which encodes them on its worker.
// A region about to be removed is captured then if it changed since its last capture.
class SKTerrainCapture : public LLSingleton<SKTerrainCapture>
{
    LLSINGLETON(SKTerrainCapture);
    ~SKTerrainCapture() override;

public:
    void start();

private:
    struct Watched
    {
        U64 mHandle      = 0;
        U64 mUpdates     = 0; // sum of the patches' update counts last seen
        U64 mCaptured    = 0; // the sum last captured
        F64 mChangedAt   = 0.0;
    };

    void tick();
    void capture(LLViewerRegion* regionp, Watched& watched);
    void onRegionRemoved(LLViewerRegion* regionp);
    Watched& watch(U64 handle);

    std::vector<Watched>                mWatched;
    LLEventTimer*                       mTimer = nullptr;
    boost::signals2::scoped_connection  mRemovedConnection;
};

#endif // SK_SKTERRAINCAPTURE_H
