/**
 * @file skworldmapsearch.h
 * @brief The 3D world map's region search: a box under the map, its results above it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#pragma once

#include "llpanel.h"
#include "v3dmath.h"

#include <string>
#include <vector>

class LLLineEditor;
class LLScrollListCtrl;

/// A search box at the foot of the map with its results above it, so the box never moves as they change.
/// Region names the map knows are matched fuzzily as you type; a name request to the grid adds the rest.
/// Up and Down pick a result, Enter or a click flies the map there, Escape clears.
class SKWorldMapSearch : public LLPanel
{
public:
    SKWorldMapSearch();

    bool postBuild() override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;

private:
    struct Hit
    {
        std::string mName;
        LLVector3d  mCentre;
        F32         mScore;
        F64         mDistance;
    };

    void onKeystroke();
    void refilter();
    void showHits();
    void select(S32 hit);
    void go(S32 hit);

    LLLineEditor*     mInput = nullptr;
    LLScrollListCtrl* mResults = nullptr;
    S32               mResultsBottom = 0;  ///< the list grows up from here
    std::string       mQuery;              ///< lower case
    std::string       mRequested;          ///< the last query asked of the grid
    F64               mTypedAt = 0.0;
    size_t            mKnownRegions = 0;   ///< the map's region count at the last filter; replies move it
    std::vector<Hit>  mHits;               ///< best first
    S32               mSelected = 0;
};
