/**
 * @file skmaprecorder.h
 * @brief Records why the mini-map and the census and air under it update, frame by frame, to a JSON lines file.
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

#ifndef SK_MAPRECORDER_H
#define SK_MAPRECORDER_H

#include "stdtypes.h"

#include <string>

class LLUUID;
class LLVector3d;

/// Main thread only. Started and stopped by SKMapRecord; every hook is guarded by
/// isRecording() at its call site, so a stopped recorder costs one load per site.
///
/// File: one JSON object per line. Each carries the frame ("f"), the seconds since the
/// recording started ("s") and its type ("t"); positions are global metres, meshes the
/// first eight hex digits of their id. The file stops at SKMapRecordMaxMB.
class SKMapRecorder
{
public:
    static bool isRecording() { return sRecording; }

    /// Once a frame, before the census's update: follows the setting, writes the frame's
    /// positions and flushes what the last frame recorded. While recording, it takes every
    /// line the world layers trace (SKWorldTrace).
    static void onFrame();
    static void start();
    static void stop();

    /// One line under construction, written when it goes out of scope.
    class Line
    {
    public:
        explicit Line(const char* type);
        ~Line();
        Line(const Line&) = delete;
        Line& operator=(const Line&) = delete;

        Line& str(const char* key, const char* value);
        Line& str(const char* key, const std::string& value) { return str(key, value.c_str()); }
        Line& num(const char* key, S64 value);
        Line& real(const char* key, F64 value);
        Line& flag(const char* key, bool value);
        Line& hex(const char* key, U64 value);
        Line& mesh(const LLUUID& id);
        Line& mesh(const char* key, const LLUUID& id);
        Line& tile(S32 x, S32 y);
        Line& xy(const char* key, const LLVector3d& pos);
        Line& xyz(const char* key, const LLVector3d& pos);
        /// "c": the box's centre, "w": its width and depth.
        Line& box(const LLVector3d& min, const LLVector3d& max);
        /// A raw JSON value, already formatted.
        Line& raw(const char* key, const std::string& json);

    private:
        void key(const char* key);
        std::string mBuf;
    };

private:
    static bool sRecording;
};

#endif // SK_MAPRECORDER_H
