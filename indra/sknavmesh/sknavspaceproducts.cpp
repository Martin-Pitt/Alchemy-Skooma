/**
 * @file sknavspaceproducts.cpp
 * @brief What the navmesh makes of its spaces: walls with a space on each side, storeys and their connectors, the sheet's
 *        cells by space, and a space's floor from its polys.
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
        constexpr U32 NO_STOREY = 0xffffffffu;
    }

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

        U64 rasterKey(F32 x, F32 y, F32 z)
        {
            // Space::mRaster's key: 0.5 m by 0.5 m by 1 m cells, biased by 2^20 per axis.
            const S64 bias = 1ll << 20;
            const U64 kx = static_cast<U64>(static_cast<S64>(std::floor(x / 0.5f)) + bias) & 0x1fffff;
            const U64 ky = static_cast<U64>(static_cast<S64>(std::floor(y / 0.5f)) + bias) & 0x1fffff;
            const U64 kz = static_cast<U64>(static_cast<S64>(std::floor(z)) + bias) & 0x1fffff;
            return (kx << 42) | (ky << 21) | kz;
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

    const FlightEnds* StoreyProduct::flightEnds(U64 key) const
    {
        const auto it = std::lower_bound(mFlightEnds.begin(), mFlightEnds.end(), key, [](const FlightEnds& f, U64 k) { return f.mFlight < k; });
        return it != mFlightEnds.end() && it->mFlight == key ? &*it : nullptr;
    }

    constexpr F32 CLIMB_END_STEP_M = 0.4f;     ///< a climb's floor is looked for this far past each end
    constexpr F32 CLIMB_END_REACH_M = 1.5f;    ///< within this in plan
    constexpr F32 CLIMB_END_DZ_M = 1.f;        ///< and this in height

    void buildStoreys(const dtNavMesh& nav, const PolySpaceFn& space_of, const RoomsProduct& rooms, StoreyProduct& out,
                      const PolyFlightFn& flight_of, F32 min_area, const std::vector<const Flight*>& flights)
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
                // Links to off-mesh connections carry no edge: a drop is no floor's neighbour.
                if (it >= static_cast<unsigned int>(ntiles) || link.edge >= poly.vertCount)
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

        // Floors a flight steps onto, which stay storeys however small: a landing is one.
        std::unordered_set<U32> landings;
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
            {
                if (kind[base[t] + static_cast<U32>(p)] == 2)
                {
                    each_link(t, p, [&](U32 j, const LLVector3&) {
                        if (j < n && kind[j] == 1)
                        {
                            landings.insert(find(j));
                        }
                    });
                }
            }
        }

        // So do the floors a Flight's ends meet, linked or not.
        flightEnds(nav, flights, flight_of, out.mFlightEnds);
        for (const FlightEnds& fe : out.mFlightEnds)
        {
            for (const std::vector<U64>& refs : fe.mPolys)
            {
                for (const U64 ref : refs)
                {
                    unsigned int salt = 0, it = 0, ip = 0;
                    nav.decodePolyId(static_cast<dtPolyRef>(ref), salt, it, ip);
                    const U32 i = it < static_cast<unsigned int>(ntiles) ? base[it] + ip : n;
                    if (i < n && kind[i] == 1)
                    {
                        landings.insert(find(i));
                    }
                }
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
            // A scrap of floor no flight reaches and no space holds is no storey: its polys get none, as a flight's do.
            const bool spaced = std::any_of(polys.begin(), polys.end(), [&](U32 i) { return space[i] != 0; });
            if (total >= min_area || landings.count(r) || spaced)
            {
                built.push_back(std::move(b));
            }
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
                const auto sr = kind[i] == 1 ? storey_of_root.find(find(i)) : storey_of_root.end();
                if (sr == storey_of_root.end())
                {
                    continue;
                }
                const U32 s = sr->second;
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

        // Flight polys: the storeys each connected run of them touches, and where.
        struct Touch
        {
            LLVector3 mSum;
            U32 mCount = 0;
        };
        std::map<U32, std::map<U32, Touch>> touches;     // flight root -> storey -> link edges
        std::map<U32, std::map<U32, F32>> flight_space;  // flight root -> space -> area
        std::map<U32, std::map<const Flight*, U32>> flight_polys;  // flight root -> Flight -> polys
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
                if (const Flight* f = flight_of ? flight_of(tile, static_cast<U32>(p)) : nullptr)
                {
                    ++flight_polys[r][f];
                }
                each_link(t, p, [&](U32 j, const LLVector3& mid) {
                    const auto sr = j < n && kind[j] == 1 ? storey_of_root.find(find(j)) : storey_of_root.end();
                    if (sr != storey_of_root.end())
                    {
                        Touch& tc = touches[r][sr->second];
                        tc.mSum += mid;
                        ++tc.mCount;
                    }
                });
            }
        }
        // The run of flight polys most of each Flight's own polys lie in.
        std::unordered_map<U64, U32> root_of_flight;
        std::unordered_map<U64, U32> root_polys;
        for (const auto& [r, by_flight] : flight_polys)
        {
            for (const auto& [f, count] : by_flight)
            {
                U32& most = root_polys[f->mKey];
                if (count > most && touches.count(r))
                {
                    most = count;
                    root_of_flight[f->mKey] = r;
                }
            }
        }

        // Each Flight's connector between the storeys its ends meet.
        std::unordered_map<U64, const Flight*> flight_by_key;
        std::unordered_map<U64, std::map<U32, F32>> flight_space_area;
        for (S32 t = 0; flight_of && t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
            {
                if (const Flight* f = flight_of(tile, static_cast<U32>(p)))
                {
                    flight_by_key.emplace(f->mKey, f);
                    const U32 i = base[t] + static_cast<U32>(p);
                    if (space[i])
                    {
                        flight_space_area[f->mKey][space[i]] += area[i];
                    }
                }
            }
        }
        for (const Flight* f : flights)
        {
            if (f)
            {
                flight_by_key.emplace(f->mKey, f);
            }
        }
        for (FlightEnds& fe : out.mFlightEnds)
        {
            for (S32 e = 0; e < 2; ++e)
            {
                std::map<S32, U32> votes;
                for (const U64 ref : fe.mPolys[e])
                {
                    const S32 s = out.storeyOfPoly(ref);
                    if (s >= 0)
                    {
                        ++votes[s];
                    }
                }
                U32 most = 0;
                for (const auto& [s, count] : votes)
                {
                    if (count > most)
                    {
                        most = count;
                        fe.mStorey[e] = s;
                    }
                }
            }
            const auto fk = flight_by_key.find(fe.mFlight);
            if (fk == flight_by_key.end())
            {
                continue;
            }
            const Flight& fl = *fk->second;
            // An end meeting no storey's poly (a landing in a flight space, or nothing) takes the storey its run of flight polys touches nearest that end, not the other end's.
            S32 st[2] = { fe.mStorey[0], fe.mStorey[1] };
            const auto rf = root_of_flight.find(fl.mKey);
            for (S32 e = 0; e < 2; ++e)
            {
                const auto tr = rf != root_of_flight.end() && st[e] < 0 ? touches.find(rf->second) : touches.end();
                F32 best_d = 1e30f;
                for (const auto& [s, tc] : tr != touches.end() ? tr->second : std::map<U32, Touch>())
                {
                    const F32 d = dist_vec(tc.mSum / static_cast<F32>(tc.mCount), e == 0 ? fl.mBottom : fl.mTop);
                    if (d < best_d && static_cast<S32>(s) != st[1 - e])
                    {
                        best_d = d;
                        st[e] = static_cast<S32>(s);
                    }
                }
            }
            const bool guessed = st[0] != fe.mStorey[0] || st[1] != fe.mStorey[1];
            if (st[0] < 0 || st[1] < 0 || (guessed && st[0] == st[1]))
            {
                continue;
            }
            StoreyConnector c;
            c.mFlight = fl.mKey;
            c.mKind = fl.mKind;
            F32 most = 0.f;
            if (const auto fs = flight_space_area.find(fl.mKey); fs != flight_space_area.end())
            {
                for (const auto& [sp, a] : fs->second)
                {
                    if (a > most)
                    {
                        most = a;
                        c.mSpace = sp;
                    }
                }
            }
            const bool flip = out.mStoreys[st[0]].mFloorZ > out.mStoreys[st[1]].mFloorZ;
            c.mStorey[0] = static_cast<U32>(flip ? st[1] : st[0]);
            c.mStorey[1] = static_cast<U32>(flip ? st[0] : st[1]);
            c.mEnd[0] = flip ? fl.mTop : fl.mBottom;
            c.mEnd[1] = flip ? fl.mBottom : fl.mTop;
            out.mConnectors.push_back(c);
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
            // A known flight: the storeys nearest its two ends, joined at its ends.
            const Flight* fl = nullptr;
            U32 fl_polys = 0;
            if (const auto fp = flight_polys.find(r); fp != flight_polys.end())
            {
                for (const auto& [f, count] : fp->second)
                {
                    if (count > fl_polys || (count == fl_polys && fl && f->mKey < fl->mKey))
                    {
                        fl = f;
                        fl_polys = count;
                    }
                }
            }
            if (fl)
            {
                continue;   // joined at its own ends above
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

        // Climbs: the floor polys just past each of a climb's own ends give the storeys and spaces it joins.
        std::map<U64, const Flight*> climbs;
        for (S32 t = 0; flight_of && t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
            {
                const Flight* f = flight_of(tile, static_cast<U32>(p));
                if (f && f->mKind == ConnectorKind::CLIMB)
                {
                    climbs[f->mKey] = f;
                }
            }
        }
        // Flight polys are never floors, so the climb's own are passed over.
        const auto floorPast = [&](const LLVector3& at) {
            U32 best = n;
            F32 best_score = CLIMB_END_REACH_M + CLIMB_END_DZ_M;
            for (S32 t = 0; t < ntiles; ++t)
            {
                const dtMeshTile* tile = nav.getTile(t);
                for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
                {
                    const U32 i = base[t] + static_cast<U32>(p);
                    if (kind[i] != 1)
                    {
                        continue;
                    }
                    const dtPoly& poly = tile->polys[p];
                    F32 d = 1e30f, wz = 0.f, w = 0.f;
                    bool inside = true;
                    for (S32 v = 0; v < poly.vertCount; ++v)
                    {
                        const LLVector3 a = polyVert(tile, poly, v), b = polyVert(tile, poly, (v + 1) % poly.vertCount);
                        const LLVector2 e(b.mV[VX] - a.mV[VX], b.mV[VY] - a.mV[VY]), to(at.mV[VX] - a.mV[VX], at.mV[VY] - a.mV[VY]);
                        inside = inside && e.mV[VX] * to.mV[VY] - e.mV[VY] * to.mV[VX] >= 0.f;
                        const F32 len2 = e.lengthSquared();
                        const F32 u = len2 > 0.f ? llclamp((to * e) / len2, 0.f, 1.f) : 0.f;
                        d = std::min(d, (to - e * u).length());
                        const F32 k = 1.f / (LLVector2(at.mV[VX] - a.mV[VX], at.mV[VY] - a.mV[VY]).length() + 1e-3f);
                        wz += a.mV[VZ] * k;
                        w += k;
                    }
                    d = inside ? 0.f : d;
                    const F32 dz = std::fabs(wz / w - at.mV[VZ]);
                    if (d <= CLIMB_END_REACH_M && dz <= CLIMB_END_DZ_M && d + dz < best_score)
                    {
                        best_score = d + dz;
                        best = i;
                    }
                }
            }
            return best;
        };
        for (const auto& [key, fp] : climbs)
        {
            const Flight& f = *fp;
            LLVector2 run(f.mTop.mV[VX] - f.mBottom.mV[VX], f.mTop.mV[VY] - f.mBottom.mV[VY]);
            run.normVec();
            const LLVector3 step(run.mV[VX] * CLIMB_END_STEP_M, run.mV[VY] * CLIMB_END_STEP_M, 0.f);
            const U32 foot = floorPast(f.mBottom - step), top = floorPast(f.mTop + step);
            if (foot >= n || top >= n)
            {
                continue;
            }
            const auto sf = storey_of_root.find(find(foot)), st = storey_of_root.find(find(top));
            if (sf != storey_of_root.end() && st != storey_of_root.end() && sf->second != st->second)
            {
                const U32 lo = std::min(sf->second, st->second), hi = std::max(sf->second, st->second);
                const bool known = std::any_of(out.mConnectors.begin(), out.mConnectors.end(), [&](const StoreyConnector& c) {
                    return c.mFlight == key && c.mStorey[0] == lo && c.mStorey[1] == hi;
                });
                if (!known)
                {
                    StoreyConnector c;
                    c.mSpace = space[foot] == space[top] ? space[foot] : 0;
                    c.mFlight = key;
                    c.mKind = ConnectorKind::CLIMB;
                    const bool flip = sf->second != lo;
                    c.mStorey[0] = lo;
                    c.mStorey[1] = hi;
                    c.mEnd[0] = flip ? f.mTop : f.mBottom;
                    c.mEnd[1] = flip ? f.mBottom : f.mTop;
                    out.mConnectors.push_back(c);
                }
            }
            if (space[foot] && space[top] && space[foot] != space[top])
            {
                ClimbLink l;
                l.mSpace[0] = space[foot];
                l.mSpace[1] = space[top];
                l.mFlight = key;
                l.mFoot = f.mBottom;
                l.mTop = f.mTop;
                out.mClimbLinks.push_back(l);
            }
        }

        // A space with polys in no storey, a flight's own: the lower storey of its connector, else the storey its polys meet most.
        std::unordered_set<U32> placed;
        for (const auto& ss : out.mSpaceStorey)
        {
            placed.insert(ss.first);
        }
        std::map<U32, std::map<U32, U32>> meets;
        for (S32 t = 0; t < ntiles; ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            for (S32 p = 0; tile && tile->header && p < tile->header->polyCount; ++p)
            {
                const U32 i = base[t] + static_cast<U32>(p);
                if (!space[i] || placed.count(space[i]))
                {
                    continue;
                }
                std::map<U32, U32>& m = meets[space[i]];
                m.emplace(NO_STOREY, 0);
                each_link(t, p, [&](U32 j, const LLVector3&) {
                    const auto sr = j < n && kind[j] == 1 ? storey_of_root.find(find(j)) : storey_of_root.end();
                    if (sr != storey_of_root.end())
                    {
                        ++m[sr->second];
                    }
                });
            }
        }
        for (const auto& [sp, by] : meets)
        {
            U32 best = NO_STOREY, most = 0;
            for (const StoreyConnector& c : out.mConnectors)
            {
                if (c.mSpace == sp)
                {
                    best = c.mStorey[0];
                    break;
                }
            }
            for (const auto& [st, count] : by)
            {
                if (best == NO_STOREY && st != NO_STOREY && count > most)
                {
                    most = count;
                    best = st;
                }
            }
            if (best != NO_STOREY && best < out.mStoreys.size())
            {
                out.mSpaceStorey.emplace_back(sp, best);
                std::vector<U32>& list = out.mStoreys[best].mSpaces;
                list.insert(std::upper_bound(list.begin(), list.end(), sp), sp);
                out.mStoreys[best].mId = list.front();
            }
        }
        std::sort(out.mSpaceStorey.begin(), out.mSpaceStorey.end());
    }

    std::vector<U32> StoreyProduct::climbLinksOf(U32 space) const
    {
        std::vector<U32> out;
        for (size_t i = 0; i < mClimbLinks.size(); ++i)
        {
            if (mClimbLinks[i].mSpace[0] == space || mClimbLinks[i].mSpace[1] == space)
            {
                out.push_back(static_cast<U32>(i));
            }
        }
        return out;
    }

    void sheetSpaces(const EnclosureProduct& enc, const LLVector3& min, const LLVector3& max, F32 res, std::vector<SheetCell>& out)
    {
        res = std::max(res, 0.05f);
        const S32 nx = std::max(1, static_cast<S32>(std::ceil((max.mV[VX] - min.mV[VX]) / res)));
        const S32 ny = std::max(1, static_cast<S32>(std::ceil((max.mV[VY] - min.mV[VY]) / res)));
        out.assign(static_cast<size_t>(nx) * ny, SheetCell());
        for (S32 iy = 0; iy < ny; ++iy)
        {
            for (S32 ix = 0; ix < nx; ++ix)
            {
                SheetCell& cell = out[static_cast<size_t>(ix) + static_cast<size_t>(iy) * nx];
                enc.gapsAt(min.mV[VX] + (ix + 0.5f) * res, min.mV[VY] + (iy + 0.5f) * res, [&](const Enclosure& e) {
                    if (e.mFloor < min.mV[VZ] || e.mFloor > max.mV[VZ] || (cell.mHit && e.mFloor <= cell.mFloor))
                    {
                        return;
                    }
                    cell.mHit = true;
                    cell.mFloor = e.mFloor;
                    cell.mSpace = e.mSpaceId;
                });
            }
        }
    }

    std::vector<std::pair<U64, U32>> polyRaster(const std::vector<std::vector<LLVector3>>& polys)
    {
        constexpr F32 CELL = 0.125f;
        std::map<U64, U32> counts;
        for (const std::vector<LLVector3>& poly : polys)
        {
            if (poly.size() < 3)
            {
                continue;
            }
            F32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const LLVector3& v : poly)
            {
                x0 = std::min(x0, v.mV[VX]);
                y0 = std::min(y0, v.mV[VY]);
                x1 = std::max(x1, v.mV[VX]);
                y1 = std::max(y1, v.mV[VY]);
            }
            for (S32 cy = static_cast<S32>(std::floor(y0 / CELL)); cy * CELL < y1; ++cy)
            {
                for (S32 cx = static_cast<S32>(std::floor(x0 / CELL)); cx * CELL < x1; ++cx)
                {
                    const F32 px = (cx + 0.5f) * CELL, py = (cy + 0.5f) * CELL;
                    // The fan triangle holding the cell's centre gives its height.
                    for (size_t k = 1; k + 1 < poly.size(); ++k)
                    {
                        const LLVector3 &a = poly[0], &b = poly[k], &c = poly[k + 1];
                        const F32 d = (b.mV[VY] - c.mV[VY]) * (a.mV[VX] - c.mV[VX]) + (c.mV[VX] - b.mV[VX]) * (a.mV[VY] - c.mV[VY]);
                        if (std::fabs(d) < 1e-9f)
                        {
                            continue;
                        }
                        const F32 u = ((b.mV[VY] - c.mV[VY]) * (px - c.mV[VX]) + (c.mV[VX] - b.mV[VX]) * (py - c.mV[VY])) / d;
                        const F32 v = ((c.mV[VY] - a.mV[VY]) * (px - c.mV[VX]) + (a.mV[VX] - c.mV[VX]) * (py - c.mV[VY])) / d;
                        const F32 w = 1.f - u - v;
                        if (u >= 0.f && v >= 0.f && w >= 0.f)
                        {
                            const F32 z = u * a.mV[VZ] + v * b.mV[VZ] + w * c.mV[VZ];
                            ++counts[rasterKey(px, py, z)];
                            break;
                        }
                    }
                }
            }
        }
        return std::vector<std::pair<U64, U32>>(counts.begin(), counts.end());
    }

    std::vector<std::vector<LLVector3>> spacePolys(const dtNavMesh& nav, const PolySpaceFn& space_of, U32 space)
    {
        std::vector<std::vector<LLVector3>> out;
        if (!space || !space_of)
        {
            return out;
        }
        for (S32 t = 0; t < nav.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = nav.getTile(t);
            if (!tile || !tile->header)
            {
                continue;
            }
            for (S32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() != DT_POLYTYPE_GROUND || space_of(tile, static_cast<U32>(p)) != space)
                {
                    continue;
                }
                std::vector<LLVector3> outline;
                for (S32 v = 0; v < poly.vertCount; ++v)
                {
                    outline.push_back(polyVert(tile, poly, v));
                }
                out.push_back(std::move(outline));
            }
        }
        return out;
    }
}
