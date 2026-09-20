/**
 * @file sknavlevels_test.cpp
 * @brief Storeys of a built mesh: one floor, two joined by a ramp, by stairs, and by a drop.
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

#include "../sknavbuild.h"
#include "../sknavlevels.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <cmath>
#include <cstring>

namespace
{
    using namespace sknav;

    const S32 AVATAR = static_cast<S32>(AgentClass::AVATAR);

    RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
    {
        auto r = std::make_shared<Record>();
        r->mShape = Shape::BOX;
        r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
        r->mAxes[0].set(1, 0, 0);
        r->mAxes[1].set(0, 1, 0);
        r->mAxes[2].set(0, 0, 1);
        r->mMin.set(x0, y0, z0);
        r->mMax.set(x1, y1, z1);
        return r;
    }

    /// A sloped deck: walkable from (x0, z0) up to (x1, z1), wound so that its
    /// face points up.
    RecordPtr ramp(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
    {
        auto tris = std::make_shared<std::vector<LLVector3>>();
        const LLVector3 a(x0, y0, z0), b(x1, y0, z1), c(x1, y1, z1), d(x0, y1, z0);
        tris->insert(tris->end(), { a, b, c, a, c, d });
        auto r = std::make_shared<Record>();
        r->mShape = Shape::TRIANGLES;
        r->mTriangles = tris;
        r->mMin.set(x0, y0, std::min(z0, z1));
        r->mMax.set(x1, y1, std::max(z0, z1));
        return r;
    }

    TerrainWindow flat(F32 z)
    {
        TerrainWindow t;
        t.mOrigin.set(-2.f, -2.f, 0.f);
        t.mSpacing = 1.f;
        t.mWidth = 37;
        t.mHeight = 37;
        t.mZ.assign(37 * 37, z);
        t.mWater = -100.f;
        return t;
    }

    TileBuildInput tile()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 16.f;
        in.mCell = 0.25f;
        in.mClasses[AVATAR] = true;
        in.mEdges[AVATAR] = true;
        in.mTerrain = flat(0.f);
        return in;
    }

    struct Built
    {
        dtNavMesh* mNav = nullptr;
        dtNavMeshQuery* mQuery = nullptr;
        Levels mLevels;

        explicit Built(const TileBuildInput& in)
        {
            const TileBuildResult r = buildTile(in);
            tut::ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));
            tut::ensure("polys", r.mPolys[AVATAR] > 0);

            mNav = dtAllocNavMesh();
            dtNavMeshParams p;
            p.orig[0] = p.orig[1] = p.orig[2] = 0.f;
            p.tileWidth = 32.f;
            p.tileHeight = 32.f;
            p.maxTiles = 1024;
            p.maxPolys = 4096;
            tut::ensure("mesh init", dtStatusSucceed(mNav->init(&p)));
            unsigned char* copy = static_cast<unsigned char*>(dtAlloc(static_cast<int>(r.mData[AVATAR].size()), DT_ALLOC_PERM));
            std::memcpy(copy, r.mData[AVATAR].data(), r.mData[AVATAR].size());
            tut::ensure("addTile", dtStatusSucceed(mNav->addTile(copy, static_cast<int>(r.mData[AVATAR].size()), DT_TILE_FREE_DATA, 0, nullptr)));
            mQuery = dtAllocNavMeshQuery();
            tut::ensure("query init", dtStatusSucceed(mQuery->init(mNav, 2048)));

            LevelParams params;
            buildLevels(*mNav, *mQuery, params, r.mLinks[AVATAR], mLevels);
        }
        ~Built()
        {
            dtFreeNavMeshQuery(mQuery);
            dtFreeNavMesh(mNav);
        }

        /// Storeys big enough to be storeys, in the order they were found.
        std::vector<S32> real() const
        {
            std::vector<S32> out;
            for (size_t i = 0; i < mLevels.mLevels.size(); ++i)
            {
                if (!mLevels.mLevels[i].mTransitional)
                {
                    out.push_back(static_cast<S32>(i));
                }
            }
            return out;
        }

        /// The storey under a point, or NO_LEVEL.
        U16 levelAt(const LLVector3& p) const
        {
            F32 centre[3], nearest[3];
            toRecast(p, centre);
            const F32 extents[3] = { 1.f, 2.f, 1.f };
            dtQueryFilter filter;
            dtPolyRef ref = 0;
            mQuery->findNearestPoly(centre, extents, &filter, &ref, nearest);
            return ref ? mLevels.levelOf(ref) : Levels::NO_LEVEL;
        }

        std::string dump() const
        {
            std::string out;
            for (size_t i = 0; i < mLevels.mLevels.size(); ++i)
            {
                const Level& l = mLevels.mLevels[i];
                out += llformat("; level %zu z=%.2f polys=%u x=%.1f..%.1f y=%.1f..%.1f%s", i, l.mZ, l.mPolys,
                                l.mMin.mV[VX], l.mMax.mV[VX], l.mMin.mV[VY], l.mMax.mV[VY],
                                l.mTransitional ? " transitional" : "");
            }
            for (const Connector& c : mLevels.mConnectors)
            {
                out += llformat("; %s %u->%u", c.mKind == ConnectorKind::CLIMB ? "climb" : "drop", c.mLow, c.mHigh);
            }
            return out;
        }

        S32 connectors(ConnectorKind kind) const
        {
            S32 n = 0;
            for (const Connector& c : mLevels.mConnectors)
            {
                n += c.mKind == kind ? 1 : 0;
            }
            return n;
        }
    };
}

namespace tut
{
    struct sknavlevels_data {};
    typedef test_group<sknavlevels_data> sknavlevels_group;
    typedef sknavlevels_group::object sknavlevels_object;
    tut::sknavlevels_group g("sknavlevels");

    // Flat ground is one storey and goes nowhere.
    template<> template<>
    void sknavlevels_object::test<1>()
    {
        const Built b(tile());
        ensure_equals(llformat("one storey, got %zu", b.real().size()), b.real().size(), 1u);
        ensure(llformat("no connectors, got %zu", b.mLevels.mConnectors.size()), b.mLevels.mConnectors.empty());
        ensure("the ground is that storey", b.levelAt(LLVector3(16.f, 16.f, 0.f)) != Levels::NO_LEVEL);
    }

    // A platform reached by a ramp is two storeys and a ramp between them.
    template<> template<>
    void sknavlevels_object::test<2>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(4.f, 4.f, 0.f, 20.f, 20.f, 3.f));
        in.mRecords.push_back(ramp(20.f, 8.f, 3.f, 27.f, 14.f, 0.f));
        const Built b(in);

        const std::vector<S32> real = b.real();
        ensure_equals("two storeys" + b.dump(), real.size(), 2u);
        const U16 ground = b.levelAt(LLVector3(28.f, 28.f, 0.f));
        const U16 top = b.levelAt(LLVector3(10.f, 10.f, 3.f));
        ensure("ground found", ground != Levels::NO_LEVEL);
        ensure("platform found", top != Levels::NO_LEVEL);
        ensure("they are different storeys", ground != top);
        ensure(llformat("platform is 3 m up: %.2f", b.mLevels.mLevels[top].mZ), std::fabs(b.mLevels.mLevels[top].mZ - 3.f) < 0.5f);
        ensure(llformat("a way up, got %d", b.connectors(ConnectorKind::CLIMB)), b.connectors(ConnectorKind::CLIMB) >= 1);
        for (const Connector& c : b.mLevels.mConnectors)
        {
            ensure("a connector runs uphill", b.mLevels.mLevels[c.mHigh].mZ > b.mLevels.mLevels[c.mLow].mZ);
        }
    }

    // Six steps of half a metre each. Every step is inside the climb height,
    // so a storey that chained small rises would swallow the whole staircase
    // and weld the two floors into one; banding keeps them apart.
    template<> template<>
    void sknavlevels_object::test<3>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(4.f, 4.f, 0.f, 20.f, 20.f, 3.f));
        for (S32 i = 0; i < 6; ++i)
        {
            const F32 x = 20.f + i;
            in.mRecords.push_back(box(x, 8.f, 0.f, x + 1.f, 14.f, 3.f - 0.5f * i));
        }
        const Built b(in);

        const U16 ground = b.levelAt(LLVector3(28.f, 28.f, 0.f));
        const U16 top = b.levelAt(LLVector3(10.f, 10.f, 3.f));
        ensure("ground found", ground != Levels::NO_LEVEL);
        ensure("upper floor found", top != Levels::NO_LEVEL);
        ensure("the staircase did not weld the floors", ground != top);
        ensure(llformat("upper floor is 3 m up: %.2f", b.mLevels.mLevels[top].mZ), std::fabs(b.mLevels.mLevels[top].mZ - 3.f) < 0.5f);
        ensure("a way up between them" + b.dump(), b.connectors(ConnectorKind::CLIMB) >= 1);
    }

    // With no way up, the only way between the platform and the ground is off
    // the edge, and that is a one-way connector.
    template<> template<>
    void sknavlevels_object::test<4>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 3.f));
        const Built b(in);

        const U16 ground = b.levelAt(LLVector3(28.f, 28.f, 0.f));
        const U16 top = b.levelAt(LLVector3(16.f, 16.f, 3.f));
        ensure("ground found", ground != Levels::NO_LEVEL);
        ensure("platform found", top != Levels::NO_LEVEL);
        ensure("two storeys", ground != top);
        ensure_equals(llformat("one drop, got %d", b.connectors(ConnectorKind::DROP)), b.connectors(ConnectorKind::DROP), 1);
        for (const Connector& c : b.mLevels.mConnectors)
        {
            if (c.mKind != ConnectorKind::DROP)
            {
                continue;
            }
            ensure("a drop is one way", c.mOneWay);
            ensure("it leaves the platform", c.mHigh == top);
            ensure("it lands on the ground", c.mLow == ground);
            ensure(llformat("it starts up high: %.2f", c.mHighPoint.mV[VZ]), c.mHighPoint.mV[VZ] > 2.5f);
        }
    }
}
