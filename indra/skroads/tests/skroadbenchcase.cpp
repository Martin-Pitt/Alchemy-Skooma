/**
 * @file skroadbenchcase.cpp
 * @brief Spike: a road bench case, read by the road model as the viewer reads the world.
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

#include "linden_common.h"

#include "skroadbenchcase.h"

#include "llsdjson.h"
#include "llsdutil.h"
#include "m3math.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <ostream>
#include <sstream>

namespace skroadbench
{
    namespace
    {
        const Case* sCase = nullptr;

        // A streamed case's world as it has come so far: the census scan the model reads, and when each linkset's
        // properties come (negative: never).
        struct StreamSnap
        {
            std::vector<skworld::RecordPtr> records, added;
            skroad::CensusView view{ 0, records, added, none(), none() };
            static const std::vector<skworld::RecordPtr>& none()
            {
                static const std::vector<skworld::RecordPtr> empty;
                return empty;
            }
        };
        struct StreamState
        {
            std::vector<U8> in;                         // by record: arrived
            std::vector<skworld::RecordPtr> arrived;
            std::shared_ptr<const skroad::CensusView> view;
            U64 stamp = 0;
            std::unordered_map<LLUUID, F64> props_at;
            std::unordered_set<LLUUID> props_told;
            std::function<void(const LLUUID&)> watch;
            F64 now = 0;
        } sStream;

        // A number in [0, 1) from a linkset and the case's seed, so a run streams the same way every time.
        F64 streamDraw(const LLUUID& id, U32 seed, U32 salt)
        {
            U64 h = std::hash<LLUUID>{}(id) ^ (U64(seed) * 0x9E3779B97F4A7C15ULL) ^ (U64(salt) << 32);
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 33;
            return F64(h >> 11) / F64(1ULL << 53);
        }

        F64 boxDistance(const skworld::Record& r, const LLVector3d& p)
        {
            F64 d2 = 0;
            for (S32 k = 0; k < 2; ++k)
            {
                const F64 v = p.mdV[k] < r.mMin.mdV[k] ? r.mMin.mdV[k] - p.mdV[k] : p.mdV[k] > r.mMax.mdV[k] ? p.mdV[k] - r.mMax.mdV[k] : 0.0;
                d2 += v * v;
            }
            return std::sqrt(d2);
        }

        // One step of the stream at case time `now` with the car at `car`: the nearest parts in radius arrive, as
        // many as the rate lets, and the properties due by now are told to the model.
        void streamStep(const Case& c, F64 now, const LLVector3d& car)
        {
            StreamState& s = sStream;
            s.now = now;
            std::vector<std::pair<F64, size_t>> due;
            for (size_t i = 0; i < c.records.size(); ++i)
            {
                if (!s.in[i] && c.records[i])
                {
                    const F64 d = boxDistance(*c.records[i], car);
                    if (d <= c.stream.radius)
                    {
                        due.push_back({ d, i });
                    }
                }
            }
            const size_t take = std::min(due.size(), size_t(std::max(1.0, c.stream.rate * c.step_s)));
            std::partial_sort(due.begin(), due.begin() + take, due.end());
            if (take > 0)
            {
                auto snap = std::make_shared<StreamSnap>();
                for (size_t k = 0; k < take; ++k)
                {
                    const skworld::RecordPtr& rec = c.records[due[k].second];
                    s.in[due[k].second] = 1;
                    s.arrived.push_back(rec);
                    snap->added.push_back(rec);
                    if (!s.props_at.count(rec->mRoot))
                    {
                        const bool never = streamDraw(rec->mRoot, c.stream.seed, 1) < c.stream.props_never;
                        const F64 delay = c.stream.props_lo + (c.stream.props_hi - c.stream.props_lo) * streamDraw(rec->mRoot, c.stream.seed, 2);
                        s.props_at[rec->mRoot] = never ? -1.0 : now + delay;
                    }
                }
                snap->records = s.arrived;
                snap->view.mStamp = ++s.stamp;
                s.view = std::shared_ptr<const skroad::CensusView>(snap, &snap->view);
            }
            for (const auto& [root, at] : s.props_at)
            {
                if (at >= 0 && at <= now && s.props_told.insert(root).second && s.watch)
                {
                    s.watch(root);
                }
            }
        }

        bool streamPropsKnown(const LLUUID& root)
        {
            auto it = sStream.props_at.find(root);
            return it != sStream.props_at.end() && it->second >= 0 && it->second <= sStream.now;
        }

        struct KitFrame                 // a part's frame as recorded, for its linkset's kit signature
        {
            LLUUID part;                // null: the part's frame is not recorded
            skroad::KitPrim prim;
            LLVector3d pos;
            LLQuaternion rot;
            LLSD vp;                    // a prim's volume params; undefined for a mesh
            LLVector3d origin;          // its record's region origin (a mesh's rebuilt faces are region-local)
        };

        LLVector3d vec3d(const LLSD& a)
        {
            return LLVector3d(a[0].asReal(), a[1].asReal(), a[2].asReal());
        }

        LLVector3 vec3(const LLSD& a)
        {
            return LLVector3(F32(a[0].asReal()), F32(a[1].asReal()), F32(a[2].asReal()));
        }

        skworld::Provenance provenance(const std::string& p)
        {
            if (p == "hull") return skworld::Provenance::HULL;
            if (p == "box") return skworld::Provenance::BOX;
            if (p == "render") return skworld::Provenance::RENDER;
            if (p == "base_hull") return skworld::Provenance::BASE_HULL;
            if (p == "sim") return skworld::Provenance::SIM;
            return skworld::Provenance::EXACT;
        }

        // What shaped the record: equal geometry, equal signature, as the census's.
        U64 signatureOf(const LLSD& r)
        {
            U64 h = skworld::SEED;
            const std::string text = LlsdToJson(r);
            for (unsigned char ch : text)
            {
                h = skworld::mix(h, ch);
            }
            return h;
        }

        void put(std::ostream& out, F64 v)
        {
            if (std::isfinite(v))
            {
                out << v;
            }
            else
            {
                out << "null";
            }
        }
    }

    bool readCase(const LLSD& sd, Case& out, std::string& why)
    {
        if (!sd.isMap() || !sd["records"].isArray() || !sd["poses"].isArray())
        {
            why = "no records or poses";
            return false;
        }
        out.name = sd["name"].asString();
        if (out.cache_dir.empty())
        {
            out.cache_dir = defaultCacheDir();
        }
        std::unordered_map<LLUUID, std::vector<KitFrame>> kit_frames;
        for (const LLSD& r : llsd::inArray(sd["records"]))
        {
            auto rec = std::make_shared<skworld::Record>();
            rec->mPart = r["part"].asUUID();
            rec->mRoot = r["root"].asUUID();
            rec->mCollides = !r.has("collides") || r["collides"].asBoolean();
            rec->mProvenance = provenance(r["prov"].asString());
            rec->mOrigin = vec3d(r["origin"]);
            rec->mMin = vec3d(r["min"]);
            rec->mMax = vec3d(r["max"]);
            const std::string shape = r["shape"].asString();
            if (shape == "box" || shape == "sphere" || shape == "cylinder")
            {
                rec->mShape = shape == "box" ? skworld::Shape::BOX : shape == "sphere" ? skworld::Shape::SPHERE : skworld::Shape::CYLINDER;
                rec->mCenter = vec3(r["center"]);
                for (S32 k = 0; k < 3; ++k)
                {
                    rec->mAxes[k] = vec3(r["axes"][k]);
                }
                rec->mHalf = vec3(r["half"]);
                if (r.has("vp"))
                {
                    // the census's shape stays analytic; the drawn faces are rebuilt for their UVs
                    auto rebuilt = std::make_shared<Rebuilt>();
                    if (rebuildPart(r, out.cache_dir, *rebuilt))
                    {
                        out.geometry[rec->mPart] = rebuilt;
                    }
                }
            }
            else if (shape == "triangles" && !r.has("tris"))
            {
                // a reference (a mesh asset or volume params): rebuilt from the mesh cache and LLVolume
                auto rebuilt = std::make_shared<Rebuilt>();
                if (!rebuildPart(r, out.cache_dir, *rebuilt) || rebuilt->physics.empty())
                {
                    out.unbuilt.push_back(rec->mPart.asString() + ": " + rebuilt->why);
                    if (rebuilt->faces.empty())
                    {
                        continue;
                    }
                }
                rec->mShape = skworld::Shape::TRIANGLES;
                rec->mProvenance = rebuilt->physics.empty() ? skworld::Provenance::RENDER : rebuilt->provenance;
                rec->mSolid = rebuilt->solid;
                rec->mConvexTris = rebuilt->convex_tris;
                auto tris = std::make_shared<std::vector<LLVector3>>(rebuilt->physics);
                if (tris->empty())
                {
                    // no physics in the cache: the drawn faces stand in, marked RENDER
                    for (const RebuiltFace& face : rebuilt->faces)
                    {
                        for (U32 i : face.indices)
                        {
                            tris->push_back(face.positions[i]);
                        }
                    }
                    tris->resize(tris->size() / 3 * 3);
                }
                rec->mTriangles = tris;
                out.geometry[rec->mPart] = rebuilt;
            }
            else if (shape == "triangles" && r["tris"].isArray())
            {
                rec->mShape = skworld::Shape::TRIANGLES;
                auto tris = std::make_shared<std::vector<LLVector3>>();
                const LLSD& t = r["tris"];
                for (S32 i = 0; i + 2 < t.size(); i += 3)
                {
                    tris->emplace_back(F32(t[i].asReal()), F32(t[i + 1].asReal()), F32(t[i + 2].asReal()));
                }
                tris->resize(tris->size() / 3 * 3);
                rec->mTriangles = tris;
            }
            else
            {
                continue;       // no geometry (recorded only for standing on Linden land)
            }
            rec->mSignature = signatureOf(r);
            if (rec->mShape == skworld::Shape::BOX && rec->mPart == rec->mRoot)
            {
                // the root's frame: a prim box's axes are its rotation's (x, y, z) * rot
                LLMatrix3 m;
                m.setRows(rec->mAxes[0], rec->mAxes[1], rec->mAxes[2]);
                LLQuaternion q(m);
                q.normalize();
                if ((LLVector3::x_axis * q - rec->mAxes[0]).magVec() < 1e-3f)
                {
                    out.root_rot[rec->mRoot] = q;
                }
            }
            if (r.has("pos") && r.has("rot") && r.has("scale") && (r.has("mesh") || r["vp"].size() >= 13))
            {
                KitFrame kf;
                kf.part = rec->mPart;
                if (r.has("mesh"))
                {
                    kf.prim.shape = skroad::kitSculptShape(r["mesh"].asUUID().asString(), 5);
                }
                else
                {
                    F64 vp[13];
                    for (S32 k = 0; k < 13; ++k)
                    {
                        vp[k] = r["vp"][k].asReal();
                    }
                    kf.prim.shape = skroad::kitVolumeShape(vp);
                    kf.vp = r["vp"];
                }
                kf.prim.scale = vec3(r["scale"]);
                kf.pos = rec->mOrigin + LLVector3d(vec3(r["pos"]));
                kf.origin = rec->mOrigin;
                const LLSD& q = r["rot"];
                kf.rot = LLQuaternion(F32(q[0].asReal()), F32(q[1].asReal()), F32(q[2].asReal()), F32(q[3].asReal()));
                kit_frames[rec->mRoot].push_back(kf);
            }
            else
            {
                kit_frames[rec->mRoot].push_back(KitFrame());   // a part without its frame: the linkset has no hash
            }
            out.records.push_back(rec);
        }
        for (auto& [root, frames] : kit_frames)
        {
            auto ri = std::find_if(frames.begin(), frames.end(), [&](const KitFrame& f) { return f.part == root; });
            if (ri == frames.end() || std::any_of(frames.begin(), frames.end(), [](const KitFrame& f) { return f.part.isNull(); }))
            {
                continue;
            }
            const LLVector3d rpos = ri->pos;
            const LLQuaternion rrot = ri->rot, inv = ~ri->rot;
            std::vector<skroad::KitPrim> prims;
            for (KitFrame& f : frames)
            {
                f.prim.root = f.part == root;
                f.prim.pos = LLVector3(f.pos - rpos) * inv;
                f.prim.rot = f.rot * inv;
                prims.push_back(f.prim);
            }
            auto shape = std::make_shared<skroad::KitShape>();
            shape->root_rot = rrot;
            for (const KitFrame& f : frames)
            {
                skroad::KitShapePrim p;
                p.scale = f.prim.scale;
                p.pos = f.prim.pos;
                p.rot = f.prim.rot;
                if (f.vp.isArray())
                {
                    p.params = volumeParams(f.vp);
                }
                else if (auto g = out.geometry.find(f.part); g != out.geometry.end())
                {
                    for (const RebuiltFace& face : g->second->faces)
                    {
                        for (U32 i : face.indices)
                        {
                            p.tris.push_back(LLVector3(f.origin + LLVector3d(face.positions[i]) - rpos) * inv);
                        }
                    }
                }
                if (p.tris.empty() && !f.vp.isArray())
                {
                    shape.reset();      // a mesh not rebuilt: no shape to measure
                    break;
                }
                shape->prims.push_back(std::move(p));
            }
            out.kits[root] = { skroad::kitHash(skroad::kitGeoSignature(prims)), rpos, rrot, shape };
        }
        for (const auto& [id, p] : llsd::inMap(sd["props"]))
        {
            skroad::host::Props props;
            props.mName = p["name"].asString();
            props.mOwnerId = p["owner"].asUUID();
            props.mGroupId = p["group"].asUUID();
            out.props[LLUUID(id)] = props;
        }
        for (const LLSD& r : llsd::inArray(sd["regions"]))
        {
            Case::Region region;
            region.x = r["x"].asReal();
            region.y = r["y"].asReal();
            region.width = r.has("width") ? r["width"].asReal() : 256.0;
            const std::string hex = r["cells"].asString();
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
            {
                region.cells.push_back(U8(std::stoul(hex.substr(i, 2), nullptr, 16)));
            }
            out.regions.push_back(std::move(region));
        }
        for (const LLSD& o : llsd::inArray(sd["linden_owners"]))
        {
            out.linden_owners.insert(o.asUUID());
        }
        for (const auto& [part, ids] : llsd::inMap(sd["textures"]))
        {
            std::vector<LLUUID>& list = out.textures[LLUUID(part)];
            for (const LLSD& id : llsd::inArray(ids))
            {
                list.push_back(id.asUUID());
            }
        }
        for (const auto& [part, id] : llsd::inMap(sd["top_tex"]))
        {
            out.top_tex[LLUUID(part)] = id.asUUID();
        }
        for (const auto& [part, faces] : llsd::inMap(sd["faces"]))
        {
            out.faces[LLUUID(part)] = faces;
        }
        for (const LLSD& p : llsd::inArray(sd["poses"]))
        {
            Pose pose;
            pose.t = p[0].asReal();
            pose.car = LLVector3d(p[1].asReal(), p[2].asReal(), p[3].asReal());
            pose.yaw = p[4].asReal() * F_PI / 180.0;
            out.poses.push_back(pose);
        }
        for (size_t i = 0; i < out.poses.size(); ++i)
        {
            const Pose& a = out.poses[i > 0 ? i - 1 : i];
            const Pose& b = out.poses[i > 0 ? i : std::min(i + 1, out.poses.size() - 1)];
            const F64 dt = b.t - a.t;
            out.poses[i].speed = dt > 1e-3 ? std::hypot(b.car.mdV[VX] - a.car.mdV[VX], b.car.mdV[VY] - a.car.mdV[VY]) / dt : 0.0;
        }
        const LLSD& s = sd["settings"];
        if (s.has("keep")) out.keep = s["keep"].asReal();
        if (s.has("look_ahead")) out.look_ahead = F32(s["look_ahead"].asReal());
        if (s.has("car_half_w")) out.car_half_w = s["car_half_w"].asReal();
        if (s.has("step_s")) out.step_s = s["step_s"].asReal();
        if (sd.has("stream"))
        {
            const LLSD& st = sd["stream"];
            Case::Stream& o = out.stream;
            o.on = true;
            if (st.has("radius")) o.radius = st["radius"].asReal();
            if (st.has("rate")) o.rate = st["rate"].asReal();
            if (st.has("props_lo")) o.props_lo = st["props_lo"].asReal();
            if (st.has("props_hi")) o.props_hi = st["props_hi"].asReal();
            if (st.has("props_never")) o.props_never = st["props_never"].asReal();
            if (st.has("seed")) o.seed = U32(st["seed"].asInteger());
        }
        if (out.poses.empty())
        {
            why = "no poses";
            return false;
        }
        return true;
    }

    bool readCaseFile(const std::string& path, Case& out, std::string& why)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            why = "cannot open " + path;
            return false;
        }
        std::stringstream text;
        text << in.rdbuf();
        LLSD sd;
        std::string err;
        if (!LlsdFromJsonString(text.str(), sd, &err))
        {
            why = "not JSON: " + err;
            return false;
        }
        return readCase(sd, out, why);
    }

    void useCase(const Case* c)
    {
        sCase = c;
    }

    void runCase(const Case& c, const std::function<void(const Step&)>& each)
    {
        using namespace skroad;
        useCase(&c);
        {
            auto watch = std::move(sStream.watch);
            sStream = StreamState();
            sStream.watch = std::move(watch);
            sStream.in.assign(c.records.size(), 0);
        }
        // a case starts as a fresh drive: nothing of the last case's pieces, line, route or surface carried into it
        forgetRoad();
        // the model's timers made before the first step read the real clock: the case's time starts after it
        const F64 base = F64(LLTimer::getTotalSeconds().value()) + 10.0;
        const F64 t0 = c.poses.front().t;
        const F64 t1 = c.poses.back().t;
        size_t seg = 0;
        bool first = true;
        std::unordered_map<LLUUID, std::string> last_states;    // each part's state at the last step
        for (F64 t = t0; t <= t1 + 1e-6; t += c.step_s)
        {
            while (seg + 2 < c.poses.size() && c.poses[seg + 1].t <= t)
            {
                ++seg;
            }
            const Pose& a = c.poses[seg];
            const Pose& b = c.poses[std::min(seg + 1, c.poses.size() - 1)];
            const F64 f = b.t > a.t ? std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0) : 0.0;
            Step s;
            s.t = t - t0;
            s.pose.t = t;
            s.pose.car = a.car + (b.car - a.car) * f;
            F64 dyaw = b.yaw - a.yaw;
            while (dyaw > F_PI) dyaw -= 2 * F_PI;
            while (dyaw < -F_PI) dyaw += 2 * F_PI;
            s.pose.yaw = a.yaw + dyaw * f;
            s.pose.speed = b.speed;

            setClock(base + (t - t0));
            if (first)
            {
                sClock.reset();
                first = false;
            }
            const F64 now = sClock.getElapsedTimeF64();
            sCarHalfW = c.car_half_w;
            const F64 hx = std::cos(s.pose.yaw), hy = std::sin(s.pose.yaw);
            const LLVector3d& pos = s.pose.car;
            if (c.stream.on)
            {
                streamStep(c, t - t0, pos);
                s.arrived = sStream.arrived.size();
                s.props_known = sStream.props_told.size();
            }
            else
            {
                s.arrived = c.records.size();
                s.props_known = c.props.size();
            }
            const auto started = std::chrono::steady_clock::now();
            refreshShapes(pos);
            noteDriveSurface(pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], now < 1.0);
            s.laid = readRoad(pos, hx, hy, s.pose.speed, c.keep, now, std::string());
            noteLineWay(now);
            s.ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
            s.walk_ms = sView.walk_ms;
            s.path = sPath;
            s.source = sView.route.empty() ? sView.source : sView.source + " | route " + sView.route;
            s.walk_end = sView.walk_end;
            s.jumped = sView.jump;
            s.jump[0] = sView.jump_x;
            s.jump[1] = sView.jump_y;
            s.jump[2] = sView.land_x;
            s.jump[3] = sView.land_y;
            s.junctions = sView.junctions;
            s.chain = sChainRoots;
            {
                static std::vector<PartState> now_states;
                partStates(now_states);
                std::unordered_map<LLUUID, std::string> seen;
                for (const PartState& p : now_states)
                {
                    std::string k(1, p.named > 0 ? 'r' : p.named == 0 ? 'n' : 'u');
                    for (auto [on, ch] : { std::pair{ p.definite, 'D' }, { p.pending, 'P' }, { p.carried, 'C' }, { p.doubtful, 'd' },
                                           { p.plaza, 'Z' }, { p.walkable, 'W' } })
                    {
                        if (on)
                        {
                            k += ch;
                        }
                    }
                    seen[p.part] = k;
                }
                for (const auto& [part, k] : seen)
                {
                    auto [it, fresh] = last_states.try_emplace(part, k);
                    if (fresh || it->second != k)
                    {
                        it->second = k;
                        s.states.push_back({ part, k });
                    }
                }
                for (auto it = last_states.begin(); it != last_states.end();)
                {
                    if (!seen.count(it->first))
                    {
                        s.states.push_back({ it->first, "-" });
                        it = last_states.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            each(s);
        }
        setClock(-1.0);
        useCase(nullptr);
    }

    void writeStep(std::ostream& out, const Step& s)
    {
        out.precision(10);
        out << "{\"t\":" << s.t << ",\"car\":[" << s.pose.car.mdV[VX] << "," << s.pose.car.mdV[VY] << "," << s.pose.car.mdV[VZ]
            << "],\"yaw\":" << s.pose.yaw * 180.0 / F_PI << ",\"speed\":" << s.pose.speed << ",\"laid\":" << (s.laid ? "true" : "false")
            << ",\"arrived\":" << s.arrived << ",\"props_known\":" << s.props_known
            << ",\"ms\":" << s.ms << ",\"walk_ms\":" << s.walk_ms << ",\"source\":" << LlsdToJson(LLSD(s.source)) << ",\"walk_end\":" << LlsdToJson(LLSD(s.walk_end));
        if (s.jumped)
        {
            out << ",\"jump\":[" << s.jump[0] << "," << s.jump[1] << "," << s.jump[2] << "," << s.jump[3] << "]";
        }
        out << ",\"path\":[";
        for (size_t i = 0; i < s.path.size(); ++i)
        {
            const skroad::LanePoint& p = s.path[i];
            out << (i ? "," : "") << "[";
            put(out, p.x);
            for (F64 v : { p.y, p.z, p.road_w, F64(p.lanes), p.lane_w, p.left_x, p.left_y, p.right_x, p.right_y, p.ax, p.ay, F64(p.access) })
            {
                out << ",";
                put(out, v);
            }
            out << "]";
        }
        out << "],\"junctions\":[";
        for (size_t i = 0; i < s.junctions.size(); ++i)
        {
            const skroad::Junction& j = s.junctions[i];
            out << (i ? "," : "") << "{\"x\":" << j.x << ",\"y\":" << j.y << ",\"from\":\"" << j.from << "\",\"chosen\":" << j.chosen
                << ",\"branches\":[";
            for (size_t k = 0; k < j.branches.size(); ++k)
            {
                const skroad::Branch& b = j.branches[k];
                out << (k ? "," : "") << "{\"root\":\"" << b.root << "\",\"part\":\"" << b.part << "\",\"turn\":" << b.turn_deg
                    << ",\"kind\":\"" << b.kind() << "\",\"x\":" << b.x << ",\"y\":" << b.y << ",\"definite\":" << (b.definite ? "true" : "false")
                    << ",\"access\":\"" << skroad::accessName(b.access) << "\",\"mouth\":" << (b.mouth ? "true" : "false") << "}";
            }
            out << "]}";
        }
        out << "],\"states\":[";
        for (size_t i = 0; i < s.states.size(); ++i)
        {
            out << (i ? "," : "") << "[\"" << s.states[i].first << "\",\"" << s.states[i].second << "\"]";
        }
        out << "],\"chain\":[";
        for (size_t i = 0; i < s.chain.size(); ++i)
        {
            out << (i ? "," : "") << "\"" << s.chain[i] << "\"";
        }
        out << "]}\n";
    }
}

// The model's host: the case.
namespace skroad::host
{
    using skroadbench::sCase;

    CensusPtr census()
    {
        static const std::vector<skworld::RecordPtr> none;
        static const skroadbench::Case* last = nullptr;
        static std::shared_ptr<const CensusView> view;
        if (!sCase)
        {
            return nullptr;
        }
        if (sCase->stream.on)
        {
            return skroadbench::sStream.view;   // null until the first part arrives
        }
        if (sCase != last)
        {
            last = sCase;
            view = std::make_shared<const CensusView>(CensusView{ 1, sCase->records, none, none, none });
        }
        return view;
    }

    const Props* properties(const LLUUID& root)
    {
        if (!sCase)
        {
            return nullptr;
        }
        if (sCase->stream.on && !skroadbench::streamPropsKnown(root))
        {
            return nullptr;
        }
        auto it = sCase->props.find(root);
        return it != sCase->props.end() ? &it->second : nullptr;
    }

    bool hasProperties(const LLUUID& root)
    {
        return properties(root) != nullptr;
    }

    bool watchProperties(std::function<void(const LLUUID&)> changed)
    {
        skroadbench::sStream.watch = std::move(changed);   // told as a streamed case's properties come
        return true;
    }

    bool lindenOwner(const LLUUID& owner)
    {
        return sCase && sCase->linden_owners.count(owner);
    }

    void learnLdpw(const LLUUID&)
    {
    }

    S32 roadLandAt(F64 x, F64 y, std::string*)
    {
        return parcelOwnership(x, y) == 0 ? 1 : -1;      // PARCEL_PUBLIC; a case carries no parcel owners
    }

    bool privateRoadsAllowed()
    {
        return true;        // the cases were recorded before the rule: their branches stand
    }

    S32 parcelOwnership(F64 x, F64 y)
    {
        if (!sCase)
        {
            return -1;
        }
        for (const skroadbench::Case::Region& r : sCase->regions)
        {
            const S32 n = S32(std::lround(std::sqrt(F64(r.cells.size()))));
            if (n <= 0 || x < r.x || y < r.y || x >= r.x + r.width || y >= r.y + r.width)
            {
                continue;
            }
            const S32 col = std::clamp(S32((x - r.x) * n / r.width), 0, n - 1);
            const S32 row = std::clamp(S32((y - r.y) * n / r.width), 0, n - 1);
            return r.cells[row * n + col] & 0x7;     // PARCEL_OWNERSHIP's bits
        }
        return -1;
    }

    void learnTextures(PieceShapes& piece)
    {
        // as skroaddriver.cpp's learnTextures, from the survey's textures and the graph's top face
        if (!sCase || (piece.textures_known && piece.top_known))
        {
            return;
        }
        auto tex = sCase->textures.find(piece.part);
        if (tex == sCase->textures.end())
        {
            return;     // its object not live
        }
        if (!piece.top_known)
        {
            if (auto top = sCase->top_tex.find(piece.part); top != sCase->top_tex.end())
            {
                piece.top_tex = top->second;
                piece.top_known = true;
            }
        }
        if (!piece.textures_known)
        {
            piece.textures = tex->second;
            std::sort(piece.textures.begin(), piece.textures.end());
            piece.textures.erase(std::unique(piece.textures.begin(), piece.textures.end()), piece.textures.end());
            piece.textures_known = !piece.textures.empty();
            piece.match_gen = 0;
        }
    }

    bool meshPlacement(const skworld::Record&, MeshPlacement&)
    {
        return false;       // the survey does not record it: a mesh's outline is its own faces merged
    }

    bool rootRotation(const LLUUID& root, LLQuaternion& rot)
    {
        if (!sCase)
        {
            return false;
        }
        auto it = sCase->root_rot.find(root);
        if (it == sCase->root_rot.end())
        {
            return false;
        }
        rot = it->second;
        return true;
    }

    void sortAsks(const LLVector3d&)
    {
    }

    F32 lookAheadSetting()
    {
        return sCase ? sCase->look_ahead : 100.f;
    }

    std::string goal()
    {
        return "explore";
    }

    bool surveyOn()
    {
        return false;
    }

    bool kitOf(const LLUUID& root, U64& hash, LLVector3d& pos, LLQuaternion& rot)
    {
        if (!sCase)
        {
            return false;
        }
        auto it = sCase->kits.find(root);
        if (it == sCase->kits.end())
        {
            return false;
        }
        hash = it->second.hash;
        pos = it->second.pos;
        rot = it->second.rot;
        return true;
    }

    bool measureKit(const LLUUID& root, U64 key)
    {
        if (!sCase)
        {
            return false;
        }
        auto it = sCase->kits.find(root);
        if (it == sCase->kits.end() || !it->second.shape)
        {
            return false;
        }
        skroad::Kit kit;
        const bool ok = skroad::measureKit(*it->second.shape, kit);
        skroad::kitMeasured(key, ok, kit);
        return true;
    }

    std::string signLibraryPath()
    {
        return "";
    }

    bool signTextures(const LLUUID&, std::vector<LLUUID>&)
    {
        return false;
    }

    std::string logPath(const std::string&)
    {
        return std::string();       // nothing driven, nothing written
    }

    std::vector<LanePoint> testCourse(const std::string&, F64, F64, F64, F64, F64, F64)
    {
        return {};
    }
}
