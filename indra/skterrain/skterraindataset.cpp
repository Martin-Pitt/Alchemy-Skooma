/**
 * @file skterraindataset.cpp
 * @brief A dataset's terrain/ or surface/ folder: its index rows and the grey16 cells they name.
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

#include "linden_common.h"

#include "skterraindataset.h"

#include "skterrainpng.h"

#include "fsyspath.h"
#include "llsd.h"
#include "llsdutil.h"
#include "llsdjson.h"

#include <cmath>
#include <fstream>
#include <iterator>

using namespace SKTerrain;

namespace
{
    std::optional<std::vector<U8>> readFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        return std::vector<U8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool writeFile(const std::filesystem::path& path, const std::vector<U8>& data)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return bool(out);
    }

    bool pair(const LLSD& value, S32& a, S32& b)
    {
        if (!value.isArray() || value.size() != 2 || !value[0].isInteger() || !value[1].isInteger())
            return false;
        a = value[0].asInteger();
        b = value[1].asInteger();
        return true;
    }
} // namespace

const char* SKTerrainDataset::layerFolder(ELayer layer)
{
    return layer == LAYER_SURFACE ? "surface" : "terrain";
}

std::vector<SKTerrainDatasetRow> SKTerrainDataset::parseIndex(const std::string& json)
{
    std::vector<SKTerrainDatasetRow> rows;
    LLSD                             index;
    if (!LlsdFromJsonString(json, index) || !index.isArray())
        return rows;

    for (const LLSD& entry : llsd::inArray(index))
    {
        S32 gx = 0, gy = 0, w = 0, h = 0;
        if (!entry.isMap() || !pair(entry["grid"], gx, gy) || !pair(entry["size"], w, h) || gx < 0 || gy < 0 ||
            w != S32(CELL_SIZE) || h != S32(CELL_SIZE) || entry["step_m"].asReal() != 1.0)
            continue;

        SKTerrainDatasetRow row;
        row.mGridX        = U32(gx);
        row.mGridY        = U32(gy);
        row.mValidSamples = U32(std::max(0, entry["valid_samples"].asInteger()));
        if (entry["height"].isString() && entry["offset_cm"].isInteger())
        {
            row.mHeight   = entry["height"].asString();
            row.mOffsetCm = entry["offset_cm"].asInteger();
        }
        if (entry["water_m"].isReal() || entry["water_m"].isInteger())
        {
            const F64 water = entry["water_m"].asReal();
            if (std::isfinite(water))
                row.mWater = F32(water);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string SKTerrainDataset::formatIndex(const std::vector<SKTerrainDatasetRow>& rows)
{
    std::string out = "[";
    char        buf[512];
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const SKTerrainDatasetRow& row = rows[i];
        const std::string height = row.mHeight.empty() || !row.mOffsetCm ? "null" : "\"" + row.mHeight + "\"";
        const std::string offset = row.mHeight.empty() || !row.mOffsetCm ? "null" : std::to_string(*row.mOffsetCm);
        std::string       water  = "null";
        if (row.mWater)
        {
            snprintf(buf, sizeof(buf), "%.2f", *row.mWater);
            water = buf;
        }
        snprintf(buf, sizeof(buf),
                 "%s{\"grid\":[%u,%u],\"height\":%s,\"offset_cm\":%s,\"origin_m\":[%llu,%llu],\"size\":[256,256],"
                 "\"step_m\":1,\"valid_samples\":%u,\"water_m\":%s}",
                 i ? "," : "", row.mGridX, row.mGridY, height.c_str(), offset.c_str(),
                 (unsigned long long)row.mGridX * CELL_SIZE, (unsigned long long)row.mGridY * CELL_SIZE,
                 row.mValidSamples, water.c_str());
        out += buf;
    }
    out += "]";
    return out;
}

std::optional<SKTerrainLayerSamples> SKTerrainDataset::readCell(const std::filesystem::path& root, const SKTerrainDatasetRow& row)
{
    if (row.mHeight.empty() || !row.mOffsetCm)
        return std::nullopt;
    const std::filesystem::path relative = fsyspath(row.mHeight);
    if (relative.is_absolute() || (!relative.empty() && *relative.lexically_normal().begin() == ".."))
        return std::nullopt;
    const std::optional<std::vector<U8>> bytes = readFile(root / relative);
    if (!bytes)
        return std::nullopt;
    const std::optional<SKTerrainPng::Grey16> image = SKTerrainPng::decode(bytes->data(), bytes->size());
    if (!image)
        return std::nullopt;
    return SKTerrainPng::toSamples(*image, *row.mOffsetCm);
}

bool SKTerrainDataset::writeCell(const std::filesystem::path& root, ELayer layer, U64 handle,
                                 const SKTerrainLayerSamples& samples, std::optional<F32> water, SKTerrainDatasetRow& row)
{
    row               = SKTerrainDatasetRow();
    row.mGridX        = gridX(handle);
    row.mGridY        = gridY(handle);
    row.mWater        = water;
    row.mValidSamples = samples.empty() ? 0 : samples.validCount();
    if (!row.mValidSamples)
        return true;

    S32                                       offset = 0;
    const std::optional<SKTerrainPng::Grey16> image  = SKTerrainPng::fromSamples(samples, offset);
    if (!image)
        return false;
    const std::vector<U8> png = SKTerrainPng::encode(*image);
    if (png.empty())
        return false;

    const std::string name = std::string(layerFolder(layer)) + "/" + std::to_string(row.mGridX) + "_" + std::to_string(row.mGridY) + ".png";
    std::error_code   ec;
    std::filesystem::create_directories(root / layerFolder(layer), ec);
    if (!writeFile(root / fsyspath(name), png))
        return false;
    row.mHeight   = name;
    row.mOffsetCm = offset;
    return true;
}
