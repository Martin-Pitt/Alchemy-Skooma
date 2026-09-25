/**
 * @file sknavair.cpp
 * @brief The open air of a tile: every gap above a solid, per column, off the navmesh heightfield.
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

#include "sknavair.h"

#include "Recast.h"

#include <algorithm>

namespace sknav
{
    void extractAir(const rcHeightfield& hf, S32 border_cells, AirGrid& out)
    {
        // Recast (x, y, z) is SL (y, z, x): a heightfield row along Recast x is
        // a run along SL y, so SL ix is Recast z and SL iy is Recast x.
        const S32 x0 = border_cells, z0 = border_cells;
        out.mWidth = std::max(0, hf.height - 2 * border_cells);
        out.mHeight = std::max(0, hf.width - 2 * border_cells);
        out.mCell = hf.cs;
        out.mUnit = hf.ch;
        out.mOrigin.set(hf.bmin[2] + z0 * hf.cs, hf.bmin[0] + x0 * hf.cs, hf.bmin[1]);
        out.mStart.assign(static_cast<size_t>(out.mWidth) * out.mHeight + 1, 0);
        out.mGaps.clear();

        // The top of the tile's height range, in span units: a gap with
        // nothing above it runs to here.
        const S32 top = static_cast<S32>((hf.bmax[1] - hf.bmin[1]) / hf.ch + 0.5f);
        const U16 open = static_cast<U16>(std::clamp(top, 0, 0xffff));

        for (S32 iy = 0; iy < out.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < out.mWidth; ++ix)
            {
                const rcSpan* s = hf.spans[(x0 + iy) + (z0 + ix) * hf.width];
                for (; s; s = s->next)
                {
                    AirGap gap;
                    gap.mFloor = static_cast<U16>(s->smax);
                    gap.mFlags = s->area != RC_NULL_AREA ? AirGap::WALKABLE : 0;
                    if (s->next)
                    {
                        gap.mCeiling = static_cast<U16>(s->next->smin);
                    }
                    else
                    {
                        gap.mCeiling = std::max(open, gap.mFloor);
                        gap.mFlags |= AirGap::OPEN;
                    }
                    if (gap.mCeiling > gap.mFloor || (gap.mFlags & AirGap::OPEN))
                    {
                        out.mGaps.push_back(gap);
                    }
                }
                out.mStart[ix + iy * out.mWidth + 1] = static_cast<U32>(out.mGaps.size());
            }
        }
    }
}
