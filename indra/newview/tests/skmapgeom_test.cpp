/**
 * @file skmapgeom_test.cpp
 * @brief The mini-map scene: stitching, surfaces, components, layers, storey selection, floor relations.
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

#include "../skmapgeom.h"

#include "sknavbuild.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace sknav;
    using skmap::TileInput;

    const LLUUID LAND("11111111-1111-1111-1111-111111111111");
    const LLUUID CLUSTER("22222222-2222-2222-2222-222222222222");

    /// One tile's polygons, built by hand: quads that may slope along x.
    struct TileMaker
    {
        std::shared_ptr<TilePolys> mPolys = std::make_shared<TilePolys>();
        std::vector<Flight> mFlights;
        std::vector<WallSegment> mSegments;
        std::vector<DropLink> mLinks;

        /// Corners counter-clockwise from south-west; edges 0 south, 1 east, 2 north, 3 west.
        S32 quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 z_west, F32 z_east, U8 area = AREA_OBJECT)
        {
            TilePolys::Poly p;
            p.mArea = area;
            p.mVertCount = 4;
            const U16 base = static_cast<U16>(mPolys->mVerts.size());
            const LLVector3 v[4] = { LLVector3(x0, y0, z_west), LLVector3(x1, y0, z_east), LLVector3(x1, y1, z_east), LLVector3(x0, y1, z_west) };
            for (S32 k = 0; k < 4; ++k)
            {
                mPolys->mVerts.push_back(v[k]);
                p.mVerts[k] = base + k;
                p.mNeighbour[k] = TilePolys::EDGE_NONE;
            }
            p.mFirstTri = static_cast<U32>(mPolys->mTris.size() / 3);
            p.mTriCount = 2;
            const S32 tris[6] = { 0, 1, 2, 0, 2, 3 };
            for (S32 t : tris)
            {
                mPolys->mTris.push_back(v[t]);
            }
            p.mZMin = llmin(z_west, z_east);
            p.mZMax = llmax(z_west, z_east);
            mPolys->mPolys.push_back(p);
            return static_cast<S32>(mPolys->mPolys.size()) - 1;
        }
        void join(S32 a, S32 ea, S32 b, S32 eb)
        {
            mPolys->mPolys[a].mNeighbour[ea] = static_cast<S16>(b);
            mPolys->mPolys[b].mNeighbour[eb] = static_cast<S16>(a);
        }
        void border(S32 a, S32 e)
        {
            mPolys->mPolys[a].mNeighbour[e] = TilePolys::EDGE_BORDER;
        }
        void flight(ConnectorKind kind, U8 treads, const LLVector3& bottom_a, const LLVector3& bottom_b, const LLVector3& top_a,
                    const LLVector3& top_b, const std::vector<U16>& polys, U64 key)
        {
            Flight f;
            f.mKind = kind;
            f.mTreads = treads;
            f.mBottomA = bottom_a;
            f.mBottomB = bottom_b;
            f.mTopA = top_a;
            f.mTopB = top_b;
            f.mBottom = (bottom_a + bottom_b) * 0.5f;
            f.mTop = (top_a + top_b) * 0.5f;
            f.mRise = f.mTop.mV[VZ] - f.mBottom.mV[VZ];
            f.mRun = dist_vec(LLVector2(f.mBottom.mV[VX], f.mBottom.mV[VY]), LLVector2(f.mTop.mV[VX], f.mTop.mV[VY]));
            f.mFootprint = { LLVector2(bottom_a.mV[VX], bottom_a.mV[VY]), LLVector2(bottom_b.mV[VX], bottom_b.mV[VY]),
                             LLVector2(top_b.mV[VX], top_b.mV[VY]), LLVector2(top_a.mV[VX], top_a.mV[VY]) };
            f.mPolys = polys;
            f.mKey = key;
            mFlights.push_back(f);
        }
        TileInput make(const LLUUID& mesh, bool terrain, S32 kx, S32 ky) const
        {
            TileInput t;
            t.mMesh = mesh;
            t.mTerrain = terrain;
            t.mKey.mX = kx;
            t.mKey.mY = ky;
            t.mContentKey = 1000 + kx * 31 + ky;
            t.mMin.set(kx * 32.0, ky * 32.0, -10.0);
            t.mMax.set(kx * 32.0 + 32.0, ky * 32.0 + 32.0, 100.0);
            t.mPolys = mPolys;
            t.mSegments = std::make_shared<const std::vector<WallSegment>>(mSegments);
            t.mLinks = std::make_shared<const std::vector<DropLink>>(mLinks);
            t.mFlights = mFlights.empty() ? nullptr : std::make_shared<const std::vector<Flight>>(mFlights);
            return t;
        }
    };

    skmap::SceneInput input(const std::vector<TileInput>& tiles)
    {
        skmap::SceneInput in;
        in.mOrigin.set(0.0, 0.0, 0.0);
        in.mMin.set(-64.0, -64.0, -1.0e4);
        in.mMax.set(128.0, 128.0, 1.0e5);
        in.mTiles = tiles;
        in.mRegions.push_back({ LLVector3d(0.0, 0.0, 0.0), 256.0 });
        return in;
    }

    S32 layerOf(const skmap::Scene& s, const LLVector3d& at)
    {
        const skmap::FloorHit hit = skmap::floorAt(s, at, 0.5f, 0.5f);
        return hit.mFound ? hit.mLayer : -1;
    }

    skmap::AgentFloor agentAt(const skmap::Scene& s, const LLVector3d& at)
    {
        skmap::AgentFloor a;
        a.mPos = at;
        const skmap::FloorHit hit = skmap::floorAt(s, at, 2.5f, 0.5f);
        a.mFound = hit.mFound;
        a.mLayer = hit.mLayer;
        a.mZ = hit.mZ;
        return a;
    }

    /// A building on the ground: land 0..32 at z 0, a storey over 8..16 at z 6 nothing joins,
    /// a neighbour's storey over 24..30 at z 6, and a stair up to a mezzanine at z 3.
    skmap::ScenePtr building(bool with_mezzanine)
    {
        TileMaker t;
        const S32 land = t.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f, AREA_GROUND);
        t.quad(8.f, 8.f, 16.f, 16.f, 6.f, 6.f);                 // the storey above, a roof joined by nothing
        t.quad(24.f, 24.f, 30.f, 30.f, 6.f, 6.f);               // a neighbour's upper floor
        if (with_mezzanine)
        {
            const S32 stair = t.quad(10.f, 9.f, 12.f, 11.f, 0.f, 3.f);
            const S32 mezz = t.quad(12.f, 9.f, 15.f, 11.f, 3.f, 3.f);
            t.join(stair, 3, land, 0);
            t.join(stair, 1, mezz, 3);
            t.flight(ConnectorKind::STAIR, 6, LLVector3(10.f, 9.f, 0.f), LLVector3(10.f, 11.f, 0.f), LLVector3(12.f, 9.f, 3.f),
                     LLVector3(12.f, 11.f, 3.f), { static_cast<U16>(stair) }, 77);
        }
        // A drop off the roof onto the land: drops never join.
        DropLink drop;
        drop.mFrom.set(16.f, 12.f, 6.f);
        drop.mTo.set(16.5f, 12.f, 0.f);
        drop.mHeight = 6.f;
        t.mLinks.push_back(drop);
        return skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
    }
}

namespace tut
{
    struct skmapgeom_data {};
    typedef test_group<skmapgeom_data> skmapgeom_group;
    typedef skmapgeom_group::object skmapgeom_object;
    tut::skmapgeom_group g("skmapgeom");

    // Two tiles stitch across a lattice line into one surface; a 1 m step does not.
    template<> template<>
    void skmapgeom_object::test<1>()
    {
        TileMaker a, b;
        a.border(a.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f, AREA_GROUND), 1);
        b.border(b.quad(32.f, 0.f, 64.f, 32.f, 0.f, 0.f, AREA_GROUND), 3);
        skmap::ScenePtr s = skmap::buildScene(input({ a.make(LAND, true, 0, 0), b.make(LAND, true, 1, 0) }));
        ensure_equals("one surface", s->mSurfaces.size(), static_cast<size_t>(1));
        ensure_equals("one layer", s->mOrder.size(), static_cast<size_t>(1));
        ensure("a ground layer", s->mLayers[s->mOrder[0]].mGround);

        TileMaker c;
        c.border(c.quad(32.f, 0.f, 64.f, 32.f, 1.f, 1.f, AREA_GROUND), 3);
        s = skmap::buildScene(input({ a.make(LAND, true, 0, 0), c.make(LAND, true, 1, 0) }));
        ensure_equals("a 1 m step does not stitch", s->mSurfaces.size(), static_cast<size_t>(2));
        ensure("the stitch steps", !s->mStitches.empty() && s->mStitches.front().mStep);
    }

    // A ramp-bridged two-storey build gives at least two layers in one component.
    template<> template<>
    void skmapgeom_object::test<2>()
    {
        TileMaker t;
        const S32 lower = t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f);
        const S32 ramp = t.quad(10.f, 0.f, 20.f, 10.f, 0.f, 3.f);
        const S32 upper = t.quad(20.f, 0.f, 30.f, 10.f, 3.f, 3.f);
        t.join(lower, 1, ramp, 3);
        t.join(ramp, 1, upper, 3);
        t.flight(ConnectorKind::RAMP, 0, LLVector3(10.f, 0.f, 0.f), LLVector3(10.f, 10.f, 0.f), LLVector3(20.f, 0.f, 3.f),
                 LLVector3(20.f, 10.f, 3.f), { static_cast<U16>(ramp) }, 5);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure("two layers", s->mOrder.size() >= 2);
        ensure_equals("one component", s->mComponents, 1);
        ensure_equals("one run", s->mRuns.size(), static_cast<size_t>(1));
        ensure("a ramp", !s->mRuns[0].mStair);
        ensure("bottom layer", s->mRuns[0].mLayer == layerOf(*s, LLVector3d(5.0, 5.0, 0.0)));
        ensure("top layer", s->mRuns[0].mTopLayer == layerOf(*s, LLVector3d(25.0, 5.0, 3.0)));
        ensure("layers linked", !s->mLayerLinks.empty());
    }

    // A stair run joins a component; a drop link alone does not.
    template<> template<>
    void skmapgeom_object::test<3>()
    {
        TileMaker t;
        const S32 lower = t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f);
        const S32 stair = t.quad(10.f, 0.f, 14.f, 10.f, 0.f, 3.f);
        const S32 upper = t.quad(14.f, 0.f, 24.f, 10.f, 3.f, 3.f);
        t.join(lower, 1, stair, 3);
        t.join(stair, 1, upper, 3);
        t.flight(ConnectorKind::STAIR, 6, LLVector3(10.f, 0.f, 0.f), LLVector3(10.f, 10.f, 0.f), LLVector3(14.f, 0.f, 3.f),
                 LLVector3(14.f, 10.f, 3.f), { static_cast<U16>(stair) }, 9);
        // Elsewhere, two floors joined only by a drop.
        t.quad(0.f, 20.f, 10.f, 30.f, 0.f, 0.f);
        t.quad(10.f, 20.f, 20.f, 30.f, 3.f, 3.f);
        DropLink drop;
        drop.mFrom.set(10.f, 25.f, 3.f);
        drop.mTo.set(9.5f, 25.f, 0.f);
        t.mLinks.push_back(drop);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure_equals("a stair run", s->mRuns.size(), static_cast<size_t>(1));
        ensure("it is a stair", s->mRuns[0].mStair);
        // The stair's floors share a component; the dropped pair are two.
        const skmap::ScenePoly& a = s->mPolys[s->mSurfaces[s->mPolys[0].mSurface].mPolys.front()];
        ensure_equals("three components", s->mComponents, 3);
        ensure("stair floors joined", s->mSurfaces[s->mPolys[0].mSurface].mComponent == s->mSurfaces[s->mPolys[2].mSurface].mComponent);
        ensure("drop floors apart", s->mSurfaces[s->mPolys[3].mSurface].mComponent != s->mSurfaces[s->mPolys[4].mSurface].mComponent);
        ensure("poly kept", a.mVertCount == 4);
    }

    // A table top 0.9 m over a floor with a touching footprint is part of its layer; two
    // separate second floors are two layers.
    template<> template<>
    void skmapgeom_object::test<4>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 8.f, 8.f, 10.f, 10.f);
        t.quad(8.5f, 2.f, 10.f, 4.f, 10.9f, 10.9f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure_equals("two surfaces", s->mSurfaces.size(), static_cast<size_t>(2));
        ensure_equals("one layer", s->mOrder.size(), static_cast<size_t>(1));

        TileMaker u;
        u.quad(0.f, 0.f, 6.f, 6.f, 4.f, 4.f);
        u.quad(20.f, 20.f, 26.f, 26.f, 4.f, 4.f);
        s = skmap::buildScene(input({ u.make(CLUSTER, false, 0, 0) }));
        ensure_equals("two layers", s->mOrder.size(), static_cast<size_t>(2));
    }

    // Ground first, then ascending base height, the larger footprint later on a tie.
    template<> template<>
    void skmapgeom_object::test<5>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f, AREA_GROUND);
        t.quad(0.f, 0.f, 4.f, 4.f, -6.f, -6.f);                 // a cellar below the ground
        t.quad(20.f, 20.f, 30.f, 30.f, 5.f, 5.f);               // large, at 5 m
        t.quad(2.f, 20.f, 6.f, 24.f, 5.f, 5.f);                 // small, at 5 m, far from the large one
        t.quad(10.f, 0.f, 14.f, 4.f, 8.f, 8.f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
        ensure_equals("five layers", s->mOrder.size(), static_cast<size_t>(5));
        ensure("ground first", s->mLayers[s->mOrder[0]].mGround);
        for (size_t i = 2; i < s->mOrder.size(); ++i)
        {
            ensure("ascending", s->mLayers[s->mOrder[i - 1]].mBaseZ <= s->mLayers[s->mOrder[i]].mBaseZ);
        }
        ensure("cellar after ground", std::fabs(s->mLayers[s->mOrder[1]].mBaseZ + 6.f) < 1e-3f);
        ensure("tie: smaller first", s->mLayers[s->mOrder[2]].mArea < s->mLayers[s->mOrder[3]].mArea);
    }

    // Storeys: a roof 12 m up makes inside; a roof elsewhere stays drawn.
    template<> template<>
    void skmapgeom_object::test<6>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f, AREA_GROUND);
        t.quad(8.f, 8.f, 16.f, 16.f, 12.f, 12.f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
        const skmap::AgentFloor under = agentAt(*s, LLVector3d(12.0, 12.0, 0.0));
        ensure("agent on the ground", under.mFound);
        skmap::LayerSelection sel = skmap::selectLayers(*s, under, EnclosureBucket::UNKNOWN, 30.f);
        ensure("a roof overhead makes inside", sel.mInside);
        ensure("the roof is cut over the agent", !sel.mCut.empty());

        const skmap::AgentFloor away = agentAt(*s, LLVector3d(28.0, 28.0, 0.0));
        sel = skmap::selectLayers(*s, away, EnclosureBucket::UNKNOWN, 30.f);
        ensure("outside", !sel.mInside);
        ensure_equals("both drawn", sel.mDrawn.size(), static_cast<size_t>(2));
        ensure("nothing cut", sel.mCut.empty());
        sel = skmap::selectLayers(*s, away, EnclosureBucket::UNKNOWN, 10.f);
        ensure_equals("past the fade the roof is skipped", sel.mDrawn.size(), static_cast<size_t>(1));
    }

    // Inside: the storey above over the interior is cut, the neighbour's is kept, a
    // drop-only roof is cut and a stair-connected mezzanine within 5 m is drawn.
    template<> template<>
    void skmapgeom_object::test<7>()
    {
        skmap::ScenePtr s = building(true);
        const skmap::AgentFloor agent = agentAt(*s, LLVector3d(13.0, 13.0, 0.0));
        ensure("agent on the ground", agent.mFound);
        const skmap::LayerSelection sel = skmap::selectLayers(*s, agent, EnclosureBucket::INDOORS, 30.f);
        ensure("inside", sel.mInside);
        const S32 roof = layerOf(*s, LLVector3d(9.0, 15.0, 6.0));
        const S32 neighbour = layerOf(*s, LLVector3d(27.0, 27.0, 6.0));
        const S32 mezzanine = layerOf(*s, LLVector3d(14.0, 10.0, 3.0));
        ensure("three layers found", roof >= 0 && neighbour >= 0 && mezzanine >= 0);
        ensure("the roof over the interior is cut", sel.mCut.count(roof) > 0);
        ensure("the neighbour's floor is kept", sel.mCut.count(neighbour) == 0);
        ensure("the mezzanine is on the storey", sel.mStorey.count(mezzanine) > 0);
        ensure("the mezzanine is not cut", sel.mCut.count(mezzanine) == 0);
        ensure("every layer drawn", sel.mDrawn.size() == s->mOrder.size());

        // Without the stair the roof is still joined to nothing but a drop: still cut.
        skmap::ScenePtr bare = building(false);
        const skmap::AgentFloor bare_agent = agentAt(*bare, LLVector3d(13.0, 13.0, 0.0));
        const skmap::LayerSelection bare_sel = skmap::selectLayers(*bare, bare_agent, EnclosureBucket::INDOORS, 30.f);
        ensure("a drop-only roof is cut", bare_sel.mCut.count(layerOf(*bare, LLVector3d(9.0, 15.0, 6.0))) > 0);
    }

    // Floor relations: same, above, below, the raw fallback and unknown for coarse avatars.
    template<> template<>
    void skmapgeom_object::test<8>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f, AREA_GROUND);
        t.quad(0.f, 0.f, 10.f, 10.f, 5.f, 5.f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
        const skmap::AgentFloor ground = agentAt(*s, LLVector3d(5.0, 5.0, 0.2));
        const skmap::AgentFloor upstairs = agentAt(*s, LLVector3d(5.0, 5.0, 5.2));
        ensure("found", ground.mFound && upstairs.mFound && ground.mLayer != upstairs.mLayer);
        ensure("above", skmap::floorRelation(*s, ground, LLVector3d(5.0, 5.0, 5.3), true) == skmap::FloorRelation::ABOVE);
        ensure("same", skmap::floorRelation(*s, ground, LLVector3d(20.0, 20.0, 0.3), true) == skmap::FloorRelation::SAME);
        ensure("below", skmap::floorRelation(*s, upstairs, LLVector3d(20.0, 20.0, 0.3), true) == skmap::FloorRelation::BELOW);
        ensure("raw above", skmap::floorRelation(*s, ground, LLVector3d(100.0, 100.0, 10.0), true) == skmap::FloorRelation::ABOVE);
        ensure("raw same", skmap::floorRelation(*s, ground, LLVector3d(100.0, 100.0, 1.0), true) == skmap::FloorRelation::SAME);
        ensure("raw below", skmap::floorRelation(*s, upstairs, LLVector3d(100.0, 100.0, 1.0), true) == skmap::FloorRelation::BELOW);
        ensure("coarse unknown", skmap::floorRelation(*s, ground, LLVector3d(5.0, 5.0, 5.3), false) == skmap::FloorRelation::UNKNOWN);
    }

    // A polygon outside every run spanning more than 4 m in height is dropped.
    template<> template<>
    void skmapgeom_object::test<9>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 4.f, 4.f, 0.f, 6.f);
        t.quad(10.f, 0.f, 14.f, 4.f, 0.f, 0.f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure_equals("tall polygon dropped", s->mPolys.size(), static_cast<size_t>(1));
    }

    WallSegment segment(F32 x0, F32 y0, F32 x1, F32 y1, EdgeKind kind, F32 value, F32 z = 0.f)
    {
        WallSegment w;
        w.mA.set(x0, y0, z);
        w.mB.set(x1, y1, z);
        w.mKind = kind;
        w.mValue = value;
        return w;
    }

    DropLink dropFrom(F32 x, F32 y, F32 z, F32 tx, F32 ty, F32 tz)
    {
        DropLink d;
        d.mFrom.set(x, y, z);
        d.mTo.set(tx, ty, tz);
        d.mHeight = z - tz;
        return d;
    }

    // Marks: walls and ledges on their floor with the floor side; OPEN is never a mark.
    template<> template<>
    void skmapgeom_object::test<10>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f);
        t.mSegments.push_back(segment(10.f, 0.f, 10.f, 10.f, EdgeKind::WALL, 2.f));
        t.mSegments.push_back(segment(0.f, 0.f, 10.f, 0.f, EdgeKind::OPEN, 0.f));
        t.mSegments.push_back(segment(10.f, 10.f, 0.f, 10.f, EdgeKind::LEDGE, 3.f));
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure_equals("two marks", s->mMarks.size(), static_cast<size_t>(2));
        ensure("a wall", s->mMarks[0].mKind == skmap::MarkKind::WALL && s->mMarks[0].mHeight == 2.f);
        ensure_equals("floor to the left", static_cast<S32>(s->mMarks[0].mFloorSide), 1);
        ensure("a ledge", s->mMarks[1].mKind == skmap::MarkKind::LEDGE);
        ensure("on the floor's layer", s->mMarks[0].mLayer == s->mPolys[0].mLayer);
        ensure_equals("the layer lists them", s->mLayers[s->mPolys[0].mLayer].mMarks.size(), static_cast<size_t>(2));
        ensure("built set", s->mBuilt.count({ 0, 0 }) == 1 && s->mBuilt.size() == 1);
    }

    // Drops: links within 0.3 m make their segment a drop edge, once; a link with no segment
    // is a drop point.
    template<> template<>
    void skmapgeom_object::test<11>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f);
        t.mSegments.push_back(segment(10.f, 10.f, 0.f, 10.f, EdgeKind::LEDGE, 3.f));
        t.mLinks.push_back(dropFrom(3.f, 10.2f, 0.f, 3.f, 11.f, -3.f));
        t.mLinks.push_back(dropFrom(6.f, 9.9f, 0.f, 6.f, 11.f, -3.f));
        t.mLinks.push_back(dropFrom(5.f, 5.f, 0.f, 5.f, 4.f, -3.f));
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        S32 edges = 0, points = 0, ledges = 0;
        for (const skmap::LineMark& m : s->mMarks)
        {
            edges += m.mKind == skmap::MarkKind::DROP_EDGE;
            points += m.mKind == skmap::MarkKind::DROP_POINT;
            ledges += m.mKind == skmap::MarkKind::LEDGE;
        }
        ensure_equals("one drop edge", edges, 1);
        ensure_equals("the ledge became it", ledges, 0);
        ensure_equals("one drop point", points, 1);
    }

    // Contours at every 0.5 m on a terrain slope and a ramp, none on a stair run.
    template<> template<>
    void skmapgeom_object::test<12>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 3.f, AREA_GROUND);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
        std::set<S32> levels;
        for (const skmap::Contour& c : s->mContours)
        {
            levels.insert(c.mLevel);
        }
        ensure("levels 0.5 to 2.5", levels == std::set<S32>({ 1, 2, 3, 4, 5 }));

        auto runScene = [](ConnectorKind kind, U8 treads) {
            TileMaker u;
            const S32 lower = u.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f);
            const S32 run = u.quad(10.f, 0.f, 14.f, 10.f, 0.f, 3.f);
            const S32 upper = u.quad(14.f, 0.f, 24.f, 10.f, 3.f, 3.f);
            u.join(lower, 1, run, 3);
            u.join(run, 1, upper, 3);
            u.flight(kind, treads, LLVector3(10.f, 0.f, 0.f), LLVector3(10.f, 10.f, 0.f), LLVector3(14.f, 0.f, 3.f),
                     LLVector3(14.f, 10.f, 3.f), { static_cast<U16>(run) }, 9);
            return skmap::buildScene(input({ u.make(CLUSTER, false, 0, 0) }));
        };
        ensure("none on a stair", runScene(ConnectorKind::STAIR, 6)->mContours.empty());
        ensure("on a ramp", !runScene(ConnectorKind::RAMP, 0)->mContours.empty());
        ensure("none on flat object floors", runScene(ConnectorKind::STAIR, 6)->mContours.empty());
    }

    // Region edges: marked only where floors come near, void with nothing across, a dashed
    // region edge once where another region lies across.
    template<> template<>
    void skmapgeom_object::test<13>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 10.f, 10.f, 0.f, 0.f, AREA_GROUND);
        skmap::SceneInput in = input({ t.make(LAND, true, 0, 0) });
        skmap::ScenePtr s = skmap::buildScene(in);
        ensure("some boundaries", !s->mBoundaries.empty());
        for (const skmap::LineMark& m : s->mBoundaries)
        {
            ensure("void", m.mKind == skmap::MarkKind::VOID_BOUNDARY);
            ensure("only near the floor", m.mA.mV[VX] <= 12.f && m.mA.mV[VY] <= 12.f && m.mB.mV[VX] <= 12.f && m.mB.mV[VY] <= 12.f);
            ensure("no layer", m.mLayer == -1);
        }

        in.mRegions.push_back({ LLVector3d(-256.0, 0.0, 0.0), 256.0 });
        s = skmap::buildScene(in);
        S32 region = 0, west_void = 0;
        for (const skmap::LineMark& m : s->mBoundaries)
        {
            const bool west = std::fabs(m.mA.mV[VX]) < 1e-3f && std::fabs(m.mB.mV[VX]) < 1e-3f;
            region += m.mKind == skmap::MarkKind::REGION_BOUNDARY && west;
            west_void += m.mKind == skmap::MarkKind::VOID_BOUNDARY && west;
        }
        ensure_equals("one region edge", region, 1);
        ensure_equals("no void across it", west_void, 0);
    }

    // Coverage is sampled a metre over each polygon's centroid.
    template<> template<>
    void skmapgeom_object::test<14>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 4.f, 4.f, 0.f, 0.f);
        t.quad(20.f, 0.f, 24.f, 4.f, 0.f, 0.f);
        skmap::SceneInput in = input({ t.make(CLUSTER, false, 0, 0) });
        in.mCoverage = [](const LLVector3d& p) {
            return p.mdV[VX] < 10.0 && std::fabs(p.mdV[VZ] - 1.0) < 1e-3 ? EnclosureBucket::INDOORS : EnclosureBucket::OUTDOORS;
        };
        skmap::ScenePtr s = skmap::buildScene(in);
        ensure("indoors", s->mPolys[0].mCoverage == static_cast<U8>(EnclosureBucket::INDOORS));
        ensure("outdoors", s->mPolys[1].mCoverage == static_cast<U8>(EnclosureBucket::OUTDOORS));
    }
}
