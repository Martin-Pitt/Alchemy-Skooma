/**
 * @file skoverviewmovers.h
 * @brief Moving objects and vehicles for the tactical overview, at their live pose.
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

#ifndef SK_OVERVIEWMOVERS_H
#define SK_OVERVIEWMOVERS_H

#include "llmatrix4a.h"
#include "llpointer.h"
#include "lluuid.h"
#include "skmappeople.h"

#include <map>
#include <set>
#include <vector>

class LLVertexBuffer;
class LLViewerObject;
class LLVolume;

/// What moves: physical objects, objects the census saw move (keyframed motion among them), elevator cars, and
/// vehicles. The static census leaves them all out. Vehicles are the mini-map's own (SKMapPeople) plus any moving
/// linkset someone sits on or that sits on click, with the mini-map's look rules. Every mover, vehicles included, is
/// drawn from its rendered faces at its live pose and true size, whole, never cut by the floor cut; vehicles in the
/// mini-map's hull colour for their look. Main thread only.
class SKOverviewMovers
{
public:
    SKOverviewMovers();
    ~SKOverviewMovers();

    /// Who moves and where, this frame. True when anything is to be drawn.
    bool update();
    /// Draws every mover part with the bound geometry program, setting each part's class code (diffuse colour) and
    /// loading view * model; the caller has set the target and the state.
    void draw(const LLMatrix4a& view) const;
    /// The vehicles, live, with their looks; hidden ones are left out.
    const std::vector<SKMapPeople::Vehicle>& vehicles() const { return mVehicles; }
    void release();

private:
    struct Mesh
    {
        const LLVolume* mVolume = nullptr;  ///< what the buffer was built from; a new LOD builds again
        LLPointer<LLVertexBuffer> mBuffer;
        U32 mCount = 0;
    };
    struct Part
    {
        LLUUID mId;
        LLMatrix4a mModel;      ///< volume space to agent space
        U8 mCode = 0;           ///< the class code it draws with (skoverviewcensus.h)
    };
    /// Adds a linkset's parts at their live pose.
    void addParts(LLViewerObject* root, U8 code, std::set<LLUUID>& live);

    std::map<LLUUID, Mesh> mMeshes;
    std::vector<Part> mParts;
    std::vector<SKMapPeople::Vehicle> mVehicles;
    std::vector<LLUUID> mCandidates;    ///< roots that move, refreshed per census scan
    U64 mStamp = 0;
};

#endif // SK_OVERVIEWMOVERS_H
