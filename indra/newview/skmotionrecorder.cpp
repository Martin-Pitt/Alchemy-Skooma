/**
 * @file skmotionrecorder.cpp
 * @brief Records object updates, per-frame object motion and the camera to a binary log.
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

#include "skmotionrecorder.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcircuit.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "message.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <ctime>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
#pragma pack(push, 1)
    struct FrameRec
    {
        U32 frame;
        F64 frame_secs;      // LLFrameTimer::getElapsedSeconds()
        U64 total_us;        // totalTime()
        F64 msg_secs;        // LLMessageSystem::getMessageTimeSeconds(), the circuit clock
        F32 frame_interval;  // gFrameIntervalSeconds
        F32 frame_dt_clamped;
        U32 updates;
        F32 network_secs;
        U64 region_handle;
        F32 time_dilation;
        F32 ping_ms;
        F32 ping_avg_ms;
        F64 last_packet_in;  // agent region circuit, message clock
        U32 tracked;
        U32 dropped;
        // Stage times of the previous frame, which finishes after its own record is written.
        F32 objects_ms;
        F32 move_ms;
        F32 camera_ms;
        F32 idle_ms;
        F32 geom_ms;
        F32 render_ms;       // display() less geom_ms and swap_ms
        F32 swap_ms;
        F32 flush_ms;
        U32 rebuilds;        // drawables rebuilt by LLPipeline::updateGeom
        U32 pad;
    };

    struct CameraRec
    {
        U32 frame;
        F64 cam_pos[3];      // global
        F32 cam_rot[4];
        F64 focus_pos[3];    // global
        U8  focus_on_avatar;
        U8  camera_mode;
        U8  pad[2];
        U32 focus_local_id;
        F32 focus_offset[3];
        F32 avatar_render_pos[3]; // agent
        U32 sit_root_local_id;
        F64 focus_raw[3];    // global, before the camera follow smoothing
    };

    struct UpdateRec
    {
        U32 frame;
        F64 msg_secs;
        U64 total_us;
        U32 local_id;
        U64 region_handle;
        U8  update_type;
        U8  stale;
        U8  is_static;
        U8  drop;            // SKMotionRecorder::EDropReason
        U32 packet_id;
        F32 time_dilation;
        F32 ping_delay;
        F32 raw_pos[3];      // parent-relative, before ping interpolation
        F32 raw_vel[3];
        F32 raw_acc[3];
        F32 raw_rot[4];
        F32 raw_omega[3];
        F32 pre_pos[3];      // interpolated state just before the update
        F32 pre_vel[3];
        F32 pre_rot[4];
        F32 applied_pos[3];
        F32 final_vel[3];    // after ALMotionPredictor::finalVelocity
        U32 crc;
        U32 parent_id;
        U64 pre_region_handle; // region pre_pos is relative to
        F32 used_vel[3];     // after clamped-velocity recovery, before ping interpolation
        U8  recover;         // 0 none, 1 ratio, 2 fallback, 3 clamped but not recovered
        U8  pad[3];
    };

    struct ObjectRec
    {
        U32 frame;
        U32 local_id;
        U64 region_handle;
        F32 pos[3];          // region-local
        F32 vel[3];
        F32 acc[3];
        F32 rot[4];
        F32 omega[3];
        F32 dt;
        F32 phase_out;
        U8  is_static;
        U8  edge_clipped;
        U8  stepped;
        U8  is_avatar;
        F32 render_pos[3];   // drawable agent position after updateMove
        F32 cam_dist;
    };

    struct InfoRec
    {
        U32 frame;
        U32 local_id;
        U8  uuid[16];
        U32 parent_id;
        U64 region_handle;
        F32 scale[3];
        U8  pcode;
        U8  flags;           // 1 physical, 2 phantom, 4 temp on rez, 8 avatar, 16 attachment
        U8  pad[2];
    };
#pragma pack(pop)

    static_assert(sizeof(FrameRec)  == 120,  "FrameRec layout");
    static_assert(sizeof(CameraRec) == 128, "CameraRec layout");
    static_assert(sizeof(UpdateRec) == 208, "UpdateRec layout");
    static_assert(sizeof(ObjectRec) == 108, "ObjectRec layout");
    static_assert(sizeof(InfoRec)   == 52,  "InfoRec layout");

    struct Field
    {
        const char* name;
        const char* type;
        U32         count;
        size_t      offset;
    };

#define SK_FIELD(rec, member, type, count) { #member, type, count, offsetof(rec, member) }

    const Field sFrameFields[] = {
        SK_FIELD(FrameRec, frame, "u32", 1), SK_FIELD(FrameRec, frame_secs, "f64", 1),
        SK_FIELD(FrameRec, total_us, "u64", 1), SK_FIELD(FrameRec, msg_secs, "f64", 1),
        SK_FIELD(FrameRec, frame_interval, "f32", 1), SK_FIELD(FrameRec, frame_dt_clamped, "f32", 1),
        SK_FIELD(FrameRec, updates, "u32", 1), SK_FIELD(FrameRec, network_secs, "f32", 1),
        SK_FIELD(FrameRec, region_handle, "u64", 1), SK_FIELD(FrameRec, time_dilation, "f32", 1),
        SK_FIELD(FrameRec, ping_ms, "f32", 1), SK_FIELD(FrameRec, ping_avg_ms, "f32", 1),
        SK_FIELD(FrameRec, last_packet_in, "f64", 1), SK_FIELD(FrameRec, tracked, "u32", 1),
        SK_FIELD(FrameRec, dropped, "u32", 1),
        SK_FIELD(FrameRec, objects_ms, "f32", 1), SK_FIELD(FrameRec, move_ms, "f32", 1),
        SK_FIELD(FrameRec, camera_ms, "f32", 1), SK_FIELD(FrameRec, idle_ms, "f32", 1),
        SK_FIELD(FrameRec, geom_ms, "f32", 1), SK_FIELD(FrameRec, render_ms, "f32", 1),
        SK_FIELD(FrameRec, swap_ms, "f32", 1), SK_FIELD(FrameRec, flush_ms, "f32", 1),
        SK_FIELD(FrameRec, rebuilds, "u32", 1),
    };

    const Field sCameraFields[] = {
        SK_FIELD(CameraRec, frame, "u32", 1), SK_FIELD(CameraRec, cam_pos, "f64", 3),
        SK_FIELD(CameraRec, cam_rot, "f32", 4), SK_FIELD(CameraRec, focus_pos, "f64", 3),
        SK_FIELD(CameraRec, focus_on_avatar, "u8", 1), SK_FIELD(CameraRec, camera_mode, "u8", 1),
        SK_FIELD(CameraRec, focus_local_id, "u32", 1), SK_FIELD(CameraRec, focus_offset, "f32", 3),
        SK_FIELD(CameraRec, avatar_render_pos, "f32", 3), SK_FIELD(CameraRec, sit_root_local_id, "u32", 1),
        SK_FIELD(CameraRec, focus_raw, "f64", 3),
    };

    const Field sUpdateFields[] = {
        SK_FIELD(UpdateRec, frame, "u32", 1), SK_FIELD(UpdateRec, msg_secs, "f64", 1),
        SK_FIELD(UpdateRec, total_us, "u64", 1), SK_FIELD(UpdateRec, local_id, "u32", 1),
        SK_FIELD(UpdateRec, region_handle, "u64", 1), SK_FIELD(UpdateRec, update_type, "u8", 1),
        SK_FIELD(UpdateRec, stale, "u8", 1), SK_FIELD(UpdateRec, is_static, "u8", 1),
        SK_FIELD(UpdateRec, drop, "u8", 1),
        SK_FIELD(UpdateRec, packet_id, "u32", 1), SK_FIELD(UpdateRec, time_dilation, "f32", 1),
        SK_FIELD(UpdateRec, ping_delay, "f32", 1), SK_FIELD(UpdateRec, raw_pos, "f32", 3),
        SK_FIELD(UpdateRec, raw_vel, "f32", 3), SK_FIELD(UpdateRec, raw_acc, "f32", 3),
        SK_FIELD(UpdateRec, raw_rot, "f32", 4), SK_FIELD(UpdateRec, raw_omega, "f32", 3),
        SK_FIELD(UpdateRec, pre_pos, "f32", 3), SK_FIELD(UpdateRec, pre_vel, "f32", 3),
        SK_FIELD(UpdateRec, pre_rot, "f32", 4), SK_FIELD(UpdateRec, applied_pos, "f32", 3),
        SK_FIELD(UpdateRec, final_vel, "f32", 3), SK_FIELD(UpdateRec, crc, "u32", 1),
        SK_FIELD(UpdateRec, parent_id, "u32", 1), SK_FIELD(UpdateRec, pre_region_handle, "u64", 1),
        SK_FIELD(UpdateRec, used_vel, "f32", 3), SK_FIELD(UpdateRec, recover, "u8", 1),
    };

    const Field sObjectFields[] = {
        SK_FIELD(ObjectRec, frame, "u32", 1), SK_FIELD(ObjectRec, local_id, "u32", 1),
        SK_FIELD(ObjectRec, region_handle, "u64", 1), SK_FIELD(ObjectRec, pos, "f32", 3),
        SK_FIELD(ObjectRec, vel, "f32", 3), SK_FIELD(ObjectRec, acc, "f32", 3),
        SK_FIELD(ObjectRec, rot, "f32", 4), SK_FIELD(ObjectRec, omega, "f32", 3),
        SK_FIELD(ObjectRec, dt, "f32", 1), SK_FIELD(ObjectRec, phase_out, "f32", 1),
        SK_FIELD(ObjectRec, is_static, "u8", 1), SK_FIELD(ObjectRec, edge_clipped, "u8", 1),
        SK_FIELD(ObjectRec, stepped, "u8", 1), SK_FIELD(ObjectRec, is_avatar, "u8", 1),
        SK_FIELD(ObjectRec, render_pos, "f32", 3), SK_FIELD(ObjectRec, cam_dist, "f32", 1),
    };

    const Field sInfoFields[] = {
        SK_FIELD(InfoRec, frame, "u32", 1), SK_FIELD(InfoRec, local_id, "u32", 1),
        SK_FIELD(InfoRec, uuid, "u8", 16), SK_FIELD(InfoRec, parent_id, "u32", 1),
        SK_FIELD(InfoRec, region_handle, "u64", 1), SK_FIELD(InfoRec, scale, "f32", 3),
        SK_FIELD(InfoRec, pcode, "u8", 1), SK_FIELD(InfoRec, flags, "u8", 1),
    };

#undef SK_FIELD

    struct Candidate
    {
        ObjectRec rec;
        LLUUID    id;
        bool      forced;
    };

    llofstream             sFile;
    std::string            sPath;
    std::vector<U8>        sBuffer;
    std::vector<Candidate> sCandidates;
    std::unordered_set<LLUUID> sSeen;
    UpdateRec              sUpdate;

    U32 sFrame = 0;
    F64 sStartSecs = 0.0;
    F32 sDuration = 0.f;
    F32 sRadius = 0.f;
    U32 sMaxObjects = 0;
    U64 sNetworkStart = 0;
    F32 sStageMs[SKMotionRecorder::STAGE_COUNT] = {};
    F32 sPrevStageMs[SKMotionRecorder::STAGE_COUNT] = {};
    U32 sPrevRebuilds = 0;
    F32 sNetworkSecs = 0.f;
    U32 sUpdates = 0;
    U64 sFrameBytes = 0;
    LLUUID sFocusId;
    LLUUID sSitRootId;

    template<typename T>
    void append(U8 tag, const T& rec)
    {
        const size_t at = sBuffer.size();
        sBuffer.resize(at + 1 + sizeof(T));
        sBuffer[at] = tag;
        memcpy(&sBuffer[at + 1], &rec, sizeof(T));
    }

    void copy3(F32* dst, const LLVector3& v) { dst[0] = v.mV[0]; dst[1] = v.mV[1]; dst[2] = v.mV[2]; }
    void copy3d(F64* dst, const LLVector3d& v) { dst[0] = v.mdV[0]; dst[1] = v.mdV[1]; dst[2] = v.mdV[2]; }
    void copy4(F32* dst, const LLQuaternion& q) { dst[0] = q.mQ[0]; dst[1] = q.mQ[1]; dst[2] = q.mQ[2]; dst[3] = q.mQ[3]; }

    U64 regionHandle(const LLViewerObject* objectp)
    {
        const LLViewerRegion* regionp = objectp->getRegion();
        return regionp ? regionp->getHandle() : 0;
    }

    void noteSeen(const LLViewerObject* objectp)
    {
        if (!sSeen.insert(objectp->getID()).second)
        {
            return;
        }
        InfoRec rec = {};
        rec.frame = sFrame;
        rec.local_id = objectp->getLocalID();
        memcpy(rec.uuid, objectp->getID().mData, 16);
        const LLViewerObject* parentp = static_cast<const LLViewerObject*>(objectp->getParent());
        rec.parent_id = parentp ? parentp->getLocalID() : 0;
        rec.region_handle = regionHandle(objectp);
        copy3(rec.scale, objectp->getScale());
        rec.pcode = objectp->getPCode();
        rec.flags = (objectp->flagUsePhysics() ? 1 : 0) | (objectp->flagPhantom() ? 2 : 0)
                  | (objectp->flagTemporaryOnRez() ? 4 : 0) | (objectp->isAvatar() ? 8 : 0)
                  | (objectp->isAttachment() ? 16 : 0);
        append(SKMotionRecorder::REC_INFO, rec);
    }

    std::string jsonReal(F64 value)
    {
        return std::isfinite(value) ? llformat("%.17g", value) : std::string("null");
    }

    std::string jsonString(const std::string& in)
    {
        std::string out = "\"";
        for (char c : in)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += c;
            }
            else if ((unsigned char)c < 0x20)
            {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)(unsigned char)c);
                out += esc;
            }
            else
            {
                out += c;
            }
        }
        return out + "\"";
    }

    template<size_t N>
    void writeLayout(std::ostringstream& out, const char* name, U8 tag, size_t size, const Field (&fields)[N])
    {
        out << jsonString(name) << ":{\"tag\":" << (U32)tag << ",\"size\":" << size << ",\"fields\":[";
        for (size_t i = 0; i < N; ++i)
        {
            out << (i ? "," : "") << "[" << jsonString(fields[i].name) << "," << jsonString(fields[i].type)
                << "," << fields[i].count << "," << fields[i].offset << "]";
        }
        out << "]}";
    }

    void writeSetting(std::ostringstream& out, const char* name, bool& first)
    {
        LLControlVariable* control = gSavedSettings.getControl(name);
        if (!control)
        {
            return;
        }
        const LLSD value = control->getValue();
        out << (first ? "" : ",") << jsonString(name) << ":";
        first = false;
        if (value.isBoolean())
        {
            out << (value.asBoolean() ? "true" : "false");
        }
        else
        {
            out << jsonReal(value.asReal());
        }
    }

    std::string buildHeader(const std::string& start_utc, const ALMotionPredictor::Tuning& tuning)
    {
        std::ostringstream out;
        out.precision(17);
        out << "{\"format\":\"skmotion\",\"version\":4,\"endian\":\"little\",\"stage_times\":\"previous_frame\"";
        out << ",\"viewer\":" << jsonString(LLVersionInfo::instance().getChannelAndVersion());
        out << ",\"start_utc\":" << jsonString(start_utc);
        out << ",\"start_total_us\":" << (U64)totalTime();
        out << ",\"start_frame_secs\":" << (F64)LLFrameTimer::getElapsedSeconds();
        out << ",\"start_msg_secs\":" << LLMessageSystem::getMessageTimeSeconds().value();
        out << ",\"agent_id\":" << jsonString(gAgent.getID().asString());

        out << ",\"settings\":{";
        bool first = true;
        for (const char* name : { "VelocityInterpolate", "PingInterpolate", "InterpolationTime", "InterpolationPhaseOut",
                                  "RegionCrossingInterpolationTime", "CameraPositionSmoothing",
                                  "ALInterpolationCadenceAware", "ALInterpolationCadenceFactor",
                                  "ALInterpolationMaxFrameStep", "SKMotionRecordSeconds", "SKMotionRecordRadius",
                                  "SKMotionRecordMaxObjects", "SKMotionRecoverClampedVelocity",
                                  "SKCameraFollowEnabled", "SKCameraFollowSmoothing" })
        {
            writeSetting(out, name, first);
        }
        out << "}";

        out << ",\"tuning\":{\"phase_out_time\":" << jsonReal(tuning.mPhaseOutTime) << ",\"max_time\":" << jsonReal(tuning.mMaxTime)
            << ",\"cadence_factor\":" << jsonReal(tuning.mCadenceFactor) << ",\"cadence_cap\":" << jsonReal(tuning.mCadenceCap)
            << ",\"max_frame_step\":" << jsonReal(tuning.mMaxFrameStep)
            << ",\"cadence_aware\":" << (tuning.mCadenceAware ? "true" : "false") << "}";

        out << ",\"regions\":[";
        first = true;
        for (const LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            const LLVector3d& origin = regionp->getOriginGlobal();
            out << (first ? "" : ",") << "{\"handle\":" << regionp->getHandle() << ",\"origin\":[" << jsonReal(origin.mdV[0])
                << "," << jsonReal(origin.mdV[1]) << "," << jsonReal(origin.mdV[2]) << "],\"name\":" << jsonString(regionp->getName()) << "}";
            first = false;
        }
        out << "]";

        out << ",\"records\":{";
        writeLayout(out, "FRAME", SKMotionRecorder::REC_FRAME, sizeof(FrameRec), sFrameFields);
        out << ",";
        writeLayout(out, "CAMERA", SKMotionRecorder::REC_CAMERA, sizeof(CameraRec), sCameraFields);
        out << ",";
        writeLayout(out, "UPDATE", SKMotionRecorder::REC_UPDATE, sizeof(UpdateRec), sUpdateFields);
        out << ",";
        writeLayout(out, "OBJECT", SKMotionRecorder::REC_OBJECT, sizeof(ObjectRec), sObjectFields);
        out << ",";
        writeLayout(out, "INFO", SKMotionRecorder::REC_INFO, sizeof(InfoRec), sInfoFields);
        out << "}}\n";
        return out.str();
    }

    LLViewerObject* sitRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            return nullptr;
        }
        LLViewerObject* parentp = static_cast<LLViewerObject*>(gAgentAvatarp->getParent());
        return parentp ? parentp->getRootEdit() : nullptr;
    }

    void fillObject(ObjectRec& rec, const LLViewerObject* objectp, bool is_static)
    {
        rec.frame = sFrame;
        rec.local_id = objectp->getLocalID();
        rec.region_handle = regionHandle(objectp);
        copy3(rec.pos, objectp->getPositionRegion());
        copy3(rec.vel, objectp->getVelocity());
        copy3(rec.acc, objectp->getAcceleration());
        copy4(rec.rot, objectp->getRotation());
        copy3(rec.omega, objectp->getAngularVelocity());
        rec.is_static = is_static ? 1 : 0;
        rec.is_avatar = objectp->isAvatar() ? 1 : 0;
    }
}

bool SKMotionRecorder::sRecording = false;
F32  SKMotionRecorder::sPhaseOut = 1.f;
bool SKMotionRecorder::sEdgeClipped = false;
F32  SKMotionRecorder::sPingDelay = 0.f;
U32  SKMotionRecorder::sRebuilds = 0;

U64 SKMotionRecorder::now()
{
    return totalTime();
}

void SKMotionRecorder::addStage(EStage stage, U64 start_us)
{
    if (sRecording)
    {
        sStageMs[stage] += (F32)((U64)totalTime() - start_us) * 1e-3f;
    }
}

void SKMotionRecorder::start()
{
    if (sRecording)
    {
        return;
    }

    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", std::gmtime(&now));
    const U32 millis = (U32)((totalTime() / 1000) % 1000);
    const std::string utc = llformat("%s.%03uZ", stamp, millis);
    sPath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, llformat("skmotion-%s%03u.skm", stamp, millis));

    sFile.open(sPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!sFile.is_open())
    {
        LL_WARNS("MotionRecorder") << "Could not open " << sPath << LL_ENDL;
        return;
    }

    sDuration = gSavedSettings.getF32("SKMotionRecordSeconds");
    sRadius = gSavedSettings.getF32("SKMotionRecordRadius");
    sMaxObjects = gSavedSettings.getU32("SKMotionRecordMaxObjects");

    const std::string header = buildHeader(utc, LLViewerObject::sPredictionTuning);
    sFile.write(header.data(), header.size());

    sBuffer.clear();
    sCandidates.clear();
    sSeen.clear();
    sFrame = 0;
    sFrameBytes = 0;
    sUpdates = 0;
    sNetworkSecs = 0.f;
    std::fill(std::begin(sStageMs), std::end(sStageMs), 0.f);
    std::fill(std::begin(sPrevStageMs), std::end(sPrevStageMs), 0.f);
    sRebuilds = 0;
    sPrevRebuilds = 0;
    sPhaseOut = 1.f;
    sEdgeClipped = false;
    sStartSecs = LLFrameTimer::getElapsedSeconds();
    sRecording = true;

    LL_INFOS("MotionRecorder") << "Recording object motion to " << sPath << LL_ENDL;
}

void SKMotionRecorder::stop()
{
    if (!sRecording)
    {
        return;
    }
    sRecording = false;

    // A partial frame has no FRAME record to anchor it, so it is dropped rather than written.
    sBuffer.clear();
    sCandidates.clear();
    sSeen.clear();
    sFile.close();

    LL_INFOS("MotionRecorder") << "Recorded " << sFrame << " frames to " << sPath << LL_ENDL;
    LLSD args;
    args["MESSAGE"] = llformat("Object motion recording (%u frames) saved to %s", sFrame, sPath.c_str());
    LLNotificationsUtil::add("SystemMessage", args);
}

void SKMotionRecorder::toggle()
{
    if (sRecording)
    {
        stop();
    }
    else
    {
        start();
    }
}

void SKMotionRecorder::onFrameBegin()
{
    if (sDuration > 0.f && LLFrameTimer::getElapsedSeconds() - sStartSecs >= sDuration)
    {
        stop();
        return;
    }

    ++sFrame;

    std::copy(std::begin(sStageMs), std::end(sStageMs), std::begin(sPrevStageMs));
    std::fill(std::begin(sStageMs), std::end(sStageMs), 0.f);
    sPrevRebuilds = sRebuilds;
    sRebuilds = 0;

    LLViewerObject* focusp = gAgentCamera.getFocusObject();
    sFocusId = focusp ? focusp->getID() : LLUUID::null;
    LLViewerObject* sitp = sitRoot();
    sSitRootId = sitp ? sitp->getID() : LLUUID::null;
}

void SKMotionRecorder::onNetworkBegin()
{
    sNetworkStart = totalTime();
}

void SKMotionRecorder::onNetworkEnd()
{
    sNetworkSecs += (F32)((U64)totalTime() - sNetworkStart) * 1e-6f;
}

void SKMotionRecorder::onUpdateBegin(const LLViewerObject* objectp, const LLMessageSystem* mesgsys, U8 update_type)
{
    sUpdate = {};
    sUpdate.frame = sFrame;
    sUpdate.msg_secs = LLMessageSystem::getMessageTimeSeconds().value();
    sUpdate.total_us = totalTime();
    sUpdate.local_id = objectp->getLocalID();
    sUpdate.pre_region_handle = regionHandle(objectp);
    sUpdate.update_type = update_type;
    sUpdate.packet_id = mesgsys ? const_cast<LLMessageSystem*>(mesgsys)->getCurrentRecvPacketID() : 0;
    copy3(sUpdate.pre_pos, objectp->getPosition());
    copy3(sUpdate.pre_vel, objectp->getVelocity());
    copy4(sUpdate.pre_rot, objectp->getRotation());
    sPingDelay = 0.f;
}

void SKMotionRecorder::onUpdateDecoded(const LLVector3& pos, const LLVector3& vel, const LLVector3& acc,
                                       const LLQuaternion& rot, const LLVector3& omega, F32 time_dilation)
{
    copy3(sUpdate.raw_pos, pos);
    copy3(sUpdate.raw_vel, vel);
    copy3(sUpdate.used_vel, vel);
    copy3(sUpdate.raw_acc, acc);
    copy4(sUpdate.raw_rot, rot);
    copy3(sUpdate.raw_omega, omega);
    sUpdate.time_dilation = time_dilation;
}

void SKMotionRecorder::onUpdateRecovered(const LLVector3& vel, U8 method)
{
    copy3(sUpdate.used_vel, vel);
    sUpdate.recover = method;
}

void SKMotionRecorder::onUpdateEnd(const LLViewerObject* objectp, U32 crc, U32 parent_id, bool stale, U8 drop)
{
    sUpdate.region_handle = regionHandle(objectp);
    sUpdate.stale = stale ? 1 : 0;
    sUpdate.drop = drop;
    sUpdate.is_static = objectp->mStatic ? 1 : 0;
    sUpdate.ping_delay = sPingDelay;
    copy3(sUpdate.applied_pos, objectp->getPosition());
    copy3(sUpdate.final_vel, objectp->getVelocity());
    sUpdate.crc = crc;
    sUpdate.parent_id = parent_id;
    noteSeen(objectp);
    append(REC_UPDATE, sUpdate);
    ++sUpdates;
}

void SKMotionRecorder::onObjectIdle(const LLViewerObject* objectp, F32 dt, bool stepped)
{
    const F32 phase_out = sPhaseOut;
    const bool clipped = sEdgeClipped;
    sPhaseOut = 1.f;
    sEdgeClipped = false;

    const LLUUID& id = objectp->getID();
    const bool forced = (id == sFocusId || id == sSitRootId) && id.notNull();
    if (!stepped && !objectp->isAvatar() && !forced)
    {
        return;
    }

    const F32 dist = dist_vec(objectp->getPositionAgent(), LLViewerCamera::getInstance()->getOrigin());
    if (!forced && sRadius > 0.f && dist > sRadius)
    {
        return;
    }

    Candidate candidate = {};
    fillObject(candidate.rec, objectp, objectp->mStatic);
    candidate.rec.dt = stepped ? ALMotionPredictor::clampFrameStep(dt, LLViewerObject::sPredictionTuning) : dt;
    candidate.rec.phase_out = phase_out;
    candidate.rec.edge_clipped = clipped ? 1 : 0;
    candidate.rec.stepped = stepped ? 1 : 0;
    candidate.rec.cam_dist = dist;
    candidate.id = id;
    candidate.forced = forced;
    sCandidates.push_back(candidate);
}

void SKMotionRecorder::onFrameEnd()
{
    // Tracked objects that did not come through idleUpdate this frame (a static focus object, say).
    for (const LLUUID& id : { sFocusId, sSitRootId })
    {
        if (id.isNull()
            || std::any_of(sCandidates.begin(), sCandidates.end(), [&](const Candidate& c) { return c.id == id; }))
        {
            continue;
        }
        if (LLViewerObject* objectp = gObjectList.findObject(id))
        {
            Candidate candidate = {};
            fillObject(candidate.rec, objectp, objectp->mStatic);
            candidate.rec.phase_out = 1.f;
            candidate.rec.cam_dist = dist_vec(objectp->getPositionAgent(), LLViewerCamera::getInstance()->getOrigin());
            candidate.id = id;
            candidate.forced = true;
            sCandidates.push_back(candidate);
        }
    }

    // Nearest first, with the forced objects ahead of everything.
    std::sort(sCandidates.begin(), sCandidates.end(), [](const Candidate& a, const Candidate& b)
    {
        return a.forced != b.forced ? a.forced : a.rec.cam_dist < b.rec.cam_dist;
    });
    U32 dropped = 0;
    U32 tracked = 0;
    if (sMaxObjects > 0 && sCandidates.size() > sMaxObjects)
    {
        dropped = (U32)sCandidates.size() - sMaxObjects;
        sCandidates.resize(sMaxObjects);
    }

    // Render positions are read here, after gPipeline.updateMove and the camera's own damped move.
    for (Candidate& candidate : sCandidates)
    {
        LLViewerObject* objectp = gObjectList.findObject(candidate.id);
        if (!objectp)
        {
            continue;
        }
        if (objectp->mDrawable.notNull())
        {
            copy3(candidate.rec.render_pos, objectp->mDrawable->getPositionAgent());
        }
        noteSeen(objectp);
        append(REC_OBJECT, candidate.rec);
        ++tracked;
    }

    CameraRec cam = {};
    cam.frame = sFrame;
    copy3d(cam.cam_pos, gAgentCamera.getCameraPositionGlobal());
    copy4(cam.cam_rot, LLViewerCamera::getInstance()->getQuaternion());
    copy3d(cam.focus_pos, gAgentCamera.getFocusGlobal());
    copy3d(cam.focus_raw, gAgentCamera.getSKFollowRawFocusGlobal());
    cam.focus_on_avatar = gAgentCamera.getFocusOnAvatar() ? 1 : 0;
    cam.camera_mode = (U8)gAgentCamera.getCameraMode();
    if (LLViewerObject* focusp = gAgentCamera.getFocusObject())
    {
        cam.focus_local_id = focusp->getLocalID();
    }
    copy3(cam.focus_offset, gAgentCamera.getFocusObjectOffset());
    if (isAgentAvatarValid())
    {
        copy3(cam.avatar_render_pos, gAgentAvatarp->getRenderPosition());
    }
    if (LLViewerObject* sitp = sitRoot())
    {
        cam.sit_root_local_id = sitp->getLocalID();
    }
    append(REC_CAMERA, cam);

    FrameRec frame = {};
    frame.frame = sFrame;
    frame.frame_secs = LLFrameTimer::getElapsedSeconds();
    frame.total_us = totalTime();
    frame.msg_secs = LLMessageSystem::getMessageTimeSeconds().value();
    frame.frame_interval = gFrameIntervalSeconds.value();
    frame.frame_dt_clamped = gFrameDTClamped;
    frame.updates = sUpdates;
    frame.network_secs = sNetworkSecs;
    frame.tracked = tracked;
    frame.dropped = dropped;
    frame.objects_ms = sPrevStageMs[STAGE_OBJECTS];
    frame.move_ms = sPrevStageMs[STAGE_MOVE];
    frame.camera_ms = sPrevStageMs[STAGE_CAMERA];
    frame.idle_ms = sPrevStageMs[STAGE_IDLE];
    frame.geom_ms = sPrevStageMs[STAGE_GEOM];
    frame.render_ms = llmax(0.f, sPrevStageMs[STAGE_DISPLAY] - sPrevStageMs[STAGE_GEOM] - sPrevStageMs[STAGE_SWAP]);
    frame.swap_ms = sPrevStageMs[STAGE_SWAP];
    frame.flush_ms = sPrevStageMs[STAGE_FLUSH];
    frame.rebuilds = sPrevRebuilds;
    const U64 flush_start = totalTime();
    if (LLViewerRegion* regionp = gAgent.getRegion())
    {
        frame.region_handle = regionp->getHandle();
        frame.time_dilation = regionp->getTimeDilation();
        if (LLCircuitData* cdp = gMessageSystem->mCircuitInfo.findCircuit(regionp->getHost()))
        {
            frame.ping_ms = (F32)cdp->getPingDelay().value();
            frame.ping_avg_ms = cdp->getPingDelayAveraged().value();
            frame.last_packet_in = cdp->getLastPacketInTime().value();
        }
    }

    const U8 tag = REC_FRAME;
    sFile.write(reinterpret_cast<const char*>(&tag), 1);
    sFile.write(reinterpret_cast<const char*>(&frame), sizeof(frame));
    if (!sBuffer.empty())
    {
        sFile.write(reinterpret_cast<const char*>(sBuffer.data()), sBuffer.size());
    }
    sBuffer.clear();
    sCandidates.clear();
    sUpdates = 0;
    sNetworkSecs = 0.f;

    sFile.flush();
    stageEnd(STAGE_FLUSH, flush_start);
    if (!sFile.good())
    {
        LL_WARNS("MotionRecorder") << "Write to " << sPath << " failed, stopping" << LL_ENDL;
        stop();
    }
}
