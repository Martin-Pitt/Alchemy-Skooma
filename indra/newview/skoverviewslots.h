/**
 * @file skoverviewslots.h
 * @brief The census overview's vertex pages: runs allocated within a page, linkset ids stable per root, and the draw
 *        commands a shot builds over a page's slots on the CPU, as overviewCommandsC.glsl builds them on the GPU.
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

#ifndef SK_OVERVIEWSLOTS_H
#define SK_OVERVIEWSLOTS_H

#include "stdtypes.h"

#include <map>
#include <set>
#include <vector>

namespace skoverview
{
    /// The one hard limit on a run of vertices, the old batch cap: a linkset whose run of one class is longer is dropped.
    constexpr U32 MAX_PAGE_VERTICES = 16u * 1024u * 1024u;

    /// The vertex runs of one page: first fit over the free runs, a freed run joining the free runs beside it.
    class SlotAllocator
    {
    public:
        static constexpr U32 NONE = 0xFFFFFFFFu;

        explicit SlotAllocator(U32 capacity = 0);

        /// The first free run that holds count vertices, cut from its start; NONE when none does, or count is 0.
        U32 allocate(U32 count);
        /// Gives a run back; it must be one allocate() gave.
        void release(U32 first, U32 count);

        U32 capacity() const { return mCapacity; }
        U32 used() const { return mUsed; }
        bool empty() const { return mUsed == 0; }
        /// The free runs, first vertex to count, none touching another.
        const std::map<U32, U32>& freeRuns() const { return mFree; }

    private:
        U32 mCapacity = 0;
        U32 mUsed = 0;
        std::map<U32, U32> mFree;
    };

    /// Linkset ids: a root keeps its id while it stays, and a new one takes the lowest id free.
    class IdPool
    {
    public:
        U32 acquire();
        void release(U32 id);
        /// One past the highest id in use, 0 when none is.
        U32 bound() const { return mNext; }
        size_t freeCount() const { return mFree.size(); }
        void clear();

    private:
        U32 mNext = 0;
        std::set<U32> mFree; ///< the ids below mNext not in use
    };

    /// A page's slot as both command builders read it, and as the slot table holds it on the GPU (std430, 48 bytes).
    struct SlotCull
    {
        U32 mFirst = 0;     ///< its run in the page
        U32 mCount = 0;
        U32 mGroup = 0;     ///< its linkset's id
        U32 mGround = 0;    ///< its linkset's cluster stands on the land
        F32 mLo[4] = {};    ///< page-local box minimum; w its cluster's lowest global z
        F32 mHi[4] = {};    ///< page-local box maximum; w its cluster's highest global z
    };
    static_assert(sizeof(SlotCull) == 48, "the slot table's std430 layout");

    /// What a shot keeps by height: everything, the ground's groups, and those overlapping [mLo, mHi] (global z).
    struct LevelCull
    {
        bool mAll = true;
        bool mGround = false;
        F32 mLo = 0.f;
        F32 mHi = 0.f;

        bool keeps(F32 min_z, F32 max_z, bool ground) const
        {
            return mAll || (mGround && ground) || (max_z >= mLo && min_z <= mHi);
        }
    };

    /// Whether a box is not wholly outside one of the planes, each (a, b, c, d) with a*x + b*y + c*z + d <= 0 inside.
    bool boxInPlanes(const F32 lo[3], const F32 hi[3], const F32 planes[6][4]);

    /// One command a slot, in their order: its run where the level keeps it and the planes hold its box, else a count
    /// of 0. The planes are in the page's own frame.
    void buildCommands(const std::vector<SlotCull>& slots, const LevelCull& level, const F32 planes[6][4],
                       std::vector<U32>& first, std::vector<U32>& count);

    /// The commands with a count, each run that starts where the last ends merged into it.
    void mergeRuns(const std::vector<U32>& first, const std::vector<U32>& count, std::vector<S32>& merged_first,
                   std::vector<S32>& merged_count);
}

#endif // SK_OVERVIEWSLOTS_H
