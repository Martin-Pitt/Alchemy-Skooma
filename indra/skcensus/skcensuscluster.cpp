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

    void forEachTile(const LLVector3d& mn, const LLVector3d& mx, F64 tile, const std::function<void(const skworld::TileKey&)>& fn)
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
        // one is no shape at all.
        std::vector<RecordPtr> live;
        live.reserve(records.size());
        for (const RecordPtr& r : records)
        {
            if (r && r->mCollides && !r->mDynamic && r->mNavRole != NavRole::DYNAMIC && (r->mExclude & EXCLUDE_ALL) != EXCLUDE_ALL)
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
        // A record wholly outside the window joins only its linkset.
        const LLVector3d pad(gap, gap, gap);
        std::vector<LLVector3d> cmin(n), cmax(n), mn(n), mx(n);
        std::vector<char> inside(n);
        std::unordered_map<TileKey, std::vector<S32>, TileKeyHash> grid;
        for (size_t i = 0; i < n; ++i)
        {
            inside[i] = clip(live[i]->mMin, live[i]->mMax, window, cmin[i], cmax[i]);
            if (!inside[i])
            {
                continue;
            }
            mn[i] = cmin[i] - pad;
            mx[i] = cmax[i] + pad;
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
        // Bounds are the clipped boxes of the members inside the window.
        std::map<S32, std::shared_ptr<Cluster>> built;
        std::map<S32, std::vector<S32>> members_of;
        for (size_t i = 0; i < n; ++i)
        {
            const S32 root = comp.find(static_cast<S32>(i));
            std::shared_ptr<Cluster>& c = built[root];
            std::vector<S32>& members = members_of[root];
            if (!c)
            {
                c = std::make_shared<Cluster>();
                c->mId = live[root]->mRoot;
            }
            c->mRecords.push_back(live[i]);
            c->mHash += recordHash(*live[i]);
            members.push_back(static_cast<S32>(i));
        }

        ClusterSet set;
        set.mTile = tile;
        for (auto& entry : built)
        {
            std::shared_ptr<Cluster>& c = entry.second;
            const std::vector<S32>& members = members_of[entry.first];
            bool placed = false;
            for (S32 i : members)
            {
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
                set.mClusters.push_back(c);
                continue;
            }
            // Per tile: the signature of the members that intersect it, in
            // member order (already sorted by root and part). The members go
            // into a grid first: a structure of twenty thousand records over
            // a square kilometre is otherwise records times tiles, millions of
            // overlap tests on the main thread at every census.
            std::unordered_map<TileKey, std::vector<S32>, TileKeyHash> by_tile;
            for (size_t i = 0; i < members.size(); ++i)
            {
                const S32 m = members[i];
                if (inside[m])
                {
                    forEachTile(cmin[m], cmax[m], tile, [&](const TileKey& k) { by_tile[k].push_back(static_cast<S32>(i)); });
                }
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
