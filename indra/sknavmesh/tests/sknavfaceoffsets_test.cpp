/**
 * @file sknavfaceoffsets_test.cpp
 * @brief Face offsets: where a wall's true face sits inside its solid cell.
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

#include "../sknavair.h"
#include "../sknavbuild.h"
#include "../sknavfaceoffsets.h"
#include "../sknavwalls.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace sknav;

    constexpr F32 CELL = 0.125f;
    constexpr F32 UNIT = 0.1f;
    constexpr S32 CLIMB = 2;

    /// Three columns along x at 0.125 m: air, solid through the jamb band, air.
    AirGrid jambGrid()
    {
        AirGrid g;
        g.mCell = CELL;
        g.mUnit = UNIT;
        g.mWidth = 3;
        g.mHeight = 1;
        AirGap floor;
        floor.mFloor = 0;
        floor.mCeiling = 40;
        floor.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap above;
        above.mFloor = 25;
        above.mCeiling = 40;
        g.mGaps = { floor, above, floor };
        g.mStart = { 0, 1, 2, 3 };
        return g;
    }

    /// A soup in Recast space, filled with SL quads.
    struct Soup
    {
        std::vector<F32> mVerts;
        std::vector<S32> mTris;

        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
        {
            for (const LLVector3* t : { &a, &b, &c, &a, &c, &d })
            {
                F32 rc[3];
                toRecast(*t, rc);
                mTris.push_back(static_cast<S32>(mVerts.size() / 3));
                mVerts.insert(mVerts.end(), rc, rc + 3);
            }
        }
        /// A wall in the plane x, across the cell and up past the band.
        void wallX(F32 x) { quad(LLVector3(x, -1.f, 0.f), LLVector3(x, 2.f, 0.f), LLVector3(x, 2.f, 3.f), LLVector3(x, -1.f, 3.f)); }
        /// A slab at height z over x0 to x1.
        void slab(F32 x0, F32 x1, F32 z) { quad(LLVector3(x0, -1.f, z), LLVector3(x1, -1.f, z), LLVector3(x1, 2.f, z), LLVector3(x0, 2.f, z)); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

    /// A corridor between wall columns at 2 and 3 + width across it, eroded at Recast's 3 cells and recovered at radius; the
    /// walls' true faces lie `offset` steps into their cells. Across runs along chf z, or along chf x when transposed.
    /// Only the walls from faces_from to faces_to along it have offsets. Returns the walkable cells across it at `at`.
    std::vector<S32> erodedCorridor(S32 width, U32 offset, U32* recovered = nullptr, bool transposed = false, F32 radius = 0.3f, S32 along = 16,
                                    S32 faces_from = 0, S32 faces_to = 16, S32 at = 7)
    {
        const S32 across = width + 6;
        const S32 w = transposed ? across : along, h = transposed ? along : across;
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        const F32 bmin[3] = { 0.f, 0.f, 0.f };
        const F32 bmax[3] = { w * CELL, 5.f, h * CELL };
        rcCreateHeightfield(&ctx, *hf, w, h, bmin, bmax, CELL, UNIT);
        const S32 wall0 = 2, wall1 = 3 + width;
        for (S32 z = 0; z < h; ++z)
        {
            for (S32 x = 0; x < w; ++x)
            {
                const S32 a = transposed ? x : z;
                const bool wall = a == wall0 || a == wall1;
                rcAddSpan(&ctx, *hf, x, z, 0, wall ? 40 : 1, wall ? RC_NULL_AREA : RC_WALKABLE_AREA, 1);
            }
        }
        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        rcBuildCompactHeightfield(&ctx, 20, 2, *hf, *chf);
        const std::vector<U8> before(chf->areas, chf->areas + chf->spanCount);
        rcErodeWalkableArea(&ctx, 3, *chf);
        // The grid's (ix, iy) is chf (z, x). Across z the near wall's face looks +ix (side 1), the far one's -ix (side 0);
        // across x, +iy (side 3) and -iy (side 2).
        std::vector<FaceOffset> table;
        for (S32 k = faces_from; k < faces_to; ++k)
        {
            if (transposed)
            {
                table.push_back(FaceOffset(faceKey(k, wall0, h, 3), offset));
                table.push_back(FaceOffset(faceKey(k, wall1, h, 2), offset));
            }
            else
            {
                table.push_back(FaceOffset(faceKey(wall0, k, h, 1), offset));
                table.push_back(FaceOffset(faceKey(wall1, k, h, 0), offset));
            }
        }
        std::sort(table.begin(), table.end(), [](const FaceOffset& a, const FaceOffset& b) { return a.mBits < b.mBits; });
        const U32 back = recoverEroded(*chf, before, radius, 0, h, w, table);
        if (recovered)
        {
            *recovered = back;
        }
        std::vector<S32> walkable;
        for (S32 a = 0; a < across; ++a)
        {
            const S32 x = transposed ? a : at, z = transposed ? at : a;
            const rcCompactCell& c = chf->cells[x + z * w];
            for (U32 i = c.index; i < c.index + c.count; ++i)
            {
                if (chf->areas[i] != RC_NULL_AREA)
                {
                    walkable.push_back(a);
                }
            }
        }
        rcFreeCompactHeightfield(chf);
        rcFreeHeightField(hf);
        return walkable;
    }
}

namespace tut
{
    struct sknavfaceoffsets_data
    {
    };
    typedef test_group<sknavfaceoffsets_data> sknavfaceoffsets_group;
    typedef sknavfaceoffsets_group::object sknavfaceoffsets_object;
    tut::sknavfaceoffsets_group sknavfaceoffsets_tests("sknavfaceoffsets");

    template<> template<>
    void sknavfaceoffsets_object::test<1>()
    {
        set_test_name("a jamb 0.3 of a cell in reads 4 steps of 16, rounded towards the air, and its far face its own");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.wallX(2.f * CELL - 0.5f * CELL);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("both sides of the solid column face a floor", stats.mSides, 2u);
        ensure_equals("both are off the boundary", table.size(), static_cast<size_t>(2));
        ensure_equals("the -x face", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
        ensure_equals("the +x face", faceOffset(table, faceKey(1, 0, 3, 1)), 8.f / 16.f);
        ensure("the residual is under a step", stats.mResidualSum >= 0.0 && stats.mResidualSum < 2.0 * CELL / 16.0);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<2>()
    {
        set_test_name("a wall on the cell edge gives an offset of 0 and no entry");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL);
        s.wallX(2.f * CELL);
        s.slab(CELL, 2.f * CELL, 0.6f);
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("measured", stats.mMeasured, 2u);
        ensure("no entry", table.empty());
        ensure_equals("offset 0", faceOffset(table, faceKey(1, 0, 3, 0)), 0.f);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<3>()
    {
        set_test_name("a side measured over two floors keeps the nearer face, and one floor seeing the boundary keeps none");
        // Column 1 is solid through both floors' bands: the lower sees a jamb 0.3 in, the upper one 0.5 in.
        AirGrid g = jambGrid();
        AirGap upper;
        upper.mFloor = 30;
        upper.mCeiling = 60;
        upper.mFlags = AirGap::TOP | AirGap::AVATAR_FLOOR;
        AirGap lower = g.mGaps[0];
        lower.mCeiling = 25;
        g.mGaps = { lower, upper, g.mGaps[1], lower, upper };
        g.mGaps[2].mFloor = 55;
        g.mGaps[2].mCeiling = 60;
        g.mStart = { 0, 2, 3, 5 };
        Soup s;
        s.quad(LLVector3(1.3f * CELL, -1.f, 0.f), LLVector3(1.3f * CELL, 2.f, 0.f), LLVector3(1.3f * CELL, 2.f, 2.f), LLVector3(1.3f * CELL, -1.f, 2.f));
        s.quad(LLVector3(1.5f * CELL, -1.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 3.2f), LLVector3(1.5f * CELL, 2.f, 5.f), LLVector3(1.5f * CELL, -1.f, 5.f));
        std::vector<FaceOffset> table;
        FaceOffsetStats stats;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table, &stats);
        ensure_equals("each side measured for both floors", stats.mMeasured, 4u);
        ensure_equals("the nearer face, 4 steps", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
        // The upper floor's band meets a wall within a step of the boundary.
        s.quad(LLVector3(CELL + 0.002f, -1.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 3.2f), LLVector3(CELL + 0.002f, 2.f, 5.f), LLVector3(CELL + 0.002f, -1.f, 5.f));
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table);
        ensure_equals("none", faceOffset(table, faceKey(1, 0, 3, 0)), 0.f);
        ensure_equals("another side keeps its own", faceOffset(table, faceKey(1, 0, 3, 1)) > 0.f, true);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<4>()
    {
        set_test_name("a wall face on its cell boundary moves back to the true face behind it");
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 0.f);     // the floor it stands on runs under it
        std::vector<WallFace> faces(1);
        faces[0].mMin.set(CELL, 0.f, 0.f);
        faces[0].mMax.set(CELL, CELL, 2.f);
        faces[0].mSide = 1;     // looks -x, solid at +x
        alignWallFaces(faces, s.mVerts.data(), s.mTris.data(), s.ntris(), CELL, UNIT);
        ensure("moved to the wall", std::fabs(faces[0].mMin.mV[VX] - 1.3f * CELL) < 1e-4f && std::fabs(faces[0].mMax.mV[VX] - 1.3f * CELL) < 1e-4f);
        ensure_equals("along it, unmoved", faces[0].mMax.mV[VY], CELL);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<5>()
    {
        set_test_name("a slab resting on the top of the jamb band only touches it, and leaves the jamb's offset alone");
        const AirGrid g = jambGrid();
        Soup s;
        s.wallX(CELL + 0.3f * CELL);
        s.slab(0.f, 3.f * CELL, 15 * UNIT);
        std::vector<FaceOffset> table;
        computeFaceOffsets(g, s.mVerts.data(), s.mTris.data(), s.ntris(), CLIMB, table);
        ensure_equals("the -x face", faceOffset(table, faceKey(1, 0, 3, 0)), 4.f / 16.f);
    }
    template<> template<>
    void sknavfaceoffsets_object::test<6>()
    {
        set_test_name("a doorway that survives only with exact erosion: 0.75 m of cells, faces 0.75 of a cell into each wall");
        U32 back = 0;
        ensure("Recast alone leaves nothing", erodedCorridor(6, 0).empty());
        const std::vector<S32> kept = erodedCorridor(6, 12, &back);
        ensure_equals("two cells clear 0.3 m of both true faces", kept.size(), static_cast<size_t>(2));
        ensure("in the middle", kept[0] == 5 && kept[1] == 6);
        ensure("recovered along the corridor, not at its open ends", back > 0 && back < 2 * 16);
        ensure("the same across chf x", erodedCorridor(6, 12, nullptr, true) == kept);
        ensure("and nothing there without the offsets", erodedCorridor(6, 0, nullptr, true).empty());
        ensure("a doorway's jambs alone, far along a long corridor", erodedCorridor(6, 12, nullptr, false, 0.3f, 48, 36, 42, 39) == kept);
        ensure("and across chf x", erodedCorridor(6, 12, nullptr, true, 0.3f, 48, 36, 42, 39) == kept);
    }

    template<> template<>
    void sknavfaceoffsets_object::test<7>()
    {
        set_test_name("no floor comes back in a gap narrower than twice the radius, whatever its faces");
        ensure("0.375 m of cells, 0.56 m between faces", erodedCorridor(3, 12).empty());
        ensure("0.5 m of cells, 0.59 m between faces", erodedCorridor(4, 6).empty());
        ensure("0.5 m of cells, 0.73 m between faces, but no cell clears both", erodedCorridor(4, 15).empty());
    }

    template<> template<>
    void sknavfaceoffsets_object::test<8>()
    {
        set_test_name("only the offsets give floor back: a span the cell faces already clear stays Recast's to take");
        // At a 0.2 m radius the corridor's second cells clear the cell faces, though Recast took them at its 3 cells.
        ensure("a step of offset, nothing back", erodedCorridor(6, 1, nullptr, false, 0.2f).empty());
    }

    template<> template<>
    void sknavfaceoffsets_object::test<9>()
    {
        set_test_name("a padded table crops to the tile's own columns");
        std::vector<FaceOffset> padded = { FaceOffset(faceKey(1, 1, 8, 2), 3), FaceOffset(faceKey(2, 3, 8, 0), 5), FaceOffset(faceKey(7, 7, 8, 1), 9) };
        std::vector<FaceOffset> out;
        cropFaceOffsets(padded, 8, 2, 4, 4, out);
        ensure_equals("one inside", out.size(), static_cast<size_t>(1));
        ensure_equals("at its own column", out[0].key(), faceKey(0, 1, 4, 0));
        ensure_equals("with its offset", out[0].offset(), 5u);
    }
}
