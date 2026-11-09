/**
 * @file skroadbenchcase.h
 * @brief Spike: a road bench case, a frozen spot of road the road model reads as the viewer reads the world.
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

#ifndef SK_ROADBENCHCASE_H
#define SK_ROADBENCHCASE_H

#include "llsd.h"
#include "skroadbenchgeometry.h"
#include "skroadkits.h"
#include "skroadmodel.h"

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A case is one spot: the census records of the parts about it (the road survey's "part" records), what the
// simulator said of their linksets ("props"), the owners known to be Lindens or LDPW's, the parts' textures, and
// the car's poses through it. The bench plays the poses through the model as beforeAgentUpdate does, and writes
// what it read at each: the lane line with the road's edges, the junctions, why the walk ended, the time it took.
// A case's model state is the process's: one case a process.
namespace skroadbench
{
    struct Pose
    {
        F64 t = 0;
        LLVector3d car;
        F64 yaw = 0;                // radians, 0 east, counter-clockwise
        F64 speed = 0;              // m/s
    };

    struct Case
    {
        std::string name;
        std::vector<skworld::RecordPtr> records;
        std::unordered_map<LLUUID, skroad::host::Props> props;
        std::unordered_set<LLUUID> linden_owners;
        std::unordered_map<LLUUID, std::vector<LLUUID>> textures;      // a part's faces'
        std::unordered_map<LLUUID, LLUUID> top_tex;                    // a part's top face's
        std::unordered_map<LLUUID, LLQuaternion> root_rot;             // a linkset's root, where its record says
        struct KitOf                    // a linkset's kit hash and root pose, where every part's frame is recorded
        {
            U64 hash = 0;
            LLVector3d pos;
            LLQuaternion rot;
            std::shared_ptr<const skroad::KitShape> shape;      // its geometry, null where a mesh was not rebuilt
        };
        std::unordered_map<LLUUID, KitOf> kits;
        std::unordered_map<LLUUID, LLSD> faces;                        // a part's faces' surfaces (survey "faces")
        // Parts recorded by reference, rebuilt: their drawn faces (by face index, as "faces") and physics
        std::string cache_dir;                                         // the viewer's asset cache (the mesh files)
        std::unordered_map<LLUUID, std::shared_ptr<const Rebuilt>> geometry;
        std::vector<std::string> unbuilt;                              // references that could not be rebuilt, and why
        struct Region                   // a region's parcel overlay: ownership bytes, a row a 4 m strip south to north
        {
            F64 x = 0, y = 0, width = 256;
            std::vector<U8> cells;
        };
        std::vector<Region> regions;
        std::vector<Pose> poses;
        F64 step_s = 0.2;           // the path is read this often (PATH_EVERY), poses between interpolated
        F64 keep = 0.0;             // SKRoadAutopilotLaneOffset
        F32 look_ahead = 100.f;     // SKRoadAutopilotLookAhead
        F64 car_half_w = 1.47;
        // Streamed: the parts arrive as the world streams in, nearest the car first, rather than all at the start,
        // and each linkset's properties some seconds after its first part, so the model settles as it does live.
        struct Stream
        {
            bool on = false;
            F64 radius = 128;           // a part arrives once the car is this near its box
            F64 rate = 400;             // parts a second at most
            F64 props_lo = 1, props_hi = 10;    // a linkset's properties this long after its first part
            F64 props_never = 0;        // the share of linksets whose properties never come
            U32 seed = 1;
        } stream;
    };

    // From the case's LLSD (the extractor's JSON): false with a reason where it is not one.
    bool readCase(const LLSD& sd, Case& out, std::string& why);
    bool readCaseFile(const std::string& path, Case& out, std::string& why);

    // The case the model's host reads (skroad::host): set before the first step.
    void useCase(const Case* c);

    struct Step
    {
        F64 t = 0;
        Pose pose;
        bool laid = false;
        F64 ms = 0;                 // refreshShapes and readRoad, wall clock
        F64 walk_ms = 0;            // of it, the walk and its laying (View::walk_ms)
        std::vector<skroad::LanePoint> path;
        std::string source, walk_end;
        bool jumped = false;
        F64 jump[4] = {};       // the lip's x, y and the landing's x, y
        std::vector<skroad::Junction> junctions;
        std::vector<LLUUID> chain;
        size_t arrived = 0;         // parts the census holds (all of them unless streamed)
        size_t props_known = 0;     // linksets whose properties have come
    };

    // Plays every pose through the model, one step every step_s between them, handing `each` what it read.
    void runCase(const Case& c, const std::function<void(const Step&)>& each);

    // One step as a JSON object on one line.
    void writeStep(std::ostream& out, const Step& s);
}

#endif // SK_ROADBENCHCASE_H
