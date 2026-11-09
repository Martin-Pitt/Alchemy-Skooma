/**
 * @file skroadland.cpp
 * @brief Land the road driver reads: parcel ownership along region edges, Linden land, ban lines and private land.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
#include "skroadhost.h"
#include "skroadland.h"
#include "skroadmodel.h"
#include "alobjectproperties.h"
#include "llagent.h"
#include "lldir.h"
#include "llfile.h"
#include "llglheaders.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llvoavatarself.h"
#include "llavatarnamecache.h"
#include "llsdjson.h"
#include "llparcel.h"
#include "llviewerparcelmgr.h"
#include "llworld.h"
#include "llviewerparceloverlay.h"
#include "llremoteparcelrequest.h"
#include <memory>
#include "llavatarpropertiesprocessor.h"
#include "skaircells.h"
#include "skairdistil.h"
#include "skworldcensus.h"
#include "skworldoverlay.h"
#include <boost/iterator/indirect_iterator.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace skroad;
using skroad::host::lindenOwnerState;

namespace skroad::land
{
    const char* ownershipName(S32 own)
    {
        switch (own)
        {
            case -1: return "not loaded";
            case PARCEL_PUBLIC: return "Linden";
            case PARCEL_OWNED: return "someone else's";
            case PARCEL_GROUP: return "group";
            case PARCEL_SELF: return "own";
            case PARCEL_FOR_SALE: return "for sale";
            case PARCEL_AUCTION: return "auction";
            default: return "other";
        }
    }

    // The edge map of the car's region: each 4 m overlay cell along each edge (0 south, 1 east, 2 north, 3 west, cells
    // west to east or south to north), safe where Linden land meets Linden land across the edge; unsafe where the
    // cell inside is not Linden land, the neighbour's cell across is not (someone's, a group's, not loaded), or there
    // is no neighbour. Made again every EDGE_MAP_S (the overlays arrive and change over seconds; 256 lookups), and
    // logged once per region and whenever its counts change.
    // (EDGE_CELLS, EdgeCell and EdgeMap are in skroaddriveinternal.h.)
    constexpr F64 EDGE_MAP_S = 2.0;
    EdgeMap sEdges;

    // The owners of the parcels along the edges. The overlay gives each cell's ownership class and the parcel lines
    // but no owner: each edge is split at the lines into its parcels, each parcel asked for once through the
    // RemoteParcelRequest cap (a point inside it, its parcel id) and ParcelInfoRequest (its owner), cached by region
    // and the cell asked at, and by parcel id. A Linden road parcel: Governor Linden's, LDPW's, or a Linden's or
    // mole's (lindenOwner). Not yet answered: unknown, never a hazard (unsafeCrossingAhead waits on it); asked
    // again after 30 s, or 5 s after a failure. Asked through the agent region's cap whichever region the parcel is
    // in (the cap takes the region id and handle): a child region's cap answered 404 for every one (Uba, 2026-11-07),
    // and the map is kept made through the whole run, not only near an edge, so the owners are in before the car is.
    struct EdgeAskStats { S32 asked = 0, answered = 0, failed = 0; };
    EdgeAskStats sEdgeAsks;

    struct ParcelAsk;
    std::map<std::pair<U64, U32>, std::unique_ptr<ParcelAsk>> sParcelAsks;     // (region handle, row << 16 | col)
    std::map<LLUUID, Road> sParcelRoad;                                         // by parcel id

    struct ParcelAsk : public LLRemoteParcelInfoObserver
    {
        LLUUID parcel;
        F64 asked = -1e9;
        Road road = Road::Unknown;
        LLUUID owner;
        bool group_owned = false;
        F64 answered = -1.0;        // when its owner arrived (the owner's profile and name get 20 s to arrive)
        bool via_own_cap = false;   // after a failure through one cap, the other is asked: the agent region's, its own
        std::string region_name;
        LLVector3 at;

        void setParcelID(const LLUUID& id) override
        {
            parcel = id;
            if (auto it = sParcelRoad.find(id); it != sParcelRoad.end() && it->second != Road::Unknown)
            {
                road = it->second;
                return;
            }
            LLRemoteParcelInfoProcessor::getInstance()->addObserver(id, this);
            LLRemoteParcelInfoProcessor::getInstance()->sendParcelInfoRequest(id);
        }
        void processParcelInfo(const LLParcelData& d) override
        {
            LLRemoteParcelInfoProcessor::getInstance()->removeObserver(d.parcel_id, this);
            ++sEdgeAsks.answered;
            owner = d.owner_id;
            // ParcelInfoReply's flags: 0x1 mature, 0x2 adult, 0x4 group owned (owner_id is then the group's id)
            group_owned = (d.flags & 0x4) != 0;
            answered = LLTimer::getTotalSeconds();
            classify(answered);
            if (road != Road::Unknown)
            {
                sParcelRoad[d.parcel_id] = road;
            }
            // ids only, no names: an owner or group missed as a road's shows here
            LL_INFOS("SKRoadDriver") << "edge parcel " << d.parcel_id << " in " << d.sim_name << " owner " << d.owner_id
                                     << (group_owned ? " (group)" : "") << llformat(" flags 0x%x", U32(d.flags))
                                     << (road == Road::Linden  ? ": Linden road"
                                         : road == Road::Other ? ": not Linden"
                                                               : ": owner still being learnt")
                                     << LL_ENDL;
        }
        // A Linden road: Governor Linden's, LDPW's (owned by or deeded to the group), or a Linden's or mole's
        // (lindenOwner). Any other group: not Linden. An owner still being learnt: unknown, never someone else's,
        // for up to 20 s after the answer, then not Linden (a profile that never answers).
        void classify(F64 now)
        {
            if (answered < 0.0)
            {
                return;
            }
            if (owner == skroad::LDPW || owner == GOVERNOR_LINDEN)
            {
                road = Road::Linden;
            }
            else if (group_owned || owner.isNull())
            {
                road = Road::Other;
            }
            else
            {
                const S32 state = lindenOwnerState(owner);
                road = state == 1 ? Road::Linden : (state == 0 || now - answered > 20.0) ? Road::Other : Road::Unknown;
            }
        }
        void setErrorStatus(S32 status, const std::string& reason) override
        {
            ++sEdgeAsks.failed;
            asked -= 25.0;      // asked again in 5 s rather than 30
            if (sEdgeAsks.failed <= 12)
            {
                LL_WARNS("SKRoadDriver") << "edge parcel request failed: " << status << " " << reason << " for "
                                         << region_name << " at (" << at.mV[VX] << ", " << at.mV[VY] << ") via "
                                         << (via_own_cap ? "its region's cap" : "the agent region's cap") << LL_ENDL;
            }
            via_own_cap = !via_own_cap;     // the other cap next time
        }
    };

    // The road state of the parcel holding overlay cell (row, col) of `region`, asked for where not yet known.
    Road edgeParcel(LLViewerRegion* region, S32 row, S32 col, F64 now)
    {
        auto& ask = sParcelAsks[{ region->getHandle(), (U32(row) << 16) | U32(col) }];
        if (!ask)
        {
            ask = std::make_unique<ParcelAsk>();
        }
        if (ask->answered >= 0.0 && ask->road != Road::Linden)
        {
            ask->classify(LLTimer::getTotalSeconds());     // its owner since learnt a Linden or a mole, or given up on
            if (ask->road != Road::Unknown && ask->parcel.notNull())
            {
                sParcelRoad[ask->parcel] = ask->road;
            }
        }
        if (ask->road == Road::Unknown && ask->answered < 0.0 && now - ask->asked > 30.0)
        {
            LLViewerRegion* agent_region = gAgent.getRegion();
            const std::string agent_url = agent_region ? agent_region->getCapability("RemoteParcelRequest") : std::string();
            const std::string own_url = region->getCapability("RemoteParcelRequest");
            const std::string& url = (ask->via_own_cap && !own_url.empty()) || agent_url.empty() ? own_url : agent_url;
            if (!url.empty())
            {
                ask->asked = now;
                ++sEdgeAsks.asked;
                // the cell's centre, 2 m inside on both axes: never a coordinate on the edge (0 or 256)
                const LLVector3 at(F32(col * 4 + 2), F32(row * 4 + 2), 0.f);
                ask->region_name = region->getName();
                ask->at = at;
                LLRemoteParcelInfoProcessor::getInstance()->requestRegionParcelInfo(
                    url, region->getRegionID(), at, region->getPosGlobalFromRegion(at), ask->getObserverHandle());
            }
        }
        return ask->road;
    }

    // Along one side of an edge: each cell's parcel's road state, the edge split into parcels at the overlay's lines,
    // one ask per parcel (at its first cell). row/col of cell i: fixed `line` across, i along.
    // What each side said last, by region and line: an overlay not in for a moment (around a crossing) keeps it
    // rather than making the whole side unknown, which held the car short of Gomes' west edge on each entry.
    struct EdgeSideKnown
    {
        std::array<S32, EDGE_CELLS> own;
        std::array<Road, EDGE_CELLS> road;
        EdgeSideKnown()
        {
            own.fill(-1);
            road.fill(Road::Unknown);
        }
    };
    std::map<std::tuple<U64, bool, S32>, EdgeSideKnown> sEdgeSidesKnown;

    void edgeSide(LLViewerRegion* region, bool along_x, S32 line, F64 now, std::array<S32, EDGE_CELLS>& own,
                  std::array<Road, EDGE_CELLS>& road)
    {
        const LLViewerParcelOverlay* overlay = region ? region->getParcelOverlay() : nullptr;
        const S32 n = region ? S32(region->getWidth() / 4) : 0;
        if (!overlay || n != EDGE_CELLS)
        {
            own.fill(-1);
            road.fill(Road::Unknown);
            if (region)
            {
                if (auto it = sEdgeSidesKnown.find({ region->getHandle(), along_x, line }); it != sEdgeSidesKnown.end())
                {
                    own = it->second.own;
                    road = it->second.road;
                }
            }
            return;
        }
        Road seg = Road::Unknown;
        for (S32 i = 0; i < EDGE_CELLS; ++i)
        {
            const S32 row = along_x ? line : i, col = along_x ? i : line;
            own[i] = overlay->ownership(LLVector3(F32(col * 4 + 2), F32(row * 4 + 2), 0.f)) & PARCEL_COLOR_MASK;
            // a new parcel where a line crosses the edge between this cell and the last
            const bool split = i == 0 || (overlay->parcelLineFlags(row, col) & (along_x ? PARCEL_WEST_LINE : PARCEL_SOUTH_LINE));
            if (split)
            {
                seg = edgeParcel(region, row, col, now);
            }
            road[i] = seg;
        }
        // kept per cell: a cell known before stays known through a moment's not-known-again
        EdgeSideKnown& kept = sEdgeSidesKnown[{ region->getHandle(), along_x, line }];
        for (S32 i = 0; i < EDGE_CELLS; ++i)
        {
            if (road[i] == Road::Unknown && kept.road[i] != Road::Unknown && kept.own[i] == own[i])
            {
                road[i] = kept.road[i];
            }
            kept.own[i] = own[i];
            kept.road[i] = road[i];
        }
    }

    const EdgeMap& edgeMap(LLViewerRegion* region, F64 now)
    {
        if (region == sEdges.region && now - sEdges.made < EDGE_MAP_S)
        {
            return sEdges;
        }
        const bool same = region == sEdges.region;
        sEdges.region = region;
        sEdges.made = now;
        const LLVector3d o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        std::string counts;
        for (S32 e = 0; e < 4; ++e)
        {
            // the neighbour across this edge, and the line of cells on each side
            const LLVector3d probe = o + (e == 0 ? LLVector3d(w / 2, -2, 0) : e == 1 ? LLVector3d(w + 2, w / 2, 0)
                                          : e == 2 ? LLVector3d(w / 2, w + 2, 0) : LLVector3d(-2, w / 2, 0));
            LLViewerRegion* nb = LLWorld::getInstance()->getRegionFromPosGlobal(probe);
            const bool along_x = e == 0 || e == 2;
            const S32 last = EDGE_CELLS - 1;
            std::array<S32, EDGE_CELLS> own_in, own_nb;
            std::array<Road, EDGE_CELLS> road_in, road_nb;
            edgeSide(region, along_x, e == 0 || e == 3 ? 0 : last, now, own_in, road_in);
            if (nb)
            {
                edgeSide(nb, along_x, e == 0 || e == 3 ? last : 0, now, own_nb, road_nb);
            }
            S32 safe = 0, unknown = 0;
            for (S32 i = 0; i < EDGE_CELLS; ++i)
            {
                EdgeCell& c = sEdges.cells[e][i];
                c.inside = own_in[i];
                c.across = nb ? own_nb[i] : -2;
                c.road_in = road_in[i];
                c.road_across = nb ? road_nb[i] : Road::Unknown;
                c.safe = nb && c.road_in == Road::Linden && c.road_across == Road::Linden;
                safe += c.safe;
                unknown += !c.safe && (c.road_in == Road::Unknown || (nb && c.road_across == Road::Unknown));
            }
            static const char* names[4] = { "south", "east", "north", "west" };
            counts += llformat("%s %d safe/%d unsafe (%d unknown); ", names[e], safe, EDGE_CELLS - safe, unknown);
        }
        if (!same || counts != sEdges.counts)
        {
            LL_INFOS("SKRoadDriver") << "edge map of " << region->getName() << ": " << counts
                                     << llformat("parcel asks %d, answered %d, failed %d", sEdgeAsks.asked,
                                                 sEdgeAsks.answered, sEdgeAsks.failed)
                                     << LL_ENDL;
        }
        sEdges.counts = counts;
        return sEdges;
    }

}

namespace skroad::land
{
    // Linden land (the road parcels: the parcel overlay's public land) at a point.
    bool lindenLandAt(F64 x, F64 y)
    {
        const LLVector3d at(x, y, 0);
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        const LLViewerParcelOverlay* overlay = region ? region->getParcelOverlay() : nullptr;
        return overlay && (overlay->ownership(region->getPosRegionFromGlobal(at)) & PARCEL_COLOR_MASK) == PARCEL_PUBLIC;
    }

}

namespace skroad::land
{
    // ---------------------------------------------------------------- private land and ban lines

    // Nexii's rules (2026-11-06, after Hollow Gardens' security orb banned the bot): private land can be road (a
    // resident's road on their own parcel), but the car never leaves the road onto private land that is not road
    // (gardens, homes), and a parcel it may not enter (ban lines: banned, not on the access list, not in the group)
    // is a wall. A line that would take the car there is cut short of it and its end is a road end (stop, turn round).

    // The ban lines the simulator sent (its collision bitmap, a 4 m cell a bit, for the parcel the agent neared),
    // kept per region for the session: each is a parcel the agent may not enter.
    std::unordered_map<U64, std::vector<U8>> sBanCells;
    boost::signals2::connection sBanWatch;
    void watchBanLines()
    {
        if (sBanWatch.connected())
        {
            return;
        }
        sBanWatch = LLViewerParcelMgr::getInstance()->setCollisionUpdateCallback([](const LLViewerRegion* region)
        {
            LLViewerParcelMgr* mgr = LLViewerParcelMgr::getInstance();
            const U8* bits = mgr->getCollisionBitmap();
            if (!region || !bits)
            {
                return;
            }
            std::vector<U8>& held = sBanCells[region->getHandle()];
            held.resize(mgr->getCollisionBitmapSize(), 0);
            for (size_t i = 0; i < held.size(); ++i)
            {
                held[i] |= bits[i];
            }
            LL_INFOS("SKRoadDriver") << "ban lines in " << region->getName() << LL_ENDL;
        });
    }

    bool banLineAt(F64 x, F64 y)
    {
        const LLVector3d at(x, y, 0);
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        auto it = region ? sBanCells.find(region->getHandle()) : sBanCells.end();
        if (it == sBanCells.end() || it->second.empty())
        {
            return false;
        }
        const S32 n = S32(std::lround(std::sqrt(F64(it->second.size() * 8))));
        const LLVector3 r = region->getPosRegionFromGlobal(at);
        const F64 w = region->getWidth();
        const S32 col = std::clamp(S32(r.mV[VX] * n / w), 0, n - 1), row = std::clamp(S32(r.mV[VY] * n / w), 0, n - 1);
        const S32 bit = col + row * n;
        return (it->second[bit / 8] >> (bit % 8)) & 1;
    }

    // Someone else's land (not Linden land, not the agent's nor its group's).
    bool privateLandAt(F64 x, F64 y)
    {
        const LLVector3d at(x, y, 0);
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        const LLViewerParcelOverlay* overlay = region ? region->getParcelOverlay() : nullptr;
        if (!overlay)
        {
            return false;
        }
        const U8 own = overlay->ownership(region->getPosRegionFromGlobal(at)) & PARCEL_COLOR_MASK;
        return own != PARCEL_PUBLIC && own != PARCEL_SELF && own != PARCEL_GROUP;
    }

    // Where the car may not go: a ban line, or private land with no road piece under the point. Null where it may.
    const char* refusedAt(F64 x, F64 y, F64 z)
    {
        if (banLineAt(x, y))
        {
            return "a ban line";
        }
        F64 top;
        if (privateLandAt(x, y) && !roadTopAt(x, y, z, 1.5, 3.0, top))
        {
            return "private land off the road";
        }
        return nullptr;
    }
}

namespace skroad::land
{
    bool parcelCovers(const LLParcel* p, LLViewerRegion* region, const LLVector3d& at)
    {
        const LLVector3 local = region->getPosRegionFromGlobal(at);
        const LLVector3& lo = p->getAABBMin();
        const LLVector3& hi = p->getAABBMax();
        return local.mV[VX] >= lo.mV[VX] - 1.f && local.mV[VX] <= hi.mV[VX] + 1.f && local.mV[VY] >= lo.mV[VY] - 1.f
               && local.mV[VY] <= hi.mV[VY] + 1.f;
    }

    // The cells of the parcel whose lines enclose a global point, by flood fill across edges with no line.
    bool parcelCellsAt(const LLVector3d& at, LineCells& out)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        const LLViewerParcelOverlay* ov = region ? region->getParcelOverlay() : nullptr;
        if (!ov)
        {
            return false;
        }
        const S32 n = S32(region->getWidth() / PARCEL_GRID_STEP_METERS);
        const LLVector3 local = region->getPosRegionFromGlobal(at);
        const S32 r0 = llclamp(S32(local.mV[VY] / PARCEL_GRID_STEP_METERS), 0, n - 1);
        const S32 c0 = llclamp(S32(local.mV[VX] / PARCEL_GRID_STEP_METERS), 0, n - 1);
        out.region = region;
        out.n = n;
        out.in.assign(size_t(n * n), 0);
        std::vector<std::pair<S32, S32>> todo{ { r0, c0 } };
        out.in[r0 * n + c0] = 1;
        while (!todo.empty())
        {
            const auto [r, c] = todo.back();
            todo.pop_back();
            const U8 here = ov->parcelLineFlags(r, c);
            auto go = [&](S32 rr, S32 cc, bool open)
            {
                if (open && rr >= 0 && cc >= 0 && rr < n && cc < n && !out.in[rr * n + cc])
                {
                    out.in[rr * n + cc] = 1;
                    todo.emplace_back(rr, cc);
                }
            };
            go(r, c - 1, !(here & PARCEL_WEST_LINE));
            go(r - 1, c, !(here & PARCEL_SOUTH_LINE));
            go(r, c + 1, c + 1 < n && !(ov->parcelLineFlags(r, c + 1) & PARCEL_WEST_LINE));
            go(r + 1, c, r + 1 < n && !(ov->parcelLineFlags(r + 1, c) & PARCEL_SOUTH_LINE));
        }
        return true;
    }

    bool cellsCover(const LineCells& cells, const LLVector3d& at)
    {
        if (!cells.region || LLWorld::getInstance()->getRegionFromPosGlobal(at) != cells.region)
        {
            return false;
        }
        const LLVector3 local = cells.region->getPosRegionFromGlobal(at);
        return cells.has(S32(local.mV[VY] / PARCEL_GRID_STEP_METERS), S32(local.mV[VX] / PARCEL_GRID_STEP_METERS));
    }

    // The point of the cells nearest `to` at least margin from their lines (a cell's side shared with another
    // of its cells is no line); false when there are none.
    bool insidePoint(const LineCells& cells, const LLVector3d& to, F64 margin, LLVector3d& out)
    {
        const LLVector3d origin = cells.region->getOriginGlobal();
        const F64 tx = to.mdV[VX] - origin.mdV[VX], ty = to.mdV[VY] - origin.mdV[VY];
        const F64 g = PARCEL_GRID_STEP_METERS;
        F64 best = 1e30;
        for (S32 r = 0; r < cells.n; ++r)
        {
            for (S32 c = 0; c < cells.n; ++c)
            {
                if (!cells.has(r, c))
                {
                    continue;
                }
                const F64 x0 = c * g + (cells.has(r, c - 1) ? 0.0 : margin);
                const F64 x1 = (c + 1) * g - (cells.has(r, c + 1) ? 0.0 : margin);
                const F64 y0 = r * g + (cells.has(r - 1, c) ? 0.0 : margin);
                const F64 y1 = (r + 1) * g - (cells.has(r + 1, c) ? 0.0 : margin);
                const F64 x = llclamp(tx, x0, x1), y = llclamp(ty, y0, y1);
                const F64 d = std::hypot(x - tx, y - ty);
                if (d < best)
                {
                    best = d;
                    out.set(origin.mdV[VX] + x, origin.mdV[VY] + y, 0.0);
                }
            }
        }
        if (best > 1e29)
        {
            return false;
        }
        out.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(out);
        return true;
    }

    std::map<U64, std::vector<S32>> sLabels;        // region handle -> the overlay's parcel of each 4 m cell

    // Each 4 m cell of the region's overlay labelled with its parcel: the cells reached without crossing a line.
    const std::vector<S32>& parcelLabels(LLViewerRegion* region)
    {
        std::vector<S32>& labels = sLabels[region->getHandle()];
        const LLViewerParcelOverlay* overlay = region->getParcelOverlay();
        const S32 n = S32(region->getWidth() / PARCEL_GRID_STEP_METERS);
        if (!labels.empty() || !overlay || n <= 0)
        {
            return labels;
        }
        labels.assign(size_t(n) * n, -1);
        S32 next = 0;
        std::vector<S32> todo;
        for (S32 start = 0; start < n * n; ++start)
        {
            if (labels[start] >= 0)
            {
                continue;
            }
            labels[start] = next;
            todo.push_back(start);
            while (!todo.empty())
            {
                const S32 cell = todo.back();
                todo.pop_back();
                const S32 r = cell / n, c = cell % n;
                auto go = [&](S32 rr, S32 cc)
                {
                    if (rr >= 0 && rr < n && cc >= 0 && cc < n && labels[rr * n + cc] < 0)
                    {
                        labels[rr * n + cc] = next;
                        todo.push_back(rr * n + cc);
                    }
                };
                const U8 here = overlay->parcelLineFlags(r, c);
                if (!(here & PARCEL_WEST_LINE))
                {
                    go(r, c - 1);
                }
                if (!(here & PARCEL_SOUTH_LINE))
                {
                    go(r - 1, c);
                }
                if (c + 1 < n && !(overlay->parcelLineFlags(r, c + 1) & PARCEL_WEST_LINE))
                {
                    go(r, c + 1);
                }
                if (r + 1 < n && !(overlay->parcelLineFlags(r + 1, c) & PARCEL_SOUTH_LINE))
                {
                    go(r + 1, c);
                }
            }
            ++next;
        }
        return labels;
    }

    S32 parcelLabelAt(const LLVector3d& at, U64& handle)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        if (!region || !region->getParcelOverlay())
        {
            return -1;
        }
        const std::vector<S32>& labels = parcelLabels(region);
        const S32 n = S32(region->getWidth() / PARCEL_GRID_STEP_METERS);
        if (labels.size() != size_t(n) * n)
        {
            return -1;
        }
        const LLVector3 p = region->getPosRegionFromGlobal(at);
        const S32 row = std::clamp(S32(p.mV[VY] / PARCEL_GRID_STEP_METERS), 0, n - 1);
        const S32 col = std::clamp(S32(p.mV[VX] / PARCEL_GRID_STEP_METERS), 0, n - 1);
        handle = region->getHandle();
        return labels[row * n + col];
    }

    void forgetParcelLabels()
    {
        sLabels.clear();
    }
}
