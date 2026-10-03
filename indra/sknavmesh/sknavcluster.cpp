/**
 * @file sknavcluster.cpp
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

#include "sknavcluster.h"

#include <algorithm>
#include <functional>
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

    void forEachTile(const LLVector3d& mn, const LLVector3d& mx, F64 tile, const std::function<void(const sknav::TileKey&)>& fn)
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

namespace sknav
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

    ClusterSet cluster(const std::vector<RecordPtr>& records, F64 gap, F64 tile)
    {
        // Static records only, in a stable order so component ids do not depend
        // on the census's iteration order. A record no class builds with, a
        // vehicle, would only re-sign the tiles it drives across.
        std::vector<RecordPtr> live;
        live.reserve(records.size());
        for (const RecordPtr& r : records)
        {
            if (r && !r->mDynamic && r->mNavRole != NavRole::DYNAMIC && (r->mExclude & EXCLUDE_ALL) != EXCLUDE_ALL)
            {
                live.push_back(r);
            }
        }
        std::sort(live.begin(), live.end(), [](const RecordPtr& a, const RecordPtr& b) {
            return a->mRoot < b->mRoot || (a->mRoot == b->mRoot && a->mPart < b->mPart);
        });

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
        // of the inflated boxes so the test is linear in the record count.
        const LLVector3d pad(gap, gap, gap);
        std::vector<LLVector3d> mn(n), mx(n);
        std::unordered_map<TileKey, std::vector<S32>, TileKeyHash> grid;
        for (size_t i = 0; i < n; ++i)
        {
            mn[i] = live[i]->mMin - pad;
            mx[i] = live[i]->mMax + pad;
            forEachTile(mn[i], mx[i], tile, [&](const TileKey& k) { grid[k].push_back(static_cast<S32>(i)); });
        }
        for (auto& entry : grid)
        {
            std::vector<S32>& members = entry.second;
            for (size_t a = 0; a < members.size(); ++a)
            {
                for (size_t b = a + 1; b < members.size(); ++b)
                {
                    const S32 i = members[a], j = members[b];
                    if (comp.find(i) != comp.find(j) && overlaps(mn[i], mx[i], mn[j], mx[j]))
                    {
                        comp.unite(i, j);
                    }
                }
            }
        }

        // Gather. The root index of a component is its smallest member index,
        // and members are sorted by root UUID, so that member's root is the id.
        std::map<S32, std::shared_ptr<Cluster>> built;
        for (size_t i = 0; i < n; ++i)
        {
            const S32 root = comp.find(static_cast<S32>(i));
            std::shared_ptr<Cluster>& c = built[root];
            if (!c)
            {
                c = std::make_shared<Cluster>();
                c->mId = live[root]->mRoot;
                c->mMin = live[i]->mMin;
                c->mMax = live[i]->mMax;
            }
            c->mRecords.push_back(live[i]);
            for (S32 k = 0; k < 3; ++k)
            {
                c->mMin.mdV[k] = std::min(c->mMin.mdV[k], live[i]->mMin.mdV[k]);
                c->mMax.mdV[k] = std::max(c->mMax.mdV[k], live[i]->mMax.mdV[k]);
            }
        }

        ClusterSet set;
        set.mTile = tile;
        for (auto& entry : built)
        {
            std::shared_ptr<Cluster>& c = entry.second;
            // Per tile: the signature of the members that intersect it, in
            // member order (already sorted by root and part). The members go
            // into a grid first: a structure of twenty thousand records over
            // a square kilometre is otherwise records times tiles, millions of
            // overlap tests on the main thread at every census.
            std::unordered_map<TileKey, std::vector<S32>, TileKeyHash> by_tile;
            for (size_t i = 0; i < c->mRecords.size(); ++i)
            {
                forEachTile(c->mRecords[i]->mMin, c->mRecords[i]->mMax, tile,
                            [&](const TileKey& k) { by_tile[k].push_back(static_cast<S32>(i)); });
            }
            forEachTile(c->mMin, c->mMax, tile, [&](const TileKey& k) {
                U64 sig = SEED;
                const auto it = by_tile.find(k);
                if (it != by_tile.end())
                {
                    for (S32 i : it->second)
                    {
                        sig = mix(sig, c->mRecords[i]->mSignature);
                    }
                }
                c->mTiles[k] = sig;
            });
            ClusterPtr cp = c;
            set.mClusters.push_back(cp);
            forEachTile(c->mMin, c->mMax, tile, [&](const TileKey& k) { set.mByTile[k].push_back(cp); });
        }
        return set;
    }
}
