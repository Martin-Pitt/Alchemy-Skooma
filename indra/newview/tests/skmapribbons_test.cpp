/**
 * @file skmapribbons_test.cpp
 * @brief Mini-map ribbons: widths in screen pixels, run merging, the length floor, cover, dashes.
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

#include "../skmapribbons.h"

#include "../skmappalette.h"

#include <cmath>

namespace
{
    skmap::LineMark mark(skmap::MarkKind kind, F32 x0, F32 y0, F32 x1, F32 y1, F32 height = 2.f, S8 side = 1)
    {
        skmap::LineMark m;
        m.mKind = kind;
        m.mA.set(x0, y0);
        m.mB.set(x1, y1);
        m.mHeight = height;
        m.mFloorSide = side;
        m.mLayer = 0;
        return m;
    }

    /// A scene of one layer holding the given marks.
    skmap::Scene scene(const std::vector<skmap::LineMark>& marks)
    {
        skmap::Scene s;
        s.mLayers.resize(1);
        for (const skmap::LineMark& m : marks)
        {
            s.mLayers[0].mMarks.push_back(static_cast<U32>(s.mMarks.size()));
            s.mMarks.push_back(m);
        }
        return s;
    }

    skmap::RibbonParams params(F32 mpp, F32 eye = 1.7f)
    {
        skmap::RibbonParams p;
        p.mMpp = mpp;
        p.mEye = eye;
        p.mClimb = 0.75f;
        p.mPeek = 0.15f;
        return p;
    }
}

namespace tut
{
    struct skmapribbons_data {};
    typedef test_group<skmapribbons_data> skmapribbons_group;
    typedef skmapribbons_group::object skmapribbons_object;
    tut::skmapribbons_group g("skmapribbons");

    // A ribbon's width in metres is its pixel width times the scale.
    template<> template<>
    void skmapribbons_object::test<1>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 10.f, 0.f, 2.f) });
        for (F32 mpp : { 0.05f, 0.5f, 2.f })
        {
            skmap::Ribbons r;
            skmap::buildLayerRibbons(s, 0, params(mpp * 0.999f), r);
            ensure_equals("one quad", r.mVerts.size(), static_cast<size_t>(6));
            F32 lo = 1e9f, hi = -1e9f;
            for (const LLVector3& v : r.mVerts)
            {
                lo = llmin(lo, v.mV[VY]);
                hi = llmax(hi, v.mV[VY]);
            }
            ensure("3 px of full cover", std::fabs((hi - lo) - 3.f * mpp * 0.999f) < 1e-4f);
            ensure_equals("colour per vertex", r.mColours.size(), r.mVerts.size());
        }
    }

    // Consecutive collinear marks of one kind merge; a turn or another kind does not.
    template<> template<>
    void skmapribbons_object::test<2>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 5.f, 0.f), mark(skmap::MarkKind::WALL, 5.1f, 0.f, 10.f, 0.f),
                                       mark(skmap::MarkKind::WALL, 10.f, 0.f, 10.f, 6.f), mark(skmap::MarkKind::LEDGE, 10.f, 6.f, 16.f, 6.f),
                                       mark(skmap::MarkKind::WALL, 15.f, 0.f, 10.1f, 0.f, 2.f, -1) });
        std::vector<skmap::MarkRun> runs;
        skmap::mergeMarks(s, 0, params(0.1f), runs);
        ensure_equals("three runs", runs.size(), static_cast<size_t>(3));
        ensure("merged across the gap and the reversed mark", std::fabs(runs[0].length() - 15.f) < 1e-3f);
        ensure("the turn stays apart", std::fabs(runs[1].length() - 6.f) < 1e-3f);
        ensure("the ledge is its own", runs[2].mKind == skmap::MarkKind::LEDGE);
    }

    // Runs shorter than max(2 m, 3 px) are dropped.
    template<> template<>
    void skmapribbons_object::test<3>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 1.5f, 0.f), mark(skmap::MarkKind::WALL, 0.f, 10.f, 2.5f, 10.f),
                                       mark(skmap::MarkKind::DROP_EDGE, 0.f, 20.f, 1.f, 20.f) });
        std::vector<skmap::MarkRun> runs;
        skmap::mergeMarks(s, 0, params(0.1f), runs);
        ensure_equals("2 m floor", runs.size(), static_cast<size_t>(1));
        skmap::mergeMarks(s, 0, params(1.f), runs);
        ensure_equals("3 px floor at 1 m a pixel", runs.size(), static_cast<size_t>(0));
    }

    // Cover against the eye height: 2 m is full at 1.7 m, 1 m is half, 0.5 m is none.
    template<> template<>
    void skmapribbons_object::test<4>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 4.f, 0.f, 2.f), mark(skmap::MarkKind::WALL, 0.f, 10.f, 4.f, 10.f, 1.f),
                                       mark(skmap::MarkKind::WALL, 0.f, 20.f, 4.f, 20.f, 0.5f) });
        std::vector<skmap::MarkRun> runs;
        skmap::mergeMarks(s, 0, params(0.05f, 1.7f), runs);
        ensure_equals("three runs", runs.size(), static_cast<size_t>(3));
        ensure("full", runs[0].mCover == sknav::Cover::FULL);
        ensure("half", runs[1].mCover == sknav::Cover::HALF);
        ensure("none", runs[2].mCover == sknav::Cover::NONE);
        // Half cover draws its shields close up, and only then.
        skmap::Ribbons close_up, far_out;
        skmap::buildLayerRibbons(scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 4.f, 0.f, 1.f) }), 0, params(0.05f), close_up);
        skmap::buildLayerRibbons(scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 4.f, 0.f, 1.f) }), 0, params(0.2f), far_out);
        ensure("shields close up", close_up.mVerts.size() > 6);
        ensure_equals("just the line far out", far_out.mVerts.size(), static_cast<size_t>(6));
        skmap::RibbonParams off = params(0.05f);
        off.mCover = false;
        skmap::buildLayerRibbons(s, 0, off, close_up);
        ensure("cover off", close_up.mVerts.empty());
    }

    // Region edges dash 6 px on and 4 off; void edges are solid.
    template<> template<>
    void skmapribbons_object::test<5>()
    {
        skmap::Scene s;
        skmap::LineMark region = mark(skmap::MarkKind::REGION_BOUNDARY, 0.f, 0.f, 100.f, 0.f);
        region.mLayer = -1;
        s.mBoundaries.push_back(region);
        skmap::Ribbons r;
        skmap::buildBoundaryRibbons(s, params(1.f), r);
        ensure_equals("ten dashes", r.mVerts.size(), static_cast<size_t>(60));
        F32 lo = 1e9f, hi = -1e9f;
        for (size_t i = 0; i < 6; ++i)
        {
            lo = llmin(lo, r.mVerts[i].mV[VX]);
            hi = llmax(hi, r.mVerts[i].mV[VX]);
        }
        ensure("6 m on at 1 m a pixel", std::fabs(hi - lo - 6.f) < 1e-4f);
        ensure("next dash 10 m on", std::fabs(r.mVerts[6].mV[VX] - 10.f) < 1e-4f || std::fabs(r.mVerts[7].mV[VX] - 10.f) < 1e-4f);

        s.mBoundaries.front().mKind = skmap::MarkKind::VOID_BOUNDARY;
        skmap::buildBoundaryRibbons(s, params(1.f), r);
        ensure_equals("void is solid", r.mVerts.size(), static_cast<size_t>(6));
    }

    // The same marks build the same triangles.
    template<> template<>
    void skmapribbons_object::test<6>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 0.f, 0.f, 5.f, 0.f), mark(skmap::MarkKind::LEDGE, 0.f, 3.f, 6.f, 3.f),
                                       mark(skmap::MarkKind::OVERHANG, 0.f, 6.f, 6.f, 6.f), mark(skmap::MarkKind::DROP_POINT, 1.f, 1.f, 1.f, 0.f),
                                       mark(skmap::MarkKind::LADDER, 2.f, 2.f, 2.f, 2.f), mark(skmap::MarkKind::DOOR_LEAF, 0.f, 9.f, 1.f, 9.f) });
        skmap::Ribbons a, b;
        skmap::buildLayerRibbons(s, 0, params(0.05f), a);
        skmap::buildLayerRibbons(s, 0, params(0.05f), b);
        ensure("something drawn", !a.mVerts.empty());
        ensure("identical", a.mVerts == b.mVerts && a.mColours == b.mColours);
    }

    // A cut cell drops the marks whose midpoint, or a glyph's anchor, lies in it; the rest stay.
    template<> template<>
    void skmapribbons_object::test<7>()
    {
        skmap::Scene s = scene({ mark(skmap::MarkKind::WALL, 2.f, 1.f, 6.f, 1.f), mark(skmap::MarkKind::LADDER, 20.f, 20.f, 20.f, 20.f),
                                 mark(skmap::MarkKind::LEDGE, 3.f, 9.f, 9.f, 9.f) });
        s.mOrigin.set(256.0, 512.0, 0.0);
        std::vector<skmap::MarkRun> all, kept;
        skmap::mergeMarks(s, 0, params(0.05f), all);
        ensure_equals("three runs uncut", all.size(), static_cast<size_t>(3));

        // The wall spans global x 258-262 at y 513, cells (64, 128) and (65, 128); the ladder is at (276, 532), cell (69, 133).
        const skmap::CutCells wall_cut = { { 64, 128 }, { 65, 128 } };
        skmap::mergeMarks(s, 0, params(0.05f), kept, &wall_cut);
        ensure_equals("the wall goes", kept.size(), static_cast<size_t>(2));
        for (const skmap::MarkRun& r : kept)
        {
            ensure("no wall left", r.mKind != skmap::MarkKind::WALL);
        }
        const skmap::CutCells glyph_cut = { { 69, 133 } };
        skmap::mergeMarks(s, 0, params(0.05f), kept, &glyph_cut);
        ensure_equals("the ladder goes", kept.size(), static_cast<size_t>(2));

        // An empty cut, or one elsewhere, builds what no cut builds.
        skmap::Ribbons uncut, none, elsewhere;
        const skmap::CutCells empty, distant = { { 0, 0 } };
        skmap::buildLayerRibbons(s, 0, params(0.05f), uncut);
        skmap::buildLayerRibbons(s, 0, params(0.05f), none, &empty);
        skmap::buildLayerRibbons(s, 0, params(0.05f), elsewhere, &distant);
        ensure("empty cut is no cut", uncut.mVerts == none.mVerts);
        ensure("a far cut changes nothing", uncut.mVerts == elsewhere.mVerts);

        // A long wall loses only the cells the cut covers, global x 256-268 over cells 64-66.
        skmap::Scene w = scene({ mark(skmap::MarkKind::WALL, 0.f, 1.f, 12.f, 1.f) });
        w.mOrigin.set(256.0, 512.0, 0.0);
        const skmap::CutCells middle = { { 65, 128 } };
        skmap::mergeMarks(w, 0, params(0.05f), kept, &middle);
        ensure_equals("both ends stay", kept.size(), static_cast<size_t>(2));
        ensure("west end", std::fabs(kept[0].mA.mV[VX]) < 1e-4f && std::fabs(kept[0].mB.mV[VX] - 4.f) < 1e-4f);
        ensure("east end", std::fabs(kept[1].mA.mV[VX] - 8.f) < 1e-4f && std::fabs(kept[1].mB.mV[VX] - 12.f) < 1e-4f);
        const skmap::CutCells west = { { 64, 128 } };
        skmap::mergeMarks(w, 0, params(0.05f), kept, &west);
        ensure_equals("the uncut pieces join again", kept.size(), static_cast<size_t>(1));
        ensure("from the cut's edge", std::fabs(kept[0].mA.mV[VX] - 4.f) < 1e-4f && std::fabs(kept[0].mB.mV[VX] - 12.f) < 1e-4f);
    }

    // Door marks survive merging at any length and never join a wall.
    template<> template<>
    void skmapribbons_object::test<8>()
    {
        const skmap::Scene s = scene({ mark(skmap::MarkKind::DOOR_SHUT, 0.f, 0.f, 0.3f, 0.f), mark(skmap::MarkKind::WALL, 0.3f, 0.f, 5.f, 0.f),
                                       mark(skmap::MarkKind::DOOR_LEAF, 5.f, 0.f, 5.3f, 0.f) });
        std::vector<skmap::MarkRun> runs;
        skmap::mergeMarks(s, 0, params(1.f), runs);
        bool shut = false, leaf = false, wall = false;
        for (const skmap::MarkRun& r : runs)
        {
            shut = shut || (r.mKind == skmap::MarkKind::DOOR_SHUT && std::fabs(r.length() - 0.3f) < 1e-4f);
            leaf = leaf || (r.mKind == skmap::MarkKind::DOOR_LEAF && std::fabs(r.length() - 0.3f) < 1e-4f);
            wall = wall || (r.mKind == skmap::MarkKind::WALL && std::fabs(r.length() - 4.7f) < 1e-4f);
        }
        ensure("the shut bar survives", shut);
        ensure("the leaf survives", leaf);
        ensure("the wall is its own", wall);
    }

    // A quarter swing is nine segments on its circle.
    template<> template<>
    void skmapribbons_object::test<9>()
    {
        skmap::LineMark swing = mark(skmap::MarkKind::DOOR_SWING, 0.f, 0.f, 1.f, 0.f);
        swing.mSweep = F_PI_BY_TWO;
        const skmap::Scene s = scene({ swing });
        skmap::Ribbons r;
        skmap::buildLayerRibbons(s, 0, params(0.05f), r);
        ensure_equals("nine segments", r.mVerts.size(), static_cast<size_t>(9 * 6));
        for (size_t i = 0; i < r.mVerts.size(); ++i)
        {
            const LLVector2 anchor(r.mVerts[i].mV[VX] - r.mOffsets[i].mV[VX] * r.mMpp, r.mVerts[i].mV[VY] - r.mOffsets[i].mV[VY] * r.mMpp);
            ensure("on the circle", std::fabs(anchor.length() - 1.f) < 1e-4f);
        }
    }

    // The doors toggle owns every door kind; the cover toggle none.
    template<> template<>
    void skmapribbons_object::test<10>()
    {
        skmap::LineMark swing = mark(skmap::MarkKind::DOOR_SWING, 0.f, 4.f, 1.f, 4.f);
        swing.mSweep = 1.f;
        const skmap::Scene s = scene({ mark(skmap::MarkKind::DOOR_SHUT, 0.f, 0.f, 1.f, 0.f), mark(skmap::MarkKind::DOOR_THRESHOLD, 0.f, 1.f, 1.f, 1.f),
                                       mark(skmap::MarkKind::DOOR_LEAF, 0.f, 2.f, 1.f, 2.f), mark(skmap::MarkKind::DOOR_JAMB, 0.f, 3.f, 0.f, 3.4f), swing });
        skmap::RibbonParams no_doors = params(0.05f);
        no_doors.mDoors = false;
        std::vector<skmap::MarkRun> runs;
        skmap::mergeMarks(s, 0, no_doors, runs);
        ensure("doors off drops them all", runs.empty());
        skmap::RibbonParams no_cover = params(0.05f);
        no_cover.mCover = false;
        skmap::mergeMarks(s, 0, no_cover, runs);
        ensure_equals("cover off keeps them", runs.size(), static_cast<size_t>(5));
    }

    // Every door kind draws in the door colour, the swing fainter.
    template<> template<>
    void skmapribbons_object::test<11>()
    {
        auto u = [](F32 v) { return static_cast<U8>(llclamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        const skmap::PaletteColour& c = skmap::palette::DOOR;
        const LLColor4U door(u(c.mR), u(c.mG), u(c.mB), u(c.mA));
        const LLColor4U faint(u(c.mR), u(c.mG), u(c.mB), u(skmap::palette::DOOR_SWING_ALPHA));
        for (skmap::MarkKind kind : { skmap::MarkKind::DOOR_SHUT, skmap::MarkKind::DOOR_THRESHOLD, skmap::MarkKind::DOOR_LEAF,
                                      skmap::MarkKind::DOOR_JAMB, skmap::MarkKind::DOOR_SWING })
        {
            skmap::LineMark m = mark(kind, 0.f, 0.f, 1.f, 0.f);
            m.mSweep = kind == skmap::MarkKind::DOOR_SWING ? 1.f : 0.f;
            skmap::Ribbons r;
            skmap::buildLayerRibbons(scene({ m }), 0, params(0.05f), r);
            ensure("drawn", !r.mColours.empty());
            for (const LLColor4U& colour : r.mColours)
            {
                ensure("the door colour", colour == (kind == skmap::MarkKind::DOOR_SWING ? faint : door));
            }
        }
    }
}
