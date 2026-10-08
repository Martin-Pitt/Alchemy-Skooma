/**
 * @file sknavspaceproducts.cpp
 * @brief What the navmesh makes of its spaces: walls with a space on each side.
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

#include "linden_common.h"

#include "sknavspaceproducts.h"

#include <cmath>

namespace sknav
{
    namespace
    {
        constexpr F32 STEP_M = 0.0625f;     ///< half a fine air cell: no column is stepped over
        constexpr F32 SAME_FLOOR_M = 0.3f;  ///< a gap whose floor is this close to the segment's is its own floor still

        /// Recast polys wind one way, so a segment's floor lies on the same side of A to B every time.
        constexpr bool SEGMENT_FLOOR_LEFT = true;
    }

    U32 spaceIdAt(const EnclosureProduct& enc, const LLVector3& p)
    {
        Enclosure e;
        return enc.enclosureAt(p, e) ? e.mSpaceId : 0;
    }

    U32 spaceAcross(const EnclosureProduct& enc, const LLVector3& p, const LLVector3& dir, F32 reach)
    {
        for (F32 d = STEP_M; d <= reach + 1e-4f; d += STEP_M)
        {
            Enclosure e;
            if (enc.enclosureAt(p + dir * d, e))
            {
                return e.mSpaceId;
            }
        }
        return 0;
    }

    void bindWallSpaces(const EnclosureProduct& enc, std::vector<WallSegment>& segments)
    {
        for (WallSegment& s : segments)
        {
            s.mSpace[0] = s.mSpace[1] = 0;
            const F32 dx = s.mB.mV[VX] - s.mA.mV[VX], dy = s.mB.mV[VY] - s.mA.mV[VY];
            const F32 len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
            {
                continue;
            }
            const LLVector3 left(-dy / len, dx / len, 0.f);
            const LLVector3 in = SEGMENT_FLOOR_LEFT ? left : -left;
            const LLVector3 mid = (s.mA + s.mB) * 0.5f;
            const S32 floor_side = SEGMENT_FLOOR_LEFT ? 0 : 1;
            U32 own = 0;
            F32 own_floor = mid.mV[VZ];
            for (const F32 h : { WALL_LOW_M, WALL_LOW_M + 1.f })
            {
                Enclosure e;
                if (enc.enclosureAt(mid + in * STEP_M + LLVector3(0.f, 0.f, h), e))
                {
                    own = e.mSpaceId;
                    own_floor = e.mFloor;
                    break;
                }
            }
            s.mSpace[floor_side] = own;
            // Beyond: past the floor's own skirt, through whatever bounds it, to the next gap.
            U32 beyond = 0;
            for (const F32 h : { WALL_LOW_M, WALL_LOW_M + 1.f })
            {
                bool crossed = false, found = false;
                for (F32 d = STEP_M; d <= WALL_REACH_M + 1e-4f && !found; d += STEP_M)
                {
                    Enclosure e;
                    if (!enc.enclosureAt(mid - in * d + LLVector3(0.f, 0.f, h), e))
                    {
                        crossed = true;
                        continue;
                    }
                    if (!crossed && e.mSpaceId == own && std::fabs(e.mFloor - own_floor) <= SAME_FLOOR_M)
                    {
                        continue;
                    }
                    beyond = e.mSpaceId;
                    found = true;
                }
                if (found)
                {
                    break;
                }
            }
            s.mSpace[1 - floor_side] = beyond;
        }
    }

    void bindWallSpaces(const EnclosureProduct& enc, std::vector<WallFace>& faces)
    {
        static const LLVector3 FACING[4] = { LLVector3(1.f, 0.f, 0.f), LLVector3(-1.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, -1.f, 0.f) };
        for (WallFace& f : faces)
        {
            const LLVector3 c = (f.mMin + f.mMax) * 0.5f;
            const LLVector3& n = FACING[f.mSide & 3];
            f.mSpace[0] = spaceAcross(enc, c, n, WALL_REACH_M);
            f.mSpace[1] = spaceAcross(enc, c, -n, WALL_REACH_M);
        }
    }
}
