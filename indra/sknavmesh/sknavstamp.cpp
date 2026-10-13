/**
 * @file sknavstamp.cpp
 * @brief A tile's walkable spans stamped with the spaces they stand in, so its polygons never cross from one space into another.
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

#include "sknavstamp.h"

#include "sknavenclosure.h"
#include "sknavspaces.h"
#include "sknavprofile.h"

#include "Recast.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace sknav
{
    namespace
    {
        constexpr S32 MULTI_REGION_REACH = 4;   ///< heightfield units a multi-region poly's span may lie off its height

        U64 quantPos(U64 h, const LLVector3& v)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                h = mix(h, static_cast<U64>(static_cast<S64>(std::lround(v.mV[k] * 100.f))));
            }
            return h;
        }

        /// Plan distance from a point to a segment.
        F32 planDistance(F32 px, F32 py, const LLVector3& a, const LLVector3& b)
        {
            const F32 dx = b.mV[VX] - a.mV[VX], dy = b.mV[VY] - a.mV[VY];
            const F32 l2 = dx * dx + dy * dy;
            const F32 u = l2 <= 0.f ? 0.f : llclamp(((px - a.mV[VX]) * dx + (py - a.mV[VY]) * dy) / l2, 0.f, 1.f);
            const F32 ex = a.mV[VX] + dx * u - px, ey = a.mV[VY] + dy * u - py;
            return std::sqrt(ex * ex + ey * ey);
        }

        /// Plan distance from a segment to a rectangle, zero when they meet.
        F32 segmentToRect(const LLVector3& a, const LLVector3& b, const LLVector3& lo, const LLVector3& hi)
        {
            F32 best = 1e30f;
            for (S32 k = 0; k <= 16; ++k)
            {
                const F32 t = k / 16.f;
                const F32 x = a.mV[VX] + (b.mV[VX] - a.mV[VX]) * t, y = a.mV[VY] + (b.mV[VY] - a.mV[VY]) * t;
                const F32 dx = std::max({ lo.mV[VX] - x, 0.f, x - hi.mV[VX] }), dy = std::max({ lo.mV[VY] - y, 0.f, y - hi.mV[VY] });
                best = std::min(best, std::sqrt(dx * dx + dy * dy));
            }
            return best;
        }

        /// Whether a door portal's threshold holds a point: across its run, within STAMP_THRESHOLD_M of its plane, at its sill.
        bool thresholdHolds(const StampPortal& p, F32 px, F32 py, F32 pz)
        {
            if (p.mKind != static_cast<U8>(PortalKind::DOOR) || std::fabs(pz - p.mCentre.mV[VZ]) > 1.f)
            {
                return false;
            }
            const F32 dx = p.mB.mV[VX] - p.mA.mV[VX], dy = p.mB.mV[VY] - p.mA.mV[VY];
            const F32 len = std::sqrt(dx * dx + dy * dy);
            if (len <= 0.f)
            {
                return false;
            }
            const F32 along = ((px - p.mA.mV[VX]) * dx + (py - p.mA.mV[VY]) * dy) / len;
            const F32 across = (px - p.mCentre.mV[VX]) * p.mNormal.mV[VX] + (py - p.mCentre.mV[VY]) * p.mNormal.mV[VY];
            return along >= 0.f && along <= len && std::fabs(across) <= STAMP_THRESHOLD_M;
        }

        /// A portal's reach as a box of compact heightfield cells, a cell wider all round than any point it can hold.
        struct PortalCells
        {
            U32 mPortal = 0;
            S32 mX0 = 0, mX1 = -1, mZ0 = 0, mZ1 = -1;

            bool holds(S32 x, S32 z) const { return x >= mX0 && x <= mX1 && z >= mZ0 && z <= mZ1; }
        };

        /// The cells whose centres can lie in a plan box: lo and hi are mesh-local x and y.
        PortalCells cellsOf(const rcCompactHeightfield& chf, U32 portal, F32 lo_x, F32 lo_y, F32 hi_x, F32 hi_y)
        {
            PortalCells c;
            c.mPortal = portal;
            c.mZ0 = static_cast<S32>(std::floor((lo_x - chf.bmin[2]) / chf.cs)) - 1;
            c.mZ1 = static_cast<S32>(std::ceil((hi_x - chf.bmin[2]) / chf.cs)) + 1;
            c.mX0 = static_cast<S32>(std::floor((lo_y - chf.bmin[0]) / chf.cs)) - 1;
            c.mX1 = static_cast<S32>(std::ceil((hi_y - chf.bmin[0]) / chf.cs)) + 1;
            return c;
        }

        /// Each door's threshold as cells, in the input's order; a door whose box cannot be bounded covers every cell.
        std::vector<PortalCells> thresholdCells(const rcCompactHeightfield& chf, const TileStampInput& input)
        {
            std::vector<PortalCells> out;
            for (size_t k = 0; k < input.mPortals.size(); ++k)
            {
                const StampPortal& p = input.mPortals[k];
                if (p.mKind != static_cast<U8>(PortalKind::DOOR))
                {
                    continue;
                }
                const F32 dx = p.mB.mV[VX] - p.mA.mV[VX], dy = p.mB.mV[VY] - p.mA.mV[VY];
                const F32 len = std::sqrt(dx * dx + dy * dy);
                if (len <= 0.f)
                {
                    continue;
                }
                // Corners where along is 0 or len and across is the threshold either side, solved from the two projections.
                const F32 ux = dx / len, uy = dy / len, nx = p.mNormal.mV[VX], ny = p.mNormal.mV[VY];
                const F32 det = ux * ny - uy * nx;
                if (std::fabs(det) < 0.05f)
                {
                    out.push_back({ static_cast<U32>(k), INT_MIN, INT_MAX, INT_MIN, INT_MAX });
                    continue;
                }
                const F32 a0 = p.mA.mV[VX] * ux + p.mA.mV[VY] * uy, c0 = p.mCentre.mV[VX] * nx + p.mCentre.mV[VY] * ny;
                F32 lo_x = FLT_MAX, lo_y = FLT_MAX, hi_x = -FLT_MAX, hi_y = -FLT_MAX;
                for (const F32 along : { 0.f, len })
                {
                    for (const F32 across : { -STAMP_THRESHOLD_M, STAMP_THRESHOLD_M })
                    {
                        const F32 r0 = a0 + along, r1 = c0 + across;
                        const F32 x = (r0 * ny - uy * r1) / det, y = (ux * r1 - nx * r0) / det;
                        lo_x = std::min(lo_x, x);
                        hi_x = std::max(hi_x, x);
                        lo_y = std::min(lo_y, y);
                        hi_y = std::max(hi_y, y);
                    }
                }
                const F32 slack = 0.01f;
                out.push_back(cellsOf(chf, static_cast<U32>(k), lo_x - slack, lo_y - slack, hi_x + slack, hi_y + slack));
            }
            return out;
        }

        /// Each portal's reach, STAMP_PORTAL_REACH_M round its run, as cells, in the input's order.
        std::vector<PortalCells> reachCells(const rcCompactHeightfield& chf, const TileStampInput& input)
        {
            std::vector<PortalCells> out;
            for (size_t k = 0; k < input.mPortals.size(); ++k)
            {
                const StampPortal& p = input.mPortals[k];
                const F32 r = STAMP_PORTAL_REACH_M + 0.01f;
                out.push_back(cellsOf(chf, static_cast<U32>(k), std::min(p.mA.mV[VX], p.mB.mV[VX]) - r, std::min(p.mA.mV[VY], p.mB.mV[VY]) - r,
                                      std::max(p.mA.mV[VX], p.mB.mV[VX]) + r, std::max(p.mA.mV[VY], p.mB.mV[VY]) + r));
            }
            return out;
        }

        /// The (slope, colour) pairs present, in order: what a set of them would iterate.
        template<typename ColourOf>
        std::vector<std::pair<U8, U8>> presentPairs(const rcCompactHeightfield& chf, const std::vector<U8>& slope_map, ColourOf colour_of)
        {
            std::vector<U8> seen(256 * 256, 0);
            for (S32 i = 0; i < chf.spanCount; ++i)
            {
                if (chf.areas[i] != RC_NULL_AREA)
                {
                    seen[slope_map[chf.areas[i]] * 256 + colour_of(i)] = 1;
                }
            }
            std::vector<std::pair<U8, U8>> out;
            for (S32 k = 0; k < 256 * 256; ++k)
            {
                if (seen[k])
                {
                    out.emplace_back(static_cast<U8>(k >> 8), static_cast<U8>(k & 0xff));
                }
            }
            return out;
        }

        /// The component with the most votes, the least on a tie; STAMP_NONE for none.
        U16 majorityOf(const std::vector<std::pair<U16, U32>>& votes)
        {
            U16 best = STAMP_NONE;
            U32 most = 0;
            for (const auto& [k, v] : votes)
            {
                if (v > most || (v == most && k < best))
                {
                    best = k;
                    most = v;
                }
            }
            return best;
        }

        /// One more vote for a component: a poly or a region holds few, so a list beats a map.
        void vote(std::vector<std::pair<U16, U32>>& votes, U16 key)
        {
            for (auto& [k, v] : votes)
            {
                if (k == key)
                {
                    ++v;
                    return;
                }
            }
            votes.emplace_back(key, 1u);
        }

        struct Spans
        {
            const rcCompactHeightfield& mChf;
            S32 mBorder;
            std::vector<S32> mX, mZ;

            Spans(const rcCompactHeightfield& chf, S32 border) : mChf(chf), mBorder(border), mX(chf.spanCount), mZ(chf.spanCount)
            {
                for (S32 z = 0; z < chf.height; ++z)
                {
                    for (S32 x = 0; x < chf.width; ++x)
                    {
                        const rcCompactCell& c = chf.cells[x + z * chf.width];
                        for (U32 i = c.index, n = c.index + c.count; i < n; ++i)
                        {
                            mX[i] = x;
                            mZ[i] = z;
                        }
                    }
                }
            }
            bool interior(S32 i) const
            {
                return mX[i] >= mBorder && mX[i] < mChf.width - mBorder && mZ[i] >= mBorder && mZ[i] < mChf.height - mBorder;
            }
            /// The span across direction dir, or -1.
            S32 neighbour(S32 i, S32 dir) const
            {
                const rcCompactSpan& s = mChf.spans[i];
                const S32 con = rcGetCon(s, dir);
                if (con == RC_NOT_CONNECTED)
                {
                    return -1;
                }
                const S32 nx = mX[i] + rcGetDirOffsetX(dir), nz = mZ[i] + rcGetDirOffsetY(dir);
                return static_cast<S32>(mChf.cells[nx + nz * mChf.width].index) + con;
            }
        };

        /// Greedy colours over groups of components, in order of each group's least component.
        std::vector<U8> colourGroups(const std::vector<std::set<U16>>& adj, const std::vector<U16>& group, std::map<U16, std::set<U16>>* group_adj)
        {
            const size_t n = group.size();
            std::map<U16, std::set<U16>> nadj;
            for (size_t a = 0; a < n; ++a)
            {
                nadj[group[a]];
                for (const U16 b : adj[a])
                {
                    if (group[a] != group[b])
                    {
                        nadj[group[a]].insert(group[b]);
                    }
                }
            }
            std::map<U16, U8> col;
            for (const auto& [g, nb] : nadj)
            {
                std::set<U8> used;
                for (const U16 x : nb)
                {
                    const auto it = col.find(x);
                    if (it != col.end())
                    {
                        used.insert(it->second);
                    }
                }
                U32 c = 1;
                while (used.count(static_cast<U8>(c)) && c < 255)
                {
                    ++c;
                }
                col[g] = static_cast<U8>(c);
            }
            std::vector<U8> out(n);
            for (size_t c = 0; c < n; ++c)
            {
                out[c] = col[group[c]];
            }
            if (group_adj)
            {
                *group_adj = std::move(nadj);
            }
            return out;
        }
    }

    void stampInput(const TileAir& air, const TileEnclosure& enclosure, const RoomsProduct& rooms, const LLVector3& tile_min,
                    const LLVector3& tile_max, TileStampInput& out)
    {
        SK_NAV_ZONE("nav.stamp.input");
        out = TileStampInput();
        std::unordered_map<U32, U16> component;
        const auto of = [&](U32 space) {
            const auto it = component.find(space);
            if (it != component.end())
            {
                return it->second;
            }
            const U16 c = static_cast<U16>(out.mComponentSpace.size());
            out.mComponentSpace.push_back(space);
            component.emplace(space, c);
            return c;
        };
        const bool answered = enclosure.mGaps.size() == air.mGaps.size();
        out.mGapComponent.assign(air.mGaps.size(), STAMP_NONE);
        for (size_t g = 0; answered && g < air.mGaps.size(); ++g)
        {
            const U16 s = enclosure.mGaps[g].mSpace;
            if (s && s <= rooms.mSpaces.size())
            {
                out.mGapComponent[g] = of(s - 1u);
            }
        }
        for (size_t p = 0; p < rooms.mPortals.size(); ++p)
        {
            const Portal& portal = rooms.mPortals[p];
            if (portal.mCrossMesh || segmentToRect(portal.mA, portal.mB, tile_min, tile_max) > STAMP_PORTAL_REACH_M)
            {
                continue;
            }
            StampPortal sp;
            sp.mCentre = portal.mCentre;
            sp.mNormal = portal.mNormal;
            sp.mA = portal.mA;
            sp.mB = portal.mB;
            sp.mKind = static_cast<U8>(portal.mKind);
            sp.mPortal = static_cast<U32>(p);
            for (S32 k = 0; k < 2; ++k)
            {
                sp.mSide[k] = portal.mSpaces[k] < rooms.mSpaces.size() ? of(portal.mSpaces[k]) : STAMP_NONE;
            }
            out.mPortals.push_back(sp);
        }

        // The partition's shape, never its ids.
        U64 h = mix(SEED, out.mGapComponent.size());
        U16 last = 0xfffe;
        U32 run = 0;
        for (const U16 c : out.mGapComponent)
        {
            if (c == last)
            {
                ++run;
                continue;
            }
            h = mix(mix(h, last), run);
            last = c;
            run = 1;
        }
        h = mix(mix(h, last), run);
        h = mix(h, out.mComponentSpace.size());
        for (const StampPortal& sp : out.mPortals)
        {
            h = quantPos(quantPos(quantPos(quantPos(h, sp.mCentre), sp.mNormal), sp.mA), sp.mB);
            h = mix(mix(mix(h, sp.mSide[0]), sp.mSide[1]), sp.mKind);
        }
        out.mPattern = h;
    }

    void stampTile(const rcCompactHeightfield& chf, S32 border, const std::vector<U32>& span_gap, const TileStampInput& input,
                   const std::vector<U8>& real, const std::vector<Flight>& flights, const StampParams& params, TileStamp& out)
    {
        SK_NAV_ZONE("nav.stamp.tile");
        out = TileStamp();
        const S32 n = chf.spanCount;
        const Spans spans(chf, border);
        std::vector<U16>& comp = out.mSpanComponent;
        comp.assign(n, STAMP_NONE);
        std::vector<U8> fillable(n, 0);
        bool any_fillable = false;
        const std::vector<PortalCells> doors = thresholdCells(chf, input), reach = reachCells(chf, input);
        for (S32 i = 0; i < n; ++i)
        {
            if (chf.areas[i] == RC_NULL_AREA || !spans.interior(i))
            {
                continue;
            }
            const S32 cx = spans.mX[i], cz = spans.mZ[i];
            const F32 px = chf.bmin[2] + (cz + 0.5f) * chf.cs;
            const F32 py = chf.bmin[0] + (cx + 0.5f) * chf.cs;
            const F32 pz = chf.bmin[1] + chf.spans[i].y * chf.ch;
            // A door's threshold is split at its leaf's plane, each half to the space on its side.
            const StampPortal* door = nullptr;
            for (const PortalCells& d : doors)
            {
                if (d.holds(cx, cz) && thresholdHolds(input.mPortals[d.mPortal], px, py, pz))
                {
                    door = &input.mPortals[d.mPortal];
                    break;
                }
            }
            if (door)
            {
                const S32 side = (px - door->mCentre.mV[VX]) * door->mNormal.mV[VX] + (py - door->mCentre.mV[VY]) * door->mNormal.mV[VY] < 0.f ? 0 : 1;
                if (door->mSide[side] != STAMP_NONE)
                {
                    comp[i] = door->mSide[side];
                    ++out.mDoorSpans;
                    continue;
                }
            }
            const U32 g = i < static_cast<S32>(span_gap.size()) ? span_gap[i] : SPAN_NO_GAP;
            if (g != SPAN_NO_GAP)
            {
                comp[i] = g < input.mGapComponent.size() ? input.mGapComponent[g] : STAMP_NONE;
                continue;
            }
            // A floor the air has no gap over is under a leaf: its side of the nearest portal's plane.
            S32 best = -1;
            F32 best_d = 0.f;
            for (const PortalCells& r : reach)
            {
                const size_t k = r.mPortal;
                const StampPortal& p = input.mPortals[k];
                if (!r.holds(cx, cz) || std::fabs(pz - p.mCentre.mV[VZ]) > 1.f)
                {
                    continue;
                }
                const F32 d = planDistance(px, py, p.mA, p.mB);
                if (d <= STAMP_PORTAL_REACH_M && (best < 0 || d < best_d))
                {
                    best = static_cast<S32>(k);
                    best_d = d;
                }
            }
            if (best >= 0)
            {
                const StampPortal& p = input.mPortals[best];
                const S32 side = (px - p.mCentre.mV[VX]) * p.mNormal.mV[VX] + (py - p.mCentre.mV[VY]) * p.mNormal.mV[VY] < 0.f ? 0 : 1;
                if (p.mSide[side] != STAMP_NONE)
                {
                    comp[i] = p.mSide[side];
                    ++out.mDoorSpans;
                    continue;
                }
            }
            fillable[i] = 1;
            any_fillable = true;
        }

        // What no portal reaches takes the component of the floor it joins.
        std::vector<S32> queue;
        for (S32 i = 0; any_fillable && i < n; ++i)
        {
            if (comp[i] != STAMP_NONE)
            {
                queue.push_back(i);
            }
        }
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const S32 i = queue[head];
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 j = spans.neighbour(i, d);
                if (j >= 0 && fillable[j] && comp[j] == STAMP_NONE)
                {
                    comp[j] = comp[i];
                    ++out.mFloodSpans;
                    queue.push_back(j);
                }
            }
        }

        // Components that touch, through a span's connection or across a portal.
        size_t ncomp = input.mComponentSpace.size();
        for (S32 i = 0; i < n; ++i)
        {
            if (comp[i] != STAMP_NONE)
            {
                ncomp = std::max<size_t>(ncomp, comp[i] + 1u);
            }
        }
        for (const StampPortal& p : input.mPortals)
        {
            for (const U16 s : p.mSide)
            {
                if (s != STAMP_NONE)
                {
                    ncomp = std::max<size_t>(ncomp, s + 1u);
                }
            }
        }
        std::vector<std::set<U16>> adj(ncomp);
        std::vector<U32> size(ncomp, 0);
        for (S32 i = 0; i < n; ++i)
        {
            const U16 a = comp[i];
            if (chf.areas[i] == RC_NULL_AREA || a == STAMP_NONE || !spans.interior(i))
            {
                continue;
            }
            ++size[a];
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 j = spans.neighbour(i, d);
                if (j < 0 || chf.areas[j] == RC_NULL_AREA || !spans.interior(j))
                {
                    continue;
                }
                const U16 b = comp[j];
                if (b != STAMP_NONE && b != a)
                {
                    adj[a].insert(b);
                    adj[b].insert(a);
                }
            }
        }
        for (const StampPortal& p : input.mPortals)
        {
            if (p.mSide[0] != STAMP_NONE && p.mSide[1] != STAMP_NONE && p.mSide[0] != p.mSide[1])
            {
                adj[p.mSide[0]].insert(p.mSide[1]);
                adj[p.mSide[1]].insert(p.mSide[0]);
            }
        }
        for (S32 i = 0; i < n; ++i)
        {
            out.mUnspacedSpans += chf.areas[i] != RC_NULL_AREA && spans.interior(i) && comp[i] == STAMP_NONE ? 1 : 0;
        }

        std::vector<U16> group(ncomp);
        for (size_t c = 0; c < ncomp; ++c)
        {
            group[c] = static_cast<U16>(c);
        }
        std::vector<U8> colours = colourGroups(adj, group, nullptr);
        std::vector<U8> slope_map(256);
        for (S32 s = 0; s < 256; ++s)
        {
            slope_map[s] = static_cast<U8>(s);
        }
        bool one = false;
        const auto colourOf = [&](S32 i) -> U8 {
            if (one)
            {
                return 1;
            }
            const U16 c = spans.interior(i) ? comp[i] : STAMP_NONE;
            return c == STAMP_NONE ? 0 : colours[c];
        };
        const auto pairs = [&]() { return presentPairs(chf, slope_map, colourOf); };
        const auto realOf = [&real](U8 s) { return s < real.size() ? real[s] : s; };
        const S32 cap = llclamp(params.mPaletteMax, 1, 63);
        std::vector<std::pair<U8, U8>> used = pairs();
        if (static_cast<S32>(used.size()) > cap)
        {
            // First the slope partition's landings: every non-flight code becomes the least of its area.
            out.mOverflow = TileStamp::CODES_MERGED;
            std::set<U8> flight_codes;
            for (const Flight& f : flights)
            {
                flight_codes.insert(f.mCodes.begin(), f.mCodes.end());
            }
            std::set<U8> present;
            for (S32 i = 0; i < n; ++i)
            {
                if (chf.areas[i] != RC_NULL_AREA)
                {
                    present.insert(chf.areas[i]);
                }
            }
            std::map<U8, U8> least;
            for (const U8 s : present)
            {
                if (!flight_codes.count(s))
                {
                    slope_map[s] = least.emplace(realOf(s), s).first->second;
                }
            }
            used = pairs();
        }
        if (static_cast<S32>(used.size()) > cap)
        {
            // Then the spaces: the smallest that touches another takes its smallest neighbour's colour.
            out.mOverflow = TileStamp::SPACES_FUSED;
            // The codes and components present, once: a fusion recolours components, and the pairs follow from these alone.
            std::vector<U32> present;
            present.reserve(n);
            for (S32 i = 0; i < n; ++i)
            {
                if (chf.areas[i] != RC_NULL_AREA)
                {
                    present.push_back((static_cast<U32>(slope_map[chf.areas[i]]) << 16) | (spans.interior(i) ? comp[i] : STAMP_NONE));
                }
            }
            std::sort(present.begin(), present.end());
            present.erase(std::unique(present.begin(), present.end()), present.end());
            const auto fusedPairs = [&]() {
                std::vector<std::pair<U8, U8>> out_pairs;
                out_pairs.reserve(present.size());
                for (const U32 k : present)
                {
                    const U16 c = static_cast<U16>(k & 0xffff);
                    out_pairs.emplace_back(static_cast<U8>(k >> 16), c == STAMP_NONE ? 0 : colours[c]);
                }
                std::sort(out_pairs.begin(), out_pairs.end());
                out_pairs.erase(std::unique(out_pairs.begin(), out_pairs.end()), out_pairs.end());
                return out_pairs;
            };
            std::map<U16, std::set<U16>> nadj;
            colours = colourGroups(adj, group, &nadj);
            while (static_cast<S32>(used.size()) > cap)
            {
                std::map<U16, U32> gsize;
                for (size_t c = 0; c < ncomp; ++c)
                {
                    gsize[group[c]] += size[c];
                }
                S32 pick = -1;
                for (const auto& [g, s] : gsize)
                {
                    if (!nadj[g].empty() && (pick < 0 || s < gsize[static_cast<U16>(pick)]))
                    {
                        pick = g;
                    }
                }
                if (pick < 0)
                {
                    break;
                }
                S32 into = -1;
                for (const U16 x : nadj[static_cast<U16>(pick)])
                {
                    if (into < 0 || gsize[x] < gsize[static_cast<U16>(into)])
                    {
                        into = x;
                    }
                }
                const U16 lo = static_cast<U16>(std::min(pick, into)), hi = static_cast<U16>(std::max(pick, into));
                for (U16& g : group)
                {
                    g = g == hi ? lo : g;
                }
                ++out.mFused;
                colours = colourGroups(adj, group, &nadj);
                used = fusedPairs();
            }
        }
        if (static_cast<S32>(used.size()) > cap)
        {
            // Last, one colour: every code once, flights merged too if even that is too many.
            out.mOverflow = TileStamp::ONE_COLOUR;
            one = true;
            used = pairs();
            if (static_cast<S32>(used.size()) > cap)
            {
                std::map<U8, U8> least;
                std::set<U8> present;
                for (S32 i = 0; i < n; ++i)
                {
                    if (chf.areas[i] != RC_NULL_AREA)
                    {
                        present.insert(chf.areas[i]);
                    }
                }
                for (const U8 s : present)
                {
                    slope_map[s] = least.emplace(realOf(s), s).first->second;
                }
                used = pairs();
            }
        }

        std::vector<U8> index(256 * 256, 0);
        out.mPalette.assign(1, StampCode());
        for (const auto& p : used)
        {
            index[p.first * 256 + p.second] = static_cast<U8>(out.mPalette.size());
            out.mPalette.push_back({ p.first, p.second });
        }
        out.mPairs = static_cast<U32>(used.size());
        out.mSpanCode.assign(n, 0);
        for (S32 i = 0; i < n; ++i)
        {
            if (chf.areas[i] != RC_NULL_AREA)
            {
                out.mSpanCode[i] = index[slope_map[chf.areas[i]] * 256 + colourOf(i)];
            }
        }
        out.mComponentColour = one ? std::vector<U8>(ncomp, 1) : colours;
    }

    size_t TileStampGeom::bytes() const
    {
        return sizeof(*this) + mPolyComponent.capacity() * sizeof(U16) + mPolyGap.capacity() * sizeof(U32) +
               mPortalEdges.capacity() * sizeof(StampPortalEdge) + mPortals.capacity() * sizeof(StampPortal);
    }

    void stampGeometry(const rcPolyMesh& pmesh, const rcCompactHeightfield& chf, S32 border, const std::vector<U32>& span_gap,
                       const TileStampInput& input, const TileStamp& stamp, TileStampGeom& out)
    {
        SK_NAV_ZONE("nav.stamp.geometry");
        out = TileStampGeom();
        out.mStamped = true;
        out.mPattern = input.mPattern;
        out.mPortals = input.mPortals;
        out.mComponents = static_cast<U32>(stamp.mComponentColour.size());
        out.mOverflow = stamp.mOverflow;
        out.mPairs = stamp.mPairs;
        out.mDoorSpans = stamp.mDoorSpans;
        out.mFloodSpans = stamp.mFloodSpans;
        out.mUnspacedSpans = stamp.mUnspacedSpans;
        const S32 npolys = pmesh.npolys, nvp = pmesh.nvp;
        out.mPolyComponent.assign(npolys, STAMP_NONE);
        out.mPolyGap.assign(npolys, SPAN_NO_GAP);

        // Each region's component, for a poly too small to cover a cell centre: counted when the first such poly asks.
        std::vector<std::vector<std::pair<U16, U32>>> region_votes;
        bool voted = false;
        const auto regionComponent = [&](U16 reg) {
            if (!voted)
            {
                voted = true;
                for (S32 z = border; z < chf.height - border; ++z)
                {
                    for (S32 x = border; x < chf.width - border; ++x)
                    {
                        const rcCompactCell& c = chf.cells[x + z * chf.width];
                        for (U32 i = c.index, n = c.index + c.count; i < n; ++i)
                        {
                            const U16 r = chf.spans[i].reg;
                            if (chf.areas[i] != RC_NULL_AREA && r && !(r & RC_BORDER_REG))
                            {
                                if (r >= region_votes.size())
                                {
                                    region_votes.resize(r + 1u);
                                }
                                vote(region_votes[r], stamp.mSpanComponent[i]);
                            }
                        }
                    }
                }
            }
            return reg < region_votes.size() ? majorityOf(region_votes[reg]) : STAMP_NONE;
        };

        // Each poly's spans: those of its region in the columns whose centres it covers.
        std::vector<std::pair<U32, U32>> gaps;
        for (S32 p = 0; p < npolys; ++p)
        {
            const unsigned short* poly = &pmesh.polys[p * nvp * 2];
            S32 nv = 0;
            F32 vx[16], vz[16];
            S32 lo_x = INT_MAX, hi_x = INT_MIN, lo_z = INT_MAX, hi_z = INT_MIN, lo_y = INT_MAX, hi_y = INT_MIN;
            for (; nv < nvp && poly[nv] != RC_MESH_NULL_IDX; ++nv)
            {
                const unsigned short* v = &pmesh.verts[poly[nv] * 3];
                vx[nv] = v[0];
                vz[nv] = v[2];
                lo_x = std::min<S32>(lo_x, v[0]);
                hi_x = std::max<S32>(hi_x, v[0]);
                lo_z = std::min<S32>(lo_z, v[2]);
                hi_z = std::max<S32>(hi_z, v[2]);
                lo_y = std::min<S32>(lo_y, v[1]);
                hi_y = std::max<S32>(hi_y, v[1]);
            }
            const bool multi = pmesh.regs[p] == RC_MULTIPLE_REGS;
            F32 twice = 0.f;
            for (S32 k = 0; k < nv; ++k)
            {
                const S32 m = (k + 1) % nv;
                twice += vx[k] * vz[m] - vx[m] * vz[k];
            }
            const F32 sign = twice < 0.f ? -1.f : 1.f;
            const auto inside = [&](S32 x, S32 z) {
                const F32 px = x + 0.5f, pz = z + 0.5f;
                for (S32 k = 0; k < nv; ++k)
                {
                    const S32 m = k + 1 < nv ? k + 1 : 0;
                    if (!(sign * ((vx[m] - vx[k]) * (pz - vz[k]) - (vz[m] - vz[k]) * (px - vx[k])) >= 0.f))
                    {
                        return false;
                    }
                }
                return true;
            };
            std::vector<std::pair<U16, U32>> comps;
            gaps.clear();
            for (S32 z = lo_z; z < hi_z; ++z)
            {
                // A poly is convex, so the centres it covers in a row are a run: estimated from its edges, then settled by the test itself.
                const F32 pz = z + 0.5f;
                F32 lo = static_cast<F32>(lo_x), hi = static_cast<F32>(hi_x - 1);
                for (S32 k = 0; k < nv; ++k)
                {
                    const S32 m = k + 1 < nv ? k + 1 : 0;
                    const F32 a = sign * (vx[m] - vx[k]), b = sign * (vz[m] - vz[k]);
                    if (b != 0.f)
                    {
                        const F32 at = vx[k] + a * (pz - vz[k]) / b - 0.5f;
                        if (b > 0.f)
                        {
                            hi = std::min(hi, std::floor(at));
                        }
                        else
                        {
                            lo = std::max(lo, std::ceil(at));
                        }
                    }
                }
                S32 x0 = static_cast<S32>(std::max(lo, static_cast<F32>(lo_x))), x1 = static_cast<S32>(std::min(hi, static_cast<F32>(hi_x - 1)));
                while (x0 <= x1 && !inside(x0, z))
                {
                    ++x0;
                }
                if (x0 > x1)
                {
                    // Nothing where the edges put the run: look along the whole row.
                    for (x0 = lo_x; x0 < hi_x && !inside(x0, z); ++x0)
                    {
                    }
                    x1 = x0;
                }
                if (x0 >= hi_x)
                {
                    continue;
                }
                while (x0 > lo_x && inside(x0 - 1, z))
                {
                    --x0;
                }
                while (x1 > x0 && !inside(x1, z))
                {
                    --x1;
                }
                while (x1 + 1 < hi_x && inside(x1 + 1, z))
                {
                    ++x1;
                }
                for (S32 x = x0; x <= x1; ++x)
                {
                    const S32 cx = x + border, cz = z + border;
                    if (cx < 0 || cz < 0 || cx >= chf.width || cz >= chf.height)
                    {
                        continue;
                    }
                    const rcCompactCell& c = chf.cells[cx + cz * chf.width];
                    // A poly Recast retriangulated across regions has none: its span is the walkable one nearest its height.
                    S32 nearest = -1, nearest_d = MULTI_REGION_REACH + 1;
                    for (U32 i = c.index, n = c.index + c.count; multi && i < n; ++i)
                    {
                        const S32 y = chf.spans[i].y;
                        const S32 d = y < lo_y ? lo_y - y : (y > hi_y ? y - hi_y : 0);
                        if (chf.areas[i] != RC_NULL_AREA && d < nearest_d)
                        {
                            nearest = static_cast<S32>(i);
                            nearest_d = d;
                        }
                    }
                    for (U32 i = c.index, n = c.index + c.count; i < n; ++i)
                    {
                        if (multi ? static_cast<S32>(i) == nearest : chf.spans[i].reg == pmesh.regs[p] && chf.areas[i] != RC_NULL_AREA)
                        {
                            vote(comps, stamp.mSpanComponent[i]);
                            const U32 g = i < span_gap.size() ? span_gap[i] : SPAN_NO_GAP;
                            if (!gaps.empty() && gaps.back().first == g)
                            {
                                ++gaps.back().second;
                            }
                            else
                            {
                                gaps.emplace_back(g, 1u);
                            }
                            break;
                        }
                    }
                }
            }
            if (comps.empty())
            {
                out.mPolyComponent[p] = multi ? STAMP_NONE : regionComponent(pmesh.regs[p]);
            }
            else
            {
                out.mPolyComponent[p] = majorityOf(comps);
                out.mImpurePolys += comps.size() > 1 ? 1 : 0;
            }
            // The gap most of its spans stand in, the least on a tie: a poly spans many leaves, so its runs are sorted rather than tallied.
            std::sort(gaps.begin(), gaps.end());
            U32 most = 0;
            for (size_t a = 0, b = 0; a < gaps.size() && gaps[a].first != SPAN_NO_GAP; a = b)
            {
                U32 count = 0;
                for (b = a; b < gaps.size() && gaps[b].first == gaps[a].first; ++b)
                {
                    count += gaps[b].second;
                }
                if (count > most)
                {
                    out.mPolyGap[p] = gaps[a].first;
                    most = count;
                }
            }
        }

        // A poly in no component walkably joined to one takes its neighbour's, ring by ring.
        std::vector<S32> ring;
        for (S32 p = 0; p < npolys; ++p)
        {
            if (out.mPolyComponent[p] != STAMP_NONE)
            {
                ring.push_back(p);
            }
        }
        for (size_t head = 0; head < ring.size(); ++head)
        {
            const S32 p = ring[head];
            const unsigned short* poly = &pmesh.polys[p * nvp * 2];
            for (S32 e = 0; e < nvp && poly[e] != RC_MESH_NULL_IDX; ++e)
            {
                const unsigned short nei = poly[nvp + e];
                if (nei != RC_MESH_NULL_IDX && !(nei & 0x8000) && out.mPolyComponent[nei] == STAMP_NONE)
                {
                    out.mPolyComponent[nei] = out.mPolyComponent[p];
                    ++out.mJoinedPolys;
                    ring.push_back(nei);
                }
            }
        }

        // Poly edges along a portal between the poly's component and the one across it.
        const auto at = [&pmesh](unsigned short vi) {
            const unsigned short* v = &pmesh.verts[vi * 3];
            const F32 rc[3] = { pmesh.bmin[0] + v[0] * pmesh.cs, pmesh.bmin[1] + v[1] * pmesh.ch, pmesh.bmin[2] + v[2] * pmesh.cs };
            return LLVector3(rc[2], rc[0], rc[1]);
        };
        for (S32 p = 0; p < npolys && !input.mPortals.empty(); ++p)
        {
            const U16 a = out.mPolyComponent[p];
            if (a == STAMP_NONE)
            {
                continue;
            }
            const unsigned short* poly = &pmesh.polys[p * nvp * 2];
            S32 nv = 0;
            while (nv < nvp && poly[nv] != RC_MESH_NULL_IDX)
            {
                ++nv;
            }
            for (S32 e = 0; e < nv; ++e)
            {
                const unsigned short nei = poly[nvp + e];
                if (nei == RC_MESH_NULL_IDX)
                {
                    continue;
                }
                const bool seam = (nei & 0x8000) != 0;
                const U16 b = seam ? STAMP_NONE : out.mPolyComponent[nei];
                if (!seam && b == a)
                {
                    continue;
                }
                const LLVector3 va = at(poly[e]), vb = at(poly[(e + 1) % nv]);
                const LLVector3 mid = (va + vb) * 0.5f;
                const F32 ex = vb.mV[VX] - va.mV[VX], ey = vb.mV[VY] - va.mV[VY];
                const F32 elen = std::sqrt(ex * ex + ey * ey);
                S32 best = -1;
                F32 best_d = 0.5f;
                for (size_t k = 0; k < input.mPortals.size(); ++k)
                {
                    const StampPortal& sp = input.mPortals[k];
                    const bool sides = (sp.mSide[0] == a && (seam || sp.mSide[1] == b)) || (sp.mSide[1] == a && (seam || sp.mSide[0] == b));
                    if (!sides || std::fabs(mid.mV[VZ] - sp.mCentre.mV[VZ]) > 1.f)
                    {
                        continue;
                    }
                    // A seam edge has no space across to vouch for it: it lies on the portal only if it runs along it, within 30 degrees.
                    if (seam)
                    {
                        const F32 px = sp.mB.mV[VX] - sp.mA.mV[VX], py = sp.mB.mV[VY] - sp.mA.mV[VY];
                        const F32 plen = std::sqrt(px * px + py * py);
                        if (elen <= 0.f || plen <= 0.f || std::fabs(ex * py - ey * px) > 0.5f * elen * plen)
                        {
                            continue;
                        }
                    }
                    const F32 d = planDistance(mid.mV[VX], mid.mV[VY], sp.mA, sp.mB);
                    if (d <= best_d)
                    {
                        best = static_cast<S32>(k);
                        best_d = d;
                    }
                }
                if (best >= 0)
                {
                    const StampPortal& sp = input.mPortals[best];
                    out.mPortalEdges.push_back({ static_cast<U16>(p), static_cast<U8>(e), static_cast<U8>(sp.mSide[0] == a ? 0 : 1),
                                                 static_cast<U16>(best) });
                }
            }
        }
    }

    U64 portalKey(U32 space_a, U32 space_b, U8 kind, const LLVector3& centre)
    {
        U64 h = mix(mix(mix(SEED, std::min(space_a, space_b)), std::max(space_a, space_b)), kind);
        for (S32 k = 0; k < 3; ++k)
        {
            h = mix(h, static_cast<U64>(static_cast<S64>(std::lround(centre.mV[k] * 4.f))));
        }
        return h;
    }

    void bindStamp(const TileStampGeom& geom, const TileStampInput& input, const TileEnclosure* enclosure, const RoomsProduct& rooms,
                   TileBinding& out)
    {
        SK_NAV_ZONE("nav.stamp.bind");
        out = TileBinding();
        const auto spaceOf = [&](U16 c) -> const Space* {
            if (c == STAMP_NONE || c >= input.mComponentSpace.size() || input.mComponentSpace[c] >= rooms.mSpaces.size())
            {
                return nullptr;
            }
            return &rooms.mSpaces[input.mComponentSpace[c]];
        };
        out.mComponentSpaceId.assign(geom.mComponents, 0);
        for (U32 c = 0; c < geom.mComponents; ++c)
        {
            const Space* s = spaceOf(static_cast<U16>(c));
            out.mComponentSpaceId[c] = s ? s->mId : 0;
        }
        const auto bucketFlag = [](U8 bucket) -> U16 {
            switch (static_cast<EnclosureBucket>(bucket))
            {
                case EnclosureBucket::INDOORS: return POLY_INDOORS;
                case EnclosureBucket::SHELTERED: return POLY_SHELTERED;
                case EnclosureBucket::OUTDOORS: return POLY_OUTDOORS;
                default: return 0;
            }
        };
        const size_t npolys = geom.mPolyComponent.size();
        out.mPolyFlags.assign(npolys, 0);
        for (size_t p = 0; p < npolys; ++p)
        {
            const U32 g = geom.mPolyGap[p];
            if (enclosure && g != SPAN_NO_GAP && g < enclosure->mGaps.size())
            {
                out.mPolyFlags[p] = bucketFlag(enclosure->mGaps[g].mBucket);
            }
            else if (const Space* s = spaceOf(geom.mPolyComponent[p]))
            {
                out.mPolyFlags[p] = bucketFlag(static_cast<U8>(s->mBucket));
            }
        }
        out.mPortalKeys.reserve(geom.mPortalEdges.size());
        for (const StampPortalEdge& e : geom.mPortalEdges)
        {
            const StampPortal& sp = geom.mPortals[e.mPortal];
            const Space* a = spaceOf(sp.mSide[0]);
            const Space* b = spaceOf(sp.mSide[1]);
            out.mPortalKeys.push_back(portalKey(a ? a->mId : 0, b ? b->mId : 0, sp.mKind, sp.mCentre));
            // The geometry may be cached from another binding, so the index is the input's, the same portal by position and kind.
            const bool same = e.mPortal < input.mPortals.size() && input.mPortals[e.mPortal].mPortal < rooms.mPortals.size() &&
                              input.mPortals[e.mPortal].mKind == sp.mKind && dist_vec(input.mPortals[e.mPortal].mCentre, sp.mCentre) < 0.01f;
            out.mPortalIndex.push_back(same ? input.mPortals[e.mPortal].mPortal : NO_PORTAL);
            if (sp.mKind == static_cast<U8>(PortalKind::DOOR) && e.mPoly < npolys)
            {
                out.mPolyFlags[e.mPoly] |= POLY_DOOR;
            }
        }
        U64 h = mix(SEED, geom.mPattern);
        for (const U32 id : out.mComponentSpaceId)
        {
            h = mix(h, id);
        }
        for (const U16 f : out.mPolyFlags)
        {
            h = mix(h, f);
        }
        for (const U64 k : out.mPortalKeys)
        {
            h = mix(h, k);
        }
        for (const U32 i : out.mPortalIndex)
        {
            h = mix(h, i);
        }
        out.mHash = h;
    }
}
