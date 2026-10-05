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

#include "llquaternion.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v4color.h"

#include "skmapallies.h"
#include "skmapallyresolver.h"
#include "skmapgeom.h"
#include "skmappolicy.h"

#include <map>
#include <vector>

/// A vehicle's captured outline, keyed by its root. A seam for the outline capture module.
class SKMapVehicleGlyphs
{
public:
    struct Glyph;
    /// The outline captured for a vehicle, or null: none are captured yet, so the box draws.
    static const Glyph* glyphFor(const LLUUID& root) { return nullptr; }
};

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
        bool mAlly = false;
        skmap::Visibility mVisibility = skmap::Visibility::DOT;
    };

    void initSingleton() override;
    void cleanupSingleton() override;

    /// Everyone but the agent the map may show, friends last. In combat that is allies only.
    const std::vector<Person>& people() const { return mPeople; }

    struct Vehicle
    {
        LLUUID mRoot;
        LLVector3d mPos;
        LLQuaternion mRot;
        LLVector3 mMin, mMax;               ///< the linkset's box in the root's frame
        std::vector<LLUUID> mCrew;          ///< seated avatars the map may show
        skmap::VehicleLook mLook = skmap::VehicleLook::CIVILIAN;
    };
    /// Linksets the census has seen driven this session, at their live pose.
    const std::vector<Vehicle>& vehicles() const { return mVehicles; }
    /// Whether the agent stands where it can be hurt, or the debug switch forces it.
    bool combat() const { return mCombat; }
    /// The user's ally groups: the active group's faction, or the group alone.
    const std::set<LLUUID>& candidates() const { return mCandidates; }
    /// The cover a wall ahead of the agent along the camera gives, at 10 Hz; NONE when off.
    sknav::Cover coverAhead() const { return mCoverAhead; }

    /// A map drew this frame: the idle work runs while one has, recently.
    void noteDrawn();
    bool active() const;

    static void onIdle(void*);

private:
    void idle();
    void gatherPeople();
    void updateCoverAhead();
    void gatherVehicles();
    void updateCombat();
    void updateCandidates();
    void loadFactions();

    std::vector<Person> mPeople;
    F64 mLastDrawn = -1.0e9;
    sknav::Cover mCoverAhead = sknav::Cover::NONE;
    F64 mCoverAt = -1.0e9;
    std::vector<Vehicle> mVehicles;
    /// Seat roots of everyone near, shown or not: whether a vehicle is crewed, and by an ally.
    std::map<LLUUID, std::pair<bool, bool>> mSeats;
    bool mCombat = false;
    SKMapAllyResolver mResolver;
    skmap::FactionTable mFactions;
    bool mFactionsLoaded = false;
    LLUUID mActiveGroup;
    bool mGroupKnown = false;
    std::set<LLUUID> mCandidates;
    std::vector<LLUUID> mVehicleRoots;
    U64 mVehicleStamp = 0;
    /// Linkset boxes by root, kept while the child set holds.
    struct BoxCache
    {
        U64 mKey = 0;
        LLVector3 mMin, mMax;
    };
    std::map<LLUUID, BoxCache> mBoxes;
};

#endif // SK_MAPPEOPLE_H
