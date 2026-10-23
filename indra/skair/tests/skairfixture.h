/**
 * @file skairfixture.h
 * @brief Reading the air raster's captured fixtures (.skaf) back: the soup a tile was made from and Recast's states.
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

#ifndef SK_AIRFIXTURE_H
#define SK_AIRFIXTURE_H

#include "skaircolumns.h"
#include "skairsoup.h"
#include "skcensustypes.h"

#include "llsdjson.h"

#include <zlib.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace airfixture
{
    using namespace skworld;

    /// A captured raster state: columns in Recast's order (x = SL y fastest, z = SL x), spans packed
    /// smin | smax << 13 | area << 26.
    struct RecastColumns
    {
        S32 mWidth = 0;
        S32 mHeight = 0;
        std::vector<U32> mStart;
        std::vector<U32> mSpans;

        static U32 smin(U32 v) { return v & 0x1fff; }
        static U32 smax(U32 v) { return (v >> 13) & 0x1fff; }
        static U32 area(U32 v) { return v >> 26; }
        /// The Recast column of SL column (ix, iy).
        S32 column(S32 ix, S32 iy) const { return iy + ix * mWidth; }
    };

    struct Fixture
    {
        std::map<std::string, std::string> mSections;
        LLSD mParams;
        F32 mBmin[3] = {};
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        S32 mBorder = 0;
        S32 mAvatarBorder = 0;

        bool has(const char* tag) const { return mSections.count(tag) != 0; }

        template<typename T> std::vector<T> array(const char* tag) const
        {
            const auto it = mSections.find(tag);
            std::vector<T> out;
            if (it != mSections.end())
            {
                out.resize(it->second.size() / sizeof(T));
                std::memcpy(out.data(), it->second.data(), out.size() * sizeof(T));
            }
            return out;
        }

        RecastColumns columns(const char* tag) const
        {
            RecastColumns c;
            const std::string& b = mSections.at(tag);
            std::memcpy(&c.mWidth, b.data(), 4);
            std::memcpy(&c.mHeight, b.data() + 4, 4);
            const size_t n = static_cast<size_t>(c.mWidth) * c.mHeight;
            c.mStart.assign(n + 1, 0);
            for (size_t i = 0; i < n; ++i)
            {
                U16 k;
                std::memcpy(&k, b.data() + 8 + i * 2, 2);
                c.mStart[i + 1] = c.mStart[i] + k;
            }
            c.mSpans.resize(c.mStart[n]);
            std::memcpy(c.mSpans.data(), b.data() + 8 + n * 2, c.mSpans.size() * 4);
            return c;
        }

        /// The frame the raster was made in, SL.
        ColumnFrame frame() const
        {
            ColumnFrame f;
            f.mOrigin.set(mBmin[2], mBmin[0], mBmin[1]);
            f.mCell = mCell;
            f.mUnit = mUnit;
            f.mBorder = mBorder;
            std::memcpy(&f.mWidth, mSections.at("RAS0").data(), 4);
            const F32 zmax = static_cast<F32>(mParams["z"][1].asReal());
            f.mHeightRange = std::ceil((zmax + mUnit) / mUnit) * mUnit - mBmin[1];
            f.mClimb = mParams["climb"].asInteger();
            return f;
        }

        /// The soup the raster was made from, back in SL axes.
        void soup(AirSoup& out) const
        {
            out = AirSoup();
            const std::vector<F32> rc = array<F32>("SOUP"), cut = array<F32>("CUTV");
            const auto toSL = [](const std::vector<F32>& in, std::vector<F32>& sl) {
                sl.resize(in.size());
                for (size_t i = 0; i + 2 < in.size(); i += 3)
                {
                    sl[i] = in[i + 2];
                    sl[i + 1] = in[i];
                    sl[i + 2] = in[i + 1];
                }
            };
            toSL(rc, out.mVerts);
            toSL(cut, out.mCutVerts);
            out.mCodes = array<U8>("CODE");
            const std::vector<U8> leaf = array<U8>("LEAF"), cues = array<U8>("CUES");
            out.mFlags.assign(out.mCodes.size(), 0);
            for (size_t t = 0; t < out.mFlags.size(); ++t)
            {
                const bool l = t < leaf.size() && leaf[t];
                out.mFlags[t] = static_cast<U8>((l ? SOUP_LEAF : 0) | ((t < cues.size() ? cues[t] : 0) << SOUP_CUE_SHIFT));
                out.mLeaves = out.mLeaves || l;
            }
            const std::vector<S32> solids = array<S32>("SOLD"), cuts = array<S32>("CUTR");
            for (size_t i = 0; i + 1 < solids.size(); i += 2)
            {
                out.mSolids.emplace_back(solids[i], solids[i + 1]);
            }
            for (size_t i = 0; i + 1 < cuts.size(); i += 2)
            {
                out.mCuts.emplace_back(cuts[i], cuts[i + 1]);
            }
        }

        void terrain(TerrainWindow& t) const
        {
            const std::string& b = mSections.at("TERR");
            std::memcpy(&t.mOrigin, b.data(), 12);
            std::memcpy(&t.mSpacing, b.data() + 12, 4);
            std::memcpy(&t.mWidth, b.data() + 16, 4);
            std::memcpy(&t.mHeight, b.data() + 20, 4);
            std::memcpy(&t.mWater, b.data() + 24, 4);
            t.mZ.resize((b.size() - 28) / 4);
            std::memcpy(t.mZ.data(), b.data() + 28, t.mZ.size() * 4);
        }
    };

    inline bool readFixture(const std::filesystem::path& path, Fixture& out)
    {
        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() < 12 || data.compare(0, 4, "SKAF") != 0)
        {
            return false;
        }
        U32 raw = 0;
        std::memcpy(&raw, data.data() + 8, 4);
        std::string body(raw, '\0');
        uLongf size = raw;
        if (uncompress(reinterpret_cast<Bytef*>(body.data()), &size, reinterpret_cast<const Bytef*>(data.data() + 12),
                       static_cast<uLong>(data.size() - 12)) != Z_OK ||
            size != raw)
        {
            return false;
        }
        for (size_t i = 0; i + 8 <= body.size();)
        {
            U32 n = 0;
            std::memcpy(&n, body.data() + i + 4, 4);
            out.mSections[body.substr(i, 4)] = body.substr(i + 8, n);
            i += 8 + n;
        }
        if (!out.has("GRID") || !out.has("PARM") || !out.has("SOUP"))
        {
            return false;
        }
        const std::string& g = out.mSections["GRID"];
        std::memcpy(out.mBmin, g.data(), 12);
        std::memcpy(&out.mCell, g.data() + 12, 4);
        std::memcpy(&out.mUnit, g.data() + 16, 4);
        std::memcpy(&out.mBorder, g.data() + 20, 4);
        std::memcpy(&out.mAvatarBorder, g.data() + 24, 4);
        return LlsdFromJsonString(out.mSections["PARM"], out.mParams);
    }
}

#endif // SK_AIRFIXTURE_H
