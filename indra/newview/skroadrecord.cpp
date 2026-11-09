/**
 * @file skroadrecord.cpp
 * @brief What the road driver writes down while the survey runs: its view, and the line's stalls and blips.
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

namespace skroad::drive
{
    // ---------------------------------------------------------------- the view, written down

    // While the road survey runs, what the driver sees once a second, to sk-road-view.jsonl beside the survey:
    // the car, the chain's linksets, the lane line, the surface changes and the junctions with their choice.
    // One object a line, appended:
    //   {"type":"view","t","car":[x,y,z],"yaw","chained","route":n,"chain":["root",...],
    //    "path":[[x,y,z],...],"changes":[[x,y,same_road],...],"junctions":[{"x","y","chosen","branches":
    //    [{"root","turn","kind"},...]},...]}
    llofstream sViewLog;
    LLTimer sSinceView;
    void writeView(F64 now, const LLVector3d& car, F64 yaw);

    // ---------------------------------------------------------------- the line's stalls, written down

    // Driving on, the lane line's far end moves on a piece at a time. Where it stops while the car keeps going and
    // the line shortens, or the line bends hard for a moment and straightens again, the walk met something it could
    // not read (a false piece, a gap, a seam): a note in the CSV ("line stalled", "line blip"), and a record in
    // sk-road-linestall.jsonl with the car's last poses and lines and every part in reach as the survey writes them
    // (the road bench's cut.py --linestall makes it a case). One object a line:
    //   {"type":"linestall","event":"stalled"|"blip","t","unix","car":[x,y,z],"yaw","speed","path":[[x,y,z,road_w,
    //    lanes],...],"bent_path":[...],"walk_end","source","chain":[...],"walk":[part,...],"poses":[[t,x,y,z,yaw],...],
    //    "lines":[[[x,y,z,road_w,lanes],...],...],"records":[survey part records],"props":{root:{name,owner,group}},
    //    "top_tex":{part:tex},"textures":{part:[tex,...]}}
    constexpr F64 STALL_S = 1.0;                // the far end not moved on for this long
    constexpr F64 STALL_MIN_V = 3.0;            // while the car goes faster than this
    constexpr F64 STALL_SHORT = 0.7;            // and the line is shorter than this of its look-ahead
    constexpr F64 STALL_ADVANCE_M = 1.0;        // moving on: the far end this much further along the car's way
    constexpr F64 BLIP_DEG = 60.0;              // a bend this sharp in the line
    constexpr F64 BLIP_CLEAR_DEG = 30.0;        // gone again (no bend past this)
    constexpr F64 BLIP_S = 1.0;                 // within this long
    constexpr F64 BLIP_CHORD_M = 3.0;           // the line's bends measured between chords this long
    constexpr F64 BLIP_SKIP_M = 5.0;            // past its start (the first point lies abreast of the car)
    constexpr F64 LINE_EVENT_GAP_S = 10.0;      // one record at most this often
    constexpr F64 LINE_EVENT_REACH_M = 80.0;    // the parts written: their boxes this near the car
    constexpr F64 LINE_EVENT_Z_M = 30.0;
    constexpr size_t LINE_EVENT_MAX_TRIS = 3000;
    constexpr F64 LINE_POSE_EVERY_S = 0.5;      // the car's poses and lines kept for the record
    constexpr F64 LINE_POSES_S = 10.0;
    struct LinePose
    {
        F64 t, x, y, z, yaw;
        std::vector<LanePoint> path;
    };
    struct LineWatch
    {
        bool have = false;
        F64 last_now = 0;
        F64 fx = 0, fy = 0;                     // the far end where it last moved on
        F64 advanced = 0;
        bool stalled = false;
        F64 bent_at = -1;
        std::vector<LanePoint> bent_path;
        F64 last_event = -1e9;                  // unix
        std::deque<LinePose> poses;
    };
    LineWatch sLineWatch;
    llofstream sLineLog;
    void watchLine(F64 now, const LLVector3d& pos, F64 yaw, F64 speed, std::string& note);
}

namespace skroad::drive
{
    void writeView(F64 now, const LLVector3d& car, F64 yaw)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - writeView");
        static LLCachedControl<bool> survey(gSavedSettings, "SKRoadSurvey", false);
        if (!survey)
        {
            if (sViewLog.is_open())
            {
                sViewLog.close();
            }
            return;
        }
        if (sSinceView.getElapsedTimeF64() < 1.0)
        {
            return;
        }
        sSinceView.reset();
        if (!sViewLog.is_open())
        {
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-view.jsonl");
            sViewLog.open(path.c_str(), std::ios::out | std::ios::app);
            sViewLog.precision(10);
        }
        if (!sViewLog.is_open())
        {
            return;
        }
        sViewLog << "{\"type\":\"view\",\"t\":" << now << ",\"unix\":" << LLTimer::getTotalSeconds().value() << ",\"car\":["
                 << car.mdV[VX] << ',' << car.mdV[VY] << ',' << car.mdV[VZ] << "],\"yaw\":" << yaw * 180 / PI
                 << ",\"chained\":" << (sView.chained ? "true" : "false") << ",\"route\":" << sRoute.size() << ",\"chain\":[";
        for (size_t i = 0; i < sChainRoots.size(); ++i)
        {
            sViewLog << (i ? "," : "") << '"' << sChainRoots[i] << '"';
        }
        sViewLog << "],\"path\":[";
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            sViewLog << (i ? "," : "") << '[' << sPath[i].x << ',' << sPath[i].y << ',' << sPath[i].z << ',' << sPath[i].road_w << ','
                     << sPath[i].lanes << ']';
        }
        sViewLog << "],\"walk_end\":\"" << sView.walk_end << "\",\"width_fallbacks\":" << sView.width_fallbacks << ",\"source\":\"" << sView.source << "\",\"chain_line\":[";
        for (size_t i = 0; i < sView.chain_line.size(); ++i)
        {
            sViewLog << (i ? "," : "") << '[' << sView.chain_line[i].x << ',' << sView.chain_line[i].y << ']';
        }
        sViewLog << "],\"changes\":[";
        for (size_t i = 0; i < sView.surface_changes.size(); ++i)
        {
            const auto& c = sView.surface_changes[i];
            sViewLog << (i ? "," : "") << '[' << c.x << ',' << c.y << ',' << (c.same_road ? 1 : 0) << ']';
        }
        sViewLog << "],\"junctions\":[";
        for (size_t i = 0; i < sView.junctions.size(); ++i)
        {
            const Junction& j = sView.junctions[i];
            sViewLog << (i ? "," : "") << "{\"x\":" << j.x << ",\"y\":" << j.y << ",\"chosen\":" << j.chosen << ",\"branches\":[";
            for (size_t b = 0; b < j.branches.size(); ++b)
            {
                const Branch& br = j.branches[b];
                sViewLog << (b ? "," : "") << "{\"root\":\"" << br.root << "\",\"turn\":" << br.turn_deg << ",\"kind\":\""
                         << br.kind() << "\"}";
            }
            sViewLog << "]}";
        }
        sViewLog << "]}\n";
        sViewLog.flush();
    }

    std::string jsonText(const std::string& s)
    {
        std::string out = "\"";
        for (const char c : s)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += c;
            }
            else if (U8(c) < 0x20)
            {
                out += llformat("\\u%04x", U32(U8(c)));
            }
            else
            {
                out += c;
            }
        }
        return out + "\"";
    }

    F64 lineLength(const std::vector<LanePoint>& path)
    {
        F64 L = 0;
        for (size_t i = 1; i < path.size(); ++i)
        {
            L += std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
        }
        return L;
    }

    // The sharpest bend in the line past its start, between chords BLIP_CHORD_M long along it.
    F64 sharpestBend(const std::vector<LanePoint>& path)
    {
        std::vector<std::pair<F64, F64>> at;        // the line at every BLIP_CHORD_M from BLIP_SKIP_M on
        F64 s = 0, next = BLIP_SKIP_M;
        for (size_t i = 1; i < path.size(); ++i)
        {
            const F64 L = std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
            while (L > 1e-9 && next <= s + L)
            {
                const F64 f = (next - s) / L;
                at.emplace_back(path[i - 1].x + f * (path[i].x - path[i - 1].x), path[i - 1].y + f * (path[i].y - path[i - 1].y));
                next += BLIP_CHORD_M;
            }
            s += L;
        }
        F64 sharpest = 0;
        for (size_t i = 2; i < at.size(); ++i)
        {
            const F64 a = std::atan2(at[i - 1].second - at[i - 2].second, at[i - 1].first - at[i - 2].first);
            const F64 b = std::atan2(at[i].second - at[i - 1].second, at[i].first - at[i - 1].first);
            sharpest = std::max(sharpest, std::fabs(wrap(b - a)) * 180 / PI);
        }
        return sharpest;
    }

    void writeLinePath(std::ostream& out, const std::vector<LanePoint>& path)
    {
        out << '[';
        for (size_t i = 0; i < path.size(); ++i)
        {
            out << (i ? "," : "") << '[' << path[i].x << ',' << path[i].y << ',' << path[i].z << ',' << path[i].road_w << ','
                << path[i].lanes << ']';
        }
        out << ']';
    }

    const char* recordShapeName(skworld::Shape shape)
    {
        switch (shape)
        {
            case skworld::Shape::BOX: return "box";
            case skworld::Shape::SPHERE: return "sphere";
            case skworld::Shape::CYLINDER: return "cylinder";
            case skworld::Shape::TRIANGLES: return "triangles";
        }
        return "?";
    }

    const char* recordProvenanceName(skworld::Provenance p)
    {
        switch (p)
        {
            case skworld::Provenance::EXACT: return "exact";
            case skworld::Provenance::HULL: return "hull";
            case skworld::Provenance::BOX: return "box";
            case skworld::Provenance::RENDER: return "render";
            case skworld::Provenance::BASE_HULL: return "base_hull";
            case skworld::Provenance::SIM: return "sim";
        }
        return "?";
    }

    // A census record as the survey writes a "part" line (skroadsurvey.cpp's writePart), less the land height.
    void writeLineRecord(std::ostream& out, const skworld::Record& r)
    {
        auto vec = [&](const LLVector3& v) { out << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']'; };
        auto vecd = [&](const LLVector3d& v) { out << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']'; };
        out << "{\"part\":\"" << r.mPart << "\",\"root\":\"" << r.mRoot << "\",\"shape\":\"" << recordShapeName(r.mShape)
            << "\",\"prov\":\"" << recordProvenanceName(r.mProvenance) << "\",\"collides\":" << (r.mCollides ? "true" : "false")
            << ",\"solid\":" << (r.mSolid ? "true" : "false") << ",\"physics_known\":" << (r.mPhysicsKnown ? "true" : "false")
            << ",\"origin\":";
        vecd(r.mOrigin);
        out << ",\"min\":";
        vecd(r.mMin);
        out << ",\"max\":";
        vecd(r.mMax);
        if (const LLViewerObject* o = gObjectList.findObject(r.mPart))
        {
            out << ",\"phys_type\":" << S32(o->getPhysicsShapeType());
            if (o->getPCode() == LL_PCODE_VOLUME)
            {
                const LLVOVolume* vol = static_cast<const LLVOVolume*>(o);
                if (const LLSculptParams* sp = vol->getSculptParams())
                {
                    out << (vol->isMesh() ? ",\"mesh\":\"" : ",\"sculpt\":\"") << sp->getSculptTexture() << '"';
                }
            }
        }
        if (r.mShape == skworld::Shape::TRIANGLES)
        {
            out << ",\"tris\":[";
            if (r.mTriangles)
            {
                const size_t n = std::min(r.mTriangles->size(), LINE_EVENT_MAX_TRIS * 3);
                for (size_t i = 0; i < n; ++i)
                {
                    const LLVector3& v = (*r.mTriangles)[i];
                    out << (i ? "," : "") << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ];
                }
            }
            out << ']';
        }
        else
        {
            out << ",\"center\":";
            vec(r.mCenter);
            out << ",\"axes\":[";
            for (S32 k = 0; k < 3; ++k)
            {
                out << (k ? "," : "");
                vec(r.mAxes[k]);
            }
            out << "],\"half\":";
            vec(r.mHalf);
        }
        out << '}';
    }

    void writeLineEvent(const char* event, F64 now, const LLVector3d& pos, F64 yaw, F64 speed, const std::vector<LanePoint>& bent)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - writeLineEvent");
        if (!sLineLog.is_open())
        {
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-linestall.jsonl");
            sLineLog.open(path.c_str(), std::ios::out | std::ios::app);
            sLineLog.precision(10);
        }
        if (!sLineLog.is_open())
        {
            return;
        }
        std::ostream& out = sLineLog;
        out << "{\"type\":\"linestall\",\"event\":\"" << event << "\",\"t\":" << now << ",\"unix\":" << LLTimer::getTotalSeconds().value()
            << ",\"car\":[" << pos.mdV[VX] << ',' << pos.mdV[VY] << ',' << pos.mdV[VZ] << "],\"yaw\":" << yaw * 180 / PI
            << ",\"speed\":" << speed << ",\"path\":";
        writeLinePath(out, sPath);
        out << ",\"bent_path\":";
        writeLinePath(out, bent);
        out << ",\"walk_end\":" << jsonText(sView.walk_end) << ",\"source\":" << jsonText(sView.source) << ",\"chain\":[";
        for (size_t i = 0; i < sChainRoots.size(); ++i)
        {
            out << (i ? "," : "") << '"' << sChainRoots[i] << '"';
        }
        out << "],\"walk\":[";
        for (size_t i = 0; i < sWalkParts.size(); ++i)
        {
            const S32 k = sWalkParts[i];
            if (k >= 0 && k < S32(sParts.size()))
            {
                out << (i ? "," : "") << '"' << sParts[k]->part << '"';
            }
        }
        out << "],\"poses\":[";
        for (size_t i = 0; i < sLineWatch.poses.size(); ++i)
        {
            const LinePose& p = sLineWatch.poses[i];
            out << (i ? "," : "") << '[' << p.t << ',' << p.x << ',' << p.y << ',' << p.z << ',' << p.yaw * 180 / PI << ']';
        }
        out << "],\"lines\":[";
        for (size_t i = 0; i < sLineWatch.poses.size(); ++i)
        {
            out << (i ? "," : "");
            writeLinePath(out, sLineWatch.poses[i].path);
        }
        // every part about the car, road or not: the candidates the model read are among them
        out << "],\"records\":[";
        std::unordered_set<LLUUID> roots;
        std::unordered_set<LLUUID> parts;
        if (CensusPtr snap = skroad::host::census())
        {
            const F64 r = LINE_EVENT_REACH_M;
            bool first = true;
            for (const skworld::RecordPtr& rec : snap->mRecords)
            {
                if (!rec || rec->mDynamic || rec->mMax.mdV[VX] < pos.mdV[VX] - r || rec->mMin.mdV[VX] > pos.mdV[VX] + r
                    || rec->mMax.mdV[VY] < pos.mdV[VY] - r || rec->mMin.mdV[VY] > pos.mdV[VY] + r
                    || rec->mMax.mdV[VZ] < pos.mdV[VZ] - LINE_EVENT_Z_M || rec->mMin.mdV[VZ] > pos.mdV[VZ] + LINE_EVENT_Z_M)
                {
                    continue;
                }
                out << (first ? "" : ",");
                first = false;
                writeLineRecord(out, *rec);
                roots.insert(rec->mRoot);
                parts.insert(rec->mPart);
            }
        }
        out << "],\"props\":{";
        bool first = true;
        for (const LLUUID& root : roots)
        {
            if (const ALObjectPropertiesCache::ServerProps* p = ALObjectPropertiesCache::instance().get(root))
            {
                out << (first ? "" : ",") << '"' << root << "\":{\"name\":" << jsonText(p->mName) << ",\"owner\":\"" << p->mOwnerId
                    << "\",\"group\":\"" << p->mGroupId << "\"}";
                first = false;
            }
        }
        out << "},\"top_tex\":{";
        first = true;
        for (const PieceShapes* p : sParts)
        {
            if (p && p->top_known && parts.count(p->part))
            {
                out << (first ? "" : ",") << '"' << p->part << "\":\"" << p->top_tex << '"';
                first = false;
            }
        }
        out << "},\"textures\":{";
        first = true;
        for (const PieceShapes* p : sParts)
        {
            if (p && p->textures_known && parts.count(p->part))
            {
                out << (first ? "" : ",") << '"' << p->part << "\":[";
                for (size_t i = 0; i < p->textures.size(); ++i)
                {
                    out << (i ? "," : "") << '"' << p->textures[i] << '"';
                }
                out << ']';
                first = false;
            }
        }
        out << "}}\n";
        sLineLog.flush();
    }

    void watchLine(F64 now, const LLVector3d& pos, F64 yaw, F64 speed, std::string& note)
    {
        LineWatch& w = sLineWatch;
        if (now < w.last_now || sPath.size() < 2)
        {
            // a new run, or no line: start over
            const F64 last_event = w.last_event;
            w = LineWatch();
            w.last_event = last_event;
            w.last_now = now;
            return;
        }
        w.last_now = now;
        if (w.poses.empty() || now - w.poses.back().t >= LINE_POSE_EVERY_S)
        {
            w.poses.push_back({ now, pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], yaw, sPath });
        }
        while (!w.poses.empty() && w.poses.front().t < now - LINE_POSES_S)
        {
            w.poses.pop_front();
        }
        const char* event = nullptr;
        // the far end: moving on along the car's way, or not
        const LanePoint& tip = sPath.back();
        const F64 hx = std::cos(yaw), hy = std::sin(yaw);
        if (!w.have || speed < STALL_MIN_V || (tip.x - w.fx) * hx + (tip.y - w.fy) * hy > STALL_ADVANCE_M)
        {
            w.have = true;
            w.fx = tip.x;
            w.fy = tip.y;
            w.advanced = now;
            w.stalled = false;
        }
        else if (!w.stalled && now - w.advanced > STALL_S && lineLength(sPath) < STALL_SHORT * lookAhead(speed)
                 && !sView.road_end)     // a line held at the road's real end has not stalled
        {
            w.stalled = true;
            event = "stalled";
            note += note.empty() ? "line stalled" : ", line stalled";
        }
        // a hard bend that comes and goes within a few frames
        const F64 bend = sharpestBend(sPath);
        if (bend > BLIP_DEG)
        {
            if (w.bent_at < 0)
            {
                w.bent_at = now;
                w.bent_path = sPath;
            }
        }
        else if (w.bent_at >= 0 && bend < BLIP_CLEAR_DEG)
        {
            if (now - w.bent_at <= BLIP_S && !event)
            {
                event = "blip";
                note += note.empty() ? "line blip" : ", line blip";
            }
            w.bent_at = -1;
        }
        const F64 unix_now = LLTimer::getTotalSeconds().value();
        if (event && unix_now - w.last_event >= LINE_EVENT_GAP_S)
        {
            w.last_event = unix_now;
            writeLineEvent(event, now, pos, yaw, speed, std::string(event) == "blip" ? w.bent_path : std::vector<LanePoint>());
            fireEvent(std::string(event) == "blip" ? "line_blip" : "line_stalled", LLSD().with("speed", speed), pos);
            if (std::string(event) == "blip")
            {
                w.bent_path.clear();
            }
        }
    }
}
