/**
 * @file skcensuscluster.cpp
 * @brief Clusters: connected components over census records, tiled on the global lattice.
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

#include "skcensuscluster.h"

#include "skcensusdelta.h"

#include <algorithm>
#include <numeric>

namespace
{
    struct Component
    {
        std::vector<S32> mParent;

        explicit Component(size_t n) : mParent(n)
        {
            std::iota(mParent.begin(), mParent.end(), 0);
        }

        S32 find(S32 i)
        {
            while (mParent[i] != i)
            {
                mParent[i] = mParent[mParent[i]];
                i = mParent[i];
            }
            return i;
        }

        void unite(S32 a, S32 b)
        {
            a = find(a);
            b = find(b);
            if (a != b)
            {
                mParent[std::max(a, b)] = std::min(a, b);
            }
        }
    };

    bool overlaps(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        for (S32 i = 0; i < 3; ++i)
        {
            if (amin.mdV[i] > bmax.mdV[i] || bmin.mdV[i] > amax.mdV[i])
            {
                return false;
            }
        }
        return true;
    }

    S32 cell(F64 metres, F64 pitch)
    {
        return static_cast<S32>(std::floor(metres / pitch));
    }

    // A box cut to the window in XY; false when nothing of it is left.
    bool clip(const LLVector3d& mn, const LLVector3d& mx, const skworld::ClusterWindow& window, LLVector3d& out_min, LLVector3d& out_max)
    {
        out_min = mn;
        out_max = mx;
        if (!window.clips())
        {
            return true;
        }
        for (S32 a = VX; a <= VY; ++a)
        {
            out_min.mdV[a] = std::max(mn.mdV[a], window.mMin.mdV[a]);
            out_max.mdV[a] = std::min(mx.mdV[a], window.mMax.mdV[a]);
            if (out_min.mdV[a] > out_max.mdV[a])
            {
                return false;
            }
        }
        return true;
    }

    // Eight bytes of an id as a big-endian word, so words compare as the bytes do.
    U64 idWord(const LLUUID& id, size_t at)
    {
        U64 w = 0;
        for (size_t b = 0; b < 8; ++b)
        {
            w = (w << 8) | id.mData[at + b];
        }
        return w;
    }

    // A tile as one word that sorts as TileKey does: x, then y.
    U64 tileOrder(const skworld::TileKey& k)
    {
        return (static_cast<U64>(static_cast<U32>(k.mX) ^ 0x80000000u) << 32) | (static_cast<U32>(k.mY) ^ 0x80000000u);
    }

    template<class Fn>
    void forEachTile(const LLVector3d& mn, const LLVector3d& mx, F64 tile, Fn&& fn)
    {
        const S32 x0 = cell(mn.mdV[VX], tile), x1 = cell(mx.mdV[VX], tile);
        const S32 y0 = cell(mn.mdV[VY], tile), y1 = cell(mx.mdV[VY], tile);
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                fn({ x, y });
            }
        }
    }
}

namespace skworld
{
    void ClusterSet::at(const LLVector3d& p, std::vector<ClusterPtr>& out) const
    {
        const auto it = mByTile.find(TileKey{ cell(p.mdV[VX], mTile), cell(p.mdV[VY], mTile) });
        if (it == mByTile.end())
        {
            return;
        }
        for (const ClusterPtr& c : it->second)
        {
            if (overlaps(p, p, c->mMin, c->mMax))
            {
                out.push_back(c);
            }
        }
    }

    const Cluster* ClusterSet::find(const LLUUID& id) const
    {
        for (const ClusterPtr& c : mClusters)
        {
            if (c->mId == id)
            {
                return c.get();
            }
        }
        return nullptr;
    }

    ClusterSet cluster(const std::vector<RecordPtr>& records, F64 gap, F64 tile, const ClusterWindow& window)
    {
        // Static records only, in a stable order so component ids do not depend
        // on the census's iteration order. A record no class builds with, a
        // vehicle, would only re-sign the tiles it drives across; a visible-only
        // one is no shape at all. Held by address: nothing here outlives the records.
        // Sorted on the ids as big-endian words, which order as LLUUID's byte compare does.
        struct Order
        {
            U64 mKey[4];
            const RecordPtr* mRecord;
        };
        std::vector<Order> order;
        order.reserve(records.size());
        for (const RecordPtr& r : records)
        {
            if (r && r->mCollides && !r->mDynamic && r->mNavRole != NavRole::DYNAMIC && (r->mExclude & EXCLUDE_ALL) != EXCLUDE_ALL)
            {
                order.push_back({ { idWord(r->mRoot, 0), idWord(r->mRoot, 8), idWord(r->mPart, 0), idWord(r->mPart, 8) }, &r });
            }
        }
        std::sort(order.begin(), order.end(), [](const Order& a, const Order& b) {
            return std::lexicographical_compare(a.mKey, a.mKey + 4, b.mKey, b.mKey + 4);
        });
        std::vector<const Record*> live;
        std::vector<const RecordPtr*> held;
        live.reserve(order.size());
        held.reserve(order.size());
        for (const Order& o : order)
        {
            held.push_back(o.mRecord);
            live.push_back(o.mRecord->get());
        }
        std::vector<Order>().swap(order);

        const size_t n = live.size();
        Component comp(n);

        // Linksets first: every part of a root is one component.
        for (size_t i = 1; i < n; ++i)
        {
            if (live[i]->mRoot == live[i - 1]->mRoot)
            {
                comp.unite(static_cast<S32>(i - 1), static_cast<S32>(i));
            }
        }

        // Then anything whose inflated AABB meets another's, through a hash grid
        // of the inflated boxes so the test is linear in the record count: one
        // flat list of (cell, record) sorted by cell, each cell's run in record order.
        // A record wholly outside the window joins only its linkset.
        const LLVector3d pad(gap, gap, gap);
        std::vector<LLVector3d> cmin(n), cmax(n), mn(n), mx(n);
        std::vector<char> inside(n);
        std::vector<std::pair<U64, S32>> grid;
        grid.reserve(n * 2);
        S32 gx0 = S32_MAX, gy0 = S32_MAX, gx1 = S32_MIN, gy1 = S32_MIN;
        for (size_t i = 0; i < n; ++i)
        {
            inside[i] = clip(live[i]->mMin, live[i]->mMax, window, cmin[i], cmax[i]);
            if (!inside[i])
            {
                continue;
            }
            mn[i] = cmin[i] - pad;
            mx[i] = cmax[i] + pad;
            forEachTile(mn[i], mx[i], tile, [&](const TileKey& k) { grid.emplace_back(tileOrder(k), static_cast<S32>(i)); });
            gx0 = std::min(gx0, cell(mn[i].mdV[VX], tile));
            gy0 = std::min(gy0, cell(mn[i].mdV[VY], tile));
            gx1 = std::max(gx1, cell(mx[i].mdV[VX], tile));
            gy1 = std::max(gy1, cell(mx[i].mdV[VY], tile));
        }
        // By cell, each cell's run in record order: counted into place over
        // the cells' box when that is no bigger than the list, else sorted.
        const U64 span = grid.empty() ? 0 : static_cast<U64>(static_cast<S64>(gx1) - gx0 + 1) * static_cast<U64>(static_cast<S64>(gy1) - gy0 + 1);
        if (!grid.empty() && span <= grid.size() * 4)
        {
            const U64 rows = static_cast<U64>(static_cast<S64>(gy1) - gy0 + 1);
            const auto dense = [&](U64 key) {
                const S32 x = static_cast<S32>(static_cast<U32>(key >> 32) ^ 0x80000000u);
                const S32 y = static_cast<S32>(static_cast<U32>(key) ^ 0x80000000u);
                return static_cast<size_t>(static_cast<U64>(static_cast<S64>(x) - gx0) * rows + static_cast<U64>(static_cast<S64>(y) - gy0));
            };
            std::vector<U32> at(span + 1, 0);
            for (const auto& e : grid)
            {
                ++at[dense(e.first) + 1];
            }
            for (size_t k = 1; k <= span; ++k)
            {
                at[k] += at[k - 1];
            }
            std::vector<std::pair<U64, S32>> sorted(grid.size());
            for (const auto& e : grid)
            {
                sorted[at[dense(e.first)]++] = e;
            }
            grid.swap(sorted);
        }
        else
        {
            std::sort(grid.begin(), grid.end());
        }
        // Each cell's boxes are copied out side by side and swept along x, so a
        // pair is tested only while their x spans meet. The components do not
        // depend on the order pairs are united in.
        struct CellBox
        {
            F64 mMin[3];
            F64 mMax[3];
            S32 mIndex;
            S32 mRoot;      ///< its component as last looked up: equal, they are one component
        };
        std::vector<CellBox> boxes;
        for (size_t first = 0; first < grid.size();)
        {
            size_t end = first + 1;
            while (end < grid.size() && grid[end].first == grid[first].first)
            {
                ++end;
            }
            const S32 cx = static_cast<S32>(static_cast<U32>(grid[first].first >> 32) ^ 0x80000000u);
            const S32 cy = static_cast<S32>(static_cast<U32>(grid[first].first) ^ 0x80000000u);
            boxes.clear();
            for (size_t a = first; a < end; ++a)
            {
                const S32 i = grid[a].second;
                boxes.push_back({ { mn[i].mdV[0], mn[i].mdV[1], mn[i].mdV[2] }, { mx[i].mdV[0], mx[i].mdV[1], mx[i].mdV[2] }, i, comp.find(i) });
            }
            std::sort(boxes.begin(), boxes.end(), [](const CellBox& a, const CellBox& b) { return a.mMin[0] < b.mMin[0]; });
            const size_t count = boxes.size();
            for (size_t a = 0; a < count; ++a)
            {
                CellBox& p = boxes[a];
                p.mRoot = comp.find(p.mIndex);
                for (size_t b = a + 1; b < count && boxes[b].mMin[0] <= p.mMax[0]; ++b)
                {
                    CellBox& q = boxes[b];
                    // A pair two cells share is tested only in the cell holding the corner where their boxes meet.
                    if (p.mRoot != q.mRoot && q.mMin[1] <= p.mMax[1] && p.mMin[1] <= q.mMax[1] && q.mMin[2] <= p.mMax[2]
                        && p.mMin[2] <= q.mMax[2] && cell(std::max(p.mMin[0], q.mMin[0]), tile) == cx
                        && cell(std::max(p.mMin[1], q.mMin[1]), tile) == cy)
                    {
                        q.mRoot = comp.find(q.mIndex);
                        if (q.mRoot != p.mRoot)
                        {
                            comp.unite(p.mIndex, q.mIndex);
                            p.mRoot = comp.find(p.mIndex);
                            q.mRoot = p.mRoot;
                        }
                    }
                }
            }
            first = end;
        }
        std::vector<std::pair<U64, S32>>().swap(grid);

        // Gather. The root index of a component is its smallest member index,
        // and members are sorted by root UUID, so that member's root is the id.
        // Clusters come in root index order, each one's members in index order,
        // laid out flat by a counting pass.
        std::vector<S32> slot(n);
        std::vector<S32> starts;
        for (size_t i = 0; i < n; ++i)
        {
            const S32 root = comp.find(static_cast<S32>(i));
            if (root == static_cast<S32>(i))
            {
                slot[i] = static_cast<S32>(starts.size());
                starts.push_back(0);
            }
            else
            {
                slot[i] = slot[root];
            }
            ++starts[slot[i]];
        }
        const size_t count = starts.size();
        std::vector<S32> member_list(n);
        {
            S32 at = 0;
            for (S32& s : starts)
            {
                const S32 c = s;
                s = at;
                at += c;
            }
            starts.push_back(at);
            std::vector<S32> fill(starts.begin(), starts.end() - 1);
            for (size_t i = 0; i < n; ++i)
            {
                member_list[fill[slot[i]]++] = static_cast<S32>(i);
            }
        }

        ClusterSet set;
        set.mTile = tile;
        set.mClusters.reserve(count);
        std::vector<U64> tile_sigs;
        for (size_t ci = 0; ci < count; ++ci)
        {
            const S32* members = member_list.data() + starts[ci];
            const size_t member_count = static_cast<size_t>(starts[ci + 1] - starts[ci]);
            auto c = std::make_shared<Cluster>();
            c->mId = live[members[0]]->mRoot;
            c->mRecords.reserve(member_count);
            for (size_t m = 0; m < member_count; ++m)
            {
                c->mRecords.push_back(*held[members[m]]);
                c->mHash += recordHash(*live[members[m]]);
            }
            bool placed = false;
            for (size_t m = 0; m < member_count; ++m)
            {
                const S32 i = members[m];
                if (!inside[i])
                {
                    continue;
                }
                if (!placed)
                {
                    c->mMin = cmin[i];
                    c->mMax = cmax[i];
                    placed = true;
                }
                for (S32 k = 0; k < 3; ++k)
                {
                    c->mMin.mdV[k] = std::min(c->mMin.mdV[k], cmin[i].mdV[k]);
                    c->mMax.mdV[k] = std::max(c->mMax.mdV[k], cmax[i].mdV[k]);
                }
            }
            if (!placed)
            {
                // Nothing of it is in the window: its own bounds, and no tiles.
                c->mMin = c->mRecords.front()->mMin;
                c->mMax = c->mRecords.front()->mMax;
                for (const RecordPtr& r : c->mRecords)
                {
                    for (S32 k = 0; k < 3; ++k)
                    {
                        c->mMin.mdV[k] = std::min(c->mMin.mdV[k], r->mMin.mdV[k]);
                        c->mMax.mdV[k] = std::max(c->mMax.mdV[k], r->mMax.mdV[k]);
                    }
                }
                set.mClusters.push_back(std::move(c));
                continue;
            }
            // Per tile: the signature of the members that intersect it, in
            // member order (already sorted by root and part): each member mixes
            // itself into the tiles its box covers, held flat over the cluster's
            // box, so no tile walks the members. A structure of twenty thousand
            // records over a square kilometre is otherwise records times tiles,
            // millions of overlap tests on the main thread at every census.
            const S32 x0 = cell(c->mMin.mdV[VX], tile), x1 = cell(c->mMax.mdV[VX], tile);
            const S32 y0 = cell(c->mMin.mdV[VY], tile), y1 = cell(c->mMax.mdV[VY], tile);
            const size_t rows = static_cast<size_t>(y1 - y0 + 1);
            tile_sigs.assign(static_cast<size_t>(x1 - x0 + 1) * rows, SEED);
            for (size_t m = 0; m < member_count; ++m)
            {
                const S32 i = members[m];
                if (inside[i])
                {
                    const U64 signature = live[i]->mSignature;
                    forEachTile(cmin[i], cmax[i], tile, [&](const TileKey& k) {
                        U64& sig = tile_sigs[static_cast<size_t>(k.mX - x0) * rows + static_cast<size_t>(k.mY - y0)];
                        sig = mix(sig, signature);
                    });
                }
            }
            for (S32 x = x0, t = 0; x <= x1; ++x)
            {
                for (S32 y = y0; y <= y1; ++y)
                {
                    c->mTiles.emplace_hint(c->mTiles.end(), TileKey{ x, y }, tile_sigs[t++]);
                }
            }
            ClusterPtr cp = std::move(c);
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    set.mByTile[{ x, y }].push_back(cp);
                }
            }
            set.mClusters.push_back(std::move(cp));
        }
        return set;
    }
}
