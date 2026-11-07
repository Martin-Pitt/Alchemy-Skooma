/**
 * @file skroaddriver.cpp
 * @brief Spike: drives the vehicle the agent sits in along the Linden road's right lane.
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

// The steering is the predictive controller of the offline benchmark (V:/Scratch/worldmap/autopilot/bench,
// controllers/predictive.py, predictive.md), ported: it beat the rule-based ones on both Route 8 (JP road
// lane strips) and Route 9 (curves of narrow pieces). In short:
//  - the road is its pieces' exact plan shapes (mesh triangles facing up, flat boxes), named as road where
//    the simulator has said so; a cross-section intersects a line across the road with them and merges the
//    intervals, so lanes come from the road's edges whatever the pieces' sizes or orientations;
//  - the lane's centre line is walked ahead from cross-section to cross-section (the lane path), which is
//    what gives the road's curvature;
//  - every tap sent is replayed through the car's measured response (the kernel table below), so the yaw
//    rate still to come from taps in flight is known; unexplained yaw rate is a decaying disturbance (a
//    region border's kick, a quirk), steered through, never hands-off;
//  - each decision rolls the car forward 2.5 s and tries 91 tap plans (none, or a tap of 6 lengths either
//    way, then optionally a second of 2 lengths 0.6 s later), scoring distance from the line, room to the
//    lane's edges (a border ahead counted as a kick of either sign) and a cost per tap;
//  - the car's gain and delay are learnt while driving (least squares over a range of delays), within
//    +-30 % of the fit.

using namespace skroad;
namespace skroad::drive {}
using namespace skroad::drive;

namespace skroad::drive
{
    F64 sUnseatedSince = -1.0;                  // when the sit was lost (a region crossing loses it for a moment)
    constexpr F64 UNSEAT_GRACE_S = 3.0;
    constexpr F64 DT = 0.02;                    // the controller's frame: the car model's sample step
    const char* const DEMAND = "road-driver";

    // ---------------------------------------------------------------- the car's tap response

#include "skroaddriverkernels.inc"

    S32 kernelFor(S32 frames)
    {
        const F64 hold = frames * DT;
        S32 best = 0;
        for (S32 k = 1; k < KERNEL_HOLDS; ++k)
        {
            if (std::fabs(KERNEL_HOLD_S[k] - hold) < std::fabs(KERNEL_HOLD_S[best] - hold))
            {
                best = k;
            }
        }
        return best;
    }

    // Nominal yaw rate (deg/s) b frames after a tap held `frames` frames went down: the longest kernel
    // while held, the hold's own after release.
    F64 tapRate(S32 frames, S32 b)
    {
        if (b < 0 || b >= KERNEL_N)
        {
            return 0.0;
        }
        return b < frames ? KERNEL[KERNEL_HOLDS - 1][b] : KERNEL[kernelFor(frames)][b];
    }
    constexpr F64 ROLL_DT = 0.1;                // cost samples
    constexpr F64 DEAD_TIME = 0.3;              // samples before this are not scored: nothing sent now acts sooner
    constexpr F64 DECIDE_EVERY = 0.04;
    constexpr F64 MIN_GAP = 0.10;               // s after a release before the next tap
    constexpr S32 HOLDS[] = { 1, 2, 4, 6, 10, 17 };         // frames of DT: 1 is one sim frame (1/45 s), the smallest tap
    constexpr F64 SECOND_AT = 0.6;
    constexpr S32 SECOND_HOLDS[] = { 4, 10 };
    constexpr F64 PROBE_QUIET_S = 1.5;
    constexpr S32 PROBE_LONG_HOLDS[] = { 25, 38, 50, 75, 100 };    // frames: 0.5, 0.75, 1, 1.5, 2 s
    constexpr F64 PROBE_LONG_QUIET_S = 2.0;     // after a long hold's release, left alone this long
    F64 sProbeQuietUntil = -100.0;
    S32 sLearnFrame = -1;          // a probe tap is left alone this long, and none follows a steer sooner
    constexpr F64 PROBE_ON_LINE_M = 0.6;        // a probe tap only this near the line,
    constexpr F64 PROBE_ALIGNED_DEG = 4.0;      // pointing along it within this
    F64 sLastSteer = -100.0;
    F64 sProbeLast = -100.0;
    S32 sProbeStep = 0;
    constexpr F64 EDGE_W = 30.0;
    constexpr F64 EDGE_MARGIN = 0.3;
    constexpr F64 TAU_NOISE = 0.4, TAU_KICK = 0.25, KICK_WINDOW = 1.0, KICK_SIZE = 30.0;
    constexpr F64 LOST_S = 8.0;                 // no road this long: give up
    constexpr F64 LOST_BRAKE_S = 6.0;           // and braking to a stop on the road no longer than this past it
    // online fit (openpilot's torqued / lagd)
    constexpr F64 GAIN_SANITY = 0.3;
    constexpr S32 DELAY_MIN = -6, DELAY_MAX = 6;           // frames
    constexpr S32 FIT_LAG = 8;
    constexpr F64 FORGET = 0.998;
    constexpr F64 FIT_MIN_SXX = 3000.0;
    constexpr F64 BORDER_SKIP = 1.5, EDGE_SKIP = 10.0;
    // speed
    constexpr F64 CROSSING_SLOW_S = 3.0;
    constexpr F32 CROSSING_SPEED = 6.f;
    constexpr F64 SHIFT_HOLD_S = 0.10, SHIFT_GAP_S = 1.5;
    constexpr F32 EMERGENCY_OVER = 8.f;
    constexpr F32 THROTTLE_BAND = 1.f;          // m/s either side of the target the throttle is held across
    bool sThrottle = false;
    constexpr F64 THROTTLE_LAG_S = 0.3;         // s from a throttle key to the speed seen answering it
    constexpr F64 THROTTLE_ACCEL = 8.0;         // m/s^2 under the throttle, at cruise
    constexpr F64 THROTTLE_DECEL = 10.0;        // m/s^2 off it
    std::deque<std::pair<F64, bool>> sThrottleLog;     // (when, held) over the last THROTTLE_LAG_S



    // ---------------------------------------------------------------- state

    struct Tap
    {
        S32 start;      // frame
        S32 dir;        // +1 left (A), -1 right (D)
        S32 hold;       // frames
    };

    bool sOn = false;
    bool sPreviewing = false;                   // the overlay drawn while stopped (see beforeAgentUpdate)

    // The way travelled. The driver is a cruise for a person driving: whatever it does to recover (backing off a
    // crash, a stuck, a lost line) it carries on the way it was going, never the way the car ends up pointing.
    // After a crash the backing turned the car round, the line was walked from its new nose, and it drove the
    // route backwards. So the line is walked along sTravel, the way the car has been going forward, and the
    // car's own track (the trail, a point every TRAIL_STEP_M, TRAIL_MAX_M of it) is kept for the session: the
    // overlay draws it, and a fresh line laid back over it is refused unless the driver turned round on purpose.
    std::deque<TrailPoint> sTrail;          // TrailPoint: skroaddriveinternal.h
    U64 sTrailCount = 0;                        // trail points ever added: sTrail[i] is number sTrailCount - size + i
    U64 sTrailTurnCount = 0;                    // the first point since the last deliberate turn (or start)
    constexpr F64 TRAIL_MAX_M = 5000.0, TRAIL_OFF_FORGET_S = 300.0, TRAIL_JUMP_M = 60.0;
    F64 sTrailOffAt = -1.0;                     // wall clock: when the driver last went off
    bool sTravelSet = false;
    F64 sTravelX = 1.0, sTravelY = 0.0;
    llofstream sLog;
    LLUUID sVehicle;

    // the controller's state (predictive.py)
    S32 sFrame = 0;
    std::vector<Tap> sTaps;
    S32 sKey = 0, sKeySince = 0, sKeyUntil = 0;
    S32 sLastRelease = -99, sNextDecide = 0, sBorderFrame = -999;
    U64 sCell = 0;
    S32 sPathFrame = -999;
    // the measured yaw rate
    F64 sPrevYaw = 0.0, sPrevYawT = -1.0, sYawRate = 0.0;    // rad/s, signed
    // online fit
    F64 sGain = 1.0;
    S32 sShift = 0;
    std::map<S32, std::pair<F64, bool>> sMeas;   // frame -> measured deg/s, usable for the fit
    F64 sSxy[DELAY_MAX - DELAY_MIN + 1], sSxx[DELAY_MAX - DELAY_MIN + 1];
    // per shift, per hold: (lateral per m/s, heading, rate) at each sample, the learnt car at gain 1
    std::map<S32, std::map<S32, std::vector<std::array<F64, 3>>>> sProfiles;

    // speed and gears, as Nexii drives: W held, the speed the gear's (see onSettingChanged)
    S32 sGear = 0;
    F64 sLastShift = -100.0;
    S32 sShiftDir = 0;
    F64 sShiftEnd = -1.0;
    std::unordered_map<S32, F64> sGearTop;
    F64 sAccelT = 0.0, sAccelV = 0.0, sAccel = 0.0;
    F64 sLastCrossingT = -100.0;

    // what it said last and in which frame, and why it stopped last (sk_road_driver::lastStopReason)
    std::string sLastSaid, sStopReason;
    U32 sLastSaidFrame = 0;

    void say(const std::string& text)
    {
        sLastSaid = text;
        sLastSaidFrame = LLFrameTimer::getFrameCount();
        LLSD args;
        args["MESSAGE"] = "Road autopilot: " + text;
        LLNotificationsUtil::add("SystemMessageTip", args);
        LL_INFOS("SKRoadDriver") << text << LL_ENDL;
    }

    // The driver's events for the control channel (sk_road_driver::setEventHook).
    std::function<void(const std::string&, const LLSD&)> sEventHook;
    void fireEvent(const std::string& kind, LLSD data, const LLVector3d& at)
    {
        if (sEventHook)
        {
            data["at"] = llsd::array(at.mdV[VX], at.mdV[VY], at.mdV[VZ]);
            sEventHook(kind, data);
        }
    }

    // The one place the driving keys go out. W and S together are the car's jump, so they never go in the same
    // frame, whatever asked for both, and S follows W only from the frame after W was let go.
    U32 sLastSentKeys = 0;
    void sendKeys(U32 keys, std::string& note)
    {
        constexpr U32 BOTH = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG;
        const bool asked_both = (keys & BOTH) == BOTH;
        if (asked_both || ((keys & AGENT_CONTROL_AT_NEG) && (sLastSentKeys & AGENT_CONTROL_AT_POS)))
        {
            keys &= asked_both ? ~BOTH : ~U32(AGENT_CONTROL_AT_NEG);
            if (asked_both)
            {
                note += note.empty() ? "W+S suppressed" : ", W+S suppressed";
            }
        }
        sLastSentKeys = keys;
        gAgent.setControlFlags(keys);
    }

    void stop(const std::string& why)
    {
        writeDriven();      // the roads driven, kept
        if (!sOn)
        {
            return;
        }
        sOn = false;
        sTrailOffAt = LLTimer::getTotalSeconds().value();
        sView.valid = false;
        SKWorldCensus::instance().removeDemand(DEMAND);
        if (SKAirCells::instanceExists())
        {
            SKAirCells::instance().removeDemand(AIR_DEMAND);
        }
        sAirTiles.clear();
        if (sLog.is_open())
        {
            sLog.close();
        }
        // turned off by its own reason said this frame (stuck, no road): that reason, not "off"
        sStopReason = why == "off" && sLastSaidFrame == LLFrameTimer::getFrameCount() ? sLastSaid : why;
        say(why);
        gSavedSettings.setBOOL("SKRoadAutopilot", false);
    }

    F64 carHalfWidth(const LLViewerObject* car)
    {
        F32 lo = 1e9f, hi = -1e9f;
        auto add = [&lo, &hi](const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale)
        {
            for (S32 i = 0; i < 8; ++i)
            {
                const LLVector3 corner((i & 1 ? .5f : -.5f) * scale.mV[VX], (i & 2 ? .5f : -.5f) * scale.mV[VY],
                                       (i & 4 ? .5f : -.5f) * scale.mV[VZ]);
                const F32 y = (pos + corner * rot).mV[VY];
                lo = llmin(lo, y);
                hi = llmax(hi, y);
            }
        };
        add(LLVector3::zero, LLQuaternion::DEFAULT, car->getScale());
        for (LLViewerObject* child : car->getChildren())
        {
            if (child && !child->isAvatar())
            {
                add(child->getPosition(), child->getRotation(), child->getScale());
            }
        }
        return hi > lo ? (hi - lo) / 2 : 1.0;
    }

    // How far the car's rear reaches behind its root (its parts' boxes along -x).
    F64 carRear(const LLViewerObject* car)
    {
        F32 lo = 1e9f;
        auto add = [&lo](const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale)
        {
            for (S32 i = 0; i < 8; ++i)
            {
                const LLVector3 corner((i & 1 ? .5f : -.5f) * scale.mV[VX], (i & 2 ? .5f : -.5f) * scale.mV[VY],
                                       (i & 4 ? .5f : -.5f) * scale.mV[VZ]);
                lo = llmin(lo, (pos + corner * rot).mV[VX]);
            }
        };
        add(LLVector3::zero, LLQuaternion::DEFAULT, car->getScale());
        for (LLViewerObject* child : car->getChildren())
        {
            if (child && !child->isAvatar())
            {
                add(child->getPosition(), child->getRotation(), child->getScale());
            }
        }
        return lo < 0 && lo > -10 ? -lo : 2.4;
    }
    F64 sCarRear = 2.4;

    // ---------------------------------------------------------------- the internal model

    F64 nominalRate(S32 f)
    {
        F64 r = 0.0;
        for (const Tap& t : sTaps)
        {
            r += t.dir * tapRate(t.hold, f - t.start);
        }
        if (sKey && f >= sKeySince)
        {
            const S32 b = f - sKeySince;
            if (b < KERNEL_N)
            {
                r += sKey * KERNEL[KERNEL_HOLDS - 1][b];
            }
        }
        return r;
    }

    F64 modelRate(S32 f)
    {
        return sGain * nominalRate(f - sShift);
    }

    void learn(F64 measured_deg, bool clean)
    {
        const S32 f = sFrame - 1;
        sMeas[f] = { measured_deg, clean };
        const S32 g = f - FIT_LAG;
        auto it = sMeas.find(g);
        if (it != sMeas.end())
        {
            auto later = sMeas.find(g + DELAY_MAX);
            const bool ok = it->second.second && (later == sMeas.end() || later->second.second);
            if (ok)
            {
                const F64 y = it->second.first;
                F64 best = -1.0;
                S32 bj = -1;
                for (S32 j = 0; j <= DELAY_MAX - DELAY_MIN; ++j)
                {
                    const F64 m = nominalRate(g - (DELAY_MIN + j));
                    sSxy[j] = FORGET * sSxy[j] + m * y;
                    sSxx[j] = FORGET * sSxx[j] + m * m;
                }
                for (S32 j = 0; j <= DELAY_MAX - DELAY_MIN; ++j)
                {
                    if (sSxx[j] > FIT_MIN_SXX)
                    {
                        const F64 ex = sSxy[j] * sSxy[j] / sSxx[j];
                        if (ex > best)
                        {
                            best = ex;
                            bj = j;
                        }
                    }
                }
                if (bj >= 0)
                {
                    sShift = DELAY_MIN + bj;
                    sGain = std::clamp(sSxy[bj] / sSxx[bj], 1 - GAIN_SANITY, 1 + GAIN_SANITY);
                }
            }
        }
        while (!sMeas.empty() && sMeas.begin()->first < g)
        {
            sMeas.erase(sMeas.begin());
        }
    }

    const std::map<S32, std::vector<std::array<F64, 3>>>& profiles()
    {
        auto it = sProfiles.find(sShift);
        if (it != sProfiles.end())
        {
            return it->second;
        }
        const S32 nf = S32(std::lround(HORIZON / DT)), sp = S32(std::lround(ROLL_DT / DT));
        std::map<S32, std::vector<std::array<F64, 3>>> prof;
        std::vector<S32> holds(std::begin(HOLDS), std::end(HOLDS));
        holds.insert(holds.end(), std::begin(SECOND_HOLDS), std::end(SECOND_HOLDS));
        for (S32 h : holds)
        {
            if (prof.count(h))
            {
                continue;
            }
            F64 hd = 0, lat = 0;
            std::vector<std::array<F64, 3>> samples;
            for (S32 fr = 0; fr < nf; ++fr)
            {
                const F64 r = tapRate(h, fr - sShift);
                hd += r * PI / 180 * DT;
                lat += hd * DT;
                if ((fr + 1) % sp == 0)
                {
                    samples.push_back({ lat, hd, r });
                }
            }
            prof[h] = samples;
        }
        return sProfiles[sShift] = prof;
    }

    // The lane ahead, part to part (see Meet).
    // On a test course with the probe on, the controller holds off while the car is near the line (1.5 m,
    // 10 deg), so the probe gets the quiet it needs between its taps; outside that it steers back.
    bool probeQuiet(const LLVector3d& pos, F64 yaw, bool probe);
    std::pair<S32, S32> pwmSteer(const LLVector3d& pos, F64 yaw, F64 speed, F64 now, std::string& note);

    // ---------------------------------------------------------------- a test course

    // SKRoadAutopilotTestCourse: no road, a known lane line laid out from where the car stood when the course was
    // chosen (or the autopilot started), for tuning the controller on flat land (a sandbox): "straight" (500 m),
    // "slalom" (a sine, 6 m either side over 80 m), "circle" (40 m radius, to the right), "figure8" (two 40 m
    // circles). The lane line is exact, so the drive log's error is the controller's.
    struct CourseAnchor
    {
        std::string name;
        F64 x = 0, y = 0, z = 0, hx = 1, hy = 0;
        bool set = false;
    };
    CourseAnchor sCourse;
    F64 sCourseS = -1;                          // where on the course the car was last (arc length)

    // A test course's name and size: "circle" (40 m radius) or "circle20"; "figure8" or "figure8_15" (each
    // loop's radius); "slalom" (6 m either side every 80 m) or "slalom_4_40" (amplitude, wavelength); "straight".
    struct CourseShape
    {
        std::string kind;
        F64 r = 40.0, amp = 6.0, wave = 80.0;
    };

    CourseShape courseShape(const std::string& name)
    {
        CourseShape c;
        for (const char* k : { "figure8", "circle", "slalom", "straight" })
        {
            if (name.rfind(k, 0) == 0)
            {
                c.kind = k;
                break;
            }
        }
        std::string rest = name.substr(c.kind.size());
        if (!rest.empty() && rest[0] == '_')
        {
            rest = rest.substr(1);
        }
        if (c.kind == "slalom")
        {
            F64 a = 0, w = 0;
            if (sscanf(rest.c_str(), "%lf_%lf", &a, &w) == 2 && a > 0 && w > 4)
            {
                c.amp = a;
                c.wave = w;
            }
        }
        else if (!rest.empty())
        {
            const F64 r = std::atof(rest.c_str());
            if (r >= 3.0)
            {
                c.r = r;
            }
        }
        return c;
    }

    void coursePoint(const std::string& name, F64 s, F64& lx, F64& ly)
    {
        const CourseShape c = courseShape(name);
        const F64 R = c.r;
        if (c.kind == "slalom")
        {
            lx = s;
            ly = c.amp * std::sin(2 * PI * s / c.wave);
        }
        else if (c.kind == "circle")
        {
            const F64 a = s / R;    // to the right: centre at (0, -R)
            lx = R * std::sin(a);
            ly = -R + R * std::cos(a);
        }
        else if (c.kind == "figure8")
        {
            const F64 loop = 2 * PI * R;
            const F64 m = std::fmod(s, 2 * loop);
            if (m < loop)
            {
                const F64 a = m / R;    // right-hand circle first
                lx = R * std::sin(a);
                ly = -R + R * std::cos(a);
            }
            else
            {
                const F64 a = (m - loop) / R;   // then the left-hand one, through the same crossing
                lx = R * std::sin(a);
                ly = R - R * std::cos(a);
            }
        }
        else
        {
            lx = s;
            ly = 0;
        }
    }

    // "lap": a line recorded on a real road (sk-road-course.json in the logs folder, {"points": [[x, y, z], ...]},
    // global metres, about a metre apart, in the order driven; made from a survey drive by
    // V:/Scratch/worldmap/autopilot/make_course.py), driven as it lies. The line is Nexii's own, smoothed, so the
    // drive log's error is the controller's against a human line on the road, with none of the road walk's mistakes.
    std::vector<std::array<F64, 3>> sLap;
    std::vector<F64> sLapS;     // arc length at each point
    bool sLapRead = false;

    bool sLapLoop = false;      // the course closes on itself ("loop": true): driven round and round
    bool sLapArena = false;     // the course is an arena track (buildArenaCourse)

    // ---------------------------------------------------------------- arena tracks

    // SKRoadAutopilotArena (on by default): the test courses are tracks laid inside the parcel the car is on (a
    // flat open one, as Lua Beta Anderson's), centred on it and sized to fit with ARENA_MARGIN_M to spare, each
    // a closed loop driven round and round: the same line every run, for every car, and the car never leaves
    // the parcel. The track's long axis is the parcel's. Sizes asked for that do not fit are shrunk to fit and
    // said. Shapes (radius r, metres): "circle<r>" (clockwise), "figure8_<r>" (two loops side by side),
    // "straight"/"oval" (a stadium: two straights joined by half circles), "slalom_<amplitude>_<wavelength>" (the
    // stadium with the sine on its straights).
    constexpr F64 ARENA_MARGIN_M = 8.0;

    // The arena: the box of the parcel the car was on when the arena was first asked for this run (a flat open
    // one, as Lua Beta Anderson's), global, kept inside its region by ARENA_REGION_MARGIN_M; failing that (no
    // parcel, or one under 30 m), 120 m about the car then. Latched: read again every frame it followed the car
    // off Anderson onto the next parcel, slopes and the sim's edge and all.
    constexpr F64 ARENA_REGION_MARGIN_M = 32.0;  // 8 left a flat region's arena a car's slip from the sim's edge
    constexpr F64 ARENA_FLAT_M = 0.5;           // the land within this of the land under the car
    constexpr F64 ARENA_DRY_M = 0.5;            // and this above the water
    constexpr F64 ARENA_STEP_M = 2.0;
    bool sArenaSet = false;
    F64 sArenaX0 = 0, sArenaX1 = 0, sArenaY0 = 0, sArenaY1 = 0;

    void arenaBox(F64 x, F64 y, F64& x0, F64& x1, F64& y0, F64& y1)
    {
        if (!sArenaSet)
        {
            sArenaSet = true;
            sArenaX0 = x - 60;
            sArenaX1 = x + 60;
            sArenaY0 = y - 60;
            sArenaY1 = y + 60;
            std::string what = "120 m about the car";
            LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            LLViewerRegion* region = gAgent.getRegion();
            if (region)
            {
                const LLVector3d o = region->getOriginGlobal();
                if (parcel)
                {
                    const LLVector3 lo = parcel->getAABBMin(), hi = parcel->getAABBMax();
                    if (hi.mV[VX] - lo.mV[VX] >= 30 && hi.mV[VY] - lo.mV[VY] >= 30)
                    {
                        sArenaX0 = o.mdV[VX] + lo.mV[VX];
                        sArenaX1 = o.mdV[VX] + hi.mV[VX];
                        sArenaY0 = o.mdV[VY] + lo.mV[VY];
                        sArenaY1 = o.mdV[VY] + hi.mV[VY];
                        what = "parcel '" + parcel->getName() + "'";
                    }
                }
                const F64 w = region->getWidth();
                sArenaX0 = std::max(sArenaX0, o.mdV[VX] + ARENA_REGION_MARGIN_M);
                sArenaX1 = std::min(sArenaX1, o.mdV[VX] + w - ARENA_REGION_MARGIN_M);
                sArenaY0 = std::max(sArenaY0, o.mdV[VY] + ARENA_REGION_MARGIN_M);
                sArenaY1 = std::min(sArenaY1, o.mdV[VY] + w - ARENA_REGION_MARGIN_M);
                // and of that, only the flat land about the car: a rectangle grown from where the car stands,
                // a side at a time by ARENA_STEP_M, while every point of the new strip (ARENA_STEP_M apart) lies
                // within ARENA_FLAT_M of the land under the car and ARENA_DRY_M above the water. A sandbox is
                // one parcel, region wide, shore and water and slopes and all: its box is no arena.
                const F64 water = region->getWaterHeight();
                const F64 h0 = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(x, y, 0));
                auto flat = [&](F64 px, F64 py)
                {
                    const F64 h = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(px, py, 0));
                    return std::fabs(h - h0) <= ARENA_FLAT_M && h >= water + ARENA_DRY_M;
                };
                auto strip_flat = [&](F64 ax, F64 ay, F64 bx, F64 by)
                {
                    const F64 L = std::hypot(bx - ax, by - ay);
                    const S32 n = std::max(1, S32(L / ARENA_STEP_M));
                    for (S32 i = 0; i <= n; ++i)
                    {
                        if (!flat(ax + (bx - ax) * i / n, ay + (by - ay) * i / n))
                        {
                            return false;
                        }
                    }
                    return true;
                };
                F64 fx0 = x - ARENA_STEP_M, fx1 = x + ARENA_STEP_M, fy0 = y - ARENA_STEP_M, fy1 = y + ARENA_STEP_M;
                bool grew = true;
                while (grew)
                {
                    grew = false;
                    if (fx0 - ARENA_STEP_M >= sArenaX0 && strip_flat(fx0 - ARENA_STEP_M, fy0, fx0 - ARENA_STEP_M, fy1))
                    {
                        fx0 -= ARENA_STEP_M;
                        grew = true;
                    }
                    if (fx1 + ARENA_STEP_M <= sArenaX1 && strip_flat(fx1 + ARENA_STEP_M, fy0, fx1 + ARENA_STEP_M, fy1))
                    {
                        fx1 += ARENA_STEP_M;
                        grew = true;
                    }
                    if (fy0 - ARENA_STEP_M >= sArenaY0 && strip_flat(fx0, fy0 - ARENA_STEP_M, fx1, fy0 - ARENA_STEP_M))
                    {
                        fy0 -= ARENA_STEP_M;
                        grew = true;
                    }
                    if (fy1 + ARENA_STEP_M <= sArenaY1 && strip_flat(fx0, fy1 + ARENA_STEP_M, fx1, fy1 + ARENA_STEP_M))
                    {
                        fy1 += ARENA_STEP_M;
                        grew = true;
                    }
                }
                sArenaX0 = fx0;
                sArenaX1 = fx1;
                sArenaY0 = fy0;
                sArenaY1 = fy1;
                char buf[160];
                snprintf(buf, sizeof(buf), ", flat to %.1f m about %.1f m (water %.1f): x %.0f-%.0f, y %.0f-%.0f in the region",
                         ARENA_FLAT_M, h0, water, fx0 - o.mdV[VX], fx1 - o.mdV[VX], fy0 - o.mdV[VY], fy1 - o.mdV[VY]);
                what += buf;
            }
            say("arena: " + what + ", " + std::to_string(S32(sArenaX1 - sArenaX0)) + " x "
                + std::to_string(S32(sArenaY1 - sArenaY0)) + " m");
        }
        x0 = sArenaX0;
        x1 = sArenaX1;
        y0 = sArenaY0;
        y1 = sArenaY1;
    }

    void buildArenaCourse(const std::string& name, F64 x, F64 y, F64 z)
    {
        sLapRead = true;
        sLap.clear();
        sLapS.clear();
        sLapLoop = true;
        sLapArena = true;
        F64 x0, x1, y0, y1;
        arenaBox(x, y, x0, x1, y0, y1);
        const F64 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        const bool along_x = x1 - x0 >= y1 - y0;
        const F64 hl = std::max(5.0, (along_x ? x1 - x0 : y1 - y0) / 2 - ARENA_MARGIN_M);     // half length
        const F64 hw = std::max(5.0, (along_x ? y1 - y0 : x1 - x0) / 2 - ARENA_MARGIN_M);     // half width
        auto put = [&](F64 l, F64 w)    // track frame (along the long axis, left) to world
        {
            const F64 wx = along_x ? cx + l : cx - w, wy = along_x ? cy + w : cy + l;
            sLap.push_back({ wx, wy, z });
            sLapS.push_back(sLapS.empty() ? 0.0
                                          : sLapS.back() + std::hypot(wx - sLap[sLap.size() - 2][0], wy - sLap[sLap.size() - 2][1]));
        };
        const CourseShape c = courseShape(name);
        std::string said;
        if (c.kind == "circle")
        {
            const F64 r = std::min(c.r, hw);
            for (F64 a = 0; a < 2 * PI - 1e-9; a += 1.0 / r)
            {
                put(r * std::sin(a), r * std::cos(a));      // clockwise seen from above
            }
            said = "circle r " + std::to_string(S32(r)) + " m";
        }
        else if (c.kind == "figure8")
        {
            const F64 r = std::min({ c.r, hl / 2, hw });
            for (F64 a = 0; a < 2 * PI - 1e-9; a += 1.0 / r)
            {
                put(r - r * std::cos(a), -r * std::sin(a));     // the right-hand loop, clockwise, from the crossing
            }
            for (F64 a = 0; a < 2 * PI - 1e-9; a += 1.0 / r)
            {
                put(-r + r * std::cos(a), -r * std::sin(a));    // the left-hand one, anticlockwise, back to it
            }
            said = "figure 8, loops r " + std::to_string(S32(r)) + " m";
        }
        else
        {
            // a stadium: straights along the long axis, half circles at the ends; the slalom's sine on the straights
            const bool slalom = c.kind == "slalom";
            const F64 amp = slalom ? std::min(c.amp, hw / 4) : 0.0;
            const F64 r = std::max(6.0, std::min(hw - amp, hl / 2));
            F64 half = hl - r;      // the straights' half length
            const F64 wave = slalom ? std::max(8.0, c.wave) : 1.0;
            if (slalom)
            {
                half = std::max(wave / 2, std::floor(2 * half / wave) * wave / 2);      // whole waves, so it joins
            }
            for (F64 l = -half; l < half; l += 1.0)        // the near straight, left to right along +l, at w = -r
            {
                put(l, -r + (slalom ? amp * std::sin(2 * PI * (l + half) / wave) : 0.0));
            }
            for (F64 a = 0; a < PI - 1e-9; a += 1.0 / r)    // the far end, round anticlockwise
            {
                put(half + r * std::sin(a), -r * std::cos(a));
            }
            for (F64 l = half; l > -half; l -= 1.0)        // the far straight, back
            {
                put(l, r - (slalom ? amp * std::sin(2 * PI * (half - l) / wave) : 0.0));
            }
            for (F64 a = 0; a < PI - 1e-9; a += 1.0 / r)    // the near end
            {
                put(-half - r * std::sin(a), r * std::cos(a));
            }
            said = std::string(slalom ? "slalom oval" : "oval") + ", ends r " + std::to_string(S32(r)) + " m, straights "
                   + std::to_string(S32(2 * half)) + " m" + (slalom ? ", " + std::to_string(S32(amp)) + " m either side every "
                   + std::to_string(S32(wave)) + " m" : std::string());
        }
        say("arena track '" + name + "': " + said + ", " + std::to_string(S32(sLapS.empty() ? 0 : sLapS.back())) + " m a lap");
    }

    std::vector<LanePoint> recordedCourse(F64 x, F64 y, F64 v)
    {
        if (!sLapRead)
        {
            sLapRead = true;
            llifstream in(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-course.json").c_str());
            std::stringstream text;
            text << in.rdbuf();
            LLSD d;
            if (in.is_open() && LlsdFromJsonString(text.str(), d) && d["points"].isArray())
            {
                for (const LLSD& q : llsd::inArray(d["points"]))
                {
                    sLap.push_back({ q[0].asReal(), q[1].asReal(), q[2].asReal() });
                    sLapS.push_back(sLapS.empty() ? 0.0
                                                  : sLapS.back() + std::hypot(sLap.back()[0] - sLap[sLap.size() - 2][0],
                                                                              sLap.back()[1] - sLap[sLap.size() - 2][1]));
                }
                sLapLoop = d["loop"].asBoolean() && sLap.size() > 2;
                sLapArena = false;
            }
            say(sLap.size() < 2 ? std::string("no recorded course (sk-road-course.json)")
                                : "recorded course: " + std::to_string(S32(sLapS.back())) + " m" + (sLapLoop ? ", a loop" : ""));
        }
        if (sLap.size() < 2)
        {
            return {};
        }
        const S32 n = S32(sLap.size());
        auto wrapI = [&](S32 i) { return sLapLoop ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); };
        // the point nearest the car: near the last one once on the course (round the join, on a loop), anywhere
        // the first time
        S32 lo = 0, hi = n - 1;
        if (sCourseS >= 0)
        {
            const S32 at = S32(std::lower_bound(sLapS.begin(), sLapS.end(), sCourseS) - sLapS.begin());
            lo = sLapLoop ? at - 30 : std::max(0, at - 30);
            hi = sLapLoop ? at + 60 : std::min(n - 1, at + 60);
        }
        S32 best = wrapI(lo);
        F64 best_d = 1e18;
        for (S32 k = lo; k <= hi; ++k)
        {
            const S32 i = wrapI(k);
            const F64 d = (sLap[i][0] - x) * (sLap[i][0] - x) + (sLap[i][1] - y) * (sLap[i][1] - y);
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        if (best_d > 20.0 * 20.0 && sLapArena)
        {
            // making its way to the track from elsewhere in the arena: aimed at the nearest point of it
        }
        else if (best_d > 20.0 * 20.0)
        {
            // off the course: found again anywhere, but only within 20 m of it (after the lap's end, found
            // "anywhere" was its start 200 m away, and the car set off cross-country for it)
            if (sCourseS >= 0)
            {
                sCourseS = -1;
                return recordedCourse(x, y, v);
            }
            say("off the recorded course");
            return {};
        }
        sCourseS = best_d > 20.0 * 20.0 && sLapArena ? -1.0 : sLapS[best];    // far: the whole track searched again
        std::vector<LanePoint> out;
        constexpr F64 LANE = 4.0;      // a lane's width, for the band and the overlay: the line is the lane's middle
        const F64 reach = lookAhead(v) + 10.0;
        F64 walked = 0, since = PATH_STEP;
        for (S32 k = best - 2; walked <= reach; ++k)
        {
            if (!sLapLoop && (k < 0 || k + 1 >= n))
            {
                if (k < 0)
                {
                    continue;
                }
                break;
            }
            const auto& a = sLap[wrapI(k)];
            const auto& b = sLap[wrapI(k + 1)];
            const F64 dx = b[0] - a[0], dy = b[1] - a[1];
            const F64 L = std::max(1e-6, std::hypot(dx, dy));
            if (k >= best)
            {
                walked += L;
            }
            if (since < PATH_STEP)
            {
                since += L;
                continue;       // one point every PATH_STEP
            }
            since = L;
            LanePoint lp;
            lp.x = a[0];
            lp.y = a[1];
            lp.z = a[2];
            lp.ax = dx / L;
            lp.ay = dy / L;
            lp.lane_w = LANE;
            lp.road_w = 2 * LANE;
            lp.lanes = 2;
            lp.left_x = lp.x - lp.ay * (LANE * 1.5);
            lp.left_y = lp.y + lp.ax * (LANE * 1.5);
            lp.right_x = lp.x + lp.ay * (LANE * 0.5);
            lp.right_y = lp.y - lp.ax * (LANE * 0.5);
            out.push_back(lp);
            if (out.size() > 4096)
            {
                break;
            }
        }
        if (out.size() < 2)
        {
            say("end of the recorded course");
            return {};
        }
        return out;
    }

    std::vector<LanePoint> testCourse(const std::string& name, F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v)
    {
        if (name == "lap")
        {
            return recordedCourse(x, y, v);
        }
        static LLCachedControl<bool> arena_on(gSavedSettings, "SKRoadAutopilotArena", true);
        if (arena_on)
        {
            if (!sLapRead)
            {
                buildArenaCourse(name, x, y, z);
            }
            return recordedCourse(x, y, v);
        }
        if (!sCourse.set || sCourse.name != name)
        {
            sCourse = { name, x, y, z, hx, hy, true };
            sCourseS = -1;
            say("test course '" + name + "' laid out from here");
        }
        const F64 ax = sCourse.hx, ay = sCourse.hy, bx = -ay, by = ax;     // the course's frame: along, left
        auto world = [&](F64 lx, F64 ly, F64& wx, F64& wy)
        {
            wx = sCourse.x + ax * lx + bx * ly;
            wy = sCourse.y + ay * lx + by * ly;
        };
        // the course's arc length nearest the car, searched over the whole course every 1 m (cheap: a few km)
        const CourseShape shape = courseShape(name);
        const F64 total = shape.kind == "slalom"   ? 10 * shape.wave
                          : shape.kind == "circle" ? 2 * PI * shape.r * 4
                          : shape.kind == "figure8" ? 2 * PI * shape.r * 8
                                                    : 500.0;
        // near where the car was on it last (so a figure 8's crossing does not jump loops), the whole course the
        // first time
        const bool first = sCourseS < 0;
        const F64 s_lo = first ? 0.0 : std::max(0.0, sCourseS - 30.0), s_hi = first ? total : std::min(total, sCourseS + 60.0);
        F64 best_s = s_lo, best_d = 1e18;
        for (F64 s = s_lo; s <= s_hi; s += 0.5)
        {
            F64 lx, ly, wx, wy;
            coursePoint(name, s, lx, ly);
            world(lx, ly, wx, wy);
            const F64 d = (wx - x) * (wx - x) + (wy - y) * (wy - y);
            if (d < best_d)
            {
                best_d = d;
                best_s = s;
            }
        }
        sCourseS = best_s;
        if (best_d > 20.0 * 20.0 || best_s >= total - lookAhead(v))
        {
            // far off the course, or at its end: laid out again from here (setting the same name again cannot be
            // seen, so this is how a course is begun afresh)
            sCourse.set = false;
            return testCourse(name, x, y, z, hx, hy, v);
        }
        std::vector<LanePoint> out;
        const F64 length = lookAhead(v) + 10.0;
        constexpr F64 W = 10.0;     // a two-lane road's width, for the overlay; the line is the lane itself
        for (F64 s = std::max(0.0, best_s - PATH_STEP); s <= std::min(total, best_s + length); s += PATH_STEP)
        {
            F64 lx0, ly0, lx1, ly1, wx, wy, wx1, wy1;
            coursePoint(name, s, lx0, ly0);
            coursePoint(name, s + 0.5, lx1, ly1);
            world(lx0, ly0, wx, wy);
            world(lx1, ly1, wx1, wy1);
            const F64 L = std::max(1e-6, std::hypot(wx1 - wx, wy1 - wy));
            LanePoint lp;
            lp.x = wx;
            lp.y = wy;
            lp.ax = (wx1 - wx) / L;
            lp.ay = (wy1 - wy) / L;
            lp.lane_w = W / 2;
            lp.road_w = W;
            lp.lanes = 2;
            lp.z = sCourse.z;
            // the line is the right lane's middle: the road's edges a quarter of the road to its left, three to its right
            lp.left_x = wx - lp.ay * (W * 0.75);
            lp.left_y = wy + lp.ax * (W * 0.75);
            lp.right_x = wx + lp.ay * (W * 0.25);
            lp.right_y = wy - lp.ax * (W * 0.25);
            out.push_back(lp);
        }
        return out;
    }

    // ---------------------------------------------------------------- the lane guard

    constexpr F64 LANE_GUARD_AHEAD_S = 0.8, LANE_GUARD_M = 0.8, LANE_GUARD_DEG_PER_M = 30.0, LANE_GUARD_MAX_DEG = 60.0;
    bool sLaneGuard = false;

    // The car's offset from the line (m, + right of it) `ahead_s` on at its speed and heading: its offset now and
    // its heading against the line where it passes.
    F64 predictedOffset(const LLVector3d& pos, F64 yaw, F64 speed, F64 ahead_s)
    {
        const F64 px = pos.mdV[VX] + std::cos(yaw) * speed * ahead_s, py = pos.mdV[VY] + std::sin(yaw) * speed * ahead_s;
        F64 best = 1e18, side = 0;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            const LanePoint& a = sPath[i];
            const LanePoint& b = sPath[i + 1];
            const F64 sx = b.x - a.x, sy = b.y - a.y, L2 = std::max(1e-9, sx * sx + sy * sy);
            const F64 f = std::clamp(((px - a.x) * sx + (py - a.y) * sy) / L2, 0.0, 1.0);
            const F64 dx = px - (a.x + f * sx), dy = py - (a.y + f * sy);
            const F64 d2 = dx * dx + dy * dy;
            if (d2 < best)
            {
                best = d2;
                side = (sx * dy - sy * dx) > 0 ? -std::sqrt(d2) : std::sqrt(d2);
            }
        }
        return side;
    }

    // ---------------------------------------------------------------- steering by tap rhythm (SKRoadAutopilotSteer "pwm")

    // The turn (deg) one tap of `frames` frames gives in a rhythm (each 0.1 s after the last), from the sweep
    // (Police Interceptor, Gears 1 and 2 alike, 2026-11-05, bench/sweep_1.json): a rhythm of 1, 2, 4, 8 or 16
    // frame taps holds 4.5, 13, 15.5, 19 or 24.5 deg/s, so a tap's share is that rate times its period. The
    // taps do not add up to their share of the key held (85 deg/s): each press starts the turn again. Past 16
    // frames, a hold: the turn builds after RAMP_DELAY_S toward HELD_RATE_DEG with RAMP_TAU_S (63 % in about a
    // second held, as measured), and carries on about half a second after release.
    constexpr F64 HELD_RATE_DEG = 85.0, RAMP_DELAY_S = 0.25, RAMP_TAU_S = 0.75, CARRY_S = 0.5;

    F64 sFrameDt = 1.0 / 60;    // the viewer's frame time, smoothed: a key changes only once a frame

    // A tap of `frames` (DT) as it will really be held and spaced at this frame rate: both rounded up to whole
    // viewer frames (at 20 fps a 2-frame tap is held 50 ms and its 0.1 s gap 100-150 ms).
    F64 heldFor(S32 frames) { return std::ceil(frames * DT / sFrameDt - 1e-6) * sFrameDt; }
    F64 gapFor() { return std::ceil(MIN_GAP / sFrameDt - 1e-6) * sFrameDt + sFrameDt / 2; }

    F64 tapTurn(S32 frames)
    {
        // the rhythm table is by the time held: at a low frame rate a tap is held longer than asked
        const S32 held_frames = S32(std::lround(heldFor(frames) / DT));
        if (held_frames != frames && held_frames > 0)
        {
            frames = held_frames;
        }
        if (frames <= 16)
        {
            static const F64 R[][2] = { { 0, 0.0 }, { 1, 4.5 }, { 2, 13.0 }, { 4, 15.5 }, { 8, 19.0 }, { 16, 24.5 } };
            F64 rate = 0;
            for (size_t i = 1; i < std::size(R); ++i)
            {
                if (frames <= R[i][0])
                {
                    const F64 f = (frames - R[i - 1][0]) / (R[i][0] - R[i - 1][0]);
                    rate = R[i - 1][1] + f * (R[i][1] - R[i - 1][1]);
                    break;
                }
            }
            return rate * (frames * DT + gapFor());
        }
        const F64 h = frames * DT;
        auto rate_at = [](F64 t) { return t <= RAMP_DELAY_S ? 0.0 : HELD_RATE_DEG * (1 - std::exp(-(t - RAMP_DELAY_S) / RAMP_TAU_S)); };
        F64 turn = 0;
        for (F64 t = 0; t < h; t += 0.01)
        {
            turn += rate_at(t) * 0.01;
        }
        return turn + rate_at(h) * CARRY_S;
    }

    constexpr F64 PWM_LAG_S = 0.3;              // s from a key to the turn it makes
    constexpr F64 PWM_RHYTHM_MAX_DEG = 24.0;    // deg/s: past this (the 16 frame rhythm's), long holds
    constexpr F64 PWM_TURN_BACK_DEG = 60.0;     // deg/s wanted with the aim behind the car
    constexpr F64 PWM_PULL_DEG = 3.0, PWM_PULL_MAX_DEG = 20.0;  // 1.5 and 12 left 0.35 m outside a 30 m circle
    constexpr F64 PWM_RELEASE_SHARE = 0.9;
    constexpr F64 PWM_WANT_MAX_DEG = 90.0;
    constexpr F64 PWM_BEND_OVER = 1.5, PWM_LATERAL_ACCEL = 2.0;     // the line's bend, times; m/s^2 of a lane change
    constexpr F64 PWM_HOLD_OVER_DEG = 6.0;      // deg/s past a tight bend's need a hold is kept to
    constexpr S32 PWM_HOLD_OPEN = 500;          // frames: a hold with no time limit (10 s), let go by the early release
    F64 sPwmPull = 0.0;
    F64 sLastErr = 0.0;         // last frame's distance off the line, + right of it
    constexpr S32 PWM_HOLDS[] = { 1, 2, 4, 8, 16, 25, 38 };      // frames: the sweep's rhythms, then holds
    F64 sPwmCredit = 0.0;       // deg of turn owed: the wanted rate integrated, less what each tap delivers
    F64 sPwmK = 1.0;            // the turn taps deliver against tapTurn's, learnt
    F64 sPwmPrevT = -1.0;
    F64 sPwmWant = 0.0, sPwmGot = 0.0;    // over the learning window: turn fired (deg, signed) and turn made
    std::deque<std::pair<F64, F64>> sPwmFired;      // (when, deg nominal, signed): turn fired, landing after the lag

    // Each frame: the yaw rate the line wants (pure pursuit from where the car will be once what was sent has
    // landed, to a point on the line a second or so ahead), integrated into turn owed; a tap fires once the turn
    // owed reaches what it gives, its length the shortest whose rhythm (tap and gap) reaches the rate wanted. A
    // gentle bend is short taps far apart, a tight one long taps close together: what Nexii does by hand, here
    // to the frame.
    // The line's mean signed curvature (rad per m, left +) over its first `reach` metres ahead of the car.
    F64 curvatureNear(F64 reach)
    {
        F64 walked = 0, turned = 0;
        for (size_t i = 0; i + 2 < sPath.size() && walked < std::max(4.0, reach); ++i)
        {
            const F64 h0 = std::atan2(sPath[i + 1].y - sPath[i].y, sPath[i + 1].x - sPath[i].x);
            const F64 h1 = std::atan2(sPath[i + 2].y - sPath[i + 1].y, sPath[i + 2].x - sPath[i + 1].x);
            turned += wrap(h1 - h0);
            walked += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
        }
        return walked > 0 ? turned / walked : 0.0;
    }

    // The yaw rate (deg/s, left positive) the line wants: pure pursuit from where the car will be once what was
    // sent has landed, to a point on the line a second or so ahead.
    F64 pwmWant(const LLVector3d& pos, F64 yaw, F64 speed)
    {
        // where the car will be after the lag, turning as it turns now, and the heading it will have once every
        // tap already sent has done turning it (the ledger's kernels, at the delivery learnt): aiming from the
        // heading now asked for the same turn again while the last taps were still landing, so an S-bend's taps
        // overshot each way
        // (the turn still to come, from the car's own turn rate and the half second it carries on, as the sweep
        // measured: the single-tap kernels' sum over every tap sent put far more turn to come than came, and the
        // car, sure it was already turning, let a 10 deg heading error run it off the road)
        const F64 pending = sYawRate * 180 / PI * CARRY_S;
        const F64 yp = yaw + sPwmK * pending * PI / 180;
        const F64 xp = pos.mdV[VX] + speed * PWM_LAG_S * std::cos(yaw + sYawRate * PWM_LAG_S / 2);
        const F64 ypos = pos.mdV[VY] + speed * PWM_LAG_S * std::sin(yaw + sYawRate * PWM_LAG_S / 2);
        // the line's point nearest that, then the aim a look-ahead further along it
        size_t at = 0;
        F64 best = 1e18;
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            const F64 d = (sPath[i].x - xp) * (sPath[i].x - xp) + (sPath[i].y - ypos) * (sPath[i].y - ypos);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        const F64 ld = std::clamp(speed * 1.2, 8.0, 30.0);
        F64 walked = 0, ax = sPath.back().x, ay = sPath.back().y;
        for (size_t i = at; i + 1 < sPath.size(); ++i)
        {
            const F64 seg = std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
            if (walked + seg >= ld)
            {
                const F64 f = (ld - walked) / std::max(1e-6, seg);
                ax = sPath[i].x + f * (sPath[i + 1].x - sPath[i].x);
                ay = sPath[i].y + f * (sPath[i + 1].y - sPath[i].y);
                break;
            }
            walked += seg;
        }
        const F64 alpha = wrap(std::atan2(ay - ypos, ax - xp) - yp);
        const F64 dist = std::max(1.0, std::hypot(ax - xp, ay - ypos));
        // past a right angle, the hardest turn toward it: pure pursuit's sine falls to nothing as the aim comes round
        // behind the car, so a car pointing away from its track (as a bench test began) never turned back
        const F64 want = std::fabs(alpha) > PI / 2 ? (alpha > 0 ? 1.0 : -1.0) * PWM_TURN_BACK_DEG
                                                   : 2.0 * speed * std::sin(alpha) / dist * 180 / PI;    // deg/s, left +
        // no more than the line's own bend over the look-ahead and a gentle lane change ask: a short sideways kink
        // in the line (a bad section, a seam) bends the mean curvature little, so it is eased through, not steered
        // into (a 2 m S at Mujoul swung the car hard left across the region edge); the lane guard and the pull for a
        // standing offset still come on top, and a car pointing away from its line is not held to it
        if (std::fabs(alpha) < PI / 2 && speed > 1.0)
        {
            const F64 bend = std::fabs(curvatureNear(ld)) * speed;      // rad/s the line's bend needs
            const F64 cap = (PWM_BEND_OVER * bend + PWM_LATERAL_ACCEL / speed) * 180 / PI;
            return std::clamp(want, -cap, cap);
        }
        return want;
    }

    std::pair<S32, S32> pwmSteer(const LLVector3d& pos, F64 yaw, F64 speed, F64 now, std::string& note)
    {
        const F64 dt = sPwmPrevT < 0 ? 0.0 : std::min(0.1, now - sPwmPrevT);
        sPwmPrevT = now;
        if (dt > 0)
        {
            sFrameDt = std::clamp(0.9 * sFrameDt + 0.1 * dt, 1.0 / 240, 0.1);
        }
        // capped: off a 2-point line pure pursuit asked 209 deg/s
        F64 want = std::clamp(pwmWant(pos, yaw, speed), -PWM_WANT_MAX_DEG, PWM_WANT_MAX_DEG);
        // and a slow pull back onto the line for an offset that stays: pure pursuit settles to one side of a circle
        // (2.3 m outside a 30 m figure 8); built from the distance off, PWM_PULL_DEG per m per s, held to
        // PWM_PULL_MAX_DEG, and let go of while the car is far off (making its way to the track)
        if (std::fabs(sLastErr) < 5.0)
        {
            sPwmPull = std::clamp(sPwmPull + PWM_PULL_DEG * sLastErr * dt, -PWM_PULL_MAX_DEG, PWM_PULL_MAX_DEG);
        }
        else
        {
            sPwmPull = 0;
        }
        want += sPwmPull;
        // the lane guard: where the car will be LANE_GUARD_AHEAD_S on, from its offset and its heading against the
        // line, more than LANE_GUARD_M off it: turned back toward it, the more the further (a 10 deg heading error
        // drifted the car 2.5 m to the road's edge at 10 m/s while the steering asked a few deg/s)
        {
            const F64 off = predictedOffset(pos, yaw, speed, LANE_GUARD_AHEAD_S);     // + right of the line
            if (std::fabs(off) > LANE_GUARD_M)
            {
                const F64 back = std::min(LANE_GUARD_MAX_DEG, LANE_GUARD_DEG_PER_M * (std::fabs(off) - LANE_GUARD_M) + 10.0);
                want += off > 0 ? back : -back;     // right of the line: turn left
                sLaneGuard = true;
            }
            else
            {
                sLaneGuard = false;
            }
        }
        // learn the taps' delivery: the turn made against the turn fired, once it has landed
        F64 fired = 0;
        for (const auto& [t, deg] : sPwmFired)
        {
            if (t < now - PWM_LAG_S && t >= now - PWM_LAG_S - dt)
            {
                fired += deg;
            }
        }
        while (!sPwmFired.empty() && sPwmFired.front().first < now - PWM_LAG_S - 1.0)
        {
            sPwmFired.pop_front();
        }
        sPwmWant += fired;
        sPwmGot += sYawRate * 180 / PI * dt;
        if (std::fabs(sPwmWant) > 30.0)
        {
            // k is not learnt any more: the rhythm table is the sweep's measurement, and k learnt against the
            // single-tap kernels climbed to 1.9 and left the car 2 m outside every circle
            sPwmWant = sPwmGot = 0;
        }
        // turn owed: the wanted rate integrated, dropped when the rate wanted turns the other way, and never
        // more than two long taps' worth
        if (want * sPwmCredit < 0)
        {
            sPwmCredit = 0;
        }
        sPwmCredit += want * dt;
        const F64 cap = 2.0 * sPwmK * tapTurn(PWM_HOLDS[std::size(PWM_HOLDS) - 1]);
        sPwmCredit = std::clamp(sPwmCredit, -cap, cap);
        if (sFrame - sLastRelease < MIN_GAP / DT)
        {
            return { 0, 0 };
        }
        // the shortest tap whose rhythm reaches the rate wanted. Short taps do not add up past about 15 deg/s:
        // in each gap the car's turn falls back to nothing and the next tap starts again from its delay (Nexii's
        // own bursts at half duty made 13-17 deg/s; his 1-1.5 s holds 38-48), so past PWM_RHYTHM_MAX_DEG the key
        // is held long, released only for the shortest gap, and a hairpin is near enough one long hold
        S32 h = PWM_HOLDS[std::size(PWM_HOLDS) - 1];
        for (S32 c : PWM_HOLDS)
        {
            if (std::fabs(want) > PWM_RHYTHM_MAX_DEG && c < 25)
            {
                continue;
            }
            // the one-frame tap's turn is a guess, so it is kept to the gentlest corrections: as the rhythm of a
            // bend it promised 17 deg/s and the car ran wide
            if (c == 1 && std::fabs(want) > 4.0)
            {
                continue;
            }
            if (sPwmK * tapTurn(c) / (heldFor(c) + gapFor()) >= std::fabs(want))
            {
                h = c;
                break;
            }
        }
        const F64 turn = sPwmK * tapTurn(h);
        if (std::fabs(want) > PWM_RHYTHM_MAX_DEG)
        {
            // past what any rhythm holds: the key held, with no time limit, until the line wants no more of it
            // (the early release below the steering): any gap at all keeps the car under 24 deg/s, held it builds
            // to 85 (the sweep), and holds cut at 0.76 s never let it
            const S32 d = want > 0 ? 1 : -1;
            sPwmCredit = 0;
            sPwmFired.push_back({ now, d * tapTurn(PWM_HOLD_OPEN) });
            char buf[96];
            snprintf(buf, sizeof(buf), "pwm %.1f/s held", want);
            note += note.empty() ? buf : std::string(", ") + buf;
            return { d, PWM_HOLD_OPEN };
        }
        if (std::fabs(sPwmCredit) < turn)
        {
            return { 0, 0 };
        }
        const S32 d = sPwmCredit > 0 ? 1 : -1;
        sPwmCredit = 0;     // the tap's turn is in the heading aimed from (pending) from now on: owed afresh
        sPwmFired.push_back({ now, d * tapTurn(h) });
        char buf[96];
        snprintf(buf, sizeof(buf), "pwm %.1f/s %d k %.2f", want, h, sPwmK);
        note += note.empty() ? buf : std::string(", ") + buf;
        return { d, h };
    }

    F64 sLiftUntil = -100.0, sLiftNext = -100.0;     // a lift off W for a bend (bendLift): until, next allowed

    // ---------------------------------------------------------------- the gears' top speeds

    // Each gear's top speed, measured while W is held: the speed once it has stopped rising (no more than
    // GEAR_FLAT_MPS2 over GEAR_FLAT_S), going nearly straight, on level road (the car rising or falling no more
    // than GEAR_LEVEL_M over the window, so a run downhill does not read high) and not just after a lift. Kept
    // per car in sk-road-car-gears.json (logs folder: {car: {gear: [m/s, ...]}}, the last GEAR_READINGS readings,
    // oldest first; a bare number, the file's old form, loads as one reading), the car being its name and creator
    // ("name @ creator"), the same for every rez of it; gear 0 while the car has not said its gear (it says it on
    // a shift: "Gear 3"). The top used is the readings' robust median (gearTopOf), so one outlier does not stick;
    // said in chat each time a gear's top is first found or moves by more than 0.3 m/s.
    // sk-road-gear-tops.json, from before, kept them per rezzed object: a rez's own entry there is adopted.
    constexpr F64 GEAR_FLAT_S = 1.5;
    constexpr F64 GEAR_FLAT_MPS2 = 0.15;
    constexpr F64 GEAR_LEVEL_M = 0.5;
    constexpr size_t GEAR_READINGS = 15;
    using GearTops = std::map<S32, F64>;
    using GearReadings = std::map<S32, std::vector<F64>>;
    std::map<std::string, GearReadings> sGearReadings;  // car -> gear -> readings, oldest first
    std::map<std::string, GearTops> sGearTops;          // car -> gear -> m/s, gearTopOf the readings
    std::map<std::string, GearReadings> sObjectTops;    // the old file's: rezzed object -> gear -> readings
    std::map<std::string, GearTops> sObjectTopsMedian;
    bool sGearTopsRead = false;
    std::string sCarKey;                            // sVehicle's car, empty while its name is not known
    LLUUID sCarKeyFor, sCarNameAsked;
    struct GearSample { F64 when, speed, z; };
    std::deque<GearSample> sGearSpeeds;             // over GEAR_FLAT_S, held and straight
    S32 sGearSpeedsGear = -99;

    std::string gearTopsPath() { return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-car-gears.json"); }

    F64 medianOf(std::vector<F64> v)
    {
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    }

    // one or two readings: their mean; more: their median, and from five on the median again of those within
    // 30 % of it
    F64 gearTopOf(const std::vector<F64>& r)
    {
        if (r.empty())
        {
            return 0.0;
        }
        if (r.size() <= 2)
        {
            return (r.front() + r.back()) / 2;
        }
        const F64 m = medianOf(r);
        if (r.size() < 5)
        {
            return m;
        }
        std::vector<F64> kept;
        for (F64 x : r)
        {
            if (std::fabs(x - m) <= 0.3 * m)
            {
                kept.push_back(x);
            }
        }
        return kept.empty() ? m : medianOf(kept);
    }

    GearTops topsOf(const GearReadings& g)
    {
        GearTops t;
        for (const auto& [gear, r] : g)
        {
            if (!r.empty())
            {
                t[gear] = gearTopOf(r);
            }
        }
        return t;
    }

    void readTops(const std::string& file, std::map<std::string, GearReadings>& into)
    {
        llifstream in(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, file).c_str());
        std::stringstream text;
        text << in.rdbuf();
        LLSD d;
        if (!in.is_open() || !LlsdFromJsonString(text.str(), d) || !d.isMap())
        {
            return;
        }
        for (const auto& [veh, gears] : llsd::inMap(d))
        {
            for (const auto& [g, top] : llsd::inMap(gears))
            {
                std::vector<F64>& r = into[veh][std::atoi(g.c_str())];
                if (top.isArray())
                {
                    for (const auto& x : llsd::inArray(top))
                    {
                        r.push_back(x.asReal());
                    }
                }
                else
                {
                    r.push_back(top.asReal());
                }
                if (r.size() > GEAR_READINGS)
                {
                    r.erase(r.begin(), r.end() - GEAR_READINGS);
                }
            }
        }
    }

    void readGearTops()
    {
        if (sGearTopsRead)
        {
            return;
        }
        sGearTopsRead = true;
        readTops("sk-road-car-gears.json", sGearReadings);
        readTops("sk-road-gear-tops.json", sObjectTops);
        for (const auto& [k, g] : sGearReadings)
        {
            sGearTops[k] = topsOf(g);
        }
        for (const auto& [k, g] : sObjectTops)
        {
            sObjectTopsMedian[k] = topsOf(g);
        }
    }

    void writeGearTops()
    {
        llofstream out(gearTopsPath().c_str(), std::ios::out | std::ios::trunc);
        out << "{";
        bool first = true;
        for (const auto& [veh, gears] : sGearReadings)
        {
            if (gears.empty())
            {
                continue;
            }
            out << (first ? "" : ",") << "\n " << LlsdToJson(LLSD(veh)) << ": {";
            first = false;
            bool fg = true;
            for (const auto& [g, r] : gears)
            {
                out << (fg ? "" : ", ") << "\"" << g << "\": [";
                for (size_t i = 0; i < r.size(); ++i)
                {
                    out << (i ? ", " : "") << std::fixed << std::setprecision(2) << r[i];
                }
                out << "]";
                fg = false;
            }
            out << "}";
        }
        out << "\n}\n";
    }

    std::string carName(const std::string& key)
    {
        const size_t at = key.find(" @ ");
        return at == std::string::npos ? key : key.substr(0, at);
    }

    // ---------------------------------------------------------------- the vehicle's profile

    // What the driver knows of a car beyond what it measures: how it shifts and says its gear. None ships with
    // the viewer; they are read from sk-road-vehicles.json in the user's settings, keyed by "name @ creator" or
    // the name alone:
    //   {"Police Interceptor @ <creator>": {"gears": "chat", "gear_word": "Gear ", "reverse_word": "Reverse"}}
    // "gears": "chat" is a car that shifts on Page Up/Down (E/C) and says its gear in chat, Reverse a gear below
    // Gear 1. A car with no profile is driven without gears: W ahead, S to brake and back.
    VehicleProfile sProfile;
    std::string sProfileFor = "\n";                 // the car key sProfile was looked up for

    LLSD readProfiles()
    {
        static LLSD profiles;
        static bool read = false;
        if (!read)
        {
            read = true;
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "sk-road-vehicles.json");
            llifstream in(path.c_str());
            std::stringstream text;
            if (in.is_open())
            {
                text << in.rdbuf();
            }
            if (!in.is_open() || !LlsdFromJsonString(text.str(), profiles) || !profiles.isMap())
            {
                profiles = LLSD::emptyMap();
            }
            LL_INFOS("SKRoadDriver") << "vehicle profiles: " << profiles.size() << " from " << path << LL_ENDL;
        }
        return profiles;
    }
}

const skroad::drive::VehicleProfile& skroad::drive::vehicleProfile()
{
    if (sProfileFor != sCarKey)
    {
        sProfileFor = sCarKey;
        sProfile = VehicleProfile();
        const LLSD profiles = readProfiles();
        const LLSD p = profiles.has(sCarKey) ? profiles[sCarKey] : profiles[carName(sCarKey)];
        if (!sCarKey.empty() && p.isMap())
        {
            sProfile.key = sCarKey;
            sProfile.chat_gears = p["gears"].asString() == "chat";
            if (p.has("gear_word"))
            {
                sProfile.gear_word = p["gear_word"].asString();
            }
            if (p.has("reverse_word"))
            {
                sProfile.reverse_word = p["reverse_word"].asString();
            }
        }
        if (!sCarKey.empty())
        {
            LL_INFOS("SKRoadDriver") << "vehicle " << sCarKey << ": " << (sProfile.chat_gears ? "gears said in chat" : "no profile, no gears")
                                     << LL_ENDL;
        }
    }
    return sProfile;
}

U32 skroad::drive::shiftKey(S32 dir)
{
    if (dir == 0 || !vehicleProfile().chat_gears)
    {
        return 0;
    }
    return dir > 0 ? AGENT_CONTROL_UP_POS : AGENT_CONTROL_UP_NEG;
}

namespace skroad::drive
{

    // The gear tops of the car driven now, from the first frame: by its name and creator once the simulator has
    // said them (asked for here), and until then by the rezzed object. On first naming a car, the best per gear
    // of this rez's old entry and of every car of the same name (other creators, and the names alone that the
    // old entries were moved to) is adopted, no gear's top above the next gear's.
    GearTops& carGears()
    {
        readGearTops();
        if (sCarKeyFor != sVehicle || sCarKey.empty())
        {
            sCarKeyFor = sVehicle;
            sCarKey.clear();
            const ALObjectPropertiesCache::ServerProps* p = sVehicle.notNull() ? ALObjectPropertiesCache::instance().get(sVehicle) : nullptr;
            if (!p)
            {
                LLViewerObject* o = sVehicle.notNull() ? gObjectList.findObject(sVehicle) : nullptr;
                if (o && sCarNameAsked != sVehicle)
                {
                    sCarNameAsked = sVehicle;
                    LLSelectMgr::getInstance()->requestObjectPropertiesFamily(o);
                }
            }
            else if (!p->mName.empty())
            {
                sCarKey = p->mCreatorId.isNull() ? p->mName : p->mName + " @ " + p->mCreatorId.asString();
                GearReadings& readings = sGearReadings[sCarKey];
                const GearReadings had = readings;
                // a gear this car has no readings for takes those of the other entries
                auto adopt = [&](const GearReadings& from)
                {
                    for (const auto& [g, r] : from)
                    {
                        if (g > 0 && !r.empty() && (!had.count(g) || had.at(g).empty()))
                        {
                            std::vector<F64>& mineR = readings[g];
                            mineR.insert(mineR.end(), r.begin(), r.end());
                            if (mineR.size() > GEAR_READINGS)
                            {
                                mineR.erase(mineR.begin(), mineR.end() - GEAR_READINGS);
                            }
                        }
                    }
                };
                if (auto it = sObjectTops.find(sVehicle.asString()); it != sObjectTops.end())
                {
                    adopt(it->second);
                }
                for (const auto& [k, g] : sGearReadings)
                {
                    if (k != sCarKey && carName(k) == p->mName)
                    {
                        adopt(g);
                    }
                }
                GearTops& mine = sGearTops[sCarKey];
                mine = topsOf(readings);
                // a reading taken through a shift or a lift can stand above the next gear's
                for (auto it = mine.rbegin(); it != mine.rend(); ++it)
                {
                    auto above = mine.find(it->first + 1);
                    if (above != mine.end() && it->second > above->second)
                    {
                        it->second = above->second;
                    }
                }
                if (readings != had)
                {
                    writeGearTops();
                }
            }
        }
        return sCarKey.empty() ? sObjectTopsMedian[sVehicle.asString()] : sGearTops[sCarKey];
    }

    GearReadings& carReadings()
    {
        return sCarKey.empty() ? sObjectTops[sVehicle.asString()] : sGearReadings[sCarKey];
    }

    void measureGearTop(F64 speed, F64 now)
    {
        readGearTops();
        if (sGearSpeedsGear != sGear || now < sLiftNext || std::fabs(sYawRate) * 180 / PI > 5.0 || speed < 1.0)
        {
            sGearSpeeds.clear();        // a shift, a lift or a bend: begun again
            sGearSpeedsGear = sGear;
            return;
        }
        sGearSpeeds.push_back({ now, speed, gAgent.getPositionGlobal().mdV[VZ] });
        while (sGearSpeeds.size() > 1 && sGearSpeeds.front().when < now - GEAR_FLAT_S)
        {
            sGearSpeeds.pop_front();
        }
        const F64 span = now - sGearSpeeds.front().when;
        if (span < GEAR_FLAT_S * 0.95)
        {
            return;
        }
        // level road only: a run downhill reads high, uphill low
        F64 zlo = sGearSpeeds.front().z, zhi = zlo;
        for (const auto& q : sGearSpeeds)
        {
            zlo = std::min(zlo, q.z);
            zhi = std::max(zhi, q.z);
        }
        if (zhi - zlo >= GEAR_LEVEL_M)
        {
            return;
        }
        // the window's halves against each other: the speed seen wobbles a metre a second either way frame to
        // frame (a range over the window never settled in a whole lap), so only a rise in the mean counts
        F64 first = 0, second = 0;
        S32 nf = 0, ns = 0;
        const F64 mid = now - GEAR_FLAT_S / 2;
        for (const auto& q : sGearSpeeds)
        {
            (q.when < mid ? first : second) += q.speed;
            ++(q.when < mid ? nf : ns);
        }
        if (!nf || !ns)
        {
            return;
        }
        first /= nf;
        second /= ns;
        if (second - first > GEAR_FLAT_MPS2 * GEAR_FLAT_S / 2)    // still rising
        {
            return;
        }
        const F64 top = (first * nf + second * ns) / (nf + ns);
        GearTops& gears = carGears();
        std::vector<F64>& r = carReadings()[sGear];
        r.push_back(top);
        if (r.size() > GEAR_READINGS)
        {
            r.erase(r.begin(), r.end() - GEAR_READINGS);
        }
        writeGearTops();
        auto it = gears.find(sGear);
        const F64 was = it == gears.end() ? -1.0 : it->second;
        gears[sGear] = gearTopOf(r);
        if (was < 0 || std::fabs(was - gears[sGear]) > 0.3)
        {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     was < 0 ? "%s %d tops out at %.1f m/s (median of %d, this run %.1f)"
                             : "%s %d tops out at %.1f m/s (median of %d, this run %.1f; was %.1f)",
                     sGear > 0 ? "gear" : "gear (not said yet)", sGear, gears[sGear], (S32)r.size(), top, was);
            say(buf);
        }
        sGearSpeeds.clear();    // the next reading a full window later
    }

    // ---------------------------------------------------------------- lifting off for a tight bend

    // The car slows hard off W (about 10 m/s^2) and turns hard on a long hold, so a bend is taken as Nexii takes
    // one: W let go for a moment (LIFT_S) and held again for at least LIFT_GAP_S (he held it about 75 % of the
    // time round the lap's tight bends), only while the sharpest bend within the next LIFT_AHEAD_S asks a turn
    // rate (speed / radius) past LIFT_RATE_DEG, short of what a long hold makes (40-70 deg/s).
    constexpr F64 LIFT_RATE_DEG = 35.0;
    constexpr F64 LIFT_AHEAD_S = 1.5;
    constexpr F64 LIFT_S = 0.1;
    constexpr F64 LIFT_GAP_S = 0.4;

    // The sharpest turn (rad per metre) along the line over the next `reach` metres, from heading changes
    // between its points.
    F64 sharpestAhead(F64 reach)
    {
        F64 walked = 0, sharpest = 0;
        for (size_t i = 0; i + 2 < sPath.size() && walked < reach; ++i)
        {
            const F64 h0 = std::atan2(sPath[i + 1].y - sPath[i].y, sPath[i + 1].x - sPath[i].x);
            const F64 h1 = std::atan2(sPath[i + 2].y - sPath[i + 1].y, sPath[i + 2].x - sPath[i + 1].x);
            const F64 len = std::hypot(sPath[i + 2].x - sPath[i].x, sPath[i + 2].y - sPath[i].y) / 2;
            if (len > 0.5)
            {
                sharpest = std::max(sharpest, std::fabs(wrap(h1 - h0)) / len);
            }
            walked += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
        }
        return sharpest;
    }

    // The same over chords of at least CURVE_BASE_M, for the gear: the line's points sit a metre or less apart,
    // and a few centimetres of jitter between them read on a straight as a bend Gear 2 could not take
    // (sk-road-autopilot.csv, 2026-11-07: a shift down and up again every second on straight road).
    constexpr F64 CURVE_BASE_M = 5.0;
    F64 sharpestAheadSmooth(F64 reach)
    {
        const size_t n = sPath.size();
        if (n < 3)
        {
            return 0.0;
        }
        std::vector<F64> arc(n, 0.0);
        for (size_t i = 1; i < n; ++i)
        {
            arc[i] = arc[i - 1] + std::hypot(sPath[i].x - sPath[i - 1].x, sPath[i].y - sPath[i - 1].y);
        }
        F64 sharpest = 0;
        size_t j = 0, k = 0;
        for (size_t i = 0; i < n && arc[i] < reach; ++i)
        {
            j = std::max(j, i + 1);
            while (j < n && arc[j] - arc[i] < CURVE_BASE_M)
            {
                ++j;
            }
            k = std::max(k, j + 1);
            while (k < n && arc[k] - arc[j] < CURVE_BASE_M)
            {
                ++k;
            }
            if (k >= n)
            {
                break;
            }
            const F64 h0 = std::atan2(sPath[j].y - sPath[i].y, sPath[j].x - sPath[i].x);
            const F64 h1 = std::atan2(sPath[k].y - sPath[j].y, sPath[k].x - sPath[j].x);
            sharpest = std::max(sharpest, std::fabs(wrap(h1 - h0)) / ((arc[k] - arc[i]) / 2));
        }
        return sharpest;
    }

    // The line's grade from its start (a few metres behind the car) over GRADE_AHEAD_M: its rise over its run; 0 on a
    // line shorter than GRADE_MIN_M. Up a slope of the road that goes on, W is kept (Nexii, Nangrim: "why brake while
    // going up a slope? made it very hard to drive up it"): no lift for a bend, no slowing for a short line at a crest,
    // and off an edge with no drop beside W is let go, never braked.
    constexpr F64 GRADE_AHEAD_M = 25.0, GRADE_MIN_M = 8.0, UPHILL_GRADE = 0.04;
    F64 gradeAhead()
    {
        F64 run = 0;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            run += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
            if (run >= GRADE_AHEAD_M || i + 2 == sPath.size())
            {
                return run >= GRADE_MIN_M ? (sPath[i + 1].z - sPath[0].z) / run : 0.0;
            }
        }
        return 0.0;
    }

    // The line's height at a point: on the segment nearest it, between its ends; fallback with no line.
    F64 lineZAt(F64 px, F64 py, F64 fallback)
    {
        F64 best = 1e18, z = fallback;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            const LanePoint &a = sPath[i], &b = sPath[i + 1];
            const F64 ex = b.x - a.x, ey = b.y - a.y, L2 = ex * ex + ey * ey;
            const F64 t = L2 > 1e-9 ? std::clamp(((px - a.x) * ex + (py - a.y) * ey) / L2, 0.0, 1.0) : 0.0;
            const F64 d = std::hypot(a.x + ex * t - px, a.y + ey * t - py);
            if (d < best)
            {
                best = d;
                z = a.z + (b.z - a.z) * t;
            }
        }
        return z;
    }

    // The speed the line allows now for the turns ahead, a junction's above all: at each point the speed that takes
    // its bend at TURN_LAT_ACCEL (v = sqrt(a r)), plus what braking at TURN_PLAN_DECEL gains on the way there after
    // the throttle's lag. Only bends tighter than TURN_R_MAX_M count; the gears handle the road's own curves. At a
    // T a resident comes down to a turning speed before the mouth (Nexii: a side road's car shot straight across
    // a main road into a wall with the line already turned onto it).
    constexpr F64 TURN_LAT_ACCEL = 2.5, TURN_PLAN_DECEL = 4.0, TURN_R_MAX_M = 30.0, TURN_BASE_M = 3.0;
    constexpr F64 JUNCTION_TURN_MPS = 5.0;      // through a junction's turn, whatever its line's radius reads
    F64 sTurnAllowR = 0, sTurnAllowAt = 0;      // the bend that set the allowance: its radius and distance
    F64 turnAllowance(const LLVector3d& pos, F64 speed, F64 dirx, F64 diry)
    {
        const size_t n = sPath.size();
        F64 allow = 1e9;
        sTurnAllowR = 0;
        sTurnAllowAt = 0;
        auto limit = [&](F64 v_turn, F64 at, F64 r)
        {
            const F64 room = std::max(0.0, at - speed * THROTTLE_LAG_S - 2.0);
            const F64 v = std::sqrt(v_turn * v_turn + 2 * TURN_PLAN_DECEL * room);
            if (v < allow)
            {
                allow = v;
                sTurnAllowR = r;
                sTurnAllowAt = at;
            }
        };
        if (n >= 3)
        {
            std::vector<F64> arc(n, 0.0);
            for (size_t i = 1; i < n; ++i)
            {
                arc[i] = arc[i - 1] + std::hypot(sPath[i].x - sPath[i - 1].x, sPath[i].y - sPath[i - 1].y);
            }
            size_t j = 0, k = 0;
            for (size_t i = 0; i < n; ++i)
            {
                j = std::max(j, i + 1);
                while (j < n && arc[j] - arc[i] < TURN_BASE_M)
                {
                    ++j;
                }
                k = std::max(k, j + 1);
                while (k < n && arc[k] - arc[j] < TURN_BASE_M)
                {
                    ++k;
                }
                if (k >= n)
                {
                    break;
                }
                const F64 at = (sPath[j].x - pos.mdV[VX]) * dirx + (sPath[j].y - pos.mdV[VY]) * diry;
                if (at < -2.0)
                {
                    continue;
                }
                const F64 h0 = std::atan2(sPath[j].y - sPath[i].y, sPath[j].x - sPath[i].x);
                const F64 h1 = std::atan2(sPath[k].y - sPath[j].y, sPath[k].x - sPath[j].x);
                const F64 curv = std::fabs(wrap(h1 - h0)) / std::max(1e-3, (arc[k] - arc[i]) / 2);
                if (curv * TURN_R_MAX_M > 1.0)
                {
                    limit(std::sqrt(TURN_LAT_ACCEL / curv), at, 1.0 / curv);
                }
            }
        }
        // a junction ahead whose chosen way turns: through it at a turning speed, the line's radius or not
        for (const Junction& jn : sView.junctions)
        {
            if (jn.chosen < 0 || jn.chosen >= S32(jn.branches.size()) || std::fabs(jn.branches[jn.chosen].turn_deg) < 30.0)
            {
                continue;
            }
            const F64 at = (jn.x - pos.mdV[VX]) * dirx + (jn.y - pos.mdV[VY]) * diry;
            if (at > -2.0)
            {
                limit(JUNCTION_TURN_MPS, at, 0.0);
            }
        }
        return allow;
    }

    // Said once a junction, on the way to it: its kind, ways and choice, the speed and the turning speed.
    F64 sJunctionSaidX = 1e18, sJunctionSaidY = 1e18;
    void noteJunctionApproach(const LLVector3d& pos, F64 speed, F64 allow, F64 dirx, F64 diry)
    {
        for (const Junction& jn : sView.junctions)
        {
            const F64 at = (jn.x - pos.mdV[VX]) * dirx + (jn.y - pos.mdV[VY]) * diry;
            if (at < 0 || at > std::max(40.0, speed * 4) || std::hypot(jn.x - sJunctionSaidX, jn.y - sJunctionSaidY) < 15.0)
            {
                continue;
            }
            sJunctionSaidX = jn.x;
            sJunctionSaidY = jn.y;
            const bool straight_on = std::any_of(jn.branches.begin(), jn.branches.end(), [](const Branch& b) { return std::fabs(b.turn_deg) < 30.0; });
            std::string ways;
            for (const Branch& b : jn.branches)
            {
                ways += (ways.empty() ? "" : ", ") + std::string(b.kind()) + llformat(" %.0f", b.turn_deg);
            }
            LL_INFOS("SKRoadDriver") << "junction approach: " << (straight_on ? (jn.branches.size() > 2 ? "cross" : "fork") : "T")
                                     << " at " << S32(at) << " m (" << ways << "), taking "
                                     << (jn.chosen >= 0 && jn.chosen < S32(jn.branches.size()) ? jn.branches[jn.chosen].kind() : "none")
                                     << ", speed " << llformat("%.1f", speed) << ", turning speed "
                                     << (allow < 1e8 ? llformat("%.1f", allow) : std::string("free")) << LL_ENDL;
            break;
        }
    }

    bool bendLift(F64 speed, F64 now)
    {
        if (now < sLiftUntil)
        {
            return true;
        }
        if (now < sLiftNext || sPath.size() < 3 || speed < 8.0     // in Gear 1 (6.5 m/s) a lift only stalls it
            || gradeAhead() > UPHILL_GRADE)                         // uphill a lift bleeds the speed the hill takes
        {
            return false;
        }
        const F64 sharpest = sharpestAhead(speed * LIFT_AHEAD_S);
        if (speed * sharpest * 180 / PI <= LIFT_RATE_DEG)
        {
            return false;
        }
        sLiftUntil = now + LIFT_S;
        sLiftNext = sLiftUntil + LIFT_GAP_S;
        return true;
    }

    // ---------------------------------------------------------------- picking the gear

    // The gear is the autopilot's: the highest whose top speed takes the sharpest bend within GEAR_AHEAD_S
    // (at the gear's top) at no more than GEAR_RATE_DEG, a margin under the lift's, so the gear does the slowing
    // and the lift is left for what the gear cannot. A gear whose top is not yet measured is tried once the one
    // below it is measured and the road allows more; the gear is known from the car's own chat ("Gear 3",
    // noteChat), and while it is not, the first shift is down, which makes the car say it. Never into Reverse
    // (below Gear 1): from it, up. A shift up the car does not answer with a new gear is the top gear.
    constexpr F64 GEAR_RATE_DEG = 20.0;         // 28 took the lap in Gear 6-7 (31 m/s), 10 m off in the bends
    constexpr F64 GEAR_AHEAD_S = 6.0;           // 3 s let it reach a bend in Gear 4 at 24 m/s, a shift down every
                                                // 1.5 s too slow to get out of it
    constexpr F64 SHIFT_DOWN_GAP_S = 0.5;       // s between shifts down (up: SHIFT_GAP_S)
    constexpr F64 GEAR_UP_ERR_M = 2.0;          // no shift up while this far off the line
    S32 sBenchGear = 0;                         // the gear the bench holds (0: the autopilot picks)
    S32 sGearMax = 99;          // the top gear, once a shift up found no higher
    S32 sGearBefore = 0;        // the gear when the last shift up was sent
    constexpr F64 GEAR_UP_FRAC = 0.75;          // up only with this share of GEAR_RATE_DEG to spare
    constexpr F64 GEAR_UP_CLEAR_S = 1.0;        // and the road clear for the next gear this long
    constexpr F64 GEAR_MIN_S = 3.0;             // after a shift up, down only for a bend GEAR_DOWN_URGENT over the rate
    constexpr F64 GEAR_DOWN_URGENT = 1.4;
    F64 sLastUpShift = -100.0;
    F64 sUpClearSince = 0.0;
    F64 sEdgeDownShift = -100.0;                // the edge guard's last shift down: no shift up straight after

    void pickGear(F64 speed, F64 now, bool road, U32& keys, std::string& note)
    {
        static LLCachedControl<S32> max_gear_c(gSavedSettings, "SKRoadAutopilotMaxGear", 4);
        const S32 max_gear = S32(max_gear_c) > 0 ? S32(max_gear_c) : 99;
        if (now < sShiftEnd)
        {
            keys |= shiftKey(sShiftDir);
            return;
        }
        if (!road || now - sLastShift < SHIFT_DOWN_GAP_S)
        {
            return;
        }
        if (sBenchGear > 0)
        {
            // the bench holds one gear: into it and nothing else
            sShiftDir = sGear == sBenchGear ? 0 : (sGear <= 0 ? (sGear < 0 ? 1 : -1) : (sGear < sBenchGear ? 1 : -1));
            if (sShiftDir)
            {
                sGearBefore = sGear;
                sLastShift = now;
                sShiftEnd = now + SHIFT_HOLD_S;
                keys |= shiftKey(sShiftDir);
            }
            return;
        }
        if (sShiftDir > 0 && sGear == sGearBefore && sGear > 0 && now - sLastShift >= SHIFT_GAP_S)
        {
            sGearMax = sGear;       // shifted up and still in it: the top gear
        }
        sShiftDir = 0;
        const GearTops& tops = carGears();
        auto top = [&](S32 g) -> F64
        {
            auto it = tops.find(g);
            return it == tops.end() ? -1.0 : it->second;
        };
        if (sGear == 0)
        {
            sShiftDir = -1;         // the gear unknown: down, and the car says which
        }
        else if (sGear < 0)
        {
            sShiftDir = 1;          // out of Reverse
        }
        else
        {
            // the fastest the bends ahead allow, at each gear's top in turn
            // with hysteresis: up only where the next gear takes the bends at GEAR_UP_FRAC of the rate, and has
            // done so for GEAR_UP_CLEAR_S; down where this gear's top passes the rate, or, within GEAR_MIN_S of a
            // shift up, passes it by GEAR_DOWN_URGENT
            auto rate = [&](F64 v) { return v * sharpestAheadSmooth(std::max(30.0, v * GEAR_AHEAD_S)) * 180 / PI; };
            auto allows = [&](F64 v) { return rate(v) <= GEAR_RATE_DEG * GEAR_UP_FRAC; };
            const F64 here = top(sGear), next = top(sGear + 1);
            const F64 here_rate = rate(here > 0 ? here : speed);
            const bool settled = now - sLastUpShift >= GEAR_MIN_S;
            const bool up_ok = next > 0 ? allows(next) : allows(here * 1.5);
            if (!up_ok)
            {
                sUpClearSince = now;
            }
            // down whenever the gear's top (or, not yet measured, the speed now) is too fast for what is ahead: a
            // lower gear is slower, measured or not
            if (sGear > 1 && here_rate > GEAR_RATE_DEG * (settled ? 1.0 : GEAR_DOWN_URGENT))
            {
                sShiftDir = -1;
            }
            else if (sGear < std::min(sGearMax, max_gear) && here > 0 && now - sLastShift >= SHIFT_GAP_S
                     && up_ok && now - sUpClearSince >= GEAR_UP_CLEAR_S && now - sEdgeDownShift >= GEAR_UP_CLEAR_S * 2
                     && std::fabs(sLastErr) < GEAR_UP_ERR_M && now >= sLiftNext
                     && lineSeen() >= std::max(SIGHT_MIN_M, (next > 0 ? next : here * 1.5) * SIGHT_LIFT_S))
            {
                // and only with the road seen far enough for the next gear's speed: on a line of a few metres no
                // bend was in sight, and it went up to Gear 3 into one
                sShiftDir = 1;
            }
            else if (sGear > max_gear)
            {
                sShiftDir = -1;
            }
        }
        if (sShiftDir)
        {
            sGearBefore = sGear;
            sLastShift = now;
            if (sShiftDir > 0)
            {
                sLastUpShift = now;
            }
            sShiftEnd = now + SHIFT_HOLD_S;
            keys |= shiftKey(sShiftDir);
            const char* what = sShiftDir > 0 ? "shift up" : "shift down";
            note += note.empty() ? what : std::string(", ") + what;
        }
    }

    // ---------------------------------------------------------------- the steering sweep

    // SKRoadAutopilotSweep = a gear (1-9; 0 off): on open flat land (a sandbox), no road needed. The car is put
    // in that gear, W held throughout, and driven in circles at one fixed tap rhythm after another, each
    // SWEEP_ON_S to the left, SWEEP_OFF_S straight, the same to the right, SWEEP_OFF_S straight: a tap of
    // SWEEP_HOLDS frames every SWEEP_GAPS seconds, then the key held throughout. The drive log's note names each
    // ("sweep g2 h4 gap0.3 left", "... straight"), so fit_sweep.py takes from it each rhythm's steady turn rate
    // and how fast the car comes to it and out of it.
    // ---------------------------------------------------------------- the handling benchmark

    // SKRoadAutopilotBench on (open flat land, a sandbox): the same tests for any car, run one after another
    // with nothing to supervise, each named in every drive-log row's note ("bench sweep g2", "bench g3
    // circle30"), scored by bench_score.py into one sheet per car and gear:
    //  1. identify: the steering sweep in each gear from 1 to SKRoadAutopilotMaxGear (about 3 min a gear);
    //  2. handling: in each of those gears, each course in BENCH_COURSES for BENCH_COURSE_S, laid out afresh
    //     from where the car is.
    // It sets SKRoadAutopilotSweep and SKRoadAutopilotTestCourse itself, holds the gear (sBenchGear), and turns
    // itself off at the end ("bench done").
    const char* const BENCH_COURSES[] = { "straight", "circle60", "circle30", "circle15", "figure8_30", "figure8_15",
                                          "slalom_4_40", "slalom_6_30" };
    constexpr F64 BENCH_COURSE_S = 60.0;
    constexpr S32 BENCH_SWEEP_GEARS = 2;
    constexpr F64 BENCH_SWEEP_S = 3.0 + 11 * 2 * (5.0 + 3.0) + 2.0;    // settle, eleven rhythms, a margin
    S32 sBenchStep = -1;
    F64 sBenchStepAt = 0.0;
    bool sBenchInGear = false;
    bool sBenchOnTrack = false;
    F64 sBenchOnSince = -1.0;
    constexpr F64 BENCH_ON_TRACK_M = 3.0;
    constexpr F64 BENCH_REACH_S = 60.0;         // s to reach the track before the test is skipped

    // The bench's tests in order: (gear, course; "" for the sweep).
    std::vector<std::pair<S32, std::string>> benchTests(S32 gears)
    {
        std::vector<std::pair<S32, std::string>> t;
        // the sweep only in the gears slow enough for it to stay in an arena (its straights and gentle rhythms run
        // 5 s at the gear's top: 33 m in Gear 1, 52 m in Gear 2, 80 m in Gear 3)
        for (S32 g = 1; g <= std::min(gears, BENCH_SWEEP_GEARS); ++g)
        {
            t.push_back({ g, "" });
        }
        for (S32 g = 1; g <= gears; ++g)
        {
            for (const char* c : BENCH_COURSES)
            {
                t.push_back({ g, c });
            }
        }
        return t;
    }

    bool sweepDone();      // the sweep has run every rhythm (below)

    // Each frame while the bench is on: the step it is at, the settings it sets, its tag for the log's note.
    void benchStep(F64 now, std::string& note)
    {
        static LLCachedControl<bool> bench_on(gSavedSettings, "SKRoadAutopilotBench", false);
        if (!bench_on)
        {
            if (sBenchStep >= 0)
            {
                sBenchStep = -1;
                sBenchGear = 0;
                gSavedSettings.setS32("SKRoadAutopilotSweep", 0);
                gSavedSettings.setString("SKRoadAutopilotTestCourse", "");
            }
            return;
        }
        static LLCachedControl<S32> max_gear_c(gSavedSettings, "SKRoadAutopilotMaxGear", 4);
        const auto tests = benchTests(std::max(1, S32(max_gear_c)));
        static LLCachedControl<S32> bench_from(gSavedSettings, "SKRoadAutopilotBenchFrom", 0);
        if (sBenchStep < 0)
        {
            // from where the last bench left off (SKRoadAutopilotBenchFrom, kept as it goes), so one stopped
            // part way does not run its sweeps again
            sBenchStep = std::clamp(S32(bench_from), 0, S32(tests.size()));
            sBenchStepAt = -1.0;
            say("bench: " + std::to_string(tests.size()) + " tests"
                + (sBenchStep > 0 ? ", from test " + std::to_string(sBenchStep + 1) : std::string()));
        }
        if (sBenchStep >= S32(tests.size()))
        {
            note += note.empty() ? "bench done" : ", bench done";
            gSavedSettings.setBOOL("SKRoadAutopilotBench", false);
            gSavedSettings.setS32("SKRoadAutopilotBenchFrom", 0);
            say("bench done");
            return;
        }
        if (sArenaSet)
        {
            // out of the arena: the bench and the autopilot stop where the car is (left running, one run
            // wandered on through 28 region crossings)
            const LLVector3d at = gAgent.getPositionGlobal();
            if (at.mdV[VX] < sArenaX0 - 10 || at.mdV[VX] > sArenaX1 + 10 || at.mdV[VY] < sArenaY0 - 10 || at.mdV[VY] > sArenaY1 + 10)
            {
                say("bench: the car left the arena; bench and autopilot off");
                gSavedSettings.setBOOL("SKRoadAutopilotBench", false);
                gSavedSettings.setBOOL("SKRoadAutopilot", false);
                return;
            }
        }
        const auto& [gear, course] = tests[sBenchStep];
        sBenchGear = gear;
        if (sBenchStepAt < 0)
        {
            // the step begins: its settings, and its clock once the car is in its gear
            gSavedSettings.setS32("SKRoadAutopilotSweep", course.empty() ? gear : 0);
            gSavedSettings.setString("SKRoadAutopilotTestCourse", course.empty() ? std::string() : course);
            sBenchStepAt = 0.0;
            sBenchInGear = false;
        }
        if (!sBenchInGear)
        {
            if (sGear == gear)
            {
                sBenchInGear = true;
                sBenchStepAt = now;
                sBenchOnTrack = false;
                sBenchOnSince = -1.0;
            }
            note += note.empty() ? "bench to gear" : ", bench to gear";
            return;
        }
        if (!course.empty() && !sBenchOnTrack)
        {
            // the clock starts once the car has been on the track (within BENCH_ON_TRACK_M) for 2 s: a test
            // begun from wherever the last left the car scored the way back, not the track
            if (std::fabs(sLastErr) < BENCH_ON_TRACK_M)
            {
                if (sBenchOnSince < 0)
                {
                    sBenchOnSince = now;
                }
                if (now - sBenchOnSince >= 2.0)
                {
                    sBenchOnTrack = true;
                    sBenchStepAt = now;
                }
            }
            else
            {
                sBenchOnSince = -1.0;
            }
            if (!sBenchOnTrack && now - sBenchStepAt > BENCH_REACH_S)
            {
                say("bench: never reached the track for g" + std::to_string(gear) + " " + course + ", skipped");
                ++sBenchStep;
                gSavedSettings.setS32("SKRoadAutopilotBenchFrom", sBenchStep);
                sBenchStepAt = -1.0;
                return;
            }
            if (!sBenchOnTrack)
            {
                note += note.empty() ? "bench to track" : ", bench to track";
                return;
            }
        }
        const F64 length = course.empty() ? 1e9 : BENCH_COURSE_S;      // the sweep: until it is done
        const std::string tag = "bench g" + std::to_string(gear) + " " + (course.empty() ? std::string("sweep") : course);
        note += note.empty() ? tag : ", " + tag;
        if (now - sBenchStepAt >= length || (course.empty() && sweepDone()))
        {
            ++sBenchStep;
            gSavedSettings.setS32("SKRoadAutopilotBenchFrom", sBenchStep);
            sBenchStepAt = -1.0;
        }
    }

    constexpr F64 SWEEP_ON_S = 5.0, SWEEP_OFF_S = 1.5, SWEEP_RUNIN_S = 1.5;
    constexpr S32 SWEEP_HOLDS[] = { 1, 2, 4, 8, 16 };
    constexpr F64 SWEEP_GAPS[] = { 0.1, 0.3 };
    F64 sSweepStart = -1.0;
    F64 sSweepShiftAt = -100.0;
    constexpr F64 SWEEP_TRAIL_S = 30.0;
    std::deque<std::pair<LLVector3d, F64>> sSweepTrail;     // the car's own track, for the overlay

    // The sweep's rhythms in order: SWEEP_HOLDS x SWEEP_GAPS, then the key held (hold 0).
    S32 sweepRhythms() { return S32(std::size(SWEEP_HOLDS) * std::size(SWEEP_GAPS)) + 1; }

    // Kept to the arena (the parcel, arenaBox): each rhythm, left then right, begins only once the car has the
    // runway ahead for SWEEP_ON_S at its speed (to the arena's edge less SWEEP_EDGE_M), and is cut short (and
    // tried again) if the car comes within SWEEP_EDGE_S of the edge. After each, SWEEP_OFF_S untouched (how the
    // turn dies away), then home: steered for the arena's middle until there is runway again. Notes: "... left",
    // "... right" while a rhythm runs, "... straight" for the untouched seconds after, "sweep: home" between.
    constexpr F64 SWEEP_EDGE_M = 6.0, SWEEP_EDGE_S = 1.5;
    S32 sSweepK = 0, sSweepSide = 0, sSweepPhase = 0;   // phase 0 home, 3 run-in, 1 rhythm, 2 untouched after
    F64 sSweepPhaseAt = 0.0;
    bool sweepDone() { return sSweepStart >= 0 && sSweepK >= sweepRhythms(); }

    // Metres from (x, y) along heading `yaw` to the arena's edge less the margin.
    F64 arenaRunway(F64 x, F64 y, F64 yaw, F64 x0, F64 x1, F64 y0, F64 y1)
    {
        const F64 dx = std::cos(yaw), dy = std::sin(yaw);
        F64 run = 1e9;
        if (dx > 1e-6) run = std::min(run, (x1 - SWEEP_EDGE_M - x) / dx);
        if (dx < -1e-6) run = std::min(run, (x0 + SWEEP_EDGE_M - x) / dx);
        if (dy > 1e-6) run = std::min(run, (y1 - SWEEP_EDGE_M - y) / dy);
        if (dy < -1e-6) run = std::min(run, (y0 + SWEEP_EDGE_M - y) / dy);
        return run;
    }

    U32 sweepKeys(S32 gear, F64 now, F64 speed, const LLVector3d& pos, F64 yaw, std::string& note)
    {
        U32 keys = AGENT_CONTROL_AT_POS;
        auto tag = [&](const std::string& t) { note += note.empty() ? t : ", " + t; };
        // into the gear first: down while it is not known (the car says it on a shift), never into Reverse
        if (sGear != gear)
        {
            sSweepStart = -1.0;
            if (now - sSweepShiftAt > 1.0)
            {
                sSweepShiftAt = now;
            }
            if (now - sSweepShiftAt < SHIFT_HOLD_S)
            {
                keys |= shiftKey(sGear <= 0 ? (sGear < 0 ? 1 : -1) : (sGear < gear ? 1 : -1));
            }
            tag("sweep: to gear");
            return keys;
        }
        if (sSweepStart < 0)
        {
            sSweepStart = now;
            sSweepK = 0;
            sSweepSide = 0;
            sSweepPhase = 0;
            sSweepPhaseAt = now;
        }
        if (sSweepK >= sweepRhythms())
        {
            tag("sweep: done");
            return keys;
        }
        F64 x0, x1, y0, y1;
        arenaBox(pos.mdV[VX], pos.mdV[VY], x0, x1, y0, y1);
        if (pos.mdV[VX] < x0 - 10 || pos.mdV[VX] > x1 + 10 || pos.mdV[VY] < y0 - 10 || pos.mdV[VY] > y1 + 10)
        {
            tag("sweep: outside the arena, stopped");     // let go of everything: it rolls to a stop
            return 0;
        }
        const F64 run = arenaRunway(pos.mdV[VX], pos.mdV[VY], yaw, x0, x1, y0, y1);
        const bool held = sSweepK == sweepRhythms() - 1;
        const S32 h = held ? 0 : SWEEP_HOLDS[sSweepK / S32(std::size(SWEEP_GAPS))];
        const F64 gap = held ? 0.0 : SWEEP_GAPS[sSweepK % S32(std::size(SWEEP_GAPS))];
        char name[64];
        if (held)
        {
            snprintf(name, sizeof(name), "sweep g%d held", gear);
        }
        else
        {
            snprintf(name, sizeof(name), "sweep g%d h%d gap%.1f", gear, h, gap);
        }
        const S32 dir = sSweepSide == 0 ? 1 : -1;
        if (sSweepPhase == 0)
        {
            // home: up to speed and pointed into the arena, then the next rhythm once there is the runway for it
            const bool settled = now - sSweepPhaseAt > 2.0 && speed > 1.0;
            if (settled && run >= speed * (SWEEP_RUNIN_S + SWEEP_ON_S))
            {
                sSweepPhase = 3;
                sSweepPhaseAt = now;
            }
            else
            {
                const F64 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                const F64 err = wrap(std::atan2(cy - pos.mdV[VY], cx - pos.mdV[VX]) - yaw) * 180 / PI;
                if (std::fabs(err) > 15.0)
                {
                    keys |= err > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                }
                tag("sweep: home");
                return keys;
            }
        }
        if (sSweepPhase == 3)
        {
            // the run-in: straight, keys off, so the rhythm starts from a steady heading and speed
            if (now - sSweepPhaseAt < SWEEP_RUNIN_S)
            {
                tag(std::string(name) + " run-in");
                return keys;
            }
            sSweepPhase = 1;
            sSweepPhaseAt = now;
        }
        if (sSweepPhase == 1)
        {
            const F64 in = now - sSweepPhaseAt;
            if (run < speed * SWEEP_EDGE_S)
            {
                // the edge first: cut short and tried again once home
                tag(std::string(name) + " cut short");
                sSweepPhase = 0;
                sSweepPhaseAt = now;
                return keys;
            }
            if (in >= SWEEP_ON_S)
            {
                sSweepPhase = 2;
                sSweepPhaseAt = now;
            }
            else
            {
                tag(std::string(name) + (dir > 0 ? " left" : " right"));
                const F64 period = h * DT + gap;
                if (held || std::fmod(in, period) < h * DT)
                {
                    keys |= dir > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                }
                return keys;
            }
        }
        // untouched after the rhythm, unless the edge comes first
        if (now - sSweepPhaseAt >= SWEEP_OFF_S || run < speed * SWEEP_EDGE_S)
        {
            sSweepPhase = 0;
            sSweepPhaseAt = now;
            if (++sSweepSide > 1)
            {
                sSweepSide = 0;
                ++sSweepK;
            }
            tag("sweep: home");
            return keys;
        }
        tag(std::string(name) + " straight");
        return keys;
    }

    // ---------------------------------------------------------------- the PWM steering sweep (spike)

    // SKRoadAutopilotPwmSweep on (with the autopilot; seated, stopped, in a flat open arena parcel): frame-exact
    // steering patterns for the PWM study (V:\Scratch\road-pwm\PLAN.md), W held in SKRoadAutopilotPwmSweepGear.
    // Each pattern runs to the left (A) and then to the right (D): its keys set and cleared on exact viewer frames
    // (no MIN_GAP), then PWS_REST_S untouched. Families: "pwm" (a tap of T frames every T + G frames for
    // PWS_PWM_S, T and G in PWS_FRAMES), "hold" (the key held PWS_HOLDS s), "ctr" (a tap of N frames, K frames
    // off, M frames the other way). Within PWS_EDGE_M of the arena's edge or off its flat land, the pattern is cut
    // and run again once the car has been steered back toward the middle. Per frame to
    // logs/sk-road-sweep-<time>.csv, per pattern to sk-road-sweep-<time>-summary.csv; ends by itself
    // ("pwm sweep done"), autopilot off.
    constexpr S32 PWS_FRAMES[] = { 1, 2, 3, 4, 6, 8 };
    constexpr F64 PWS_HOLDS[] = { 0.25, 0.5, 1.0 };
    constexpr S32 PWS_CTR_N[] = { 4, 8, 16 }, PWS_CTR_M[] = { 1, 2 }, PWS_CTR_K[] = { 0, 1, 2 };
    constexpr F64 PWS_PWM_S = 4.5;              // 1.5 s to build, 3 s steady
    constexpr F64 PWS_STEADY_S = 1.5;           // steady rate: the mean over the pattern's last 1.5 s
    constexpr F64 PWS_REST_S = 2.0, PWS_REST_SHORT_S = 3.0;    // untouched after (short patterns: their carry)
    constexpr F64 PWS_EDGE_M = 10.0, PWS_START_EDGE_M = 20.0, PWS_FLAT_M = 0.5;

    struct PwsPattern
    {
        S32 kind = 0;            // 0 pwm, 1 hold, 2 counter-tap
        S32 a = 0, b = 0, c = 0; // pwm: tap, gap; ctr: N, M, K
        F64 hold = 0.0;
        std::string id;
    };

    struct PwsSample { F64 t, rate; };

    struct PwsState
    {
        bool on = false;
        std::vector<PwsPattern> patterns;
        size_t k = 0;           // pattern index
        S32 side = 0;           // 0 A (left), 1 D (right)
        S32 phase = 0;          // 0 to gear, 1 home, 2 settle, 3 run, 4 rest, 5 done
        F64 phaseAt = 0.0, startT = 0.0, shiftAt = -100.0;
        S32 frame = 0, runFrame = 0;
        bool ending = false;    // pwm: past PWS_PWM_S, finishing the tap under way
        F64 keyUpAt = -1.0, firstKeyAt = -1.0;
        std::vector<PwsSample> run, rest;
        F64 fpsSum = 0.0;
        S32 fpsN = 0;
        F64 h0 = 0.0;           // the land height under the car where the sweep began
        S32 family = -1;
        F64 prevT = -1.0;
    };
    PwsState sPws;
    llofstream sPwsLog, sPwsSummary;

    std::vector<PwsPattern> pwsPatterns()
    {
        std::vector<PwsPattern> p;
        for (S32 t : PWS_FRAMES)
        {
            for (S32 g : PWS_FRAMES)
            {
                PwsPattern q;
                q.kind = 0;
                q.a = t;
                q.b = g;
                q.id = llformat("pwm t%d g%d", t, g);
                p.push_back(q);
            }
        }
        for (F64 h : PWS_HOLDS)
        {
            PwsPattern q;
            q.kind = 1;
            q.hold = h;
            q.id = llformat("hold %.2f", h);
            p.push_back(q);
        }
        for (S32 n : PWS_CTR_N)
        {
            for (S32 m : PWS_CTR_M)
            {
                for (S32 kk : PWS_CTR_K)
                {
                    PwsPattern q;
                    q.kind = 2;
                    q.a = n;
                    q.b = m;
                    q.c = kk;
                    q.id = llformat("ctr n%d m%d k%d", n, m, kk);
                    p.push_back(q);
                }
            }
        }
        return p;
    }

    const char* const PWS_FAMILY[] = { "pwm", "hold", "counter-tap" };

    // The pattern's key on run frame f (+1 its side's key, -1 the other, 0 none); done once it has ended.
    S32 pwsKey(const PwsPattern& q, S32 f, F64 in, bool& ending, bool& done)
    {
        done = false;
        if (q.kind == 0)
        {
            const S32 period = q.a + q.b, at = f % period;
            if (in >= PWS_PWM_S && (at == 0 || at >= q.a))
            {
                ending = true;      // stop on a release, never part way through a tap
            }
            done = ending;
            return !ending && at < q.a ? 1 : 0;
        }
        if (q.kind == 1)
        {
            done = in >= q.hold;
            return done ? 0 : 1;
        }
        const S32 n = q.a, m = q.b, kk = q.c;
        if (f < n) return 1;
        if (f < n + kk) return 0;
        if (f < n + kk + m) return -1;
        done = true;
        return 0;
    }

    std::string pwsStamp()
    {
        const std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
        return buf;
    }

    F64 pwsEpoch()
    {
        return std::chrono::duration<F64>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void pwsSummarise(const PwsPattern& q, S32 gear)
    {
        const char side = sPws.side == 0 ? 'A' : 'D';
        const F64 sign = sPws.side == 0 ? 1.0 : -1.0;      // A turns left: yaw rate +
        F64 steady = NAN, peak = 0.0;
        const F64 run_end = sPws.run.empty() ? 0.0 : sPws.run.back().t;
        if (run_end - sPws.firstKeyAt >= 2.0)
        {
            F64 sum = 0;
            S32 n = 0;
            for (const auto& s : sPws.run)
            {
                if (s.t >= run_end - PWS_STEADY_S)
                {
                    sum += s.rate * sign;
                    ++n;
                }
            }
            steady = n ? sum / n : NAN;
        }
        for (const auto& s : sPws.run)
        {
            peak = std::max(peak, s.rate * sign);
        }
        for (const auto& s : sPws.rest)
        {
            peak = std::max(peak, s.rate * sign);
        }
        const F64 ref = std::isnan(steady) ? peak : steady;
        F64 rise = NAN;
        for (const auto* part : { &sPws.run, &sPws.rest })
        {
            for (const auto& s : *part)
            {
                if (std::isnan(rise) && ref > 1.0 && s.rate * sign >= 0.9 * ref)
                {
                    rise = s.t - sPws.firstKeyAt;
                }
            }
        }
        // carry: deg turned (own side +) from the key up; settle: until |rate| stays under 2 deg/s or 10 % of the peak
        F64 carry = 0.0, settle = 0.0, prev = sPws.keyUpAt;
        const F64 quiet = std::max(2.0, 0.1 * std::fabs(peak));
        for (const auto& s : sPws.rest)
        {
            carry += s.rate * sign * (s.t - prev);
            prev = s.t;
            if (std::fabs(s.rate) >= quiet)
            {
                settle = s.t - sPws.keyUpAt;
            }
        }
        const F64 fps = sPws.fpsN && sPws.fpsSum > 0 ? sPws.fpsN / sPws.fpsSum : 0.0;
        sPwsSummary << q.id << ' ' << side << ',' << q.id << ',' << side << ',' << steady << ',' << peak << ',' << rise
                    << ',' << carry << ',' << settle << ',' << gear << ',' << fps << ',' << sPws.firstKeyAt << ','
                    << sPws.keyUpAt << '\n';
        sPwsSummary.flush();
    }

    void pwsStop(const std::string& why)
    {
        if (sPwsLog.is_open()) sPwsLog.close();
        if (sPwsSummary.is_open()) sPwsSummary.close();
        sPws.on = false;
        say(why);
    }

    // Each frame while SKRoadAutopilotPwmSweep is on: the keys, and the frame's log row.
    U32 pwmSweepKeys(F64 now, F64 speed, const LLVector3d& pos, F64 yaw, std::string& note)
    {
        static LLCachedControl<S32> gear_c(gSavedSettings, "SKRoadAutopilotPwmSweepGear", 1);
        const S32 gear = std::max(1, S32(gear_c));
        auto tag = [&](const std::string& t) { note += note.empty() ? t : ", " + t; };
        if (!sPws.on)
        {
            if (speed > 0.5)
            {
                gSavedSettings.setBOOL("SKRoadAutopilotPwmSweep", false);
                say("pwm sweep: stop the car first");
                return 0;
            }
            sPws = PwsState();
            sPws.on = true;
            sPws.patterns = pwsPatterns();
            sPws.startT = now;
            sPws.phaseAt = now;
            sPws.h0 = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(pos.mdV[VX], pos.mdV[VY], 0));
            const std::string stamp = pwsStamp();
            const std::string base = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-sweep-" + stamp);
            sPwsLog.open((base + ".csv").c_str(), std::ios::out);
            sPwsLog.precision(10);
            sPwsLog << "# vehicle " << sVehicle << ", gear " << gear << "\n"
                    << "epoch,t,frame,dt,keys,yaw_deg,yaw_rate_deg,speed,pattern,side,phase\n";
            sPwsSummary.open((base + "-summary.csv").c_str(), std::ios::out);
            sPwsSummary.precision(10);
            sPwsSummary << "pattern,id,side,steady_rate_deg,peak_rate_deg,rise90_s,carry_deg,settle_s,gear,mean_fps,"
                           "key_down_t,key_up_t\n";
            F64 x0, x1, y0, y1;
            arenaBox(pos.mdV[VX], pos.mdV[VY], x0, x1, y0, y1);
            say(llformat("pwm sweep: %d patterns a side in gear %d, %.0f x %.0f m; log sk-road-sweep-%s",
                         S32(sPws.patterns.size()), gear, x1 - x0, y1 - y0, stamp.c_str()));
        }
        const F64 dt = sPws.prevT < 0 ? 0.0 : now - sPws.prevT;
        sPws.prevT = now;
        ++sPws.frame;
        U32 keys = AGENT_CONTROL_AT_POS;
        const PwsPattern* q = sPws.k < sPws.patterns.size() ? &sPws.patterns[sPws.k] : nullptr;
        const S32 dir = sPws.side == 0 ? 1 : -1;

        F64 x0, x1, y0, y1;
        arenaBox(pos.mdV[VX], pos.mdV[VY], x0, x1, y0, y1);
        const F64 edge = std::min({ pos.mdV[VX] - x0, x1 - pos.mdV[VX], pos.mdV[VY] - y0, y1 - pos.mdV[VY] });
        const F64 land = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(pos.mdV[VX], pos.mdV[VY], 0));
        const bool flat = std::fabs(land - sPws.h0) <= PWS_FLAT_M;
        if (edge < -10.0)
        {
            pwsStop("pwm sweep: the car left the arena; sweep and autopilot off");
            gSavedSettings.setBOOL("SKRoadAutopilotPwmSweep", false);
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            return 0;
        }
        auto enter = [&](S32 phase)
        {
            sPws.phase = phase;
            sPws.phaseAt = now;
        };

        if (sPws.phase == 0)
        {
            // into the gear (W held), as the old sweep does
            if (sGear == gear)
            {
                enter(1);
            }
            else
            {
                if (now - sPws.shiftAt > 1.0) sPws.shiftAt = now;
                if (now - sPws.shiftAt < SHIFT_HOLD_S)
                {
                    keys |= shiftKey(sGear <= 0 ? (sGear < 0 ? 1 : -1) : (sGear < gear ? 1 : -1));
                }
            }
        }
        if (sPws.phase == 1)
        {
            // home: up to speed and pointed at the middle, room to the edge
            const F64 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            const F64 err = wrap(std::atan2(cy - pos.mdV[VY], cx - pos.mdV[VX]) - yaw) * 180 / PI;
            const bool room = edge >= PWS_START_EDGE_M || std::hypot(cx - pos.mdV[VX], cy - pos.mdV[VY]) < 10.0;
            if (room && flat && std::fabs(err) < 30.0 && now - sPws.phaseAt > 2.0 && speed > 1.0)
            {
                enter(2);
            }
            else if (std::fabs(err) > 15.0)
            {
                keys |= err > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
            }
        }
        if (sPws.phase == 2 && now - sPws.phaseAt >= 1.5)
        {
            // settled straight: the run, from this frame
            if (!q)
            {
                enter(5);
            }
            else
            {
                if (q->kind != sPws.family)
                {
                    if (sPws.family >= 0)
                    {
                        say(std::string("pwm sweep: end of ") + PWS_FAMILY[sPws.family] + llformat(" at %.3f", pwsEpoch()));
                    }
                    sPws.family = q->kind;
                    say(std::string("pwm sweep: start of ") + PWS_FAMILY[q->kind] + llformat(" at %.3f", pwsEpoch()));
                }
                enter(3);
                sPws.runFrame = 0;
                sPws.ending = false;
                sPws.run.clear();
                sPws.rest.clear();
                sPws.fpsSum = 0.0;
                sPws.fpsN = 0;
                sPws.firstKeyAt = now;
                sPws.keyUpAt = -1.0;
            }
        }
        if ((sPws.phase == 3 || sPws.phase == 4) && (edge < PWS_EDGE_M || !flat))
        {
            // too near the edge or off the flat: cut, home, the same pattern again
            tag("pwm sweep: cut, home");
            LL_INFOS("SKRoadDriver") << "pwm sweep: " << (q ? q->id : std::string()) << (dir > 0 ? " A" : " D")
                                     << " cut at the edge" << LL_ENDL;
            enter(1);
        }
        if (sPws.phase == 3 && q)
        {
            bool done = false;
            const S32 key = pwsKey(*q, sPws.runFrame, now - sPws.phaseAt, sPws.ending, done);
            ++sPws.runFrame;
            if (key)
            {
                keys |= key * dir > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
            }
            sPws.run.push_back({ now, sYawRate * 180 / PI });
            if (dt > 0) { sPws.fpsSum += dt; ++sPws.fpsN; }
            if (done)
            {
                sPws.keyUpAt = now;
                enter(4);
            }
        }
        else if (sPws.phase == 4 && q)
        {
            sPws.rest.push_back({ now, sYawRate * 180 / PI });
            if (dt > 0) { sPws.fpsSum += dt; ++sPws.fpsN; }
            if (now - sPws.phaseAt >= (q->kind == 0 ? PWS_REST_S : PWS_REST_SHORT_S))
            {
                pwsSummarise(*q, gear);
                if (++sPws.side > 1)
                {
                    sPws.side = 0;
                    ++sPws.k;
                }
                // straight on to the next while there is room; home otherwise
                const PwsPattern* next = sPws.k < sPws.patterns.size() ? &sPws.patterns[sPws.k] : nullptr;
                if (!next)
                {
                    enter(5);
                }
                else if (edge >= PWS_START_EDGE_M && flat)
                {
                    enter(2);
                    sPws.phaseAt = now - 1.5;       // the rest was the settle
                }
                else
                {
                    enter(1);
                }
            }
        }
        if (sPws.phase == 5)
        {
            if (sPws.family >= 0)
            {
                say(std::string("pwm sweep: end of ") + PWS_FAMILY[sPws.family] + llformat(" at %.3f", pwsEpoch()));
            }
            pwsStop("pwm sweep done");
            gSavedSettings.setBOOL("SKRoadAutopilotPwmSweep", false);
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            return 0;
        }

        static const char* const PHASES[] = { "gear", "home", "settle", "run", "rest", "done" };
        std::string ks;
        if (keys & AGENT_CONTROL_AT_POS) ks += 'W';
        if (keys & AGENT_CONTROL_YAW_POS) ks += 'A';
        if (keys & AGENT_CONTROL_YAW_NEG) ks += 'D';
        if (keys & ~(AGENT_CONTROL_AT_POS | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG)) ks += 'S';    // a shift
        sPwsLog << pwsEpoch() << ',' << now - sPws.startT << ',' << sPws.frame << ',' << dt << ',' << ks << ','
                << yaw * 180 / PI << ',' << sYawRate * 180 / PI << ',' << speed << ',' << (q ? q->id : std::string())
                << ',' << (dir > 0 ? 'A' : 'D') << ',' << PHASES[sPws.phase] << '\n';
        tag("pwm sweep " + std::string(q ? q->id : "") + (dir > 0 ? " A " : " D ") + PHASES[sPws.phase]);
        return keys;
    }

    bool pwmSweepOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "SKRoadAutopilotPwmSweep", false);
        if (!on && sPws.on)
        {
            pwsStop("pwm sweep off");
        }
        return on;
    }

    bool backClear(const LLVector3d& pos, F64 yaw, F64 speed);     // room behind, below

    // ---------------------------------------------------------------- getting unstuck

    // Run into something (a mailbox, a rail, a tree): W held and the car under STUCK_MPS for STUCK_S. A driver
    // backs off and points the nose back at the road, then drives on. So: S held; if S only brakes (this car's
    // reverse is a gear below Gear 1, "Reverse"), into Reverse with W. Backing, a turn key held to swing the nose
    // toward the line (its aim point, or straight back the way the car came with no line): which key does that
    // backwards differs between cars, so the turn is checked after RECOVER_CHECK_S and the key swapped if the nose
    // swings the wrong way. Done after RECOVER_BACK_M, or once the nose points within RECOVER_ALIGN_DEG of the
    // aim, or after RECOVER_MAX_S; then into Gear 1 and on. Three times in 30 s: it gives up and stops.
    constexpr F64 STUCK_WINDOW_S = 2.0, STUCK_MOVED_M = 0.8;
    std::deque<std::pair<F64, LLVector3d>> sStuckTrail;
    constexpr F64 RECOVER_BACK_M = 3.0, RECOVER_ALIGN_DEG = 20.0, RECOVER_MAX_S = 6.0, RECOVER_CHECK_S = 0.6;
    // Backing on past RECOVER_BACK_M while the nose is still swinging round toward the lane, up to RECOVER_BACK_MAX_M.
    constexpr F64 RECOVER_BACK_MAX_M = 6.0;
    constexpr F64 RECOVER_S_TRY_S = 1.0;
    constexpr F64 RECOVER_GIVE_UP_S = 20.0;
    F64 sRecoverStarted = 0.0;
    F64 sStuckSince = -1.0;
    S32 sRecoverPhase = 0;          // 0 driving, 1 backing with S, 2 into Reverse, 3 backing in Reverse, 4 into Gear 1
    F64 sRecoverAt = 0.0, sRecoverShiftAt = -100.0;
    LLVector3d sRecoverFrom;
    S32 sRecoverKey = 0;            // the turn key held while backing
    bool sRecoverChecked = false;
    F64 sRecoverCheckAt = 0.0;     // the next check of which way the held key swings the nose
    S32 sRecoverFlips = 0;
    // Backing off, judged by the error to the lane over a window from the key's change, not by the yaw rate the
    // instant the check comes: at Mounford the nose swung away under D and the first check rightly swapped to A,
    // but 0.6 s on the yaw rate still carried D's swing (the wheel and the car lag the key) and swapped back to
    // D, the flips spent; D held, it backed from 23 to 73 deg off. One swap per back-off; still growing after it,
    // the backing stops and the car turns on the spot.
    constexpr F64 RECOVER_JUDGE_S = 1.0, RECOVER_JUDGE_DEG = 2.0;
    constexpr F64 RECOVER_LEAD_S = 1.0;            // s of the swing the nose carries on with once the key is let go
    F64 sRecoverCheckErr = 0.0;     // |error| when the window began, deg
    bool sRecoverAbort = false;     // still swinging away after the swap: stop backing
    S32 sRecoverKeyLeft = -1;       // backing, the key that swings the nose left (learnt; first guess a real car's)
    U32 sRecoverKeys = 0;
    std::deque<F64> sRecoverTimes;
    // The heading a recovery steers for: the lane's heading where the car is, cut back toward the lane's centre by
    // atan(offset / RECOVER_AIM_M), at most RECOVER_CUT_MAX_DEG.
    constexpr F64 RECOVER_AIM_M = 8.0, RECOVER_CUT_MAX_DEG = 30.0;
    // The escalation ladder, counted per spot: stuck again within LADDER_SPOT_M of where the last recovery began
    // (and within LADDER_FORGET_S of it) is the next rung; anywhere else starts at the bottom. Rungs: turn on the
    // spot (only where the nose is over SPIN_MIN_DEG off the aim, else straight to backing off), back off, turn
    // round three-point; past the top it gives up. LADDER_MAX_N recoveries in LADDER_MAX_WINDOW_S anywhere also
    // give up (creeping a few metres between stucks, never the same spot).
    constexpr F64 LADDER_SPOT_M = 10.0, LADDER_FORGET_S = 120.0, LADDER_MAX_WINDOW_S = 120.0;
    constexpr S32 LADDER_TOP = 3, LADDER_MAX_N = 8;
    LLVector3d sLadderAt;
    F64 sLadderLast = -1e9;
    S32 sLadderRung = -1;
    // Turning on the spot (recover phase 5): A/D taps with nothing else held, as the mission's placing step turns
    // the car (0.2 s held each 0.5 s, A anticlockwise), until the nose is within SPIN_DONE_DEG of the aim. Not
    // turning (under SPIN_PROGRESS_DEG in SPIN_CHECK_S): the other key once, then on to backing off.
    constexpr F64 SPIN_MIN_DEG = 30.0, SPIN_DONE_DEG = 12.0, SPIN_MAX_S = 10.0, SPIN_CHECK_S = 2.0, SPIN_PROGRESS_DEG = 4.0;
    constexpr F64 SPIN_TAP_S = 0.2, SPIN_TAP_EVERY_S = 0.5, SPIN_STOPPED_MPS = 0.6;
    // Backed off and still over REORIENT_DEG off the aim: turned on the spot before driving on.
    constexpr F64 REORIENT_DEG = 20.0;
    S32 sSpinKey = 1;               // + A (YAW_POS)
    bool sSpinFlipped = false, sSpinThenBack = false;
    F64 sSpinTapNext = 0.0, sSpinTapUntil = 0.0, sSpinCheckAt = 0.0, sSpinCheckErr = 0.0;
    F64 sRecoverYaw0 = 0.0;         // the heading when the step began, for its log line
    const char* sRecoverStep = "";
    bool sFrozenTried = false;      // the frozen stop's one recovery, until the car moves
    // The car's pose not changing by a single bit is no car against a wall (that jitters a few cm and degrees):
    // the region has stopped sending the car's updates. After a 22 s viewer stall and a crossing into Tiberius the
    // car read exactly the same speed 0, position and heading for 15 s, frame after frame, while the driver backed
    // and shifted through three "stucks" into a stop. Frozen, it is not counted stuck; frozen for FROZEN_GIVE_UP_S
    // it stops, saying so.
    constexpr F64 FROZEN_S = 1.0, FROZEN_RECOVER_S = 6.0, FROZEN_GIVE_UP_S = 20.0;
    LLVector3d sPoseSeen;
    F64 sYawSeen = 0.0, sPoseChangedAt = -1.0;
    bool sFrozenSaid = false;

    // A region crossing: from a few metres short of the edge (heading across it on the car's velocity) or the
    // car's, the agent's region or the car object changing, until the new region sends the car's physics again
    // (its velocity changing, CROSS_FRESH_N updates, with the agent in the car's region). The car floats a
    // moment in the handover: the frozen, stuck, edge, drop, lost-road and line-stall checks all misread it
    // (runs ended at region edges and corners), so in a crossing nothing is decided and W is held as it went in.
    constexpr F64 CROSS_AHEAD_M = 4.0, CROSS_AHEAD_S = 0.3, CROSS_AHEAD_GIVE_UP_S = 3.0, CROSS_STALL_S = 8.0,
                  CROSS_MIN_S = 0.25;
    constexpr S32 CROSS_FRESH_N = 5;
    struct Crossing
    {
        bool on = false;
        bool changed = false;           // the region (or the car object) has changed since it began
        F64 began = 0.0, changed_at = 0.0;
        U32 keys = 0;                   // what is held through it: W as it went in, nothing else
        S32 fresh = 0;                  // the car's physics updates seen since the change
        std::string from, to;
    };
    Crossing sCross;
    bool inCrossing() { return sCross.on; }
    LLViewerRegion* sCrossCarRegion = nullptr;
    LLViewerRegion* sCrossAgentRegion = nullptr;
    LLViewerObject* sCrossCar = nullptr;
    LLVector3 sCrossVel, sCrossOmega;
    LLViewerRegion* sCrossStalledIn = nullptr;     // a stalled crossing is not begun again until the region moves on

    std::string regionName(LLViewerRegion* region)
    {
        return region ? region->getName() : std::string("(none)");
    }

    // An unsafe crossing ahead (Junapel > Artsebarsky: the car dropped off the road, crossed onto a full private
    // parcel and was returned): heading for a region edge on the car's velocity, the edge cell it heads into (the edge
    // map below), where the car's own side is a Linden road parcel, must have one across too, and the car no sudden
    // drop near the edge. A car on land that is not a Linden road (a private road) is left to the mission's trespass
    // checks. Someone's land across a Linden road's edge, or no region beyond it, or a drop, brakes hard to a stop CROSS_GUARD_MARGIN_M short of the edge and ends
    // the run, the edge kept for the mission as a ground hazard (sk_road_driver::unsafeCrossing).
    constexpr F64 CROSS_GUARD_MARGIN_M = 9.0, CROSS_GUARD_LOOK_M = 12.0, CROSS_GUARD_DROP_M = 4.0, CROSS_GUARD_BRAKE_S = 8.0;
    constexpr F64 CROSS_GUARD_DROP_GRADE = 0.35;    // height lost per metre covered that is a fall, not a slope
    F64 sGuardSpeed = 0.0;                          // the car's ground speed, for the drop's grade
    struct CrossGuard
    {
        bool braking = false;
        F64 since = 0.0;
        std::string why;
        LLSD hazard;            // {region, at [x, y, z], ownership, why}; undefined: none this run
        LLVector3d edge;        // the edge point the stop is planned short of
        F64 dx = 0.0, dy = 0.0; // the line of travel it was planned along
    };
    CrossGuard sGuard;

    // A guard's stop is straight and progressive (Barkley, 11:56: S held from 14 m/s 17 m out, with the car swerving
    // along a seam, rolled it onto its side against a wall): no turn keys, the throttle let go, and S pressed only for
    // the share of each GUARD_PULSE_S that the deceleration still needed beyond what coasting gives (THROTTLE_DECEL)
    // asks for, to come down to `target` m/s within `room` m. A full press only when even that is not enough.
    constexpr F64 GUARD_PULSE_S = 0.3, GUARD_MIN_DUTY = 0.2;
    bool guardBrakePress(F64 speed, F64 target, F64 room, F64 now)
    {
        const F64 dv2 = speed * speed - target * target;
        if (dv2 <= 0.0)
        {
            return false;
        }
        const F64 run = room - speed * THROTTLE_LAG_S;
        const F64 need = run > 0.5 ? dv2 / (2 * run) : 2 * THROTTLE_DECEL;
        if (need <= THROTTLE_DECEL * 0.8)
        {
            return false;       // coasting does it
        }
        const F64 duty = std::clamp((need - THROTTLE_DECEL * 0.8) / THROTTLE_DECEL, GUARD_MIN_DUTY, 1.0);
        return duty >= 0.95 || std::fmod(now, GUARD_PULSE_S) < duty * GUARD_PULSE_S;
    }
    std::deque<std::pair<F64, F64>> sGuardZ;       // (t, z) of the last few seconds

    // Where the road meets the region's edge: the lane line (sPath, the line the car steers on) walked on from the
    // car until it leaves the region, so a road crossing the edge on a slant, or a car not quite square to it, is
    // judged where the road crosses rather than where the car's velocity happens to point. With no line near the car
    // reaching the edge, the velocity ray. The edge (0 south, 1 east, 2 north, 3 west), the point on it, the road's
    // half width there (5 m when unknown) and whether the line found it.
    struct EdgeCrossing
    {
        S32 e = 0;
        LLVector3d pt;
        F64 half_w = 5.0;
        bool on_line = false;
        bool line_away = false;     // a lane line runs on from the car and does not reach the edge: not heading for it
    };
    EdgeCrossing edgeCrossing(LLViewerRegion* region, const LLVector3d& pos, F64 vx, F64 vy, F64 to_edge)
    {
        const LLVector3d o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        EdgeCrossing out;
        // the first boundary the segment a->b (region-local) leaves [0, w] through, at a->b's fraction t
        auto leave = [w](F64 ax, F64 ay, F64 bx, F64 by, F64& t, S32& e)
        {
            t = 2.0;
            const F64 dx = bx - ax, dy = by - ay;
            if (dx > 0 && bx > w && (w - ax) / dx < t) { t = (w - ax) / dx; e = 1; }
            if (dx < 0 && bx < 0 && -ax / dx < t) { t = -ax / dx; e = 3; }
            if (dy > 0 && by > w && (w - ay) / dy < t) { t = (w - ay) / dy; e = 2; }
            if (dy < 0 && by < 0 && -ay / dy < t) { t = -ay / dy; e = 0; }
            return t <= 1.0;
        };
        if (sPath.size() >= 2)
        {
            size_t at = 0;
            F64 best = 1e18;
            for (size_t i = 0; i < sPath.size(); ++i)
            {
                const F64 d = std::hypot(sPath[i].x - pos.mdV[VX], sPath[i].y - pos.mdV[VY]);
                if (d < best)
                {
                    best = d;
                    at = i;
                }
            }
            // the line near the car (within a lane or two of it), reaching the edge within a sane run
            const F64 reach = to_edge + 60.0;
            F64 run = 0.0;
            for (size_t i = at; best < 12.0 && i + 1 < sPath.size() && run < reach; ++i)
            {
                const LanePoint &a = sPath[i], &b = sPath[i + 1];
                const F64 ax = a.x - o.mdV[VX], ay = a.y - o.mdV[VY], bx = b.x - o.mdV[VX], by = b.y - o.mdV[VY];
                F64 t = 2.0;
                S32 e = 0;
                if (ax >= 0 && ax <= w && ay >= 0 && ay <= w && leave(ax, ay, bx, by, t, e))
                {
                    out.e = e;
                    out.pt = LLVector3d(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, pos.mdV[VZ]);
                    const F64 road_w = t < 0.5 ? a.road_w : b.road_w;
                    out.half_w = road_w > 1.0 ? road_w / 2 : 5.0;
                    out.on_line = true;
                    return out;
                }
                run += std::hypot(bx - ax, by - ay);
            }
            // the line the car follows bends away before the edge (a curve near the edge): the velocity ray would
            // only say where the car points this moment, not where the road takes it
            if (best < 12.0 && run >= std::min(reach, 40.0))
            {
                out.line_away = true;
            }
        }
        // no line to the edge: the velocity ray
        const F64 px = pos.mdV[VX] - o.mdV[VX], py = pos.mdV[VY] - o.mdV[VY];
        const F64 len = to_edge + 1.0;
        F64 t = 2.0;
        S32 e = 0;
        if (!leave(px, py, px + vx * len, py + vy * len, t, e))
        {
            // the ray stays inside (rounding): the nearer of the two edges it points at
            const F64 lx = px + vx * to_edge, ly = py + vy * to_edge;
            const F64 dx_edge = vx > 0 ? w - lx : lx, dy_edge = vy > 0 ? w - ly : ly;
            e = dx_edge < dy_edge ? (vx > 0 ? 1 : 3) : (vy > 0 ? 2 : 0);
            t = to_edge / len;
        }
        out.e = e;
        out.pt = LLVector3d(pos.mdV[VX] + vx * len * t, pos.mdV[VY] + vy * len * t, pos.mdV[VZ]);
        return out;
    }

    // Whether the edge the car's road is heading into is unsafe, or the car dropped off the road near it; why in
    // `why`, the edge point in `land`. The cell judged is the one where the road crosses the edge (edgeCrossing).
    // Edge cells are 4 m and parcel lines need not follow the road, so where that cell is not a Linden road on both
    // sides, a cell within the road's half width (and a cell more) that is stands for it, so long as its run of safe
    // cells reaches within a cell of the crossing point: the road parcel covers the lane, and the lane cell's lot only
    // lies beside it. CROSS_SAFE, CROSS_WAIT (a side not known yet: no Linden road known to be missing, so the car
    // crawls up and holds short until it resolves, never a hazard) or CROSS_UNSAFE (the car off the road, land across
    // known not to be a Linden road, no region beyond, a drop: the run ends, a hazard).
    enum : S32 { CROSS_SAFE, CROSS_WAIT, CROSS_UNSAFE };

    // A cell still not known EDGE_WAIT_FALLBACK_S after the car first waited on it (its parcel's owner never answers,
    // or keeps failing) holds no longer: the overlay's class says Linden land (a Linden road parcel is), or the lane
    // line runs on over the edge, and it is taken as road, logged; otherwise the car keeps waiting.
    constexpr F64 EDGE_WAIT_FALLBACK_S = 6.0;
    std::map<std::tuple<U64, S32, S32>, F64> sEdgeWaitSince;     // (region, edge, cell): when first waited on
    S32 waitOrFallBack(LLViewerRegion* region, S32 e, S32 cell, S32 own, const EdgeCrossing& x, F64 now, const std::string& why)
    {
        const auto key = std::make_tuple(region->getHandle(), e, cell);
        auto [it, fresh] = sEdgeWaitSince.try_emplace(key, now);
        if (!fresh && now - it->second > 60.0)
        {
            it->second = now;   // a wait long ago: this one starts afresh
        }
        if (now - it->second < EDGE_WAIT_FALLBACK_S)
        {
            return CROSS_WAIT;
        }
        const bool linden_land = own == PARCEL_PUBLIC;
        if (!linden_land && !x.on_line)
        {
            return CROSS_WAIT;
        }
        static std::set<std::tuple<U64, S32, S32>> logged;
        if (logged.insert(key).second)
        {
            LL_INFOS("SKRoadDriver") << "edge guard fallback in " << region->getName() << llformat(": edge %d cell %d ", e, cell) << why
                                     << llformat(" after %.1f s; taken as road: ", now - it->second)
                                     << (linden_land ? "the overlay says Linden land" : "the lane line runs on over the edge")
                                     << " (" << ownershipName(own) << ")" << LL_ENDL;
        }
        return CROSS_SAFE;
    }
    S32 unsafeCrossingAhead(LLViewerRegion* region, F64 now, const LLVector3d& pos, F64 vx, F64 vy, F64 to_edge,
                             LLVector3d& land, std::string& why, LLSD& hazard)
    {
        // the car on the edge itself is crossing already (its position clamped to the old region until the handover):
        // too late to hold short, and the region beyond is looked up from a point the handover has not caught up with
        {
            const LLVector3d o0 = region->getOriginGlobal();
            const F64 w0 = region->getWidth();
            const F64 cx = pos.mdV[VX] - o0.mdV[VX], cy = pos.mdV[VY] - o0.mdV[VY];
            if (std::min(std::min(cx, w0 - cx), std::min(cy, w0 - cy)) < 1.0)
            {
                land = pos;
                return CROSS_SAFE;
            }
        }
        const EdgeCrossing x = edgeCrossing(region, pos, vx, vy, to_edge);
        land = x.pt;
        const EdgeMap& m = edgeMap(region, now);
        const LLVector3d o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        const F64 lx = land.mdV[VX] - o.mdV[VX], ly = land.mdV[VY] - o.mdV[VY];
        const S32 e = x.e;
        const F64 cell_m = w / EDGE_CELLS;
        const F64 along = (e == 0 || e == 2) ? lx : ly;
        const S32 lane_i = std::clamp(S32(along / cell_m), 0, EDGE_CELLS - 1);
        S32 used = lane_i;
        if (!m.cells[e][lane_i].safe)
        {
            // a safe cell within the road's half width (+1 cell), nearest first, whose safe run covers the lane point
            const S32 reach = S32(std::ceil(x.half_w / cell_m)) + 1;
            for (S32 d = 1; d <= reach && used == lane_i; ++d)
            {
                for (const S32 i : { lane_i - d, lane_i + d })
                {
                    if (i < 0 || i >= EDGE_CELLS || !m.cells[e][i].safe)
                    {
                        continue;
                    }
                    S32 lo = i, hi = i;
                    while (lo > 0 && m.cells[e][lo - 1].safe)
                    {
                        --lo;
                    }
                    while (hi < EDGE_CELLS - 1 && m.cells[e][hi + 1].safe)
                    {
                        ++hi;
                    }
                    if (along >= (lo - 1) * cell_m && along <= (hi + 2) * cell_m)
                    {
                        used = i;
                        break;
                    }
                }
            }
        }
        const EdgeCell& c = m.cells[e][used];
        // a drop is a fall, not a road going downhill: the height lost must be steep for the ground covered since
        // (a 10% grade at 14 m/s loses 4 m in 3 s), so it counts only beyond CROSS_GUARD_DROP_GRADE of the run
        F64 drop = 0.0;
        const F64 speed = sGuardSpeed;
        for (const auto& [t, z] : sGuardZ)
        {
            const F64 lost = z - pos.mdV[VZ];
            const F64 run = std::max(1.0, speed * std::max(0.0, now - t));
            if (lost > CROSS_GUARD_DROP_GRADE * run)
            {
                drop = std::max(drop, lost);
            }
        }
        // 3 m across the edge from where the road crosses it
        const LLVector3d beyond_pt(land.mdV[VX] + (e == 1 ? 3.0 : e == 3 ? -3.0 : 0.0),
                                   land.mdV[VY] + (e == 2 ? 3.0 : e == 0 ? -3.0 : 0.0), 0);
        LLViewerRegion* beyond = LLWorld::getInstance()->getRegionFromPosGlobal(beyond_pt);
        const std::string region_name = beyond ? beyond->getName() : std::string("(no region)");
        // the lane line itself runs from the car over the edge: the car is on its road, and a drop in its height history
        // (Dammastock -> Finsteraarhorn twice, a straight downhill under an overpass, 4.7-5 m) is the road going down, not a fall
        if (drop > CROSS_GUARD_DROP_M && !x.on_line)
        {
            why = llformat("the car dropped %.1f m near the edge (off the road)", drop);
        }
        else if (c.safe || (!x.on_line && x.line_away))
        {
            return CROSS_SAFE;
        }
        else if (c.road_in == Road::Other)
        {
            // the car's own side is not a Linden road (a private road, or land the mission's trespass checks
            // watch): the rule is only for leaving a Linden road, so nothing here ends the run
            return CROSS_SAFE;
        }
        else if (c.road_in == Road::Unknown)
        {
            why = "the parcel at the edge is not known yet";
            return waitOrFallBack(region, e, used, c.inside, x, now, why);
        }
        else if (c.across == -2)
        {
            why = "no region beyond the edge";
        }
        else if (c.road_across == Road::Other)
        {
            why = "the land across is not a Linden road: " + std::string(ownershipName(c.across));
        }
        else
        {
            why = "the parcel across is not known yet";
            return waitOrFallBack(region, e, used, c.across, x, now, why);
        }
        // the velocity ray alone (no lane line to the edge) meeting the edge at a shallow angle, close to it: a car
        // swerving along a road on the seam (Barkley, 11:56: the car weaved over the Bonzo seam eight times in 40 s,
        // its stored road dropped), not one heading off the road. Held short at a crawl, never a hazard: the run ends
        // only on a crossing the car is really making.
        if (!x.on_line && c.across != -2)
        {
            const F64 across_v = (e == 1 || e == 3) ? std::fabs(vx) : std::fabs(vy);
            const F64 cx = pos.mdV[VX] - o.mdV[VX], cy = pos.mdV[VY] - o.mdV[VY];
            const F64 off = e == 1 ? w - cx : e == 3 ? cx : e == 2 ? w - cy : cy;
            if (across_v < 0.42 && off < 6.0)
            {
                static F64 logged_t = -100.0;
                if (now - logged_t > 5.0)
                {
                    logged_t = now;
                    LL_INFOS("SKRoadDriver") << "edge guard: velocity ray at a shallow angle"
                                             << llformat(" (%.0f deg, car %.1f m from edge %d)", std::asin(std::min(1.0, across_v)) * 180.0 / F_PI, off, e)
                                             << " judged a swerve along the seam, held short instead of a stop: " << why << LL_ENDL;
                }
                why = "a swerve toward the edge along the seam: " + why;
                return CROSS_WAIT;
            }
        }
        const S32 own = c.across == -2 ? -1 : c.across;
        why = "unsafe crossing ahead: " + region_name + llformat(" (%.0f, %.0f along the edge) ", lx, ly) + ownershipName(own)
              + ": " + why;
        LL_INFOS("SKRoadDriver") << "edge guard stop in " << region->getName() << llformat(": edge %d crossed at (%.1f, %.1f) by the ", e, lx, ly)
                                 << (x.on_line ? "lane line" : "velocity ray")
                                 << llformat(", cell %d judged (lane cell %d, road half width %.1f m), car at (%.1f, %.1f) local",
                                             used, lane_i, x.half_w, pos.mdV[VX] - o.mdV[VX], pos.mdV[VY] - o.mdV[VY])
                                 << LL_ENDL;
        hazard = LLSD::emptyMap();
        hazard["region"] = region_name;
        hazard["at"] = LLSD().with(0, beyond_pt.mdV[VX]).with(1, beyond_pt.mdV[VY]).with(2, pos.mdV[VZ]);
        hazard["edge_at"] = LLSD().with(0, land.mdV[VX]).with(1, land.mdV[VY]);
        hazard["from_region"] = region->getName();
        hazard["ownership"] = ownershipName(own);
        hazard["why"] = why;
        return CROSS_UNSAFE;
    }

    // Waiting on an edge cell not known yet (CROSS_WAIT): the car capped to a crawl and held CROSS_GUARD_MARGIN_M
    // short of the edge, applied to the frame's keys just before they go; on from the moment it resolves safe.
    struct EdgeWait
    {
        bool on = false;
        F64 to_edge = 1e9, since = -1.0;
        std::string why;
    };
    EdgeWait sEdgeWait;

    void resetCrossing()
    {
        sGuard = CrossGuard();
        sEdgeWait = EdgeWait();
        sGuardZ.clear();
        sCross = Crossing();
        sCrossCarRegion = sCrossAgentRegion = nullptr;
        sCrossCar = nullptr;
        sCrossStalledIn = nullptr;
    }

    // The heading (rad) the nose should come round to: toward the line's aim point a few metres on, or with no
    // line the way the car came (its heading reversed).
    F64 recoverAim(const LLVector3d& pos, F64 yaw)
    {
        if (sPath.size() >= 2)
        {
            size_t at = 0;
            F64 best = 1e18;
            for (size_t i = 0; i < sPath.size(); ++i)
            {
                const F64 d = std::hypot(sPath[i].x - pos.mdV[VX], sPath[i].y - pos.mdV[VY]);
                if (d < best)
                {
                    best = d;
                    at = i;
                }
            }
            // the lane's heading at the nearest lane point, not the bearing to a point a few metres on: on the kerb
            // beside or past the lane's points that bearing swung round (aim -100 then 66 deg a second later at
            // Symmetry, 09:32) and could point across or behind the car. Off the centre it cuts back in toward it.
            const LanePoint& q = sPath[at];
            const F64 n = std::hypot(q.ax, q.ay);
            if (n > 1e-6)
            {
                const F64 ax = q.ax / n, ay = q.ay / n;
                const F64 dx = pos.mdV[VX] - q.x, dy = pos.mdV[VY] - q.y;
                const F64 left = dy * ax - dx * ay;     // the car's offset left of the lane's centre
                const F64 cut_max = RECOVER_CUT_MAX_DEG * PI / 180;
                const F64 cut = std::max(-cut_max, std::min(cut_max, std::atan2(left, RECOVER_AIM_M)));
                return wrap(std::atan2(ay, ax) - cut);
            }
            const size_t next = std::min(sPath.size() - 1, at + 1);
            if (next != at)
            {
                return std::atan2(sPath[next].y - sPath[at].y, sPath[next].x - sPath[at].x);
            }
        }
        if (sTravelSet)
        {
            return std::atan2(sTravelY, sTravelX);     // no line: the way travelled
        }
        return yaw;     // no line: back out straight
    }

    void startTurnRound(bool flip, const LLVector3d& pos, F64 yaw, F64 now, const std::string& why);     // below
    std::string placeText(const LLVector3d& pos);

    // One recovery step ended (or was given up): which, and the heading before and after, in the log.
    void logRecoverStep(const char* how, F64 yaw, const LLVector3d& pos)
    {
        LL_INFOS("SKRoadDriver") << "recovery " << sRecoverStep << " " << how << ": heading "
                                 << S32(std::lround(sRecoverYaw0 * 180 / PI)) << " -> " << S32(std::lround(yaw * 180 / PI))
                                 << " deg, aim " << S32(std::lround(recoverAim(pos, yaw) * 180 / PI)) << " deg" << LL_ENDL;
    }

    void beginSpin(F64 now, F64 yaw, F64 aim_err, bool then_back)
    {
        sRecoverPhase = 5;
        sRecoverStep = "turn on the spot";
        sRecoverYaw0 = yaw;
        sRecoverAt = now;
        sRecoverStarted = now;
        sSpinKey = aim_err > 0 ? 1 : -1;     // A turns left, anticlockwise (as the placing step)
        sSpinFlipped = false;
        sSpinThenBack = then_back;
        sSpinTapNext = now;
        sSpinTapUntil = 0.0;
        sSpinCheckAt = now;
        sSpinCheckErr = std::fabs(aim_err);
    }

    void beginBackOff(F64 now, const LLVector3d& pos, F64 yaw)
    {
        sRecoverPhase = 1;
        sRecoverStep = "back off";
        sRecoverYaw0 = yaw;
        sRecoverAt = now;
        sRecoverStarted = now;
        sRecoverFrom = pos;
        sRecoverChecked = false;
        // backing, a key turns the nose one way or the other; first guess: as a real car, the wheel turned away
        // from where the nose should go
        const F64 aim = recoverAim(pos, yaw);
        const F64 err = wrap(aim - yaw);
        sRecoverKey = err > 0 ? sRecoverKeyLeft : -sRecoverKeyLeft;
        sRecoverCheckAt = now + RECOVER_JUDGE_S;
        sRecoverCheckErr = std::fabs(err) * 180 / PI;
        sRecoverAbort = false;
        sRecoverFlips = 0;
        LL_INFOS("SKRoadDriver") << "back off: heading " << S32(std::lround(yaw * 180 / PI)) << " deg, lane "
                                 << S32(std::lround(aim * 180 / PI)) << " deg (" << S32(std::lround(err * 180 / PI))
                                 << " off), steering the nose " << (err > 0 ? "left" : "right") << " while backing with "
                                 << (sRecoverKey > 0 ? "A" : "D") << LL_ENDL;
    }

    // Stuck (or frozen) here: the next rung of the ladder for this spot. False if it gave up (the autopilot off) or
    // handed the car to the three-point turn.
    bool beginRecovery(const char* why, F64 now, const LLVector3d& pos, F64 yaw)
    {
        while (!sRecoverTimes.empty() && sRecoverTimes.front() < now - LADDER_MAX_WINDOW_S)
        {
            sRecoverTimes.pop_front();
        }
        sRecoverTimes.push_back(now);
        const F64 from_last = std::hypot(pos.mdV[VX] - sLadderAt.mdV[VX], pos.mdV[VY] - sLadderAt.mdV[VY]);
        if (sLadderRung < 0 || from_last > LADDER_SPOT_M || now - sLadderLast > LADDER_FORGET_S)
        {
            sLadderRung = 0;
            sLadderAt = pos;
        }
        else
        {
            ++sLadderRung;
        }
        sLadderLast = now;
        const F64 aim = recoverAim(pos, yaw);
        const F64 aim_err = wrap(aim - yaw);
        const F64 off_deg = std::fabs(aim_err) * 180 / PI;
        LL_INFOS("SKRoadDriver") << "stuck (" << why << ") at " << placeText(pos) << ": rung " << sLadderRung << " here, "
                                 << sRecoverTimes.size() << " in " << S32(LADDER_MAX_WINDOW_S) << " s; heading "
                                 << S32(std::lround(yaw * 180 / PI)) << " deg, aim " << S32(std::lround(aim * 180 / PI))
                                 << " deg (" << S32(std::lround(off_deg)) << " off)" << LL_ENDL;
        if (sLadderRung >= LADDER_TOP || sRecoverTimes.size() >= size_t(LADDER_MAX_N))
        {
            say(sLadderRung >= LADDER_TOP ? "stuck at the same spot after turning, backing off and turning round: stopping"
                                          : "stuck " + std::to_string(LADDER_MAX_N) + " times in "
                                                + std::to_string(S32(LADDER_MAX_WINDOW_S)) + " s: stopping");
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            sRecoverTimes.clear();
            sLadderRung = -1;
            return false;
        }
        sRecoverStarted = now;
        sStuckSince = -1.0;
        sStuckTrail.clear();
        if (sLadderRung == 0 && off_deg > SPIN_MIN_DEG)
        {
            // stopped facing well off the road (the kerb, across a junction, past a bend): turned to face along the
            // lane first, never backed
            say(std::string(why) + ": turning on the spot to face the road");
            beginSpin(now, yaw, aim_err, true);
            return true;
        }
        if (sLadderRung <= 1)
        {
            say(std::string(why) + ": backing off");
            beginBackOff(now, pos, yaw);
            return true;
        }
        // backed off here already and stuck again: turned round three-point to the way travelled
        sRecoverPhase = 0;
        sRecoverStep = "three-point turn";
        LL_INFOS("SKRoadDriver") << "recovery three-point turn: heading " << S32(std::lround(yaw * 180 / PI)) << " deg" << LL_ENDL;
        startTurnRound(false, pos, yaw, now, std::string(why) + " again here: turning round");
        return false;
    }

    bool recovering(U32 keys, F64 speed, F64 now, const LLVector3d& pos, F64 yaw, std::string& note)
    {
        auto tag = [&](const char* t) { note += note.empty() ? t : std::string(", ") + t; };
        if (sPoseChangedAt < 0 || pos != sPoseSeen || yaw != sYawSeen)
        {
            sFrozenTried = false;       // the car has moved: the frozen stop may try its recovery again
        }
        // only with W held: a car at rest with no keys (waiting for the road) is sent no updates either
        if (sPoseChangedAt < 0 || pos != sPoseSeen || yaw != sYawSeen || !(keys & AGENT_CONTROL_AT_POS))
        {
            sPoseSeen = pos;
            sYawSeen = yaw;
            sPoseChangedAt = now;
            sFrozenSaid = false;
        }
        if (sRecoverPhase == 0 && now - sPoseChangedAt > FROZEN_S)
        {
            // no updates for the car: neither stuck nor anything the keys can mend
            sStuckSince = -1.0;
            sStuckTrail.clear();
            if (!sFrozenSaid)
            {
                sFrozenSaid = true;
                say("the car's position has stopped updating: waiting");
            }
            // a car pinned still is sent no updates either: once, while seated and W held, treated as stuck (turned
            // on the spot or backed off) before the stop below
            if (!sFrozenTried && now - sPoseChangedAt > FROZEN_RECOVER_S && isAgentAvatarValid() && gAgentAvatarp->isSitting())
            {
                sFrozenTried = true;
                sFrozenSaid = false;
                if (beginRecovery("no position updates, maybe pinned", now, pos, yaw))
                {
                    return recovering(keys, speed, now, pos, yaw, note);
                }
                return false;
            }
            if (now - sPoseChangedAt > FROZEN_GIVE_UP_S)
            {
                say("the car's position has not updated in " + std::to_string(S32(FROZEN_GIVE_UP_S)) + " s: stopping");
                gSavedSettings.setBOOL("SKRoadAutopilot", false);
                sPoseChangedAt = -1.0;
            }
            tag("frozen");
            return false;
        }
        if (sRecoverPhase == 0)
        {
            // stuck: W held and under STUCK_MOVED_M moved in STUCK_WINDOW_S (grinding on a rail the speed seen
            // jitters 0-1.2 m/s, and a speed test that any jitter reset never fired)
            const bool pushing = (keys & AGENT_CONTROL_AT_POS) != 0;
            sStuckTrail.push_back({ now, pos });
            while (sStuckTrail.size() > 1 && sStuckTrail.front().first < now - STUCK_WINDOW_S)
            {
                sStuckTrail.pop_front();
            }
            if (!pushing)
            {
                sStuckSince = -1.0;
                sStuckTrail.clear();
                return false;
            }
            if (sStuckSince < 0)
            {
                sStuckSince = now;
            }
            const LLVector3d& first = sStuckTrail.front().second;
            const F64 moved = std::hypot(pos.mdV[VX] - first.mdV[VX], pos.mdV[VY] - first.mdV[VY]);
            if (now - sStuckSince < STUCK_WINDOW_S || moved >= STUCK_MOVED_M)
            {
                return false;
            }
            sStuckTrail.clear();
            if (!beginRecovery("stuck against something", now, pos, yaw))
            {
                return false;
            }
        }
        if (now - sRecoverStarted > RECOVER_GIVE_UP_S)
        {
            // the whole recovery has taken too long: stopped, for the mission to take the car back
            sRecoverPhase = 0;
            say("could not get unstuck: stopping");
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            return false;
        }
        const F64 aim_err = wrap(recoverAim(pos, yaw) - yaw);
        // where the nose will be once the swing it carries dies away (it turns on ~1 s after the key is let go:
        // Soulgiver, a back-off from 50 deg off swung through the lane to 73 deg off the other way)
        const F64 aim_err_led = aim_err - sYawRate * RECOVER_LEAD_S;
        const F64 backed = std::hypot(pos.mdV[VX] - sRecoverFrom.mdV[VX], pos.mdV[VY] - sRecoverFrom.mdV[VY]);
        U32 k = 0;
        auto steer = [&]()
        {
            if (std::fabs(aim_err) * 180 / PI < 8.0 || std::fabs(aim_err_led) * 180 / PI < 8.0
                || (aim_err > 0) != (aim_err_led > 0))
            {
                return;     // there already, or the swing carries it there: the key let go
            }
            // checked while the car moves, every RECOVER_CHECK_S: the nose should swing toward the lane; if it swings
            // away, the other key (twice at most: S and Reverse may steer differently on one car)
            if (now >= sRecoverCheckAt)
            {
                const F64 off = std::fabs(aim_err) * 180 / PI;
                const F64 grew = off - sRecoverCheckErr;
                const char* verdict = "holding";
                if (speed < 0.3)
                {
                    verdict = "too slow to judge";
                }
                else if (grew < -RECOVER_JUDGE_DEG)
                {
                    verdict = "closing";
                    sRecoverKeyLeft = aim_err > 0 ? sRecoverKey : -sRecoverKey;
                }
                else if (grew > RECOVER_JUDGE_DEG)
                {
                    if (sRecoverFlips < 1)
                    {
                        ++sRecoverFlips;
                        sRecoverKey = -sRecoverKey;
                        verdict = "growing, the other key";
                    }
                    else
                    {
                        sRecoverAbort = true;
                        verdict = "growing after the swap, stop backing";
                    }
                }
                LL_INFOS("SKRoadDriver") << "back off check: yaw " << S32(std::lround(yaw * 180 / PI)) << " deg, rate "
                                         << S32(std::lround(sYawRate * 180 / PI)) << " deg/s, error "
                                         << S32(std::lround(aim_err * 180 / PI)) << " deg (was "
                                         << S32(std::lround(sRecoverCheckErr)) << " off), speed " << speed << ", key "
                                         << (sRecoverKey > 0 ? "A" : "D") << ": " << verdict << LL_ENDL;
                if (speed >= 0.3)
                {
                    sRecoverChecked = true;
                    sRecoverCheckErr = off;
                }
                sRecoverCheckAt = now + RECOVER_JUDGE_S;
            }
            k |= sRecoverKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
        };
        // asked where it is used, not once up here: Reverse engaging resets sRecoverAt and sRecoverFrom in the same
        // call, and a verdict taken before that went on the S phase's clock (4 s gone at Dorje): the Reverse phase
        // ended the frame it began, and the car went back into Gear 1 having backed nothing
        auto done = [&]()
        {
            const F64 moved = std::hypot(pos.mdV[VX] - sRecoverFrom.mdV[VX], pos.mdV[VY] - sRecoverFrom.mdV[VY]);
            // done once the nose is near the lane's heading, not at a fixed distance: past RECOVER_BACK_M only while
            // still swinging round, up to RECOVER_BACK_MAX_M
            const F64 off = std::min(std::fabs(aim_err), std::fabs(aim_err_led)) * 180 / PI;
            return sRecoverAbort || (off < RECOVER_ALIGN_DEG && moved > 1.0) || (moved >= RECOVER_BACK_M && off < REORIENT_DEG)
                   || moved >= RECOVER_BACK_MAX_M || now - sRecoverAt > RECOVER_MAX_S;
        };
        // a back-off over: logged, and driven on only facing near enough along the lane; still over REORIENT_DEG off
        // the aim picked again from here (the lane ahead, not the line it was stuck on), turned on the spot first
        auto finished = [&](const char* how)
        {
            logRecoverStep(how, yaw, pos);
            sRecoverPhase = 0;
            sStuckSince = -1.0;
            sStuckTrail.clear();
            LL_INFOS("SKRoadDriver") << "back off over in gear " << sGear << ": "
                                     << S32(std::lround(std::fabs(aim_err) * 180 / PI)) << " deg off the lane ("
                                     << (std::fabs(aim_err) * 180 / PI > REORIENT_DEG ? "turning on the spot" : "driving on")
                                     << ")" << LL_ENDL;
            if (std::fabs(aim_err) * 180 / PI > REORIENT_DEG)
            {
                LL_INFOS("SKRoadDriver") << "backed off still " << S32(std::lround(std::fabs(aim_err) * 180 / PI))
                                         << " deg off the lane: turning on the spot first" << LL_ENDL;
                beginSpin(now, yaw, aim_err, false);
                tag("stuck: re-orienting");
                sRecoverKeys = 0;
                return true;
            }
            tag(how);
            return false;
        };
        if (sRecoverPhase == 5)
        {
            // turning on the spot: stopped first (nothing held), then A/D taps toward the aim
            const F64 off = std::fabs(aim_err) * 180 / PI;
            if (off < SPIN_DONE_DEG)
            {
                logRecoverStep("faced the road", yaw, pos);
                sRecoverPhase = 0;
                sStuckSince = -1.0;
                sStuckTrail.clear();
                tag("turned on the spot");
                return false;
            }
            if (now - sSpinCheckAt > SPIN_CHECK_S)
            {
                // not coming round: the other key once (the shorter way may be blocked, or the key the other way
                // on this car), then backing off
                const bool progress = sSpinCheckErr - off > SPIN_PROGRESS_DEG;
                sSpinCheckAt = now;
                sSpinCheckErr = off;
                if (!progress)
                {
                    if (!sSpinFlipped)
                    {
                        sSpinFlipped = true;
                        sSpinKey = -sSpinKey;
                        LL_INFOS("SKRoadDriver") << "turn on the spot: not turning, the other key" << LL_ENDL;
                    }
                    else
                    {
                        logRecoverStep("would not turn", yaw, pos);
                        if (sSpinThenBack)
                        {
                            say("would not turn on the spot: backing off");
                            beginBackOff(now, pos, yaw);
                            return recovering(keys, speed, now, pos, yaw, note);
                        }
                        sRecoverPhase = 0;
                        sStuckSince = -1.0;
                        tag("would not turn");
                        return false;
                    }
                }
            }
            if (now - sRecoverAt > SPIN_MAX_S)
            {
                logRecoverStep("timed out", yaw, pos);
                if (sSpinThenBack)
                {
                    say("turning on the spot took too long: backing off");
                    beginBackOff(now, pos, yaw);
                    return recovering(keys, speed, now, pos, yaw, note);
                }
                sRecoverPhase = 0;
                sStuckSince = -1.0;
                tag("turn on the spot: timed out");
                return false;
            }
            if (speed < SPIN_STOPPED_MPS)
            {
                if (now >= sSpinTapNext)
                {
                    sSpinTapUntil = now + SPIN_TAP_S;
                    sSpinTapNext = now + SPIN_TAP_EVERY_S;
                }
                if (now < sSpinTapUntil)
                {
                    k |= sSpinKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                }
            }
            tag("stuck: turning on the spot");
            sRecoverKeys = k;
            return true;
        }
        if (sRecoverPhase == 1)
        {
            // S: backs up where S reverses; where it only brakes the car does not move, and Reverse is tried
            if (!backClear(pos, yaw, speed))
            {
                return finished("unstuck: no room behind");
            }
            if (done() && backed > 0.5)
            {
                return finished("unstuck");
            }
            // S has not backed it off: into Reverse (also once its time is up having moved under half a metre: moved
            // 0.3-0.5 m and stopped, it held S for 500 s, neither done nor handed on)
            if ((now - sRecoverAt > RECOVER_S_TRY_S && backed < 0.3) || (now - sRecoverAt > RECOVER_MAX_S && backed <= 0.5))
            {
                sRecoverPhase = 2;
                sRecoverShiftAt = -100.0;
            }
            else
            {
                k |= AGENT_CONTROL_AT_NEG;
                steer();
                tag("stuck: backing (S)");
                sRecoverKeys = k;
                return true;
            }
        }
        if (sRecoverPhase == 2)
        {
            // down into Reverse (the car says it), a shift a second
            if (sGear < 0)
            {
                sRecoverPhase = 3;
                sRecoverAt = now;
                sRecoverFrom = pos;
                sRecoverChecked = false;
                sRecoverCheckAt = now + RECOVER_JUDGE_S;
                sRecoverCheckErr = std::fabs(aim_err) * 180 / PI;
            }
            else
            {
                if (now - sRecoverShiftAt > 1.0)
                {
                    sRecoverShiftAt = now;
                }
                if (now - sRecoverShiftAt < SHIFT_HOLD_S)
                {
                    k |= shiftKey(-1);
                }
                tag("stuck: into Reverse");
                sRecoverKeys = k;
                return true;
            }
        }
        if (sRecoverPhase == 3)
        {
            if (done() || !backClear(pos, yaw, speed))
            {
                sRecoverPhase = 4;
                sRecoverShiftAt = -100.0;
            }
            else
            {
                k |= AGENT_CONTROL_AT_POS;
                steer();
                tag("stuck: backing (Reverse)");
                sRecoverKeys = k;
                return true;
            }
        }
        // back up into Gear 1, then the steering has it again
        if (sGear >= 1)
        {
            return finished("unstuck");
        }
        if (now - sRecoverShiftAt > 1.0)
        {
            sRecoverShiftAt = now;
        }
        if (now - sRecoverShiftAt < SHIFT_HOLD_S)
        {
            k |= shiftKey(1);
        }
        tag("stuck: into Gear 1");
        sRecoverKeys = k;
        return true;
    }

    // ---------------------------------------------------------------- the way travelled, and turning round

    void trailClear(const char* why)
    {
        if (!sTrail.empty())
        {
            LL_INFOS("SKRoadDriver") << "trail forgotten (" << why << ")" << LL_ENDL;
        }
        sTrail.clear();
        sTrailTurnCount = sTrailCount;
        sTravelSet = false;
    }

    F64 travelYaw()
    {
        return std::atan2(sTravelY, sTravelX);
    }

    // The trail, and the way travelled: only while the car goes forward under its own steering (not backing off
    // a stuck, not turning round), and the way follows the car's motion only within TRAVEL_FOLLOW_DEG of it, so a
    // junction's turn is followed and a car that has ended up pointing elsewhere is not.
    constexpr F64 TRAVEL_FOLLOW_DEG = 75.0, TRAVEL_BLEND = 0.2;
    void noteTrail(const LLVector3d& pos, F64 yaw, F64 speed, const LLVector3& vel, bool manoeuvring)
    {
        if (!sTrail.empty()
            && std::hypot(pos.mdV[VX] - sTrail.back().x, pos.mdV[VY] - sTrail.back().y) > TRAIL_JUMP_M + speed)
        {
            trailClear("the car jumped: a teleport or a rez");
        }
        if (!sTravelSet)
        {
            sTravelX = std::cos(yaw);
            sTravelY = std::sin(yaw);
            sTravelSet = true;
            sTrailTurnCount = sTrailCount;
        }
        if (manoeuvring || speed < 1.0 || vel.mV[VX] * std::cos(yaw) + vel.mV[VY] * std::sin(yaw) <= 0)
        {
            return;
        }
        if (sTrail.empty() || std::hypot(pos.mdV[VX] - sTrail.back().x, pos.mdV[VY] - sTrail.back().y) >= TRAIL_STEP_M)
        {
            sTrail.push_back({ pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ] });
            ++sTrailCount;
            while (F64(sTrail.size()) * TRAIL_STEP_M > TRAIL_MAX_M)
            {
                sTrail.pop_front();
            }
        }
        if (speed > 2.0)
        {
            const F64 vx = vel.mV[VX] / speed, vy = vel.mV[VY] / speed;
            if (vx * sTravelX + vy * sTravelY > std::cos(TRAVEL_FOLLOW_DEG * PI / 180))
            {
                const F64 nx = sTravelX * (1 - TRAVEL_BLEND) + vx * TRAVEL_BLEND, ny = sTravelY * (1 - TRAVEL_BLEND) + vy * TRAVEL_BLEND;
                const F64 L = std::hypot(nx, ny);
                if (L > 1e-6)
                {
                    sTravelX = nx / L;
                    sTravelY = ny / L;
                }
            }
        }
    }

    // A line whose start runs back over the trail driven since the last deliberate turn (within TRAIL_NEAR_M of it,
    // against its way): the walk found the road the car came along, the wrong way.
    constexpr F64 TRAIL_NEAR_M = 3.0, TRAIL_BACK_LOOK_M = 400.0;
    bool lineBackOverTrail(const std::vector<LanePoint>& line)
    {
        if (line.size() < 4 || sTrail.size() < 4)
        {
            return false;
        }
        const U64 first_global = sTrailCount - sTrail.size();
        size_t lo = sTrailTurnCount > first_global ? size_t(sTrailTurnCount - first_global) : 0;
        lo = std::max<size_t>(lo, sTrail.size() > size_t(TRAIL_BACK_LOOK_M / TRAIL_STEP_M) ? sTrail.size() - size_t(TRAIL_BACK_LOOK_M / TRAIL_STEP_M) : 0);
        if (lo + 2 >= sTrail.size())
        {
            return false;
        }
        S32 back = 0, checked = 0;
        for (size_t i = 0; i < std::min<size_t>(line.size(), 8); ++i)
        {
            const LanePoint& q = line[i];
            F64 best = TRAIL_NEAR_M * TRAIL_NEAR_M;
            size_t at = sTrail.size();
            for (size_t k = lo; k + 1 < sTrail.size(); ++k)
            {
                const F64 d = (sTrail[k].x - q.x) * (sTrail[k].x - q.x) + (sTrail[k].y - q.y) * (sTrail[k].y - q.y);
                if (d < best)
                {
                    best = d;
                    at = k;
                }
            }
            if (at == sTrail.size())
            {
                continue;
            }
            ++checked;
            const F64 tx = sTrail[at + 1].x - sTrail[at].x, ty = sTrail[at + 1].y - sTrail[at].y, L = std::hypot(tx, ty);
            if (L > 1e-3 && (q.ax * tx + q.ay * ty) / L < -0.5)
            {
                ++back;
            }
        }
        return checked >= 3 && back * 2 > checked;
    }

    // ---------------------------------------------------------------- room behind

    // What the car would stand on at a point: the road or the air's floor near near_z, else the land.
    F64 groundAt(F64 px, F64 py, F64 near_z)
    {
        F64 h = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(px, py, 0));
        F64 top, clear;
        if (roadTopAt(px, py, near_z, 0.6, 30.0, top))
        {
            h = std::max(h, top);
        }
        if (airFloor(px, py, near_z, 0.6, 30.0, top, clear))
        {
            h = std::max(h, top);
        }
        return h;
    }

    // A drop at a point for a car on the road at road_z: the ground more than BACK_DROP_M below it, or water more
    // than BACK_WATER_M below it over the ground.
    constexpr F64 BACK_DROP_M = 2.5, BACK_WATER_M = 1.0;
    bool dropAt(F64 px, F64 py, F64 road_z)
    {
        const F64 g = groundAt(px, py, road_z + 0.5);
        if (road_z - g > BACK_DROP_M)
        {
            return true;
        }
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(LLVector3d(px, py, road_z));
        const F64 water = region ? region->getWaterHeight() : -1e9;
        return water > g + 0.2 && road_z - water > BACK_WATER_M;
    }

    // Whether the car may back: behind both rear corners, out to its stopping distance backing at its speed and at
    // least BACK_CLEAR_M, no drop or water (Fujin: the turn round's reverse leg backed off the ramp's side into the
    // water, z 32.7 to 20.2), checked as the forward guards check ahead.
    constexpr F64 BACK_CLEAR_M = 1.5, BACK_STEP_M = 0.5;
    bool backClear(const LLVector3d& pos, F64 yaw, F64 speed)
    {
        const F64 x = pos.mdV[VX], y = pos.mdV[VY], z = pos.mdV[VZ];
        const F64 road_z = groundAt(x, y, z);
        if (z - road_z > 3.0)
        {
            return true;        // on nothing known
        }
        const F64 hx = std::cos(yaw), hy = std::sin(yaw);
        const F64 reach = BACK_CLEAR_M + speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S;
        for (const F64 side : { -sCarHalfW, sCarHalfW })
        {
            const F64 cx = x - hx * sCarRear - hy * side, cy = y - hy * sCarRear + hx * side;
            for (F64 d = 0; d <= reach + 1e-6; d += BACK_STEP_M)
            {
                if (dropAt(cx - hx * d, cy - hy * d, road_z))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Room for a three-point turn where the car is: the road there TURN_MIN_ROAD_W wide or more, with no drop or water
    // TURN_ACROSS_M either side of its middle (with no line, room).
    constexpr F64 TURN_MIN_ROAD_W = 6.5, TURN_ACROSS_M = 3.0;
    bool turnRoom(const LLVector3d& pos)
    {
        if (sPath.size() < 2)
        {
            return true;
        }
        size_t at = 0;
        F64 best = 1e18;
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            const F64 d = std::hypot(sPath[i].x - pos.mdV[VX], sPath[i].y - pos.mdV[VY]);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        const LanePoint& q = sPath[at];
        if (q.road_w >= OPEN_W)
        {
            return true;
        }
        if (q.road_w < TURN_MIN_ROAD_W)
        {
            return false;
        }
        const F64 al = std::max(1e-6, std::hypot(q.ax, q.ay));
        const F64 mx = (q.left_x + q.right_x) / 2, my = (q.left_y + q.right_y) / 2, road_z = groundAt(mx, my, pos.mdV[VZ]);
        for (const F64 s : { -TURN_ACROSS_M, TURN_ACROSS_M })
        {
            if (dropAt(mx - q.ay / al * s, my + q.ax / al * s, road_z))
            {
                return false;
            }
        }
        return true;
    }

    // The trail point TRAIL_AIM_M back along the trail from where the car is on it, where that lies behind the car.
    constexpr F64 TRAIL_AIM_M = 6.0;
    bool trailAimBehind(const LLVector3d& pos, F64 yaw, F64& ax, F64& ay)
    {
        S32 at = -1;
        F64 best = 1e18;
        for (S32 i = 0; i < S32(sTrail.size()); ++i)
        {
            const F64 d = std::hypot(sTrail[i].x - pos.mdV[VX], sTrail[i].y - pos.mdV[VY]);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        if (at < 0 || best > 6.0)
        {
            return false;
        }
        F64 run = 0;
        S32 i = at;
        while (i > 0 && run < TRAIL_AIM_M)
        {
            run += std::hypot(sTrail[i].x - sTrail[i - 1].x, sTrail[i].y - sTrail[i - 1].y);
            --i;
        }
        ax = sTrail[i].x;
        ay = sTrail[i].y;
        return run > 1.0 && (ax - pos.mdV[VX]) * std::cos(yaw) + (ay - pos.mdV[VY]) * std::sin(yaw) < 0;
    }

    // Turning round, three-point: stopped first, then forward with the turn toward the way wanted, backing in
    // Reverse with the opposite lock (never S with a turn key: braking with one held, this car turns the other
    // way), and forward again, until the nose is within TURN_DONE_DEG of the way wanted. For a dead end (the way
    // travelled itself turned round) and for a car that a recovery left pointing back against the way travelled.
    constexpr F64 TURN_DONE_DEG = 25.0, TURN_FWD_MAX_M = 6.0, TURN_BACK_MAX_M = 5.0, TURN_LEG_S = 5.0, TURN_STALL_S = 1.5;
    constexpr F64 TURN_GIVE_UP_S = 45.0, TURN_SHIFT_WAIT_S = 6.0, TURN_CREEP_MPS = 3.0, TURN_ROOM_M = 2.5;
    constexpr F64 FACING_BACK_DEG = 110.0;
    // Where the road is too narrow for that (turnRoom) and the trail lies behind, the car first backs straight along
    // the trail, up to TRAIL_BACK_MAX_M, to where it is wide enough, then turns. Every backing leg stops where the
    // room behind is not clear (backClear).
    constexpr F64 TRAIL_BACK_MAX_M = 40.0, TRAIL_BACK_MPS = 2.0;
    S32 sTurnPhase = 0;         // 0 not, 1 stopping, 2 forward with the turn, 3 into Reverse, 4 backing, 5 into Gear 1,
                                // 6 backing along the trail, 7 turning on the spot, 8 a short straight back-off
                                // before turning on the spot
    // A leg that cannot move (no room ahead or behind, the back not clear, against something) is a blocked leg.
    // Arpeggio: against a light pole on a bend every leg was refused the instant it began, and the turn round
    // flipped Gear 1 <-> Reverse a few times a second for 45 s. Now a leg refused at its start, TURN_BLOCKED_MAX
    // blocked legs in a row, or TURN_NO_PROGRESS_LEGS legs without the heading TURN_PROGRESS_DEG nearer the way
    // wanted fall back to turning on the spot (this car turns on the spot: A/D taps, the other key if one way is
    // blocked); a spin that will not turn backs off straight once (TURN_SPIN_BACK_M) and spins again, and one that
    // still will not ends the run. Gear changes are at least TURN_GEAR_GAP_S apart.
    constexpr S32 TURN_BLOCKED_MAX = 2, TURN_NO_PROGRESS_LEGS = 3;
    constexpr F64 TURN_PROGRESS_DEG = 10.0, TURN_GEAR_GAP_S = 1.5, TURN_LEG_MOVED_M = 0.3;
    constexpr F64 TURN_SPIN_BACK_M = 1.2, TURN_SPIN_BACK_S = 2.5;
    S32 sTurnBlocked = 0, sTurnLegs = 0;
    F64 sTurnBestErr = 180.0, sTurnLegErr0 = 0.0, sTurnGearAt = -100.0;
    bool sTurnSpinBacked = false;
    bool sTurnTrail = false;    // the backing leg is along the trail
    S32 sBackPosYaw = -1;       // backing, the yaw rate's sign with the left key held (learnt; first guess a real car's)
    S32 sTrailKey = 0;          // the turn key held backing along the trail last frame: + left
    S32 sTurnSide = 1;          // + left
    S32 sTurnBackKey = -1;      // the key that swings the nose toward the aim backing (checked, as recovering's)
    bool sTurnChecked = false;
    bool sTurnBackWithS = false;   // Reverse never came: S backs it instead
    F64 sTurnStarted = 0.0, sTurnAt = 0.0, sTurnShiftAt = -100.0, sTurnStallAt = 0.0;
    LLVector3d sTurnFrom, sTurnStallFrom;

    const char* turnPhaseName(S32 phase)
    {
        static const char* names[] = { "none", "stopping", "forward", "into Reverse", "backing", "into Gear 1",
                                       "backing along the trail", "on the spot", "backing off to spin" };
        return phase >= 0 && phase <= 8 ? names[phase] : "?";
    }

    F64 sTurnYaw = 0.0;     // the heading this frame, for the leg's log line

    // The leg ending is logged with its outcome: how far it moved, how long, the heading's error before and after.
    void beginTurnLeg(S32 phase, const LLVector3d& pos, F64 now, const std::string& outcome = std::string())
    {
        if (sTurnPhase != 0)
        {
            const F64 moved = std::hypot(pos.mdV[VX] - sTurnFrom.mdV[VX], pos.mdV[VY] - sTurnFrom.mdV[VY]);
            const F64 err = std::fabs(wrap(travelYaw() - sTurnYaw)) * 180 / PI;
            LL_INFOS("SKRoadDriver") << "turn round leg " << turnPhaseName(sTurnPhase) << " -> " << turnPhaseName(phase)
                                     << ": " << (outcome.empty() ? std::string("ended") : outcome) << ", moved " << moved
                                     << " m in " << (now - sTurnAt) << " s, off the way " << sTurnLegErr0 << " -> " << err
                                     << " deg" << LL_ENDL;
            sTurnLegErr0 = err;
        }
        sTurnPhase = phase;
        sTurnAt = now;
        sTurnFrom = pos;
        sTurnStallAt = now;
        sTurnStallFrom = pos;
        sTurnShiftAt = -100.0;
        sTurnChecked = false;
    }

    // Whether a point lies on the road the line runs along, TURN_ROOM_M short of its edge counted in (with no line,
    // anywhere).
    bool onRoadAt(F64 x, F64 y)
    {
        if (sPath.size() < 2)
        {
            return true;
        }
        size_t at = 0;
        F64 best = 1e18;
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            const F64 d = (sPath[i].x - x) * (sPath[i].x - x) + (sPath[i].y - y) * (sPath[i].y - y);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        const LanePoint& q = sPath[at];
        if (q.road_w >= OPEN_W)
        {
            return true;
        }
        const F64 mx = (q.left_x + q.right_x) / 2, my = (q.left_y + q.right_y) / 2;
        const F64 across = std::fabs((x - mx) * q.ay - (y - my) * q.ax);
        const F64 along = (x - q.x) * q.ax + (y - q.y) * q.ay;
        const bool past_end = (at == 0 && along < -TURN_ROOM_M) || (at + 1 == sPath.size() && along > -TURN_ROOM_M);
        return !past_end && across < std::max(0.5, q.road_w / 2 - 0.3);
    }

    std::string placeText(const LLVector3d& pos)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos);
        if (!region)
        {
            return llformat("%.1f, %.1f", pos.mdV[VX], pos.mdV[VY]);
        }
        const LLVector3 local = region->getPosRegionFromGlobal(pos);
        return llformat("%s %.0f, %.0f, %.0f (global %.1f, %.1f)", region->getName().c_str(), local.mV[VX], local.mV[VY],
                        local.mV[VZ], pos.mdV[VX], pos.mdV[VY]);
    }

    // flip: a dead end, the way travelled turned round with the car (the trail behind it now the way on)
    void startTurnRound(bool flip, const LLVector3d& pos, F64 yaw, F64 now, const std::string& why)
    {
        if (flip)
        {
            sTravelX = -sTravelX;
            sTravelY = -sTravelY;
            sTrailTurnCount = sTrailCount;
            sPath.clear();      // the line to the dead end: walked again the other way
        }
        const F64 err = wrap(travelYaw() - yaw);
        // the way round the shorter one; dead ahead or behind, to the left (across the other lane, on the right)
        sTurnSide = std::fabs(err) > 170 * PI / 180 ? 1 : (err > 0 ? 1 : -1);
        sTurnBackKey = -sTurnSide;      // first guess, as a real car's wheel backing
        sTurnBackWithS = false;
        sTurnTrail = false;
        sTurnStarted = now;
        sTurnBlocked = 0;
        sTurnLegs = 0;
        sTurnSpinBacked = false;
        sTurnGearAt = -100.0;
        sTurnBestErr = std::fabs(err) * 180 / PI;
        sTurnLegErr0 = sTurnBestErr;
        sTurnYaw = yaw;
        sKey = 0;
        beginTurnLeg(1, pos, now);
        say(why);
    }

    void endTurnRound(const LLVector3d& pos, F64 now, const char* how)
    {
        sTurnPhase = 0;
        sStuckSince = -1.0;
        sStuckTrail.clear();
        sKey = 0;
        sPath.clear();          // walked fresh along the way travelled
        LL_INFOS("SKRoadDriver") << "turn round " << how << " at " << placeText(pos) << " after " << (now - sTurnStarted) << " s"
                                 << LL_ENDL;
    }

    // This frame's keys while turning round (true), or false once it is not.
    bool turningRound(F64 speed, F64 now, const LLVector3d& pos, F64 yaw, F64 yaw_rate, U32& keys, std::string& note)
    {
        if (sTurnPhase == 0)
        {
            return false;
        }
        auto tag = [&](const char* t) { note += note.empty() ? t : std::string(", ") + t; };
        sTurnYaw = yaw;
        // the run ended cleanly: the turn round over, the autopilot off
        auto stopRun = [&](const char* how, const std::string& said)
        {
            endTurnRound(pos, now, how);
            say(said);
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            keys = 0;
            return true;
        };
        if (now - sTurnStarted > TURN_GIVE_UP_S)
        {
            endTurnRound(pos, now, "given up");
            say("could not turn round: stopping");
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            keys = 0;
            return true;
        }
        const F64 err = wrap(travelYaw() - yaw);
        const bool aligned = std::fabs(err) * 180 / PI < TURN_DONE_DEG;
        const F64 moved = std::hypot(pos.mdV[VX] - sTurnFrom.mdV[VX], pos.mdV[VY] - sTurnFrom.mdV[VY]);
        // a leg that has stopped moving (against something): on to the next
        if (std::hypot(pos.mdV[VX] - sTurnStallFrom.mdV[VX], pos.mdV[VY] - sTurnStallFrom.mdV[VY]) > 0.3)
        {
            sTurnStallAt = now;
            sTurnStallFrom = pos;
        }
        const bool stalled = now - sTurnAt > TURN_STALL_S && now - sTurnStallAt > TURN_STALL_S;
        const F64 fx = std::cos(yaw), fy = std::sin(yaw);
        const F64 off_deg = std::fabs(err) * 180 / PI;
        U32 k = 0;
        // to turning on the spot, the key first the shorter way round to the way wanted
        auto toSpin = [&](const std::string& why)
        {
            LL_INFOS("SKRoadDriver") << "turn round: " << why << ": turning on the spot" << LL_ENDL;
            sSpinKey = err > 0 ? 1 : -1;
            sSpinFlipped = false;
            sSpinTapNext = now;
            sSpinTapUntil = 0.0;
            sSpinCheckAt = now;
            sSpinCheckErr = off_deg;
            beginTurnLeg(7, pos, now, why);
        };
        const bool room_ahead = onRoadAt(pos.mdV[VX] + fx * TURN_ROOM_M, pos.mdV[VY] + fy * TURN_ROOM_M);
        auto roomBehind = [&]()
        { return onRoadAt(pos.mdV[VX] - fx * TURN_ROOM_M, pos.mdV[VY] - fy * TURN_ROOM_M) && backClear(pos, yaw, 0.0); };
        // a three-point leg ending: blocked where it barely moved; the heading's progress counted over the legs
        auto endLeg = [&](S32 next, const std::string& outcome)
        {
            const bool blocked = moved < TURN_LEG_MOVED_M;
            sTurnBlocked = blocked ? sTurnBlocked + 1 : 0;
            if (sTurnBestErr - off_deg > TURN_PROGRESS_DEG)
            {
                sTurnBestErr = off_deg;
                sTurnLegs = 0;
            }
            else
            {
                ++sTurnLegs;
            }
            if (sTurnBlocked >= TURN_BLOCKED_MAX)
            {
                toSpin(outcome + ", " + std::to_string(sTurnBlocked) + " blocked legs");
            }
            else if (sTurnLegs >= TURN_NO_PROGRESS_LEGS)
            {
                toSpin(outcome + ", no heading progress in " + std::to_string(sTurnLegs) + " legs");
            }
            else
            {
                beginTurnLeg(next, pos, now, outcome + (blocked ? " (blocked)" : ""));
            }
        };
        if (sTurnPhase == 7)
        {
            if (off_deg < TURN_DONE_DEG)
            {
                LL_INFOS("SKRoadDriver") << "turn round leg on the spot: faced the way, off " << off_deg << " deg" << LL_ENDL;
                endTurnRound(pos, now, "done on the spot");
                return false;
            }
            if (now - sSpinCheckAt > SPIN_CHECK_S)
            {
                const bool progress = sSpinCheckErr - off_deg > SPIN_PROGRESS_DEG;
                sSpinCheckAt = now;
                sSpinCheckErr = off_deg;
                if (!progress)
                {
                    if (!sSpinFlipped)
                    {
                        sSpinFlipped = true;
                        sSpinKey = -sSpinKey;
                        LL_INFOS("SKRoadDriver") << "turn round on the spot: not turning, the other key" << LL_ENDL;
                    }
                    else if (!sTurnSpinBacked && backClear(pos, yaw, 0.0))
                    {
                        // held by contact both ways: a short straight back-off (no turn key), then spin again
                        sTurnSpinBacked = true;
                        say("would not turn on the spot: backing off a little first");
                        beginTurnLeg(8, pos, now, "would not turn either way");
                        keys = 0;
                        return true;
                    }
                    else
                    {
                        return stopRun("given up on the spot", "could not turn round, not even on the spot: stopping");
                    }
                }
            }
            if (speed < SPIN_STOPPED_MPS)
            {
                if (now >= sSpinTapNext)
                {
                    sSpinTapUntil = now + SPIN_TAP_S;
                    sSpinTapNext = now + SPIN_TAP_EVERY_S;
                }
                if (now < sSpinTapUntil)
                {
                    k |= sSpinKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                }
            }
            tag("turning round: on the spot");
            keys = k;
            return true;
        }
        if (sTurnPhase == 8)
        {
            // straight back: Reverse's W where in Reverse, else S (no turn key)
            if (moved >= TURN_SPIN_BACK_M || now - sTurnAt > TURN_SPIN_BACK_S || !backClear(pos, yaw, speed))
            {
                toSpin("backed off");
                keys = 0;
                return true;
            }
            if (speed < TURN_CREEP_MPS)
            {
                k |= sGear < 0 ? AGENT_CONTROL_AT_POS : AGENT_CONTROL_AT_NEG;
            }
            tag("turning round: backing off to spin");
            keys = k;
            return true;
        }
        if (sTurnPhase == 1)
        {
            // stopping: off the keys, S (no turn key) to brake while still rolling forward
            if (speed > 2.0 && sGear >= 0)
            {
                k |= AGENT_CONTROL_AT_NEG;
            }
            tag("turning round: stopping");
            if (speed < 0.5 || now - sTurnAt > 8.0)
            {
                if (aligned)
                {
                    endTurnRound(pos, now, "not needed");
                    return false;
                }
                F64 tx, ty;
                if (!turnRoom(pos) && trailAimBehind(pos, yaw, tx, ty) && backClear(pos, yaw, 0.0))
                {
                    // too narrow to turn here: backed along the trail first
                    sTurnTrail = true;
                    say("no room to turn round here: backing along the way the car came");
                    beginTurnLeg(sGear < 0 ? 6 : 3, pos, now);
                }
                else if (!room_ahead && !roomBehind())
                {
                    toSpin("no room ahead or behind");
                }
                else
                {
                    beginTurnLeg(sGear < 0 ? 5 : 2, pos, now, "stopped");
                }
            }
            keys = k;
            return true;
        }
        if (sTurnPhase == 2)
        {
            if (aligned)
            {
                endTurnRound(pos, now, "done");
                return false;
            }
            const bool room = room_ahead;
            if (!room || moved >= TURN_FWD_MAX_M || now - sTurnAt > TURN_LEG_S || stalled)
            {
                endLeg(3, !room ? "no room ahead" : stalled ? "stalled" : moved >= TURN_FWD_MAX_M ? "full length" : "timed out");
                if (sTurnPhase == 7)
                {
                    keys = 0;
                    return true;
                }
            }
            else
            {
                if (speed < TURN_CREEP_MPS)
                {
                    k |= AGENT_CONTROL_AT_POS;
                }
                k |= sTurnSide > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                tag("turning round: forward");
                keys = k;
                return true;
            }
        }
        if (sTurnPhase == 3)
        {
            // down into Reverse, a shift a second; a car that never says Reverse backs with S, no turn key
            if (sGear < 0 || now - sTurnAt > TURN_SHIFT_WAIT_S + TURN_GEAR_GAP_S)
            {
                sTurnBackWithS = sGear >= 0;
                sTurnGearAt = now;
                if (!sTurnTrail && !roomBehind())
                {
                    toSpin("no room behind to back into");
                    keys = 0;
                    return true;
                }
                beginTurnLeg(sTurnTrail ? 6 : 4, pos, now, sTurnBackWithS ? "no Reverse: backing with S" : "in Reverse");
            }
            else
            {
                if (now - sTurnGearAt < TURN_GEAR_GAP_S)
                {
                    tag("turning round: waiting to shift");
                    keys = 0;
                    return true;
                }
                if (now - sTurnShiftAt > 1.0)
                {
                    sTurnShiftAt = now;
                }
                if (now - sTurnShiftAt < SHIFT_HOLD_S)
                {
                    k |= shiftKey(-1);
                }
                tag("turning round: into Reverse");
                keys = k;
                return true;
            }
        }
        if (sTurnPhase == 6)
        {
            F64 tx, ty;
            const bool aim = trailAimBehind(pos, yaw, tx, ty);
            if (!aim || turnRoom(pos) || !backClear(pos, yaw, speed) || moved >= TRAIL_BACK_MAX_M || now - sTurnAt > 20.0 || stalled)
            {
                sTurnTrail = false;
                beginTurnLeg(5, pos, now, "trail leg ended");
            }
            else
            {
                if (speed < TRAIL_BACK_MPS)
                {
                    k |= sTurnBackWithS ? AGENT_CONTROL_AT_NEG : AGENT_CONTROL_AT_POS;
                }
                if (!sTurnBackWithS)
                {
                    // the tail toward the trail: the yaw rate wanted has the sign of the tail's error
                    const F64 tail_err = wrap(std::atan2(ty - pos.mdV[VY], tx - pos.mdV[VX]) - (yaw + PI));
                    if (sTrailKey && speed > 0.5 && std::fabs(yaw_rate) * 180 / PI > 3.0)
                    {
                        sBackPosYaw = (yaw_rate > 0 ? 1 : -1) * sTrailKey;
                    }
                    sTrailKey = 0;
                    if (std::fabs(tail_err) * 180 / PI > 4.0)
                    {
                        sTrailKey = (tail_err > 0 ? 1 : -1) * sBackPosYaw;
                        k |= sTrailKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                    }
                }
                tag("turning round: backing along the trail");
                keys = k;
                return true;
            }
        }
        if (sTurnPhase == 4)
        {
            const bool room = onRoadAt(pos.mdV[VX] - fx * TURN_ROOM_M, pos.mdV[VY] - fy * TURN_ROOM_M) && backClear(pos, yaw, speed);
            if (aligned || !room || moved >= TURN_BACK_MAX_M || now - sTurnAt > TURN_LEG_S || stalled)
            {
                endLeg(5, aligned ? "aligned"
                          : !room ? "no room behind"
                          : stalled ? "stalled"
                          : moved >= TURN_BACK_MAX_M ? "full length" : "timed out");
                if (sTurnPhase == 7)
                {
                    keys = 0;
                    return true;
                }
            }
            else
            {
                if (speed < TURN_CREEP_MPS)
                {
                    k |= sTurnBackWithS ? AGENT_CONTROL_AT_NEG : AGENT_CONTROL_AT_POS;
                }
                if (!sTurnBackWithS)
                {
                    // the nose should swing toward the way wanted; checked once it moves, the other key if not
                    if (!sTurnChecked && now - sTurnAt > RECOVER_CHECK_S && speed > 0.5 && std::fabs(yaw_rate) * 180 / PI > 3.0)
                    {
                        sTurnChecked = true;
                        if (yaw_rate * err < 0)
                        {
                            sTurnBackKey = -sTurnBackKey;
                        }
                    }
                    k |= sTurnBackKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
                }
                tag(sTurnBackWithS ? "turning round: backing (S)" : "turning round: backing");
                keys = k;
                return true;
            }
        }
        // sTurnPhase 5: up into Gear 1, then done or forward again
        if (sGear >= 1 || (sTurnBackWithS && sGear >= 0) || now - sTurnAt > TURN_SHIFT_WAIT_S + TURN_GEAR_GAP_S)
        {
            if (aligned)
            {
                endTurnRound(pos, now, "done");
                return false;
            }
            sTurnGearAt = now;
            if (!room_ahead)
            {
                toSpin("no room ahead to drive into");
                keys = 0;
                return true;
            }
            beginTurnLeg(2, pos, now, "in Gear 1");
            keys = 0;
            tag("turning round: forward");
            return true;
        }
        if (now - sTurnGearAt < TURN_GEAR_GAP_S)
        {
            tag("turning round: waiting to shift");
            keys = 0;
            return true;
        }
        if (now - sTurnShiftAt > 1.0)
        {
            sTurnShiftAt = now;
        }
        if (now - sTurnShiftAt < SHIFT_HOLD_S)
        {
            k |= shiftKey(1);
        }
        tag("turning round: into Gear 1");
        keys = k;
        return true;
    }

    // A dead end: the walk ended on the road's last part (walk_end), nothing carries the line on, and the line ends
    // within DEAD_END_M ahead, for DEAD_END_HOLD_S (not a part still loading); stopped DEAD_END_STOP_M short of it.
    constexpr F64 DEAD_END_M = 30.0, DEAD_END_HOLD_S = 1.0, DEAD_END_STOP_M = 7.0, LINE_END_STOP_M = 3.0;
    F64 sDeadEndSince = -1.0;

    // ---------------------------------------------------------------- a drop to water

    // Water where the car would be within its stopping distance, its surface more than WATER_DROP_M below the road
    // the car is on (nothing under that point at the road's height: no road, no air floor, the land below the
    // water): the car stops, whatever width the road was read at (Behrii: the road read 2-3 m wide and the edge
    // guard measured from those edges; the car drove off the bridge's side into the water). The car's footprint
    // is checked, its middle and both sides, from its nose on, straight on and along the line.
    constexpr F64 WATER_DROP_M = 1.0, WATER_NOSE_M = 2.5, WATER_STEP_M = 1.0, WATER_SIDE_M = 0.3, WATER_NOSE_S = 0.5;
    bool waterDropAhead(const LLVector3d& pos, F64 yaw, F64 speed)
    {
        const F64 x = pos.mdV[VX], y = pos.mdV[VY], z = pos.mdV[VZ];
        // what the car stands on: the road or the air's floor under it, else the land
        auto support = [&](F64 px, F64 py, F64 near_z, F64& h)
        {
            h = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(px, py, 0));
            F64 top, clear;
            if (roadTopAt(px, py, near_z, 0.6, 30.0, top))
            {
                h = std::max(h, top);
            }
            if (airFloor(px, py, near_z, 0.6, 30.0, top, clear))
            {
                h = std::max(h, top);
            }
            return h;
        };
        F64 road_z;
        support(x, y, z, road_z);
        if (z - road_z > 3.0)
        {
            return false;       // in the air (a jump), or on nothing known
        }
        const F64 side = sCarHalfW + WATER_SIDE_M;
        auto over_water = [&](F64 cx, F64 cy, F64 ax, F64 ay)
        {
            for (const F64 l : { -side, 0.0, side })
            {
                const F64 px = cx + ay * l, py = cy - ax * l;
                LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(LLVector3d(px, py, road_z));
                if (!region)
                {
                    continue;
                }
                const F64 water = region->getWaterHeight();
                F64 g;
                support(px, py, road_z + 0.5, g);
                if (water > g + 0.2 && road_z - water > WATER_DROP_M)
                {
                    return true;
                }
            }
            return false;
        };
        // straight on along the nose for half a second (drifting off the side), and along the line for the
        // stopping distance (a line laid off the road, over the water)
        const F64 hx = std::cos(yaw), hy = std::sin(yaw);
        const F64 reach = WATER_NOSE_M + std::max(1.0, speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S);
        for (F64 d = WATER_NOSE_M; d <= WATER_NOSE_M + std::max(1.0, speed * WATER_NOSE_S) + 1e-6; d += WATER_STEP_M)
        {
            if (over_water(x + hx * d, y + hy * d, hx, hy))
            {
                return true;
            }
        }
        F64 run = 0;
        for (size_t i = 0; i + 1 < sPath.size() && run < reach; ++i)
        {
            const LanePoint& a = sPath[i];
            const LanePoint& b = sPath[i + 1];
            const F64 L = std::hypot(b.x - a.x, b.y - a.y);
            if (L < 1e-6)
            {
                continue;
            }
            const F64 ax = (b.x - a.x) / L, ay = (b.y - a.y) / L;
            for (F64 t = 0; t < L && run + t < reach; t += WATER_STEP_M)
            {
                const F64 px = a.x + ax * t, py = a.y + ay * t;
                if ((px - x) * hx + (py - y) * hy > WATER_NOSE_M && over_water(px, py, ax, ay))
                {
                    return true;
                }
            }
            if ((b.x - x) * hx + (b.y - y) * hy > 0)
            {
                run += L;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- jumps

    // A jump the model found (View::jump: a ramp's lip with the road going on beyond a gap, Fujin's ramp over the
    // water), with SKRoadAutopilotJumps on, is taken as a driver takes one: from JUMP_COMMIT_M short of the lip, W held and up to the top gear,
    // nothing braking it (the edge guard, short sight); in the air, past the lip until the landing, W alone, no
    // steering; the road picked up again past the landing. Latched, since the walk from the ramp's top or from the
    // air may not see the lip again.
    constexpr F64 JUMP_COMMIT_M = 80.0, JUMP_STRAIGHT_M = 20.0, JUMP_PAST_LAND_M = 5.0, JUMP_AIR_MAX_S = 6.0;
    struct JumpRun
    {
        bool on = false;
        F64 lx = 0, ly = 0, gx = 0, gy = 0, dx = 1, dy = 0;
        F64 left_at = -1.0;     // when it passed the lip
    };
    JumpRun sJump;
    F64 sJumpNeed = 0, sJumpTop = 0;    // the speed the last jump needed and the car's top then (vetLine)

    // Jumps are rare and not driven unless SKRoadAutopilotJumps is on: at a jump with at least PLACE_ROAD_ON_M of
    // road going on past the landing (not a short broken piece in the water), the lip is where the car stops, and
    // it asks to be placed across (wantsPlaceAcross) on the lane PLACE_PAST_LAND_M past the landing. Latched until
    // placed, or the car is PLACE_FORGET_M from the lip.
    constexpr F64 PLACE_ROAD_ON_M = 50.0, PLACE_PAST_LAND_M = 8.0, PLACE_FORGET_M = 60.0;
    struct PlaceRun
    {
        bool want = false, asked = false;
        F64 lip_x = 0, lip_y = 0, lip_dx = 1, lip_dy = 0;
        sk_road_driver::PlaceAcross to;
    };
    PlaceRun sPlace;

    // The jump's keys over `keys`; false where no jump is being taken.
    bool takeJump(bool road, const LLVector3d& pos, F64 yaw, F64 speed, F64 now, U32& keys, std::string& note)
    {
        const F64 x = pos.mdV[VX], y = pos.mdV[VY];
        if (road && sView.jump)
        {
            const F64 ahead = (sView.jump_x - x) * sView.jump_dx + (sView.jump_y - y) * sView.jump_dy;
            if (ahead > 0 && ahead < JUMP_COMMIT_M && (!sJump.on || sJump.left_at < 0))
            {
                if (!sJump.on)
                {
                    LL_INFOS("SKRoadDriver") << "jump ahead at " << placeText(LLVector3d(sView.jump_x, sView.jump_y, sView.jump_z))
                                             << ", landing " << std::hypot(sView.land_x - sView.jump_x, sView.land_y - sView.jump_y)
                                             << " m on" << LL_ENDL;
                    fireEvent("jump", LLSD().with("lip", llsd::array(sView.jump_x, sView.jump_y, sView.jump_z))
                                          .with("land", llsd::array(sView.land_x, sView.land_y, sView.land_z))
                                          .with("need", sJumpNeed).with("top", sJumpTop), pos);
                }
                sJump.on = true;
                sJump.lx = sView.jump_x;
                sJump.ly = sView.jump_y;
                sJump.gx = sView.land_x;
                sJump.gy = sView.land_y;
                sJump.dx = sView.jump_dx;
                sJump.dy = sView.jump_dy;
            }
        }
        if (!sJump.on)
        {
            return false;
        }
        const F64 to_lip = (sJump.lx - x) * sJump.dx + (sJump.ly - y) * sJump.dy;
        const F64 to_land = (sJump.gx - x) * sJump.dx + (sJump.gy - y) * sJump.dy;
        const F64 off_way = std::fabs((x - sJump.lx) * sJump.dy - (y - sJump.ly) * sJump.dx);
        if (to_lip > 0 && sJump.left_at < 0 && (to_lip > JUMP_COMMIT_M + 10.0 || off_way > 15.0))
        {
            sJump = JumpRun();      // gone another way (a junction, a turn round)
            return false;
        }
        if (to_land < -JUMP_PAST_LAND_M || (sJump.left_at >= 0 && now - sJump.left_at > JUMP_AIR_MAX_S))
        {
            LL_INFOS("SKRoadDriver") << "jump done at " << placeText(pos) << LL_ENDL;
            sJump = JumpRun();
            return false;
        }
        keys &= ~AGENT_CONTROL_AT_NEG;
        keys |= AGENT_CONTROL_AT_POS;
        if (to_lip > 0)
        {
            // the run-up: up through the gears to the top, lined up straight in the last JUMP_STRAIGHT_M
            static LLCachedControl<S32> max_gear_c(gSavedSettings, "SKRoadAutopilotMaxGear", 4);
            const S32 top = std::min(sGearMax, S32(max_gear_c) > 0 ? S32(max_gear_c) : 99);
            if (now < sShiftEnd && sShiftDir > 0)
            {
                keys = (keys & ~AGENT_CONTROL_UP_NEG) | shiftKey(1);
            }
            else
            {
                // no shift down (the edge guard and short sight shift down toward the lip)
                keys &= ~(AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG);
                if (sShiftDir < 0)
                {
                    sShiftDir = 0;
                    sShiftEnd = now;
                }
                if (sGear > 0 && sGear < top && now - sLastShift >= SHIFT_GAP_S)
                {
                    sGearBefore = sGear;
                    sShiftDir = 1;
                    sLastShift = now;
                    sShiftEnd = now + SHIFT_HOLD_S;
                    keys |= shiftKey(1);
                }
            }
            if (to_lip < JUMP_STRAIGHT_M)
            {
                // straight at it: a tap only to hold the way across, never one away from it
                const F64 heading_err = wrap(std::atan2(sJump.dy, sJump.dx) - yaw) * 180 / PI;
                if (std::fabs(heading_err) < 3.0)
                {
                    keys &= ~(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
                    sKey = 0;
                }
            }
            note += note.empty() ? "jump: run-up" : ", jump: run-up";
        }
        else
        {
            // in the air (or over the gap): W alone
            if (sJump.left_at < 0)
            {
                sJump.left_at = now;
                LL_INFOS("SKRoadDriver") << "jump: off the lip at " << speed << " m/s in gear " << sGear << LL_ENDL;
            }
            keys = AGENT_CONTROL_AT_POS;
            sKey = 0;
            note += note.empty() ? "jump: in the air" : ", jump: in the air";
        }
        return true;
    }

    // ---------------------------------------------------------------- road not yet loaded

    // The car on Linden land that goes on at least MISSING_AHEAD_M along its heading, and the line ahead shorter
    // than that: the road is there but its pieces have not loaded.
    constexpr F64 MISSING_AHEAD_M = 30.0, MISSING_STEP_M = 3.0;
    constexpr F64 ROAD_PARCEL_MIN_M = 6.0, ROAD_PARCEL_MAX_M = 30.0;
    bool sWaitingForRoad = false;
    F64 sWaitSince = 0;
    constexpr F64 ROAD_WAIT_S = 30.0;
    bool roadNotLoaded(const LLVector3d& pos, F64 yaw)
    {
        if (!lindenLandAt(pos.mdV[VX], pos.mdV[VY]))
        {
            return false;
        }
        const F64 hx = std::cos(yaw), hy = std::sin(yaw);
        F64 strip = 0;
        for (F64 d = MISSING_STEP_M; d <= MISSING_AHEAD_M + 1e-6; d += MISSING_STEP_M)
        {
            if (!lindenLandAt(pos.mdV[VX] + hx * d, pos.mdV[VY] + hy * d))
            {
                break;
            }
            strip = d;
        }
        if (strip < MISSING_AHEAD_M)
        {
            return false;       // the road parcel ends (or turns) within reach: no gap to tell
        }
        // and road-shaped: a strip ROAD_PARCEL_MIN_M to ROAD_PARCEL_MAX_M across, as the Linden road parcels are
        // on the map (a park or a wide Linden plot is not a road whose pieces are missing)
        const F64 rx = hy, ry = -hx;
        F64 across = 0;
        for (S32 side : { -1, 1 })
        {
            F64 d = 0;
            while (d < ROAD_PARCEL_MAX_M && lindenLandAt(pos.mdV[VX] + rx * side * (d + 1), pos.mdV[VY] + ry * side * (d + 1)))
            {
                d += 1;
            }
            across += d;
        }
        if (across < ROAD_PARCEL_MIN_M || across >= ROAD_PARCEL_MAX_M)
        {
            return false;
        }
        // waiting, it starts again once the line reaches the strip's length; driving, it stops when the line is
        // under two thirds of it
        return lineSeen() < (sWaitingForRoad ? MISSING_AHEAD_M : MISSING_AHEAD_M * 2 / 3);
    }

    // A line that would take the car onto refused land (refusedAt) is cut short of it, its end a road end.
    constexpr F64 REFUSE_BACK_M = 4.0, REFUSE_STEP_M = 1.0, REFUSE_EVENT_GAP_S = 5.0, REFUSE_TRUE_END_M = 12.0;

    // the last refused point found ahead: what refused it and how far on, for the edge guard's report
    const char* sRefusedWhy = nullptr;
    F64 sEdgeSince = -1.0;                  // when the edge guard last began to fire
    constexpr F64 EDGE_HOLD_S = 3.0, EDGE_CRAWL_MPS = 2.0, EDGE_CREEP_LINE_M = 8.0;
    F64 sRefusedAtM = 0.0;

    // From the car's nose on, for its stopping distance: a refused point straight ahead (the edge guard's).
    bool refusedAhead(const LLVector3d& pos, F64 yaw, F64 speed)
    {
        const F64 hx = std::cos(yaw), hy = std::sin(yaw);
        const F64 reach = 2.5 + std::max(2.0, speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S);
        for (F64 d = 2.5; d <= reach; d += REFUSE_STEP_M)
        {
            // the road's top looked for at the road's height there (the line's), not the car's: up a slope the road
            // 10 m on stood 2 m above the car, was not found, and every frame read as private land off the road
            const F64 px = pos.mdV[VX] + hx * d, py = pos.mdV[VY] + hy * d;
            if (const char* why = refusedAt(px, py, lineZAt(px, py, pos.mdV[VZ])))
            {
                sRefusedWhy = why;
                sRefusedAtM = d;
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- a jump's speed

    // The speed a jump needs: a projectile off the ramp's lip at the ramp's pitch (read from the line before the lip,
    // JUMP_PITCH_DEG where it says nothing) landing the gap away at the landing's height, with JUMP_SPEED_MARGIN.
    // Fujin's 32 m gap off a 10 degree ramp needs ~30 m/s, as Nexii found. Huge where no speed lands it.
    constexpr F64 GRAVITY = 9.8, JUMP_PITCH_DEG = 10.0, JUMP_PITCH_MIN_DEG = 5.0, JUMP_PITCH_MAX_DEG = 35.0;
    constexpr F64 JUMP_SPEED_MARGIN = 1.1, JUMP_RAMP_M = 8.0;
    F64 jumpPitch()
    {
        // the line's rise over its last JUMP_RAMP_M or more before the lip
        for (size_t i = sPath.size(); i-- > 0;)
        {
            const LanePoint& q = sPath[i];
            const F64 back = (sView.jump_x - q.x) * sView.jump_dx + (sView.jump_y - q.y) * sView.jump_dy;
            if (back >= JUMP_RAMP_M && back < 3 * JUMP_RAMP_M)
            {
                const F64 deg = std::atan2(sView.jump_z - q.z, back) * 180 / PI;
                return deg > 0 ? std::clamp(deg, JUMP_PITCH_MIN_DEG, JUMP_PITCH_MAX_DEG) : JUMP_PITCH_DEG;
            }
        }
        return JUMP_PITCH_DEG;
    }
    F64 jumpNeed()
    {
        const F64 gap = std::hypot(sView.land_x - sView.jump_x, sView.land_y - sView.jump_y);
        const F64 dz = sView.land_z - sView.jump_z;
        const F64 th = jumpPitch() * PI / 180;
        const F64 under = 2 * std::cos(th) * std::cos(th) * (gap * std::tan(th) - dz);
        return under > 1e-6 ? JUMP_SPEED_MARGIN * std::sqrt(GRAVITY * gap * gap / under) : 1e9;
    }
    // The car's top speed: its fastest gear measured, up to the gear the autopilot may use; 0 where none is yet.
    F64 carTopSpeed()
    {
        static LLCachedControl<S32> max_gear_c(gSavedSettings, "SKRoadAutopilotMaxGear", 4);
        F64 top = 0;
        for (const auto& [g, v] : carGears())
        {
            if (g > 0 && (S32(max_gear_c) <= 0 || g <= S32(max_gear_c)))
            {
                top = std::max(top, v);
            }
        }
        return top;
    }


    // The line as read, vetted: cut short of a refused point (a road end only where the model's end is there too),
    // and a jump the car cannot make
    // a road end at its lip.
    void vetLine(const LLVector3d& pos, F64 now)
    {
        static F64 last_refused = -100.0, last_jump_refused = -100.0;
        F64 run = 0;
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            const LanePoint q = sPath[i];
            if (i > 0)
            {
                run += std::hypot(q.x - sPath[i - 1].x, q.y - sPath[i - 1].y);
            }
            const char* why = refusedAt(q.x, q.y, q.z);
            if (!why)
            {
                continue;
            }
            // cut REFUSE_BACK_M short of it
            const F64 keep_to = std::max(0.0, run - REFUSE_BACK_M);
            F64 r = 0;
            size_t last = 0;
            for (size_t k = 1; k < i; ++k)
            {
                const F64 l = std::hypot(sPath[k].x - sPath[k - 1].x, sPath[k].y - sPath[k - 1].y);
                if (r + l > keep_to)
                {
                    break;
                }
                r += l;
                last = k;
            }
            const LanePoint end = sPath[last];
            sPath.resize(last + 1);
            sView.jump = false;     // whatever lay past the cut
            // the line cut, the car held to the road short of it: a line that wandered off the road is no road end,
            // only the model's own end there is (the cut made one of every wander: dead ends and turns round)
            const bool true_end = sView.road_end
                                  && std::hypot(sView.road_end_x - q.x, sView.road_end_y - q.y) < REFUSE_TRUE_END_M;
            sView.road_end = true_end;
            if (true_end)
            {
                sView.road_end_x = end.x;
                sView.road_end_y = end.y;
                const F64 al = std::hypot(end.ax, end.ay);
                const F64 toward = std::atan2(q.y - end.y, q.x - end.x);
                sView.road_end_dx = al > 1e-6 ? end.ax / al : std::cos(toward);
                sView.road_end_dy = al > 1e-6 ? end.ay / al : std::sin(toward);
            }
            sView.walk_end = (true_end ? std::string(WALK_END_ROAD_END) : std::string()) + "refused: " + why;
            sView.source += " (cut: " + std::string(why) + ")";
            if (now - last_refused > REFUSE_EVENT_GAP_S)
            {
                LL_INFOS("SKRoadDriver") << "line refused at " << placeText(LLVector3d(q.x, q.y, q.z)) << ": " << why << LL_ENDL;
                fireEvent("refused_private", LLSD().with("why", why).with("point", llsd::array(q.x, q.y, q.z)), pos);
            }
            last_refused = now;
            break;
        }
        static LLCachedControl<bool> jumps_c(gSavedSettings, "SKRoadAutopilotJumps", false);
        if (sView.jump && !jumps_c)
        {
            // not driven: the lip is a road end, stopped at; past a landing with road going on, placed across
            const F64 lx = sView.land_x, ly = sView.land_y, dx = sView.jump_dx, dy = sView.jump_dy;
            F64 on = 0;
            S32 target = -1;
            for (size_t i = 0; i + 1 < sPath.size(); ++i)
            {
                const LanePoint &a = sPath[i], &b = sPath[i + 1];
                if ((a.x - lx) * dx + (a.y - ly) * dy < 0)
                {
                    continue;
                }
                if (target < 0 && on >= PLACE_PAST_LAND_M)
                {
                    target = S32(i);
                }
                on += std::hypot(b.x - a.x, b.y - a.y);
            }
            sView.jump = false;
            sView.road_end = true;
            sView.road_end_x = sView.jump_x;
            sView.road_end_y = sView.jump_y;
            sView.road_end_dx = dx;
            sView.road_end_dy = dy;
            char text[96];
            if (on >= PLACE_ROAD_ON_M && target >= 0)
            {
                const LanePoint& t = sPath[target];
                const F64 al = std::max(1e-6, std::hypot(t.ax, t.ay));
                if (!sPlace.want)
                {
                    LL_INFOS("SKRoadDriver") << "jump at " << placeText(LLVector3d(sView.jump_x, sView.jump_y, sView.jump_z))
                                             << ": stopping at the lip to be placed across, " << on << " m of road past the landing" << LL_ENDL;
                }
                sPlace.want = true;
                sPlace.lip_x = sView.jump_x;
                sPlace.lip_y = sView.jump_y;
                sPlace.lip_dx = dx;
                sPlace.lip_dy = dy;
                sPlace.to = { t.x, t.y, t.z, t.ax / al, t.ay / al, on };
                snprintf(text, sizeof(text), "jump: placed across (%.0f m of road on)", on);
            }
            else
            {
                snprintf(text, sizeof(text), "jump: only %.0f m of road past the landing", on);
            }
            sView.walk_end = std::string(WALK_END_ROAD_END) + text;
        }
        if (sView.jump)
        {
            sJumpNeed = jumpNeed();
            sJumpTop = carTopSpeed();
            if (sJumpTop > 0 && sJumpTop < sJumpNeed)
            {
                // too far for the car: the lip is the road's end, turned round at
                sView.jump = false;
                sView.road_end = true;
                sView.road_end_x = sView.jump_x;
                sView.road_end_y = sView.jump_y;
                sView.road_end_dx = sView.jump_dx;
                sView.road_end_dy = sView.jump_dy;
                char text[96];
                snprintf(text, sizeof(text), "jump needs %.0f m/s, the car tops %.0f", sJumpNeed, sJumpTop);
                sView.walk_end = std::string(WALK_END_ROAD_END) + text;
                if (now - last_jump_refused > REFUSE_EVENT_GAP_S)
                {
                    LL_INFOS("SKRoadDriver") << "jump refused at " << placeText(LLVector3d(sView.jump_x, sView.jump_y, sView.jump_z))
                                             << ": " << text << LL_ENDL;
                    fireEvent("jump_refused", LLSD().with("need", sJumpNeed).with("top", sJumpTop), pos);
                }
                last_jump_refused = now;
            }
        }
    }

    // ---------------------------------------------------------------- the road's edge

    // Where the car will be EDGE_AHEAD_S on at its speed and heading, outside the road's edges (the cross-
    // section's, as the overlay draws them) by more than EDGE_SLACK_M: a line laid badly must not take it over
    // a drop with no rail.
    constexpr F64 EDGE_AHEAD_S = 0.8, EDGE_SLACK_M = 0.3;
    constexpr F64 DROP_OUT_M = 1.5, DROP_M = 2.5, DROP_MARGIN_M = 1.0;
    S32 sDropSide = 0;      // the last check's drop: -1 left, 1 right, 0 none

    bool offRoadAhead(const LLVector3d& pos, F64 yaw, F64 speed)
    {
        if (sPath.size() < 2 || speed < 2.0)
        {
            return false;
        }
        const F64 px = pos.mdV[VX] + std::cos(yaw) * speed * EDGE_AHEAD_S;
        const F64 py = pos.mdV[VY] + std::sin(yaw) * speed * EDGE_AHEAD_S;
        size_t at = 0;
        F64 best = 1e18;
        for (size_t i = 0; i < sPath.size(); ++i)
        {
            const F64 d = (sPath[i].x - px) * (sPath[i].x - px) + (sPath[i].y - py) * (sPath[i].y - py);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        const LanePoint& q = sPath[at];
        // across the road at that section: + right; the edges' offsets from the line
        const F64 rx = q.ay, ry = -q.ax;
        const F64 off = (px - q.x) * rx + (py - q.y) * ry;
        const F64 left = (q.left_x - q.x) * rx + (q.left_y - q.y) * ry;      // negative: left of the line
        const F64 right = (q.right_x - q.x) * rx + (q.right_y - q.y) * ry;
        if (right - left < 2.0)
        {
            return false;       // no edges known at this section
        }
        // a drop past an edge (the ground DROP_OUT_M beyond it, objects or terrain, more than DROP_M below the
        // road): that edge is kept to DROP_MARGIN_M inside, not EDGE_SLACK_M outside, so a bad line or a slide
        // stops short of a cliff with no rail
        // the road's height there in its own plane: the line's, between its points, not the nearest point's (up a
        // slope that stood a few metres above the road beside the car, and the land beside read as a drop)
        const F64 road_z = lineZAt(px, py, q.z);
        auto drop = [&](F64 edge_off)
        {
            const F64 out = edge_off + (edge_off < 0 ? -DROP_OUT_M : DROP_OUT_M);
            const F64 gx = q.x + rx * out, gy = q.y + ry * out;
            F64 ground = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(gx, gy, 0));
            F64 floor, clear;
            if (airFloor(gx, gy, road_z + 0.5, 0.6, 30.0, floor, clear))
            {
                ground = std::max(ground, floor);
            }
            return ground < road_z - DROP_M;
        };
        const bool drop_left = drop(left), drop_right = drop(right);
        sDropSide = drop_left ? -1 : drop_right ? 1 : 0;
        return off < left + (drop_left ? DROP_MARGIN_M : -EDGE_SLACK_M) || off > right - (drop_right ? DROP_MARGIN_M : -EDGE_SLACK_M);
    }

    bool probeQuiet(const LLVector3d& pos, F64 yaw, bool probe)
    {
        static LLCachedControl<std::string> course(gSavedSettings, "SKRoadAutopilotTestCourse", "");
        if (!probe || std::string(course).empty() || sPath.size() < 2)
        {
            return false;
        }
        const LanePoint& q = sPath[0];
        const F64 e = -(q.ax * (pos.mdV[VY] - q.y) - q.ay * (pos.mdV[VX] - q.x));
        const F64 he = std::fabs(wrap(yaw - std::atan2(q.ay, q.ax))) * 180 / PI;
        return std::fabs(e) < 1.5 && he < 10.0;
    }

    // ---------------------------------------------------------------- the decision

    // Returns the tap to start now: direction (+1 left, -1 right, 0 none) and hold in frames.
    // The slack about the lane line the car may wander in before it is worth a tap: wide on a straight (the
    // lane's room, BAND_STRAIGHT_M at most), narrowing with the path's curvature over the next 20 m to
    // BAND_TIGHT_M in a tight bend (Nexii: the line wider on straights, tighter the more it curves).
    constexpr F64 BAND_STRAIGHT_M = 0.6;
    constexpr F64 BAND_TIGHT_M = 0.15;
    constexpr F64 BAND_CURVE_R_M = 60.0;        // curvature 1/60 m and more: the tight band
    constexpr F64 BAND_W = 1.0, BAND_PULL = 0.03;
    F64 laneBand(const std::vector<LanePoint>& path, size_t at)
    {
        F64 turn = 0, len = 0;
        for (size_t i = at; i + 1 < path.size() && len < 20.0; ++i)
        {
            const F64 a = std::atan2(path[i].ay, path[i].ax), b = std::atan2(path[i + 1].ay, path[i + 1].ax);
            turn = std::max(turn, std::fabs(wrap(b - a)) / std::max(0.5, std::hypot(path[i + 1].x - path[i].x, path[i + 1].y - path[i].y)));
            len += std::hypot(path[i + 1].x - path[i].x, path[i + 1].y - path[i].y);
        }
        const F64 tight = std::min(1.0, turn * BAND_CURVE_R_M);         // 0 straight .. 1 at a 60 m radius or less
        const F64 room = std::max(BAND_TIGHT_M, at < path.size() ? path[at].lane_w / 2 - sCarHalfW - EDGE_MARGIN : BAND_STRAIGHT_M);
        return std::max(BAND_TIGHT_M, std::min(room, BAND_STRAIGHT_M) * (1 - tight) + BAND_TIGHT_M * tight);
    }

    std::pair<S32, S32> decide(F64 x, F64 y, F64 yaw, F64 speed, F64 yaw_rate_rad, F64 cell_edge, F64 keep)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - decide");
        const F64 v = std::max(V_PLAN_MIN, speed);
        const std::vector<LanePoint>& path = sPath;
        if (path.size() < 2)
        {
            return { 0, 0 };
        }
        const S32 nf = S32(std::lround(HORIZON / DT)), sp = S32(std::lround(ROLL_DT / DT)), n = nf / sp;
        const F64 dist = yaw_rate_rad * 180 / PI - modelRate(sFrame - 1);
        const F64 tau = (sFrame - sBorderFrame) < KICK_WINDOW / DT ? TAU_KICK : TAU_NOISE;
        std::vector<F64> err(n), room_l(n), room_r(n), band(n);
        sView.rollout.clear();
        S32 f = sFrame;
        size_t seg = 0;
        for (S32 i = 0; i < n; ++i)
        {
            for (S32 k = 0; k < sp; ++k)
            {
                const F64 rate = modelRate(f) + dist * std::exp(-(f - sFrame) * DT / tau);
                yaw += rate * PI / 180 * DT;
                x += std::cos(yaw) * v * DT;
                y += std::sin(yaw) * v * DT;
                ++f;
            }
            sView.rollout.push_back({ x, y });
            while (seg + 2 < path.size())
            {
                const LanePoint& nx = path[seg + 1];
                if ((x - nx.x) * nx.ax + (y - nx.y) * nx.ay >= 0)
                {
                    ++seg;
                }
                else
                {
                    break;
                }
            }
            const LanePoint& p = path[seg];
            const LanePoint& q = path[seg + 1];
            F64 sx = q.x - p.x, sy = q.y - p.y;
            const F64 L = std::max(1e-6, std::hypot(sx, sy));
            sx /= L;
            sy /= L;
            const F64 e = sx * (y - p.y) - sy * (x - p.x);       // + left of the line
            err[i] = e;
            band[i] = laneBand(path, seg);
            const F64 mid = p.lanes == 2 ? keep : 0.0;           // the lane's middle, m right of the line
            const F64 across = -e - mid;
            const F64 half = std::max(0.0, p.lane_w / 2 - sCarHalfW - EDGE_MARGIN);   // never below none
            room_l[i] = half + across;
            room_r[i] = half - across;
        }
        const auto& prof = profiles();
        const F64 g = sGain;
        std::vector<F64> kick;
        const F64 edge_t = cell_edge / v;
        if (edge_t < HORIZON)
        {
            kick.assign(n, 0.0);
            const S32 k0 = S32(edge_t / DT);
            F64 hd = 0, lat = 0;
            for (S32 fr = 0; fr < nf; ++fr)
            {
                const S32 b = fr - k0;
                const F64 r = (b >= 0 && b < 50) ? KICK_SIZE * std::exp(-b * DT / 0.25) : 0.0;
                hd += r * PI / 180 * DT;
                lat += hd * DT;
                if ((fr + 1) % sp == 0)
                {
                    kick[(fr + 1) / sp - 1] = v * lat;
                }
            }
        }
        const S32 i0 = std::max(0, S32(std::ceil(DEAD_TIME / ROLL_DT - 1e-9)) - 1);
        const S32 k2 = S32(std::lround(SECOND_AT / ROLL_DT));
        std::vector<std::pair<S32, S32>> firsts{ { 0, 0 } }, seconds{ { 0, 0 } };
        for (S32 d : { 1, -1 })
        {
            for (S32 h : HOLDS)
            {
                firsts.push_back({ d, h });
            }
            for (S32 h : SECOND_HOLDS)
            {
                seconds.push_back({ d, h });
            }
        }
        std::pair<S32, S32> best{ 0, 0 };
        F64 best_cost = 1e18;
        for (const auto& [d1, h1] : firsts)
        {
            const std::vector<std::array<F64, 3>>* p1 = d1 ? &prof.at(h1) : nullptr;
            for (const auto& [d2, h2] : seconds)
            {
                const std::vector<std::array<F64, 3>>* p2 = d2 ? &prof.at(h2) : nullptr;
                F64 cost = 0.0;
                bool pruned = false;
                for (S32 i = i0; i < n; ++i)
                {
                    F64 de = 0;
                    if (p1)
                    {
                        de += d1 * g * v * (*p1)[i][0];
                    }
                    if (p2 && i >= k2)
                    {
                        de += d2 * g * v * (*p2)[i - k2][0];
                    }
                    const F64 e = err[i] + de;
                    // only what lies outside the band counts, and a slight pull to the middle so it does not
                    // ride the band's edge: on a straight it leaves the car be, in a bend it holds it close
                    const F64 out = std::max(0.0, std::fabs(e) - band[i]);
                    cost += out * out * BAND_W + BAND_PULL * e * e;
                    if (kick.empty())
                    {
                        const F64 over = std::max(0.0, de - room_l[i]) + std::max(0.0, -de - room_r[i]);
                        cost += EDGE_W * over * over;
                    }
                    else
                    {
                        for (F64 ks : { kick[i], -kick[i] })
                        {
                            const F64 over = std::max(0.0, de + ks - room_l[i]) + std::max(0.0, -(de + ks) - room_r[i]);
                            cost += 0.5 * EDGE_W * over * over;
                        }
                    }
                    if (cost >= best_cost)
                    {
                        pruned = true;
                        break;
                    }
                }
                if (!pruned && cost < best_cost)
                {
                    best_cost = cost;
                    best = { d1, h1 };
                }
            }
        }
        return best;
    }
}



namespace skroad::drive
{
    // Traffic: the other vehicles on the road (a scripted bus roaming the Linden roads, often phantom, which the
    // car drove straight through). A vehicle is a root object with an avatar sat on it or moving faster than
    // TRAFFIC_MOVING; each is placed against the lane line the car keeps: how far along it, how far across. One
    // ahead in the car's lane and no faster is followed at a time gap; on two lanes, one slower than the car's
    // cruise is passed in the left lane when that lane is clear and the road straight and seen far enough, and
    // the car comes back once it is a safe gap behind.
    constexpr F64 TRAFFIC_SCAN_M = 110.0;       // objects this far from the car are looked at
    constexpr F64 TRAFFIC_EVERY_S = 0.2;        // between scans of the object list
    constexpr F64 TRAFFIC_MOVING = 1.0;         // m/s: faster than this, an object is traffic
    constexpr F64 TRAFFIC_IN_LANE_M = 2.0;      // across from a lane's line: in that lane
    constexpr F64 TRAFFIC_ON_ROAD_M = 8.0;      // across from the line kept: still on the road
    constexpr F64 TRAFFIC_ON_ROAD_Z = 4.0;      // above or below the line: not on the road
    constexpr F64 LEADER_REACH_M = 50.0;        // a vehicle ahead in the lane further than this is not followed
    constexpr F64 BUMPERS_M = 5.0;              // centre to centre less this: the gap between the two
    constexpr F64 FOLLOW_GAP_S = 2.0;           // the time gap kept behind a slower vehicle
    constexpr F64 FOLLOW_MIN_M = 8.0;           // and never less than this
    constexpr F64 FOLLOW_BRAKE_SHARE = 0.5;     // under this share of the gap, and closing: S as well
    constexpr F64 OVERTAKE_SLOWER = 2.0;        // m/s: the leader this much slower than the car's cruise
    constexpr F64 OVERTAKE_AFTER_S = 3.0;       // followed this long before passing
    constexpr F64 OVERTAKE_MAX_TURN_DEG = 20.0; // the line turns no more than this over the pass
    constexpr F64 OVERTAKE_MARGIN_M = 10.0;     // ahead of the leader before coming back, at the least
    constexpr F64 OVERTAKE_MAX_S = 25.0;        // a pass taking longer is given up
    constexpr F64 LANE_MOVE_MPS = 1.5;          // how fast the line moves across between lanes
    constexpr F64 ONCOMING_CLEAR_S = 2.5;       // the left lane kept clear this long at the closing speed, mid-pass
    constexpr F64 ONCOMING_GUESS_MPS = 10.0;    // an oncoming car's speed where it is not seen moving yet

    struct Vehicle
    {
        LLUUID id;
        F64 s;          // along the line from the car (+ ahead), centre to centre
        F64 lat;        // across from the line the car keeps now (+ right)
        F64 v;          // speed along the line (+ the car's way, - oncoming)
        bool two_lanes; // the road has two lanes where it is
    };

    std::vector<std::pair<LLUUID, LLViewerObject*>> sTrafficObjs;   // the last scan's (looked up again by id)
    F64 sTrafficScanT = -100.0;
    S32 sOvertake = 0;          // 0 keeping right, 1 out and passing, 2 coming back
    LLUUID sOvertakeId;         // the vehicle being passed
    F64 sOvertakeSince = -1.0;
    F64 sFollowSince = -1.0;    // since when a leader slower than the cruise has been followed
    F64 sKeepExtra = 0.0;       // the lane line moved by this from the right lane's (m, + right): -2 laneShift in the left
    F64 sTrafficT = -1.0;       // the last frame's time, for moving the line
    std::string sTrafficState;  // the last state logged

    void resetTraffic()
    {
        sTrafficObjs.clear();
        sTrafficScanT = -100.0;
        sOvertake = 0;
        sOvertakeId.setNull();
        sOvertakeSince = sFollowSince = -1.0;
        sKeepExtra = 0.0;
        sTrafficT = -1.0;
        sTrafficState.clear();
    }

    // The nearest stretch of sPath to a point: its distance along the line, across it (+ right), its segment;
    // returns the squared distance.
    F64 placeOnPath(F64 x, F64 y, F64& s, F64& lat, size_t& seg)
    {
        F64 best = 1e18, run = 0;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            const LanePoint& a = sPath[i];
            const LanePoint& b = sPath[i + 1];
            const F64 sx = b.x - a.x, sy = b.y - a.y, L = std::max(1e-6, std::hypot(sx, sy));
            const F64 f = std::clamp(((x - a.x) * sx + (y - a.y) * sy) / (L * L), 0.0, 1.0);
            const F64 dx = x - (a.x + f * sx), dy = y - (a.y + f * sy);
            const F64 d2 = dx * dx + dy * dy;
            if (d2 < best)
            {
                best = d2;
                s = run + f * L;
                lat = (sy * dx - sx * dy) / L;
                seg = i;
            }
            run += L;
        }
        return best;
    }

    // Each vehicle about the car on the road, placed along and across sPath, the line the car keeps.
    std::vector<Vehicle> seeTraffic(const LLViewerObject* car, const LLVector3d& pos, F64 now)
    {
        if (now - sTrafficScanT >= TRAFFIC_EVERY_S)
        {
            sTrafficScanT = now;
            sTrafficObjs.clear();
            const S32 n = gObjectList.getNumObjects();
            for (S32 i = 0; i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                // roots only: a part, an attachment (parented to its avatar) and avatars themselves are not
                if (!o || o->isDead() || o == car || o->getParent() || o->isAvatar() || o->isAttachment())
                {
                    continue;
                }
                const LLVector3d op = o->getPositionGlobal();
                if (std::hypot(op.mdV[VX] - pos.mdV[VX], op.mdV[VY] - pos.mdV[VY]) > TRAFFIC_SCAN_M)
                {
                    continue;
                }
                bool vehicle = o->getVelocity().magVec() > TRAFFIC_MOVING;
                for (const LLViewerObject* c : o->getChildren())
                {
                    vehicle = vehicle || (c && c->isAvatar());
                }
                if (vehicle)
                {
                    sTrafficObjs.emplace_back(o->getID(), o);
                }
            }
        }
        std::vector<Vehicle> out;
        if (sPath.size() < 2)
        {
            return out;
        }
        F64 s_car = 0, lat_car = 0;
        size_t seg_car = 0;
        placeOnPath(pos.mdV[VX], pos.mdV[VY], s_car, lat_car, seg_car);
        for (const auto& [id, o0] : sTrafficObjs)
        {
            LLViewerObject* o = gObjectList.findObject(id);
            if (!o || o != o0 || o->isDead())
            {
                continue;
            }
            const LLVector3d op = o->getPositionGlobal();
            F64 s = 0, lat = 0;
            size_t seg = 0;
            const F64 d2 = placeOnPath(op.mdV[VX], op.mdV[VY], s, lat, seg);
            const LanePoint& q = sPath[seg];
            if (d2 > TRAFFIC_ON_ROAD_M * TRAFFIC_ON_ROAD_M || std::fabs(op.mdV[VZ] - q.z) > TRAFFIC_ON_ROAD_Z)
            {
                continue;   // off the road, or past the line's ends
            }
            const LLVector3& v = o->getVelocity();
            out.push_back({ id, s - s_car, lat, v.mV[VX] * q.ax + v.mV[VY] * q.ay, q.lanes == 2 });
        }
        return out;
    }

    // Over the next `len` metres of the line from the car: the most it turns from its way here, whether it keeps
    // two lanes, and how far it is seen (less than `len` where it ends).
    void lineAhead(F64 len, F64& turn_deg, bool& two_lanes, F64& seen)
    {
        turn_deg = 0;
        two_lanes = sPath.size() >= 2;
        seen = 0;
        if (!two_lanes)
        {
            return;
        }
        const F64 a0 = std::atan2(sPath[0].ay, sPath[0].ax);
        for (size_t i = 0; i + 1 < sPath.size() && seen < len; ++i)
        {
            seen += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
            turn_deg = std::max(turn_deg, std::fabs(wrap(std::atan2(sPath[i].ay, sPath[i].ax) - a0)) * 180 / PI);
            two_lanes = two_lanes && sPath[i].lanes == 2;
        }
    }

    // Each frame on the road: follows a vehicle ahead in the lane (W let go, S when too close and not turning),
    // passes a slow one where it may, and moves the lane line across for it (sKeepExtra, which the next walk of the
    // path adds to `keep`).
    void traffic(const LLViewerObject* car, const LLVector3d& pos, F64 speed, F64 cruise, F64 keep, F64 now, U32& keys,
                 std::string& note)
    {
        auto tag = [&](const std::string& t) { note += note.empty() ? t : ", " + t; };
        const F64 dt = sTrafficT < 0 ? 0.0 : std::clamp(now - sTrafficT, 0.0, 0.2);
        sTrafficT = now;
        const std::vector<Vehicle> seen_v = seeTraffic(car, pos, now);
        const F64 lane_w = 2 * laneShift(keep);         // the right lane's line to the left lane's
        const F64 lat_left = -sKeepExtra - lane_w;      // the left lane's line, across from the line kept now
        const bool two = !sPath.empty() && sPath[0].lanes == 2;
        char buf[160];

        // the leader: the nearest ahead in the lane the car is in now, not coming toward it; and the one being passed
        const Vehicle* lead = nullptr;
        const Vehicle* passed = nullptr;
        for (const Vehicle& v : seen_v)
        {
            if (v.id == sOvertakeId)
            {
                passed = &v;
            }
            if (v.s > 0 && v.s < LEADER_REACH_M && std::fabs(v.lat) < TRAFFIC_IN_LANE_M && v.v > -TRAFFIC_MOVING
                && (!lead || v.s < lead->s))
            {
                lead = &v;
            }
        }
        // anything in the left lane from beside the car to `reach` ahead, plus how far it comes toward the car in `time`
        auto leftBlocked = [&](F64 reach, F64 time) {
            for (const Vehicle& v : seen_v)
            {
                if (v.id != sOvertakeId && v.two_lanes && std::fabs(v.lat - lat_left) < TRAFFIC_IN_LANE_M
                    && v.s > -BUMPERS_M && v.s < reach + std::max(0.0, -v.v) * time)
                {
                    return true;
                }
            }
            return false;
        };

        // a pass under way: back once the passed vehicle is a safe gap behind, or at once when the left lane closes
        if (sOvertake == 1)
        {
            const F64 behind = passed ? -passed->s : 0.0;     // + the car ahead of it
            const F64 back_gap = std::max(OVERTAKE_MARGIN_M, speed * 1.0) + BUMPERS_M;
            const F64 reach = (speed + ONCOMING_GUESS_MPS) * ONCOMING_CLEAR_S + BUMPERS_M;
            const char* why = nullptr;
            if (!passed)
            {
                why = "return (passed vehicle lost)";
            }
            else if (behind > back_gap)
            {
                why = "return";
            }
            else if (now - sOvertakeSince > OVERTAKE_MAX_S)
            {
                why = "return (pass too long)";
            }
            else if (!two)
            {
                why = "return (one lane)";
            }
            else if (leftBlocked(reach, ONCOMING_CLEAR_S))
            {
                why = "abort overtake: oncoming";
                say("oncoming traffic: back into the lane");
            }
            if (why)
            {
                sOvertake = 2;
                tag(why);
            }
            else
            {
                snprintf(buf, sizeof(buf), "overtaking (%.0f m to pass)", back_gap - behind);
                tag(buf);
            }
        }
        if (sOvertake == 2 && std::fabs(sKeepExtra) < 0.3)
        {
            sOvertake = 0;
            sOvertakeId.setNull();
        }

        // following: W let go inside the gap; S as well well inside it, closing and not turning
        bool follow = false;
        if (lead && sOvertake != 1)
        {
            const F64 gap = lead->s - BUMPERS_M;
            const F64 want = std::max(FOLLOW_MIN_M, FOLLOW_GAP_S * speed);
            const F64 closing = speed - lead->v;
            if (gap < want && closing > -0.5)
            {
                follow = true;
                keys &= ~AGENT_CONTROL_AT_POS;
                const bool turning = (keys & (AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG)) != 0;
                const bool brake = (gap < want * FOLLOW_BRAKE_SHARE || gap < 4.0) && closing > 0.0 && speed > 1.0;
                if (brake && !turning)
                {
                    keys |= AGENT_CONTROL_AT_NEG;
                }
                snprintf(buf, sizeof(buf), "follow%s (gap %.0f m, leader %.1f m/s)", brake ? ": braking" : "", gap, lead->v);
                tag(buf);
            }
        }
        sFollowSince = follow && lead->v < cruise - OVERTAKE_SLOWER ? (sFollowSince < 0 ? now : sFollowSince) : -1.0;

        // a pass begins: followed long enough, two lanes, the left lane clear for the whole pass, the road straight and
        // seen that far
        if (sOvertake == 0 && follow && two && sFollowSince >= 0 && now - sFollowSince >= OVERTAKE_AFTER_S)
        {
            const F64 v_pass = std::max(cruise, speed);
            const F64 dv = std::max(3.0, v_pass - lead->v);
            const F64 gain = lead->s + BUMPERS_M + OVERTAKE_MARGIN_M;     // to be the margin ahead of it
            const F64 t = gain / dv + 2 * lane_w / LANE_MOVE_MPS;
            const F64 need = v_pass * t;
            F64 turn = 0, sight = 0;
            bool two_all = false;
            lineAhead(need, turn, two_all, sight);
            if (sight < need || !two_all || turn > OVERTAKE_MAX_TURN_DEG)
            {
                snprintf(buf, sizeof(buf), "no overtake: %s (need %.0f m)", sight < need ? "sight" : !two_all ? "one lane" : "bend", need);
                tag(buf);
            }
            else if (leftBlocked(need, t))
            {
                tag("no overtake: oncoming");
            }
            else
            {
                sOvertake = 1;
                sOvertakeId = lead->id;
                sOvertakeSince = now;
                sFollowSince = -1.0;
                snprintf(buf, sizeof(buf), "overtake (leader %.1f m/s, %.0f m ahead, pass ~%.0f m)", lead->v, lead->s, need);
                tag(buf);
                say("overtaking a slower vehicle");
            }
        }

        // the lane line moves across toward its lane at LANE_MOVE_MPS
        const F64 target = sOvertake == 1 ? -lane_w : 0.0;
        sKeepExtra += std::clamp(target - sKeepExtra, -LANE_MOVE_MPS * dt, LANE_MOVE_MPS * dt);
        const std::string state = sOvertake == 1 ? "overtake" : sOvertake == 2 ? "return" : follow ? "follow" : "";
        if (state != sTrafficState)
        {
            LL_INFOS("SKRoadDriver") << "traffic: " << (state.empty() ? "clear" : state) << LL_ENDL;
            sTrafficState = state;
        }
    }
}

void sk_road_driver::onSettingChanged(const LLSD& enabled)
{
    if (enabled.asBoolean() && !sOn)
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            say("sit in a vehicle first");
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
            return;
        }
        LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent());
        const LLViewerObject* car = seat ? seat->getRootEdit() : nullptr;
        sVehicle = car ? car->getID() : LLUUID::null;
        sCarHalfW = car ? carHalfWidth(car) : 1.47;
        sCarRear = car ? carRear(car) : 2.4;
        sOn = true;
        skroute::follow::reset();   // a route goal is planned afresh from where the car is
        sPreviewing = false;
        sCourse.set = false;        // a test course is laid out from where the car is now
        sClock.reset();
        sFrame = 0;
        sTaps.clear();
        sKey = 0;
        sLastRelease = -99;
        sNextDecide = 0;
        sPwmCredit = 0.0;
        sPwmPrevT = -1.0;
        sPwmWant = sPwmGot = 0.0;
        sPwmFired.clear();
        sThrottleLog.clear();
        sPwmPull = 0.0;
        sPwmK = 1.0;
        sRecoverPhase = 0;
        sLadderRung = -1;
        sFrozenTried = false;
        sTurnPhase = 0;
        sDeadEndSince = -1.0;
        // the trail is kept across a short stop (a takeover, a stuck), forgotten after a long one; the way
        // travelled starts again from the nose: the driver turned on is pointing the way meant
        if (sTrailOffAt >= 0 && LLTimer::getTotalSeconds().value() - sTrailOffAt > TRAIL_OFF_FORGET_S)
        {
            trailClear("off for long");
        }
        sTravelSet = false;
        sStuckSince = -1.0;
        sRecoverTimes.clear();
        sPoseChangedAt = -1.0;
        sFrozenSaid = false;
        sLiftUntil = sLiftNext = -100.0;
        sGearMax = 99;
        sJump = JumpRun();
        sGearBefore = 0;
        sShiftDir = 0;
        sBenchStep = -1;
        sBenchGear = 0;
        sArenaSet = false;      // the arena: the parcel the car is on as this run begins
        sBorderFrame = -999;
        // times on the run clock, which starts again at 0: left from the last run they held off every steer
        // (the probe's quiet) until the clock passed them (three runs that never steered)
        sLastSteer = -100.0;
        sProbeLast = -100.0;
        sProbeQuietUntil = -100.0;
        sProbeStep = 0;
        sLearnFrame = -1;
        sCell = 0;
        sPath.clear();
        sPathFrame = -999;
        sDriven.clear();
        sHaveLastLine = false;
        sLastSeenRoad = 0.0;
        sPrevYawT = -1.0;
        sYawRate = 0.0;
        sMeas.clear();
        std::fill(std::begin(sSxy), std::end(sSxy), 0.0);
        std::fill(std::begin(sSxx), std::end(sSxx), 0.0);
        // the gain and delay learnt are kept for the same vehicle; a new one starts from the fit
        static LLUUID learnt_for;
        if (learnt_for != sVehicle)
        {
            sGain = 1.0;
            sShift = 0;
            learnt_for = sVehicle;
        }
        sShapesRead = false;
        sGatherDirty = true;
        sDriveTex.clear();
        driveTexChanged();
        sGear = 0;
        sLastShift = -100.0;
        sShiftEnd = -1.0;
        sAccelT = sAccelV = sAccel = 0.0;
        sLastCrossingT = -100.0;
        resetCrossing();
        resetTraffic();
        SKWorldCensus::instance().addDemand(DEMAND, shapeReach() + 2.f);
        if (SKAirCells::instanceExists())
        {
            SKAirCells::instance().addDemand(AIR_DEMAND);
        }
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-autopilot.csv");
        sLog.open(path.c_str(), std::ios::out | std::ios::app);
        sLog.precision(8);
        sLog << "# run (predictive); t,speed,yaw_deg,yaw_rate_deg,dist_deg,err_m,path_n,road_w,lanes,boundary_m,"
                "target,keys,gain,shift,gear,note\n";
        sLog << "# vehicle " << sVehicle << "\n";
        LL_INFOS("SKRoadDriver") << "vehicle half width " << sCarHalfW << " m" << LL_ENDL;
        say("on. Any movement key takes over.");
    }
    else if (!enabled.asBoolean() && sOn)
    {
        stop("off");
    }
}

void sk_road_driver::noteChat(const LLUUID& from, const std::string& text)
{
    if (!sOn || from.isNull() || from != sVehicle)
    {
        return;
    }
    const VehicleProfile& car = vehicleProfile();
    if (!car.chat_gears)
    {
        return;
    }
    if (!car.reverse_word.empty() && text.find(car.reverse_word) != std::string::npos)
    {
        sGear = -1;
    }
    else if (const size_t at = car.gear_word.empty() ? std::string::npos : text.find(car.gear_word); at != std::string::npos)
    {
        const S32 gear = std::atoi(text.c_str() + at + car.gear_word.size());
        if (gear > 0)
        {
            sGear = gear;
        }
    }
    LL_INFOS("SKRoadDriver") << "vehicle says '" << text << "': gear " << sGear << LL_ENDL;
}

namespace skroad::drive
{
    // SKRoadAutopilotGoal "route": the committed route to SKRoadAutopilotRouteTo ("x,y", global metres) over the
    // road dataset (SKRoadAutopilotRouteDataset), by SKRoadAutopilotRouteCost; chooseBranch asks it at junctions.
    void tickRoute(const LLVector3d& pos, F64 now)
    {
        static const bool picker_set = (skroad::setBranchPicker(skroute::follow::pickBranch), true);
        (void)picker_set;
        static LLCachedControl<std::string> goal(gSavedSettings, "SKRoadAutopilotGoal", "explore");
        if (std::string(goal) != "route")
        {
            return;
        }
        static LLCachedControl<std::string> to(gSavedSettings, "SKRoadAutopilotRouteTo", "");
        static LLCachedControl<std::string> dataset(gSavedSettings, "SKRoadAutopilotRouteDataset", "");
        static LLCachedControl<std::string> cost(gSavedSettings, "SKRoadAutopilotRouteCost", "shortest");
        skroute::follow::Config cfg;
        cfg.dataset = dataset;
        cfg.cost = cost;
        cfg.has_target = sscanf(std::string(to).c_str(), "%lf,%lf", &cfg.x, &cfg.y) == 2;
        skroute::follow::tick(pos.mdV[VX], pos.mdV[VY], now, cfg);
    }
}

void sk_road_driver::beforeAgentUpdate()
{
    static const bool course_set = (skroad::host::setTestCourse(&skroad::drive::testCourse), true);   // the model's test courses
    (void)course_set;

    // Preview: with the overlay on and seated in a vehicle, the road the autopilot would follow is worked
    // out and drawn while it is stopped too (for looking and screenshots); it sends no keys and learns nothing.
    static LLCachedControl<bool> overlay(gSavedSettings, "SKRoadAutopilotOverlay", true);
    const bool preview = !sOn && overlay && isAgentAvatarValid() && gAgentAvatarp->isSitting();
    if (!sOn && !preview)
    {
        if (sPreviewing)
        {
            sPreviewing = false;
            SKWorldCensus::instance().removeDemand(DEMAND);
        if (SKAirCells::instanceExists())
        {
            SKAirCells::instance().removeDemand(AIR_DEMAND);
        }
        sAirTiles.clear();
            sView.valid = false;
            sPath.clear();
            sDriveTex.clear();
            driveTexChanged();
        }
        return;
    }
    if (preview && !sPreviewing)
    {
        sPreviewing = true;
        SKWorldCensus::instance().addDemand(DEMAND, shapeReach() + 2.f);
        if (SKAirCells::instanceExists())
        {
            SKAirCells::instance().addDemand(AIR_DEMAND);
        }
        sClock.reset();
        sPath.clear();
        sDriveTex.clear();
        driveTexChanged();
    }
    LL_PROFILE_ZONE_SCOPED;
    constexpr U32 USER_MOVE = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS
                              | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG
                              | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG
                              | AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG;
    if (sOn && gAgent.getAutoPilot())
    {
        // the viewer's own autopilot (turning to face a selection, a click to walk): seated, its turns are
        // steering keys, and they are not the driver taking over
        gAgent.stopAutoPilot();
        gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG | AGENT_CONTROL_AT_POS
                                 | AGENT_CONTROL_AT_NEG);
    }
    if (!preview && (gAgent.getControlFlags() & USER_MOVE))
    {
        stop("you took over");
        return;
    }
    if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
    {
        // a region crossing loses the sit for a moment while the car is handed over: keys off, and stopped only
        // once it has stayed lost for UNSEAT_GRACE_S (a crossing ended a 1.6 km run here)
        const F64 t = LLTimer::getTotalSeconds().value();
        if (sUnseatedSince < 0)
        {
            sUnseatedSince = t;
        }
        gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG | AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG);
        if (t - sUnseatedSince > UNSEAT_GRACE_S)
        {
            sUnseatedSince = -1.0;
            stop("no longer seated");
        }
        return;
    }
    sUnseatedSince = -1.0;
    LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent());
    LLViewerObject* car = seat ? seat->getRootEdit() : nullptr;
    if (!car)
    {
        stop("no vehicle");
        return;
    }
    static LLCachedControl<F32> cruise(gSavedSettings, "SKRoadAutopilotSpeed");
    static LLCachedControl<F32> keep_c(gSavedSettings, "SKRoadAutopilotLaneOffset");
    static LLCachedControl<bool> learn_on(gSavedSettings, "SKRoadAutopilotLearn", true);
    static LLCachedControl<bool> use_gears(gSavedSettings, "SKRoadAutopilotUseGears", true);
    const F64 keep = F32(keep_c);

    const F64 now = sClock.getElapsedTimeF64();
    sFrame = S32(std::lround(now / DT));
    static LLCachedControl<std::string> steer_mode_g(gSavedSettings, "SKRoadAutopilotSteer", "pwm");
    sLaneGuard = false;     // set again this frame by the steering, if it still holds
    const LLVector3d pos = car->getPositionGlobal();
    const LLVector3 fwd3 = LLVector3::x_axis * car->getRotationRegion();
    const F64 yaw = std::atan2(fwd3.mV[VY], fwd3.mV[VX]);
    const LLVector3& vel = car->getVelocity();
    const F64 speed = std::hypot(vel.mV[VX], vel.mV[VY]);
    if (sPrevYawT < 0)
    {
        sPrevYaw = yaw;
        sPrevYawT = now;
    }
    else if (now - sPrevYawT >= 0.04)
    {
        sYawRate = wrap(yaw - sPrevYaw) / (now - sPrevYawT);   // signed, rad/s
        sPrevYaw = yaw;
        sPrevYawT = now;
    }
    {
        // the simulator's own angular velocity where it sends one: the difference of the interpolated rotation
        // over 0.04 s is mostly noise (median 6-7 deg/s, p90 17-19 against the model), which the planner tapped
        // against as a disturbance (77 % of taps reversing the last)
        const F32 wz = car->getAngularVelocity().mV[VZ];
        if (wz != 0.f || std::fabs(sYawRate) < 0.05)
        {
            sYawRate = wz;
        }
    }
    const F64 hx = std::cos(yaw), hy = std::sin(yaw);

    // region borders: the frame of the last one, and how far along the heading to the next
    const U64 cell = (U64(U32(S32(std::floor(pos.mdV[VX] / 256)))) << 32) | U32(S32(std::floor(pos.mdV[VY] / 256)));
    if (sCell && cell != sCell)
    {
        sBorderFrame = sFrame;
        sLastCrossingT = now;
    }
    sCell = cell;
    auto toEdge = [](F64 p, F64 h) { return h > 1e-3 ? (256 - std::fmod(p, 256.0)) / h : h < -1e-3 ? std::fmod(p, 256.0) / -h : 1e9; };
    const F64 boundary = std::min(toEdge(pos.mdV[VX], hx), toEdge(pos.mdV[VY], hy));

    // an unsafe crossing ahead (sGuard): braked hard to a stop short of the edge, then the run ends
    if (!preview)
    {
        sGuardZ.emplace_back(now, pos.mdV[VZ]);
        sGuardSpeed = speed;
        while (!sGuardZ.empty() && now - sGuardZ.front().first > 3.0)
        {
            sGuardZ.pop_front();
        }
        if (sGuard.braking)
        {
            // short of the edge by the margin, along the line it was planned on; nothing but S, and that in pulses
            const F64 ahead_m = (sGuard.edge.mdV[VX] - pos.mdV[VX]) * sGuard.dx + (sGuard.edge.mdV[VY] - pos.mdV[VY]) * sGuard.dy;
            const bool press = guardBrakePress(speed, 0.0, ahead_m - CROSS_GUARD_MARGIN_M, now);
            std::string brake_note = press ? "unsafe crossing ahead: braking" : "unsafe crossing ahead: coasting";
            sendKeys(press ? AGENT_CONTROL_AT_NEG : 0, brake_note);
            if (speed < 0.3 || now - sGuard.since > CROSS_GUARD_BRAKE_S)
            {
                stop(sGuard.why);
            }
            return;
        }
        LLViewerRegion* car_region = car->getRegion();
        if (car_region)
        {
            edgeMap(car_region, now);       // kept made all run, so the edge parcels' owners are asked early
        }
        // held short of an edge not known yet: still judged along the heading, the car may be (nearly) stopped
        const bool waiting = sEdgeWait.on;
        sEdgeWait.on = false;
        if (!sCross.on && car_region && (speed > 0.5 || waiting))
        {
            const F64 vx = speed > 0.5 ? vel.mV[VX] / speed : hx, vy = speed > 0.5 ? vel.mV[VY] / speed : hy;
            const F64 to_edge = std::min(toEdge(pos.mdV[VX], vx), toEdge(pos.mdV[VY], vy));
            // decided while there is still room to stop CROSS_GUARD_MARGIN_M short of it
            const F64 brake_m = speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S;
            LLVector3d land;
            std::string why;
            LLSD hazard;
            const F64 reach = CROSS_GUARD_MARGIN_M + brake_m + CROSS_GUARD_LOOK_M;
            S32 verdict = to_edge < reach ? unsafeCrossingAhead(car_region, now, pos, vx, vy, to_edge, land, why, hazard)
                                          : CROSS_SAFE;
            F64 stop_edge = to_edge;
            // near a corner (Karpov 251,2 and Bonzo 254,252, seen 5-6 m out: the crossing into the first region held
            // the guard off until the car was all but over the second edge): the second edge the car reaches within
            // its stopping distance, judged now from the region across the first, the worse verdict kept
            const F64 to_second = std::max(toEdge(pos.mdV[VX], vx), toEdge(pos.mdV[VY], vy));
            if (verdict == CROSS_SAFE && to_second < reach && to_second - to_edge > 2.5)
            {
                const F64 step = to_edge + 1.5;     // past unsafeCrossingAhead's "within 1 m: crossing already"
                const LLVector3d past(pos.mdV[VX] + vx * step, pos.mdV[VY] + vy * step, pos.mdV[VZ]);
                LLViewerRegion* next_region = LLWorld::getInstance()->getRegionFromPosGlobal(past);
                if (next_region && next_region != car_region)
                {
                    LLVector3d land2;
                    std::string why2;
                    LLSD hazard2;
                    const S32 v2 = unsafeCrossingAhead(next_region, now, past, vx, vy, to_second - step, land2, why2, hazard2);
                    if (v2 != CROSS_SAFE)
                    {
                        LL_INFOS("SKRoadDriver") << "edge guard: corner ahead, the second edge (" << next_region->getName()
                                                 << llformat(", %.1f m on) ", to_second)
                                                 << (v2 == CROSS_WAIT ? "not known yet" : "unsafe") << LL_ENDL;
                        verdict = v2;
                        land = land2;
                        why = why2;
                        hazard = hazard2;
                        stop_edge = to_second;
                    }
                }
            }
            if (verdict == CROSS_WAIT)
            {
                sEdgeWait.on = true;
                sEdgeWait.to_edge = stop_edge;
                if (!waiting || why != sEdgeWait.why)
                {
                    sEdgeWait.since = waiting ? sEdgeWait.since : now;
                    LL_INFOS("SKRoadDriver") << "edge ahead not known yet, crawling up to hold short: " << why << " at "
                                             << placeText(pos) << ", " << to_edge << " m to the edge at " << speed
                                             << " m/s" << LL_ENDL;
                }
                sEdgeWait.why = why;
            }
            else if (waiting)
            {
                LL_INFOS("SKRoadDriver") << "edge ahead resolved after " << (now - sEdgeWait.since) << " s: "
                                         << (verdict == CROSS_SAFE ? "safe, on" : "unsafe") << LL_ENDL;
            }
            if (verdict == CROSS_UNSAFE)
            {
                sGuard.braking = true;
                sGuard.since = now;
                sGuard.why = why;
                sGuard.hazard = hazard;
                sGuard.edge = LLVector3d(pos.mdV[VX] + vx * stop_edge, pos.mdV[VY] + vy * stop_edge, pos.mdV[VZ]);
                sGuard.dx = vx;
                sGuard.dy = vy;
                LL_WARNS("SKRoadDriver") << why << " at " << placeText(pos) << ", " << stop_edge << " m to the edge at "
                                         << speed << " m/s" << LL_ENDL;
                say(why);
                const bool press = guardBrakePress(speed, 0.0, stop_edge - CROSS_GUARD_MARGIN_M, now);
                std::string brake_note = press ? "unsafe crossing ahead: braking" : "unsafe crossing ahead: coasting";
                sendKeys(press ? AGENT_CONTROL_AT_NEG : 0, brake_note);
                return;
            }
        }
    }
    // a region crossing (sCross): held through, nothing decided, until the new region sends the car again
    if (!preview)
    {
        LLViewerRegion* car_region = car->getRegion();
        LLViewerRegion* agent_region = gAgent.getRegion();
        const bool seen_before = sCrossCar != nullptr;
        const bool region_moved = seen_before && (car_region != sCrossCarRegion || agent_region != sCrossAgentRegion
                                                  || car != sCrossCar || sBorderFrame == sFrame);
        LLViewerRegion* from_region = sCrossCarRegion;
        sCrossCarRegion = car_region;
        sCrossAgentRegion = agent_region;
        sCrossCar = car;
        if (region_moved && car_region != sCrossStalledIn)
        {
            sCrossStalledIn = nullptr;
        }
        const LLVector3 omega = car->getAngularVelocity();
        const bool fresh_update = vel != sCrossVel || omega != sCrossOmega;
        sCrossVel = vel;
        sCrossOmega = omega;
        // short of the edge on the car's velocity: within CROSS_AHEAD_M, or CROSS_AHEAD_S at its speed
        const F64 vx = speed > 1e-3 ? vel.mV[VX] / speed : 0.0, vy = speed > 1e-3 ? vel.mV[VY] / speed : 0.0;
        const F64 to_edge = std::min(toEdge(pos.mdV[VX], vx), toEdge(pos.mdV[VY], vy));
        const bool ahead = speed > 1.0 && to_edge < std::max(CROSS_AHEAD_M, speed * CROSS_AHEAD_S);
        if (!sCross.on && (region_moved || (ahead && !sEdgeWait.on)) && !sCrossStalledIn)
        {
            sCross = Crossing();
            sCross.on = true;
            sCross.began = now;
            sCross.keys = sLastSentKeys & AGENT_CONTROL_AT_POS;
            sCross.from = regionName(region_moved ? from_region : car_region);
            sCross.to = region_moved ? regionName(car_region) : "?";
            if (region_moved)
            {
                sCross.changed = true;
                sCross.changed_at = now;
            }
            LL_INFOS("SKRoadDriver") << "crossing from " << sCross.from << (region_moved ? " to " + sCross.to : " (ahead)")
                                     << " at " << placeText(pos) << ", " << to_edge << " m to the edge" << LL_ENDL;
        }
        else if (sCross.on && region_moved)
        {
            // the region changed (or changed again, at a corner): updates counted afresh from here
            sCross.changed = true;
            sCross.changed_at = now;
            sCross.fresh = 0;
            sCross.to = regionName(car_region);
            LL_INFOS("SKRoadDriver") << "crossing: into " << sCross.to << " (agent in " << regionName(agent_region)
                                     << ")" << LL_ENDL;
        }
        if (sCross.on)
        {
            bool ended = false;
            if (!sCross.changed)
            {
                if (now - sCross.began > CROSS_AHEAD_GIVE_UP_S && !ahead)
                {
                    LL_INFOS("SKRoadDriver") << "crossing called off: no region change in "
                                             << (now - sCross.began) << " s" << LL_ENDL;
                    ended = true;
                }
            }
            else
            {
                if (fresh_update && car_region && car_region == agent_region && now > sCross.changed_at)
                {
                    ++sCross.fresh;
                }
                if (sCross.fresh >= CROSS_FRESH_N && now - sCross.changed_at >= CROSS_MIN_S)
                {
                    LL_INFOS("SKRoadDriver") << "crossing from " << sCross.from << " to " << sCross.to << " done in "
                                             << (now - sCross.began) << " s (" << (now - sCross.changed_at)
                                             << " s from the change)" << LL_ENDL;
                    ended = true;
                }
                else if (now - sCross.changed_at > CROSS_STALL_S)
                {
                    // no updates: the frozen check takes it from here
                    LL_WARNS("SKRoadDriver") << "crossing stalled: from " << sCross.from << " to " << sCross.to
                                             << ", no physics updates in " << (now - sCross.changed_at) << " s"
                                             << LL_ENDL;
                    say("crossing stalled: no updates from " + sCross.to);
                    sCrossStalledIn = car_region;
                    ended = true;
                }
            }
            if (ended)
            {
                sCross.on = false;
                // what counted time through the handover starts again
                sLastSeenRoad = std::max(sLastSeenRoad, now);
                sStuckSince = -1.0;
                sStuckTrail.clear();
                if (!sCrossStalledIn)
                {
                    sPoseChangedAt = -1.0;
                    sFrozenSaid = false;
                }
            }
            else
            {
                // held as it went in: W or nothing, no wheel, no brake
                sKey = 0;
                std::string note = "crossing " + sCross.from + ">" + sCross.to;
                sendKeys(sCross.keys, note);
                sThrottle = (sLastSentKeys & AGENT_CONTROL_AT_POS) != 0;
                sView.valid = true;
                sView.car = pos;
                sView.yaw = yaw;
                sView.tapping = 0;
                sView.waiting = false;
                sLog << now << ',' << speed << ',' << yaw * 180 / PI << ',' << sYawRate * 180 / PI << ",0,"
                     << sLastErr << ',' << sPath.size() << ',' << (sPath.empty() ? 0.0 : sPath[0].road_w) << ','
                     << (sPath.empty() ? 0 : sPath[0].lanes) << ',' << boundary << ",0," << std::hex << sCross.keys
                     << std::dec << ',' << sGain << ',' << sShift << ',' << sGear << ',' << note << '\n';
                return;
            }
        }
    }

    // the taps: a held key's tap ends; old ones are forgotten
    // a long hold (rhythm steering) lets go as soon as the turn it has begun is enough: the line, aimed from the
    // heading the car will have if it is let go now, wants less than half the rhythm's top rate the same way.
    // Held its full 0.76 s, each hold at 21 m/s swung the car past the line and the next went back: a 15 m weave.
    if (sKey && sFrame < sKeyUntil && sFrame - sKeySince >= 4 && sKeyUntil - sKeySince >= 25 && sPath.size() >= 2
        && std::string(steer_mode_g) != "plan")
    {
        // let go now, the car turns on at about its present rate for CARRY_S (the sweep's half-second carry-on);
        // the single-tap kernels, the longest 0.35 s, put far too little after a long hold
        const S32 dir = sKey;
        sKey = 0;
        // with the pull for a standing offset, as the steering adds it: without it a hold let go a car the pull
        // was bringing in from 0.85 m outside a 15 m circle
        const F64 want = pwmWant(car->getPositionGlobal(), yaw + sYawRate * CARRY_S, speed) + sPwmPull;
        sKey = dir;
        // let go once the line wants no more than the fastest rhythm holds: at half of it the car fell back to the
        // rhythms in a 15 m circle (26 deg/s wanted, 24 the fastest rhythm) and ran a metre wide
        // and in a bend that needs more than any rhythm holds (speed times the line's curvature just ahead), not
        // before the car turns faster than it needs: let go on the turn still to come alone, a 15 m circle (26
        // deg/s) was held a moment, fell back to the rhythms' 24 and ran 2 m wide
        const F64 need = speed * curvatureNear(speed * 1.0) * 180 / PI * dir;
        const F64 need_all = need + sPwmPull * dir;
        const bool bend_held = need_all > PWM_RHYTHM_MAX_DEG && sYawRate * 180 / PI * dir < need_all + PWM_HOLD_OVER_DEG;
        if (want * dir < PWM_RHYTHM_MAX_DEG * PWM_RELEASE_SHARE && !bend_held)
        {
            sKeyUntil = sFrame;
        }
    }
    if (sKey && sFrame >= sKeyUntil)
    {
        sTaps.push_back({ sKeySince, sKey, sFrame - sKeySince });     // as held (a dropped frame holds it longer)
        sKey = 0;
        sLastRelease = sFrame;
    }
    sTaps.erase(std::remove_if(sTaps.begin(), sTaps.end(), [](const Tap& t) { return sFrame - t.start >= KERNEL_N + 30; }),
                sTaps.end());
    if (learn_on && sFrame > 0 && !preview && sFrame != sLearnFrame)
    {
        sLearnFrame = sFrame;       // once a controller frame, not every render frame (2-3 times over)
        const bool clean = (sFrame - sBorderFrame) > BORDER_SKIP / DT && boundary > EDGE_SKIP;
        learn(sYawRate * 180 / PI, clean);
    }

    // the way travelled (while driving: a preview walks from the nose)
    if (!preview)
    {
        noteTrail(pos, yaw, speed, vel, sRecoverPhase != 0 || sTurnPhase != 0);
    }
    const F64 wx = !preview && sTravelSet ? sTravelX : hx, wy = !preview && sTravelSet ? sTravelY : hy;

    // the road
    refreshShapes(pos);
    // the air along the path as far as it is laid out
    refreshAir(pos, lookAhead(std::max(V_PLAN_MIN, speed)) + 10.0);
    tickLdpw();
    askProperties();
    noteDriveSurface(pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], now < 1.0);
    std::string note;
    if (sPath.empty() || sFrame - sPathFrame >= PATH_EVERY / DT)
    {
        // the road: its Linden pieces joined top edge to top edge, the line through the joins
        static LLCachedControl<std::string> course(gSavedSettings, "SKRoadAutopilotTestCourse", "");
        static std::string last_course;
        if (std::string(course) != last_course)
        {
            // the course changed or was cleared: nothing of the last one is kept (its path, its route)
            last_course = course;
            sCourse.set = false;
            sCourseS = -1;
            sLap.clear();       // read again: the course file may have been remade, or the arena track another
            sLapS.clear();
            sLapRead = false;
            sLapArena = false;
            sPath.clear();
            sPathTime = -100.0;
            sRoute.clear();
            sView.path.clear();
        }
        // keep + sKeepExtra: the left lane's line while passing (traffic)
        // walked along the way travelled, not the nose: after a recovery the nose may point anywhere
        std::vector<LanePoint> before = sPath;
        if (readRoad(pos, wx, wy, speed, keep + (sOn ? sKeepExtra : 0.0), now, course))
        {
            sPathFrame = sFrame;
        }
        if (!preview && std::string(course).empty())
        {
            watchBanLines();
            vetLine(pos, now);
        }
        if (!preview && std::string(course).empty() && lineBackOverTrail(sPath))
        {
            // back the way it came, over the trail: the line had before stands
            LL_INFOS("SKRoadDriver") << "line refused: it ran back over the trail at " << placeText(pos) << LL_ENDL;
            sPath = std::move(before);
            sView.source = "refused (back over the trail)";
        }
    }
    const bool road = !sPath.empty();
    noteLineWay(now);
    if (!preview)
    {
        tickRoute(pos, now);
    }
    writeView(now, pos, yaw);
    if (preview)
    {
        sView.valid = true;     // the road's pieces and segments are drawn, path or none
        sView.car = pos;
        sView.yaw = yaw;
        sView.path = roadForView();
        sView.tapping = 0;
        sView.waiting = false;
        return;
    }
    // the roads driven, for choosing junctions (on real roads only: not a test course or the sweep)
    {
        static LLCachedControl<std::string> course_d(gSavedSettings, "SKRoadAutopilotTestCourse", "");
        static LLCachedControl<S32> sweep_d(gSavedSettings, "SKRoadAutopilotSweep", 0);
        if (std::string(course_d).empty() && S32(sweep_d) == 0 && !pwmSweepOn() && speed > 1.0)
        {
            noteDriven(pos, now);
        }
    }
    // the sweep needs no road, and the bench runs it before any track: neither stops for want of one
    static LLCachedControl<S32> sweep_on(gSavedSettings, "SKRoadAutopilotSweep", 0);
    static LLCachedControl<bool> bench_on(gSavedSettings, "SKRoadAutopilotBench", false);
    // and not on a road-shaped Linden strip: there the road is coming (a teleport or a rez just made: its pieces
    // arrive over seconds), and the wait for it to load (roadNotLoaded, up to ROAD_WAIT_S) decides instead; the
    // mission lost the road 50 m from a fresh rez this way
    // stopped first, on the road (braked below), never left rolling on past its edge onto someone's land
    if (!road && sTurnPhase == 0 && now - sLastSeenRoad > LOST_S && now > LOST_S && S32(sweep_on) == 0 && !pwmSweepOn() && !bench_on && !roadNotLoaded(pos, yaw)
        && (speed < 0.5 || now - sLastSeenRoad > LOST_S + LOST_BRAKE_S))
    {
        sendKeys(AGENT_CONTROL_AT_NEG, note);
        stop("lost the road, stopping");
        return;
    }
    if (!road)
    {
        sView.valid = true;     // waiting: what it sees still drawn
        sView.car = pos;
        sView.yaw = yaw;
        sView.path.clear();
        sView.waiting = true;
    }
    if (!road)
    {
        note = "waiting for the road";
    }

    U32 keys = 0;
    static LLCachedControl<bool> probe_on(gSavedSettings, "SKRoadAutopilotProbe", false);
    const bool probe = probe_on;
    const bool pwm = std::string(steer_mode_g) != "plan";
    // steering
    if (sKey)
    {
        keys |= sKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
    }
    else if (probe && now < sProbeQuietUntil)
    {
        // a probe tap's response being recorded: nothing else steers
    }
    else if (probe && road && sPath.size() >= 2 && speed > 3.0
             && now - sLastSteer >= PROBE_QUIET_S && [&] {
                    const LanePoint& q = sPath[0];
                    const F64 e = -(q.ax * (pos.mdV[VY] - q.y) - q.ay * (pos.mdV[VX] - q.x));
                    const F64 he = std::fabs(wrap(yaw - std::atan2(q.ay, q.ax))) * 180 / PI;
                    // on a test course (open flat land) the probe may tap off the line and turning a little; on a
                    // road only on it and steady
                    static LLCachedControl<std::string> course(gSavedSettings, "SKRoadAutopilotTestCourse", "");
                    const bool open = !std::string(course).empty();
                    return std::fabs(e) < (open ? 2.5 : PROBE_ON_LINE_M) && he < (open ? 15.0 : PROBE_ALIGNED_DEG)
                           && std::fabs(sYawRate) * 180 / PI < (open ? 8.0 : 3.0);
                }())
    {
        // Probe: on the line, pointing along it and steady, with nothing steered for a while: one test tap, of
        // each planned length in turn, alternating sides, left alone for PROBE_QUIET_S so its response is the
        // car's own (closed-loop taps are confounded with the swing they correct). Off the line, the normal
        // controller below brings it back first. The drive log's note names each ("probe 17").
        sProbeLast = now;
        // SKRoadAutopilotProbeLong: the long holds a bend takes (0.5-2 s), each left alone for 2 s past its
        // release (PROBE_LONG_QUIET_S); otherwise the planner's own taps
        static LLCachedControl<bool> probe_long(gSavedSettings, "SKRoadAutopilotProbeLong", false);
        const S32 n = probe_long ? S32(std::size(PROBE_LONG_HOLDS)) : S32(std::size(HOLDS));
        const S32 hold = probe_long ? PROBE_LONG_HOLDS[(sProbeStep / 2) % n] : HOLDS[(sProbeStep / 2) % n];
        sProbeQuietUntil = now + (probe_long ? hold * DT + PROBE_LONG_QUIET_S : PROBE_QUIET_S);
        sKey = (sProbeStep % 2) ? -1 : 1;
        ++sProbeStep;
        sKeySince = sFrame;
        sKeyUntil = sFrame + hold;
        keys |= sKey > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
        note = "probe " + std::to_string(hold);
    }
    else if (road && pwm && sPath.size() >= 2 && lineSeen() >= STEER_MIN_LINE_M && speed > 1.0 && !probeQuiet(pos, yaw, probe))
    {
        const auto [d, h] = pwmSteer(pos, yaw, speed, now, note);
        if (d)
        {
            sLastSteer = now;
            sKey = d;
            sKeySince = sFrame;
            sKeyUntil = sFrame + h;
            keys |= d > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
        }
    }
    else if (road && !pwm && sFrame - sLastRelease >= MIN_GAP / DT && sFrame >= sNextDecide && !probeQuiet(pos, yaw, probe))
    {
        sNextDecide = sFrame + S32(std::lround(DECIDE_EVERY / DT));
        const auto [d, h] = decide(pos.mdV[VX], pos.mdV[VY], yaw, speed, sYawRate, boundary, keep);
        if (d)
        {
            sLastSteer = now;
            sKey = d;
            sKeySince = sFrame;
            sKeyUntil = sFrame + h;
            keys |= d > 0 ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG;
        }
    }

    // speed: slower toward and just after a border; coast when too fast; gears as Nexii drives
    F32 target = cruise;
    if (boundary < speed * CROSSING_SLOW_S || now - sLastCrossingT < 1.5)
    {
        target = std::min(target, CROSSING_SPEED);
    }
    if (!road)
    {
        target = 0.f;
    }
    if (now - sAccelT > 0.5)
    {
        sAccel = sAccelT > 0 ? (speed - sAccelV) / (now - sAccelT) : 0.0;
        sAccelT = now;
        sAccelV = speed;
    }
    // The speed the car will have once the throttle sent in the last THROTTLE_LAG_S has acted: the speed seen
    // (from the sim's updates, extrapolated) lags the key by that much, and the car answers it hard
    // (THROTTLE_ACCEL / THROTTLE_DECEL, from Nexii's cruise logs, 2026-11-05).
    sThrottleLog.push_back({ now, sThrottle });
    while (sThrottleLog.size() > 1 && sThrottleLog[1].first < now - THROTTLE_LAG_S)
    {
        sThrottleLog.pop_front();
    }
    F64 ahead = speed;
    for (size_t i = 0; i < sThrottleLog.size(); ++i)
    {
        const F64 t0 = std::max(sThrottleLog[i].first, now - THROTTLE_LAG_S);
        const F64 t1 = i + 1 < sThrottleLog.size() ? sThrottleLog[i + 1].first : now;
        if (t1 > t0)
        {
            ahead += (t1 - t0) * (sThrottleLog[i].second ? THROTTLE_ACCEL : -THROTTLE_DECEL);
        }
    }
    ahead = std::max(0.0, ahead);
    if (use_gears)
    {
        // a cruise: W held on the road, the speed the gear's (pickGear chooses it from the bends ahead); no
        // target speed to throttle to, so no pulsing of W, which rocked the car and flashed its brake lights. Off
        // the road it lets go and the car rolls to a stop. The one exception is a bend tighter than the car turns
        // at its speed (bendLift), as Nexii takes one by hand: W let go a moment at a time.
        pickGear(speed, now, road, keys, note);
        // the lane guard lets W off only while it is moving the car back (below 3 m/s it never holds W back: set
        // once and never cleared at a standstill, it stopped the car for good)
        if (road && !bendLift(speed, now) && !(sLaneGuard && speed > 3.0))
        {
            keys |= AGENT_CONTROL_AT_POS;
            measureGearTop(speed, now);
        }
        else if (road)
        {
            note += note.empty() ? "lift" : ", lift";
        }
        else if (speed > 0.5 && sTurnPhase == 0 && sRecoverPhase == 0)
        {
            // the line lost: braked to a stop where it is (on the road), the wheel left alone
            keys &= ~(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
            sKey = 0;
            keys |= AGENT_CONTROL_AT_NEG;
            note += note.empty() ? "no line: braking" : ", no line: braking";
        }
        else
        {
            note += note.empty() ? "coast" : ", coast";
        }
    }
    else if (speed < target - 0.5f)
    {
        keys |= AGENT_CONTROL_AT_POS;
    }
    else if (speed > target + 3.f)
    {
        keys |= AGENT_CONTROL_AT_NEG;
    }
    if (road && sTurnPhase == 0 && sRecoverPhase == 0)
    {
        // the turns ahead (a junction's first): W let go above the speed they allow, braked well above it while no
        // turn key is held (braking while turning turns this car the other way)
        const F64 allow = turnAllowance(pos, speed, sTravelX, sTravelY);
        noteJunctionApproach(pos, speed, allow, sTravelX, sTravelY);
        if (speed > allow)
        {
            keys &= ~AGENT_CONTROL_AT_POS;
            const bool turning_now = (keys & (AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG)) != 0;
            if (speed > allow + 1.5 && !turning_now)
            {
                keys |= AGENT_CONTROL_AT_NEG;
            }
            const std::string what = llformat("turn ahead (r %.0f m at %.0f m): slowing to %.1f", sTurnAllowR, sTurnAllowAt, allow);
            note += note.empty() ? what : ", " + what;
        }
    }
    benchStep(now, note);
    static LLCachedControl<std::string> course_g(gSavedSettings, "SKRoadAutopilotTestCourse", "");
    static LLCachedControl<S32> sweep_gear_c(gSavedSettings, "SKRoadAutopilotSweep", 0);
    const bool pwm_sweep = pwmSweepOn();     // the PWM sweep stands in for the sweep below: no road, its own keys
    const S32 sweep_gear = pwm_sweep ? std::max(1, S32(sweep_gear_c)) : S32(sweep_gear_c);
    const bool was_recovering = sRecoverPhase != 0;
    if (S32(sweep_gear) == 0 && sTurnPhase == 0 && recovering(keys, speed, now, pos, yaw, note))
    {
        keys = sRecoverKeys;        // stuck against something: backing off it (recovering)
    }
    else if (S32(sweep_gear) == 0 && sTurnPhase == 0 && sRecoverPhase == 0 && std::string(course_g).empty()
             && std::fabs(wrap(travelYaw() - yaw)) * 180 / PI > FACING_BACK_DEG)
    {
        // pointing back against the way travelled (a recovery left it turned round): turned back to it, never
        // driven the way the nose now points
        startTurnRound(false, pos, yaw, now,
                       was_recovering ? "unstuck facing back: turning round to carry on" : "facing back against the way travelled: turning round");
    }
    else if (pwm_sweep)
    {
        keys = pwmSweepKeys(now, speed, pos, yaw, note);
    }
    else if (S32(sweep_gear) > 0)
    {
        keys = sweepKeys(S32(sweep_gear), now, speed, pos, yaw, note);     // the sweep drives: no road, its own keys
    }
    else
    {
        sSweepStart = -1.0;
    }
    if (S32(sweep_gear) == 0 && sRecoverPhase == 0 && sTurnPhase == 0 && std::string(course_g).empty() && roadNotLoaded(pos, yaw))
    {
        // the Linden road parcel goes on ahead and no road has loaded on it: a full stop, the wheel left alone,
        // until the line reaches as far (a slow region loading at a junction sent the car down the only road it
        // could see, the wrong one)
        keys &= ~(AGENT_CONTROL_AT_POS | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
        sKey = 0;
        if (speed > 0.3)
        {
            keys |= AGENT_CONTROL_AT_NEG;
        }
        if (!sWaitingForRoad)
        {
            say("waiting for the road to load");
            sWaitSince = now;
        }
        sWaitingForRoad = true;
        if (now - sWaitSince > ROAD_WAIT_S)
        {
            // Linden land, but no road comes (a park, an empty Linden parcel): not a road to wait on
            say("no road has loaded here in " + std::to_string(S32(ROAD_WAIT_S)) + " s: stopping");
            gSavedSettings.setBOOL("SKRoadAutopilot", false);
        }
        note += note.empty() ? "road not loaded: stopped" : ", road not loaded: stopped";
    }
    else if (S32(sweep_gear) == 0 && sRecoverPhase == 0 && sTurnPhase == 0 && std::string(course_g).empty())
    {
        sWaitingForRoad = false;
        // the road it can see: the line's length ahead of the car against the distance it covers in
        // SIGHT_LIFT_S (off W, and down a gear) and SIGHT_BRAKE_S (braking too, when not turning): at 15 m/s on a
        // line of a few metres it took a bend above a beach in Gear 3 and went over
        F64 seen = 0;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            seen += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
        }
        const bool turning = (keys & (AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG)) != 0;
        const bool off_edge = road && offRoadAhead(pos, yaw, speed);
        const bool drop_beside = off_edge && sDropSide != 0;
        const bool refused = !off_edge && refusedAhead(pos, yaw, speed);
        const bool edge = off_edge || refused;
        // what the edge guard took for an edge, said once a spell: the stops it held on clear road (Soulgiver,
        // Nangrim, 2026-11-07) left nothing saying which check fired
        if (edge && sEdgeSince < 0)
        {
            sEdgeSince = now;
            LL_INFOS("SKRoad") << "edge: " << (off_edge ? (sDropSide ? "off the road ahead, drop beside" : "off the road ahead")
                                                       : (sRefusedWhy ? sRefusedWhy : "refused"))
                               << (refused ? " at " + std::to_string(sRefusedAtM) + " m" : std::string())
                               << ", speed " << speed << ", line " << lineSeen() << " m" << LL_ENDL;
        }
        else if (!edge)
        {
            sEdgeSince = -1.0;
        }
        // an edge slows the car to a crawl and never holds it: held stopped past EDGE_HOLD_S with a line ahead, it
        // creeps on along it at EDGE_CRAWL_MPS (a real road end or a line's end stops it below, whatever this says)
        const bool creep = edge && !drop_beside && now - sEdgeSince > EDGE_HOLD_S && sPath.size() >= 2 && lineSeen() >= EDGE_CREEP_LINE_M;
        const bool short_sight = speed > 3.0 && seen < std::max(SIGHT_MIN_M, speed * SIGHT_LIFT_S);
        // up a slope of a road that goes on (no road end within what it takes to stop): W kept for a short line (a
        // crest shortens it), let go for an edge with no drop beside, never braked
        const F64 stop_m = DEAD_END_M + speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S;
        const bool end_near = sView.road_end
                              && (sView.road_end_x - pos.mdV[VX]) * sTravelX + (sView.road_end_y - pos.mdV[VY]) * sTravelY < stop_m;
        const bool uphill = road && !end_near && !drop_beside && gradeAhead() > UPHILL_GRADE;
        if (uphill && (edge || short_sight))
        {
            if (edge && !(creep && speed < EDGE_CRAWL_MPS))
            {
                keys &= ~AGENT_CONTROL_AT_POS;
            }
            const char* what = edge ? "uphill edge: W off" : "uphill short sight: W kept";
            note += note.empty() ? what : std::string(", ") + what;
        }
        else if (edge || short_sight)
        {
            // off W and a gear down; S (braking) only while not turning: braking with a turn key held, this car
            // turned the other way, toward the drop
            keys &= ~AGENT_CONTROL_AT_POS;
            const bool brake = edge || seen < speed * SIGHT_BRAKE_S;
            if (brake && !turning && speed > EDGE_CRAWL_MPS)
            {
                keys |= AGENT_CONTROL_AT_NEG;
            }
            if (creep && speed < EDGE_CRAWL_MPS)
            {
                keys |= AGENT_CONTROL_AT_POS;
            }
            if (sGear > 1 && now - sLastShift > SHIFT_DOWN_GAP_S && !(keys & (AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG)))
            {
                sLastShift = now;
                sEdgeDownShift = now;
                sShiftDir = -1;
                sShiftEnd = now + SHIFT_HOLD_S;
                keys |= shiftKey(-1);
            }
            const char* what = creep ? "edge: crawling on along the line"
                               : edge ? (sDropSide ? "edge (drop beside): slowing" : "edge: slowing") : "short sight: slowing";
            note += note.empty() ? what : std::string(", ") + what;
        }
        // a dead end ahead (the road's last part, a rounded cap: Route 5's end): slowed to a stop short of it, then
        // turned round and driven back; it drove off the end into a bench
        F64 end_ahead = 1e9;
        if (road && sPath.size() >= 2)
        {
            const LanePoint& e = sPath.back();
            end_ahead = (e.x - pos.mdV[VX]) * sTravelX + (e.y - pos.mdV[VY]) * sTravelY;
        }
        if (road && sView.road_end)
        {
            end_ahead = (sView.road_end_x - pos.mdV[VX]) * sTravelX + (sView.road_end_y - pos.mdV[VY]) * sTravelY;
        }
        // a line that ends where the road does not (the walk found no way on, or a cut): stopped short of its end,
        // on the road, and held until the line goes on (Nangrim: at a T with no line on the car crept into the wall,
        // too slow for the short sight's braking)
        if (road && !sView.road_end && !sPlace.want
            && end_ahead < LINE_END_STOP_M + speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S)
        {
            keys &= ~(AGENT_CONTROL_AT_POS | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
            sKey = 0;
            if (speed > 0.3)
            {
                keys |= AGENT_CONTROL_AT_NEG;
            }
            note += note.empty() ? "line ends: stopping" : ", line ends: stopping";
        }
        // only the model's road end: a walk that stopped because its next part has not loaded is no end
        const bool dead_end = road && sView.road_end && !sView.jump && end_ahead < DEAD_END_M && end_ahead > -5.0;
        if (sPlace.want && std::hypot(pos.mdV[VX] - sPlace.lip_x, pos.mdV[VY] - sPlace.lip_y) > PLACE_FORGET_M)
        {
            sPlace = PlaceRun();    // gone another way, or placed by hand
        }
        const F64 to_lip = sPlace.want ? (sPlace.lip_x - pos.mdV[VX]) * sPlace.lip_dx + (sPlace.lip_y - pos.mdV[VY]) * sPlace.lip_dy : 1e9;
        if (sPlace.want && to_lip < DEAD_END_M)
        {
            // a jump to be placed across: stopped short of the lip and held there, never turned round
            sDeadEndSince = -1.0;
            if (to_lip < DEAD_END_STOP_M + speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S)
            {
                keys &= ~(AGENT_CONTROL_AT_POS | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
                sKey = 0;
                if (speed > 0.3)
                {
                    keys |= AGENT_CONTROL_AT_NEG;
                }
                note += note.empty() ? "jump: stopping to be placed across" : ", jump: stopping to be placed across";
                if (speed < 0.5 && !sPlace.asked)
                {
                    sPlace.asked = true;
                    say("stopped at a jump: asking to be placed across");
                    fireEvent("place_across", LLSD().with("to", llsd::array(sPlace.to.x, sPlace.to.y, sPlace.to.z))
                                                  .with("heading", llsd::array(sPlace.to.dx, sPlace.to.dy))
                                                  .with("road_on", sPlace.to.road_on), pos);
                }
            }
        }
        else if (!dead_end)
        {
            sDeadEndSince = -1.0;
        }
        else
        {
            if (sDeadEndSince < 0)
            {
                sDeadEndSince = now;
            }
            // stop short of it: off W and braking (no turn key with S) once the end is within what it takes to stop
            if (end_ahead < DEAD_END_STOP_M + speed * speed / (2 * THROTTLE_DECEL) + speed * THROTTLE_LAG_S)
            {
                keys &= ~(AGENT_CONTROL_AT_POS | AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
                sKey = 0;
                if (speed > 1.0)
                {
                    keys |= AGENT_CONTROL_AT_NEG;
                }
                note += note.empty() ? "dead end: stopping" : ", dead end: stopping";
                if (now - sDeadEndSince > DEAD_END_HOLD_S && (speed < 0.5 || end_ahead < 3.0))
                {
                    LL_INFOS("SKRoadDriver") << "dead end (" << sView.walk_end << ")" << LL_ENDL;
                    sDeadEndSince = -1.0;
                    fireEvent("dead_end", LLSD().with("walk_end", sView.walk_end), pos);
                    startTurnRound(true, pos, yaw, now, "dead end at " + placeText(pos) + ": turning round");
                }
            }
        }
        // the other vehicles on the road: followed, and passed on two lanes
        if (road)
        {
            // the cruise: the top of the gear it is in, where it has been measured
            const GearTops& tops = carGears();
            const auto top = tops.find(sGear);
            const F64 cruise_v = std::max<F64>(F32(cruise), top != tops.end() ? top->second : 0.0);
            traffic(car, pos, speed, cruise_v, keep, now, keys, note);
        }
    }
    if (S32(sweep_gear) == 0)
    {
        U32 turn_keys = 0;
        if (turningRound(speed, now, pos, yaw, sYawRate, turn_keys, note))
        {
            keys = turn_keys;       // turning round: its keys alone
            sKey = 0;
        }
    }
    if (S32(sweep_gear) == 0 && sTurnPhase == 0 && sRecoverPhase == 0 && std::string(course_g).empty())
    {
        if (road && waterDropAhead(pos, yaw, speed))
        {
            // off W and braking (no turn key with S: braking with one held, this car turned the other way)
            keys &= ~AGENT_CONTROL_AT_POS;
            if (speed > 0.5)
            {
                keys &= ~(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
                sKey = 0;
                keys |= AGENT_CONTROL_AT_NEG;
            }
            note += note.empty() ? "drop to water ahead: stopping" : ", drop to water ahead: stopping";
        }
        takeJump(road, pos, yaw, speed, now, keys, note);
    }
    else
    {
        sJump = JumpRun();
    }
    // an edge ahead not known yet (sEdgeWait): a crawl, held CROSS_GUARD_MARGIN_M short; steering left as it is
    if (sEdgeWait.on && !preview)
    {
        const bool hold = sEdgeWait.to_edge <= CROSS_GUARD_MARGIN_M + speed * THROTTLE_LAG_S;
        const F64 cap = hold ? 0.0 : EDGE_CRAWL_MPS;
        if (speed >= cap)
        {
            keys &= ~AGENT_CONTROL_AT_POS;
        }
        // down to the crawl by the hold point (or to a stop at it), progressively: S in pulses, and while S is down
        // above a crawl the turn keys let go, so the car never brakes hard while steering
        if (speed > cap + 0.5)
        {
            const F64 room = hold ? 0.0 : std::max(0.0, sEdgeWait.to_edge - CROSS_GUARD_MARGIN_M);
            if (guardBrakePress(speed, cap, room, now))
            {
                keys |= AGENT_CONTROL_AT_NEG;
                if (speed > 4.0)
                {
                    keys &= ~U32(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
                }
            }
        }
        const char* what = hold ? "edge not known yet: holding short" : "edge not known yet: crawling";
        note += note.empty() ? what : std::string(", ") + what;
    }
    sendKeys(keys, note);
    sThrottle = (sLastSentKeys & AGENT_CONTROL_AT_POS) != 0;

    sView.valid = true;
    sView.car = pos;
    sView.yaw = yaw;
    sView.path = roadForView();
    sView.tapping = (keys & AGENT_CONTROL_YAW_POS) ? 1 : (keys & AGENT_CONTROL_YAW_NEG) ? -1 : 0;
    sView.waiting = !road;
    if (S32(sweep_gear) > 0)
    {
        // the sweep follows no line: the overlay draws the car's own track over the last SWEEP_TRAIL_S instead,
        // so its circles show
        if (sSweepTrail.empty() || std::hypot(pos.mdV[VX] - sSweepTrail.back().first.mdV[VX],
                                              pos.mdV[VY] - sSweepTrail.back().first.mdV[VY]) > 1.0)
        {
            sSweepTrail.push_back({ pos, now });
        }
        while (!sSweepTrail.empty() && sSweepTrail.front().second < now - SWEEP_TRAIL_S)
        {
            sSweepTrail.pop_front();
        }
        sView.path.clear();
        for (size_t i = 0; i + 1 < sSweepTrail.size(); ++i)
        {
            const LLVector3d& a = sSweepTrail[i].first;
            const LLVector3d& b = sSweepTrail[i + 1].first;
            const F64 L = std::max(1e-6, std::hypot(b.mdV[VX] - a.mdV[VX], b.mdV[VY] - a.mdV[VY]));
            LanePoint lp;
            lp.x = a.mdV[VX];
            lp.y = a.mdV[VY];
            lp.z = a.mdV[VZ];
            lp.ax = (b.mdV[VX] - a.mdV[VX]) / L;
            lp.ay = (b.mdV[VY] - a.mdV[VY]) / L;
            lp.lane_w = 4;
            lp.road_w = 8;
            lp.lanes = 2;
            lp.left_x = lp.right_x = lp.x;
            lp.left_y = lp.right_y = lp.y;
            sView.path.push_back(lp);
        }
        sView.waiting = false;
    }
    else
    {
        sSweepTrail.clear();
    }

    // off the line, + right of it: to the nearest stretch of it (the first point lies a few metres behind the
    // car, and across a bend its tangent is far off the line beside the car)
    F64 err0 = 0;
    {
        F64 best = 1e18;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            const LanePoint& a = sPath[i];
            const LanePoint& b = sPath[i + 1];
            const F64 sx = b.x - a.x, sy = b.y - a.y, L2 = std::max(1e-9, sx * sx + sy * sy);
            const F64 f = std::clamp(((pos.mdV[VX] - a.x) * sx + (pos.mdV[VY] - a.y) * sy) / L2, 0.0, 1.0);
            const F64 dx = pos.mdV[VX] - (a.x + f * sx), dy = pos.mdV[VY] - (a.y + f * sy);
            const F64 d2 = dx * dx + dy * dy;
            if (d2 < best)
            {
                best = d2;
                err0 = (sx * dy - sy * dx) > 0 ? -std::sqrt(d2) : std::sqrt(d2);
            }
        }
    }
    sLastErr = sPath.size() >= 2 ? err0 : 99.0;
    watchLine(now, pos, yaw, speed, note);
    sLog << now << ',' << speed << ',' << yaw * 180 / PI << ',' << sYawRate * 180 / PI << ','
         << (sYawRate * 180 / PI - modelRate(sFrame - 1)) << ',' << err0 << ',' << sPath.size() << ','
         << (sPath.empty() ? 0.0 : sPath[0].road_w) << ',' << (sPath.empty() ? 0 : sPath[0].lanes) << ',' << boundary
         << ',' << target << ',' << std::hex << keys << std::dec << ',' << sGain << ',' << sShift << ',' << sGear << ','
         << note << '\n';
}


// ---------------------------------------------------------------- the drive mission's queries

namespace skroad::drive
{
    constexpr char PLACE_DEMAND[] = "road-mission-place";
    bool sPlacing = false;
}

LLSD sk_road_driver::unsafeCrossing()
{
    return sGuard.hazard;
}

bool sk_road_driver::isCrossing()
{
    return sOn && sCross.on;
}

bool sk_road_driver::isRunning()
{
    return sOn;
}

const std::string& sk_road_driver::lastStopReason()
{
    return sStopReason;
}

const std::string& sk_road_driver::lastSaid()
{
    return sLastSaid;
}

F64 sk_road_driver::roadAhead(F64 x, F64 y, F64 z, F64 hx, F64 hy)
{
    if (sOn)
    {
        return -1.0;
    }
    const F64 L = std::hypot(hx, hy);
    if (L < 1e-6)
    {
        return 0.0;
    }
    refreshShapes(LLVector3d(x, y, z));
    const std::vector<LanePoint> line = boxWalk(x, y, z, hx / L, hy / L, 8.0, 0.0);
    F64 len = 0;
    for (size_t i = 0; i + 1 < line.size(); ++i)
    {
        len += std::hypot(line[i + 1].x - line[i].x, line[i + 1].y - line[i].y);
    }
    return len;
}

bool sk_road_driver::placeOnRoad(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 half_width, RoadPlace& out)
{
    if (sOn)
    {
        return false;       // the model is the running driver's, about its car
    }
    const F64 L = std::hypot(hx, hy);
    if (L < 1e-6)
    {
        return false;
    }
    hx /= L;
    hy /= L;
    if (!sPlacing)
    {
        sPlacing = true;
        sShapesRead = false;
        sGatherDirty = true;
        SKWorldCensus::instance().addDemand(PLACE_DEMAND, shapeReach() + 2.f);
    }
    refreshShapes(LLVector3d(x, y, z));
    const F64 kept_w = sCarHalfW;
    sCarHalfW = half_width;
    LanePoint lp;
    const bool found = laneAt(x, y, z, hx, hy, 0.0, lp);
    const F64 shift = found && lp.lanes == 2 ? laneShift(0.0) : 0.0;
    sCarHalfW = kept_w;
    if (!found)
    {
        return false;
    }
    out.lane_x = lp.x;
    out.lane_y = lp.y;
    out.centre_x = lp.x - lp.ay * shift;
    out.centre_y = lp.y + lp.ax * shift;
    out.z = lp.z;
    out.dir_x = lp.ax;
    out.dir_y = lp.ay;
    out.road_w = lp.road_w;
    out.lanes = lp.lanes;
    return true;
}

void sk_road_driver::placingDone()
{
    if (sPlacing)
    {
        sPlacing = false;
        SKWorldCensus::instance().removeDemand(PLACE_DEMAND);
        sShapesRead = false;        // the driver gathers its own from its car
        sGatherDirty = true;
    }
}

std::vector<sk_road_driver::LinePoint> sk_road_driver::currentLaneLine()
{
    std::vector<LinePoint> out;
    if (!sOn)
    {
        return out;
    }
    out.reserve(sPath.size());
    for (const LanePoint& q : sPath)
    {
        out.push_back({ q.x, q.y, q.z, S32(q.access) });
    }
    return out;
}

const std::string& sk_road_driver::walkEnd()
{
    static const std::string none;
    return sOn ? sView.walk_end : none;
}

sk_road_driver::RoadEnd sk_road_driver::roadEnd()
{
    RoadEnd e;
    if (!sOn)
    {
        return e;
    }
    e.end = sView.road_end;
    e.x = sView.road_end_x;
    e.y = sView.road_end_y;
    e.dx = sView.road_end_dx;
    e.dy = sView.road_end_dy;
    e.jump = sView.jump;
    e.lip_x = sView.jump_x;
    e.lip_y = sView.jump_y;
    e.lip_z = sView.jump_z;
    e.land_x = sView.land_x;
    e.land_y = sView.land_y;
    e.land_z = sView.land_z;
    e.need_speed = sJumpNeed;
    e.top_speed = sJumpTop;
    return e;
}

bool sk_road_driver::wantsPlaceAcross(PlaceAcross& out)
{
    if (!sPlace.asked)
    {
        return false;
    }
    out = sPlace.to;
    return true;
}

void sk_road_driver::placedAcross()
{
    sPlace = PlaceRun();
    sJump = JumpRun();
}

S32 sk_road_driver::gear()
{
    return sGear;
}

void sk_road_driver::setEventHook(std::function<void(const std::string& kind, const LLSD& data)> hook)
{
    sEventHook = std::move(hook);
}

namespace
{
    std::function<bool()> sOverlayHider;
}

void sk_road_driver::setOverlayHider(std::function<bool()> hidden)
{
    sOverlayHider = std::move(hidden);
}

bool skroad::drive::overlayHidden()
{
    return sOverlayHider && sOverlayHider();
}
