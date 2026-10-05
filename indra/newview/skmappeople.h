/**
 * @file skmappeople.h
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

#ifndef SK_MAPPEOPLE_H
#define SK_MAPPEOPLE_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v4color.h"

#include "skmapgeom.h"

#include <vector>

/// Main thread only. Does nothing while no map has drawn for a couple of seconds.
class SKMapPeople : public LLSingleton<SKMapPeople>
{
    LLSINGLETON(SKMapPeople);
    ~SKMapPeople() override;

public:
    struct Person
    {
        LLUUID mId;
        LLVector3d mPos;
        bool mLoaded = false;               ///< a full object, not a coarse location
        bool mZKnown = true;                ///< false when both it and the camera sit at the coarse ceiling
        bool mHasHeading = false;
        F32 mHeading = 0.f;                 ///< radians from east, counter-clockwise
        bool mSitting = false;
        LLUUID mSeatRoot;
        bool mFriend = false;
        LLColor4 mColour;                   ///< the general dot colour
        skmap::FloorRelation mFloor = skmap::FloorRelation::SAME;
    };

    void initSingleton() override;
    void cleanupSingleton() override;

    /// Everyone but the agent, friends last.
    const std::vector<Person>& people() const { return mPeople; }
    /// Whether the agent stands where it can be hurt; general mode until the combat commit.
    bool combat() const { return false; }

    /// A map drew this frame: the idle work runs while one has, recently.
    void noteDrawn();
    bool active() const;

    static void onIdle(void*);

private:
    void idle();
    void gatherPeople();

    std::vector<Person> mPeople;
    F64 mLastDrawn = -1.0e9;
};

#endif // SK_MAPPEOPLE_H
