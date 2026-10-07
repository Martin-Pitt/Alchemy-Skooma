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

    // The root pane's corner sits 4 px off the click; widths and heights follow the window.
    template<> template<>
    void skpopuplayout_object::test<1>()
    {
        const PopupRect r = PopupLayout::placeFirst(600.f, 300.f, 174.f, 100.f, 1280.f, 720.f);
        ensure_equals("x", r.mX, 604.f);
        ensure_equals("y", r.mY, 304.f);
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
                ensure("clamped", inside(PopupLayout::placeFirst(x, y, 174.f, 200.f, 1280.f, 720.f), 1280.f, 720.f));
            }
        }
    }

    // The second root pane: left of the first, else right, else below; the study's openContext.
    template<> template<>
    void skpopuplayout_object::test<2>()
    {
        // Room on the left.
        PopupRect first = PopupLayout::placeFirst(600.f, 300.f, 174.f, 150.f, 1280.f, 720.f);
        PopupRect beside = PopupLayout::placeBeside(first, 174.f, 100.f, 1280.f, 720.f);
        ensure("left", beside.mX == first.mX - 178.f && beside.mY == first.mY);
        // Against the left edge: right.
        first = PopupLayout::placeFirst(50.f, 300.f, 174.f, 150.f, 1280.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, 1280.f, 720.f);
        ensure("right", beside.mX == first.mX + 178.f && beside.mY == first.mY);
        // Against the right edge the first pane is pushed in and the second still goes left.
        first = PopupLayout::placeFirst(1270.f, 300.f, 174.f, 150.f, 1280.f, 720.f);
        ensure_equals("pushed in", first.mX, 1098.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, 1280.f, 720.f);
        ensure_equals("left of it", beside.mX, 920.f);
        // A window too narrow for two: below.
        first = PopupLayout::placeFirst(50.f, 100.f, 174.f, 150.f, 300.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, 300.f, 720.f);
        ensure("below", beside.mX == first.mX && beside.mY == first.bottom() + 4.f);
        // Near the bottom, below is clamped back in.
        first = PopupLayout::placeFirst(50.f, 600.f, 174.f, 150.f, 300.f, 720.f);
        beside = PopupLayout::placeBeside(first, 174.f, 100.f, 300.f, 720.f);
        ensure("inside", inside(beside, 300.f, 720.f));
    }

    // A child pane: left of its parent, else right, else under the root group; the study's
    // openPerson and openModerate.
    template<> template<>
    void skpopuplayout_object::test<3>()
    {
        // People pane left of the map pane: a person pane goes left of the people pane.
        const PopupRect map = PopupLayout::placeFirst(600.f, 300.f, 174.f, 200.f, 1280.f, 720.f);
        const PopupRect people = PopupLayout::placeBeside(map, 174.f, 120.f, 1280.f, 720.f);
        PopupRect person = PopupLayout::placeChild(people, people.mY + 26.f, 174.f, 250.f, { map }, 1280.f, 720.f);
        ensure("left of its parent", person.mX == people.mX - 176.f && person.mY == people.mY + 26.f);

        // Map pane at the left edge, people to its right: the person pane would cover the map
        // pane on the left, so it goes under both.
        const PopupRect map2 = PopupLayout::placeFirst(20.f, 100.f, 174.f, 200.f, 1280.f, 720.f);
        const PopupRect people2 = PopupLayout::placeBeside(map2, 174.f, 120.f, 1280.f, 720.f);
        person = PopupLayout::placeChild(people2, people2.mY + 4.f, 174.f, 250.f, { map2 }, 1280.f, 720.f);
        ensure("under the group", person.mX == people2.mX && person.mY == llmax(map2.bottom(), people2.bottom()) + 4.f);

        // No room on the left of a pane at the edge, and nothing on the right: right.
        const PopupRect lone = PopupLayout::placeFirst(0.f, 100.f, 174.f, 200.f, 1280.f, 720.f);
        const PopupRect moderate = PopupLayout::placeChild(lone, lone.mY + 50.f, 174.f, 130.f, {}, 1280.f, 720.f);
        ensure("right of its parent", moderate.mX == lone.right() + 2.f && moderate.mY == lone.mY + 50.f);
        ensure("inside", inside(moderate, 1280.f, 720.f));
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
        nav.hover(1, 2);
        ensure("a plain row shuts it", !nav.panes()[3].mOpen);
        nav.hover(1, 0);
        nav.hover(2, 0);
        ensure("in the child", nav.activePane() == 2 && nav.panes()[2].mOpen);
        ensure("headings and separators are never active", PopupNav::step({ heading(), separator() }, -1, 1) == -1);
    }
}
