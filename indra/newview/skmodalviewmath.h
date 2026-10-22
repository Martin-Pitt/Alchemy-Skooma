/**
 * @file skmodalviewmath.h
 * @brief The modal view's claim order and its camera snapshot maths, free of the viewer.
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

#pragma once

#include "stdtypes.h"
#include "v3dmath.h"

#include <vector>

namespace skmodalview
{
    /// The views that cover the world, each with a priority. The highest priority is on top; among
    /// equals the newest claim is. Keys are opaque to it.
    class Claims
    {
    public:
        /// Adds a claim, or moves an existing one to its new priority as the newest at that priority.
        void claim(const void* key, S32 priority);
        /// Drops a claim; false when it held none.
        bool release(const void* key);

        bool empty() const { return mClaims.empty(); }
        size_t size() const { return mClaims.size(); }
        bool holds(const void* key) const;
        /// The claim on top, or null when there is none.
        const void* top() const;
        /// Every claim, top first.
        std::vector<const void*> ordered() const;

    private:
        struct Claim
        {
            const void* mKey;
            S32 mPriority;
            U64 mSequence;
        };
        std::vector<Claim> mClaims;
        U64 mNextSequence = 0;
    };

    /// A camera point kept relative to the agent when the camera follows it, so a restore puts it at the
    /// same offset from wherever the agent is then; a camera that did not follow is kept where it was.
    inline LLVector3d storePoint(const LLVector3d& point_global, const LLVector3d& agent_global, bool follows)
    {
        return follows ? point_global - agent_global : point_global;
    }

    inline LLVector3d restorePoint(const LLVector3d& stored, const LLVector3d& agent_global, bool follows)
    {
        return follows ? stored + agent_global : stored;
    }
} // namespace skmodalview
