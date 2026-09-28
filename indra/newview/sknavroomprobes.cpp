/**
 * @file sknavroomprobes.cpp
 * @brief Automatic reflection probe spheres for the navmesh's rooms, in place of the octree's.
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

#include "sknavroomprobes.h"

#include "llagent.h"
#include "llreflectionmap.h"
#include "llreflectionmapmanager.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "pipeline.h"

#include <algorithm>
#include <chrono>

namespace
{
    // Spaces this far from the camera are considered; the budget trims the rest.
    constexpr F64 REACH_M = 192.0;
    constexpr F32 UPDATE_PERIOD_S = 1.f;

    U64 signatureOf(const sknav::Space& space)
    {
        U64 h = 1469598103934665603ull;
        auto mix = [&h](U64 v) { h = (h ^ v) * 1099511628211ull; };
        mix(static_cast<U64>(space.mBucket));
        mix(static_cast<U64>(std::lround(space.mCeilingMean * 8.f)));
        mix(static_cast<U64>(std::lround(space.mFloorMin * 8.f)));
        mix(static_cast<U64>(std::lround(space.mOpenShare * 16.f)));
        for (const auto& r : space.mRaster)
        {
            mix(r.first);
        }
        return h;
    }
}

SKNavRoomProbes::SKNavRoomProbes() = default;

SKNavRoomProbes::~SKNavRoomProbes() = default;

void SKNavRoomProbes::clear()
{
    if (!mHeld.empty() || mPlaced)
    {
        gPipeline.mReflectionMapManager.setNavRoomsVolumes({});
    }
    mHeld.clear();
    mParts.clear();
    mCandidates = mClearedOut = mOverBudget = mPlaced = 0;
}

void SKNavRoomProbes::update(const SKNavWorldViewPtr& view)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavRoomProbes", true);
    static LLCachedControl<U32> max_probes(gSavedSettings, "SKNavRoomProbesMax", 128);
    if (!enabled || !view || !LLPipeline::sReflectionProbesEnabled)
    {
        clear();
        return;
    }

    // A probe only this holds was dropped by the manager, on a teleport or a reset.
    for (auto it = mHeld.begin(); it != mHeld.end();)
    {
        it = it->second.mProbe->getNumRefs() == 1 ? mHeld.erase(it) : std::next(it);
    }

    if (gFrameTimeSeconds < mNextUpdate)
    {
        return;
    }
    mNextUpdate = gFrameTimeSeconds + UPDATE_PERIOD_S;
    const auto started = std::chrono::steady_clock::now();

    LLReflectionMapManager& manager = gPipeline.mReflectionMapManager;
    const sknav::ProbeParams params;

    std::vector<LLReflectionMap*> placed_probes;
    manager.getNavRoomsContext(placed_probes);
    std::vector<sknav::ProbeInfluence> placed;
    for (LLReflectionMap* probe : placed_probes)
    {
        LLViewerObject* vobj = probe->mViewerObject;
        sknav::ProbeInfluence inf;
        inf.mCentre.set(probe->mOrigin.getF32ptr());
        inf.mRadius = probe->mRadius;
        if (vobj->getReflectionProbeIsBox())
        {
            const LLQuaternion rot = vobj->getRenderRotation();
            inf.mBox = true;
            inf.mAxes[0] = LLVector3::x_axis * rot;
            inf.mAxes[1] = LLVector3::y_axis * rot;
            inf.mAxes[2] = LLVector3::z_axis * rot;
            inf.mHalf = vobj->getScale() * 0.5f;
        }
        placed.push_back(inf);
    }

    const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera);
    const LLVector3d reach(REACH_M, REACH_M, REACH_M);

    struct Candidate
    {
        Key mKey;
        LLVector3 mOrigin;
        F32 mRadius = 0.f;
        LLVector3 mMin;
        LLVector3 mMax;
    };
    std::vector<Candidate> candidates;
    std::vector<sknav::ProbeSlot> slots;
    mClearedOut = 0;
    for (auto& p : mParts)
    {
        p.second.mSeen = false;
    }
    view->spacesIn(camera_global - reach, camera_global + reach, [&](const SKNavWorldView::Mesh& mesh, const sknav::Space& space)
    {
        if (!sknav::wantsProbes(space, params))
        {
            return;
        }
        Parts& parts = mParts[std::make_pair(mesh.mId, space.mId)];
        parts.mSeen = true;
        const U64 signature = signatureOf(space);
        if (parts.mSignature != signature || parts.mParts.empty())
        {
            parts.mSignature = signature;
            parts.mParts = sknav::spaceProbes(space, params);
        }
        const LLVector3 origin = gAgent.getPosAgentFromGlobal(mesh.mOrigin);
        for (const sknav::ProbePart& part : parts.mParts)
        {
            Candidate c;
            c.mKey = Key(mesh.mId, space.mId, part.mPart);
            c.mOrigin = origin + part.mOrigin;
            c.mRadius = sknav::clearRadius(c.mOrigin, part.mRadius, placed, params);
            if (c.mRadius <= 0.f)
            {
                ++mClearedOut;
                continue;
            }
            c.mMin = origin + part.mMin;
            c.mMax = origin + part.mMax;
            sknav::ProbeSlot slot;
            slot.mKey = candidates.size();
            slot.mOrigin = c.mOrigin;
            slot.mRadius = c.mRadius;
            slot.mArea = part.mArea;
            slot.mDistance = std::max(dist_vec(camera, c.mOrigin) - c.mRadius, 0.f);
            slots.push_back(slot);
            candidates.push_back(c);
        }
    });
    for (auto it = mParts.begin(); it != mParts.end();)
    {
        it = it->second.mSeen ? std::next(it) : mParts.erase(it);
    }

    // The probe array's cap, less the default probe and every placed one.
    const U32 room = std::max(static_cast<S32>(manager.probeCount()) - 1 - static_cast<S32>(placed.size()), 0);
    const U32 cap = std::min<U32>(max_probes, std::min<U32>(room, LL_MAX_REFLECTION_PROBE_COUNT - 1));
    mCandidates = static_cast<U32>(slots.size());
    sknav::keepBudget(slots, cap);
    mOverBudget = mCandidates - static_cast<U32>(slots.size());

    std::map<Key, Held> held;
    std::vector<std::pair<LLVector3, LLVector3>> volumes;
    for (const sknav::ProbeSlot& slot : slots)
    {
        const Candidate& c = candidates[slot.mKey];
        volumes.emplace_back(c.mMin, c.mMax);
        auto it = mHeld.find(c.mKey);
        if (it != mHeld.end())
        {
            Held h = it->second;
            const LLVector3 at = gAgent.getPosAgentFromGlobal(h.mOrigin);
            if (sknav::shouldReplace(at, h.mRadius, c.mOrigin, c.mRadius, params))
            {
                h.mProbe->mOrigin.load3(c.mOrigin.mV);
                h.mProbe->mRadius = c.mRadius;
                h.mProbe->mLastUpdateTime = 0.f;    // first in line to re-render
                h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
                h.mRadius = c.mRadius;
                ++mMoves;
            }
            held[c.mKey] = h;
            continue;
        }
        Held h;
        h.mProbe = manager.registerNavRoomsProbe(c.mOrigin, c.mRadius);
        if (h.mProbe.isNull())
        {
            continue;
        }
        h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
        h.mRadius = c.mRadius;
        held[c.mKey] = h;
    }
    mHeld.swap(held);
    mPlaced = static_cast<U32>(mHeld.size());
    manager.setNavRoomsVolumes(volumes);
    mLastMs = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
}

std::string SKNavRoomProbes::statsLine() const
{
    return llformat("room probes %u of %u parts (%u clear of placed, %u over budget), %u moves, %.2f ms",
                    mPlaced, mCandidates + mClearedOut, mClearedOut, mOverBudget, mMoves, mLastMs);
}
