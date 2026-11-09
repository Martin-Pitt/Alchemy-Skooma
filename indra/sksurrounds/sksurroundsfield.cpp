/**
 * @file sksurroundsfield.cpp
 * @brief The distant regions' layout and the edge levels their seams share.
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

#include "sksurroundsfield.h"

#include <algorithm>
#include <cmath>

namespace SKSurrounds
{
    namespace
    {
        S32 floorDiv(S32 a, S32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

        // Distance from the camera to a node's box on the ground, the camera's height included.
        F64 boxDistance(const F64 cam[3], F64 camZ, S32 level, S32 x, S32 y)
        {
            const F64 s  = F64(REGION_METRES << level);
            const F64 x0 = F64(x) * REGION_METRES, y0 = F64(y) * REGION_METRES;
            const F64 dx = std::max({ x0 - cam[0], 0.0, cam[0] - (x0 + s) });
            const F64 dy = std::max({ y0 - cam[1], 0.0, cam[1] - (y0 + s) });
            return std::sqrt(dx * dx + dy * dy + camZ * camZ);
        }

        bool nodeLess(const Node& a, const Node& b)
        {
            if (a.mLevel != b.mLevel)
                return a.mLevel < b.mLevel;
            return a.mX != b.mX ? a.mX < b.mX : a.mY < b.mY;
        }
    }

    bool Selection::inReach(S32 level, S32 x, S32 y) const
    {
        return boxDistance(mCamera, 0.0, level, x, y) <= mParams.mReach;
    }

    bool Selection::containsSpecial(S32 level, S32 x, S32 y) const
    {
        const S32 s = 1 << level;
        for (const Node& n : mNodes)
        {
            if (x <= n.mX && n.mX < x + s && y <= n.mY && n.mY < y + s)
                return true;
        }
        return false;
    }

    void Selection::visit(S32 level, S32 x, S32 y)
    {
        if (!inReach(level, x, y))
            return;
        if (level > 0 && (containsSpecial(level, x, y) ||
                          boxDistance(mCamera, mCamera[2], level, x, y) < mParams.mSplit * F64(REGION_METRES << level)))
        {
            const S32 h = 1 << (level - 1);
            visit(level - 1, x, y);
            visit(level - 1, x, y + h);
            visit(level - 1, x + h, y);
            visit(level - 1, x + h, y + h);
            return;
        }
        if (level == 0 && mSpecial[(y - mGridY) * mGridW + (x - mGridX)])
            return;
        Node n;
        n.mX     = x;
        n.mY     = y;
        n.mLevel = S8(level);
        mFar.push_back(n);
    }

    S8 Selection::levelAt(S32 x, S32 y) const
    {
        if (x < mGridX || y < mGridY || x >= mGridX + mGridW || y >= mGridY + mGridH)
            return NO_NEIGHBOUR;
        return mLevels[(y - mGridY) * mGridW + (x - mGridX)];
    }

    void Selection::paint()
    {
        std::fill(mLevels.begin(), mLevels.end(), NO_NEIGHBOUR);
        for (const Node& n : mFar)
        {
            const S32 s = n.sizeCells();
            for (S32 j = 0; j < s; ++j)
                std::fill_n(&levelAt(n.mX, n.mY + j), s, n.mLevel);
        }
        for (const Node& n : mNodes)
            levelAt(n.mX, n.mY) = LEVEL_SPECIAL;
    }

    S8 Selection::sideLevel(const Node& node, Side side) const
    {
        const S32 s = node.sizeCells();
        S8        best = NO_NEIGHBOUR;
        for (S32 i = 0; i < s; ++i)
        {
            S8 l = NO_NEIGHBOUR;
            switch (side)
            {
                case SIDE_WEST: l = levelAt(node.mX - 1, node.mY + i); break;
                case SIDE_EAST: l = levelAt(node.mX + s, node.mY + i); break;
                case SIDE_SOUTH: l = levelAt(node.mX + i, node.mY - 1); break;
                default: l = levelAt(node.mX + i, node.mY + s); break;
            }
            best = std::min(best, l);
        }
        return best;
    }

    void Selection::select(const F64 camera[3], std::span<const Cell> live, const SelectParams& params)
    {
        std::copy_n(camera, 3, mCamera);
        mParams = params;
        mFar.clear();
        mNodes.clear();

        const S32 top = REGION_METRES << TOP_LEVEL;
        const S32 tx0 = S32(std::floor((camera[0] - params.mReach) / top));
        const S32 tx1 = S32(std::floor((camera[0] + params.mReach) / top));
        const S32 ty0 = S32(std::floor((camera[1] - params.mReach) / top));
        const S32 ty1 = S32(std::floor((camera[1] + params.mReach) / top));

        // The grid covers the top nodes and every live and ring cell.
        const S32 nearCells = std::max(params.mNearCells, 0);
        S32       x0 = tx0 << TOP_LEVEL, y0 = ty0 << TOP_LEVEL;
        S32       x1 = (tx1 + 1) << TOP_LEVEL, y1 = (ty1 + 1) << TOP_LEVEL;
        for (const Cell& c : live)
        {
            x0 = std::min(x0, c.mX - nearCells);
            y0 = std::min(y0, c.mY - nearCells);
            x1 = std::max(x1, c.mX + nearCells + 1);
            y1 = std::max(y1, c.mY + nearCells + 1);
        }
        mGridX = x0;
        mGridY = y0;
        mGridW = x1 - x0;
        mGridH = y1 - y0;
        mLevels.resize(size_t(mGridW) * mGridH);
        mSpecial.assign(size_t(mGridW) * mGridH, 0);

        for (const Cell& c : live)
            mSpecial[(c.mY - mGridY) * mGridW + (c.mX - mGridX)] = 1;
        for (const Cell& c : live)
        {
            for (S32 j = -nearCells; j <= nearCells; ++j)
                for (S32 i = -nearCells; i <= nearCells; ++i)
                {
                    U8& f = mSpecial[(c.mY + j - mGridY) * mGridW + (c.mX + i - mGridX)];
                    if (!f)
                        f = 2;
                }
        }
        // Live, then ring, each by (x, y); mNodes holds them while the far nodes are found.
        for (U8 band = 1; band <= 2; ++band)
        {
            for (S32 x = x0; x < x1; ++x)
                for (S32 y = y0; y < y1; ++y)
                {
                    if (mSpecial[(y - mGridY) * mGridW + (x - mGridX)] != band)
                        continue;
                    Node n;
                    n.mX     = x;
                    n.mY     = y;
                    n.mLevel = LEVEL_SPECIAL;
                    n.mBand  = band == 1 ? Band::Live : Band::Ring;
                    mNodes.push_back(n);
                }
        }

        for (S32 tx = tx0; tx <= tx1; ++tx)
            for (S32 ty = ty0; ty <= ty1; ++ty)
                visit(TOP_LEVEL, tx << TOP_LEVEL, ty << TOP_LEVEL);

        // Balance: a node splits while a cell across its edges is more than one level finer.
        for (bool changed = true; changed;)
        {
            changed = false;
            paint();
            mNext.clear();
            for (const Node& n : mFar)
            {
                bool split = false;
                if (n.mLevel > 0)
                {
                    for (U8 side = 0; side < SIDE_COUNT && !split; ++side)
                        split = sideLevel(n, Side(side)) < n.mLevel - 1;
                }
                if (!split)
                {
                    mNext.push_back(n);
                    continue;
                }
                changed     = true;
                const S32 h = 1 << (n.mLevel - 1);
                for (S32 dx = 0; dx <= h; dx += h)
                    for (S32 dy = 0; dy <= h; dy += h)
                    {
                        if (!inReach(n.mLevel - 1, n.mX + dx, n.mY + dy))
                            continue;
                        Node c   = n;
                        c.mX     = n.mX + dx;
                        c.mY     = n.mY + dy;
                        c.mLevel = S8(n.mLevel - 1);
                        mNext.push_back(c);
                    }
            }
            mFar.swap(mNext);
        }

        paint();
        std::sort(mFar.begin(), mFar.end(), nodeLess);
        mNodes.insert(mNodes.end(), mFar.begin(), mFar.end());
        for (Node& n : mNodes)
            for (U8 side = 0; side < SIDE_COUNT; ++side)
                n.mNeighbour[side] = sideLevel(n, Side(side));
    }

    F32 edgeLevel(const F32 a[3], const F32 b[3], const F32 origin[3], F32 density)
    {
        if (density <= 0.f)
            return 16.f;
        auto length = [](F32 x, F32 y, F32 z)
        {
            const F32 xx = x * x, yy = y * y, zz = z * z;
            const F32 s  = xx + yy;
            return std::sqrt(s + zz);
        };
        const F32 len = length(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
        F32       d3[3];
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 sum = a[i] + b[i];
            const F32 mid = 0.5f * sum;
            d3[i]         = mid - origin[i];
        }
        const F32 d     = length(d3[0], d3[1], d3[2]);
        const F32 num   = len * density;
        const F32 level = num / std::max(d, 1.f);
        return std::min(std::max(level, 1.f), MAX_EDGE_LEVEL);
    }

    void liveOrigin(const F64 camera[3], const F64 agentOrigin[2], const F64 frame[2], F32 out[3])
    {
        for (S32 i = 0; i < 2; ++i)
        {
            const F32 ca = F32(camera[i] - agentOrigin[i]);
            const F32 ra = F32(frame[i] - agentOrigin[i]);
            out[i]       = ca - ra;
        }
        out[2] = F32(camera[2]);
    }

    void farOrigin(const F64 camera[3], const F64 frame[2], F32 out[3])
    {
        out[0] = F32(camera[0] - frame[0]);
        out[1] = F32(camera[1] - frame[1]);
        out[2] = F32(camera[2]);
    }

    S32 ceilOdd(F64 level)
    {
        const S32 n = S32(std::ceil(level));
        return (n & 1) ? n : n + 1;
    }

    S32 fineHalfLevel(F32 parentLevel, bool fineFractionalOdd)
    {
        const F64 h = F64(parentLevel) / 2.0;
        if (fineFractionalOdd)
            return std::min(ceilOdd(h), 31);
        return std::min(std::max(S32(std::ceil(h)), 1), 32);
    }

    F32 parentParameter(S32 k, S32 m, bool firstHalf)
    {
        const S32 j  = firstHalf ? k : k + m;
        const S32 n2 = 2 * m;
        if (2 * j <= n2)
            return F32(j) / F32(n2);
        const F32 q = F32(n2 - j) / F32(n2);
        return 1.f - q;
    }

    MapTile mapTile(const Node& node)
    {
        MapTile t;
        t.mLevel  = std::max<S32>(node.mLevel, 0) + 1;
        const S32 span = 1 << (t.mLevel - 1);
        t.mX      = node.mX - (node.mX - floorDiv(node.mX, span) * span);
        t.mY      = node.mY - (node.mY - floorDiv(node.mY, span) * span);
        t.mMetres = F32(REGION_METRES * span);
        return t;
    }

    void mapUV(const MapTile& tile, F32 localX, F32 localY, F32 uv[2])
    {
        uv[0] = localX / tile.mMetres;
        uv[1] = localY / tile.mMetres;
    }
}
