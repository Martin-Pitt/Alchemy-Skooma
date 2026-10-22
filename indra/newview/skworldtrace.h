/**
 * @file skworldtrace.h
 * @brief A trace of what the world layers do, handed to whoever records it.
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

#ifndef SK_WORLDTRACE_H
#define SK_WORLDTRACE_H

#include "stdtypes.h"

#include <functional>
#include <string>

class LLUUID;
class LLVector3d;

/// Main thread only. The census, air and rooms write lines here; a recorder above them registers a sink and gets
/// each line as it is finished. With no sink, tracing() is false, and every call site guarded by it costs one load.
///
/// A line is the body of one JSON object: "t" its type, then the fields in the order given, positions global metres,
/// meshes the first eight hex digits of their id. A sink adds what it keeps of its own (frame, time) and the braces.
class SKWorldTrace
{
public:
    typedef std::function<void(const std::string& line)> Sink;

    /// A sink by name; a second under the same name replaces the first.
    static void addSink(const std::string& name, const Sink& sink);
    static void removeSink(const std::string& name);
    static bool tracing() { return sTracing; }

    /// One line under construction, handed to every sink when it goes out of scope.
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
    static bool sTracing;
};

#endif // SK_WORLDTRACE_H
