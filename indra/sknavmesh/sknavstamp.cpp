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

#include "Recast.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

namespace sknav
{
    namespace
    {
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

        /// The door portal whose threshold holds a point: across its run, within STAMP_THRESHOLD_M of its plane, at its sill.
        const StampPortal* thresholdOf(const TileStampInput& input, F32 px, F32 py, F32 pz)
        {
            for (const StampPortal& p : input.mPortals)
            {
                if (p.mKind != static_cast<U8>(PortalKind::DOOR) || std::fabs(pz - p.mCentre.mV[VZ]) > 1.f)
                {
                    continue;
                }
                const F32 dx = p.mB.mV[VX] - p.mA.mV[VX], dy = p.mB.mV[VY] - p.mA.mV[VY];
                const F32 len = std::sqrt(dx * dx + dy * dy);
                if (len <= 0.f)
                {
                    continue;
                }
                const F32 along = ((px - p.mA.mV[VX]) * dx + (py - p.mA.mV[VY]) * dy) / len;
                const F32 across = (px - p.mCentre.mV[VX]) * p.mNormal.mV[VX] + (py - p.mCentre.mV[VY]) * p.mNormal.mV[VY];
                if (along >= 0.f && along <= len && std::fabs(across) <= STAMP_THRESHOLD_M)
                {
                    return &p;
                }
            }
            return nullptr;
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
        out = TileStamp();
        const S32 n = chf.spanCount;
        const Spans spans(chf, border);
        std::vector<U16>& comp = out.mSpanComponent;
        comp.assign(n, STAMP_NONE);
        std::vector<U8> fillable(n, 0);
        for (S32 i = 0; i < n; ++i)
        {
            if (chf.areas[i] == RC_NULL_AREA || !spans.interior(i))
            {
                continue;
            }
            const F32 px = chf.bmin[2] + (spans.mZ[i] + 0.5f) * chf.cs;
            const F32 py = chf.bmin[0] + (spans.mX[i] + 0.5f) * chf.cs;
            const F32 pz = chf.bmin[1] + chf.spans[i].y * chf.ch;
            // A door's threshold is split at its leaf's plane, each half to the space on its side.
            if (const StampPortal* door = thresholdOf(input, px, py, pz))
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
            for (size_t k = 0; k < input.mPortals.size(); ++k)
            {
                const StampPortal& p = input.mPortals[k];
                if (std::fabs(pz - p.mCentre.mV[VZ]) > 1.f)
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
        }

        // What no portal reaches takes the component of the floor it joins.
        std::deque<S32> queue;
        for (S32 i = 0; i < n; ++i)
        {
            if (comp[i] != STAMP_NONE)
            {
                queue.push_back(i);
            }
        }
        while (!queue.empty())
        {
            const S32 i = queue.front();
            queue.pop_front();
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
        const auto pairs = [&]() {
            std::set<std::pair<U8, U8>> s;
            for (S32 i = 0; i < n; ++i)
            {
                if (chf.areas[i] != RC_NULL_AREA)
                {
                    s.emplace(slope_map[chf.areas[i]], colourOf(i));
                }
            }
            return s;
        };
        const auto realOf = [&real](U8 s) { return s < real.size() ? real[s] : s; };
        const S32 cap = llclamp(params.mPaletteMax, 1, 63);
        std::set<std::pair<U8, U8>> used = pairs();
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
            while (static_cast<S32>(used.size()) > cap)
            {
                std::map<U16, U32> gsize;
                for (size_t c = 0; c < ncomp; ++c)
                {
                    gsize[group[c]] += size[c];
                }
                std::map<U16, std::set<U16>> nadj;
                colourGroups(adj, group, &nadj);
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
                colours = colourGroups(adj, group, nullptr);
                used = pairs();
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

        std::map<std::pair<U8, U8>, U8> index;
        out.mPalette.assign(1, StampCode());
        for (const auto& p : used)
        {
            index[p] = static_cast<U8>(out.mPalette.size());
            out.mPalette.push_back({ p.first, p.second });
        }
        out.mPairs = static_cast<U32>(used.size());
        out.mSpanCode.assign(n, 0);
        for (S32 i = 0; i < n; ++i)
        {
            if (chf.areas[i] != RC_NULL_AREA)
            {
                out.mSpanCode[i] = index[{ slope_map[chf.areas[i]], colourOf(i) }];
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

        // Each region's component, for a poly too small to cover a cell centre.
        std::map<U16, std::map<U16, U32>> region_votes;
        for (S32 z = border; z < chf.height - border; ++z)
        {
            for (S32 x = border; x < chf.width - border; ++x)
            {
                const rcCompactCell& c = chf.cells[x + z * chf.width];
                for (U32 i = c.index, n = c.index + c.count; i < n; ++i)
                {
                    if (chf.areas[i] != RC_NULL_AREA && chf.spans[i].reg && !(chf.spans[i].reg & RC_BORDER_REG))
                    {
                        ++region_votes[chf.spans[i].reg][stamp.mSpanComponent[i]];
                    }
                }
            }
        }
        const auto majority = [](const std::map<U16, U32>& votes) {
            U16 best = STAMP_NONE;
            U32 most = 0;
            for (const auto& [k, v] : votes)
            {
                if (v > most)
                {
                    best = k;
                    most = v;
                }
            }
            return best;
        };

        // Each poly's spans: those of its region in the columns whose centres it covers.
        for (S32 p = 0; p < npolys; ++p)
        {
            const unsigned short* poly = &pmesh.polys[p * nvp * 2];
            S32 nv = 0;
            F32 vx[16], vz[16];
            S32 lo_x = INT_MAX, hi_x = INT_MIN, lo_z = INT_MAX, hi_z = INT_MIN;
            for (; nv < nvp && poly[nv] != RC_MESH_NULL_IDX; ++nv)
            {
                const unsigned short* v = &pmesh.verts[poly[nv] * 3];
                vx[nv] = v[0];
                vz[nv] = v[2];
                lo_x = std::min<S32>(lo_x, v[0]);
                hi_x = std::max<S32>(hi_x, v[0]);
                lo_z = std::min<S32>(lo_z, v[2]);
                hi_z = std::max<S32>(hi_z, v[2]);
            }
            F32 twice = 0.f;
            for (S32 k = 0; k < nv; ++k)
            {
                const S32 m = (k + 1) % nv;
                twice += vx[k] * vz[m] - vx[m] * vz[k];
            }
            const F32 sign = twice < 0.f ? -1.f : 1.f;
            std::map<U16, U32> comps;
            std::map<U32, U32> gaps;
            for (S32 z = lo_z; z < hi_z; ++z)
            {
                for (S32 x = lo_x; x < hi_x; ++x)
                {
                    const F32 px = x + 0.5f, pz = z + 0.5f;
                    bool inside = true;
                    for (S32 k = 0; k < nv && inside; ++k)
                    {
                        const S32 m = (k + 1) % nv;
                        inside = sign * ((vx[m] - vx[k]) * (pz - vz[k]) - (vz[m] - vz[k]) * (px - vx[k])) >= 0.f;
                    }
                    const S32 cx = x + border, cz = z + border;
                    if (!inside || cx < 0 || cz < 0 || cx >= chf.width || cz >= chf.height)
                    {
                        continue;
                    }
                    const rcCompactCell& c = chf.cells[cx + cz * chf.width];
                    for (U32 i = c.index, n = c.index + c.count; i < n; ++i)
                    {
                        if (chf.spans[i].reg == pmesh.regs[p] && chf.areas[i] != RC_NULL_AREA)
                        {
                            ++comps[stamp.mSpanComponent[i]];
                            ++gaps[i < span_gap.size() ? span_gap[i] : SPAN_NO_GAP];
                            break;
                        }
                    }
                }
            }
            if (comps.empty())
            {
                const auto it = region_votes.find(pmesh.regs[p]);
                out.mPolyComponent[p] = it != region_votes.end() ? majority(it->second) : STAMP_NONE;
            }
            else
            {
                out.mPolyComponent[p] = majority(comps);
                out.mImpurePolys += comps.size() > 1 ? 1 : 0;
            }
            U32 most = 0;
            for (const auto& [g, v] : gaps)
            {
                if (g != SPAN_NO_GAP && v > most)
                {
                    out.mPolyGap[p] = g;
                    most = v;
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
        out.mHash = h;
    }
}
