/**
 * @file skterrainpng.cpp
 * @brief Greyscale 16-bit PNG, the dataset's exchange format for a cell's heights.
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

#include "skterrainpng.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include <png.h>

using namespace SKTerrain;

namespace
{
    constexpr U32 MAX_SIDE = 4096;

    struct ReadSource
    {
        const U8* mData;
        size_t    mSize;
        size_t    mPos;
    };

    void readCallback(png_structp png, png_bytep out, png_size_t length)
    {
        ReadSource* src = static_cast<ReadSource*>(png_get_io_ptr(png));
        if (src->mSize - src->mPos < length)
            png_error(png, "truncated");
        memcpy(out, src->mData + src->mPos, length);
        src->mPos += length;
    }

    void writeCallback(png_structp png, png_bytep data, png_size_t length)
    {
        std::vector<U8>* out = static_cast<std::vector<U8>*>(png_get_io_ptr(png));
        out->insert(out->end(), data, data + length);
    }

    void flushCallback(png_structp) {}

    // libpng reports through longjmp; nothing with a destructor may live in the frames it unwinds.
    bool decodeInto(const U8* data, size_t size, SKTerrainPng::Grey16& image, std::vector<png_bytep>& rows)
    {
        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
            return false;
        png_infop info = png_create_info_struct(png);
        if (!info)
        {
            png_destroy_read_struct(&png, nullptr, nullptr);
            return false;
        }
        ReadSource src{ data, size, 0 };
        if (setjmp(png_jmpbuf(png)))
        {
            png_destroy_read_struct(&png, &info, nullptr);
            return false;
        }
        png_set_read_fn(png, &src, readCallback);
        png_read_info(png, info);

        const png_uint_32 width  = png_get_image_width(png, info);
        const png_uint_32 height = png_get_image_height(png, info);
        if (png_get_color_type(png, info) != PNG_COLOR_TYPE_GRAY || png_get_bit_depth(png, info) != 16 || width == 0 ||
            height == 0 || width > MAX_SIDE || height > MAX_SIDE)
        {
            png_destroy_read_struct(&png, &info, nullptr);
            return false;
        }
        png_set_swap(png); // network order to the host's
        png_read_update_info(png, info);

        image.mWidth  = width;
        image.mHeight = height;
        image.mPixels.resize(size_t(width) * height);
        rows.resize(height);
        for (png_uint_32 y = 0; y < height; ++y)
            rows[y] = reinterpret_cast<png_bytep>(image.mPixels.data() + size_t(y) * width);
        png_read_image(png, rows.data());
        png_read_end(png, nullptr);
        png_destroy_read_struct(&png, &info, nullptr);
        return true;
    }

    bool encodeInto(const SKTerrainPng::Grey16& image, std::vector<U8>& out, std::vector<png_bytep>& rows)
    {
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
            return false;
        png_infop info = png_create_info_struct(png);
        if (!info)
        {
            png_destroy_write_struct(&png, nullptr);
            return false;
        }
        if (setjmp(png_jmpbuf(png)))
        {
            png_destroy_write_struct(&png, &info);
            return false;
        }
        png_set_write_fn(png, &out, writeCallback, flushCallback);
        png_set_IHDR(png, info, image.mWidth, image.mHeight, 16, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        png_set_swap(png);
        rows.resize(image.mHeight);
        for (U32 y = 0; y < image.mHeight; ++y)
            rows[y] = reinterpret_cast<png_bytep>(const_cast<U16*>(image.mPixels.data() + size_t(y) * image.mWidth));
        png_write_image(png, rows.data());
        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
        return true;
    }
} // namespace

std::optional<SKTerrainPng::Grey16> SKTerrainPng::decode(const U8* data, size_t size)
{
    Grey16                 image;
    std::vector<png_bytep> rows;
    if (!data || !decodeInto(data, size, image, rows))
        return std::nullopt;
    return image;
}

std::vector<U8> SKTerrainPng::encode(const Grey16& image)
{
    std::vector<U8>        out;
    std::vector<png_bytep> rows;
    if (image.mPixels.size() != size_t(image.mWidth) * image.mHeight || !encodeInto(image, out, rows))
        return {};
    return out;
}

std::optional<SKTerrainLayerSamples> SKTerrainPng::toSamples(const Grey16& image, S32 offset_cm)
{
    if (image.mWidth != CELL_SIZE || image.mHeight != CELL_SIZE)
        return std::nullopt;
    SKTerrainLayerSamples out;
    out.resize();
    for (U32 i = 0; i < CELL_SAMPLES; ++i)
    {
        if (const U16 value = image.mPixels[i])
        {
            out.mHeightCm[i] = offset_cm + value;
            out.mValid[i]    = 1;
        }
    }
    return out;
}

std::optional<SKTerrainPng::Grey16> SKTerrainPng::fromSamples(const SKTerrainLayerSamples& samples, S32& offset_cm)
{
    if (samples.empty())
        return std::nullopt;
    S32 lo = INT_MAX;
    S32 hi = INT_MIN;
    for (U32 i = 0; i < CELL_SAMPLES; ++i)
    {
        if (samples.mValid[i])
        {
            lo = std::min(lo, samples.mHeightCm[i]);
            hi = std::max(hi, samples.mHeightCm[i]);
        }
    }
    if (lo > hi || S64(hi) - lo > 65534)
        return std::nullopt;

    offset_cm = lo - 1;
    Grey16 image;
    image.mWidth  = CELL_SIZE;
    image.mHeight = CELL_SIZE;
    image.mPixels.assign(CELL_SAMPLES, 0);
    for (U32 i = 0; i < CELL_SAMPLES; ++i)
    {
        if (samples.mValid[i])
            image.mPixels[i] = U16(samples.mHeightCm[i] - offset_cm);
    }
    return image;
}
