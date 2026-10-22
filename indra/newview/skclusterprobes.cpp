/**
 * @file skclusterprobes.cpp
 * @brief Fill reflection probes over the census's clusters, between the default probe and the rest.
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

#include "llviewerprecompiledheaders.h"

#include "skclusterprobes.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llreflectionmap.h"
#include "llreflectionmapmanager.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "sknavcluster.h"
#include "sknavprobes.h"
#include "skworldcensus.h"

#include <algorithm>

namespace
{
    // Clusters this far from the camera are considered; the budget trims the rest.
    constexpr F64 REACH_M = 160.0;
    constexpr F32 UPDATE_PERIOD_S = 1.f;
    // A cluster smaller than this along every axis is left to the room and octree probes.
    constexpr F64 MIN_EXTENT_M = 6.0;
    constexpr U32 MAX_CELLS = 3;
    // sphereWeight's distance weight reaches 1 out to about 0.88 of the radius, so a cell's
    // farthest corner sits inside that and the cell is covered in full.
    constexpr F32 FULL_COVER_SHARE = 0.85f;
    constexpr F32 MIN_RADIUS_M = 8.f;
    constexpr F32 MAX_RADIUS_M = 96.f;
    // A held probe within this of a candidate keeps its capture.
    constexpr F64 MATCH_M = 1.0;
    constexpr F32 MATCH_RADIUS_SHARE = 0.1f;
    constexpr F64 LIFT_STEP_M = 1.0;

    struct Candidate
    {
        LLVector3d mOrigin;
        F32 mRadius = 0.f;
        F64 mDistance = 0.0;
    };

    U32 cellsAlong(F64 extent, F64 cell_m)
    {
        return static_cast<U32>(llclamp(static_cast<S32>(std::ceil(extent / llmax(cell_m, 1.0))), 1, static_cast<S32>(MAX_CELLS)));
    }

    // One candidate per occupied cell of the cluster's footprint grid.
    void placeCluster(const sknav::Cluster& cluster, const sknav::SolidLattice* solids, const LLVector3d& camera,
                      F64 cell_m, F64 height_share, std::vector<Candidate>& out)
    {
        const LLVector3d extent = cluster.mMax - cluster.mMin;
        const U32 nx = cellsAlong(extent.mdV[VX], cell_m);
        const U32 ny = cellsAlong(extent.mdV[VY], cell_m);
        const F64 sx = extent.mdV[VX] / nx;
        const F64 sy = extent.mdV[VY] / ny;
        for (U32 ix = 0; ix < nx; ++ix)
        {
            for (U32 iy = 0; iy < ny; ++iy)
            {
                const F64 cx0 = cluster.mMin.mdV[VX] + sx * ix;
                const F64 cy0 = cluster.mMin.mdV[VY] + sy * iy;
                const F64 cx1 = cx0 + sx;
                const F64 cy1 = cy0 + sy;
                // What the cell actually holds: the members' bounds clipped to it.
                LLVector3d lo(DBL_MAX, DBL_MAX, DBL_MAX);
                LLVector3d hi(-DBL_MAX, -DBL_MAX, -DBL_MAX);
                for (const sknav::RecordPtr& r : cluster.mRecords)
                {
                    if (r->mMax.mdV[VX] < cx0 || r->mMin.mdV[VX] > cx1 || r->mMax.mdV[VY] < cy0 || r->mMin.mdV[VY] > cy1)
                    {
                        continue;
                    }
                    lo.mdV[VX] = llmin(lo.mdV[VX], llmax(r->mMin.mdV[VX], cx0));
                    lo.mdV[VY] = llmin(lo.mdV[VY], llmax(r->mMin.mdV[VY], cy0));
                    lo.mdV[VZ] = llmin(lo.mdV[VZ], r->mMin.mdV[VZ]);
                    hi.mdV[VX] = llmax(hi.mdV[VX], llmin(r->mMax.mdV[VX], cx1));
                    hi.mdV[VY] = llmax(hi.mdV[VY], llmin(r->mMax.mdV[VY], cy1));
                    hi.mdV[VZ] = llmax(hi.mdV[VZ], r->mMax.mdV[VZ]);
                }
                if (lo.mdV[VX] > hi.mdV[VX])
                {
                    continue; // an empty corner of an L-shaped cluster
                }
                Candidate c;
                c.mOrigin.set((lo.mdV[VX] + hi.mdV[VX]) * 0.5, (lo.mdV[VY] + hi.mdV[VY]) * 0.5,
                              lo.mdV[VZ] + (hi.mdV[VZ] - lo.mdV[VZ]) * height_share);
                // A capture from inside a wall sees nothing; rise until clear, past the top if need be.
                if (solids)
                {
                    const F64 ceiling = hi.mdV[VZ] + 4.0;
                    while (c.mOrigin.mdV[VZ] < ceiling && solids->contains(c.mOrigin))
                    {
                        c.mOrigin.mdV[VZ] += LIFT_STEP_M;
                    }
                }
                F64 reach2 = 0.0;
                for (U32 corner = 0; corner < 8; ++corner)
                {
                    const LLVector3d p((corner & 1) ? hi.mdV[VX] : lo.mdV[VX], (corner & 2) ? hi.mdV[VY] : lo.mdV[VY],
                                       (corner & 4) ? hi.mdV[VZ] : lo.mdV[VZ]);
                    reach2 = llmax(reach2, (p - c.mOrigin).magVecSquared());
                }
                c.mRadius = llclamp(static_cast<F32>(std::sqrt(reach2)) / FULL_COVER_SHARE, MIN_RADIUS_M, MAX_RADIUS_M);
                c.mDistance = llmax((c.mOrigin - camera).magVec() - c.mRadius, 0.0);
                out.push_back(c);
            }
        }
    }
}

void SKClusterProbes::update(LLReflectionMapManager& manager, const std::shared_ptr<const SKCensusSnapshot>& census,
                             const sknav::SolidLattice* solids)
{
    // A probe only this holds was dropped by the manager, on a teleport or a reset.
    mHeld.erase(std::remove_if(mHeld.begin(), mHeld.end(), [](const Held& h) { return h.mProbe->getNumRefs() == 1; }), mHeld.end());

    if (gFrameTimeSeconds < mNextUpdate || !census || !solids)
    {
        return;
    }
    mNextUpdate = gFrameTimeSeconds + UPDATE_PERIOD_S;
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - cluster probes");

    static LLCachedControl<U32> max_probes(gSavedSettings, "SKClusterProbesMax", 24);
    static LLCachedControl<F32> cell_m(gSavedSettings, "SKClusterProbesCellM", 32.f);
    static LLCachedControl<F32> height_share(gSavedSettings, "SKClusterProbesHeight", 0.65f);

    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    std::vector<Candidate> candidates;
    mClusters = 0;
    for (const sknav::ClusterPtr& cluster : census->mClusters.mClusters)
    {
        const LLVector3d extent = cluster->mMax - cluster->mMin;
        if (extent.mdV[VX] < MIN_EXTENT_M && extent.mdV[VY] < MIN_EXTENT_M && extent.mdV[VZ] < MIN_EXTENT_M)
        {
            continue;
        }
        LLVector3d nearest;
        for (S32 i = 0; i < 3; ++i)
        {
            nearest.mdV[i] = llclamp(camera.mdV[i], cluster->mMin.mdV[i], cluster->mMax.mdV[i]);
        }
        if ((nearest - camera).magVec() > REACH_M)
        {
            continue;
        }
        ++mClusters;
        placeCluster(*cluster, solids, camera, cell_m(), llclamp(static_cast<F64>(height_share()), 0.0, 1.0), candidates);
    }
    mCandidates = static_cast<U32>(candidates.size());
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.mDistance < b.mDistance; });
    if (candidates.size() > max_probes())
    {
        candidates.resize(max_probes());
    }

    // Keep every held probe a candidate still matches, then move the rest onto what is left.
    std::vector<bool> taken(candidates.size(), false);
    std::vector<Held> kept;
    std::vector<Held> spare;
    for (Held& h : mHeld)
    {
        S32 best = -1;
        F64 best_d = MATCH_M;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            const F64 d = (candidates[i].mOrigin - h.mOrigin).magVec();
            if (!taken[i] && d <= best_d && fabsf(candidates[i].mRadius - h.mRadius) <= h.mRadius * MATCH_RADIUS_SHARE)
            {
                best = static_cast<S32>(i);
                best_d = d;
            }
        }
        if (best >= 0)
        {
            taken[best] = true;
            // The agent frame shifts on a region crossing; the capture itself still holds.
            h.mProbe->mOrigin.load3(gAgent.getPosAgentFromGlobal(h.mOrigin).mV);
            kept.push_back(h);
        }
        else
        {
            spare.push_back(h);
        }
    }
    mCreated = mMoved = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (taken[i])
        {
            continue;
        }
        const Candidate& c = candidates[i];
        const LLVector3 origin = gAgent.getPosAgentFromGlobal(c.mOrigin);
        Held h;
        if (!spare.empty())
        {
            h = spare.back();
            spare.pop_back();
            h.mProbe->mOrigin.load3(origin.mV);
            h.mProbe->mRadius = c.mRadius;
            manager.invalidateNavRoomsProbe(h.mProbe);
            ++mMoved;
        }
        else
        {
            h.mProbe = manager.registerClusterProbe(origin, c.mRadius);
            if (h.mProbe.isNull())
            {
                break; // the manager is out of probes
            }
            ++mCreated;
        }
        h.mOrigin = c.mOrigin;
        h.mRadius = c.mRadius;
        kept.push_back(h);
    }
    // Whatever is still spare lets go here; the manager deletes a probe nobody else references.
    mHeld.swap(kept);
}

void SKClusterProbes::clear()
{
    mHeld.clear();
    mNextUpdate = 0.0;
}

std::string SKClusterProbes::statsLine() const
{
    return llformat("cluster probes %u held, %u clusters, %u candidates, %u new, %u moved", static_cast<U32>(mHeld.size()), mClusters,
                    mCandidates, mCreated, mMoved);
}
