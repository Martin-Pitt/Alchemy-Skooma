/**
 * @file skmodalviewmath.cpp
 * @brief The modal view's claim order and its camera snapshot maths, free of the viewer.
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

#include "linden_common.h"

#include "skmodalviewmath.h"

#include <algorithm>
#include <cmath>

namespace skmodalview
{
    void Claims::claim(const void* key, S32 priority)
    {
        release(key);
        mClaims.push_back({ key, priority, mNextSequence++ });
    }

    bool Claims::release(const void* key)
    {
        auto it = std::find_if(mClaims.begin(), mClaims.end(), [key](const Claim& c) { return c.mKey == key; });
        if (it == mClaims.end())
        {
            return false;
        }
        mClaims.erase(it);
        return true;
    }

    bool Claims::holds(const void* key) const
    {
        return std::any_of(mClaims.begin(), mClaims.end(), [key](const Claim& c) { return c.mKey == key; });
    }

    const void* Claims::top() const
    {
        const Claim* best = nullptr;
        for (const Claim& c : mClaims)
        {
            if (!best || c.mPriority > best->mPriority || (c.mPriority == best->mPriority && c.mSequence > best->mSequence))
            {
                best = &c;
            }
        }
        return best ? best->mKey : nullptr;
    }

    std::vector<const void*> Claims::ordered() const
    {
        std::vector<Claim> sorted = mClaims;
        std::sort(sorted.begin(), sorted.end(), [](const Claim& a, const Claim& b) {
            return a.mPriority != b.mPriority ? a.mPriority > b.mPriority : a.mSequence > b.mSequence;
        });
        std::vector<const void*> keys;
        keys.reserve(sorted.size());
        for (const Claim& c : sorted)
        {
            keys.push_back(c.mKey);
        }
        return keys;
    }

    bool addMoveAction(const std::string& action, Move& move)
    {
        struct Step
        {
            const char* mAction;
            F32 mForward, mLeft, mUp;
            bool mFast;
        };
        static constexpr Step steps[] = {
            { "push_forward", 1.f, 0.f, 0.f, false },  { "run_forward", 1.f, 0.f, 0.f, true },
            { "push_backward", -1.f, 0.f, 0.f, false }, { "run_backward", -1.f, 0.f, 0.f, true },
            { "slide_left", 0.f, 1.f, 0.f, false },    { "turn_left", 0.f, 1.f, 0.f, false },
            { "run_left", 0.f, 1.f, 0.f, true },       { "slide_right", 0.f, -1.f, 0.f, false },
            { "turn_right", 0.f, -1.f, 0.f, false },   { "run_right", 0.f, -1.f, 0.f, true },
            { "jump", 0.f, 0.f, 1.f, false },          { "push_down", 0.f, 0.f, -1.f, false },
            { "toggle_down", 0.f, 0.f, -1.f, false },
        };
        for (const Step& step : steps)
        {
            if (action == step.mAction)
            {
                move.mForward = std::clamp(move.mForward + step.mForward, -1.f, 1.f);
                move.mLeft = std::clamp(move.mLeft + step.mLeft, -1.f, 1.f);
                move.mUp = std::clamp(move.mUp + step.mUp, -1.f, 1.f);
                move.mFast |= step.mFast;
                return true;
            }
        }
        return false;
    }

    bool groundDirection(const LLVector3& at, const LLVector3& up, F32 forward, F32 left, LLVector3& dir)
    {
        LLVector3 ahead(at.mV[VX], at.mV[VY], 0.f);
        if (ahead.normVec() < 0.05f)
        {
            ahead.setVec(up.mV[VX], up.mV[VY], 0.f);
            if (ahead.normVec() < 1.0e-3f)
            {
                return false;
            }
        }
        const LLVector3 side(-ahead.mV[VY], ahead.mV[VX], 0.f);
        dir = ahead * forward + side * left;
        return dir.normVec() > 0.f;
    }

    F64 flycamSpeedScale(F64 height, F64 max_scale)
    {
        // Linear in height just above the reference, as the flycam's own speed scaled by height would be,
        // bending smoothly onto the cap so a climb never makes the next stick push a jump.
        const F64 over = height / FLYCAM_REFERENCE_HEIGHT - 1.0;
        const F64 room = max_scale - 1.0;
        if (over <= 0.0 || room <= 0.0)
        {
            return 1.0;
        }
        return 1.0 + room * std::tanh(over / room);
    }
} // namespace skmodalview
