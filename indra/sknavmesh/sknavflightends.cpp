/**
 * @file sknavflightends.cpp
 * @brief Flight ends: the floor polys each end of a stair or ramp meets, and why an end meets none.
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

#include "sknavflightends.h"

#include "sknavbuild.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace sknav
{
    namespace
    {
        constexpr F32 TILE_PROBE_M = 0.5f;  ///< an end whose floor would lie this far out past it, off every tile, is at the tiles' edge

        struct Info
        {
            const dtMeshTile* mTile = nullptr;
            const dtPoly* mPoly = nullptr;
            U64 mRef = 0;
            U64 mFlight = 0;
            bool mOwned = false;
            F32 mX0 = 0.f, mY0 = 0.f, mX1 = 0.f, mY1 = 0.f;
        };

        struct Box
        {
            F32 mX0, mY0, mX1, mY1;
        };

        LLVector3 vert(const dtMeshTile* tile, const dtPoly& poly, S32 v)
        {
            return fromRecast(&tile->verts[poly.verts[v] * 3]);
        }

        F32 cross2(F32 ax, F32 ay, F32 bx, F32 by)
        {
            return ax * by - ay * bx;
        }

        /// Plan distance from p to the segment ab, and the parameter of the closest point.
        F32 pointSeg(const LLVector3& p, const LLVector3& a, const LLVector3& b, F32& u)
        {
            const F32 ex = b.mV[VX] - a.mV[VX], ey = b.mV[VY] - a.mV[VY], l2 = ex * ex + ey * ey;
            u = l2 > 0.f ? llclamp(((p.mV[VX] - a.mV[VX]) * ex + (p.mV[VY] - a.mV[VY]) * ey) / l2, 0.f, 1.f) : 0.f;
            return std::hypot(a.mV[VX] + ex * u - p.mV[VX], a.mV[VY] + ey * u - p.mV[VY]);
        }

        /// The poly's height under a plan point inside it, by its fan.
        bool zInside(const dtMeshTile* tile, const dtPoly& poly, const LLVector3& p, F32& z)
        {
            const LLVector3 v0 = vert(tile, poly, 0);
            for (S32 i = 1; i + 1 < poly.vertCount; ++i)
            {
                const LLVector3 v1 = vert(tile, poly, i), v2 = vert(tile, poly, i + 1);
                const F32 ux = v1.mV[VX] - v0.mV[VX], uy = v1.mV[VY] - v0.mV[VY], wx = v2.mV[VX] - v0.mV[VX], wy = v2.mV[VY] - v0.mV[VY];
                const F32 d = cross2(ux, uy, wx, wy);
                if (std::fabs(d) < 1e-9f)
                {
                    continue;
                }
                const F32 px = p.mV[VX] - v0.mV[VX], py = p.mV[VY] - v0.mV[VY];
                const F32 s = cross2(px, py, wx, wy) / d, t = cross2(ux, uy, px, py) / d;
                if (s >= -1e-4f && t >= -1e-4f && s + t <= 1.f + 1e-4f)
                {
                    z = v0.mV[VZ] + s * (v1.mV[VZ] - v0.mV[VZ]) + t * (v2.mV[VZ] - v0.mV[VZ]);
                    return true;
                }
            }
            return false;
        }

        /// Plan distance from the segment ab to the poly, and the poly's height where they come closest.
        F32 touch(const dtMeshTile* tile, const dtPoly& poly, const LLVector3& a, const LLVector3& b, F32& z)
        {
            for (const LLVector3* p : { &a, &b })
            {
                if (zInside(tile, poly, *p, z))
                {
                    return 0.f;
                }
            }
            F32 best = 1e30f;
            for (S32 i = 0; i < poly.vertCount; ++i)
            {
                const LLVector3 c = vert(tile, poly, i), d = vert(tile, poly, (i + 1) % poly.vertCount);
                const F32 d1 = cross2(d.mV[VX] - c.mV[VX], d.mV[VY] - c.mV[VY], a.mV[VX] - c.mV[VX], a.mV[VY] - c.mV[VY]);
                const F32 d2 = cross2(d.mV[VX] - c.mV[VX], d.mV[VY] - c.mV[VY], b.mV[VX] - c.mV[VX], b.mV[VY] - c.mV[VY]);
                const F32 d3 = cross2(b.mV[VX] - a.mV[VX], b.mV[VY] - a.mV[VY], c.mV[VX] - a.mV[VX], c.mV[VY] - a.mV[VY]);
                const F32 d4 = cross2(b.mV[VX] - a.mV[VX], b.mV[VY] - a.mV[VY], d.mV[VX] - a.mV[VX], d.mV[VY] - a.mV[VY]);
                if (d1 * d2 < 0.f && d3 * d4 < 0.f)
                {
                    // The segments cross: distance 0 at the crossing.
                    z = c.mV[VZ] + (d.mV[VZ] - c.mV[VZ]) * (d3 / (d3 - d4));
                    return 0.f;
                }
                F32 u = 0.f;
                for (const LLVector3* p : { &a, &b })
                {
                    const F32 dd = pointSeg(*p, c, d, u);
                    if (dd < best)
                    {
                        best = dd;
                        z = c.mV[VZ] + (d.mV[VZ] - c.mV[VZ]) * u;
                    }
                }
                for (const LLVector3* p : { &c, &d })
                {
                    const F32 dd = pointSeg(*p, a, b, u);
                    if (dd < best)
                    {
                        best = dd;
                        z = p->mV[VZ];
                    }
                }
            }
            return best;
        }
    }

    void flightEnds(const dtNavMesh& nav, const std::vector<const Flight*>& flights, const PolyFlightFn& flight_of, std::vector<FlightEnds>& out)
    {
        out.clear();
        std::map<U64, const Flight*> by_key;
        for (const Flight* f : flights)
        {
            if (f)
            {
                by_key.emplace(f->mKey, f);
            }
        }
        std::vector<Info> polys;
        std::unordered_map<U64, std::vector<size_t>> own;
        std::unordered_map<U64, U64> key_of_ref;
        std::vector<Box> tiles;
        for (S32 t = 0; t < nav.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            const LLVector3 b0 = fromRecast(tile->header->bmin), b1 = fromRecast(tile->header->bmax);
            tiles.push_back({ std::min(b0.mV[VX], b1.mV[VX]), std::min(b0.mV[VY], b1.mV[VY]), std::max(b0.mV[VX], b1.mV[VX]),
                              std::max(b0.mV[VY], b1.mV[VY]) });
            const dtPolyRef base = nav.getPolyRefBase(tile);
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3)
                {
                    continue;
                }
                Info in;
                in.mTile = tile;
                in.mPoly = &poly;
                in.mRef = static_cast<U64>(base | static_cast<dtPolyRef>(p));
                if (const Flight* f = flight_of ? flight_of(tile, static_cast<U32>(p)) : nullptr)
                {
                    in.mFlight = f->mKey;
                    in.mOwned = true;
                    by_key.emplace(f->mKey, f);
                    own[f->mKey].push_back(polys.size());
                    key_of_ref[in.mRef] = f->mKey;
                }
                in.mX0 = in.mY0 = 1e30f;
                in.mX1 = in.mY1 = -1e30f;
                for (S32 v = 0; v < poly.vertCount; ++v)
                {
                    const LLVector3 a = vert(tile, poly, v);
                    in.mX0 = std::min(in.mX0, a.mV[VX]);
                    in.mY0 = std::min(in.mY0, a.mV[VY]);
                    in.mX1 = std::max(in.mX1, a.mV[VX]);
                    in.mY1 = std::max(in.mY1, a.mV[VY]);
                }
                polys.push_back(in);
            }
        }
        const auto on_tiles = [&tiles](F32 x, F32 y) {
            return std::any_of(tiles.begin(), tiles.end(), [x, y](const Box& b) { return x >= b.mX0 && x <= b.mX1 && y >= b.mY0 && y <= b.mY1; });
        };

        for (const auto& [key, f] : by_key)
        {
            FlightEnds fe;
            fe.mFlight = key;
            const LLVector3 a[2] = { f->mBottomA, f->mTopA }, b[2] = { f->mBottomB, f->mTopB };
            const F32 z[2] = { f->mBottom.mV[VZ], f->mTop.mV[VZ] };
            std::set<U64> found[2];
            const auto mine = own.find(key);
            // Across the links of its own polys, at an end's height and near its edge.
            for (size_t k = 0; mine != own.end() && k < mine->second.size(); ++k)
            {
                const Info& in = polys[mine->second[k]];
                for (U32 l = in.mPoly->firstLink; l != DT_NULL_LINK; l = in.mTile->links[l].next)
                {
                    const dtLink& link = in.mTile->links[l];
                    const dtMeshTile* nt = nullptr;
                    const dtPoly* np = nullptr;
                    if (link.edge >= in.mPoly->vertCount || dtStatusFailed(nav.getTileAndPolyByRef(link.ref, &nt, &np)) ||
                        np->getType() != DT_POLYTYPE_GROUND)
                    {
                        continue;
                    }
                    const auto kr = key_of_ref.find(static_cast<U64>(link.ref));
                    if (kr != key_of_ref.end() && kr->second == key)
                    {
                        continue;
                    }
                    const LLVector3 mid = (vert(in.mTile, *in.mPoly, link.edge) + vert(in.mTile, *in.mPoly, (link.edge + 1) % in.mPoly->vertCount)) * 0.5f;
                    const S32 e = std::fabs(mid.mV[VZ] - z[0]) <= std::fabs(mid.mV[VZ] - z[1]) ? 0 : 1;
                    F32 u = 0.f;
                    if (std::fabs(mid.mV[VZ] - z[e]) <= FLIGHT_END_DZ_M && pointSeg(mid, a[e], b[e], u) <= FLIGHT_END_REACH_M)
                    {
                        found[e].insert(static_cast<U64>(link.ref));
                    }
                }
            }
            // Touching an end edge in plan at its height, else out past it across the erosion gap.
            for (S32 e = 0; e < 2; ++e)
            {
                const LLVector3 m = (a[e] + b[e]) * 0.5f, o = (a[1 - e] + b[1 - e]) * 0.5f;
                LLVector2 away(m.mV[VX] - o.mV[VX], m.mV[VY] - o.mV[VY]);
                away.normVec();
                for (const F32 reach : { FLIGHT_END_TOUCH_M, FLIGHT_END_PAST_M })
                {
                    if (!found[e].empty())
                    {
                        break;
                    }
                    const F32 x0 = std::min(a[e].mV[VX], b[e].mV[VX]) - reach, x1 = std::max(a[e].mV[VX], b[e].mV[VX]) + reach;
                    const F32 y0 = std::min(a[e].mV[VY], b[e].mV[VY]) - reach, y1 = std::max(a[e].mV[VY], b[e].mV[VY]) + reach;
                    for (const Info& in : polys)
                    {
                        if ((in.mOwned && in.mFlight == key) || in.mX1 < x0 || in.mX0 > x1 || in.mY1 < y0 || in.mY0 > y1)
                        {
                            continue;
                        }
                        const F32 cx = (in.mX0 + in.mX1) * 0.5f - m.mV[VX], cy = (in.mY0 + in.mY1) * 0.5f - m.mV[VY];
                        if (reach > FLIGHT_END_TOUCH_M && cx * away.mV[0] + cy * away.mV[1] <= 0.f)
                        {
                            continue;   // past the end means out beyond it
                        }
                        F32 pz = 0.f;
                        if (touch(in.mTile, *in.mPoly, a[e], b[e], pz) <= reach && std::fabs(pz - z[e]) <= FLIGHT_END_DZ_M)
                        {
                            found[e].insert(in.mRef);
                        }
                    }
                }
            }
            for (S32 e = 0; e < 2; ++e)
            {
                fe.mPolys[e].assign(found[e].begin(), found[e].end());
                if (!fe.mPolys[e].empty())
                {
                    continue;
                }
                bool reaches = false;
                for (size_t k = 0; mine != own.end() && k < mine->second.size() && !reaches; ++k)
                {
                    const Info& in = polys[mine->second[k]];
                    for (S32 v = 0; v < in.mPoly->vertCount && !reaches; ++v)
                    {
                        reaches = std::fabs(vert(in.mTile, *in.mPoly, v).mV[VZ] - z[e]) <= FLIGHT_END_DZ_M;
                    }
                }
                // Out past the end, away from the other one.
                const LLVector3 m = (a[e] + b[e]) * 0.5f, o = (a[1 - e] + b[1 - e]) * 0.5f;
                LLVector2 away(m.mV[VX] - o.mV[VX], m.mV[VY] - o.mV[VY]);
                away.normVec();
                const bool edge = !on_tiles(m.mV[VX], m.mV[VY]) || !on_tiles(m.mV[VX] + away.mV[0] * TILE_PROBE_M, m.mV[VY] + away.mV[1] * TILE_PROBE_M);
                fe.mGap[e] = mine == own.end() ? EndGap::ERODED : !reaches ? EndGap::SHORT : edge ? EndGap::TILE_EDGE : EndGap::NO_FLOOR;
            }
            out.push_back(std::move(fe));
        }
    }

    const char* endGapName(EndGap g)
    {
        static const char* const NAMES[] = { "", "eroded", "short", "tile_edge", "no_floor" };
        return NAMES[static_cast<S32>(g)];
    }
}
