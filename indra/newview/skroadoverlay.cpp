/**
 * @file skroadoverlay.cpp
 * @brief The road driver's overlay: the road, lanes, line and trail drawn in the world.
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
#include "skroaddriver.h"
#include "skroaddriveinternal.h"
#include "skroadmodel.h"
#include "skroadroutefollow.h"
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
#include "skroaddriveinternal.h"

using namespace skroad;
using namespace skroad::drive;


void sk_road_driver::renderDebug()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKRoadAutopilotOverlay", true);
    if ((!sOn && !sPreviewing) || !show || !sView.valid || overlayHidden())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;

    // House style: thin lines, one saturated colour per series, white for the answer.
    static const LLColor4 FAINT(0.55f, 0.55f, 0.6f, 0.25f);    // road shapes in reach
    static const LLColor4 BLUE(0.3f, 0.45f, 1.f, 0.9f);        // a cross-section across the road, edge to edge
    static const LLColor4 AMBER(1.f, 0.7f, 0.f, 1.f);          // the lane path: the line it keeps, walked ahead
    static const LLColor4 VIOLET(0.65f, 0.4f, 1.f, 1.f);       // the car predicted with no new tap
    static const LLColor4 RED(1.f, 0.2f, 0.2f, 1.f);           // a tap; waiting for the road
    static const LLColor4 PINK(1.f, 0.4f, 0.8f, 1.f);          // the lane path when no linkset chain was found

    const F64 lift = 0.25;
    auto at = [](F64 x, F64 y, F64 z) { return gAgent.getPosAgentFromGlobal(LLVector3d(x, y, z)); };
    const F64 zc = sView.car.mdV[VZ];

    // Built at most four times a second (and when the agent's region, so its frame, changes), drawn every
    // frame: rebuilding tens of thousands of lines a frame took the viewer from 44 to 3 fps.
    static SKWorldOverlayGeom geom;
    static LLTimer since_build;
    static LLViewerRegion* built_in = nullptr;
    static bool built = false;
    if (built && since_build.getElapsedTimeF64() < OVERLAY_EVERY_S && built_in == gAgent.getRegion())
    {
        SKWorldOverlayGeom::bindShader();
        {
            LLGLEnable blend(GL_BLEND);
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            geom.draw();
        }
        SKWorldOverlayGeom::unbindShader();
        return;
    }
    built = true;
    built_in = gAgent.getRegion();
    since_build.reset();
    geom.clear();
    const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
    geom.setEdgeWidth(3.f, camera, LLViewerCamera::getInstance()->getView(), LLViewerCamera::getInstance()->getViewHeightInPixels());

    // Data-visualisation style by default: the road ahead as the driver reads it (its edges, its centre), the
    // line it drives (amber), junctions and surface changes as small marks. The pieces, segments, joins and
    // cross-sections only with SKRoadAutopilotOverlayDetail.
    static LLCachedControl<bool> detail(gSavedSettings, "SKRoadAutopilotOverlayDetail", false);
    // The road's surface under a point whose height is known only roughly (the car's centre, a lane point's z): the
    // highest road top within 3 m below `hint`, else `hint` less a car's ride height. Every overlay line is laid at
    // this plus `lift`, so none sinks into the road where the hint sat low or stale.
    auto surface = [](F64 x, F64 y, F64 hint)
    {
        const F64 floor_z = hint - 3.0;
        const F64 top = topAt(x, y, floor_z, 3.0);
        return top > floor_z ? top : hint - 0.5;
    };
    {
        // the trail: where the car has driven this session, grey, kept behind it as history
        static const LLColor4 GREY(0.6f, 0.6f, 0.6f, 0.8f);
        for (size_t i = 0; i + 1 < sTrail.size(); ++i)
        {
            const TrailPoint& a = sTrail[i];
            const TrailPoint& b = sTrail[i + 1];
            if (std::hypot(b.x - a.x, b.y - a.y) > TRAIL_STEP_M * 4)
            {
                continue;       // a gap (backing off, turning round): not joined across
            }
            geom.line(at(a.x, a.y, surface(a.x, a.y, a.z) + lift), at(b.x, b.y, surface(b.x, b.y, b.z) + lift), GREY);
        }
        if (!sTrail.empty() && sOn)
        {
            const TrailPoint& a = sTrail.back();
            const F64 cx = sView.car.mdV[VX], cy = sView.car.mdV[VY];
            geom.line(at(a.x, a.y, surface(a.x, a.y, a.z) + lift), at(cx, cy, surface(cx, cy, zc) + lift), GREY);
        }
    }
    if (const skroute::Route* route = skroute::follow::route())
    {
        // the committed route, thin cyan, within the overlay's reach of the car
        static const LLColor4 CYAN(0.f, 0.85f, 0.95f, 0.9f);
        constexpr F64 ROUTE_DRAW_M = 600.0;
        for (size_t i = 0; i + 1 < route->line.size(); ++i)
        {
            const skroute::Point& a = route->line[i];
            const skroute::Point& b = route->line[i + 1];
            if (std::hypot(a.x - sView.car.mdV[VX], a.y - sView.car.mdV[VY]) > ROUTE_DRAW_M)
            {
                continue;
            }
            geom.line(at(a.x, a.y, a.z + lift + 0.5), at(b.x, b.y, b.z + lift + 0.5), CYAN);
        }
    }
    if (!detail)
    {
        // The clean view, a legend of four meanings: recognised road neutral (edges solid, centre dashed) over its
        // whole recognised length in reach, not only where the lane walk went; the planned line amber, full
        // length; junctions as one translucent area with a short arrow per branch (the chosen one amber); the
        // road's end a red bar across it.
        static const LLColor4 ROAD(0.85f, 0.9f, 1.f, 0.8f);
        static const LLColor4 ROAD_CENTRE(0.85f, 0.9f, 1.f, 0.4f);
        static const LLColor4 JUNCTION_FILL(0.85f, 0.9f, 1.f, 0.14f);
        static const LLColor4 JUNCTION_EDGE(0.85f, 0.9f, 1.f, 0.6f);
        constexpr F64 DASH_M = 1.5;
        const F64 reach = shapeReach();
        auto in_reach = [&](F64 x, F64 y)
        {
            return std::fabs(x - sView.car.mdV[VX]) <= reach && std::fabs(y - sView.car.mdV[VY]) <= reach;
        };
        // a straight stretch of road from its centre (x, y), axis (ux, uy), length and width; z from `z_of`
        auto strip = [&](F64 x, F64 y, F64 ux, F64 uy, F64 len, F64 width, const std::function<F64(F64, F64)>& z_of)
        {
            const F64 hx = -uy * width / 2, hy = ux * width / 2;
            const F64 x0 = x - ux * len / 2, y0 = y - uy * len / 2;
            // each edge in steps of at most 2 m, every vertex at the surface there, so it follows slope and crown
            const S32 steps = std::max(1, S32(std::ceil(len / 2.0)));
            for (S32 i = 0; i < steps; ++i)
            {
                const F64 a = len * i / steps, b = len * (i + 1) / steps;
                for (const F64 side : { 1.0, -1.0 })
                {
                    const F64 ax = x0 + ux * a + side * hx, ay = y0 + uy * a + side * hy;
                    const F64 bx = x0 + ux * b + side * hx, by = y0 + uy * b + side * hy;
                    geom.line(at(ax, ay, z_of(ax, ay) + lift), at(bx, by, z_of(bx, by) + lift), ROAD);
                }
            }
            // dashes laid from a world-fixed phase, so pieces end to end read as one dashed line
            const F64 s0 = -len / 2, s1 = len / 2;
            const F64 phase = ux * x + uy * y;
            for (F64 s = std::floor((phase + s0) / (2 * DASH_M)) * 2 * DASH_M - phase; s < s1; s += 2 * DASH_M)
            {
                const F64 a = std::max(s, s0), b = std::min(s + DASH_M, s1);
                if (b > a)
                {
                    geom.line(at(x + ux * a, y + uy * a, z_of(x + ux * a, y + uy * a) + lift),
                              at(x + ux * b, y + uy * b, z_of(x + ux * b, y + uy * b) + lift), ROAD_CENTRE);
                }
            }
        };
        // the model's segments in reach, each along its own axis and at its own height; `skip`: roots not drawn
        // A road built of two linksets side by side, a half each, is one road to the model (siblings: its sections
        // see both halves, the walk's edges are the outer deck's and its centre the seam), so it is drawn as one:
        // the halves together, one pair of edges and one centre, from the lowest-numbered half; the car's road's
        // halves are the car's road. Each segment's fate is logged once a second (road_set).
        static F64 s_seg_log_at = -1e9;
        const F64 now_s = LLTimer::getTotalSeconds().value();
        const bool seg_log = now_s - s_seg_log_at > 1.0;
        if (seg_log)
        {
            s_seg_log_at = now_s;
        }
        auto drawSegments = [&](const std::unordered_set<LLUUID>* skip)
        {
            constexpr F64 MIN_ROAD_W = 3.0;
            std::ostringstream lg;
            S32 drawn = 0, skipped = 0;
            auto note = [&](S32 i, const Segment& sg, const char* what)
            {
                if (seg_log && in_reach(sg.cx, sg.cy))
                {
                    lg << " [" << i << " " << sg.root.asString().substr(0, 8) << " w" << llformat("%.1f", sg.width) << ": " << what << "]";
                }
            };
            auto unfit = [&](const Segment& sg) -> const char*
            {
                if (sg.square) return "square";
                if (sg.pending) return "pending";
                if (!sg.definite && !sg.matches) return "not road";
                return nullptr;
            };
            for (S32 i = 0; i < S32(sSegments.size()); ++i)
            {
                const Segment& sg = sSegments[i];
                if (!in_reach(sg.cx, sg.cy))
                {
                    continue;
                }
                if (const char* why = unfit(sg))
                {
                    note(i, sg, why);
                    ++skipped;
                    continue;
                }
                const std::vector<S32>& sib = siblings(i);
                bool car_road = skip && skip->count(sg.root), first = true;
                for (S32 j : sib)
                {
                    car_road = car_road || (skip && skip->count(sSegments[j].root));
                    first = first && (j > i || unfit(sSegments[j]));
                }
                if (car_road)
                {
                    note(i, sg, sib.empty() ? "the car's road" : "the car's road (a half)");
                    ++skipped;
                    continue;
                }
                if (!first)
                {
                    note(i, sg, "drawn with its other half");
                    ++skipped;
                    continue;
                }
                // the halves' extent across and along this half's axis
                F64 a_lo = -sg.width / 2, a_hi = sg.width / 2, s_lo = sg.lo, s_hi = sg.hi;
                for (S32 j : sib)
                {
                    const Segment& b = sSegments[j];
                    if (unfit(b))
                    {
                        continue;
                    }
                    const F64 across = -(b.cx - sg.cx) * sg.ay + (b.cy - sg.cy) * sg.ax;
                    const F64 along = (b.cx - sg.cx) * sg.ax + (b.cy - sg.cy) * sg.ay;
                    const F64 bm = (b.lo + b.hi) / 2 * (b.ax * sg.ax + b.ay * sg.ay), bh = (b.hi - b.lo) / 2;
                    a_lo = std::min(a_lo, across - b.width / 2);
                    a_hi = std::max(a_hi, across + b.width / 2);
                    s_lo = std::min(s_lo, along + bm - bh);
                    s_hi = std::max(s_hi, along + bm + bh);
                }
                const F64 width = a_hi - a_lo;
                if (width < MIN_ROAD_W)
                {
                    note(i, sg, "narrower than a lane");
                    ++skipped;
                    continue;
                }
                const F64 mid = (s_lo + s_hi) / 2, off = (a_lo + a_hi) / 2;
                strip(sg.cx + mid * sg.ax - sg.ay * off, sg.cy + mid * sg.ay + sg.ax * off, sg.ax, sg.ay, s_hi - s_lo, width,
                      [&sg](F64 x, F64 y) { return zAt(sg, x, y); });
                note(i, sg, sib.empty() ? "drawn" : "drawn with its halves");
                ++drawn;
            }
            if (seg_log)
            {
                LL_INFOS("SKRoadOverlay") << "road_set: " << drawn << " drawn, " << skipped << " skipped:" << lg.str() << LL_ENDL;
            }
        };
        if (sView.source == "boxes")
        {
            // The car's own road: its edges and centre from the walk's sections (each lane point's edges, the
            // road's direction there and its height), so they run along the road as the walk read it, up its slope.
            const std::vector<LanePoint>& lp = sView.path;
            // the model's own height at the section (its z), not a probe of the surface: where the model's slope is wrong, the
            // line visibly leaves the road
            auto lp_z = [&](F64, F64, const LanePoint& p) { return p.z != 0 ? p.z : zc - 0.5; };
            for (size_t i = 0; i + 1 < lp.size(); ++i)
            {
                const LanePoint& p = lp[i];
                const LanePoint& q = lp[i + 1];
                if (p.road_w >= OPEN_W || q.road_w >= OPEN_W || std::hypot(q.x - p.x, q.y - p.y) > 8.0)
                {
                    continue;       // open ground or a gap (a jump): no edges to draw
                }
                geom.line(at(p.left_x, p.left_y, lp_z(p.left_x, p.left_y, p) + lift),
                          at(q.left_x, q.left_y, lp_z(q.left_x, q.left_y, q) + lift), ROAD);
                geom.line(at(p.right_x, p.right_y, lp_z(p.right_x, p.right_y, p) + lift),
                          at(q.right_x, q.right_y, lp_z(q.right_x, q.right_y, q) + lift), ROAD);
                const F64 mpx = (p.left_x + p.right_x) / 2, mpy = (p.left_y + p.right_y) / 2;
                const F64 mqx = (q.left_x + q.right_x) / 2, mqy = (q.left_y + q.right_y) / 2;
                if (i % 2 == 0)
                {
                    geom.line(at(mpx, mpy, lp_z(mpx, mpy, p) + lift), at(mqx, mqy, lp_z(mqx, mqy, q) + lift), ROAD_CENTRE);
                }
            }
            // Other roads in reach: one strip per segment (a road linkset, as the model chains them), never a strip per
            // part, whose own frame on a part about as long as wide lies across the road and whose pavements and side
            // strips each drew edges of their own. Skipped: the car's own road (the walk's, above), squarish segments
            // (the junction area stands for them), and what the model does not take for road surface: neither
            // definite road nor in the driving surface's textures, or narrower than a lane.
            // The car's road by the chain's roots, not sWalkParts: those are indices into sParts, which the census
            // clears and renumbers between walks, so a rebuild between the two read the car's road as other roads
            // (or another road as the car's) and its edges came and went.
            const std::unordered_set<LLUUID> walked(sChainRoots.begin(), sChainRoots.end());
            // the car's road's halves are skipped with it (drawSegments): its edges above are the whole deck's
            drawSegments(lp.empty() ? nullptr : &walked);
        }
        else
        {
            drawSegments(nullptr);
        }
        // The planned lane line: amber from where the car is along it, its whole length ahead; behind the car, grey,
        // the road the car drove. Both are the model's stored road (StoredRoad, roadForView), which keeps what it
        // walked: what lies behind the car does not move from refresh to refresh.
        static const LLColor4 LINE_PAST(0.6f, 0.6f, 0.6f, 0.7f);
        const std::vector<LanePoint>& path = sView.path;
        const F64 cx = sView.car.mdV[VX], cy = sView.car.mdV[VY];
        const F64 fx = std::cos(sView.yaw), fy = std::sin(sView.yaw);
        auto lane_z = [&](const LanePoint& p) { return surface(p.x, p.y, (p.z != 0 ? p.z : zc - 0.5) + 1.0) + lift + 0.05; };
        size_t first_ahead = 0;
        while (first_ahead < path.size() && (path[first_ahead].x - cx) * fx + (path[first_ahead].y - cy) * fy < 0)
        {
            ++first_ahead;
        }
        for (size_t i = 0; i + 1 < first_ahead; ++i)
        {
            const LanePoint& a = path[i];
            const LanePoint& b = path[i + 1];
            if (std::hypot(b.x - a.x, b.y - a.y) < 8.0)
            {
                geom.line(at(a.x, a.y, lane_z(a)), at(b.x, b.y, lane_z(b)), LINE_PAST);
            }
        }
        // ahead: starts at the car's own place on the line, so the cut moves with the car, smoothly
        if (first_ahead < path.size())
        {
            const LanePoint& p = path[first_ahead];
            const F64 s = (p.x - cx) * p.ax + (p.y - cy) * p.ay;
            const F64 sx = p.x - p.ax * s, sy = p.y - p.ay * s;
            geom.edge(at(sx, sy, lane_z(p)), at(p.x, p.y, lane_z(p)), AMBER);
            if (first_ahead > 0)
            {
                const LanePoint& b = path[first_ahead - 1];
                geom.line(at(b.x, b.y, lane_z(b)), at(sx, sy, lane_z(p)), LINE_PAST);
            }
        }
        for (size_t i = first_ahead; i + 1 < path.size(); ++i)
        {
            const LanePoint& p = path[i];
            const LanePoint& q = path[i + 1];
            geom.edge(at(p.x, p.y, lane_z(p)), at(q.x, q.y, lane_z(q)), AMBER);
        }
        // junctions: the area between the way in and the branches' mouths, and an arrow out along each branch
        for (const Junction& jn : sView.junctions)
        {
            const F64 z = surface(jn.x, jn.y, zc) + lift + 0.1;
            std::vector<std::pair<F64, F64>> pts{ { jn.x, jn.y } };
            for (const Branch& br : jn.branches)
            {
                pts.emplace_back(br.x, br.y);
            }
            F64 mx = 0, my = 0;
            for (const auto& p : pts)
            {
                mx += p.first / pts.size();
                my += p.second / pts.size();
            }
            std::sort(pts.begin(), pts.end(), [mx, my](const auto& a, const auto& b)
                      { return std::atan2(a.second - my, a.first - mx) < std::atan2(b.second - my, b.first - mx); });
            F64 area = 0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % pts.size()];
                area += (a.first * b.second - b.first * a.second) / 2;
            }
            if (std::fabs(area) < 6.0)
            {
                // the mouths lie too close together to span an area: a disc stands for it
                pts.clear();
                for (S32 i = 0; i < 16; ++i)
                {
                    pts.emplace_back(mx + 3 * std::cos(2 * PI * i / 16), my + 3 * std::sin(2 * PI * i / 16));
                }
            }
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % pts.size()];
                geom.tri(at(mx, my, z), at(a.first, a.second, z), at(b.first, b.second, z), JUNCTION_FILL);
                geom.line(at(a.first, a.second, z), at(b.first, b.second, z), JUNCTION_EDGE);
            }
            for (size_t b = 0; b < jn.branches.size(); ++b)
            {
                const Branch& br = jn.branches[b];
                const bool chosen = S32(b) == jn.chosen;
                const LLColor4& colour = chosen ? AMBER : JUNCTION_EDGE;
                const F64 tx = br.x + br.dx * 5, ty = br.y + br.dy * 5;
                const F64 px = -br.dy, py = br.dx;
                auto draw = [&](const LLVector3& a, const LLVector3& c) { chosen ? geom.edge(a, c, colour) : geom.line(a, c, colour); };
                draw(at(br.x, br.y, z), at(tx, ty, z));
                draw(at(tx, ty, z), at(tx - br.dx * 1.2 + px * 0.8, ty - br.dy * 1.2 + py * 0.8, z));
                draw(at(tx, ty, z), at(tx - br.dx * 1.2 - px * 0.8, ty - br.dy * 1.2 - py * 0.8, z));
            }
        }
        // the road's end: a bar across it where the line stops
        if (sView.road_end && !sView.jump)
        {
            const F64 w = !path.empty() && path.back().road_w < OPEN_W ? path.back().road_w : 6.0;
            const F64 z = surface(sView.road_end_x, sView.road_end_y, (!path.empty() && path.back().z != 0 ? path.back().z + 1.0 : zc)) + lift + 0.1;
            const F64 rx = sView.road_end_dy * w / 2, ry = -sView.road_end_dx * w / 2;
            geom.edge(at(sView.road_end_x - rx, sView.road_end_y - ry, z), at(sView.road_end_x + rx, sView.road_end_y + ry, z), RED);
        }
        geom.upload();
        SKWorldOverlayGeom::bindShader();
        {
            LLGLEnable blend(GL_BLEND);
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            geom.draw();
        }
        SKWorldOverlayGeom::unbindShader();
        return;
    }

    // the road's faces near the car, each once (a part's several census records carry the same faces)
    std::unordered_set<U64> drawn_faces;
    for (const Shape* s : sShapes)
    {
        if (std::fabs(s->x[0] - sView.car.mdV[VX]) > OVERLAY_FACES_M || std::fabs(s->y[0] - sView.car.mdV[VY]) > OVERLAY_FACES_M)
        {
            continue;
        }
        U64 fk = 1469598103934665603ULL;
        for (S32 k = 0; k < s->n; ++k)
        {
            fk = (fk ^ U64(std::llround(s->x[k] * 50) * 31 + std::llround(s->y[k] * 50) * 17 + std::llround(s->zv[k] * 20))) * 1099511628211ULL;
        }
        if (!drawn_faces.insert(fk).second)
        {
            continue;
        }
        for (S32 k = 0; k < s->n; ++k)
        {
            const S32 k1 = (k + 1) % s->n;
            geom.line(at(s->x[k], s->y[k], s->zv[k] + 0.1), at(s->x[k1], s->y[k1], s->zv[k1] + 0.1), FAINT);
        }
    }
    static const LLColor4 CYAN(0.f, 0.9f, 1.f, 0.8f);         // a road segment's centre line, end to end
    static const LLColor4 GREEN(0.2f, 1.f, 0.3f, 1.f);
    const bool by_parts = sView.source == "boxes";
    if (by_parts)
    {
        // each part's top (white: walked), and where parts meet (green, along what they share)
        std::vector<bool> walked(sParts.size(), false);
        for (S32 k : sWalkParts)
        {
            if (k < S32(walked.size()))
            {
                walked[k] = true;
            }
        }
        for (S32 k = 0; k < S32(sParts.size()); ++k)
        {
            const PieceShapes& p = *sParts[k];
            if (std::fabs(p.cx - sView.car.mdV[VX]) > shapeReach() || std::fabs(p.cy - sView.car.mdV[VY]) > shapeReach())
            {
                continue;
            }
            for (const TopEdge& e : p.top)
            {
                geom.line(at(e.x0, e.y0, e.z0 + lift), at(e.x1, e.y1, e.z1 + lift), walked[k] ? LLColor4::white : CYAN);
            }
            for (const Continuation& c : sConts[k])
            {
                if (c.other > k)
                {
                    geom.edge(at(c.ax, c.ay, c.z + lift), at(c.bx, c.by, c.z + lift), GREEN);
                }
            }
        }
    }
    for (const Segment& sg : sSegments)
    {
        if (by_parts)
        {
            break;
        }
        if (std::fabs(sg.cx - sView.car.mdV[VX]) > shapeReach() || std::fabs(sg.cy - sView.car.mdV[VY]) > shapeReach())
        {
            continue;
        }
        const F64 z = sg.zc + lift;
        if (sg.square)
        {
            // no direction: a cross marks it
            geom.line(at(sg.cx - 1, sg.cy, z), at(sg.cx + 1, sg.cy, z), CYAN);
            geom.line(at(sg.cx, sg.cy - 1, z), at(sg.cx, sg.cy + 1, z), CYAN);
            continue;
        }
        const F64 x0 = sg.cx + sg.lo * sg.ax, y0 = sg.cy + sg.lo * sg.ay, x1 = sg.cx + sg.hi * sg.ax, y1 = sg.cy + sg.hi * sg.ay;
        const bool chained = std::find(sChainRoots.begin(), sChainRoots.end(), sg.root) != sChainRoots.end();
        const F64 z0 = zAt(sg, x0, y0) + lift, z1 = zAt(sg, x1, y1) + lift;     // along its slope
        geom.line(at(x0, y0, z0), at(x1, y1, z1), chained ? LLColor4::white : CYAN);   // white: on the chain ahead
        if (sg.boxed)
        {
            // its box's outline on the road's top: how road pieces meet, end face to end face
            const F64 hx = -sg.ay * sg.width / 2, hy = sg.ax * sg.width / 2;
            const F64 c[4][2] = { { x0 - hx, y0 - hy }, { x1 - hx, y1 - hy }, { x1 + hx, y1 + hy }, { x0 + hx, y0 + hy } };
            for (S32 k = 0; k < 4; ++k)
            {
                const S32 k1 = (k + 1) % 4;
                geom.line(at(c[k][0], c[k][1], zAt(sg, c[k][0], c[k][1]) + lift),
                          at(c[k1][0], c[k1][1], zAt(sg, c[k1][0], c[k1][1]) + lift), CYAN);
            }
        }
        const F64 nx = -sg.ay * 0.5, ny = sg.ax * 0.5;     // short ticks at its ends
        geom.line(at(x0 - nx, y0 - ny, z0), at(x0 + nx, y0 + ny, z0), CYAN);
        geom.line(at(x1 - nx, y1 - ny, z1), at(x1 + nx, y1 + ny, z1), CYAN);
    }
    // where road pieces meet, top edge on top edge: green along what they share
    for (S32 i = 0; i < S32(sJoins.size()) && !by_parts; ++i)
    {
        for (const Join& j : sJoins[i])
        {
            if (j.other < i || std::fabs(j.x - sView.car.mdV[VX]) > shapeReach() || std::fabs(j.y - sView.car.mdV[VY]) > shapeReach())
            {
                continue;
            }
            geom.edge(at(j.ax, j.ay, j.z + lift), at(j.bx, j.by, j.z + lift), GREEN);
        }
    }
    // where the road's surface ends: a bar across it, magenta where the same road goes on re-surfaced, red
    // where another road starts
    static const LLColor4 MAGENTA(1.f, 0.f, 1.f, 1.f);
    for (const auto& c : sView.surface_changes)
    {
        const F64 rx = c.dy * c.w / 2, ry = -c.dx * c.w / 2;
        const F64 zt = topAt(c.x, c.y, c.z, 1.5);
        geom.edge(at(c.x - rx, c.y - ry, zt + lift), at(c.x + rx, c.y + ry, zt + lift), c.same_road ? MAGENTA : RED);
    }
    // junctions ahead: a ring, each branch an arrow, the one taken white
    static const LLColor4 WHITE(1.f, 1.f, 1.f, 1.f);
    for (const Junction& jn : sView.junctions)
    {
        const F64 z = zc + lift + 0.3;
        for (size_t b = 0; b < jn.branches.size(); ++b)
        {
            const Branch& br = jn.branches[b];
            geom.edge(at(jn.x, jn.y, z), at(br.x + br.dx * 6, br.y + br.dy * 6, z), S32(b) == jn.chosen ? WHITE : RED);
        }
        const S32 N = 12;
        for (S32 i = 0; i < N; ++i)
        {
            const F64 a0 = 2 * PI * i / N, a1 = 2 * PI * (i + 1) / N;
            geom.line(at(jn.x + 1.5 * std::cos(a0), jn.y + 1.5 * std::sin(a0), z),
                      at(jn.x + 1.5 * std::cos(a1), jn.y + 1.5 * std::sin(a1), z), WHITE);
        }
    }
    const std::vector<LanePoint>& path = sView.path;
    for (size_t i = 0; i < path.size(); ++i)
    {
        const LanePoint& p = path[i];
        const F64 z = (p.z != 0 ? p.z : zc) + lift;
        if (i % 2 == 0 && p.road_w < OPEN_W)
        {
            geom.line(at(p.left_x, p.left_y, z), at(p.right_x, p.right_y, z), BLUE);
        }
        if (i + 1 < path.size())
        {
            const LanePoint& q = path[i + 1];
            // amber: drawn through the chained linksets; pink: the fallback walk through everything drivable
            geom.edge(at(p.x, p.y, z), at(q.x, q.y, (q.z != 0 ? q.z : zc) + lift), sView.chained ? AMBER : PINK);
        }
    }
    for (size_t i = 0; i + 1 < sView.rollout.size(); ++i)
    {
        geom.line(at(sView.rollout[i].first, sView.rollout[i].second, zc + lift),
                  at(sView.rollout[i + 1].first, sView.rollout[i + 1].second, zc + lift), VIOLET);
    }
    const F64 fx = std::cos(sView.yaw), fy = std::sin(sView.yaw);
    if (sView.tapping)
    {
        const F64 cx = sView.car.mdV[VX] - fy * sView.tapping * 2.0, cy = sView.car.mdV[VY] + fx * sView.tapping * 2.0;
        geom.edge(at(cx - fx * 1.5, cy - fy * 1.5, zc + lift), at(cx + fx * 1.5, cy + fy * 1.5, zc + lift), RED);
    }
    if (inCrossing())
    {
        // a region crossing: a cyan ring, held through
        static const LLColor4 CYAN(0.2f, 0.9f, 1.f, 1.f);
        const S32 N = 24;
        for (S32 i = 0; i < N; ++i)
        {
            const F64 a0 = 2 * PI * i / N, a1 = 2 * PI * (i + 1) / N;
            geom.line(at(sView.car.mdV[VX] + 5 * std::cos(a0), sView.car.mdV[VY] + 5 * std::sin(a0), zc + lift),
                      at(sView.car.mdV[VX] + 5 * std::cos(a1), sView.car.mdV[VY] + 5 * std::sin(a1), zc + lift), CYAN);
        }
    }
    if (sView.waiting)
    {
        const S32 N = 24;
        for (S32 i = 0; i < N; ++i)
        {
            const F64 a0 = 2 * PI * i / N, a1 = 2 * PI * (i + 1) / N;
            geom.line(at(sView.car.mdV[VX] + 4 * std::cos(a0), sView.car.mdV[VY] + 4 * std::sin(a0), zc + lift),
                      at(sView.car.mdV[VX] + 4 * std::cos(a1), sView.car.mdV[VY] + 4 * std::sin(a1), zc + lift), RED);
        }
    }

    geom.upload();
    SKWorldOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        geom.draw();
    }
    SKWorldOverlayGeom::unbindShader();
}
