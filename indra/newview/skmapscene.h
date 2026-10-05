/**
 * @file skmapscene.h
 * @brief The mini-map floorplan: navmesh demand, the scene worker, cell bakes and their draw.
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

#ifndef SK_MAPSCENE_H
#define SK_MAPSCENE_H

#include "llsingleton.h"
#include "threadpool.h"

#include "skmapgeom.h"

#include <memory>

class SKNetMap;

/// Main thread only; scene analysis runs on a one-thread pool of its own. Costs nothing while
/// no map has drawn for a couple of seconds.
class SKMapScene : public LLSingleton<SKMapScene>
{
    LLSINGLETON(SKMapScene);
    ~SKMapScene() override;

public:
    void initSingleton() override;
    void cleanupSingleton() override;

    /// A map drew this frame: hold the navmesh demand, and follow its centre and shape.
    void noteDrawn(const SKNetMap& map);
    bool active() const;

    /// The latest scene, or null before the first lands.
    skmap::ScenePtr scene() const { return mScene; }
    /// Whether a 32 m lattice cell holds a published navmesh tile.
    bool built(S32 x, S32 y) const;

    struct Stats
    {
        U32 mJobs = 0;
        F32 mJobMs = 0.f;
        U32 mPolys = 0;
        U32 mLayers = 0;
    };
    const Stats& stats() const { return mStats; }

    static void onIdle(void*);

private:
    void idle();
    void updateDemand(bool want);
    void maybePost();
    void gather(skmap::SceneInput& in) const;
    void land(const skmap::ScenePtr& scene, U32 generation, F32 millis);

    /// What a scene was built from: a new job goes out when any of it moves.
    struct Trigger
    {
        LLVector3d mMin, mMax;
        U64 mNavRevision = 0;
        U64 mViewRevision = 0;
        U64 mCensus = 0;
        U64 mRegions = 0;
        bool operator==(const Trigger& o) const
        {
            return mMin == o.mMin && mMax == o.mMax && mNavRevision == o.mNavRevision && mViewRevision == o.mViewRevision
                && mCensus == o.mCensus && mRegions == o.mRegions;
        }
    };
    Trigger currentTrigger() const;

    std::unique_ptr<LL::ThreadPool> mPool;
    std::shared_ptr<bool> mAlive;
    U32 mGeneration = 0;
    bool mInFlight = false;
    F64 mLastPost = -1.0e9;
    Trigger mPosted;
    bool mHasPosted = false;
    skmap::ScenePtr mScene;
    bool mDemanding = false;
    F64 mLastDrawn = -1.0e9;
    LLVector3d mMapCentre;
    F32 mMapLong = 200.f;
    F32 mMapShort = 200.f;
    Stats mStats;
};

#endif // SK_MAPSCENE_H
