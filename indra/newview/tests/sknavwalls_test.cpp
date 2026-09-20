/**
 * @file sknavwalls_test.cpp
 * @brief Walls off a built tile: cover classes, wall edges, ledges, drop links, exposed faces.
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
#include "../sknavwalls.h"

#include <cmath>

namespace
{
    using namespace sknav;

    const S32 AVATAR = static_cast<S32>(AgentClass::AVATAR);
    const S32 WEATHER = static_cast<S32>(AgentClass::WEATHER);

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
        in.mZMax = 24.f;
        in.mCell = 0.25f;           // coarser than the viewer, to keep the tests quick
        in.mClasses[AVATAR] = true;
        in.mClasses[WEATHER] = true;
        in.mParams[WEATHER].mHeight = 0.5f;
        in.mParams[WEATHER].mRadius = 0.f;
        in.mWallFaces = true;
        in.mEdges[AVATAR] = true;
        in.mEdges[WEATHER] = true;
        in.mTerrain = flat(0.f);
        return in;
    }

    /// Total length of the segments of one kind, and the mean of their values.
    void summarise(const std::vector<WallSegment>& segs, EdgeKind kind, F32& length, F32& mean_value)
    {
        length = 0.f;
        F32 weighted = 0.f;
        for (const WallSegment& s : segs)
        {
            if (s.mKind != kind)
            {
                continue;
            }
            const F32 len = (s.mB - s.mA).magVec();
            length += len;
            weighted += len * s.mValue;
        }
        mean_value = length > 0.f ? weighted / length : 0.f;
    }
}

namespace tut
{
    struct sknavwalls_data {};
    typedef test_group<sknavwalls_data> sknavwalls_group;
    typedef sknavwalls_group::object sknavwalls_object;
    tut::sknavwalls_group g("sknavwalls");

    // Cover is read-time: the same wall is full cover to a short avatar and
    // half cover to a tall one.
    template<> template<>
    void sknavwalls_object::test<1>()
    {
        const F32 climb = 0.75f;
        ensure("0.5 m is no cover", coverClass(0.5f, 1.7f, climb) == Cover::NONE);
        ensure("1.0 m is half", coverClass(1.0f, 1.7f, climb) == Cover::HALF);
        ensure("1.6 m is full at eye 1.7", coverClass(1.6f, 1.7f, climb) == Cover::FULL);
        ensure("1.6 m is half at eye 2.2", coverClass(1.6f, 2.2f, climb) == Cover::HALF);
        ensure("2.2 m is full at eye 2.2", coverClass(2.2f, 2.2f, climb) == Cover::FULL);
        ensure("the climb height is the floor of half cover", coverClass(climb, 1.7f, climb) == Cover::HALF);
        ensure("just under the climb is none", coverClass(climb - 0.01f, 1.7f, climb) == Cover::NONE);
    }

    // A wall across the tile is WALL edges of its own height on both sides,
    // and the tile border is not an edge at all.
    template<> template<>
    void sknavwalls_object::test<2>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(14.f, 4.f, 0.f, 15.f, 28.f, 2.f));
        const TileBuildResult r = buildTile(in);
        ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));

        F32 wall_len = 0.f, wall_h = 0.f, ledge_len = 0.f, drop = 0.f;
        summarise(r.mSegments[AVATAR], EdgeKind::WALL, wall_len, wall_h);
        summarise(r.mSegments[AVATAR], EdgeKind::LEDGE, ledge_len, drop);
        // Two sides of a 24 m wall, minus the ends.
        ensure(llformat("wall edges found: %.1f m", wall_len), wall_len > 30.f);
        ensure(llformat("wall height %.2f m", wall_h), std::fabs(wall_h - 2.f) < 0.35f);
        ensure(llformat("no ledges on flat ground: %.1f m", ledge_len), ledge_len < 2.f);
        ensure("no drop links on flat ground", r.mLinks[AVATAR].empty());
    }

    // A platform is a ledge all round its top, with the drop to the ground and
    // one link every couple of metres.
    template<> template<>
    void sknavwalls_object::test<3>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 3.f));
        const TileBuildResult r = buildTile(in);
        ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));

        F32 ledge_len = 0.f, drop = 0.f, wall_len = 0.f, wall_h = 0.f;
        summarise(r.mSegments[AVATAR], EdgeKind::LEDGE, ledge_len, drop);
        summarise(r.mSegments[AVATAR], EdgeKind::WALL, wall_len, wall_h);
        ensure(llformat("ledge round the top: %.1f m", ledge_len), ledge_len > 40.f);
        ensure(llformat("drop is 3 m: %.2f", drop), std::fabs(drop - 3.f) < 0.4f);
        ensure(llformat("the ground sees a 3 m wall: %.1f m at %.2f", wall_len, wall_h), wall_len > 40.f && std::fabs(wall_h - 3.f) < 0.4f);

        ensure(llformat("links round the ledge: %zu", r.mLinks[AVATAR].size()), r.mLinks[AVATAR].size() > 15);
        for (const DropLink& link : r.mLinks[AVATAR])
        {
            ensure(llformat("link drops 3 m: %.2f", link.mHeight), std::fabs(link.mHeight - 3.f) < 0.4f);
            ensure("link starts on the platform", link.mFrom.mV[VZ] > 2.5f);
            ensure("link lands on the ground", std::fabs(link.mTo.mV[VZ]) < 0.5f);
        }
    }

    // Too far to drop for the avatar; the weather class reaches further and
    // needs no headroom at the landing.
    template<> template<>
    void sknavwalls_object::test<4>()
    {
        TileBuildInput in = tile();
        in.mDropMax[AVATAR] = 10.f;
        in.mDropMax[WEATHER] = 24.f;
        in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 15.f));
        const TileBuildResult r = buildTile(in);
        ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));

        F32 ledge_len = 0.f, drop = 0.f;
        summarise(r.mSegments[AVATAR], EdgeKind::LEDGE, ledge_len, drop);
        ensure(llformat("still a ledge: %.1f m at %.2f", ledge_len, drop), ledge_len > 40.f && std::fabs(drop - 15.f) < 0.6f);
        ensure("15 m is too far to jump", r.mLinks[AVATAR].empty());
        ensure(llformat("but not too far for water: %zu", r.mLinks[WEATHER].size()), !r.mLinks[WEATHER].empty());
    }

    // Exposed faces: a block on flat ground shows its four sides, from the
    // ground to its top, and nothing above it.
    template<> template<>
    void sknavwalls_object::test<5>()
    {
        TileBuildInput in = tile();
        in.mRecords.push_back(box(12.f, 12.f, 0.f, 20.f, 20.f, 2.f));
        const TileBuildResult r = buildTile(in);
        ensure_equals("built: " + r.mError, static_cast<int>(r.mStatus), static_cast<int>(TileBuildResult::SUCCESS));
        ensure(llformat("faces found: %zu", r.mFaces.size()), !r.mFaces.empty());

        // Faces on the block's four sides, by the side they face.
        F32 area[4] = { 0.f, 0.f, 0.f, 0.f };
        F32 highest = 0.f;
        for (const WallFace& f : r.mFaces)
        {
            const LLVector3 size = f.mMax - f.mMin;
            const F32 along = std::max(size.mV[VX], size.mV[VY]);
            const F32 tall = size.mV[VZ];
            // Only the block reaches above the ground in this scene.
            if (f.mMin.mV[VX] > 10.f && f.mMax.mV[VX] < 22.f && f.mMin.mV[VY] > 10.f && f.mMax.mV[VY] < 22.f)
            {
                area[f.mSide] += along * tall;
                highest = std::max(highest, f.mMax.mV[VZ]);
            }
        }
        for (S32 side = 0; side < 4; ++side)
        {
            ensure(llformat("side %d has face area %.1f", side, area[side]), area[side] > 8.f);
        }
        ensure(llformat("faces stop at the block's top: %.2f", highest), std::fabs(highest - 2.f) < 0.3f);
    }
}
