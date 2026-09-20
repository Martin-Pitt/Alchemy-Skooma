/**
 * @file sknavlevels.cpp
 * @brief Storeys of a structure and the stairs, ramps and drops between them.
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

#include "sknavlevels.h"

#include "sknavbuild.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace
{
    using namespace sknav;

    struct Poly
    {
        dtPolyRef mRef = 0;
        LLVector3 mCentre;      // mesh-local SL
        LLVector3 mMin;
        LLVector3 mMax;
        bool mFlat = false;
        U16 mLevel = Levels::NO_LEVEL;
        S32 mRun = -1;          // index of the sloped/transitional run it belongs to
    };

    /// Every walkable polygon of the mesh, in SL mesh-local metres.
    void gather(const dtNavMesh& nav, const LevelParams& params, std::vector<Poly>& out, std::unordered_map<U64, S32>& index)
    {
        for (int t = 0; t < nav.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            const dtPolyRef base = nav.getPolyRefBase(tile);
            for (int i = 0; i < tile->header->polyCount; ++i)
            {
                const dtPoly& poly = tile->polys[i];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount == 0)
                {
                    continue;
                }
                Poly p;
                p.mRef = base | static_cast<dtPolyRef>(i);
                LLVector3 sum;
                p.mMin.set(1e9f, 1e9f, 1e9f);
                p.mMax.set(-1e9f, -1e9f, -1e9f);
                for (int v = 0; v < poly.vertCount; ++v)
                {
                    const LLVector3 pos = fromRecast(&tile->verts[poly.verts[v] * 3]);
                    sum += pos;
                    for (S32 k = 0; k < 3; ++k)
                    {
                        p.mMin.mV[k] = std::min(p.mMin.mV[k], pos.mV[k]);
                        p.mMax.mV[k] = std::max(p.mMax.mV[k], pos.mV[k]);
                    }
                }
                p.mCentre = sum / static_cast<F32>(poly.vertCount);
                const F32 rise = p.mMax.mV[VZ] - p.mMin.mV[VZ];
                const F32 run = std::max(p.mMax.mV[VX] - p.mMin.mV[VX], p.mMax.mV[VY] - p.mMin.mV[VY]);
                p.mFlat = rise <= params.mStepTolerance || rise <= params.mFlatGradient * run;
                index[p.mRef] = static_cast<S32>(out.size());
                out.push_back(p);
            }
        }
    }

    /// The polygons this one is linked to, by index into the gathered list.
    void neighbours(const dtNavMesh& nav, const std::unordered_map<U64, S32>& index, dtPolyRef ref, std::vector<S32>& out)
    {
        out.clear();
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(nav.getTileAndPolyByRef(ref, &tile, &poly)))
        {
            return;
        }
        for (unsigned int i = poly->firstLink; i != DT_NULL_LINK; i = tile->links[i].next)
        {
            const dtLink& link = tile->links[i];
            if (!link.ref)
            {
                continue;
            }
            const auto it = index.find(link.ref);
            if (it != index.end())
            {
                out.push_back(it->second);
            }
        }
    }
}

namespace sknav
{
    void buildLevels(const dtNavMesh& nav, const dtNavMeshQuery& query, const LevelParams& params,
                     const std::vector<DropLink>& links, Levels& out)
    {
        out.mPolyLevel.clear();
        out.mLevels.clear();
        out.mConnectors.clear();

        std::vector<Poly> polys;
        std::unordered_map<U64, S32> index;
        gather(nav, params, polys, index);
        if (polys.empty())
        {
            return;
        }

        std::vector<S32> nei;
        std::deque<S32> queue;

        // Storeys: flat polygons reached from a seed without leaving its band.
        // Seeding from the lowest polygon upward makes the result the same
        // whatever order Detour happens to hold its tiles in.
        std::vector<S32> order(polys.size());
        for (size_t i = 0; i < polys.size(); ++i)
        {
            order[i] = static_cast<S32>(i);
        }
        std::sort(order.begin(), order.end(), [&](S32 a, S32 b) {
            if (polys[a].mCentre.mV[VZ] != polys[b].mCentre.mV[VZ])
            {
                return polys[a].mCentre.mV[VZ] < polys[b].mCentre.mV[VZ];
            }
            return polys[a].mRef < polys[b].mRef;
        });

        for (S32 seed : order)
        {
            if (!polys[seed].mFlat || polys[seed].mLevel != Levels::NO_LEVEL)
            {
                continue;
            }
            const U16 level = static_cast<U16>(out.mLevels.size());
            const F32 seed_z = polys[seed].mCentre.mV[VZ];
            Level info;
            info.mMin.set(1e9f, 1e9f, 1e9f);
            info.mMax.set(-1e9f, -1e9f, -1e9f);
            F64 z_sum = 0.0;

            polys[seed].mLevel = level;
            queue.push_back(seed);
            while (!queue.empty())
            {
                const S32 at = queue.front();
                queue.pop_front();
                const Poly& p = polys[at];
                info.mPolys++;
                z_sum += p.mCentre.mV[VZ];
                for (S32 k = 0; k < 3; ++k)
                {
                    info.mMin.mV[k] = std::min(info.mMin.mV[k], p.mMin.mV[k]);
                    info.mMax.mV[k] = std::max(info.mMax.mV[k], p.mMax.mV[k]);
                }
                neighbours(nav, index, p.mRef, nei);
                for (S32 n : nei)
                {
                    if (polys[n].mLevel != Levels::NO_LEVEL || !polys[n].mFlat)
                    {
                        continue;
                    }
                    if (std::fabs(polys[n].mCentre.mV[VZ] - seed_z) > params.mBand)
                    {
                        continue;   // the band is what a staircase cannot climb
                    }
                    polys[n].mLevel = level;
                    queue.push_back(n);
                }
            }
            info.mZ = static_cast<F32>(z_sum / info.mPolys);
            const F32 footprint = (info.mMax.mV[VX] - info.mMin.mV[VX]) * (info.mMax.mV[VY] - info.mMin.mV[VY]);
            info.mTransitional = footprint < params.mMinArea;
            out.mLevels.push_back(info);
        }

        // Runs: sloped polygons, and the small storeys that are really landings
        // or treads, grouped into whatever joins two real storeys.
        auto transitional = [&](S32 i) {
            const U16 level = polys[i].mLevel;
            return level == Levels::NO_LEVEL || out.mLevels[level].mTransitional;
        };
        S32 runs = 0;
        for (size_t start = 0; start < polys.size(); ++start)
        {
            if (polys[start].mRun >= 0 || !transitional(static_cast<S32>(start)))
            {
                continue;
            }
            const S32 run = runs++;
            std::vector<S32> members;
            polys[start].mRun = run;
            queue.push_back(static_cast<S32>(start));
            while (!queue.empty())
            {
                const S32 at = queue.front();
                queue.pop_front();
                members.push_back(at);
                neighbours(nav, index, polys[at].mRef, nei);
                for (S32 n : nei)
                {
                    if (polys[n].mRun < 0 && transitional(n))
                    {
                        polys[n].mRun = run;
                        queue.push_back(n);
                    }
                }
            }

            // Which real storeys this run touches, and where.
            S32 low = -1, high = -1;
            LLVector3 low_point, high_point;
            for (S32 m : members)
            {
                neighbours(nav, index, polys[m].mRef, nei);
                for (S32 n : nei)
                {
                    const U16 level = polys[n].mLevel;
                    if (level == Levels::NO_LEVEL || out.mLevels[level].mTransitional)
                    {
                        continue;
                    }
                    if (low < 0 || out.mLevels[level].mZ < out.mLevels[low].mZ)
                    {
                        low = level;
                        low_point = polys[n].mCentre;
                    }
                    if (high < 0 || out.mLevels[level].mZ > out.mLevels[high].mZ)
                    {
                        high = level;
                        high_point = polys[n].mCentre;
                    }
                }
            }
            if (low >= 0 && high >= 0 && low != high)
            {
                Connector c;
                c.mLow = static_cast<U16>(low);
                c.mHigh = static_cast<U16>(high);
                c.mLowPoint = low_point;
                c.mHighPoint = high_point;
                c.mKind = ConnectorKind::CLIMB;
                out.mConnectors.push_back(c);
            }
        }

        // A drop is a connector too, but only downward.
        const F32 extents[3] = { 1.f, 2.f, 1.f };
        dtQueryFilter filter;
        auto levelAt = [&](const LLVector3& point) -> U16 {
            F32 centre[3], nearest[3];
            toRecast(point, centre);
            dtPolyRef ref = 0;
            if (dtStatusFailed(query.findNearestPoly(centre, extents, &filter, &ref, nearest)) || !ref)
            {
                return Levels::NO_LEVEL;
            }
            const auto it = index.find(ref);
            return it == index.end() ? Levels::NO_LEVEL : polys[it->second].mLevel;
        };
        for (const DropLink& link : links)
        {
            const U16 from = levelAt(link.mFrom);
            const U16 to = levelAt(link.mTo);
            if (from == Levels::NO_LEVEL || to == Levels::NO_LEVEL || from == to)
            {
                continue;
            }
            if (out.mLevels[from].mTransitional || out.mLevels[to].mTransitional)
            {
                continue;
            }
            // One per pair of storeys: a ledge makes a link every couple of
            // metres and the map wants one arrow, not thirty.
            const bool known = std::any_of(out.mConnectors.begin(), out.mConnectors.end(), [&](const Connector& c) {
                return c.mKind == ConnectorKind::DROP && c.mHigh == from && c.mLow == to;
            });
            if (known)
            {
                continue;
            }
            Connector c;
            c.mHigh = from;
            c.mLow = to;
            c.mHighPoint = link.mFrom;
            c.mLowPoint = link.mTo;
            c.mKind = ConnectorKind::DROP;
            c.mOneWay = true;
            out.mConnectors.push_back(c);
        }

        out.mPolyLevel.reserve(polys.size());
        for (const Poly& p : polys)
        {
            if (p.mLevel != Levels::NO_LEVEL)
            {
                out.mPolyLevel[p.mRef] = p.mLevel;
            }
        }
    }
}
