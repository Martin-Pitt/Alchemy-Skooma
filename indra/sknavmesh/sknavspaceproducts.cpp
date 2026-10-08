/**
 * @file sknavspaceproducts.cpp
 * @brief What the navmesh makes of its spaces: walls with a space on each side, and storeys and their connectors.
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

#include "sknavbuild.h"
#include "sknavstamp.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace sknav
{
    namespace
    {
        constexpr F32 STEP_M = 0.0625f;     ///< half a fine air cell: no column is stepped over
        constexpr F32 SAME_FLOOR_M = 0.3f;  ///< a gap whose floor is this close to the segment's is its own floor still

        /// Recast polys wind one way, so a segment's floor lies on the same side of A to B every time.
        constexpr bool SEGMENT_FLOOR_LEFT = true;

        U32 polyIndexBase(const dtNavMesh& nav, std::vector<U32>& base)
        {
            const S32 ntiles = nav.getMaxTiles();
            base.assign(static_cast<size_t>(ntiles) + 1, 0);
            for (S32 t = 0; t < ntiles; ++t)
            {
                const dtMeshTile* tile = nav.getTile(t);
                base[t + 1] = base[t] + (tile && tile->header ? static_cast<U32>(tile->header->polyCount) : 0u);
            }
            return base[ntiles];
        }

        LLVector3 polyVert(const dtMeshTile* tile, const dtPoly& poly, S32 v)
        {
            return fromRecast(&tile->verts[poly.verts[v] * 3]);
        }

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

    S32 StoreyProduct::storeyOf(U32 space) const
    {
        const auto it = std::lower_bound(mSpaceStorey.begin(), mSpaceStorey.end(), std::make_pair(space, 0u));
        return it != mSpaceStorey.end() && it->first == space ? static_cast<S32>(it->second) : -1;
    }

    S32 StoreyProduct::storeyOfPoly(U64 ref) const
    {
        const auto it = std::lower_bound(mPolyStorey.begin(), mPolyStorey.end(), std::make_pair(ref, 0u));
        return it != mPolyStorey.end() && it->first == ref ? static_cast<S32>(it->second) : -1;
    }

    void buildStoreys(const dtNavMesh& nav, const PolySpaceFn& space_of, const RoomsProduct& rooms, StoreyProduct& out)
    {
        out = StoreyProduct();
        std::vector<U32> base;
        const U32 n = polyIndexBase(nav, base);
        std::unordered_set<U32> flight_spaces;
        std::unordered_map<U32, U32> space_index;
        for (size_t i = 0; i < rooms.mSpaces.size(); ++i)
        {
            space_index[rooms.mSpaces[i].mId] = static_cast<U32>(i);
            if (rooms.mSpaces[i].mFlight)
            {
                flight_spaces.insert(rooms.mSpaces[i].mId);
            }
        }

        std::vector<U32> space(n, 0), root(n);
        std::vector<U8> kind(n, 0);     // 0 not ground, 1 floor, 2 flight
        std::vector<F32> height(n, 0.f), area(n, 0.f);
        for (U32 i = 0; i < n; ++i)
        {
            root[i] = i;
        }
        const S32 ntiles = nav.getMaxTiles();
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                const U32 i = base[t] + static_cast<U32>(p);
                if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3)
                {
                    continue;
                }
                space[i] = space_of ? space_of(tile, static_cast<U32>(p)) : 0;
                kind[i] = (poly.flags & POLY_FLIGHT) || flight_spaces.count(space[i]) ? 2 : 1;
                F32 z = 0.f, a2 = 0.f;
                const LLVector3 v0 = polyVert(tile, poly, 0);
                for (S32 v = 0; v < poly.vertCount; ++v)
                {
                    const LLVector3 a = polyVert(tile, poly, v), b = polyVert(tile, poly, (v + 1) % poly.vertCount);
                    z += a.mV[VZ];
                    a2 += (a.mV[VX] - v0.mV[VX]) * (b.mV[VY] - v0.mV[VY]) - (b.mV[VX] - v0.mV[VX]) * (a.mV[VY] - v0.mV[VY]);
                }
                height[i] = z / poly.vertCount;
                area[i] = std::fabs(a2) * 0.5f;
                ++out.mPolys;
                out.mFlightPolys += kind[i] == 2 ? 1 : 0;
            }
        }
        const auto find = [&root](U32 i) {
            while (root[i] != i)
            {
                i = root[i] = root[root[i]];
            }
            return i;
        };
        // Each poly's neighbours across Detour's links, as (index, the shared edge's middle).
        const auto each_link = [&](S32 t, S32 p, const std::function<void(U32, const LLVector3&)>& fn) {
            const dtMeshTile* tile = nav.getTile(t);
            const dtPoly& poly = tile->polys[p];
            for (U32 k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
            {
                const dtLink& link = tile->links[k];
                unsigned int salt = 0, it = 0, ip = 0;
                nav.decodePolyId(link.ref, salt, it, ip);
                if (it >= static_cast<unsigned int>(ntiles))
                {
                    continue;
                }
                const LLVector3 a = polyVert(tile, poly, link.edge), b = polyVert(tile, poly, (link.edge + 1) % poly.vertCount);
                fn(base[it] + ip, (a + b) * 0.5f);
            }
        };
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const U32 i = base[t] + static_cast<U32>(p);
                if (!kind[i])
                {
                    continue;
                }
                each_link(t, p, [&](U32 j, const LLVector3&) {
                    // Floors join floors and flights join flights; a storey never steps onto a flight.
                    if (j < n && kind[j] == kind[i])
                    {
                        const U32 a = find(i), b = find(j);
                        root[std::max(a, b)] = std::min(a, b);
                    }
                });
            }
        }

        // Storeys: the floor components, lowest floor first.
        std::map<U32, std::vector<U32>> comps;
        for (U32 i = 0; i < n; ++i)
        {
            if (kind[i] == 1)
            {
                comps[find(i)].push_back(i);
            }
        }
        struct Built
        {
            Storey mStorey;
            U32 mRoot = 0;
        };
        std::vector<Built> built;
        for (auto& [r, polys] : comps)
        {
            Built b;
            b.mRoot = r;
            std::sort(polys.begin(), polys.end(), [&](U32 a, U32 c) { return height[a] < height[c] || (height[a] == height[c] && a < c); });
            F32 total = 0.f;
            for (const U32 i : polys)
            {
                total += area[i];
            }
            F32 acc = 0.f;
            b.mStorey.mFloorZ = height[polys.front()];
            for (const U32 i : polys)
            {
                acc += area[i];
                if (acc >= total * 0.5f)
                {
                    b.mStorey.mFloorZ = height[i];
                    break;
                }
            }
            b.mStorey.mArea = total;
            built.push_back(std::move(b));
        }
        std::sort(built.begin(), built.end(), [](const Built& a, const Built& b) {
            return a.mStorey.mFloorZ < b.mStorey.mFloorZ || (a.mStorey.mFloorZ == b.mStorey.mFloorZ && a.mRoot < b.mRoot);
        });
        std::unordered_map<U32, U32> storey_of_root;
        for (size_t s = 0; s < built.size(); ++s)
        {
            storey_of_root[built[s].mRoot] = static_cast<U32>(s);
            out.mStoreys.push_back(built[s].mStorey);
            out.mStoreys.back().mMin.set(1e30f, 1e30f, 1e30f);
            out.mStoreys.back().mMax.set(-1e30f, -1e30f, -1e30f);
        }
        // Bounds, and each space's storey by where most of its poly area lies.
        std::map<U32, std::map<U32, F32>> space_area;
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const U32 i = base[t] + static_cast<U32>(p);
                if (kind[i] != 1)
                {
                    continue;
                }
                const U32 s = storey_of_root[find(i)];
                out.mPolyStorey.emplace_back(static_cast<U64>(nav.getPolyRefBase(tile) | static_cast<dtPolyRef>(p)), s);
                Storey& st = out.mStoreys[s];
                const dtPoly& poly = tile->polys[p];
                for (S32 v = 0; v < poly.vertCount; ++v)
                {
                    const LLVector3 a = polyVert(tile, poly, v);
                    for (S32 k = 0; k < 3; ++k)
                    {
                        st.mMin.mV[k] = std::min(st.mMin.mV[k], a.mV[k]);
                        st.mMax.mV[k] = std::max(st.mMax.mV[k], a.mV[k]);
                    }
                }
                if (space[i])
                {
                    space_area[space[i]][s] += area[i];
                }
            }
        }
        for (const auto& [sp, by] : space_area)
        {
            U32 best = 0;
            F32 most = -1.f;
            for (const auto& [s, a] : by)
            {
                if (a > most)
                {
                    most = a;
                    best = s;
                }
            }
            out.mSpaceStorey.emplace_back(sp, best);
            out.mStoreys[best].mSpaces.push_back(sp);
        }
        std::sort(out.mSpaceStorey.begin(), out.mSpaceStorey.end());
        std::sort(out.mPolyStorey.begin(), out.mPolyStorey.end());
        for (Storey& st : out.mStoreys)
        {
            st.mId = st.mSpaces.empty() ? 0 : st.mSpaces.front();
        }

        // Connectors: each flight's polys, the storeys they touch, and where.
        struct Touch
        {
            LLVector3 mSum;
            U32 mCount = 0;
        };
        std::map<U32, std::map<U32, Touch>> touches;     // flight root -> storey -> link edges
        std::map<U32, std::map<U32, F32>> flight_space;  // flight root -> space -> area
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const U32 i = base[t] + static_cast<U32>(p);
                if (kind[i] != 2)
                {
                    continue;
                }
                const U32 r = find(i);
                if (space[i])
                {
                    flight_space[r][space[i]] += area[i];
                }
                each_link(t, p, [&](U32 j, const LLVector3& mid) {
                    if (j < n && kind[j] == 1)
                    {
                        Touch& tc = touches[r][storey_of_root[find(j)]];
                        tc.mSum += mid;
                        ++tc.mCount;
                    }
                });
            }
        }
        for (const auto& [r, by_storey] : touches)
        {
            if (by_storey.size() < 2)
            {
                continue;
            }
            U32 fspace = 0;
            F32 most = 0.f;
            if (const auto fs = flight_space.find(r); fs != flight_space.end())
            {
                for (const auto& [sp, a] : fs->second)
                {
                    if (a > most)
                    {
                        most = a;
                        fspace = sp;
                    }
                }
            }
            // The flight space's stair portals, by the storey of the space across each.
            std::map<U32, LLVector3> portal_end;
            if (const auto si = space_index.find(fspace); si != space_index.end())
            {
                const Space& fsp = rooms.mSpaces[si->second];
                for (const U32 pi : fsp.mPortals)
                {
                    if (pi >= rooms.mPortals.size() || rooms.mPortals[pi].mKind != PortalKind::STAIR)
                    {
                        continue;
                    }
                    const Portal& portal = rooms.mPortals[pi];
                    const U32 other = portal.mSpaces[0] == si->second ? portal.mSpaces[1] : portal.mSpaces[0];
                    if (other >= rooms.mSpaces.size())
                    {
                        continue;
                    }
                    const S32 s = out.storeyOf(rooms.mSpaces[other].mId);
                    if (s >= 0 && by_storey.count(static_cast<U32>(s)) && !portal_end.count(static_cast<U32>(s)))
                    {
                        portal_end[static_cast<U32>(s)] = portal.mCentre;
                    }
                }
            }
            // Storeys are numbered lowest first, so the map's order joins each to the next one up.
            std::vector<U32> order;
            for (const auto& entry : by_storey)
            {
                order.push_back(entry.first);
            }
            for (size_t k = 1; k < order.size(); ++k)
            {
                StoreyConnector c;
                c.mSpace = fspace;
                c.mPortals = true;
                for (S32 e = 0; e < 2; ++e)
                {
                    const U32 s = order[k - 1 + e];
                    c.mStorey[e] = s;
                    if (const auto pe = portal_end.find(s); pe != portal_end.end())
                    {
                        c.mEnd[e] = pe->second;
                    }
                    else
                    {
                        const Touch& tc = by_storey.at(s);
                        c.mEnd[e] = tc.mSum / static_cast<F32>(tc.mCount);
                        c.mPortals = false;
                    }
                }
                out.mConnectors.push_back(c);
            }
        }
    }
}
