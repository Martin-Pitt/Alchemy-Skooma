/**
 * @file skoverviewmovers.h
 * @brief Moving objects and vehicles for the overview's camera view, at their live pose.
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

#ifndef SK_OVERVIEWMOVERS_H
#define SK_OVERVIEWMOVERS_H

#include "llmatrix4a.h"
#include "llpointer.h"
#include "llquaternion.h"
#include "llrect.h"
#include "llvolume.h"
#include "lluuid.h"
#include "v3dmath.h"

#include <map>
#include <set>
#include <vector>

class LLVertexBuffer;
class LLViewerObject;
class LLVolume;

namespace skoverview
{
    /// How a vehicle is styled: not drawn as one at all, in the movers' colour, or in one of two hull colours whose
    /// meaning the mover source picks.
    enum class VehicleLook : U8 { HIDDEN, PLAIN, PRIMARY, SECONDARY };

    /// A linkset driven as a vehicle, at its live pose.
    struct Vehicle
    {
        LLUUID mRoot;
        LLVector3d mPos;
        LLQuaternion mRot;
        LLVector3 mMin, mMax;               ///< the linkset's box in the root's frame
        VehicleLook mLook = VehicleLook::PLAIN;
    };

    /// Who knows the vehicles and whose they are: the people layer above the overview registers one. Main thread.
    class MoverSource
    {
    public:
        virtual ~MoverSource() = default;
        /// The vehicles the source tracks this frame, with their looks.
        virtual void vehicles(std::vector<Vehicle>& out) const = 0;
        /// The look of a linkset the source does not track that someone sits on (crew, never empty) or that sits on
        /// click (crew empty).
        virtual VehicleLook lookOf(const std::vector<LLUUID>& crew) const = 0;
    };
}

/// What moves: physical objects, objects the census saw move (keyframed motion among them), elevator cars, and
/// vehicles. The static census leaves them all out. Vehicles are the mover source's own plus any moving linkset
/// someone sits on or that sits on click, styled by the source; with no source every one is plain, drawn as any mover
/// is. Every mover, vehicles included, is drawn from its rendered faces at its live pose and true size, whole, never
/// cut by the floor cut; vehicles in the hull colour of their look. Main thread only.
class SKOverviewMovers
{
public:
    SKOverviewMovers();
    ~SKOverviewMovers();

    /// The source of the vehicles and their looks, or none; the caller keeps it alive until it sets another.
    void setSource(const skoverview::MoverSource* source) { mSource = source; }

    /// Who moves and where, this frame. True when anything is to be drawn.
    bool update();
    /// Single parts to draw live as well: those the census found restless (SKOverviewCensus::restlessParts).
    void setLoose(std::vector<LLUUID> parts) { mLoose = std::move(parts); }
    /// Draws every mover part with the bound geometry program, setting each part's class code (diffuse colour) and
    /// loading view * model; the caller has set the target and the state.
    void draw(const LLMatrix4a& view) const;
    /// The window rect, w by h pixels, the parts' boxes project to, clamped; false when none is in front of the eye.
    bool screenBounds(const LLMatrix4a& view, const LLMatrix4a& proj, S32 w, S32 h, LLRect& rect) const;
    /// The window rect, w by h pixels, an agent-space box projects to through view_proj, clamped; the whole view for a
    /// box across the eye's plane, false for one wholly behind it.
    static bool screenRectOf(const LLMatrix4a& view_proj, const LLVector4a& lo, const LLVector4a& hi, S32 w, S32 h, LLRect& rect);
    /// The vehicles, live, with their looks; hidden ones are left out.
    const std::vector<skoverview::Vehicle>& vehicles() const { return mVehicles; }
    void release();

private:
    struct Mesh
    {
        LLVolumeParams mParams;  ///< the shape the buffer was built from at its top LOD, whatever LOD the world draws it at
        bool mBuilt = false;     ///< false until a build works, so the next update tries again
        LLPointer<LLVertexBuffer> mBuffer;
        U32 mCount = 0;
    };
    struct Part
    {
        LLUUID mId;
        LLMatrix4a mModel;      ///< volume space to agent space
        U8 mCode = 0;           ///< the class code it draws with (skoverviewcensus.h)
        LLVector4a mExtents[2]; ///< its box at its live pose, agent space
    };
    /// Adds a linkset's parts at their live pose.
    void addParts(LLViewerObject* root, U8 code, std::set<LLUUID>& live);
    /// Adds one part at its live pose, once.
    void addPart(LLViewerObject* object, U8 code, std::set<LLUUID>& live);
    std::vector<LLUUID> mLoose;

    std::map<LLUUID, Mesh> mMeshes;
    std::vector<Part> mParts;
    std::vector<skoverview::Vehicle> mVehicles;
    const skoverview::MoverSource* mSource = nullptr;
    std::vector<LLUUID> mCandidates;    ///< roots that move, refreshed per census scan
    U64 mStamp = 0;
};

#endif // SK_OVERVIEWMOVERS_H
