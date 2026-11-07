/**
 * @file skterraindataset.h
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

#ifndef SK_SKTERRAINDATASET_H
#define SK_SKTERRAINDATASET_H

#include "skterraincell.h"

#include <filesystem>
#include <optional>
#include <string>

// One row of <root>/<layer>/index.json. A cell with no known sample has no height file; its water
// may still be known, and a missing water is not sea level.
struct SKTerrainDatasetRow
{
    U32                mGridX = 0;
    U32                mGridY = 0;
    std::string        mHeight; // relative to the dataset root, empty when there is none
    std::optional<S32> mOffsetCm;
    std::optional<F32> mWater;
    U32                mValidSamples = 0;

    U64 handle() const { return SKTerrain::handleFromGrid(mGridX, mGridY); }
};

namespace SKTerrainDataset
{
    const char* layerFolder(SKTerrain::ELayer layer); // "terrain", "surface"

    // Rows that do not describe a 256 m cell at 1 m are left out.
    std::vector<SKTerrainDatasetRow> parseIndex(const std::string& json);
    std::string                      formatIndex(const std::vector<SKTerrainDatasetRow>& rows);

    // Reads the row's PNG under root; nothing when it has none or it does not decode.
    std::optional<SKTerrainLayerSamples> readCell(const std::filesystem::path& root, const SKTerrainDatasetRow& row);

    // Writes <root>/<layer>/<gx>_<gy>.png and fills the row; a layer with nothing known gets a row and no file.
    bool writeCell(const std::filesystem::path& root, SKTerrain::ELayer layer, U64 handle,
                   const SKTerrainLayerSamples& samples, std::optional<F32> water, SKTerrainDatasetRow& row);
} // namespace SKTerrainDataset

#endif // SK_SKTERRAINDATASET_H
