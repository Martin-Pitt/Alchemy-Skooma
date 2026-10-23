/**
 * @file skworldtrace.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skworldtrace.h"

#include "lluuid.h"
#include "v3dmath.h"

#include <cstdio>
#include <map>

bool SKWorldTrace::sTracing = false;

namespace
{
    std::map<std::string, SKWorldTrace::Sink>& sinks()
    {
        static std::map<std::string, SKWorldTrace::Sink> all;
        return all;
    }

    void appendReal(std::string& out, F64 v, const char* fmt)
    {
        char tmp[48];
        const int n = snprintf(tmp, sizeof(tmp), fmt, v);
        out.append(tmp, n > 0 ? llmin<size_t>(n, sizeof(tmp) - 1) : 0);
    }
}

void SKWorldTrace::addSink(const std::string& name, const Sink& sink)
{
    sinks()[name] = sink;
    sTracing = true;
}

void SKWorldTrace::removeSink(const std::string& name)
{
    sinks().erase(name);
    sTracing = !sinks().empty();
}

SKWorldTrace::Line::Line(const char* type)
{
    mBuf.reserve(160);
    mBuf += "\"t\":\"";
    mBuf += type;
    mBuf += '"';
}

SKWorldTrace::Line::~Line()
{
    for (const auto& entry : sinks())
    {
        entry.second(mBuf);
    }
}

void SKWorldTrace::Line::key(const char* key)
{
    mBuf += ",\"";
    mBuf += key;
    mBuf += "\":";
}

SKWorldTrace::Line& SKWorldTrace::Line::str(const char* k, const char* value)
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

SKWorldTrace::Line& SKWorldTrace::Line::num(const char* k, S64 value)
{
    key(k);
    mBuf += std::to_string(value);
    return *this;
}

SKWorldTrace::Line& SKWorldTrace::Line::real(const char* k, F64 value)
{
    key(k);
    appendReal(mBuf, value, "%.3f");
    return *this;
}

SKWorldTrace::Line& SKWorldTrace::Line::flag(const char* k, bool value)
{
    key(k);
    mBuf += value ? "true" : "false";
    return *this;
}

SKWorldTrace::Line& SKWorldTrace::Line::hex(const char* k, U64 value)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%016llx", static_cast<unsigned long long>(value));
    return str(k, tmp);
}

SKWorldTrace::Line& SKWorldTrace::Line::mesh(const LLUUID& id)
{
    return mesh("mesh", id);
}

SKWorldTrace::Line& SKWorldTrace::Line::mesh(const char* k, const LLUUID& id)
{
    return str(k, id.asString().substr(0, 8));
}

SKWorldTrace::Line& SKWorldTrace::Line::tile(S32 x, S32 y)
{
    key("tile");
    mBuf += '[' + std::to_string(x) + ',' + std::to_string(y) + ']';
    return *this;
}

SKWorldTrace::Line& SKWorldTrace::Line::xy(const char* k, const LLVector3d& pos)
{
    key(k);
    mBuf += '[';
    appendReal(mBuf, pos.mdV[VX], "%.2f");
    mBuf += ',';
    appendReal(mBuf, pos.mdV[VY], "%.2f");
    mBuf += ']';
    return *this;
}

SKWorldTrace::Line& SKWorldTrace::Line::xyz(const char* k, const LLVector3d& pos)
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

SKWorldTrace::Line& SKWorldTrace::Line::box(const LLVector3d& min, const LLVector3d& max)
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

SKWorldTrace::Line& SKWorldTrace::Line::raw(const char* k, const std::string& json)
{
    key(k);
    mBuf += json;
    return *this;
}
