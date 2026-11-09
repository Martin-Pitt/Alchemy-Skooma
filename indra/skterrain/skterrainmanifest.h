/**
 * @file skterrainmanifest.h
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

#ifndef SK_SKTERRAINMANIFEST_H
#define SK_SKTERRAINMANIFEST_H

#include "skterraincell.h"

#include <optional>
#include <string>

// One region file as the index knows it.
struct SKTerrainManifestRow
{
    U64         mHandle   = 0;
    U32         mCells    = 1; // grid cells the handle's region covers
    U32         mLayers   = 0; // bit per SKTerrain::ELayer present
    std::array<U32, SKTerrain::LAYER_COUNT> mRevision{};
    std::array<F32, SKTerrain::LAYER_COUNT> mMin{};
    std::array<F32, SKTerrain::LAYER_COUNT> mMax{};
    F32         mWater       = 20.f;
    U64         mCaptureDate = 0;
    U64         mLastSeen    = 0;
    std::string mSource; // "live", "seed", or the producer that last put a layer
    U64         mFileSize = 0;
    U32         mChecksum = 0; // the file's own trailing CRC

    bool hasLayer(SKTerrain::ELayer layer) const { return (mLayers >> layer) & 1; }

    // The row a region file's header gives; the caller adds what only it knows.
    static SKTerrainManifestRow fromHeader(const SKTerrainCell& header, U64 file_size, U32 checksum);
};

struct SKTerrainManifest
{
    static constexpr U32 FORMAT_VERSION = 1;

    std::string                       mGrid;
    std::vector<SKTerrainManifestRow> mRows;

    std::string format() const;

    // Nothing for a manifest of another version or grid, or one with a damaged row: it is rebuilt.
    static std::optional<SKTerrainManifest> parse(const std::string& text, const std::string& grid);

    // The region file's name for a handle, "<gx>_<gy>.skt".
    static std::string fileName(U64 handle);
    static std::optional<U64> handleFromFileName(const std::string& name);

    // The file's trailing CRC, as the manifest records it.
    static U32 checksumOf(const U8* data, size_t size);
};

#endif // SK_SKTERRAINMANIFEST_H
