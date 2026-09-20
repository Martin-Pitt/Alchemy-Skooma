/**
 * @file sknavcluster_test.cpp
 * @brief Clustering of census records: linksets, gaps, skyboxes, movers, stable ids and tile keys.
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

#include "../test/lltut.h"

#include "../sknavcluster.h"

#include <algorithm>

namespace
{
    using namespace sknav;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        return u;
    }

    RecordPtr box(U32 root, U32 part, F64 x0, F64 y0, F64 z0, F64 x1, F64 y1, F64 z1, bool dynamic = false)
    {
        auto r = std::make_shared<Record>();
        r->mRoot = id(root);
        r->mPart = id(part);
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        r->mDynamic = dynamic;
        r->mSignature = mix(mix(SEED, root), part);
        return r;
    }

    const Cluster* holding(const ClusterSet& s, U32 root)
    {
        for (const ClusterPtr& c : s.mClusters)
        {
            for (const RecordPtr& r : c->mRecords)
            {
                if (r->mRoot == id(root))
                {
                    return c.get();
                }
            }
        }
        return nullptr;
    }
}

namespace tut
{
    struct sknavcluster_data {};
    typedef test_group<sknavcluster_data> sknavcluster_group;
    typedef sknavcluster_group::object sknavcluster_object;
    tut::sknavcluster_group g("sknavcluster");

    // A linkset is one cluster however far apart its parts are.
    template<> template<>
    void sknavcluster_object::test<1>()
    {
        std::vector<RecordPtr> recs = {
            box(1, 1, 0, 0, 0, 1, 1, 1),
            box(1, 2, 50, 50, 0, 51, 51, 1),
        };
        const ClusterSet s = cluster(recs, 0.5);
        ensure_equals("one cluster", s.mClusters.size(), 1u);
        ensure_equals("both parts", s.mClusters[0]->mRecords.size(), 2u);
        ensure("id is the root", s.mClusters[0]->mId == id(1));
    }

    // A 0.4 m gap joins under a 0.5 m inflation; 0.6 m separates.
    template<> template<>
    void sknavcluster_object::test<2>()
    {
        std::vector<RecordPtr> close = { box(1, 1, 0, 0, 0, 1, 1, 1), box(2, 2, 1.4, 0, 0, 2.4, 1, 1) };
        std::vector<RecordPtr> apart = { box(1, 1, 0, 0, 0, 1, 1, 1), box(2, 2, 2.1, 0, 0, 3.1, 1, 1) };
        ensure_equals("0.4 m joins", cluster(close, 0.5).mClusters.size(), 1u);
        ensure_equals("0.6 m separates", cluster(apart, 0.5).mClusters.size(), 2u);
        ensure_equals("1.1 m joins at gap 0.6", cluster(apart, 0.6).mClusters.size(), 1u);
    }

    // A skybox is its own cluster; a mover never joins; ids do not depend on input order.
    template<> template<>
    void sknavcluster_object::test<3>()
    {
        std::vector<RecordPtr> recs = {
            box(7, 7, 10, 10, 3000, 20, 20, 3010),
            box(3, 3, 10, 10, 0, 20, 20, 10),
            box(2, 2, 19.9, 10, 0, 30, 20, 10),
            box(9, 9, 15, 15, 0, 16, 16, 2, true),
        };
        ClusterSet s = cluster(recs, 0.5);
        ensure_equals("skybox apart, mover out", s.mClusters.size(), 2u);
        const Cluster* ground = holding(s, 3);
        ensure("ground cluster", ground != nullptr);
        ensure_equals("ground has both", ground->mRecords.size(), 2u);
        ensure("smallest root is the id", ground->mId == id(2));
        ensure("mover absent", holding(s, 9) == nullptr);

        std::reverse(recs.begin(), recs.end());
        const ClusterSet t = cluster(recs, 0.5);
        ensure("id stable under order", holding(t, 3)->mId == id(2));
    }

    // Tile keys lie on the global lattice and a bridging object keeps them.
    template<> template<>
    void sknavcluster_object::test<4>()
    {
        std::vector<RecordPtr> recs = { box(1, 1, 30, 5, 0, 34, 6, 2), box(5, 5, 60, 5, 0, 64, 6, 2) };
        ClusterSet s = cluster(recs, 0.5);
        ensure_equals("two clusters", s.mClusters.size(), 2u);
        const Cluster* a = holding(s, 1);
        ensure("a spans tiles 0 and 1", a->mTiles.count({ 0, 0 }) == 1 && a->mTiles.count({ 1, 0 }) == 1);
        const U64 sigTile0 = a->mTiles.at({ 0, 0 });
        const U64 sigTile1 = a->mTiles.at({ 1, 0 });

        recs.push_back(box(3, 3, 34.2, 5, 0, 59.8, 6, 2));
        s = cluster(recs, 0.5);
        ensure_equals("bridged into one", s.mClusters.size(), 1u);
        const Cluster* m = holding(s, 1);
        ensure("merged id is the smallest root", m->mId == id(1));
        ensure_equals("tile 0 unchanged", m->mTiles.at({ 0, 0 }), sigTile0);
        ensure("tile 1 changed: the bridge is in it", m->mTiles.at({ 1, 0 }) != sigTile1);

        std::vector<ClusterPtr> hit;
        s.at(LLVector3d(62, 5.5, 1), hit);
        ensure_equals("lookup by position", hit.size(), 1u);
        hit.clear();
        s.at(LLVector3d(62, 40, 1), hit);
        ensure("miss outside the box", hit.empty());
    }
}
