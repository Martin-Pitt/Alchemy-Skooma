/**
 * @file skmotionrecorder.h
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

#ifndef SK_SKMOTIONRECORDER_H
#define SK_SKMOTIONRECORDER_H

#include "stdtypes.h"

class LLMessageSystem;
class LLQuaternion;
class LLVector3;
class LLViewerObject;

// Captures what the viewer receives and what it does with it, frame by frame, so interpolators can
// be replayed and compared offline. Each hook is guarded by an inline isRecording() at its call site.
//
// File: one JSON header line describing every record layout, then packed little-endian records,
// each preceded by a U8 type tag. Records are buffered per frame and written when the frame ends.
class SKMotionRecorder
{
public:
    enum ERecordType : U8
    {
        REC_FRAME  = 1,
        REC_CAMERA = 2,
        REC_UPDATE = 3,
        REC_OBJECT = 4,
        REC_INFO   = 5,
    };

    static bool isRecording() { return sRecording; }

    static void start();
    static void stop();
    static void toggle();

    // LLAppViewer::idle: after the frame timers, around idleNetwork, and once the camera has moved.
    static void onFrameBegin();
    static void onNetworkBegin();
    static void onNetworkEnd();
    static void onFrameEnd();

    // LLViewerObject::processUpdateMessage: object state before the message touches it, the
    // decoded motion before ping interpolation, and the result (or the stale drop).
    static void onUpdateBegin(const LLViewerObject* objectp, const LLMessageSystem* mesgsys, U8 update_type);
    static void onUpdateDecoded(const LLVector3& pos, const LLVector3& vel, const LLVector3& acc,
                                const LLQuaternion& rot, const LLVector3& omega, F32 time_dilation);
    static void onUpdateEnd(const LLViewerObject* objectp, const LLVector3& applied_pos, U32 crc, U32 parent_id, bool stale);

    // LLViewerObject::idleUpdate, after prediction has run (or not) for this object this frame.
    static void onObjectIdle(const LLViewerObject* objectp, F32 dt, bool stepped);

    // Written unconditionally by interpolateLinearMotion and read back by onObjectIdle.
    static F32  sPhaseOut;
    static bool sEdgeClipped;
    // Written unconditionally by the ping interpolation block of processUpdateMessage.
    static F32  sPingDelay;

private:
    static bool sRecording;
};

#endif // SK_SKMOTIONRECORDER_H
