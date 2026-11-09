/**
 * @file skterrainmanifest.cpp
 * @brief The cache's index of a grid's region files, read without opening any of them.
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

#include "skterrainmanifest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

using namespace SKTerrain;

namespace
{
    constexpr char HEADER[] = "SKTerrainIndex";

    std::vector<std::string> splitTabs(const std::string& line)
    {
        std::vector<std::string> out;
        size_t                   start = 0;
        while (true)
        {
            const size_t tab = line.find('\t', start);
            out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos)
                return out;
            start = tab + 1;
        }
    }

    template <typename T>
    bool number(const std::string& text, T& out)
    {
        std::istringstream in(text);
        in >> out;
        return !in.fail() && in.eof();
    }

    // strtof reads back exactly what %.9g wrote; a stream's float parse is inlined into this file and
    // compiled with its fast-math flags, which libc++ rounds off by an ULP or two.
    bool number(const std::string& text, F32& out)
    {
        if (text.empty())
            return false;
        char* end = nullptr;
        out       = strtof(text.c_str(), &end);
        return end && *end == 0;
    }

    bool hex(const std::string& text, U64& out)
    {
        if (text.empty() || text.size() > 16)
            return false;
        char* end = nullptr;
        out       = strtoull(text.c_str(), &end, 16);
        return end && *end == 0;
    }

    std::string clean(const std::string& source)
    {
        std::string out = source;
        for (char& c : out)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        return out;
    }
} // namespace

SKTerrainManifestRow SKTerrainManifestRow::fromHeader(const SKTerrainCell& header, U64 file_size, U32 checksum)
{
    SKTerrainManifestRow row;
    row.mHandle      = header.mHandle;
    row.mWater       = header.mWater;
    row.mCaptureDate = header.mCaptureDate;
    for (U32 l = 0; l < LAYER_COUNT; ++l)
    {
        const SKTerrainLayerInfo& info = header.mInfo[l];
        if (info.mValid)
            row.mLayers |= 1u << l;
        row.mRevision[l] = info.mRevision;
        row.mMin[l]      = info.mMin;
        row.mMax[l]      = info.mMax;
    }
    row.mFileSize = file_size;
    row.mChecksum = checksum;
    return row;
}

std::string SKTerrainManifest::format() const
{
    std::string out = std::string(HEADER) + "\t" + std::to_string(FORMAT_VERSION) + "\t" + clean(mGrid) + "\n";
    char        buf[512];
    for (const SKTerrainManifestRow& r : mRows)
    {
        // U64 is unsigned long long on every platform, which PRIu64 is not on Linux.
        snprintf(buf, sizeof(buf), "%llx\t%u\t%u\t%u\t%u\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%llu\t%llu\t",
                 static_cast<unsigned long long>(r.mHandle), r.mCells, r.mLayers, r.mRevision[0], r.mRevision[1], r.mMin[0],
                 r.mMax[0], r.mMin[1], r.mMax[1], r.mWater, static_cast<unsigned long long>(r.mCaptureDate),
                 static_cast<unsigned long long>(r.mLastSeen));
        out += buf;
        out += clean(r.mSource);
        snprintf(buf, sizeof(buf), "\t%llu\t%08x\n", static_cast<unsigned long long>(r.mFileSize), r.mChecksum);
        out += buf;
    }
    return out;
}

std::optional<SKTerrainManifest> SKTerrainManifest::parse(const std::string& text, const std::string& grid)
{
    std::istringstream in(text);
    std::string        line;
    if (!std::getline(in, line))
        return std::nullopt;
    const std::vector<std::string> head = splitTabs(line);
    U32                            version = 0;
    if (head.size() != 3 || head[0] != HEADER || !number(head[1], version) || version != FORMAT_VERSION || head[2] != clean(grid))
        return std::nullopt;

    SKTerrainManifest manifest;
    manifest.mGrid = grid;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const std::vector<std::string> f = splitTabs(line);
        SKTerrainManifestRow           r;
        U64                            checksum = 0;
        if (f.size() != 15 || !hex(f[0], r.mHandle) || !number(f[1], r.mCells) || !number(f[2], r.mLayers) ||
            !number(f[3], r.mRevision[0]) || !number(f[4], r.mRevision[1]) || !number(f[5], r.mMin[0]) ||
            !number(f[6], r.mMax[0]) || !number(f[7], r.mMin[1]) || !number(f[8], r.mMax[1]) || !number(f[9], r.mWater) ||
            !number(f[10], r.mCaptureDate) || !number(f[11], r.mLastSeen) || !number(f[13], r.mFileSize) ||
            !hex(f[14], checksum) || checksum > 0xffffffffull || (r.mLayers >> LAYER_COUNT) != 0)
            return std::nullopt;
        r.mSource   = f[12];
        r.mChecksum = U32(checksum);
        manifest.mRows.push_back(std::move(r));
    }
    return manifest;
}

std::string SKTerrainManifest::fileName(U64 handle)
{
    return std::to_string(gridX(handle)) + "_" + std::to_string(gridY(handle)) + ".skt";
}

std::optional<U64> SKTerrainManifest::handleFromFileName(const std::string& name)
{
    unsigned gx = 0, gy = 0;
    char     tail[8] = {};
    if (sscanf(name.c_str(), "%u_%u.%4s", &gx, &gy, tail) != 3 || strcmp(tail, "skt") != 0 || gx >= (1u << 24) || gy >= (1u << 24) ||
        name != fileName(handleFromGrid(gx, gy)))
        return std::nullopt;
    return handleFromGrid(gx, gy);
}

U32 SKTerrainManifest::checksumOf(const U8* data, size_t size)
{
    if (size < 4)
        return 0;
    const U8* p = data + size - 4;
    return U32(p[0]) | U32(p[1]) << 8 | U32(p[2]) << 16 | U32(p[3]) << 24;
}
