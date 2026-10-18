/**
 * @file skpopuplayout_test.cpp
 * @brief Side-by-side popup panes: placement at the window's edges and keyboard moves.
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

#include "../skpopuplayout.h"

namespace
{
    using sk::PopupLayout;
    using sk::PopupNav;
    using sk::PopupRect;
    using sk::PopupRow;

    PopupRow action(S32 opens = -1, bool enabled = true)
    {
        PopupRow r;
        r.mOpens = opens;
        r.mEnabled = enabled;
        return r;
    }
    PopupRow heading()
    {
        PopupRow r;
        r.mKind = PopupRow::Kind::HEADING;
        return r;
    }
    PopupRow separator()
    {
        PopupRow r;
        r.mKind = PopupRow::Kind::SEPARATOR;
        return r;
    }

    bool inside(const PopupRect& r, F32 w, F32 h)
    {
        return r.mX >= 8.f - 1e-3f && r.mY >= 8.f - 1e-3f && r.right() <= w - 8.f + 1e-3f && r.bottom() <= h - 8.f + 1e-3f;
    }
}

namespace tut
{
    struct skpopuplayout_data {};
    typedef test_group<skpopuplayout_data> skpopuplayout_group;
    typedef skpopuplayout_group::object skpopuplayout_object;
    tut::skpopuplayout_group g("skpopuplayout");

    // The root pane's near corner sits 4 px off the click, growing towards the centre.
    template<> template<>
    void skpopuplayout_object::test<1>()
    {
        const PopupFacing rd { true, true };
        PopupRect r = PopupLayout::placeFirst(600.f, 300.f, 174.f, 100.f, rd, 1280.f, 720.f);
        ensure_equals("x", r.mX, 604.f);
        ensure_equals("y", r.mY, 304.f);
        r = PopupLayout::placeFirst(1000.f, 600.f, 174.f, 100.f, { false, false }, 1280.f, 720.f);
        ensure("left and up", r.right() == 996.f && r.bottom() == 596.f);
        const PopupFacing f = PopupFacing::towardsCentre(100.f, 600.f, 1280.f, 720.f);
        ensure("facing", f.mRight && !f.mDown);
        ensure_equals("width", PopupLayout::paneWidth(1280.f), 174.f);
        ensure_equals("narrow width", PopupLayout::paneWidth(150.f), 134.f);
        const std::vector<PopupRow> rows = { heading(), action(), separator(), action() };
        ensure_equals("height", PopupLayout::contentHeight(rows), 8.f + 22.f * 3.f + 9.f);
        ensure_equals("row top", PopupLayout::rowTop(rows, 3), 4.f + 22.f * 2.f + 9.f);
        std::vector<PopupRow> many(60, action());
        ensure_equals("capped at the window", PopupLayout::paneHeight(many, 400.f), 384.f);
        // Clamped 8 px inside at every edge.
        for (F32 x : { -50.f, 0.f, 600.f, 1270.f, 1400.f })
        {
            for (F32 y : { -50.f, 0.f, 300.f, 715.f })
            {
                const PopupFacing c = PopupFacing::towardsCentre(x, y, 1280.f, 720.f);
                ensure("clamped", inside(PopupLayout::placeFirst(x, y, 174.f, 200.f, c, 1280.f, 720.f), 1280.f, 720.f));
            }
        }
    }

    // The second root pane: the facing side, else the other, else stacked the facing way.
    template<> template<>
    void skpopuplayout_object::test<2>()
    {
        const PopupFacing rd { true, true };
        PopupRect first = PopupLayout::placeFirst(50.f, 300.f, 174.f, 150.f, rd, 1280.f, 720.f);
        PopupRect beside = PopupLayout::placeBeside(first, 174.f, 100.f, rd, 1280.f, 720.f);
        ensure("right", beside.mX == first.right() + 4.f && beside.mY == first.mY);
        const PopupFacing lu { false, false };
        first = PopupLayout::placeFirst(1200.f, 600.f, 174.f, 150.f, lu, 1280.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, lu, 1280.f, 720.f);
        ensure("left, bottoms level", beside.right() == first.mX - 4.f && beside.bottom() == first.bottom());
        // Facing right with no room there: left.
        first = { 1090.f, 300.f, 174.f, 150.f };
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, rd, 1280.f, 720.f);
        ensure_equals("flipped left", beside.mX, 1090.f - 178.f);
        // A window too narrow for two: below.
        first = PopupLayout::placeFirst(50.f, 100.f, 174.f, 150.f, rd, 300.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, rd, 300.f, 720.f);
        ensure("below", beside.mX == first.mX && beside.mY == first.bottom() + 4.f);
        first = PopupLayout::placeFirst(50.f, 600.f, 174.f, 150.f, rd, 300.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, rd, 300.f, 720.f);
        ensure("inside", inside(beside, 300.f, 720.f));
    }

    // A child pane sits beside its opener row like a submenu, never detached below the group.
    template<> template<>
    void skpopuplayout_object::test<3>()
    {
        // The screenshot case: click at the top left, map then people to the right, person beside its row.
        const PopupFacing rd = PopupFacing::towardsCentre(40.f, 20.f, 1280.f, 720.f);
        const PopupRect map = PopupLayout::placeFirst(40.f, 20.f, 174.f, 120.f, rd, 1280.f, 720.f);
        const PopupRect people = PopupLayout::placeBeside(map, 174.f, 118.f, rd, 1280.f, 720.f);
        const F32 row = people.mY + 26.f;
        PopupRect person = PopupLayout::placeChild(people, row, 22.f, 174.f, 250.f, rd, { map }, 1280.f, 720.f);
        ensure("right of its row", person.mX == people.right() + 2.f && person.mY == row);

        // Bottom right, facing left and up: left of the people pane, bottom on the row's bottom.
        const PopupFacing lu = PopupFacing::towardsCentre(1200.f, 700.f, 1280.f, 720.f);
        const PopupRect map2 = PopupLayout::placeFirst(1200.f, 700.f, 174.f, 120.f, lu, 1280.f, 720.f);
        const PopupRect people2 = PopupLayout::placeBeside(map2, 174.f, 118.f, lu, 1280.f, 720.f);
        const F32 row2 = people2.mY + 48.f;
        person = PopupLayout::placeChild(people2, row2, 22.f, 174.f, 250.f, lu, { map2 }, 1280.f, 720.f);
        ensure("left, bottom on the row", person.right() == people2.mX - 2.f && person.bottom() == row2 + 22.f);

        // No room on the facing side: the other side, level with the row.
        const PopupRect edge { 1098.f, 100.f, 174.f, 200.f };
        PopupRect moderate = PopupLayout::placeChild(edge, 150.f, 22.f, 174.f, 130.f, rd, {}, 1280.f, 720.f);
        ensure("flipped left", moderate.mX == edge.mX - 176.f && moderate.mY == 150.f);
        // Too tall below the row: shifted up, not sideways.
        moderate = PopupLayout::placeChild(edge, 650.f, 22.f, 174.f, 130.f, lu, {}, 1280.f, 720.f);
        ensure("shifted inside", moderate.mX == edge.mX - 176.f && inside(moderate, 1280.f, 720.f));
    }

    // Keys: Up and Down wrap and skip what cannot be chosen; Home and End; Right opens a pane
    // and focuses its first row; Left goes back to the row that opened it; Escape closes all.
    template<> template<>
    void skpopuplayout_object::test<4>()
    {
        std::vector<PopupNav::Pane> panes(3);
        panes[0].mRows = { action(), separator(), action(-1, false), action(), action(2) };
        panes[1].mRows = { heading(), action(), action() };
        panes[2].mParent = 0;
        panes[2].mRows = { heading(), action(), action() };
        PopupNav nav;
        nav.reset(panes);
        ensure("roots open, children shut", nav.panes()[0].mOpen && nav.panes()[1].mOpen && !nav.panes()[2].mOpen);
        ensure_equals("first row", nav.activeRow(), 0);
        nav.key(sk::PopupKey::DOWN);
        ensure_equals("skips the separator and the disabled row", nav.activeRow(), 3);
        nav.key(sk::PopupKey::DOWN);
        ensure_equals("the opener", nav.activeRow(), 4);
        nav.key(sk::PopupKey::DOWN);
        ensure_equals("wraps", nav.activeRow(), 0);
        nav.key(sk::PopupKey::UP);
        ensure_equals("wraps back", nav.activeRow(), 4);
        nav.key(sk::PopupKey::HOME);
        ensure_equals("home", nav.activeRow(), 0);
        nav.key(sk::PopupKey::END);
        ensure_equals("end", nav.activeRow(), 4);
        ensure("enter on an action activates", nav.key(sk::PopupKey::HOME) == PopupNav::Result::MOVED
                                                    && nav.key(sk::PopupKey::ENTER) == PopupNav::Result::ACTIVATE);
        nav.key(sk::PopupKey::END);
        ensure("right opens", nav.key(sk::PopupKey::RIGHT) == PopupNav::Result::OPENED && nav.panes()[2].mOpen);
        ensure("into its first row", nav.activePane() == 2 && nav.activeRow() == 1);
        ensure("left closes", nav.key(sk::PopupKey::LEFT) == PopupNav::Result::CLOSED_CHILD && !nav.panes()[2].mOpen);
        ensure("back on the opener", nav.activePane() == 0 && nav.activeRow() == 4);
        ensure("escape closes all", nav.key(sk::PopupKey::ESCAPE) == PopupNav::Result::CLOSE_ALL);
    }

    // The pointer: hovering an opener opens its pane and shuts its siblings; a plain row shuts them.
    template<> template<>
    void skpopuplayout_object::test<5>()
    {
        std::vector<PopupNav::Pane> panes(4);
        panes[0].mRows = { action() };
        panes[1].mRows = { action(2), action(3), action() };
        panes[2].mParent = 1;
        panes[2].mRows = { action() };
        panes[3].mParent = 1;
        panes[3].mRows = { action() };
        PopupNav nav;
        nav.reset(panes);
        ensure("opens", nav.hover(1, 0) == PopupNav::Result::OPENED && nav.panes()[2].mOpen);
        nav.hover(1, 1);
        ensure("the sibling shuts", nav.panes()[3].mOpen && !nav.panes()[2].mOpen);
        ensure("a plain row shuts it, and says so", nav.hover(1, 2) == PopupNav::Result::CLOSED_CHILD && !nav.panes()[3].mOpen);
        ensure("nothing more to shut", nav.hover(1, 2) == PopupNav::Result::NONE);
        nav.hover(1, 0);
        nav.hover(2, 0);
        ensure("in the child", nav.activePane() == 2 && nav.panes()[2].mOpen);
        ensure("headings and separators are never active", PopupNav::step({ heading(), separator() }, -1, 1) == -1);
    }
}
