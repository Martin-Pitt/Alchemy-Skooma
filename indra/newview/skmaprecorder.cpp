/**
 * @file skmaprecorder.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skmaprecorder.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "skworldtrace.h"

#include <cstdio>
#include <memory>

bool SKMapRecorder::sRecording = false;

namespace
{
    std::unique_ptr<llofstream> sFile;
    std::string sPath;
    std::string sBuffer;
    F64 sStarted = 0.0;
    U64 sWritten = 0;
    U64 sLimit = 0;

    void flush()
    {
        if (sFile && !sBuffer.empty())
        {
            sFile->write(sBuffer.data(), sBuffer.size());
            sWritten += sBuffer.size();
        }
        sBuffer.clear();
    }

    std::string filePath()
    {
        static LLCachedControl<std::string> dir(gSavedSettings, "SKMapRecordPath", "");
        std::string stamp = LLDate::now().asString();
        for (char& c : stamp)
        {
            c = (c == ':') ? '-' : c;
        }
        const std::string name = "skmaprec-" + stamp + ".jsonl";
        const std::string folder = dir();
        if (!folder.empty() && LLFile::isdir(folder))
        {
            return folder + gDirUtilp->getDirDelimiter() + name;
        }
        return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
    }

    void appendReal(std::string& out, F64 v, const char* fmt)
    {
        char tmp[48];
        const int n = snprintf(tmp, sizeof(tmp), fmt, v);
        out.append(tmp, n > 0 ? llmin<size_t>(n, sizeof(tmp) - 1) : 0);
    }

    const char* TRACE_SINK = "skmaprecorder";

    /// What the census, air and rooms trace, framed as the recorder's own lines are.
    void traceLine(const std::string& body)
    {
        sBuffer += "{\"f\":";
        sBuffer += std::to_string(LLFrameTimer::getFrameCount());
        sBuffer += ",\"s\":";
        appendReal(sBuffer, LLTimer::getTotalSeconds() - sStarted, "%.4f");
        sBuffer += ',';
        sBuffer += body;
        sBuffer += "}\n";
    }
}

void SKMapRecorder::start()
{
    if (sRecording)
    {
        return;
    }
    static LLCachedControl<U32> max_mb(gSavedSettings, "SKMapRecordMaxMB", 256);
    sPath = filePath();
    sFile = std::make_unique<llofstream>(sPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!sFile->is_open())
    {
        LL_WARNS("SKMapRecorder") << "cannot open " << sPath << LL_ENDL;
        sFile.reset();
        gSavedSettings.setBOOL("SKMapRecord", false);
        return;
    }
    sStarted = LLTimer::getTotalSeconds();
    sWritten = 0;
    sLimit = static_cast<U64>(llmax<U32>(max_mb, 1)) << 20;
    sBuffer.clear();
    sRecording = true;
    SKWorldTrace::addSink(TRACE_SINK, &traceLine);
    Line("header").num("version", 1).str("date", LLDate::now().asString()).num("limit", static_cast<S64>(sLimit));
    LL_INFOS("SKMapRecorder") << "recording to " << sPath << LL_ENDL;
}

void SKMapRecorder::stop()
{
    if (!sRecording)
    {
        return;
    }
    Line("end");
    SKWorldTrace::removeSink(TRACE_SINK);
    sRecording = false;
    flush();
    sFile.reset();
    LL_INFOS("SKMapRecorder") << "recorded " << sWritten << " bytes to " << sPath << LL_ENDL;
    LLSD args;
    args["MESSAGE"] = "Map recording saved: " + sPath;
    LLNotificationsUtil::add("SystemMessageTip", args);
}

void SKMapRecorder::onFrame()
{
    static LLCachedControl<bool> wanted(gSavedSettings, "SKMapRecord", false);
    if (wanted != sRecording)
    {
        wanted ? start() : stop();
    }
    if (!sRecording)
    {
        return;
    }
    flush();
    if (sWritten >= sLimit)
    {
        Line("cap").num("bytes", static_cast<S64>(sWritten));
        gSavedSettings.setBOOL("SKMapRecord", false);
        stop();
        return;
    }
    Line line("frame");
    line.xyz("agent", gAgent.getPositionGlobal()).xyz("cam", gAgentCamera.getCameraPositionGlobal());
    if (const LLViewerRegion* region = gAgent.getRegion())
    {
        line.xy("region", region->getOriginGlobal());
    }
}

SKMapRecorder::Line::Line(const char* type)
{
    mBuf.reserve(160);
    mBuf += "{\"f\":";
    mBuf += std::to_string(LLFrameTimer::getFrameCount());
    mBuf += ",\"s\":";
    appendReal(mBuf, LLTimer::getTotalSeconds() - sStarted, "%.4f");
    mBuf += ",\"t\":\"";
    mBuf += type;
    mBuf += '"';
}

SKMapRecorder::Line::~Line()
{
    if (!sRecording)
    {
        return;
    }
    mBuf += "}\n";
    sBuffer += mBuf;
}

void SKMapRecorder::Line::key(const char* key)
{
    mBuf += ",\"";
    mBuf += key;
    mBuf += "\":";
}

SKMapRecorder::Line& SKMapRecorder::Line::str(const char* k, const char* value)
{
    key(k);
    mBuf += '"';
    for (const char* c = value; *c; ++c)
    {
        if (*c == '"' || *c == '\\')
        {
            mBuf += '\\';
        }
        mBuf += (static_cast<unsigned char>(*c) < 0x20) ? ' ' : *c;
    }
    mBuf += '"';
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::num(const char* k, S64 value)
{
    key(k);
    mBuf += std::to_string(value);
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::real(const char* k, F64 value)
{
    key(k);
    appendReal(mBuf, value, "%.3f");
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::flag(const char* k, bool value)
{
    key(k);
    mBuf += value ? "true" : "false";
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::hex(const char* k, U64 value)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%016llx", static_cast<unsigned long long>(value));
    return str(k, tmp);
}

SKMapRecorder::Line& SKMapRecorder::Line::mesh(const LLUUID& id)
{
    return mesh("mesh", id);
}

SKMapRecorder::Line& SKMapRecorder::Line::mesh(const char* k, const LLUUID& id)
{
    return str(k, id.asString().substr(0, 8));
}

SKMapRecorder::Line& SKMapRecorder::Line::tile(S32 x, S32 y)
{
    key("tile");
    mBuf += '[' + std::to_string(x) + ',' + std::to_string(y) + ']';
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::xy(const char* k, const LLVector3d& pos)
{
    key(k);
    mBuf += '[';
    appendReal(mBuf, pos.mdV[VX], "%.2f");
    mBuf += ',';
    appendReal(mBuf, pos.mdV[VY], "%.2f");
    mBuf += ']';
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::xyz(const char* k, const LLVector3d& pos)
{
    key(k);
    mBuf += '[';
    appendReal(mBuf, pos.mdV[VX], "%.2f");
    mBuf += ',';
    appendReal(mBuf, pos.mdV[VY], "%.2f");
    mBuf += ',';
    appendReal(mBuf, pos.mdV[VZ], "%.2f");
    mBuf += ']';
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::box(const LLVector3d& min, const LLVector3d& max)
{
    xy("c", (min + max) * 0.5);
    key("w");
    mBuf += '[';
    appendReal(mBuf, max.mdV[VX] - min.mdV[VX], "%.1f");
    mBuf += ',';
    appendReal(mBuf, max.mdV[VY] - min.mdV[VY], "%.1f");
    mBuf += ']';
    return *this;
}

SKMapRecorder::Line& SKMapRecorder::Line::raw(const char* k, const std::string& json)
{
    key(k);
    mBuf += json;
    return *this;
}
