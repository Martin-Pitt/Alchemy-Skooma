/**
 * @file skinventoryexplorerassets.h
 * @brief Inventory Explorer's asset reads for the details: sounds, animations, experiences and landmarks
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef SK_SKINVENTORYEXPLORERASSETS_H
#define SK_SKINVENTORYEXPLORERASSETS_H

#include "lluuid.h"
#include "v3math.h"

#include <string>
#include <vector>

class LLLandmark;

enum ESKAssetState
{
    SK_ASSET_LOADING,
    SK_ASSET_READY,
    SK_ASSET_FAILED,
};

// A sound decoded once: its length and, per slice, the peak and the share of bass, mids and treble.
struct SKSoundInfo
{
    static constexpr S32 SLICES = 256;

    ESKAssetState mState = SK_ASSET_LOADING;
    F32 mLength = 0.f;
    S32 mRate = 0;
    S32 mChannels = 0;
    std::vector<F32> mPeaks;        // 0..1, the loudest slice at 1
    std::vector<LLVector3> mBands;  // bass, mids, treble, summing to 1
};

// What an animation asset says about itself, read through LLKeyframeMotion's deserializer.
struct SKAnimInfo
{
    ESKAssetState mState = SK_ASSET_LOADING;
    F32 mDuration = 0.f;
    bool mLoop = false;
    F32 mLoopIn = 0.f;
    F32 mLoopOut = 0.f;
    F32 mEaseIn = 0.f;
    F32 mEaseOut = 0.f;
    S32 mPriority = 0;
    S32 mHandPose = 1;              // LLHandMotion::HAND_POSE_RELAXED
    std::string mEmote;
    S32 mJoints = 0;
    std::vector<std::string> mRegions;  // keys: body, hands, face, wings, tail, hind
};

// A script's experience, asked of the region once per item.
struct SKExperienceInfo
{
    ESKAssetState mState = SK_ASSET_LOADING;
    bool mNoCapability = false;
    LLUUID mExperienceID;
};

// Session-only caches: nothing is written to disk. A lookup with fetch false only reads what is already known.
namespace SKInventoryAssets
{
    // Moves whenever anything arrives, so a view rebuilds only then.
    U32 revision();

    const SKSoundInfo* sound(const LLUUID& asset_id, bool fetch);
    const SKAnimInfo* animation(const LLUUID& asset_id, bool fetch);
    const SKExperienceInfo* experience(const LLUUID& item_id, bool fetch);
    LLLandmark* landmark(const LLUUID& asset_id, bool fetch);
}

#endif // SK_SKINVENTORYEXPLORERASSETS_H
