/**
 * @file skmapplan.cpp
 * @brief The mini-map's plan: which navmesh spaces are drawn and at what storey rank, and the geometry that draws them.
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

#include "llviewerprecompiledheaders.h"

#include "skmapplan.h"

#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "sknavmesh.h"
#include "sknavpolys.h"
#include "sknavrooms.h"

#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

using namespace skmap::plan;

namespace
{
    // Selection and style constants are proto2's (V:/Scratch/minimap-gpu/proto2 README, owner rulings 10, 15, 18, 22).
    constexpr F32 CONTOUR_STEP_M = 0.5f;        // the contour interval
    constexpr F32 DIM_ALPHA = 0.45f;            // the proto's "dimmed outside"
    constexpr F32 ROOF_SHADOW_M = 2.f;          // inner roof shadow falloff width
    constexpr F32 ROOF_SHADOW_ALPHA = 0.1f;
    constexpr S32 ROOF_SHADOW_DISC = 16;        // segments of the shadow's end discs
    constexpr F32 RANK_CLOSED_ROOF = -0.5f;     // closed buildings' roofs, under the ground plan
    constexpr F32 RANK_STOREY_STEP = 1.f / 1000.f;

    struct Rgba
    {
        F32 r, g, b, a;
    };
    constexpr Rgba rgb(F32 r, F32 g, F32 b, F32 a = 1.f) { return { r / 255.f, g / 255.f, b / 255.f, a }; }
    const Rgba COL_WALL_LINE = rgb(229, 228, 225, 1.f);
    const Rgba COL_HATCH_LINE = rgb(208, 208, 205, -0.9f);      // a negative alpha draws the half-cover hatch
    const Rgba COL_LEDGE = rgb(158, 166, 176, 0.6f);
    const Rgba COL_CONTOUR = rgb(140, 148, 158, 0.30f);
    const Rgba COL_DOOR = rgb(160, 112, 255, 1.f);
    const Rgba COL_OPENING = rgb(61, 220, 132, 1.f);
    const Rgba COL_STAIR = rgb(255, 160, 64, 1.f);
    const Rgba COL_CASING = rgb(32, 36, 40, 0.75f);
    const Rgba COL_CASING_LINE = rgb(158, 166, 176, 0.9f);
    const Rgba COL_RUNG = rgb(158, 166, 176, 0.7f);
    const Rgba COL_REGION = rgb(158, 166, 176, 0.6f);
    const Rgba COL_GAP = rgb(255, 0, 255, 1.f);
    const Rgba COL_ARROW_EDGE = rgb(25, 25, 25, 0.8f);
    const Rgba COL_ROOF_LINE = rgb(0, 0, 0, 0.4f);

    // Pixel widths (rulings 15, 18, 22).
    constexpr F32 PX_SOLID = 2.f, PX_HATCHED = 3.f, PX_LEDGE = 1.f, PX_CONTOUR = 1.f, PX_ROOF = 1.f, PX_DOOR = 3.f;
    constexpr F32 PX_CASING = 3.f, PX_CASING_LINE = 1.f, PX_RUNG = 0.8f, PX_RUNG_PITCH = 6.f, PX_CAP = 2.5f;
    constexpr F32 PX_CENTRE = 3.f, PX_ARROW_LONG = 13.f, PX_ARROW_WIDE = 11.f, PX_ARROW_EDGE = 0.8f, PX_REGION = 1.f;

    // Floor kinds the shader reads.
    constexpr F32 KIND_FLOOR = 0.f, KIND_OTHER = 2.f, KIND_GAP = 4.f;

    U64 pairKey(U32 a, U32 b) { return a < b ? (static_cast<U64>(a) << 32) | b : (static_cast<U64>(b) << 32) | a; }

    void addUnique(std::vector<U32>& v, U32 x)
    {
        if (std::find(v.begin(), v.end(), x) == v.end())
        {
            v.push_back(x);
        }
    }

    U64 mix(U64 h, U64 v)
    {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
    U64 bitsOf(F32 f)
    {
        U32 u;
        memcpy(&u, &f, sizeof(u));
        return u;
    }
}

bool Options::operator==(const Options& o) const
{
    return mOverlap == o.mOverlap && mOutside == o.mOutside && mGaps == o.mGaps && mLedges == o.mLedges && mOther == o.mOther
        && mContours == o.mContours && mRoofOutline == o.mRoofOutline && mDoors == o.mDoors && mShade == o.mShade && mTerrain == o.mTerrain
        && mRegionEdges == o.mRegionEdges && mWalls == o.mWalls && mStairs == o.mStairs;
}

void Selection::set(U32 s, F32 r)
{
    if (mRankOf[s] >= 0)
    {
        mRank[mRankOf[s]].second = r;
        return;
    }
    mRankOf[s] = static_cast<S32>(mRank.size());
    mRank.emplace_back(s, r);
}

size_t Geometry::triangles() const
{
    size_t n = mShadowClip.size() + mShadowBand.size() + mTop.size();
    for (const Rank& r : mRanks)
    {
        n += r.mFloors.size() + r.mWalls.size() + r.mWallLines.size() + r.mLines.size();
    }
    return n / 3;
}

// ---------- topology ----------

namespace
{
    std::vector<U32> flood(const Data& d, U32 start)
    {
        std::vector<U32> out{ start };
        std::vector<U8> seen(d.mSpaces.size(), 0);
        seen[start] = 1;
        std::vector<U32> q{ start };
        while (!q.empty())
        {
            const U32 c = q.back();
            q.pop_back();
            for (U32 n : d.mAdj[c])
            {
                if (!seen[n])
                {
                    seen[n] = 1;
                    out.push_back(n);
                    q.push_back(n);
                }
            }
        }
        return out;
    }

    /// The poly's floor height at (x, y), or false when the point is outside it.
    bool polyHeightAt(const Data& d, U32 poly, F32 x, F32 y, F32& z)
    {
        const LLVector3& o = d.vert(poly, 0);
        for (U32 k = 1; k + 1 < d.mPolys[poly].mCount; ++k)
        {
            const LLVector3 &b = d.vert(poly, k), &c = d.vert(poly, k + 1);
            const F32 v0x = b.mV[VX] - o.mV[VX], v0y = b.mV[VY] - o.mV[VY];
            const F32 v1x = c.mV[VX] - o.mV[VX], v1y = c.mV[VY] - o.mV[VY];
            const F32 px = x - o.mV[VX], py = y - o.mV[VY];
            const F32 den = v0x * v1y - v1x * v0y;
            if (fabsf(den) < 1e-6f)
            {
                continue;
            }
            const F32 u = (px * v1y - v1x * py) / den, v = (v0x * py - px * v0y) / den;
            if (u >= -1e-3f && v >= -1e-3f && u + v <= 1.001f)
            {
                z = o.mV[VZ] + u * (b.mV[VZ] - o.mV[VZ]) + v * (c.mV[VZ] - o.mV[VZ]);
                return true;
            }
        }
        return false;
    }

    /// Region meshes share no Detour links: an open edge of outdoor floor with another mesh's outdoor floor just
    /// across it, at the same height, joins the two spaces with an adjacent portal, so the ground floods across.
    void joinRegionSeams(Data& d)
    {
        LL_PROFILE_ZONE_NAMED("SKMapPlan.Seams");
        constexpr F32 CELL_M = 8.f;
        constexpr F32 REACH_M = 0.5f;   // across the edge, past the agent-radius erosion of both sides
        constexpr F32 CLIMB_M = 1.f;
        const auto meshOf = [&](U32 p) { return d.mSpaces[d.mPolys[p].mSpace].mMesh; };
        const auto groundPoly = [&](U32 p) { return d.mPolys[p].mSpace >= 0 && d.outdoorish(d.mPolys[p].mSpace); };
        const auto cellKey = [](S32 x, S32 y) { return (static_cast<U64>(static_cast<U32>(x)) << 32) | static_cast<U32>(y); };
        if (d.mSpaceIndex.size() < 2)
        {
            return;
        }
        std::unordered_map<U64, std::vector<U32>> grid;
        for (U32 p = 0; p < d.mPolys.size(); ++p)
        {
            if (!groundPoly(p))
            {
                continue;
            }
            F32 x0 = F32_MAX, y0 = F32_MAX, x1 = -F32_MAX, y1 = -F32_MAX;
            for (U32 k = 0; k < d.mPolys[p].mCount; ++k)
            {
                const LLVector3& v = d.vert(p, k);
                x0 = llmin(x0, v.mV[VX]), y0 = llmin(y0, v.mV[VY]), x1 = llmax(x1, v.mV[VX]), y1 = llmax(y1, v.mV[VY]);
            }
            for (S32 y = ll_round(floorf(y0 / CELL_M)); y <= ll_round(floorf(y1 / CELL_M)); ++y)
            {
                for (S32 x = ll_round(floorf(x0 / CELL_M)); x <= ll_round(floorf(x1 / CELL_M)); ++x)
                {
                    grid[cellKey(x, y)].push_back(p);
                }
            }
        }
        std::unordered_set<U64> joined;
        for (U32 p = 0; p < d.mPolys.size(); ++p)
        {
            if (!groundPoly(p))
            {
                continue;
            }
            const U32 mesh = meshOf(p);
            const S32 a = d.mPolys[p].mSpace;
            for (U32 e = 0; e < d.mPolys[p].mCount; ++e)
            {
                const S32 n = d.mNbr[d.mPolys[p].mFirst + e];
                if (n >= 0 || (n == NBR_BORDER && d.mBorder.count((static_cast<U64>(p) << 3) | e)))
                {
                    continue;
                }
                const LLVector3 &va = d.vert(p, e), &vb = d.vert(p, e + 1);
                const LLVector3 mid = (va + vb) * 0.5f;
                LLVector2 nrm(vb.mV[VY] - va.mV[VY], va.mV[VX] - vb.mV[VX]);
                if (nrm.normalize() < 1e-4f)
                {
                    continue;
                }
                // The polys' winding is not known here: probe both sides and the edge itself, where the meshes overlap.
                for (F32 side : { 0.f, REACH_M, -REACH_M })
                {
                    const F32 x = mid.mV[VX] + nrm.mV[VX] * side, y = mid.mV[VY] + nrm.mV[VY] * side;
                    const auto cell = grid.find(cellKey(ll_round(floorf(x / CELL_M)), ll_round(floorf(y / CELL_M))));
                    if (cell == grid.end())
                    {
                        continue;
                    }
                    for (U32 q : cell->second)
                    {
                        F32 z = 0.f;
                        const S32 b = d.mPolys[q].mSpace;
                        if (meshOf(q) == mesh || joined.count(pairKey(a, b)) || !polyHeightAt(d, q, x, y, z) || fabsf(z - mid.mV[VZ]) > CLIMB_M)
                        {
                            continue;
                        }
                        joined.insert(pairKey(a, b));
                        Portal out;
                        out.mKind = PortalKind::ADJACENT;
                        out.mSpaces[0] = a;
                        out.mSpaces[1] = b;
                        out.mA = va;
                        out.mB = vb;
                        d.mPortals.push_back(out);
                    }
                }
            }
        }
    }
}

void Data::finish()
{
    const size_t ns = mSpaces.size(), np = mPolys.size();
    mSpacePolys.assign(ns, {});
    for (U32 i = 0; i < np; ++i)
    {
        if (mPolys[i].mSpace >= 0)
        {
            mSpacePolys[mPolys[i].mSpace].push_back(i);
        }
    }
    mFlightOf.assign(np, -1);
    for (U32 f = 0; f < mFlights.size(); ++f)
    {
        for (U32 p : mFlights[f].mPolys)
        {
            mFlightOf[p] = static_cast<S32>(f);
        }
    }
    mRoofsOf.clear();
    for (U32 s = 0; s < ns; ++s)
    {
        if (mSpaces[s].mRoof && mSpaces[s].mBuilding)
        {
            auto it = std::find_if(mRoofsOf.begin(), mRoofsOf.end(), [&](const auto& e) { return e.first == mSpaces[s].mBuilding; });
            if (it == mRoofsOf.end())
            {
                mRoofsOf.emplace_back(mSpaces[s].mBuilding, std::vector<U32>());
                it = mRoofsOf.end() - 1;
            }
            it->second.push_back(s);
        }
    }
    mWallsOfPoly.assign(np, {});
    for (U32 w = 0; w < mWalls.size(); ++w)
    {
        for (U32 p : mWalls[w].mPolys)
        {
            mWallsOfPoly[p].push_back(w);
        }
    }

    // Space pairs whose polys share an edge with no portal between them, and space-less polys beside a space.
    std::unordered_set<U64> portal_pair, touched;
    for (const Portal& p : mPortals)
    {
        if (p.mSpaces[0] >= 0 && p.mSpaces[1] >= 0)
        {
            portal_pair.insert(pairKey(p.mSpaces[0], p.mSpaces[1]));
        }
    }
    mTouchOf.assign(ns, {});
    mNoSpaceNbr.assign(ns, {});
    for (U32 i = 0; i < np; ++i)
    {
        const Poly& p = mPolys[i];
        for (U32 e = 0; e < p.mCount; ++e)
        {
            auto visit = [&](U32 n)
            {
                const S32 a = p.mSpace, b = mPolys[n].mSpace;
                if (a >= 0 && b < 0)
                {
                    addUnique(mNoSpaceNbr[a], n);
                }
                if (a < 0 || b < 0 || a == b || portal_pair.count(pairKey(a, b)) || !touched.insert(pairKey(a, b)).second)
                {
                    return;
                }
                mTouchOf[a].push_back(b);
                mTouchOf[b].push_back(a);
            };
            const S32 n = mNbr[p.mFirst + e];
            if (n >= 0)
            {
                visit(n);
            }
            else if (n == NBR_BORDER)
            {
                const auto it = mBorder.find((static_cast<U64>(i) << 3) | e);
                if (it != mBorder.end())
                {
                    for (const BorderLink& l : it->second)
                    {
                        visit(l.mPoly);
                    }
                }
            }
        }
    }

    // Flood adjacency: portals, and climb links as level separators (ruling 21 for outdoor stairs).
    mAdj.assign(ns, {});
    mStairAdj.assign(ns, {});
    for (const Portal& p : mPortals)
    {
        const S32 a = p.mSpaces[0], b = p.mSpaces[1];
        if (a < 0 || b < 0)
        {
            continue;
        }
        auto& m = p.mKind == PortalKind::STAIR && !(outdoorish(a) && outdoorish(b)) ? mStairAdj : mAdj;
        addUnique(m[a], b);
        addUnique(m[b], a);
    }
    for (const auto& c : mClimbs)
    {
        addUnique(mStairAdj[c.first], c.second);
        addUnique(mStairAdj[c.second], c.first);
    }

    // Ground level: per region mesh, the connected set holding its largest outdoor space with polys.
    mGround.assign(ns, 0);
    mGroundOrder.clear();
    std::vector<S32> roots(mSpaceIndex.size(), -1);
    for (U32 s = 0; s < ns; ++s)
    {
        const Space& sp = mSpaces[s];
        if (sp.mBucket == Bucket::OUTDOORS && !mSpacePolys[s].empty() && (roots[sp.mMesh] < 0 || sp.mArea > mSpaces[roots[sp.mMesh]].mArea))
        {
            roots[sp.mMesh] = static_cast<S32>(s);
        }
    }
    mStreetRoom.assign(ns, 0);
    std::unordered_map<U64, std::map<S32, U32>> links;   // building -> storey -> street links
    for (S32 root : roots)
    {
        // A region whose ground joins an earlier one's across the seam is already flooded.
        if (root < 0 || mGround[root])
        {
            continue;
        }
        for (U32 s : flood(*this, root))
        {
            mGround[s] = 1;
            mGroundOrder.push_back(s);
        }
        // Street rooms: indoor spaces one door or opening off the ground's outdoor set.
        std::vector<U8> out(ns, 0);
        std::vector<U32> q{ static_cast<U32>(root) }, outdoor{ static_cast<U32>(root) };
        out[root] = 1;
        while (!q.empty())
        {
            const U32 c = q.back();
            q.pop_back();
            for (U32 n : mAdj[c])
            {
                if (!indoor(n) && !out[n])
                {
                    out[n] = 1;
                    outdoor.push_back(n);
                    q.push_back(n);
                }
            }
        }
        for (U32 s : outdoor)
        {
            for (U32 n : mAdj[s])
            {
                if (!indoor(n))
                {
                    continue;
                }
                mStreetRoom[n] = 1;
                if (mSpaces[n].mStorey >= 0)
                {
                    ++links[mSpaces[n].mBuilding][mSpaces[n].mStorey];
                }
            }
        }
    }
    // Ground storey per building (ruling 12): the most street links, the lower index on a tie.
    mGroundStorey.clear();
    for (const auto& b : links)
    {
        S32 best = -1;
        U32 most = 0;
        for (const auto& st : b.second)
        {
            if (best < 0 || st.second > most)
            {
                best = st.first;
                most = st.second;
            }
        }
        mGroundStorey[b.first] = best;
    }
    // Closed buildings: no indoor space has a door or opening to a space that is not indoors.
    std::unordered_set<U64> open;
    for (const Portal& p : mPortals)
    {
        const S32 a = p.mSpaces[0], b = p.mSpaces[1];
        if ((p.mKind == PortalKind::DOOR || p.mKind == PortalKind::OPENING) && a >= 0 && b >= 0 && indoor(a) != indoor(b))
        {
            open.insert(mSpaces[indoor(a) ? a : b].mBuilding);
        }
    }
    mNoEntry.clear();
    for (const auto& r : mRoofsOf)
    {
        if (open.count(r.first))
        {
            continue;
        }
        for (U32 s = 0; s < ns; ++s)
        {
            if (mSpaces[s].mBuilding == r.first && indoor(s))
            {
                mNoEntry.push_back(r.first);
                break;
            }
        }
    }
}

// ---------- selection ----------

namespace
{
    bool isFlightSpace(const Data& d, U32 s)
    {
        const Space& sp = d.mSpaces[s];
        if (sp.mFlight)
        {
            return true;
        }
        if (!sp.mPortals.empty()
            && std::all_of(sp.mPortals.begin(), sp.mPortals.end(), [&](U32 p) { return d.mPortals[p].mKind == PortalKind::STAIR; }))
        {
            return true;
        }
        return std::any_of(d.mSpacePolys[s].begin(), d.mSpacePolys[s].end(), [&](U32 p) { return d.mFlightOf[p] >= 0; });
    }

    S32 groundStoreyOf(const Data& d, U64 b)
    {
        const auto it = d.mGroundStorey.find(b);
        return it == d.mGroundStorey.end() ? -2 : it->second;
    }

    /// Overlap modes: a, the ground storey plus each other street room on top; b, the ground storey only; c, no
    /// interiors. Inside a building, its own storey only; on a roof not joined to the ground, the roof's flood.
    std::vector<std::pair<U32, F32>> storeyFilter(const Data& d, const std::vector<U32>& plan, S32 vs, Overlap mode)
    {
        std::vector<std::pair<U32, F32>> out;
        const S32 v_st = vs >= 0 && d.indoor(vs) ? d.mSpaces[vs].mStorey : -1;
        if (vs >= 0 && d.mSpaces[vs].mRoof && std::none_of(plan.begin(), plan.end(), [&](U32 s) { return d.mGround[s] != 0; }))
        {
            for (U32 s : plan)
            {
                out.emplace_back(s, 1.f);
            }
            return out;
        }
        for (U32 s : plan)
        {
            if (!d.indoor(s))
            {
                out.emplace_back(s, 0.f);
                continue;
            }
            const S32 st = d.mSpaces[s].mStorey;
            const U64 b = d.mSpaces[s].mBuilding;
            if (vs >= 0 && d.indoor(vs) && b == d.mSpaces[vs].mBuilding)
            {
                if (static_cast<S32>(s) == vs || (v_st >= 0 && st == v_st))
                {
                    out.emplace_back(s, 1.f);
                }
                continue;
            }
            if (mode == Overlap::ROOFS)
            {
                continue;
            }
            if (st >= 0 && st == groundStoreyOf(d, b))
            {
                out.emplace_back(s, 0.f);
            }
            else if (mode == Overlap::STOREY && d.mStreetRoom[s])
            {
                out.emplace_back(s, 1.f);
            }
        }
        return out;
    }

    void maxInto(std::vector<std::pair<U32, F32>>& v, U32 k, F32 r)
    {
        for (auto& e : v)
        {
            if (e.first == k)
            {
                e.second = llmax(e.second, r);
                return;
            }
        }
        v.emplace_back(k, r);
    }
}

Selection skmap::plan::select(const Data& d, S32 vs, const Options& opts, bool standing)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.Select");
    Selection sel;
    const U32 ns = static_cast<U32>(d.mSpaces.size());
    sel.mRankOf.assign(ns, -1);
    sel.mDim.assign(ns, 0);
    std::vector<U8> ground_why(ns, 0);
    std::vector<U32> no_storey, roof_gaps;
    const Overlap mode = opts.mOverlap;
    // Standing on an outdoor space the ground level does not hold: a raised deck or balcony.
    const bool open_air = vs >= 0 && (d.mSpaces[vs].mBucket == Bucket::OUTDOORS || d.mSpaces[vs].mBucket == Bucket::SHELTERED);
    const S32 deck = standing && open_air && !d.mGround[vs] ? vs : -1;
    // Outdoors (or nowhere) there is no current storey: the plan is the ground and the buildings by the overlap mode,
    // with nothing highlighted or dimmed. Only an indoor space switches the map to its building's storey.
    if (!d.indoor(vs))
    {
        vs = -1;
    }
    const bool outdoors = vs < 0;
    U32 viewer_mesh = U32_MAX;
    if (vs >= 0)
    {
        viewer_mesh = d.mSpaces[vs].mMesh;
        const std::vector<U32> plan = flood(d, vs);
        for (U32 s : plan)
        {
            if (d.indoor(s) && d.mSpaces[s].mStorey < 0 && static_cast<S32>(s) != vs)
            {
                no_storey.push_back(s);
            }
        }
        sel.mOnGround = std::any_of(plan.begin(), plan.end(), [&](U32 s) { return d.mGround[s] != 0; });
        sel.mInside = d.indoor(vs);
        for (const auto& e : storeyFilter(d, plan, vs, mode))
        {
            sel.set(e.first, sel.mOnGround ? e.second : 1.f);
        }
    }
    // Off the ground level (or off every space), the ground outside under the plan (ruling 9); other region meshes'
    // ground always, as each region is its own build.
    if (opts.mOutside != Outside::NONE || sel.mOnGround || outdoors)
    {
        for (const auto& e : storeyFilter(d, d.mGroundOrder, -1, mode))
        {
            const bool own = d.mSpaces[e.first].mMesh == viewer_mesh;
            if ((own && sel.mOnGround) || sel.has(e.first) || (!outdoors && !own && !sel.mOnGround && opts.mOutside == Outside::NONE))
            {
                continue;
            }
            sel.set(e.first, 0.f);
            ground_why[e.first] = !outdoors && (own || !sel.mOnGround);
        }
    }
    // On a deck: its outdoor set, and through its DOOR and OPENING portals each indoor room's storey, drawn as a street
    // entrance's storey is (owner ruling 2026-10-14); the deck stays outdoors, so nothing is highlighted or dimmed.
    std::vector<U64> entered;
    const auto isEntered = [&](U64 b) { return std::find(entered.begin(), entered.end(), b) != entered.end(); };
    if (deck >= 0)
    {
        std::vector<U8> seen(ns, 0);
        std::vector<U32> q{ static_cast<U32>(deck) }, open{ static_cast<U32>(deck) };
        seen[deck] = 1;
        while (!q.empty())
        {
            const U32 c = q.back();
            q.pop_back();
            for (U32 n : d.mAdj[c])
            {
                if (!seen[n] && !d.indoor(n))
                {
                    seen[n] = 1;
                    open.push_back(n);
                    q.push_back(n);
                }
            }
        }
        std::set<std::pair<U64, S32>> storeys;
        for (U32 s : open)
        {
            sel.set(s, 1.f);
            for (U32 pi : d.mSpaces[s].mPortals)
            {
                const Portal& p = d.mPortals[pi];
                const S32 room = p.mSpaces[0] == static_cast<S32>(s) ? p.mSpaces[1] : p.mSpaces[0];
                if ((p.mKind != PortalKind::DOOR && p.mKind != PortalKind::OPENING) || !d.indoor(room) || seen[room])
                {
                    continue;
                }
                seen[room] = 1;
                const U64 b = d.mSpaces[room].mBuilding;
                const S32 st = d.mSpaces[room].mStorey;
                sel.set(room, 1.f);
                if (b && !isEntered(b))
                {
                    entered.push_back(b);
                }
                if (st < 0 || !storeys.insert({ b, st }).second)
                {
                    continue;
                }
                for (U32 m : flood(d, room))
                {
                    if (d.indoor(m) && d.mSpaces[m].mBuilding == b && d.mSpaces[m].mStorey == st)
                    {
                        sel.set(m, 1.f);
                    }
                }
            }
        }
    }
    // Mode c: every other building's roofs, higher covering lower by its storeys' floor order. Modes a/b: a building with
    // no entrance shows its roofs under the ground plan.
    const U64 vb = vs >= 0 ? d.mSpaces[vs].mBuilding : 0;
    for (const auto& br : d.mRoofsOf)
    {
        const U64 b = br.first;
        const bool closed = mode != Overlap::ROOFS && std::find(d.mNoEntry.begin(), d.mNoEntry.end(), b) != d.mNoEntry.end();
        if ((vs >= 0 && b == vb) || isEntered(b) || (mode != Overlap::ROOFS && !closed))
        {
            continue;
        }
        std::vector<S32> sts;
        for (U32 r : br.second)
        {
            const S32 st = d.mSpaces[r].mStorey;
            if (st >= 0 && std::find(sts.begin(), sts.end(), st) == sts.end())
            {
                sts.push_back(st);
            }
        }
        std::stable_sort(sts.begin(), sts.end(), [&](S32 a, S32 c) { return d.mStoreyFloor[a] < d.mStoreyFloor[c]; });
        for (U32 r : br.second)
        {
            if (sel.has(r))
            {
                continue;
            }
            const S32 st = d.mSpaces[r].mStorey;
            if (st < 0)
            {
                if (!d.mSpacePolys[r].empty())
                {
                    roof_gaps.push_back(r);
                }
                continue;
            }
            const F32 order = static_cast<F32>(std::find(sts.begin(), sts.end(), st) - sts.begin() + 1);
            sel.set(r, (closed ? RANK_CLOSED_ROOF : 1.f) + order * RANK_STOREY_STEP);
        }
    }
    if (mode == Overlap::ROOFS)
    {
        for (U32 s = 0; s < ns; ++s)
        {
            if (d.mSpaces[s].mRoof && !d.mSpaces[s].mBuilding && !sel.has(s) && !d.mSpacePolys[s].empty())
            {
                roof_gaps.push_back(s);
            }
        }
    }
    // Flights: a STAIR neighbour of a drawn space that is a flight space, at that space's rank (ruling 15).
    const std::vector<std::pair<U32, F32>> drawn = sel.mRank;
    for (const auto& e : drawn)
    {
        for (U32 n : d.mStairAdj[e.first])
        {
            if (!sel.has(n) && isFlightSpace(d, n))
            {
                sel.set(n, e.second);
            }
        }
    }
    if (!sel.mOnGround && opts.mOutside == Outside::DIM)
    {
        for (U32 s = 0; s < ns; ++s)
        {
            sel.mDim[s] = ground_why[s] && sel.has(s);
        }
    }
    // Flight records with a poly in a drawn space, at the highest such rank.
    for (U32 f = 0; f < d.mFlights.size(); ++f)
    {
        F32 r = -1.f;
        for (U32 p : d.mFlights[f].mPolys)
        {
            const S32 s = d.mPolys[p].mSpace;
            if (sel.has(s))
            {
                r = llmax(r, sel.rank(s));
            }
        }
        if (r >= 0.f)
        {
            sel.mFlights.emplace_back(f, r);
        }
    }
    // Other level (ruling 15): STAIR neighbours of drawn spaces that are not drawn.
    if (opts.mOther)
    {
        for (const auto& e : sel.mRank)
        {
            for (U32 n : d.mStairAdj[e.first])
            {
                if (!sel.has(n))
                {
                    addUnique(sel.mOther, n);
                }
            }
        }
    }
    // Gaps: portal-less neighbours, space-less polys beside drawn floors, storey-less plan rooms, undecided roofs.
    for (const auto& e : sel.mRank)
    {
        for (U32 n : d.mTouchOf[e.first])
        {
            if (!sel.has(n))
            {
                maxInto(sel.mGapSpaces, n, e.second);
            }
        }
        for (U32 p : d.mNoSpaceNbr[e.first])
        {
            maxInto(sel.mGapPolys, p, e.second);
        }
    }
    for (U32 s : no_storey)
    {
        if (!sel.has(s))
        {
            maxInto(sel.mGapSpaces, s, 0.f);
        }
    }
    for (U32 s : roof_gaps)
    {
        maxInto(sel.mGapSpaces, s, 1.f);
    }
    // Roof outlines: outside a building, the outline of its roof spaces' polys, over everything; not in mode c.
    if (opts.mRoofOutline && mode != Overlap::ROOFS)
    {
        std::vector<U8> in(d.mPolys.size(), 0);
        for (const auto& br : d.mRoofsOf)
        {
            if ((vs >= 0 && br.first == vb) || isEntered(br.first))
            {
                continue;
            }
            std::vector<U32> polys;
            for (U32 r : br.second)
            {
                for (U32 p : d.mSpacePolys[r])
                {
                    in[p] = 1;
                    polys.push_back(p);
                }
            }
            const auto member = [&](U32 p) { return in[p] != 0; };
            for (U32 p : polys)
            {
                for (U32 e = 0; e < d.mPolys[p].mCount; ++e)
                {
                    d.edgeOutline(p, e, member, [&](const LLVector3& a, const LLVector3& b) { sel.mRoofLines.emplace_back(a, b); });
                }
            }
            for (U32 p : polys)
            {
                in[p] = 0;
            }
            sel.mRoofPolys.insert(sel.mRoofPolys.end(), polys.begin(), polys.end());
        }
    }

    U64 h = 0;
    for (const auto& e : sel.mRank)
    {
        h = mix(mix(h, e.first), bitsOf(e.second));
    }
    for (U32 s = 0; s < ns; ++s)
    {
        h = sel.mDim[s] ? mix(h, s) : h;
    }
    for (const auto& e : sel.mFlights)
    {
        h = mix(mix(h, e.first), bitsOf(e.second));
    }
    for (U32 s : sel.mOther)
    {
        h = mix(h, s);
    }
    h = mix(mix(mix(h, sel.mGapSpaces.size()), sel.mGapPolys.size()), sel.mRoofLines.size());
    sel.mHash = h;
    return sel;
}

// ---------- geometry ----------

namespace
{
    struct Builder
    {
        F32 mMpp = 1.f;
        std::map<F32, Geometry::Rank> mRanks;
        Geometry::Rank& rank(F32 r)
        {
            Geometry::Rank& g = mRanks[r];
            g.mRank = r;
            return g;
        }
    };

    void vtx(std::vector<Vertex>& out, F32 x, F32 y, F32 z, const Rgba& c, F32 k0 = 0.f, F32 k1 = 0.f, F32 k2 = 0.f, F32 k3 = 0.f)
    {
        out.push_back({ { x, y, z }, { c.r, c.g, c.b, c.a }, { k0, k1, k2, k3 } });
    }

    /// A segment as a quad wpx texels wide about its line.
    void pushLine(std::vector<Vertex>& out, F32 mpp, const LLVector2& a, const LLVector2& b, F32 wpx, const Rgba& c, F32 z = 0.f)
    {
        const LLVector2 d = b - a;
        const F32 len = d.length();
        const LLVector2 n = len > 0.f ? LLVector2(-d.mV[VY], d.mV[VX]) / len : LLVector2(0.f, 1.f);
        const LLVector2 h = n * (wpx * mpp * 0.5f);
        const LLVector2 a0 = a - h, a1 = a + h, b0 = b - h, b1 = b + h;
        vtx(out, a0.mV[0], a0.mV[1], z, c);
        vtx(out, b0.mV[0], b0.mV[1], z, c);
        vtx(out, b1.mV[0], b1.mV[1], z, c);
        vtx(out, a0.mV[0], a0.mV[1], z, c);
        vtx(out, b1.mV[0], b1.mV[1], z, c);
        vtx(out, a1.mV[0], a1.mV[1], z, c);
    }
    LLVector2 xy(const LLVector3& v) { return LLVector2(v.mV[VX], v.mV[VY]); }

    /// A run's line moved `face` metres to its right (floor on the left), mitred at inner points; where the run turns
    /// back on itself the point takes the next segment's normal.
    std::vector<LLVector3> offsetLine(const std::vector<LLVector3>& line, F32 face)
    {
        const size_t n = line.size();
        std::vector<LLVector2> nr;
        for (size_t k = 0; k + 1 < n; ++k)
        {
            const F32 dx = line[k + 1].mV[VX] - line[k].mV[VX], dy = line[k + 1].mV[VY] - line[k].mV[VY];
            F32 l = std::sqrt(dx * dx + dy * dy);
            l = l > 0.f ? l : 1.f;
            nr.emplace_back(dy / l, -dx / l);
        }
        if (nr.empty())
        {
            return line;
        }
        std::vector<LLVector3> out;
        for (size_t k = 0; k < n; ++k)
        {
            const LLVector2& a = nr[k > 0 ? k - 1 : 0];
            const LLVector2& b = nr[llmin(nr.size() - 1, k)];
            LLVector2 m = a + b;
            const F32 c = a * b;
            if (c <= 0.f)
            {
                m = b;
            }
            else
            {
                const F32 l = m.length();
                const F32 s = 1.f / std::sqrt((1.f + c) / 2.f);
                m = m / l * s;
            }
            out.emplace_back(line[k].mV[VX] + m.mV[VX] * face, line[k].mV[VY] + m.mV[VY] * face, line[k].mV[VZ]);
        }
        return out;
    }

    void floorPoly(const Data& d, std::vector<Vertex>& out, U32 p, F32 kind, F32 alpha)
    {
        const Poly& poly = d.mPolys[p];
        const F32 outdoor = d.indoor(poly.mSpace) ? 0.f : 1.f;
        const Rgba c{ 0.f, 0.f, 0.f, alpha };
        for (U32 j = 1; j + 1 < poly.mCount; ++j)
        {
            for (U32 k : { 0u, j, j + 1 })
            {
                const LLVector3& v = d.vert(p, k);
                vtx(out, v.mV[VX], v.mV[VY], v.mV[VZ], c, kind, outdoor, poly.mWater);
            }
        }
    }

    enum class EndState { OPEN, HIDDEN, UNKNOWN };

    /// Flight marks at the composite's scale (rulings 16-18, 20, 22).
    void flightMarks(const Data& d, const Flight& f, F32 mpp, bool gaps, const std::vector<F32>& drawn_poly, std::vector<Vertex>& lines,
                     std::vector<Vertex>& tris, std::vector<Vertex>& edges)
    {
        const LLVector2 bm = (xy(f.mBottom[0]) + xy(f.mBottom[1])) * 0.5f, tm = (xy(f.mTop[0]) + xy(f.mTop[1])) * 0.5f;
        const F32 len = (tm - bm).length();
        if (len < 1e-3f || f.mOutline.empty())
        {
            return;
        }
        const LLVector2 up = (tm - bm) / len, ac(-up.mV[VY], up.mV[VX]);
        // Tread extent across the flight: the walkable outline projected on the across axis (ruling 20).
        F32 a0 = F32_MAX, a1 = -F32_MAX;
        for (const auto& s : f.mOutline)
        {
            for (const LLVector2& p : { s.first, s.second })
            {
                const F32 t = (p - bm) * ac;
                a0 = llmin(a0, t);
                a1 = llmax(a1, t);
            }
        }
        const auto at = [&](F32 u, F32 a) { return bm + up * u + ac * a; };
        for (const auto& s : f.mOutline)
        {
            pushLine(lines, mpp, s.first, s.second, PX_CASING, COL_CASING);
        }
        for (const auto& s : f.mOutline)
        {
            pushLine(lines, mpp, s.first, s.second, PX_CASING_LINE, COL_CASING_LINE);
        }
        if (f.mStair)
        {
            // Rungs are a fixed screen-pitch pattern marking a stair, never a step count.
            const S32 n = llmax(1, static_cast<S32>(std::floor(len / (PX_RUNG_PITCH * mpp))));
            for (S32 i = 1; i < n; ++i)
            {
                pushLine(lines, mpp, at(len * i / n, a0), at(len * i / n, a1), PX_RUNG, COL_RUNG);
            }
        }
        for (S32 top = 0; top < 2; ++top)
        {
            const std::vector<U32>& ep = f.mEnds[top];
            const EndState st = ep.empty() ? EndState::UNKNOWN
                                           : std::any_of(ep.begin(), ep.end(), [&](U32 p) { return drawn_poly[p] > -F32_MAX; })
                                                 ? EndState::OPEN
                                                 : EndState::HIDDEN;
            if (st == EndState::OPEN || (st == EndState::UNKNOWN && !gaps))
            {
                continue;
            }
            const LLVector3* e = top ? f.mTop : f.mBottom;
            const F32 u = ((xy(e[0]) + xy(e[1])) * 0.5f - bm) * up;
            pushLine(lines, mpp, at(u, a0), at(u, a1), PX_CAP, st == EndState::HIDDEN ? COL_STAIR : COL_GAP);
        }
        F32 al = PX_ARROW_LONG * mpp, aw = PX_ARROW_WIDE * mpp;
        const F32 k = llmin(1.f, 0.8f * len / al, 0.9f * (a1 - a0) / aw);
        al *= k;
        aw *= k;
        const F32 tip_off = llmin(0.05f * len, 2.f * mpp);
        const LLVector2 tip = at(len - tip_off, (a0 + a1) / 2.f), base = at(len - tip_off - al, (a0 + a1) / 2.f);
        pushLine(lines, mpp, at(0.05f * len, (a0 + a1) / 2.f), base, PX_CENTRE, COL_STAIR);
        const LLVector2 l = base + ac * (aw / 2.f), r = base - ac * (aw / 2.f);
        vtx(tris, tip.mV[0], tip.mV[1], 0.f, COL_STAIR);
        vtx(tris, l.mV[0], l.mV[1], 0.f, COL_STAIR);
        vtx(tris, r.mV[0], r.mV[1], 0.f, COL_STAIR);
        pushLine(edges, mpp, tip, l, PX_ARROW_EDGE, COL_ARROW_EDGE);
        pushLine(edges, mpp, l, r, PX_ARROW_EDGE, COL_ARROW_EDGE);
        pushLine(edges, mpp, r, tip, PX_ARROW_EDGE, COL_ARROW_EDGE);
    }
}

void skmap::plan::buildGeometry(const Data& d, const Selection& sel, const Options& opts, F32 mpp, Geometry& out)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.Geometry");
    Builder B;
    B.mMpp = mpp;
    const F32 ledge_fade = zoomFade(mpp, ZOOM_DETAIL_MPP, ZOOM_LEDGE_GONE_MPP);
    const F32 wall_fade = zoomFade(mpp, ZOOM_DETAIL_MPP, ZOOM_WALL_GONE_MPP);
    const F32 NONE = -F32_MAX;
    std::vector<F32> drawn_poly(d.mPolys.size(), NONE);
    std::vector<U32> drawn_order;
    const auto mark = [&](U32 p, F32 r)
    {
        drawn_poly[p] = r;
        drawn_order.push_back(p);
    };
    for (const auto& e : sel.mRank)
    {
        const F32 alpha = sel.mDim[e.first] ? DIM_ALPHA : 1.f;
        for (U32 p : d.mSpacePolys[e.first])
        {
            floorPoly(d, B.rank(e.second).mFloors, p, KIND_FLOOR, alpha);
            mark(p, e.second);
        }
    }
    for (const auto& e : sel.mFlights)
    {
        for (U32 p : d.mFlights[e.first].mPolys)
        {
            if (drawn_poly[p] == NONE)
            {
                floorPoly(d, B.rank(e.second).mFloors, p, KIND_FLOOR, 1.f);
                mark(p, e.second);
            }
        }
    }
    // Other level: muted, at the lowest rank, under everything drawn.
    for (U32 s : sel.mOther)
    {
        for (U32 p : d.mSpacePolys[s])
        {
            if (drawn_poly[p] == NONE)
            {
                floorPoly(d, B.rank(-1.f).mFloors, p, KIND_OTHER, 1.f);
            }
        }
    }
    if (opts.mGaps)
    {
        for (const auto& e : sel.mGapSpaces)
        {
            for (U32 p : d.mSpacePolys[e.first])
            {
                if (drawn_poly[p] == NONE)
                {
                    floorPoly(d, B.rank(e.second).mFloors, p, KIND_GAP, 1.f);
                }
            }
        }
        for (const auto& e : sel.mGapPolys)
        {
            if (drawn_poly[e.first] == NONE)
            {
                floorPoly(d, B.rank(e.second).mFloors, e.first, KIND_GAP, 1.f);
            }
        }
    }
    // Walls: the wall runs of the drawn polys, at the highest rank among them, depth-tested at the run's own floor.
    std::vector<std::pair<U32, F32>> walls;
    std::vector<S32> wall_at(d.mWalls.size(), -1);
    for (U32 p : drawn_order)
    {
        for (U32 w : d.mWallsOfPoly[p])
        {
            if (wall_at[w] < 0)
            {
                wall_at[w] = static_cast<S32>(walls.size());
                walls.emplace_back(w, drawn_poly[p]);
            }
            else
            {
                walls[wall_at[w]].second = llmax(walls[wall_at[w]].second, drawn_poly[p]);
            }
        }
    }
    out.mWallRuns = 0;
    for (const auto& e : walls)
    {
        const Wall& w = d.mWalls[e.first];
        if ((w.mClass == WallClass::NONE && !opts.mLedges) || (w.mClass != WallClass::NONE && !opts.mWalls))
        {
            continue;
        }
        Geometry::Rank& g = B.rank(e.second);
        ++out.mWallRuns;
        // A run that met no face (face < 0) keeps its line where it is.
        const std::vector<LLVector3> line = w.mClass == WallClass::NONE || w.mFace < 0.f ? w.mLine : offsetLine(w.mLine, w.mFace);
        // Floor out to the face: the navmesh edge stops the erosion width short of the wall; the run's face closes it.
        if (w.mClass != WallClass::NONE && w.mFace > 0.f && !w.mPolys.empty())
        {
            const Poly& p0 = d.mPolys[w.mPolys[0]];
            const F32 outdoor = d.indoor(p0.mSpace) ? 0.f : 1.f;
            const Rgba c{ 0.f, 0.f, 0.f, 1.f };
            for (size_t k = 0; k + 1 < w.mLine.size(); ++k)
            {
                const LLVector3 &e0 = w.mLine[k], &e1 = w.mLine[k + 1], &l0 = line[k], &l1 = line[k + 1];
                const F32 z0 = e0.mV[VZ], z1 = e1.mV[VZ];
                vtx(g.mFloors, e0.mV[0], e0.mV[1], z0, c, KIND_FLOOR, outdoor, p0.mWater);
                vtx(g.mFloors, e1.mV[0], e1.mV[1], z1, c, KIND_FLOOR, outdoor, p0.mWater);
                vtx(g.mFloors, l1.mV[0], l1.mV[1], z1, c, KIND_FLOOR, outdoor, p0.mWater);
                vtx(g.mFloors, e0.mV[0], e0.mV[1], z0, c, KIND_FLOOR, outdoor, p0.mWater);
                vtx(g.mFloors, l1.mV[0], l1.mV[1], z1, c, KIND_FLOOR, outdoor, p0.mWater);
                vtx(g.mFloors, l0.mV[0], l0.mV[1], z0, c, KIND_FLOOR, outdoor, p0.mWater);
            }
        }
        // Poché between the face and the far face (R8), one surface per pixel in the composite.
        if (w.mClass != WallClass::NONE && w.mFar > w.mFace)
        {
            const std::vector<LLVector3> back = offsetLine(w.mLine, w.mFar);
            const F32 half = w.mClass == WallClass::SOLID ? 0.f : 1.f;
            const Rgba c{ 0.f, 0.f, 0.f, 1.f };
            for (size_t k = 0; k + 1 < line.size(); ++k)
            {
                for (const LLVector3* q : { &line[k], &line[k + 1], &back[k + 1], &line[k], &back[k + 1], &back[k] })
                {
                    vtx(g.mWalls, q->mV[VX], q->mV[VY], w.mFloor, c, half, 1.f);
                }
            }
        }
        // Zoomed out, ledges fade away and wall outlines thin and fade, leaving the walls their fill.
        const F32 fade = w.mClass == WallClass::NONE ? ledge_fade : wall_fade;
        if (fade <= 0.f)
        {
            continue;
        }
        const F32 px = (w.mClass == WallClass::SOLID ? PX_SOLID : w.mClass == WallClass::HATCHED ? PX_HATCHED : PX_LEDGE) * (w.mClass == WallClass::NONE ? 1.f : 0.5f + 0.5f * fade);
        Rgba c = w.mClass == WallClass::SOLID ? COL_WALL_LINE : w.mClass == WallClass::HATCHED ? COL_HATCH_LINE : COL_LEDGE;
        c.a *= fade;
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            pushLine(g.mWallLines, mpp, xy(line[k]), xy(line[k + 1]), px, c, w.mFloor);
        }
    }
    // Contours on drawn floors (build.py leaves stairs out).
    if (opts.mContours && wall_fade > 0.f)
    {
        Rgba contour = COL_CONTOUR;
        contour.a *= wall_fade;
        for (const Contour& c : d.mContours)
        {
            if (drawn_poly[c.mPoly] != NONE)
            {
                pushLine(B.rank(drawn_poly[c.mPoly]).mLines, mpp, c.mA, c.mB, PX_CONTOUR, contour);
            }
        }
    }
    // Roof outlines: on a rank above every storey drawn, so they lie over everything.
    if (!sel.mRoofLines.empty())
    {
        F32 top = 1.f;
        for (const auto& r : B.mRanks)
        {
            top = llmax(top, r.first);
        }
        std::vector<Vertex>& l = B.rank(top + 1.f).mLines;
        Rgba roof = COL_ROOF_LINE;
        roof.a *= 0.5f + 0.5f * wall_fade;
        for (const auto& s : sel.mRoofLines)
        {
            pushLine(l, mpp, xy(s.first), xy(s.second), PX_ROOF, roof);
        }
    }
    // Doors and openings on the higher rank of their drawn sides; seam 'adjacent' portals are plain shared floor.
    if (opts.mDoors)
    {
        for (const Portal& p : d.mPortals)
        {
            if (p.mKind == PortalKind::STAIR || p.mKind == PortalKind::ADJACENT)
            {
                continue;
            }
            const bool ha = sel.has(p.mSpaces[0]), hb = sel.has(p.mSpaces[1]);
            if (!ha && !hb)
            {
                continue;
            }
            const F32 r = llmax(ha ? sel.rank(p.mSpaces[0]) : -1.f, hb ? sel.rank(p.mSpaces[1]) : -1.f);
            pushLine(B.rank(r).mLines, mpp, xy(p.mA), xy(p.mB), PX_DOOR, p.mKind == PortalKind::DOOR ? COL_DOOR : COL_OPENING);
        }
    }
    // Flight marks after each rank's lines: casing, rungs, caps, centreline; then arrowheads, then their edges.
    {
        std::map<F32, std::vector<Vertex>> tris, edges;
        for (const auto& e : opts.mStairs ? sel.mFlights : std::vector<std::pair<U32, F32>>())
        {
            flightMarks(d, d.mFlights[e.first], mpp, opts.mGaps, drawn_poly, B.rank(e.second).mLines, tris[e.second], edges[e.second]);
        }
        for (auto& t : tris)
        {
            std::vector<Vertex>& l = B.rank(t.first).mLines;
            l.insert(l.end(), t.second.begin(), t.second.end());
            l.insert(l.end(), edges[t.first].begin(), edges[t.first].end());
        }
    }
    // Roof inner shadow: around every outline segment a two-sided band and a disc at its start, fading to nothing over
    // ROOF_SHADOW_M; the composite max-blends them and clips them to the roofs.
    out.mShadowClip.clear();
    out.mShadowBand.clear();
    const Rgba clip_c{ 0.f, 0.f, 0.f, 1.f };
    for (U32 p : sel.mRoofPolys)
    {
        const Poly& poly = d.mPolys[p];
        for (U32 j = 1; j + 1 < poly.mCount; ++j)
        {
            for (U32 k : { 0u, j, j + 1 })
            {
                vtx(out.mShadowClip, d.vert(p, k).mV[VX], d.vert(p, k).mV[VY], 0.f, clip_c);
            }
        }
    }
    const Rgba edge_c{ ROOF_SHADOW_ALPHA, 0.f, 0.f, 1.f }, zero_c{ 0.f, 0.f, 0.f, 1.f };
    const auto tri = [&](const LLVector2& p, const LLVector2& q, const LLVector2& r, const Rgba& cp, const Rgba& cq, const Rgba& cr)
    {
        vtx(out.mShadowBand, p.mV[0], p.mV[1], 0.f, cp);
        vtx(out.mShadowBand, q.mV[0], q.mV[1], 0.f, cq);
        vtx(out.mShadowBand, r.mV[0], r.mV[1], 0.f, cr);
    };
    for (const auto& s : sel.mRoofLines)
    {
        const LLVector2 a = xy(s.first), b = xy(s.second);
        F32 len = (b - a).length();
        len = len > 0.f ? len : 1.f;
        const LLVector2 n = LLVector2(-(b.mV[VY] - a.mV[VY]), b.mV[VX] - a.mV[VX]) / len * ROOF_SHADOW_M;
        for (F32 sgn : { 1.f, -1.f })
        {
            const LLVector2 ai = a + n * sgn, bi = b + n * sgn;
            tri(a, b, bi, edge_c, edge_c, zero_c);
            tri(a, bi, ai, edge_c, zero_c, zero_c);
        }
        for (S32 k = 0; k < ROOF_SHADOW_DISC; ++k)
        {
            const F32 t0 = F_TWO_PI * k / ROOF_SHADOW_DISC, t1 = F_TWO_PI * (k + 1) / ROOF_SHADOW_DISC;
            tri(a, a + LLVector2(std::cos(t0), std::sin(t0)) * ROOF_SHADOW_M, a + LLVector2(std::cos(t1), std::sin(t1)) * ROOF_SHADOW_M, edge_c,
                zero_c, zero_c);
        }
    }
    // Region edges over everything.
    out.mTop.clear();
    for (const Region& r : d.mRegions)
    {
        if (!opts.mRegionEdges)
        {
            break;
        }
        const LLVector2 a = r.mMin, b = r.mMin + LLVector2(static_cast<F32>(r.mSide), 0.f),
                        c = r.mMin + LLVector2(static_cast<F32>(r.mSide), static_cast<F32>(r.mSide)),
                        e = r.mMin + LLVector2(0.f, static_cast<F32>(r.mSide));
        pushLine(out.mTop, mpp, a, b, PX_REGION, COL_REGION);
        pushLine(out.mTop, mpp, b, c, PX_REGION, COL_REGION);
        pushLine(out.mTop, mpp, c, e, PX_REGION, COL_REGION);
        pushLine(out.mTop, mpp, e, a, PX_REGION, COL_REGION);
    }
    out.mRanks.clear();
    for (auto& r : B.mRanks)
    {
        out.mRanks.push_back(std::move(r.second));
    }
}

// ---------- gather ----------

namespace
{
    bool overlaps(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        return amin.mdV[VX] <= bmax.mdV[VX] && amax.mdV[VX] >= bmin.mdV[VX] && amin.mdV[VY] <= bmax.mdV[VY] && amax.mdV[VY] >= bmin.mdV[VY];
    }

    Bucket bucketOf(sknav::EnclosureBucket b)
    {
        switch (b)
        {
            case sknav::EnclosureBucket::OUTDOORS: return Bucket::OUTDOORS;
            case sknav::EnclosureBucket::SHELTERED: return Bucket::SHELTERED;
            case sknav::EnclosureBucket::INDOORS: return Bucket::INDOORS;
            default: return Bucket::UNKNOWN;
        }
    }

    /// The region meshes meeting the box, by the view's meshes: those whose tiles are the region's own.
    template <typename Fn> void forRegionMeshes(const LLVector3d& min, const LLVector3d& max, const Fn& fn)
    {
        const SKNavWorldViewPtr view = SKNavMesh::instance().view();
        if (!view)
        {
            return;
        }
        for (const SKNavWorldView::Mesh& m : view->meshes())
        {
            if (!m.mRooms || !overlaps(m.mMin, m.mMax, min, max))
            {
                continue;
            }
            if (SKNavMesh::instance().terrainTilesIn(m.mId, m.mMin, m.mMax))
            {
                fn(m);
            }
        }
    }
}

U64 skmap::plan::gatherKey(const LLVector3d& min, const LLVector3d& max)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.GatherKey");
    SKNavMesh& nav = SKNavMesh::instance();
    U64 h = 0;
    forRegionMeshes(min, max, [&](const SKNavWorldView::Mesh& m)
    {
        U64 id[2];
        memcpy(id, m.mId.mData, sizeof(id));
        h = mix(mix(h, id[0]), id[1]);
        h = mix(h, m.mRooms->mGeneration);
        h = mix(h, nav.detailResident(m.mId) ? 1 : 0);
        h = mix(h, nav.mapInputsKey(m.mId, m.mMin, m.mMax));
    });
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const LLVector3d o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        if (overlaps(o, o + LLVector3d(w, w, 0.0), min, max))
        {
            h = mix(mix(h, region->getHandle()), bitsOf(region->getWaterHeight()));
        }
    }
    return h;
}

void skmap::plan::gatherKeyParts(const LLVector3d& min, const LLVector3d& max, GatherParts& out)
{
    SKNavMesh& nav = SKNavMesh::instance();
    out = GatherParts();
    forRegionMeshes(min, max, [&](const SKNavWorldView::Mesh& m)
    {
        out.mMeshes.push_back({ m.mId, m.mMin, m.mMax, m.mRooms->mGeneration, nav.detailResident(m.mId), nav.mapInputsKey(m.mId, m.mMin, m.mMax) });
    });
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const LLVector3d o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        if (overlaps(o, o + LLVector3d(w, w, 0.0), min, max))
        {
            out.mRegions.emplace_back(region->getHandle(), region->getWaterHeight());
        }
    }
}

struct skmap::plan::Snapshot
{
    struct Border
    {
        U16 mPoly = 0;
        U8 mEdge = 0;
        U64 mNbr = 0;
        F32 mT0 = 0.f;
        F32 mT1 = 1.f;
    };
    struct Tile
    {
        SKNavMesh::PolyTile mPoly;
        F32 mWater = 0.f;
        std::shared_ptr<const std::vector<Border>> mBorder;
        std::shared_ptr<const std::vector<sknav::WallRun>> mRuns;
    };
    struct Mesh
    {
        LLUUID mId;
        LLVector3d mOrigin;
        sknav::RoomsProductPtr mRooms;
        sknav::StoreyProductPtr mStoreys;
        std::vector<Tile> mTiles;
    };
    struct Land
    {
        LLVector3d mOrigin;
        S32 mSide = 256;
        F32 mWater = 0.f;
        std::shared_ptr<const std::vector<F32>> mHeights;
    };
    LLVector3d mOrigin;
    std::vector<Mesh> mMeshes;
    std::vector<Land> mLands;
    U32 mTilesRead = 0;     ///< tiles whose border links were read again
    U32 mLandsRead = 0;
};

struct skmap::plan::Gatherer::Cache
{
    struct Border
    {
        U64 mKey = 0;
        std::shared_ptr<const std::vector<Snapshot::Border>> mLinks;
    };
    struct Land
    {
        U64 mStamp = 0;
        std::shared_ptr<const std::vector<F32>> mHeights;
    };
    std::map<std::pair<LLUUID, std::pair<S32, S32>>, Border> mBorders;
    std::map<U64, Land> mLands;
};

skmap::plan::Gatherer::Gatherer() : mCache(std::make_unique<Cache>()) {}
skmap::plan::Gatherer::~Gatherer() = default;

namespace
{
    /// The land's patches as they stand, the way SKNavMesh::landStamp reads them.
    U64 landStamp(LLViewerRegion& region)
    {
        LLSurface& land = region.getLand();
        const S32 per_edge = land.getPatchesPerEdge();
        U64 h = mix(0ULL, region.getHandle());
        for (S32 y = 0; y < per_edge; ++y)
        {
            for (S32 x = 0; x < per_edge; ++x)
            {
                if (const LLSurfacePatch* patch = land.getPatch(x, y))
                {
                    h = mix(h, static_cast<U64>(patch->getUpdateCount()));
                }
            }
        }
        return mix(h, static_cast<U64>(region.getWidth()));
    }
}

SnapshotPtr skmap::plan::Gatherer::snapshot(const LLVector3d& origin, const LLVector3d& min, const LLVector3d& max)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.Snapshot");
    SKNavMesh& nav = SKNavMesh::instance();
    auto snap = std::make_shared<Snapshot>();
    snap->mOrigin = origin;
    decltype(Cache::mBorders) borders;
    forRegionMeshes(min, max, [&](const SKNavWorldView::Mesh& m)
    {
        nav.requestDetail(m.mId);   // wall runs read the tiles' air (ruling 13)
        Snapshot::Mesh ms;
        ms.mId = m.mId;
        ms.mOrigin = m.mOrigin;
        ms.mRooms = m.mRooms;
        ms.mStoreys = nav.storeysLatest(m.mId);
        std::map<std::pair<S32, S32>, size_t> at;
        nav.polysIn(m.mMin, m.mMax, sknav::AgentClass::AVATAR, [&](const SKNavMesh::PolyTile& t)
        {
            if (t.mMesh != m.mId || !t.mPolys)
            {
                return;
            }
            at[{ t.mKey.mX, t.mKey.mY }] = ms.mTiles.size();
            Snapshot::Tile st;
            st.mPoly = t;
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(t.mRegion);
            st.mWater = region ? region->getWaterHeight() : 0.f;
            ms.mTiles.push_back(std::move(st));
        });
        // Border links name the neighbours' Detour refs: read again only when a tile or a neighbour is re-added.
        for (Snapshot::Tile& st : ms.mTiles)
        {
            const sknav::TileKey& key = st.mPoly.mKey;
            U64 k = 0;
            for (S32 i = 0; i < 9; ++i)
            {
                const auto n = at.find({ key.mX + i % 3 - 1, key.mY + i / 3 - 1 });
                k = mix(k, n == at.end() ? 0 : ms.mTiles[n->second].mPoly.mPolyRefBase);
            }
            const auto id = std::make_pair(m.mId, std::make_pair(key.mX, key.mY));
            const auto cached = mCache->mBorders.find(id);
            if (cached != mCache->mBorders.end() && cached->second.mKey == k)
            {
                st.mBorder = cached->second.mLinks;
            }
            else
            {
                auto links = std::make_shared<std::vector<Snapshot::Border>>();
                nav.borderLinksIn(st.mPoly.mMin, st.mPoly.mMax, sknav::AgentClass::AVATAR,
                                  [&](const LLUUID& mesh, const sknav::TileKey& lk, U16 poly, U8 edge, U64 nbr, F32 t0, F32 t1)
                {
                    if (mesh == m.mId && lk.mX == key.mX && lk.mY == key.mY)
                    {
                        links->push_back({ poly, edge, nbr, t0, t1 });
                    }
                });
                st.mBorder = links;
                ++snap->mTilesRead;
            }
            borders[id] = { k, st.mBorder };
        }
        nav.wallRunsLatestIn(m.mId, m.mMin, m.mMax, [&](const sknav::TileKey& key, const std::shared_ptr<const std::vector<sknav::WallRun>>& runs)
        {
            const auto it = at.find({ key.mX, key.mY });
            if (it != at.end())
            {
                ms.mTiles[it->second].mRuns = runs;
            }
        });
        snap->mMeshes.push_back(std::move(ms));
    });
    mCache->mBorders.swap(borders);

    // The regions' land: one height per metre, the surface's own grid, read again when a patch updates.
    decltype(Cache::mLands) lands;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const LLVector3d o = region->getOriginGlobal();
        const S32 side = static_cast<S32>(region->getWidth());
        if (!overlaps(o, o + LLVector3d(side, side, 0.0), min, max))
        {
            continue;
        }
        const U64 stamp = landStamp(*region);
        Cache::Land& c = lands[region->getHandle()];
        const auto cached = mCache->mLands.find(region->getHandle());
        if (cached != mCache->mLands.end() && cached->second.mStamp == stamp)
        {
            c = cached->second;
        }
        else
        {
            LL_PROFILE_ZONE_NAMED("SKMapPlan.Land");
            auto heights = std::make_shared<std::vector<F32>>((side + 1) * (side + 1));
            const LLSurface& land = region->getLand();
            for (S32 y = 0; y <= side; ++y)
            {
                for (S32 x = 0; x <= side; ++x)
                {
                    (*heights)[y * (side + 1) + x] = land.resolveHeightRegion(static_cast<F32>(llmin(x, side - 1)), static_cast<F32>(llmin(y, side - 1)));
                }
            }
            c = { stamp, heights };
            ++snap->mLandsRead;
        }
        snap->mLands.push_back({ o, side, region->getWaterHeight(), c.mHeights });
    }
    mCache->mLands.swap(lands);
    return snap;
}

DataPtr skmap::plan::assemble(const Snapshot& snap)
{
    LL_PROFILE_ZONE_NAMED("SKMapPlan.Gather");
    auto d = std::make_shared<Data>();
    const LLVector3d origin = snap.mOrigin;
    d->mOrigin = origin;
    const auto local = [&](const LLVector3d& g) { return LLVector3(g - origin); };

    for (const Snapshot::Mesh& m : snap.mMeshes)
    {
        const U32 mi = static_cast<U32>(d->mSpaceIndex.size());
        d->mSpaceIndex.emplace_back(m.mId, std::unordered_map<U32, U32>());
        auto& ids = d->mSpaceIndex.back().second;
        const sknav::RoomsProduct& rooms = *m.mRooms;
        const sknav::StoreyProductPtr& storeys = m.mStoreys;
        const U32 space_base = static_cast<U32>(d->mSpaces.size()), storey_base = static_cast<U32>(d->mStoreyFloor.size());
        const U32 portal_base = static_cast<U32>(d->mPortals.size());
        const LLVector3 mo = local(m.mOrigin);
        if (storeys)
        {
            for (const sknav::Storey& st : storeys->mStoreys)
            {
                d->mStoreyFloor.push_back(st.mFloorZ + mo.mV[VZ]);
            }
        }
        for (const sknav::Space& s : rooms.mSpaces)
        {
            ids[s.mId] = static_cast<U32>(d->mSpaces.size());
            Space sp;
            sp.mBucket = bucketOf(s.mBucket);
            sp.mRoof = s.mRoof;
            sp.mFlight = s.mFlight;
            sp.mBuilding = s.mBuilding ? (static_cast<U64>(mi + 1) << 32) | s.mBuilding : 0;
            const S32 st = storeys ? storeys->storeyOf(s.mId) : -1;
            sp.mStorey = st >= 0 ? static_cast<S32>(storey_base) + st : -1;
            sp.mArea = s.mFloorArea;
            sp.mMesh = mi;
            for (U32 p : s.mPortals)
            {
                sp.mPortals.push_back(portal_base + p);
            }
            d->mSpaces.push_back(std::move(sp));
        }
        for (const sknav::Portal& p : rooms.mPortals)
        {
            Portal out;
            out.mKind = p.mKind == sknav::PortalKind::DOOR ? PortalKind::DOOR : p.mKind == sknav::PortalKind::STAIR ? PortalKind::STAIR : PortalKind::OPENING;
            // A cross-mesh portal's far space is not in this product.
            for (S32 k = 0; k < 2; ++k)
            {
                out.mSpaces[k] = !p.mCrossMesh && p.mSpaces[k] < rooms.mSpaces.size() ? static_cast<S32>(space_base + p.mSpaces[k]) : -1;
            }
            out.mA = mo + p.mA;
            out.mB = mo + p.mB;
            d->mPortals.push_back(out);
        }
        const auto index_of = [&](U32 id) { const auto it = ids.find(id); return it == ids.end() ? -1 : static_cast<S32>(it->second); };
        if (storeys)
        {
            for (const sknav::SeamPortal& p : storeys->mSeamPortals)
            {
                Portal out;
                out.mKind = p.mKind == sknav::SeamPortal::DOOR ? PortalKind::DOOR : p.mKind == sknav::SeamPortal::STAIR ? PortalKind::STAIR : PortalKind::ADJACENT;
                out.mSpaces[0] = index_of(p.mSpace[0]);
                out.mSpaces[1] = index_of(p.mSpace[1]);
                out.mA = mo + p.mA;
                out.mB = mo + p.mB;
                d->mPortals.push_back(out);
            }
            for (const sknav::ClimbLink& c : storeys->mClimbLinks)
            {
                const S32 a = index_of(c.mSpace[0]), b = index_of(c.mSpace[1]);
                if (a >= 0 && b >= 0)
                {
                    d->mClimbs.emplace_back(a, b);
                }
            }
        }

        // Polys, tile by tile; the Detour refs name them for the flight ends and the border links.
        std::unordered_map<U64, U32> by_ref;
        std::vector<U32> tile_base(m.mTiles.size(), 0);
        std::map<U64, U32> flight_of_key;
        for (size_t ti = 0; ti < m.mTiles.size(); ++ti)
        {
            const SKNavMesh::PolyTile& t = m.mTiles[ti].mPoly;
            const U32 base = static_cast<U32>(d->mPolys.size());
            tile_base[ti] = base;
            const LLVector3 to = local(t.mOrigin);
            const F32 water = m.mTiles[ti].mWater;
            const sknav::TilePolys& tp = *t.mPolys;
            for (U32 i = 0; i < tp.mPolys.size(); ++i)
            {
                const sknav::TilePolys::Poly& p = tp.mPolys[i];
                Poly out;
                out.mFirst = static_cast<U32>(d->mVerts.size());
                out.mCount = p.mVertCount;
                const U32 sid = t.mPolySpace && i < t.mPolySpace->size() ? (*t.mPolySpace)[i] : 0;
                out.mSpace = sid ? index_of(sid) : -1;
                out.mWater = water;
                for (U32 k = 0; k < p.mVertCount; ++k)
                {
                    d->mVerts.push_back(to + tp.mVerts[p.mVerts[k]]);
                    const S16 n = p.mNeighbour[k];
                    d->mNbr.push_back(n >= 0 ? static_cast<S32>(base + n) : n == sknav::TilePolys::EDGE_BORDER ? NBR_BORDER : NBR_NONE);
                }
                d->mPolys.push_back(out);
                if (t.mPolyRefBase)
                {
                    by_ref[t.mPolyRefBase | i] = base + i;
                }
            }
            if (t.mFlights)
            {
                for (const sknav::Flight& f : *t.mFlights)
                {
                    auto it = flight_of_key.find(f.mKey);
                    if (it == flight_of_key.end())
                    {
                        it = flight_of_key.emplace(f.mKey, static_cast<U32>(d->mFlights.size())).first;
                        Flight out;
                        out.mStair = f.mKind == sknav::ConnectorKind::STAIR;
                        out.mBottom[0] = to + f.mBottomA;
                        out.mBottom[1] = to + f.mBottomB;
                        out.mTop[0] = to + f.mTopA;
                        out.mTop[1] = to + f.mTopB;
                        d->mFlights.push_back(std::move(out));
                    }
                    for (U16 p : f.mPolys)
                    {
                        if (p < tp.mPolys.size())
                        {
                            d->mFlights[it->second].mPolys.push_back(base + p);
                        }
                    }
                }
            }
        }
        // Cross-tile neighbours from Detour's own external links (R13).
        for (size_t ti = 0; ti < m.mTiles.size(); ++ti)
        {
            const U32 count = static_cast<U32>(m.mTiles[ti].mPoly.mPolys->mPolys.size());
            if (!m.mTiles[ti].mBorder)
            {
                continue;
            }
            for (const Snapshot::Border& b : *m.mTiles[ti].mBorder)
            {
                const auto nb = by_ref.find(b.mNbr);
                if (nb != by_ref.end() && b.mPoly < count)
                {
                    d->mBorder[(static_cast<U64>(tile_base[ti] + b.mPoly) << 3) | b.mEdge].push_back({ nb->second, b.mT0, b.mT1 });
                }
            }
        }
        if (storeys)
        {
            for (const auto& kf : flight_of_key)
            {
                if (const sknav::FlightEnds* ends = storeys->flightEnds(kf.first))
                {
                    for (S32 k = 0; k < 2; ++k)
                    {
                        for (U64 ref : ends->mPolys[k])
                        {
                            const auto it = by_ref.find(ref);
                            if (it != by_ref.end())
                            {
                                d->mFlights[kf.second].mEnds[k].push_back(it->second);
                            }
                        }
                    }
                }
            }
        }
        const LLVector3 ol = local(m.mOrigin);
        for (size_t ti = 0; ti < m.mTiles.size(); ++ti)
        {
            if (!m.mTiles[ti].mRuns)
            {
                continue;
            }
            const U32 count = static_cast<U32>(m.mTiles[ti].mPoly.mPolys->mPolys.size());
            for (const sknav::WallRun& r : *m.mTiles[ti].mRuns)
            {
                Wall w;
                for (const LLVector3& v : r.mLine)
                {
                    w.mLine.push_back(ol + v);
                }
                w.mClass = r.mClass == sknav::WallClass::SOLID ? WallClass::SOLID : r.mClass == sknav::WallClass::HATCHED ? WallClass::HATCHED : WallClass::NONE;
                w.mFace = r.mFace;
                w.mFar = r.mFar;
                w.mFloor = r.mFloorZ + ol.mV[VZ];
                for (U16 p : r.mPolys)
                {
                    if (p < count)
                    {
                        w.mPolys.push_back(tile_base[ti] + p);
                    }
                }
                d->mWalls.push_back(std::move(w));
            }
        }
    }

    // Flight outlines: their polys' edge stretches no other of their polys shares (ruling 20).
    std::vector<S32> flight_of(d->mPolys.size(), -1);
    for (U32 f = 0; f < d->mFlights.size(); ++f)
    {
        for (U32 p : d->mFlights[f].mPolys)
        {
            flight_of[p] = static_cast<S32>(f);
        }
    }
    for (U32 f = 0; f < d->mFlights.size(); ++f)
    {
        Flight& fl = d->mFlights[f];
        const auto member = [&](U32 p) { return flight_of[p] == static_cast<S32>(f); };
        for (U32 p : fl.mPolys)
        {
            for (U32 e = 0; e < d->mPolys[p].mCount; ++e)
            {
                d->edgeOutline(p, e, member, [&](const LLVector3& a, const LLVector3& b) { fl.mOutline.emplace_back(xy(a), xy(b)); });
            }
        }
    }
    // Contours: per fan triangle at CONTOUR_STEP_M, not on stair polys (ruling 15; ramps keep them).
    for (U32 p = 0; p < d->mPolys.size(); ++p)
    {
        if (flight_of[p] >= 0 && d->mFlights[flight_of[p]].mStair)
        {
            continue;
        }
        const Poly& poly = d->mPolys[p];
        for (U32 j = 1; j + 1 < poly.mCount; ++j)
        {
            const LLVector3 t[3] = { d->vert(p, 0), d->vert(p, j), d->vert(p, j + 1) };
            const F32 lo = llmin(t[0].mV[VZ], t[1].mV[VZ], t[2].mV[VZ]), hi = llmax(t[0].mV[VZ], t[1].mV[VZ], t[2].mV[VZ]);
            for (S32 k = static_cast<S32>(std::ceil(lo / CONTOUR_STEP_M)); k <= static_cast<S32>(std::floor(hi / CONTOUR_STEP_M)); ++k)
            {
                const F32 z = k * CONTOUR_STEP_M;
                LLVector2 pts[3];
                S32 n = 0;
                for (S32 e = 0; e < 3; ++e)
                {
                    const LLVector3 &a = t[e], &b = t[(e + 1) % 3];
                    if ((a.mV[VZ] - z) * (b.mV[VZ] - z) < 0.f)
                    {
                        const F32 s = (z - a.mV[VZ]) / (b.mV[VZ] - a.mV[VZ]);
                        pts[n++] = LLVector2(a.mV[VX] + (b.mV[VX] - a.mV[VX]) * s, a.mV[VY] + (b.mV[VY] - a.mV[VY]) * s);
                    }
                }
                if (n == 2)
                {
                    d->mContours.push_back({ p, pts[0], pts[1] });
                }
            }
        }
    }
    for (const Snapshot::Land& l : snap.mLands)
    {
        Region r;
        r.mMin = xy(local(l.mOrigin));
        r.mSide = l.mSide;
        r.mWater = l.mWater;
        r.mHeights = *l.mHeights;
        d->mRegions.push_back(std::move(r));
    }
    joinRegionSeams(*d);
    d->finish();
    return d;
}

skmap::plan::Whereabouts skmap::plan::whereabouts(const LLVector3d& feet)
{
    const F64 LIFT_M = 0.3;
    const F64 FLOOR_NEAR_M = 1.0;
    Whereabouts out;
    if (!SKNavMesh::instanceExists())
    {
        return out;
    }
    const SKNavMesh& nav = SKNavMesh::instance();
    const SKNavWorldViewPtr world = nav.view();
    const LLVector3d pos(feet.mdV[VX], feet.mdV[VY], feet.mdV[VZ] + LIFT_M);
    SKNavWorldView::SpaceId id;
    sknav::Enclosure gap;
    if (world && world->airGapAt(pos, id, gap) && std::fabs(gap.mFloor - feet.mdV[VZ]) <= FLOOR_NEAR_M)
    {
        out.mFound = true;
        out.mSpace.mMesh = id.mMesh;
        out.mSpace.mId = id.mId;
        out.mBucket = bucketOf(gap.mBucket);
        out.mIndoor = out.mBucket == Bucket::INDOORS;
        out.mFloor = gap.mFloor;
        out.mCeiling = gap.mCeiling;
        return out;
    }
    // No air here yet: the navmesh's own floor.
    if (nav.floorUnder(pos, sknav::AgentClass::AVATAR, out.mFloor))
    {
        out.mFound = true;
        if (!nav.spaceAt(pos, out.mSpace.mMesh, out.mSpace.mId))
        {
            out.mSpace = SpaceRef();
        }
    }
    return out;
}

skmap::plan::SnapshotStats skmap::plan::stats(const Snapshot& snap)
{
    return { snap.mTilesRead, static_cast<U32>(snap.mLands.size()), static_cast<U32>(snap.mMeshes.size()) };
}

S32 skmap::plan::spaceIndex(const Data& d, const SpaceRef& ref)
{
    if (!ref.mId)
    {
        return -1;
    }
    for (const auto& m : d.mSpaceIndex)
    {
        if (m.first == ref.mMesh)
        {
            const auto it = m.second.find(ref.mId);
            return it == m.second.end() ? -1 : static_cast<S32>(it->second);
        }
    }
    return -1;
}
