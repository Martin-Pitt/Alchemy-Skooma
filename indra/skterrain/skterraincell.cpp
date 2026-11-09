/**
 * @file skterraincell.cpp
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

#include "skterraincell.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

#include <zlib.h>

using namespace SKTerrain;

namespace
{
    constexpr char MAGIC[4]      = { 'S', 'K', 'T', 'C' };
    constexpr U32  MAX_SIDECAR   = 1u << 20;
    constexpr U32  MAX_BLOCK     = CELL_SAMPLES * 4;

    // Little endian, whatever the host.
    struct Writer
    {
        std::vector<U8>& mOut;
        void bytes(const void* data, size_t size)
        {
            const U8* p = static_cast<const U8*>(data);
            mOut.insert(mOut.end(), p, p + size);
        }
        void u8(U8 v) { mOut.push_back(v); }
        void u32(U32 v)
        {
            for (int i = 0; i < 4; ++i)
                mOut.push_back(U8(v >> (8 * i)));
        }
        void u64(U64 v)
        {
            u32(U32(v));
            u32(U32(v >> 32));
        }
        void s32(S32 v) { u32(U32(v)); }
        void f32(F32 v)
        {
            U32 u;
            memcpy(&u, &v, 4);
            u32(u);
        }
    };

    struct Reader
    {
        const U8* mData;
        size_t    mSize;
        size_t    mPos = 0;
        bool      mOk  = true;

        bool need(size_t n)
        {
            if (!mOk || mSize - mPos < n)
                mOk = false;
            return mOk;
        }
        const U8* bytes(size_t n)
        {
            if (!need(n))
                return nullptr;
            const U8* p = mData + mPos;
            mPos += n;
            return p;
        }
        U8 u8() { return need(1) ? mData[mPos++] : 0; }
        U32 u32()
        {
            if (!need(4))
                return 0;
            U32 v = 0;
            for (int i = 0; i < 4; ++i)
                v |= U32(mData[mPos + i]) << (8 * i);
            mPos += 4;
            return v;
        }
        U64 u64()
        {
            U64 lo = u32();
            return lo | (U64(u32()) << 32);
        }
        S32 s32() { return S32(u32()); }
        F32 f32()
        {
            U32 u = u32();
            F32 v;
            memcpy(&v, &u, 4);
            return v;
        }
    };

    struct LayerHeader
    {
        S32 mOffsetCm = 0;
        U8  mBytes    = 0; // per sample, 2 or 4
        U32 mPacked   = 0;
    };

    // Stored values are height - offset with 0 for missing, as the dataset's PNG has them; each row is
    // kept as differences along it and split into byte planes, which deflate takes far better.
    std::vector<U8> packLayer(const SKTerrainLayerSamples& layer, LayerHeader& header)
    {
        S32 lo = INT_MAX;
        S32 hi = INT_MIN;
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
        {
            if (layer.mValid[i])
            {
                lo = std::min(lo, layer.mHeightCm[i]);
                hi = std::max(hi, layer.mHeightCm[i]);
            }
        }
        if (lo > hi)
            lo = hi = 0;
        header.mOffsetCm = lo - 1;
        header.mBytes    = (S64(hi) - header.mOffsetCm) <= 0xffff ? 2 : 4;

        std::vector<U8> planes(size_t(CELL_SAMPLES) * header.mBytes);
        for (U32 row = 0; row < CELL_SIZE; ++row)
        {
            U32 prev = 0;
            for (U32 col = 0; col < CELL_SIZE; ++col)
            {
                const U32 i     = row * CELL_SIZE + col;
                const U32 value = layer.mValid[i] ? U32(S64(layer.mHeightCm[i]) - header.mOffsetCm) : 0;
                const U32 delta = value - prev;
                prev            = value;
                for (U32 b = 0; b < header.mBytes; ++b)
                    planes[size_t(b) * CELL_SAMPLES + i] = U8(delta >> (8 * b));
            }
        }

        uLongf          packed_size = compressBound(uLong(planes.size()));
        std::vector<U8> packed(packed_size);
        if (compress2(packed.data(), &packed_size, planes.data(), uLong(planes.size()), 6) != Z_OK)
            return {};
        packed.resize(packed_size);
        header.mPacked = U32(packed_size);
        return packed;
    }

    bool unpackLayer(const U8* packed, const LayerHeader& header, SKTerrainLayerSamples& layer)
    {
        const size_t    raw_size = size_t(CELL_SAMPLES) * header.mBytes;
        std::vector<U8> planes(raw_size);
        uLongf          out_size = uLongf(raw_size);
        if (uncompress(planes.data(), &out_size, packed, header.mPacked) != Z_OK || out_size != raw_size)
            return false;

        layer.resize();
        for (U32 row = 0; row < CELL_SIZE; ++row)
        {
            U32 value = 0;
            for (U32 col = 0; col < CELL_SIZE; ++col)
            {
                const U32 i     = row * CELL_SIZE + col;
                U32       delta = 0;
                for (U32 b = 0; b < header.mBytes; ++b)
                    delta |= U32(planes[size_t(b) * CELL_SAMPLES + i]) << (8 * b);
                value += delta;
                if (header.mBytes == 2)
                    value &= 0xffff;
                if (value)
                {
                    layer.mHeightCm[i] = S32(S64(header.mOffsetCm) + value);
                    layer.mValid[i]    = 1;
                }
            }
        }
        return true;
    }

    void layerRange(const SKTerrainLayerSamples& layer, SKTerrainLayerInfo& info)
    {
        S32 lo    = INT_MAX;
        S32 hi    = INT_MIN;
        U32 valid = 0;
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
        {
            if (layer.mValid[i])
            {
                lo = std::min(lo, layer.mHeightCm[i]);
                hi = std::max(hi, layer.mHeightCm[i]);
                ++valid;
            }
        }
        info.mValid = valid;
        info.mMin   = valid ? F32(lo) / 100.f : 0.f;
        info.mMax   = valid ? F32(hi) / 100.f : -1.f;
    }

    SKTerrainCell::EDecode readHeader(Reader& in, SKTerrainCell& cell, std::array<LayerHeader, LAYER_COUNT>& layers,
                                      U32& layer_mask, U32& sidecar_size)
    {
        const U8* magic = in.bytes(4);
        if (!magic || memcmp(magic, MAGIC, 4) != 0)
            return SKTerrainCell::DECODE_CORRUPT;
        if (in.u32() != SKTerrainCell::FORMAT_VERSION)
            return in.mOk ? SKTerrainCell::DECODE_VERSION : SKTerrainCell::DECODE_CORRUPT;

        cell.mHandle      = in.u64();
        cell.mWater       = in.f32();
        cell.mCaptureDate = in.u64();
        layer_mask        = in.u32();
        sidecar_size      = in.u32();
        for (U32 l = 0; l < LAYER_COUNT; ++l)
        {
            SKTerrainLayerInfo& info = cell.mInfo[l];
            info.mRevision           = in.u32();
            info.mStamp              = in.u64();
            info.mValid              = in.u32();
            info.mMin                = in.f32();
            info.mMax                = in.f32();
            layers[l].mOffsetCm      = in.s32();
            layers[l].mBytes         = in.u8();
            in.bytes(3);
            layers[l].mPacked = in.u32();
            if ((layer_mask & (1u << l)) && ((layers[l].mBytes != 2 && layers[l].mBytes != 4) || layers[l].mPacked > MAX_BLOCK))
                return SKTerrainCell::DECODE_CORRUPT;
        }
        if (!in.mOk || sidecar_size > MAX_SIDECAR || (layer_mask >> LAYER_COUNT) != 0 || !std::isfinite(cell.mWater))
            return SKTerrainCell::DECODE_CORRUPT;
        return SKTerrainCell::DECODE_OK;
    }

    bool checksumMatches(const U8* data, size_t size)
    {
        if (size < 4)
            return false;
        const size_t body   = size - 4;
        const U32    stored = U32(data[body]) | U32(data[body + 1]) << 8 | U32(data[body + 2]) << 16 | U32(data[body + 3]) << 24;
        return U32(crc32(0, data, uInt(body))) == stored;
    }
} // namespace

U32 SKTerrainLayerSamples::validCount() const
{
    return U32(std::count(mValid.begin(), mValid.end(), U8(1)));
}

SKTerrainLayerSamples SKTerrainLayerSamples::fromMetres(const F32* heights, const U8* valid)
{
    SKTerrainLayerSamples out;
    out.resize();
    for (U32 i = 0; i < CELL_SAMPLES; ++i)
    {
        const F32 h = heights[i];
        if ((valid && !valid[i]) || !std::isfinite(h))
            continue;
        out.mHeightCm[i] = S32(std::lround(F64(h) * 100.0));
        out.mValid[i]    = 1;
    }
    return out;
}

SKTerrainPyramid SKTerrainPyramid::build(const SKTerrainLayerSamples& samples, F32 fill_height)
{
    constexpr F32 NONE_MIN = std::numeric_limits<F32>::max();
    constexpr F32 NONE_MAX = -std::numeric_limits<F32>::max();

    SKTerrainPyramid pyramid;
    for (U32 k = 0; k < LEVEL_COUNT; ++k)
    {
        Level&    level = pyramid.mLevels[k];
        const U32 n     = CELL_SIZE >> k;
        const U32 step  = 1u << k;
        level.mSize     = n;
        level.mHeight.resize(size_t(n) * n);
        level.mValid.resize(size_t(n) * n);
        level.mMin.resize(size_t(n) * n);
        level.mMax.resize(size_t(n) * n);

        for (U32 j = 0; j < n; ++j)
        {
            for (U32 i = 0; i < n; ++i)
            {
                const U32 node = j * n + i;
                const U32 base = (j * step) * CELL_SIZE + i * step;
                const bool ok  = !samples.empty() && samples.mValid[base];
                level.mValid[node]  = ok;
                level.mHeight[node] = ok ? F32(samples.mHeightCm[base]) / 100.f : fill_height;

                if (k == 0)
                {
                    level.mMin[node] = ok ? level.mHeight[node] : NONE_MIN;
                    level.mMax[node] = ok ? level.mHeight[node] : NONE_MAX;
                    continue;
                }
                const Level& child = pyramid.mLevels[k - 1];
                const U32    cn    = child.mSize;
                F32          lo    = NONE_MIN;
                F32          hi    = NONE_MAX;
                for (U32 dj = 0; dj < 2; ++dj)
                {
                    for (U32 di = 0; di < 2; ++di)
                    {
                        const U32 c = (2 * j + dj) * cn + 2 * i + di;
                        lo          = std::min(lo, child.mMin[c]);
                        hi          = std::max(hi, child.mMax[c]);
                    }
                }
                level.mMin[node] = lo;
                level.mMax[node] = hi;
            }
        }
    }

    const Level& top = pyramid.mLevels[LEVEL_COUNT - 1];
    if (top.mMin[0] <= top.mMax[0])
    {
        pyramid.mMin = top.mMin[0];
        pyramid.mMax = top.mMax[0];
    }
    return pyramid;
}

void SKTerrainCell::setLayer(ELayer layer, SKTerrainLayerSamples samples, U64 stamp)
{
    mLayers[layer] = std::move(samples);
    SKTerrainLayerInfo& info = mInfo[layer];
    ++info.mRevision;
    info.mStamp = stamp;
    if (mLayers[layer].empty())
    {
        info.mValid = 0;
        info.mMin   = 0.f;
        info.mMax   = -1.f;
    }
    else
    {
        layerRange(mLayers[layer], info);
    }
}

std::vector<U8> SKTerrainCell::encode() const
{
    std::array<LayerHeader, LAYER_COUNT>     headers{};
    std::array<std::vector<U8>, LAYER_COUNT> packed;
    U32                                      layer_mask = 0;
    for (U32 l = 0; l < LAYER_COUNT; ++l)
    {
        if (mLayers[l].empty())
            continue;
        packed[l] = packLayer(mLayers[l], headers[l]);
        if (packed[l].empty())
            return {};
        layer_mask |= 1u << l;
    }

    std::vector<U8> out;
    Writer          w{ out };
    w.bytes(MAGIC, 4);
    w.u32(FORMAT_VERSION);
    w.u64(mHandle);
    w.f32(mWater);
    w.u64(mCaptureDate);
    w.u32(layer_mask);
    w.u32(U32(std::min<size_t>(mSidecar.size(), MAX_SIDECAR)));
    for (U32 l = 0; l < LAYER_COUNT; ++l)
    {
        const SKTerrainLayerInfo& info = mInfo[l];
        w.u32(info.mRevision);
        w.u64(info.mStamp);
        w.u32(info.mValid);
        w.f32(info.mMin);
        w.f32(info.mMax);
        w.s32(headers[l].mOffsetCm);
        w.u8(headers[l].mBytes);
        w.u8(0);
        w.u8(0);
        w.u8(0);
        w.u32(headers[l].mPacked);
    }
    w.bytes(mSidecar.data(), std::min<size_t>(mSidecar.size(), MAX_SIDECAR));
    for (U32 l = 0; l < LAYER_COUNT; ++l)
        w.bytes(packed[l].data(), packed[l].size());
    w.u32(U32(crc32(0, out.data(), uInt(out.size()))));
    return out;
}

SKTerrainCell::EDecode SKTerrainCell::peek(const U8* data, size_t size, SKTerrainCell& header_only)
{
    Reader                               in{ data, size };
    std::array<LayerHeader, LAYER_COUNT> headers{};
    U32                                  layer_mask   = 0;
    U32                                  sidecar_size = 0;
    header_only                                       = SKTerrainCell();
    const EDecode result                              = readHeader(in, header_only, headers, layer_mask, sidecar_size);
    if (result != DECODE_OK)
        return result;
    return checksumMatches(data, size) ? DECODE_OK : DECODE_CORRUPT;
}

SKTerrainCell::EDecode SKTerrainCell::decode(const U8* data, size_t size)
{
    *this = SKTerrainCell();
    Reader                               in{ data, size };
    std::array<LayerHeader, LAYER_COUNT> headers{};
    U32                                  layer_mask   = 0;
    U32                                  sidecar_size = 0;
    EDecode                              result       = readHeader(in, *this, headers, layer_mask, sidecar_size);
    if (result != DECODE_OK)
        return result;
    if (!checksumMatches(data, size))
        return DECODE_CORRUPT;

    if (const U8* sidecar = in.bytes(sidecar_size))
        mSidecar.assign(reinterpret_cast<const char*>(sidecar), sidecar_size);
    for (U32 l = 0; l < LAYER_COUNT; ++l)
    {
        if (!(layer_mask & (1u << l)))
            continue;
        const U8* packed = in.bytes(headers[l].mPacked);
        if (!packed || !unpackLayer(packed, headers[l], mLayers[l]))
        {
            *this = SKTerrainCell();
            return DECODE_CORRUPT;
        }
    }
    if (!in.mOk || in.mPos + 4 != size)
    {
        *this = SKTerrainCell();
        return DECODE_CORRUPT;
    }
    return DECODE_OK;
}
