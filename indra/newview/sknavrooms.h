/**
 * @file sknavrooms.h
 * @brief The per-mesh enclosure pass, scheduled off the main thread, and the view its answers are read through.
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

#ifndef SK_NAVROOMS_H
#define SK_NAVROOMS_H

#include "lluuid.h"
#include "sknavenclosure.h"
#include "sknavspaces.h"
#include "threadpool.h"
#include "v3dmath.h"

#include <atomic>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SKNavMesh;
class SKNavOverlayGeom;

/// What the enclosure pass knows, over every mesh at once. Immutable: the
/// main thread swaps in a new one when a pass lands, and any thread may read
/// one it holds. Positions and heights are global.
class SKNavWorldView
{
public:
    enum class Channel : U8 { S, E, D, FLOOR_Z, TOP };

    struct Mesh
    {
        LLUUID mId;
        LLVector3d mOrigin;
        LLVector3d mMin;        ///< the footprint of its tiles, global
        LLVector3d mMax;
        sknav::EnclosureProductPtr mProduct;
        sknav::RoomsProductPtr mRooms;
    };

    /// A space: its mesh and its id there.
    struct SpaceId
    {
        LLUUID mMesh;
        U32 mId = 0;
        bool operator==(const SpaceId& o) const { return mMesh == o.mMesh && mId == o.mId; }
    };
    typedef std::function<void(const Mesh& mesh, const sknav::Space& space)> SpaceFn;
    typedef std::function<void(const Mesh& mesh, const sknav::Portal& portal)> PortalFn;

    /// Moves whenever a mesh's answers change.
    U64 revision() const { return mRevision; }

    /// The gap holding pos, over every mesh: an answer beats UNKNOWN, and
    /// the higher floor wins among answers. False where no mesh has air.
    bool enclosureAt(const LLVector3d& pos, sknav::Enclosure& out) const;

    /// The space holding pos, as enclosureAt finds its gap; false in none.
    bool spaceAt(const LLVector3d& pos, SpaceId& out) const;
    /// A space by id, or null. Its positions are relative to its mesh's origin.
    const sknav::Space* space(const SpaceId& id, const Mesh** mesh_out = nullptr) const;
    /// Spaces whose box, and portals whose centre, meet the global box.
    void spacesIn(const LLVector3d& min, const LLVector3d& max, const SpaceFn& fn) const;
    void portalsIn(const LLVector3d& min, const LLVector3d& max, const PortalFn& fn) const;
    void portalsOf(const SpaceId& id, const PortalFn& fn) const;

    /// The top of the highest solid over a point, from the sky map; false
    /// when nothing stands there.
    bool topOfWorld(const LLVector3d& xy, F64& top) const;

    /// Every gap over a point, per mesh and lowest first.
    void gapsAt(const LLVector3d& xy, const std::function<void(const sknav::Enclosure&)>& fn) const;

    /// A grid of res-metre samples over the box's plan, row-major along y:
    /// per sample, the highest gap whose floor lies within the box's height,
    /// read on the channel. NaN where there is no answer.
    void raster(const LLVector3d& min, const LLVector3d& max, F32 res, Channel channel, std::vector<F32>& out) const;

    const std::vector<Mesh>& meshes() const { return mMeshes; }

private:
    friend class SKNavRooms;
    U64 mRevision = 0;
    std::vector<Mesh> mMeshes;
    std::shared_ptr<const sknav::SkyMap> mSky;
};
typedef std::shared_ptr<const SKNavWorldView> SKNavWorldViewPtr;

/// Schedules the enclosure pass. A mesh whose air changed is marked dirty,
/// and once it has been quiet for SKNavRoomsSettleMs its tiles' air and the
/// sky map are snapshotted and handed to a queue of its own, one pass per
/// mesh at a time, each in 20 ms chunks so a newer generation can cancel it.
/// Meshes that touch read each other's answers one generation late, and a
/// mesh whose answers moved re-runs its neighbours, at most three times.
/// Main thread only, besides the view it hands out.
class SKNavRooms
{
public:
    SKNavRooms();
    ~SKNavRooms();

    void update(const SKNavMesh& navmesh);
    /// Join the queue; nothing lands after this.
    void close();

    SKNavWorldViewPtr view() const { return mView; }
    U64 revision() const { return mView ? mView->revision() : 0; }
    std::string statsLine() const;

    /// Tint the floor of every gap near the camera by e (mode 1) or by bucket
    /// (mode 2), from the coarse answers. Agent coordinates.
    void gatherOverlay(SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach, U32 mode, bool opaque) const;

    /// A tile's per-gap answers as a JSON member, leading comma included,
    /// when the view holds answers for exactly this air.
    void writeTile(std::ostream& file, const LLUUID& mesh, const sknav::TileKey& key, const sknav::TileAirPtr& air) const;
    /// A mesh's spaces and portals as JSON members, leading comma included, global.
    void writeSpaces(std::ostream& file, const LLUUID& mesh) const;

private:
    struct Job;
    struct MeshState
    {
        LLUUID mId;
        LLVector3d mOrigin;
        LLVector3d mMin;
        LLVector3d mMax;
        U64 mAirSignature = 0;
        U64 mDoorSignature = 0;             ///< the doors and lifts over it
        bool mDirty = false;
        bool mInFlight = false;
        F64 mChanged = 0.0;                 ///< when its air last moved
        U64 mGeneration = 0;
        U32 mReruns = 0;                    ///< neighbour-driven re-runs since its air last moved
        std::shared_ptr<std::atomic<U64>> mLatest;
        sknav::EnclosureProductPtr mProduct;
        sknav::RoomsProductPtr mRooms;
    };

    void post(MeshState& state, const SKNavMesh& navmesh);
    void runChunk(const std::shared_ptr<Job>& job, const std::weak_ptr<bool>& alive);
    void finish(const std::shared_ptr<Job>& job);
    void rebuildView();
    bool touches(const MeshState& a, const MeshState& b) const;
    static std::vector<std::pair<LLVector3d, LLVector3d>> lifts(const LLVector3d& min, const LLVector3d& max);

    std::map<LLUUID, MeshState> mMeshes;
    std::unique_ptr<LL::ThreadPool> mThread;
    std::shared_ptr<bool> mAlive;
    std::shared_ptr<const sknav::SkyMap> mSky;
    U64 mSkyVersion = ~0ull;
    U64 mSeenRevision = ~0ull;
    U64 mSeenFeatures = ~0ull;
    SKNavWorldViewPtr mView;
    U64 mViewRevision = 0;
    bool mViewDirty = false;

    // stats
    F32 mLastMs = 0.f;
    F32 mLastWallMs = 0.f;
    F32 mLastStageMs[sknav::EnclosurePass::NUM_STAGES] = {};
    F32 mLastSpacesMs = 0.f;
    F32 mLastSpaceStageMs[sknav::SpacesPass::NUM_STAGES] = {};
    U32 mPasses = 0;
    U32 mCancelled = 0;
};

#endif // SK_NAVROOMS_H
