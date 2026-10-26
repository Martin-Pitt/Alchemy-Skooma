/**
 * @file sklegendview_test.cpp
 * @brief The legend's layout, collapse and hover over the headless UI.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "sklegendsamples.h"
#include "sklegendview.h"
#include "../lluictrlfactory.h"

#include "alheadlessui_fixture.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    using Line = SKLegendView::Line;

    struct sklegendview_data
    {
        ll_test::HeadlessUI& ui = ll_test::HeadlessUI::get();

        static SKLegendView* make(S32 w, S32 h)
        {
            SKLegendView::Params p(LLUICtrlFactory::getDefaultParams<SKLegendView>());
            p.name = "legend";
            p.rect = LLRect(0, h, w, 0);
            return LLUICtrlFactory::create<SKLegendView>(p);
        }

        static const Line* find(const SKLegendView& v, Line::Kind k, S32 section = -2)
        {
            for (const Line& l : v.lines())
            {
                if (l.kind == k && (section == -2 || l.section == section))
                {
                    return &l;
                }
            }
            return nullptr;
        }
    };

    TEST_CASE_METHOD(sklegendview_data, "sklegendview: a legend that fits lays out every row inside the rect", "[sklegendview]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(220, 400);
        v->setLegend(SKLegendSamples::beacons());
        CHECK(v->hiddenRows() == 0);
        CHECK_FALSE(v->tooSmall());
        S32 rows = 0;
        S32 last_top = 400;
        for (const Line& l : v->lines())
        {
            CHECK(l.rect.mLeft >= 0);
            CHECK(l.rect.mRight <= 220);
            CHECK(l.rect.mBottom >= 0);
            CHECK(l.rect.mTop <= last_top);
            last_top = l.rect.mBottom;
            rows += l.kind == Line::Kind::Row ? 1 : 0;
        }
        CHECK(rows == 6);
        CHECK(v->contentHeight() <= 400);
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_data, "sklegendview: reshape never changes the rect and collapses to more then too small", "[sklegendview]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(220, 400);
        v->setLegend(SKLegendSamples::beacons());
        const S32 need = v->contentHeight();

        v->reshape(220, need - 20);
        CHECK(v->getRect().getHeight() == need - 20);
        CHECK(v->hiddenRows() > 0);
        REQUIRE(find(*v, Line::Kind::More) != nullptr);
        CHECK(find(*v, Line::Kind::More)->rect.mBottom >= 0);

        v->reshape(220, 40);
        CHECK(v->tooSmall());
        CHECK(find(*v, Line::Kind::Title) != nullptr);
        CHECK(find(*v, Line::Kind::Row) == nullptr);

        v->reshape(40, 400);
        CHECK(v->getRect().getWidth() == 40);
        CHECK(v->tooSmall());
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_data, "sklegendview: hovering a row or the ramp emits its key and leaving clears it", "[sklegendview]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(220, 400);
        v->setLegend(SKLegendSamples::nameTagDistance());
        std::vector<SKVizKey> heard;
        v->onHoverKey([&](const SKVizKey& k) { heard.push_back(k); });

        const Line* bar = find(*v, Line::Kind::RampBar);
        REQUIRE(bar != nullptr);
        v->handleHover(bar->rect.mLeft + 1, bar->rect.mBottom + 2, 0);
        REQUIRE(heard.size() == 1);
        CHECK(heard.back().scale == "name-tag-distance");
        CHECK(heard.back().category == 0);

        const Line* row = nullptr;
        for (const Line& l : v->lines())
        {
            if (l.kind == Line::Kind::Row && l.row == 2)
            {
                row = &l;
            }
        }
        REQUIRE(row != nullptr);
        v->handleHover(row->rect.mLeft + 30, row->rect.getCenterY(), 0);
        REQUIRE(heard.size() == 2);
        CHECK(heard.back().category == 2);
        CHECK(v->isRowLit(row->section, 2));
        CHECK_FALSE(v->isRowLit(row->section, 1));

        v->handleHover(row->rect.mLeft + 31, row->rect.getCenterY(), 0);
        CHECK(heard.size() == 2);

        v->onMouseLeave(0, 0, 0);
        REQUIRE(heard.size() == 3);
        CHECK(heard.back().empty());
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_data, "sklegendview: a continuous ramp hovers to its value and a highlight lights only the matching row", "[sklegendview]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(220, 600);
        auto          complexity = SKLegendSamples::avatarComplexityScale();
        v->setLegend(SKLegendSamples::avatarDrawInfo(complexity));
        const Line* bar = find(*v, Line::Kind::RampBar);
        REQUIRE(bar != nullptr);
        const S32      bw = std::min(bar->rect.getWidth(), 160);
        const SKVizKey k  = v->keyAt(bar->rect.mLeft + bw / 2, bar->rect.mBottom + 1);
        CHECK(k.scale == "avatar-complexity");
        CHECK(std::fabs(k.value - 0.5 * complexity->hi()) < complexity->hi() / bw);

        // A runtime domain change relayouts the ticks on the next draw.
        const F64 old_last = v->ticksOf(bar->section)->values.back();
        complexity->setDomain(0.0, 100000.0);
        v->refreshIfScalesChanged();
        CHECK(v->ticksOf(bar->section)->values.back() != old_last);

        v->setLegend(SKLegendSamples::updateType());
        SKVizKey h;
        h.scale    = "update-type";
        h.category = 1;
        v->setHighlight(h);
        S32 lit = 0;
        for (const Line& l : v->lines())
        {
            lit += (l.kind == Line::Kind::Row && v->isRowLit(l.section, l.row)) ? 1 : 0;
        }
        CHECK(lit == 1);
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_data, "sklegendview: ticks under every built-in ramp keep their labels apart", "[sklegendview]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(200, 2000);
        v->setLegend(SKLegendSamples::builtInScales());
        CHECK(v->hiddenRows() == 0);
        for (S32 s = 0; s < (S32)v->legend().sections.size(); ++s)
        {
            const SKTicks* t = v->ticksOf(s);
            REQUIRE(t != nullptr);
            CHECK(t->values.size() <= 7);
        }
        v->die();
    }
}
