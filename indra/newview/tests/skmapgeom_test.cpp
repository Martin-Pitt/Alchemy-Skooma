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
#include <array>
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

    skmap::SceneInput input(const std::vector<TileInput>& tiles, const std::vector<RecordPtr>* records = nullptr)
    {
        skmap::SceneInput in;
        in.mOrigin.set(0.0, 0.0, 0.0);
        in.mMin.set(-64.0, -64.0, -1.0e4);
        in.mMax.set(128.0, 128.0, 1.0e5);
        in.mTiles = tiles;
        in.mRegions.push_back({ LLVector3d(0.0, 0.0, 0.0), 256.0 });
        if (records)
        {
            in.mRecords = std::make_shared<const std::vector<RecordPtr>>(*records);
        }
        return in;
    }

    LLUUID partId(U64 signature)
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "00000000-0000-0000-0000-%012llx", static_cast<unsigned long long>(signature));
        return LLUUID(buf);
    }

    /// A census box at region-local metres, its AABB from its corners.
    Record boxRecordOf(const LLVector3& c, const LLVector3& h, U64 signature, const LLVector3& a0 = LLVector3(1.f, 0.f, 0.f),
                       const LLVector3& a1 = LLVector3(0.f, 1.f, 0.f), const LLVector3& a2 = LLVector3(0.f, 0.f, 1.f))
    {
        Record r;
        r.mRole = Role::NAV;
        r.mShape = Shape::BOX;
        r.mSolid = true;
        r.mCenter = c;
        r.mHalf = h;
        r.mAxes[0] = a0;
        r.mAxes[1] = a1;
        r.mAxes[2] = a2;
        r.mMin.set(DBL_MAX, DBL_MAX, DBL_MAX);
        r.mMax.set(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (S32 i = 0; i < 8; ++i)
        {
            LLVector3 p = c;
            for (S32 b = 0; b < 3; ++b)
            {
                p += r.mAxes[b] * (((i >> b) & 1) ? h.mV[b] : -h.mV[b]);
            }
            for (S32 k = 0; k < 3; ++k)
            {
                r.mMin.mdV[k] = llmin(r.mMin.mdV[k], static_cast<F64>(p.mV[k]));
                r.mMax.mdV[k] = llmax(r.mMax.mdV[k], static_cast<F64>(p.mV[k]));
            }
        }
        r.mSignature = signature;
        r.mPart = partId(signature);
        r.mRoot = partId(signature + 1000000);
        return r;
    }

    RecordPtr boxRecord(const LLVector3& c, const LLVector3& h, U64 signature)
    {
        return std::make_shared<const Record>(boxRecordOf(c, h, signature));
    }

    std::vector<const skmap::SectionPiece*> piecesOf(const skmap::Scene& s, U64 signature, S32 plane = INT_MIN)
    {
        std::vector<const skmap::SectionPiece*> out;
        for (const skmap::SectionPiece& p : s.mPieces)
        {
            if (p.mSource == signature && (plane == INT_MIN || p.mPlane == plane))
            {
                out.push_back(&p);
            }
        }
        return out;
    }

    std::vector<S32> levelsAt(const skmap::LevelGrid& g, S32 gx, S32 gy)
    {
        std::vector<S32> out;
        const S32 x = gx - g.mX0, y = gy - g.mY0;
        if (x < 0 || x >= g.mW || y < 0 || y >= g.mH)
        {
            return out;
        }
        const U32 c = g.mCell[static_cast<size_t>(y) * g.mW + x];
        if (c)
        {
            out.assign(g.mLists.begin() + c, g.mLists.begin() + c + g.mLists[c - 1]);
        }
        return out;
    }

    bool samePieces(const skmap::Scene& a, const skmap::Scene& b)
    {
        if (a.mPieces.size() != b.mPieces.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.mPieces.size(); ++i)
        {
            const skmap::SectionPiece& p = a.mPieces[i];
            const skmap::SectionPiece& q = b.mPieces[i];
            if (p.mSource != q.mSource || p.mPlane != q.mPlane || p.mLoops != q.mLoops || p.mWind != q.mWind || p.mLines != q.mLines
                || p.mFlags != q.mFlags || p.mGroup != q.mGroup || p.mStyle != q.mStyle || p.mShift != q.mShift || p.mMin != q.mMin
                || p.mMax != q.mMax || p.mSlice->mPts.size() != q.mSlice->mPts.size()
                || (!p.mSlice->mPts.empty()
                    && std::memcmp(p.mSlice->mPts.data(), q.mSlice->mPts.data(), p.mSlice->mPts.size() * sizeof(LLVector2)) != 0))
            {
                return false;
            }
        }
        return true;
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
    skmap::SceneInput buildingInput(bool with_mezzanine)
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
        return input({ t.make(LAND, true, 0, 0) });
    }

    skmap::ScenePtr building(bool with_mezzanine)
    {
        return skmap::buildScene(buildingInput(with_mezzanine));
    }

    /// The same input moved whole: meshes, tiles, the analysis box and the regions.
    skmap::SceneInput shifted(skmap::SceneInput in, const LLVector3d& by)
    {
        in.mOrigin += by;
        in.mMin += by;
        in.mMax += by;
        for (TileInput& t : in.mTiles)
        {
            t.mOrigin += by;
            t.mMin += by;
            t.mMax += by;
        }
        for (skmap::RegionSquare& r : in.mRegions)
        {
            r.mOrigin += by;
        }
        return in;
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

    // A polygon outside every run spanning more than 100 m in height is a glitch and dropped.
    template<> template<>
    void skmapgeom_object::test<9>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 4.f, 4.f, 0.f, 150.f);
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

    /// A 32 m tile of 4 m terrain quads, all joined, with a wall along x = 16.
    TileMaker grid()
    {
        TileMaker t;
        for (S32 y = 0; y < 8; ++y)
        {
            for (S32 x = 0; x < 8; ++x)
            {
                const S32 id = t.quad(x * 4.f, y * 4.f, x * 4.f + 4.f, y * 4.f + 4.f, 0.f, 0.f, AREA_GROUND);
                if (x > 0)
                {
                    t.join(id - 1, 1, id, 3);
                }
                if (y > 0)
                {
                    t.join(id - 8, 2, id, 0);
                }
            }
        }
        t.quad(40.f, 40.f, 44.f, 44.f, 6.f, 6.f);
        t.mSegments.push_back(segment(44.f, 40.f, 44.f, 44.f, EdgeKind::WALL, 2.f, 6.f));
        return t;
    }

    // Hashes: the same input gives the same keys, a changed polygon moves only the leaves
    // within 2 m of it (and their ancestors), and a layer's mark hash ignores other layers.
    template<> template<>
    void skmapgeom_object::test<15>()
    {
        const TileMaker t = grid();
        skmap::SceneInput in = input({ t.make(LAND, true, 0, 0), });
        in.mTiles.front().mMax.set(64.0, 64.0, 100.0);
        skmap::ScenePtr a = skmap::buildScene(in);
        skmap::ScenePtr b = skmap::buildScene(in);
        ensure("deterministic hashes", a->mCellHashes == b->mCellHashes && a->mMarkHashes == b->mMarkHashes);
        const S32 ground = layerOf(*a, LLVector3d(2.0, 2.0, 0.0));
        ensure("ground layer", ground >= 0);
        ensure("a leaf key", skmap::cellKey(*a, ground, skmap::cellAt(0, 2.0, 2.0)) != 0);
        ensure("a 512 m key", skmap::cellKey(*a, ground, skmap::cellAt(6, 2.0, 2.0)) != 0);
        ensure("nothing far away", skmap::cellKey(*a, ground, skmap::cellAt(0, 300.0, 300.0)) == 0);

        // One polygon, (12..16, 12..16), turns indoors.
        in.mCoverage = [](const LLVector3d& p) {
            return p.mdV[VX] > 12.0 && p.mdV[VX] < 16.0 && p.mdV[VY] > 12.0 && p.mdV[VY] < 16.0 ? EnclosureBucket::INDOORS
                                                                                                    : EnclosureBucket::UNKNOWN;
        };
        skmap::ScenePtr c = skmap::buildScene(in);
        const std::map<skmap::CellId, U64>& before = a->mCellHashes[ground];
        const std::map<skmap::CellId, U64>& after = c->mCellHashes[layerOf(*c, LLVector3d(2.0, 2.0, 0.0))];
        S32 changed = 0;
        for (const auto& entry : before)
        {
            if (entry.first.mTier != 0)
            {
                continue;
            }
            const auto it = after.find(entry.first);
            ensure("leaf kept", it != after.end());
            if (it->second == entry.second)
            {
                continue;
            }
            ++changed;
            const LLVector3d lo = entry.first.minCorner(), hi = entry.first.maxCorner();
            ensure("changed only within 2 m", lo.mdV[VX] <= 16.0 + 2.0 && hi.mdV[VX] >= 12.0 - 2.0 && lo.mdV[VY] <= 16.0 + 2.0
                                                  && hi.mdV[VY] >= 12.0 - 2.0);
        }
        ensure("the leaves around it changed", changed > 0);
        ensure("a far leaf kept its key", before.at(skmap::cellAt(0, 28.0, 28.0)) == after.at(skmap::cellAt(0, 28.0, 28.0)));

        // The deck's mark hash does not move when only the ground gains a wall.
        TileMaker more = grid();
        more.mSegments.push_back(segment(0.f, 32.f, 0.f, 0.f, EdgeKind::WALL, 2.f, 0.f));
        skmap::SceneInput in2 = input({ more.make(LAND, true, 0, 0) });
        in2.mTiles.front().mMax.set(64.0, 64.0, 100.0);
        skmap::ScenePtr d = skmap::buildScene(in2);
        const S32 deck_a = layerOf(*a, LLVector3d(42.0, 42.0, 6.0));
        const S32 deck_d = layerOf(*d, LLVector3d(42.0, 42.0, 6.0));
        const S32 ground_d = layerOf(*d, LLVector3d(2.0, 2.0, 0.0));
        ensure("deck found", deck_a >= 0 && deck_d >= 0);
        ensure("the deck's mark hash holds", a->mMarkHashes[deck_a] == d->mMarkHashes[deck_d]);
        ensure("the ground's moved", a->mMarkHashes[ground] != d->mMarkHashes[ground_d]);
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

    // Every cell has its own key: leaves with the same records, outside and inside one quad's
    // edge or deep inside it, never share a bake.
    template<> template<>
    void skmapgeom_object::test<16>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 32.f, 32.f, 0.f, 0.f);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        const S32 l = layerOf(*s, LLVector3d(16.0, 16.0, 0.0));
        ensure("one layer", l >= 0);
        const U64 outside = skmap::cellKey(*s, l, { 0, -1, 1 });
        const U64 edge = skmap::cellKey(*s, l, { 0, 0, 1 });
        const U64 inner_a = skmap::cellKey(*s, l, { 0, 1, 1 });
        const U64 inner_b = skmap::cellKey(*s, l, { 0, 2, 1 });
        ensure("all four keyed", outside && edge && inner_a && inner_b);
        ensure("outside and inside the edge differ", outside != edge);
        ensure("two inner leaves differ", inner_a != inner_b);
        ensure("two parents differ", skmap::cellKey(*s, l, { 1, 0, 0 }) != skmap::cellKey(*s, l, { 1, 1, 0 }));
    }

    /// Terrain tiles in a row along x, each a sloped 32 m quad stitched to its neighbours: the
    /// first two flat at 0, then 0 to 3.5, 3.5 to 7 and 7 to 10.5. With ramp, tile 0 also holds
    /// a ramp from the land at (8, 8) up to a deck at 1.5 m.
    skmap::SceneInput terrainRow(S32 tiles, bool ramp, const LLVector3d& origin)
    {
        static const F32 Z[5][2] = { { 0.f, 0.f }, { 0.f, 0.f }, { 0.f, 3.5f }, { 3.5f, 7.f }, { 7.f, 10.5f } };
        std::vector<TileInput> row;
        for (S32 k = 0; k < tiles; ++k)
        {
            TileMaker t;
            const S32 land = t.quad(k * 32.f, 0.f, k * 32.f + 32.f, 32.f, Z[k][0], Z[k][1], AREA_GROUND);
            t.border(land, 1);
            t.border(land, 3);
            if (ramp && k == 0)
            {
                const S32 slope = t.quad(8.f, 8.f, 12.f, 10.f, 0.f, 1.5f);
                const S32 deck = t.quad(12.f, 8.f, 16.f, 10.f, 1.5f, 1.5f);
                t.join(slope, 3, land, 0);
                t.join(slope, 1, deck, 3);
                t.flight(ConnectorKind::RAMP, 0, LLVector3(8.f, 8.f, 0.f), LLVector3(8.f, 10.f, 0.f), LLVector3(12.f, 8.f, 1.5f),
                         LLVector3(12.f, 10.f, 1.5f), { static_cast<U16>(slope) }, 91);
            }
            row.push_back(t.make(LAND, true, k, 0));
        }
        skmap::SceneInput in = input(row);
        in.mOrigin = origin;
        in.mMin.set(-64.0, -64.0, -1.0e4);
        in.mMax.set(192.0, 128.0, 1.0e5);
        return in;
    }

    // A scene built from another origin over the same tiles gives every node the same key.
    template<> template<>
    void skmapgeom_object::test<17>()
    {
        skmap::SceneInput in = terrainRow(3, true, LLVector3d(0.0, 0.0, 0.0));
        in.mTiles.front().mSegments =
            std::make_shared<const std::vector<WallSegment>>(std::vector<WallSegment>{ segment(0.f, 32.f, 0.f, 0.f, EdgeKind::WALL, 2.f) });
        skmap::ScenePtr a = skmap::buildScene(in);
        in.mOrigin.set(-64.0, -64.0, 0.0);
        skmap::ScenePtr b = skmap::buildScene(in);
        ensure("some runs", !a->mRuns.empty());
        ensure("some marks", !a->mMarks.empty());
        ensure_equals("same layers", a->mLayers.size(), b->mLayers.size());
        ensure("same mark hashes", a->mMarkHashes == b->mMarkHashes);
        ensure("same cell keys", a->mCellHashes == b->mCellHashes);
    }

    // A box step that moves the ground's median keeps every ground leaf away from the new
    // tiles; a ramp, graded from that median, re-keys.
    template<> template<>
    void skmapgeom_object::test<18>()
    {
        const LLVector3d origin(0.0, 0.0, 0.0);
        skmap::ScenePtr a = skmap::buildScene(terrainRow(2, false, origin));
        skmap::ScenePtr b = skmap::buildScene(terrainRow(5, false, origin));
        const S32 ga = layerOf(*a, LLVector3d(4.0, 4.0, 0.0));
        const S32 gb = layerOf(*b, LLVector3d(4.0, 4.0, 0.0));
        ensure("ground layers", ga >= 0 && gb >= 0 && a->mLayers[ga].mGround && b->mLayers[gb].mGround);
        ensure("one surface", b->mSurfaces.size() == 1);
        ensure("the median moved", a->mLayers[ga].mBaseZ != b->mLayers[gb].mBaseZ);
        S32 kept = 0;
        for (const auto& entry : a->mCellHashes[ga])
        {
            const LLVector3d hi = entry.first.maxCorner();
            if (entry.first.mTier != 0 || hi.mdV[VX] + skmap::HASH_REACH_M > 64.0)
            {
                continue;
            }
            ensure("a ground leaf keeps its key", skmap::cellKey(*b, gb, entry.first) == entry.second);
            ++kept;
        }
        ensure("leaves compared", kept > 0);

        skmap::ScenePtr ra = skmap::buildScene(terrainRow(2, true, origin));
        skmap::ScenePtr rb = skmap::buildScene(terrainRow(5, true, origin));
        const S32 rga = layerOf(*ra, LLVector3d(4.0, 4.0, 0.0));
        const S32 rgb = layerOf(*rb, LLVector3d(4.0, 4.0, 0.0));
        ensure("the ramp is the ground's", ra->mRuns.size() == 1 && ra->mRuns[0].mLayer == rga && !ra->mRuns[0].mStair);
        const skmap::CellId leaf = skmap::cellAt(0, 10.0, 9.0);
        ensure("the ramp leaf re-keys", skmap::cellKey(*ra, rga, leaf) != skmap::cellKey(*rb, rgb, leaf));
        ensure("a far leaf does not", skmap::cellKey(*ra, rga, skmap::cellAt(0, 28.0, 28.0))
                                          == skmap::cellKey(*rb, rgb, skmap::cellAt(0, 28.0, 28.0)));
    }

    // The building at a mainland origin: every layer, footprint, floor and cut moves by whole
    // cells, so nothing mixes scene-local and global coordinates.
    template<> template<>
    void skmapgeom_object::test<19>()
    {
        const LLVector3d by(255744.0, 256000.0, 0.0);
        const S32 dx = static_cast<S32>(by.mdV[VX] / skmap::INDEX_CELL_M), dy = static_cast<S32>(by.mdV[VY] / skmap::INDEX_CELL_M);
        skmap::ScenePtr a = skmap::buildScene(buildingInput(true));
        skmap::ScenePtr b = skmap::buildScene(shifted(buildingInput(true), by));
        ensure_equals("same layers", a->mLayers.size(), b->mLayers.size());
        ensure("same order", a->mOrder == b->mOrder);
        ensure_equals("same surfaces", a->mSurfaces.size(), b->mSurfaces.size());
        ensure_equals("same runs", a->mRuns.size(), b->mRuns.size());
        ensure_equals("same marks", a->mMarks.size(), b->mMarks.size());
        auto move = [&](const std::set<std::pair<S32, S32>>& cells) {
            std::set<std::pair<S32, S32>> out;
            for (const auto& c : cells)
            {
                out.insert({ c.first + dx, c.second + dy });
            }
            return out;
        };
        for (size_t l = 0; l < a->mLayers.size(); ++l)
        {
            ensure("a footprint moves whole", move(a->mLayers[l].mFootprint) == b->mLayers[l].mFootprint);
        }
        for (const LLVector3d& p : { LLVector3d(13.0, 13.0, 0.0), LLVector3d(9.0, 15.0, 6.0), LLVector3d(27.0, 27.0, 6.0), LLVector3d(14.0, 10.0, 3.0) })
        {
            const S32 la = layerOf(*a, p);
            ensure("a floor at home", la >= 0);
            ensure_equals("the same floor abroad", layerOf(*b, p + by), la);
        }
        const skmap::LayerSelection sa = skmap::selectLayers(*a, agentAt(*a, LLVector3d(13.0, 13.0, 0.0)), EnclosureBucket::INDOORS, 30.f);
        const skmap::LayerSelection sb = skmap::selectLayers(*b, agentAt(*b, LLVector3d(13.0, 13.0, 0.0) + by), EnclosureBucket::INDOORS, 30.f);
        ensure("something cut", !sa.mCut.empty());
        ensure("same drawn", sa.mDrawn == sb.mDrawn);
        ensure("same storey", sa.mStorey == sb.mStorey);
        ensure_equals("same cut layers", sa.mCut.size(), sb.mCut.size());
        for (const auto& entry : sa.mCut)
        {
            const auto other = sb.mCut.find(entry.first);
            ensure("the cut layer abroad", other != sb.mCut.end());
            ensure("the cut moves whole", move(entry.second) == other->second);
        }
    }

    // Hillside ground spanning several metres stays; a glitched polygon hundreds of metres tall does not.
    template<> template<>
    void skmapgeom_object::test<20>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 20.f, 20.f, 0.f, 8.f, AREA_GROUND);
        t.quad(20.f, 0.f, 22.f, 2.f, 0.f, 500.f, AREA_GROUND);
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(LAND, true, 0, 0) }));
        ensure_equals("only the hillside is kept", s->mPolys.size(), static_cast<size_t>(1));
        ensure("the hillside is ground", layerOf(*s, LLVector3d(10.0, 10.0, 4.0)) >= 0);
    }

    // With no records the scene is today's: no section, and every wall segment keeps its wall mark.
    template<> template<>
    void skmapgeom_object::test<21>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 16.f, 16.f, 0.f, 0.f);
        for (const auto& seg : { std::make_pair(LLVector3(0.f, 0.f, 0.f), LLVector3(16.f, 0.f, 0.f)),
                                 std::make_pair(LLVector3(16.f, 0.f, 0.f), LLVector3(16.f, 16.f, 0.f)),
                                 std::make_pair(LLVector3(0.f, 16.f, 0.f), LLVector3(0.f, 0.f, 0.f)) })
        {
            WallSegment w;
            w.mA = seg.first;
            w.mB = seg.second;
            w.mKind = EdgeKind::WALL;
            w.mValue = 3.f;
            t.mSegments.push_back(w);
        }
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }));
        ensure("no section", !s->mHasSection);
        ensure("no pieces", s->mPieces.empty());
        for (const skmap::SceneLayer& layer : s->mLayers)
        {
            ensure("no level grid", layer.mLevels.mW == 0 && layer.mPieces.empty());
        }
        size_t walls = 0;
        for (const skmap::LineMark& m : s->mMarks)
        {
            walls += m.mKind == skmap::MarkKind::WALL ? 1 : 0;
        }
        ensure_equals("every wall segment a wall mark", walls, static_cast<size_t>(3));
    }

    // Only static avatar obstacles are cut; a placeholder is provisional; a claimed door leaf goes to its door.
    template<> template<>
    void skmapgeom_object::test<22>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 40.f, 6.f, 0.f, 0.f);
        const LLVector3 h(0.5f, 0.1f, 1.5f);
        auto at = [&](F32 x, U64 sig) { return boxRecordOf(LLVector3(x, 7.f, 1.5f), h, sig); };
        std::vector<RecordPtr> recs;
        Record dynamic = at(2.f, 1);
        dynamic.mDynamic = true;
        Record vehicle = at(5.f, 2);
        vehicle.mExclude = EXCLUDE_ALL;
        vehicle.mFeature = FeatureKind::VEHICLE;
        Record elevator = at(8.f, 3);
        elevator.mExclude = EXCLUDE_ALL;
        elevator.mFeature = FeatureKind::ELEVATOR;
        Record exclusion = at(11.f, 4);
        exclusion.mNavRole = NavRole::EXCLUSION_VOLUME;
        Record surround = at(14.f, 5);
        surround.mRole = Role::SURROUND;
        Record placeholder = at(17.f, 6);
        placeholder.mProvenance = Provenance::BOX;
        Record leaf = at(20.f, 7);
        leaf.mFeature = FeatureKind::DOOR;
        leaf.mDynamic = true;
        Record plain = at(23.f, 8);
        for (const Record& r : { dynamic, vehicle, elevator, exclusion, surround, placeholder, leaf, plain })
        {
            recs.push_back(std::make_shared<const Record>(r));
        }
        skmap::SceneInput in = input({ t.make(CLUSTER, false, 0, 0) }, &recs);
        skmap::DoorInput door;
        door.mPlanePoint.set(20.0, 7.0, 0.0);
        door.mNormal.set(0.f, 1.f, 0.f);
        door.mWidth = 1.f;
        door.mMin.set(19.5, 6.9, 0.0);
        door.mMax.set(20.5, 7.1, 3.0);
        door.mLeaves = { leaf.mPart };
        in.mDoors.push_back(door);
        skmap::ScenePtr s = skmap::buildScene(in);
        ensure("a section", s->mHasSection);
        for (U64 sig : { 1ull, 2ull, 3ull, 4ull, 5ull, 7ull })
        {
            ensure("left out", piecesOf(*s, sig).empty());
        }
        ensure("the placeholder is cut", !piecesOf(*s, 6).empty());
        for (const skmap::SectionPiece* p : piecesOf(*s, 6))
        {
            ensure("provisional", p->mStyle == skmap::SectionStyle::PROVISIONAL);
        }
        ensure("a plain wall is solid", !piecesOf(*s, 8).empty() && piecesOf(*s, 8)[0]->mStyle == skmap::SectionStyle::SOLID);
        ensure_equals("one door's leaves", s->mDoorLeaves.size(), static_cast<size_t>(1));
        ensure("the leaf is its door's", s->mDoorLeaves[0].size() == 1 && s->mDoorLeaves[0][0]->mSignature == 7);

        // The same leaf unclaimed and at rest is an ordinary wall.
        leaf.mDynamic = false;
        recs[6] = std::make_shared<const Record>(leaf);
        skmap::SceneInput unclaimed = input({ t.make(CLUSTER, false, 0, 0) }, &recs);
        skmap::ScenePtr u = skmap::buildScene(unclaimed);
        ensure("an unclaimed leaf is cut", !piecesOf(*u, 7).empty());
    }

    // The level field: Recast's lattice, six cells of reach, and a ramp's levels following it.
    template<> template<>
    void skmapgeom_object::test<23>()
    {
        TileMaker t;
        t.quad(0.f, 0.f, 16.f, 16.f, 30.375f, 30.375f);
        std::vector<RecordPtr> recs = { boxRecord(LLVector3(8.f, 17.f, 31.5f), LLVector3(4.f, 0.1f, 1.5f), 1) };
        skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }, &recs));
        const S32 l = layerOf(*s, LLVector3d(8.0, 8.0, 30.375));
        ensure("the floor's layer", l >= 0);
        const skmap::LevelGrid& g = s->mLayers[l].mLevels;
        for (S32 y = 0; y < 16; ++y)
        {
            for (S32 x = 0; x < 16; ++x)
            {
                ensure("one level, 243", levelsAt(g, x, y) == std::vector<S32>({ 243 }));
            }
        }
        ensure("six cells beyond", levelsAt(g, 21, 8) == std::vector<S32>({ 243 }));
        ensure("seven beyond, nothing", levelsAt(g, 22, 8).empty());
        ensure("listed", skmap::levelListed(g, 21, 8, 243) && !skmap::levelListed(g, 22, 8, 243));
        ensure_approximately_equals_range("low plane", skmap::sectionPlaneZ(246), 30.8125f, 1e-5f);
        ensure_approximately_equals_range("high plane", skmap::sectionPlaneZ(254), 31.8125f, 1e-5f);
        std::set<S32> planes;
        for (const skmap::SectionPiece* p : piecesOf(*s, 1))
        {
            planes.insert(p->mPlane);
        }
        ensure("cut at 246 and 254", planes == std::set<S32>({ 246, 254 }));

        TileMaker ramp;
        ramp.quad(0.f, 0.f, 12.f, 4.f, 0.f, 3.f);
        skmap::ScenePtr r = skmap::buildScene(input({ ramp.make(CLUSTER, false, 0, 0) }, &recs));
        const skmap::LevelGrid& rg = r->mLayers[layerOf(*r, LLVector3d(1.0, 1.0, 0.25))].mLevels;
        for (S32 x = 0; x < 12; ++x)
        {
            ensure("levels follow the ramp", levelsAt(rg, x, 1) == std::vector<S32>({ skmap::sectionLevel(0.25f * (x + 0.5f)) }));
        }
    }

    /// A wall beside a floor and a post; a crate whose top joins the floor around it.
    skmap::SceneInput piecesInput(std::vector<RecordPtr>& recs)
    {
        TileMaker t;
        t.quad(0.f, 4.5f, 16.f, 12.f, 0.f, 0.f);
        const S32 south = t.quad(20.f, 0.f, 28.f, 3.5f, 0.f, 0.f);
        const S32 north = t.quad(20.f, 4.5f, 28.f, 8.f, 0.f, 0.f);
        const S32 west = t.quad(20.f, 3.5f, 23.5f, 4.5f, 0.f, 0.f);
        const S32 east = t.quad(24.5f, 3.5f, 28.f, 4.5f, 0.f, 0.f);
        const S32 top = t.quad(23.5f, 3.5f, 24.5f, 4.5f, 0.75f, 0.75f);
        t.join(top, 0, south, 2);
        t.join(top, 2, north, 0);
        t.join(top, 3, west, 1);
        t.join(top, 1, east, 3);
        recs = { boxRecord(LLVector3(8.f, 4.f, 1.5f), LLVector3(4.f, 0.1f, 1.5f), 1), boxRecord(LLVector3(24.f, 4.f, 0.35f), LLVector3(0.5f, 0.5f, 0.35f), 2),
                 boxRecord(LLVector3(14.f, 3.f, 0.5f), LLVector3(0.05f, 0.05f, 0.5f), 3) };
        return input({ t.make(CLUSTER, false, 0, 0) }, &recs);
    }

    // Pieces: a wall at both planes, a climbable crate as an island at the low one, a post as clutter.
    template<> template<>
    void skmapgeom_object::test<24>()
    {
        std::vector<RecordPtr> recs;
        skmap::ScenePtr s = skmap::buildScene(piecesInput(recs));
        std::set<S32> layers;
        for (const LLVector3d& p : { LLVector3d(21.0, 1.0, 0.0), LLVector3d(21.0, 6.0, 0.0), LLVector3d(21.0, 4.0, 0.0),
                                     LLVector3d(26.0, 4.0, 0.0), LLVector3d(24.0, 4.0, 0.75) })
        {
            layers.insert(layerOf(*s, p));
        }
        ensure("the crate's floors are one layer", layers.size() == 1 && *layers.begin() >= 0);

        std::set<S32> wall_planes;
        for (const skmap::SectionPiece* p : piecesOf(*s, 1))
        {
            wall_planes.insert(p->mPlane);
            ensure_approximately_equals_range("wall thickness", p->mThickness, 0.1951f, 1e-4f);
            ensure_equals("wall flags", static_cast<S32>(p->mFlags), 0);
        }
        ensure("the wall at 3 and 11", wall_planes == std::set<S32>({ 3, 11 }));

        const std::vector<const skmap::SectionPiece*> crate = piecesOf(*s, 2, 3);
        ensure("the crate at the low plane", crate.size() == 1 && (crate[0]->mFlags & skmap::PIECE_ISLAND));
        ensure("the ground around it gates it low", (crate[0]->mFlags & skmap::PIECE_ISLAND_LOW) != 0);
        ensure("and not high", (crate[0]->mFlags & skmap::PIECE_ISLAND_HIGH) == 0);
        ensure("the crate not at the high plane", piecesOf(*s, 2, 11).empty());

        const std::vector<const skmap::SectionPiece*> post = piecesOf(*s, 3, 3);
        ensure("the post is clutter", post.size() == 1 && (post[0]->mFlags & skmap::PIECE_BLOB));
    }

    // Pure: no cache, a fresh cache and a warm one build the same scene.
    template<> template<>
    void skmapgeom_object::test<25>()
    {
        std::vector<RecordPtr> recs;
        const skmap::SceneInput in = piecesInput(recs);
        skmap::SectionCache cache;
        skmap::ScenePtr plain = skmap::buildScene(in);
        skmap::ScenePtr cold = skmap::buildScene(in, &cache);
        skmap::ScenePtr warm = skmap::buildScene(in, &cache);
        ensure("pieces made", !plain->mPieces.empty());
        ensure("the same keys, cold", plain->mCellHashes == cold->mCellHashes);
        ensure("the same keys, warm", plain->mCellHashes == warm->mCellHashes);
        ensure("the same pieces, cold", samePieces(*plain, *cold));
        ensure("the same pieces, warm", samePieces(*plain, *warm));
        ensure("the cold build sliced", cold->mSectionStats.mSlices > 0);
        ensure_equals("the warm build sliced nothing", warm->mSectionStats.mSlices, 0u);
        ensure_equals("all hits", warm->mSectionStats.mCacheHits, cold->mSectionStats.mSlices);
    }

    // What moves a leaf's key and what does not.
    template<> template<>
    void skmapgeom_object::test<26>()
    {
        // (a) A new snapshot of the same records moves nothing and slices nothing.
        std::vector<RecordPtr> recs;
        const skmap::SceneInput in = piecesInput(recs);
        skmap::SectionCache cache;
        skmap::ScenePtr a = skmap::buildScene(in, &cache);
        std::vector<RecordPtr> again;
        for (const RecordPtr& r : recs)
        {
            again.push_back(std::make_shared<const Record>(*r));
        }
        skmap::SceneInput in2 = in;
        in2.mRecords = std::make_shared<const std::vector<RecordPtr>>(again);
        skmap::ScenePtr b = skmap::buildScene(in2, &cache);
        ensure("a new snapshot keeps every key", a->mCellHashes == b->mCellHashes);
        ensure_equals("and hits the cache", b->mSectionStats.mSlices, 0u);

        // (b) The box and its origin moved 64 m.
        skmap::SceneInput moved = in;
        const LLVector3d by(64.0, 0.0, 0.0);
        moved.mOrigin += by;
        moved.mMin += by;
        moved.mMax += by;
        skmap::ScenePtr m = skmap::buildScene(moved, &cache);
        ensure_equals("a box step slices nothing", m->mSectionStats.mSlices, 0u);
        ensure_equals("same layers", a->mCellHashes.size(), m->mCellHashes.size());
        size_t compared = 0;
        for (size_t l = 0; l < a->mCellHashes.size(); ++l)
        {
            for (const auto& entry : a->mCellHashes[l])
            {
                const auto it = m->mCellHashes[l].find(entry.first);
                if (it != m->mCellHashes[l].end())
                {
                    ensure("a leaf keeps its key across a box step", it->second == entry.second);
                    ++compared;
                }
            }
        }
        ensure("leaves compared", compared > 0);

        // (c) One record re-signed and moved 0.5 m: only leaves near its old and new boxes move.
        std::vector<RecordPtr> shifted_recs = recs;
        Record wall = *recs[0];
        wall.mCenter += LLVector3(0.5f, 0.f, 0.f);
        wall.mMin.mdV[VX] += 0.5;
        wall.mMax.mdV[VX] += 0.5;
        wall.mSignature = 101;
        shifted_recs[0] = std::make_shared<const Record>(wall);
        skmap::SceneInput in3 = in;
        in3.mRecords = std::make_shared<const std::vector<RecordPtr>>(shifted_recs);
        skmap::ScenePtr c = skmap::buildScene(in3, &cache);
        const F64 x0 = llmin(recs[0]->mMin.mdV[VX], wall.mMin.mdV[VX]) - skmap::HASH_REACH_M;
        const F64 x1 = llmax(recs[0]->mMax.mdV[VX], wall.mMax.mdV[VX]) + skmap::HASH_REACH_M;
        const F64 y0 = recs[0]->mMin.mdV[VY] - skmap::HASH_REACH_M, y1 = recs[0]->mMax.mdV[VY] + skmap::HASH_REACH_M;
        size_t changed = 0;
        for (size_t l = 0; l < a->mCellHashes.size(); ++l)
        {
            std::set<skmap::CellId> cells;
            for (const auto& e : a->mCellHashes[l])
            {
                cells.insert(e.first);
            }
            for (const auto& e : c->mCellHashes[l])
            {
                cells.insert(e.first);
            }
            for (const skmap::CellId& cell : cells)
            {
                if (cell.mTier != 0 || skmap::cellKey(*a, static_cast<S32>(l), cell) == skmap::cellKey(*c, static_cast<S32>(l), cell))
                {
                    continue;
                }
                ++changed;
                const LLVector3d lo = cell.minCorner(), hi = cell.maxCorner();
                ensure("only near the record", lo.mdV[VX] <= x1 && hi.mdV[VX] >= x0 && lo.mdV[VY] <= y1 && hi.mdV[VY] >= y0);
            }
        }
        ensure("something moved", changed > 0);

        // (d) A floor 2.5 m beyond a leaf rises 1 m: the leaf's key moves through the levels alone.
        auto floors = [](F32 z) {
            TileMaker t;
            t.quad(0.f, 0.f, 5.f, 8.f, 0.f, 0.f);
            t.quad(10.5f, 0.f, 12.f, 8.f, z, z);
            return t.make(CLUSTER, false, 0, 0);
        };
        std::vector<RecordPtr> low_wall = { boxRecord(LLVector3(6.5f, 4.f, 0.8f), LLVector3(0.1f, 3.f, 0.8f), 1) };
        skmap::ScenePtr d0 = skmap::buildScene(input({ floors(0.f) }, &low_wall));
        skmap::ScenePtr d1 = skmap::buildScene(input({ floors(1.f) }, &low_wall));
        const S32 l0 = layerOf(*d0, LLVector3d(2.0, 2.0, 0.0)), l1 = layerOf(*d1, LLVector3d(2.0, 2.0, 0.0));
        ensure("one layer", l0 >= 0 && l0 == layerOf(*d0, LLVector3d(11.0, 2.0, 0.0)) && l1 == layerOf(*d1, LLVector3d(11.0, 2.0, 1.0)));
        ensure("the same pieces", samePieces(*d0, *d1));
        const skmap::CellId leaf = skmap::cellAt(0, 6.5, 4.0);
        ensure("the leaf holds a piece", skmap::cellKey(*d0, l0, leaf) != 0);
        ensure("the leaf's key moves", skmap::cellKey(*d0, l0, leaf) != skmap::cellKey(*d1, l1, leaf));

        // (e) A box 4 m from any floor still gets a leaf.
        TileMaker tiny;
        tiny.quad(0.f, 0.f, 5.f, 5.f, 0.f, 0.f);
        std::vector<RecordPtr> distant = { boxRecord(LLVector3(9.5f, 2.5f, 1.5f), LLVector3(0.5f, 0.5f, 1.5f), 1) };
        skmap::ScenePtr e = skmap::buildScene(input({ tiny.make(CLUSTER, false, 0, 0) }, &distant));
        skmap::ScenePtr none = skmap::buildScene(input({ tiny.make(CLUSTER, false, 0, 0) }));
        const S32 le = layerOf(*e, LLVector3d(2.0, 2.0, 0.0));
        const skmap::CellId far_leaf = skmap::cellAt(0, 9.5, 2.5);
        ensure("no leaf there without it", skmap::cellKey(*none, layerOf(*none, LLVector3d(2.0, 2.0, 0.0)), far_leaf) == 0);
        ensure("its leaf exists", skmap::cellKey(*e, le, far_leaf) != 0);
    }

    // Orphans: a navmesh wall line stays only where nothing the section cut stands behind it.
    template<> template<>
    void skmapgeom_object::test<27>()
    {
        TileMaker t;
        t.quad(-4.f, -4.f, 8.f, 0.f, 0.f, 0.f);
        WallSegment w;
        w.mA.set(0.f, 0.f, 0.f);
        w.mB.set(4.f, 0.f, 0.f);
        w.mKind = EdgeKind::WALL;
        w.mValue = 3.f;
        t.mSegments.push_back(w);
        auto walls = [&](const std::vector<RecordPtr>& recs) {
            skmap::ScenePtr s = skmap::buildScene(input({ t.make(CLUSTER, false, 0, 0) }, &recs));
            std::vector<std::pair<F32, F32>> out;
            for (const skmap::LineMark& m : s->mMarks)
            {
                if (m.mKind == skmap::MarkKind::WALL)
                {
                    ensure_equals("the floor side kept", static_cast<S32>(m.mFloorSide), -1);
                    out.emplace_back(llmin(m.mA.mV[VX], m.mB.mV[VX]), llmax(m.mA.mV[VX], m.mB.mV[VX]));
                }
            }
            return out;
        };
        auto span = [](F32 x0, F32 y0, F32 x1, F32 y1, U64 sig) {
            return boxRecord(LLVector3((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 1.5f), LLVector3((x1 - x0) * 0.5f, (y1 - y0) * 0.5f, 1.5f), sig);
        };
        auto same = [](F32 a, F32 b) { return std::fabs(a - b) < 1e-3f; };

        ensure("covered all along", walls({ span(-0.5f, 0.375f, 4.5f, 0.625f, 1) }).empty());
        std::vector<std::pair<F32, F32>> kept = walls({});
        ensure("nothing behind: the whole wall", kept.size() == 1 && same(kept[0].first, 0.f) && same(kept[0].second, 4.f));
        kept = walls({ span(-0.5f, 0.375f, 2.f, 0.625f, 1) });
        ensure("half covered: the rest", kept.size() == 1 && same(kept[0].first, 2.5f) && same(kept[0].second, 4.f));
        kept = walls({ span(-0.5f, 1.5f, 4.5f, 1.8f, 1) });
        ensure("too far behind: the whole wall", kept.size() == 1 && same(kept[0].first, 0.f) && same(kept[0].second, 4.f));
        kept = walls({ span(1.9f, 0.4f, 2.1f, 0.6f, 1) });
        ensure("clutter covers nothing", kept.size() == 1 && same(kept[0].first, 0.f) && same(kept[0].second, 4.f));
    }

    /// The oracle's doorway: a 1.2 m door in the plane x = 0, floor either side, its one leaf as given.
    skmap::ScenePtr doorway(const LLVector3& c, const LLVector3& h, bool claimed)
    {
        TileMaker t;
        t.quad(-4.f, -2.f, -0.3f, 2.f, 0.f, 0.f);
        t.quad(0.3f, -2.f, 4.f, 2.f, 0.f, 0.f);
        Record leaf = boxRecordOf(c, h, 9);
        leaf.mFeature = FeatureKind::DOOR;
        std::vector<RecordPtr> recs = { std::make_shared<const Record>(leaf) };
        skmap::SceneInput in = input({ t.make(CLUSTER, false, 0, 0) }, &recs);
        skmap::DoorInput door;
        door.mPlanePoint.set(0.0, 0.0, 0.0);
        door.mNormal.set(1.f, 0.f, 0.f);
        door.mWidth = 1.2f;
        door.mSill = 0.0;
        door.mMin.set(-0.03, -0.6, 0.0);
        door.mMax.set(0.03, 0.6, 2.2);
        if (claimed)
        {
            door.mLeaves = { leaf.mPart };
        }
        in.mDoors.push_back(door);
        return skmap::buildScene(in);
    }

    std::vector<const skmap::LineMark*> marksOf(const skmap::Scene& s, skmap::MarkKind kind)
    {
        std::vector<const skmap::LineMark*> out;
        for (const skmap::LineMark& m : s.mMarks)
        {
            if (m.mKind == kind)
            {
                out.push_back(&m);
            }
        }
        return out;
    }

    bool at(const LLVector2& p, F32 x, F32 y)
    {
        return std::fabs(p.mV[VX] - x) <= 0.01f && std::fabs(p.mV[VY] - y) <= 0.01f;
    }

    // A shut leaf: one bar across the doorway.
    template<> template<>
    void skmapgeom_object::test<28>()
    {
        skmap::ScenePtr s = doorway(LLVector3(0.f, 0.f, 1.1f), LLVector3(0.03f, 0.6f, 1.1f), true);
        const std::vector<const skmap::LineMark*> shut = marksOf(*s, skmap::MarkKind::DOOR_SHUT);
        ensure_equals("one shut bar", shut.size(), static_cast<size_t>(1));
        ensure("jamb to jamb", at(shut[0]->mA, 0.f, -0.6f) && at(shut[0]->mB, 0.f, 0.6f));
        ensure("no leaf", marksOf(*s, skmap::MarkKind::DOOR_LEAF).empty());
        ensure("no jambs", marksOf(*s, skmap::MarkKind::DOOR_JAMB).empty());
    }

    // An open leaf: the threshold, the leaf where it is now, and its swing from where it rests shut.
    template<> template<>
    void skmapgeom_object::test<29>()
    {
        skmap::ScenePtr s = doorway(LLVector3(0.6f, -0.57f, 1.1f), LLVector3(0.6f, 0.03f, 1.1f), true);
        const std::vector<const skmap::LineMark*> threshold = marksOf(*s, skmap::MarkKind::DOOR_THRESHOLD);
        ensure("a threshold", threshold.size() == 1 && at(threshold[0]->mA, 0.f, -0.6f) && at(threshold[0]->mB, 0.f, 0.6f));
        const std::vector<const skmap::LineMark*> leaf = marksOf(*s, skmap::MarkKind::DOOR_LEAF);
        ensure("the leaf from its hinge", leaf.size() == 1 && at(leaf[0]->mA, 0.f, -0.57f) && at(leaf[0]->mB, 1.2f, -0.57f));
        const std::vector<const skmap::LineMark*> swing = marksOf(*s, skmap::MarkKind::DOOR_SWING);
        ensure_equals("a swing", swing.size(), static_cast<size_t>(1));
        ensure("about the hinge", at(swing[0]->mA, 0.f, -0.57f));
        ensure("from the shut end", at(swing[0]->mB, 0.f, 0.63f));
        ensure_approximately_equals_range("a quarter turn clockwise", swing[0]->mSweep, -F_PI_BY_TWO, 0.01f);
        ensure("no shut bar", marksOf(*s, skmap::MarkKind::DOOR_SHUT).empty());
    }

    // A door with no leaf record keeps its jambs and leaf.
    template<> template<>
    void skmapgeom_object::test<30>()
    {
        skmap::ScenePtr s = doorway(LLVector3(0.f, 0.f, 1.1f), LLVector3(0.03f, 0.6f, 1.1f), false);
        ensure_equals("two jambs", marksOf(*s, skmap::MarkKind::DOOR_JAMB).size(), static_cast<size_t>(2));
        ensure_equals("one leaf", marksOf(*s, skmap::MarkKind::DOOR_LEAF).size(), static_cast<size_t>(1));
        ensure("no state", marksOf(*s, skmap::MarkKind::DOOR_SHUT).empty() && marksOf(*s, skmap::MarkKind::DOOR_THRESHOLD).empty());
    }

    /// Floor at 0 either side of a wall line along y = 0, and the given wall boxes, x0..x1 by 0.3 m, z0..z1.
    skmap::ScenePtr wallLine(const std::vector<std::array<F32, 4>>& walls, bool report)
    {
        TileMaker t;
        t.quad(-4.f, -4.f, 7.f, -0.5f, 0.f, 0.f);
        t.quad(-4.f, 0.5f, 7.f, 4.f, 0.f, 0.f);
        std::vector<RecordPtr> recs;
        U64 sig = 1;
        for (const std::array<F32, 4>& w : walls)
        {
            recs.push_back(boxRecord(LLVector3((w[0] + w[1]) * 0.5f, 0.f, (w[2] + w[3]) * 0.5f),
                                     LLVector3((w[1] - w[0]) * 0.5f, 0.15f, (w[3] - w[2]) * 0.5f), sig++));
        }
        skmap::SceneInput in = input({ t.make(CLUSTER, false, 0, 0) }, &recs);
        in.mSectionReport = report;
        return skmap::buildScene(in);
    }

    // A window: the sill reads half cover with the head over it as its lintel; the walls either side read full.
    template<> template<>
    void skmapgeom_object::test<31>()
    {
        skmap::ScenePtr s = wallLine({ { -3.f, 0.f, 0.f, 3.f }, { 0.f, 3.f, 0.f, 0.9f }, { 3.f, 6.f, 0.f, 3.f }, { 0.f, 3.f, 2.2f, 3.f } }, true);
        ensure("one layer", s->mOrder.size() == 1);
        std::vector<const skmap::SectionReport*> half, full;
        for (const skmap::SectionReport& e : s->mSectionReport)
        {
            (e.mFull ? full : half).push_back(&e);
        }
        ensure_equals("one half entry", half.size(), static_cast<size_t>(1));
        ensure_equals("two full entries", full.size(), static_cast<size_t>(2));
        for (const skmap::SectionReport* e : full)
        {
            ensure("a side wall", e->mPart == partId(1) || e->mPart == partId(3));
            ensure_approximately_equals_range("full from the floor's high plane", e->mHigh, 1.4375f, 1e-4f);
        }
        const skmap::SectionReport& r = *half[0];
        ensure("the sill", r.mPart == partId(2));
        ensure_approximately_equals_range("half from the floor's low plane", r.mLow, 0.4375f, 1e-4f);
        ensure_approximately_equals_range("lintel", r.mLintel, 2.2f, 0.01f);
        ensure_approximately_equals_range("thickness", r.mThickness, 0.2727f, 0.001f);
        ensure_approximately_equals_range("length", r.mLength, 3.f, 0.001f);
        ensure_equals("one opening", s->mSectionStats.mHalfOpenings, 1u);
        ensure_equals("the two walls are full", s->mSectionStats.mFullRecords, 2u);
    }

    // A lone half wall has nothing over it.
    template<> template<>
    void skmapgeom_object::test<32>()
    {
        TileMaker t;
        t.quad(-4.f, -4.f, 7.f, -0.5f, 0.f, 0.f);
        std::vector<RecordPtr> recs = { boxRecord(LLVector3(1.25f, 0.f, 0.55f), LLVector3(1.25f, 0.1f, 0.55f), 1) };
        skmap::SceneInput in = input({ t.make(CLUSTER, false, 0, 0) }, &recs);
        in.mSectionReport = true;
        skmap::ScenePtr s = skmap::buildScene(in);
        ensure_equals("one entry", s->mSectionReport.size(), static_cast<size_t>(1));
        ensure("it is half", !s->mSectionReport[0].mFull);
        ensure_equals("no lintel", s->mSectionReport[0].mLintel, -1.f);
        ensure_approximately_equals_range("thickness", s->mSectionReport[0].mThickness, 0.1852f, 0.001f);
        ensure_equals("no opening", s->mSectionStats.mHalfOpenings, 0u);
    }

    // The report is collected only when asked for; the counts always are.
    template<> template<>
    void skmapgeom_object::test<33>()
    {
        const std::vector<std::array<F32, 4>> walls = { { -3.f, 0.f, 0.f, 3.f }, { 0.f, 3.f, 0.f, 0.9f }, { 3.f, 6.f, 0.f, 3.f }, { 0.f, 3.f, 2.2f, 3.f } };
        skmap::ScenePtr on = wallLine(walls, true);
        skmap::ScenePtr off = wallLine(walls, false);
        ensure("no report", off->mSectionReport.empty());
        ensure_equals("full", off->mSectionStats.mFullRecords, on->mSectionStats.mFullRecords);
        ensure_equals("half", off->mSectionStats.mHalfRecords, on->mSectionStats.mHalfRecords);
        ensure_equals("openings", off->mSectionStats.mHalfOpenings, on->mSectionStats.mHalfOpenings);
        ensure_equals("islands", off->mSectionStats.mIslands, on->mSectionStats.mIslands);
    }
}
