/**
 * @file skterraincell.h
 * @brief A region's cached ground: its layers on the 1 m lattice, their pyramids, and the file they are kept in.
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

#ifndef SK_SKTERRAINCELL_H
#define SK_SKTERRAINCELL_H

#include "stdtypes.h"

#include <array>
#include <string>
#include <vector>

// The ground of one 256 m region cell, as the dataset defines it: a 256 x 256 lattice at 1 m,
// row 0 south, heights in centimetres, each sample either known or missing.
namespace SKTerrain
{
    constexpr U32 CELL_SIZE    = 256;
    constexpr U32 CELL_SAMPLES = CELL_SIZE * CELL_SIZE;
    constexpr U32 LEVEL_COUNT  = 9; // 256, 128, ... 1

    // The surface layer is the top of whatever stands on the ground, on the same lattice and datum.
    enum ELayer : U32
    {
        LAYER_TERRAIN = 0,
        LAYER_SURFACE = 1,
        LAYER_COUNT
    };

    // Region handles are global metres packed x << 32 | y, as the viewer's are.
    inline U64 handleFromGrid(U32 gx, U32 gy) { return (U64(gx) * CELL_SIZE << 32) | U64(gy) * CELL_SIZE; }
    inline U32 gridX(U64 handle) { return U32(handle >> 32) / CELL_SIZE; }
    inline U32 gridY(U64 handle) { return U32(handle & 0xffffffffu) / CELL_SIZE; }
} // namespace SKTerrain

// One layer as stored: heights in whole centimetres, a missing sample marked apart.
struct SKTerrainLayerSamples
{
    std::vector<S32> mHeightCm; // CELL_SAMPLES, row-major from the south-west corner
    std::vector<U8>  mValid;    // CELL_SAMPLES, 1 where the height is known

    bool empty() const { return mHeightCm.empty(); }
    U32  validCount() const;
    void resize() { mHeightCm.assign(SKTerrain::CELL_SAMPLES, 0); mValid.assign(SKTerrain::CELL_SAMPLES, 0); }

    // Metres rounded to centimetres once; a non-finite value is missing.
    static SKTerrainLayerSamples fromMetres(const F32* heights, const U8* valid);
};

// Each level keeps every 2^k-th lattice point of the one below; a node's min and max are over the
// known samples of the base block it stands for. A level with no known sample under a node has
// min > max there.
struct SKTerrainPyramid
{
    struct Level
    {
        U32              mSize = 0;
        std::vector<F32> mHeight; // mSize^2, missing samples filled with the fill height
        std::vector<U8>  mValid;  // mSize^2
        std::vector<F32> mMin;    // mSize^2
        std::vector<F32> mMax;    // mSize^2
    };

    std::array<Level, SKTerrain::LEVEL_COUNT> mLevels; // [0] is 256^2
    F32 mMin = 0.f; // over known samples; mMin > mMax when there are none
    F32 mMax = -1.f;

    bool hasData() const { return mMin <= mMax; }

    // Missing samples are filled with fill_height, the cell's water.
    static SKTerrainPyramid build(const SKTerrainLayerSamples& samples, F32 fill_height);
};

struct SKTerrainLayerInfo
{
    U32 mRevision = 0;
    U64 mStamp    = 0; // the producer's time, seconds since the epoch
    U32 mValid    = 0;
    F32 mMin      = 0.f;
    F32 mMax      = -1.f;
};

// The region file: a header, each present layer's lattice, and a sidecar the cache does not read
// (the region's terrain look, serialised by whoever wrote it).
struct SKTerrainCell
{
    static constexpr U32 FORMAT_VERSION = 1;

    U64         mHandle      = 0;
    F32         mWater       = 20.f;
    U64         mCaptureDate = 0;
    std::string mSidecar;

    std::array<SKTerrainLayerInfo, SKTerrain::LAYER_COUNT>    mInfo;
    std::array<SKTerrainLayerSamples, SKTerrain::LAYER_COUNT> mLayers;

    bool hasLayer(SKTerrain::ELayer layer) const { return !mLayers[layer].empty(); }

    // Replaces one layer, bumping its revision and its min and max alone.
    void setLayer(SKTerrain::ELayer layer, SKTerrainLayerSamples samples, U64 stamp);

    std::vector<U8> encode() const;

    enum EDecode
    {
        DECODE_OK,
        DECODE_CORRUPT,
        DECODE_VERSION, // written by another format version; the file is to be dropped
    };
    EDecode decode(const U8* data, size_t size);

    // Reads only the header, for indexing without inflating the layers.
    static EDecode peek(const U8* data, size_t size, SKTerrainCell& header_only);
};

#endif // SK_SKTERRAINCELL_H
