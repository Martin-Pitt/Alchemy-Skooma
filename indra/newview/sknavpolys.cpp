/**
 * @file sknavpolys.cpp
 * @brief One Detour tile's polygons as plain data, for readers that must not hold Detour pointers.
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

#include "sknavpolys.h"

#include "sknavbuild.h"
#include "sknavexport.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cfloat>

namespace sknav
{
    std::shared_ptr<const TilePolys> extractTilePolys(const dtMeshTile& tile)
    {
        auto out = std::make_shared<TilePolys>();
        if (!tile.header)
        {
            return out;
        }
        const dtMeshHeader& header = *tile.header;
        out->mVerts.reserve(header.vertCount);
        for (int i = 0; i < header.vertCount; ++i)
        {
            out->mVerts.push_back(fromRecast(&tile.verts[i * 3]));
        }

        // Detour keeps the ground polys first and the off-mesh ones after them, so
        // a ground poly's index is the same with or without the off-mesh ones.
        out->mPolys.reserve(header.polyCount);
        for (int p = 0; p < header.polyCount; ++p)
        {
            const dtPoly& poly = tile.polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            {
                continue;
            }
            TilePolys::Poly q;
            q.mArea = polyArea(poly.flags);
            q.mVertCount = static_cast<U8>(llmin(static_cast<S32>(poly.vertCount), TilePolys::MAX_VERTS));
            F32 zmin = FLT_MAX, zmax = -FLT_MAX;
            for (S32 v = 0; v < q.mVertCount; ++v)
            {
                q.mVerts[v] = poly.verts[v];
                const unsigned short nei = poly.neis[v];
                if (nei == 0)
                {
                    q.mNeighbour[v] = TilePolys::EDGE_NONE;
                }
                else if (nei & DT_EXT_LINK)
                {
                    q.mNeighbour[v] = TilePolys::EDGE_BORDER;
                }
                else
                {
                    q.mNeighbour[v] = static_cast<S16>(nei - 1);
                }
                const F32 z = out->mVerts[poly.verts[v]].mV[VZ];
                zmin = llmin(zmin, z);
                zmax = llmax(zmax, z);
            }

            q.mFirstTri = static_cast<U32>(out->mTris.size() / 3);
            if (tile.detailMeshes && p < header.detailMeshCount)
            {
                const dtPolyDetail& detail = tile.detailMeshes[p];
                for (unsigned int t = 0; t < detail.triCount; ++t)
                {
                    const unsigned char* tri = &tile.detailTris[(detail.triBase + t) * 4];
                    for (int k = 0; k < 3; ++k)
                    {
                        const F32* rc = tri[k] < poly.vertCount ? &tile.verts[poly.verts[tri[k]] * 3]
                                                                : &tile.detailVerts[(detail.vertBase + tri[k] - poly.vertCount) * 3];
                        const LLVector3 corner = fromRecast(rc);
                        zmin = llmin(zmin, corner.mV[VZ]);
                        zmax = llmax(zmax, corner.mV[VZ]);
                        out->mTris.push_back(corner);
                    }
                }
                q.mTriCount = static_cast<U16>(detail.triCount);
            }
            q.mZMin = zmin;
            q.mZMax = zmax;
            out->mPolys.push_back(q);
        }
        return out;
    }
}
