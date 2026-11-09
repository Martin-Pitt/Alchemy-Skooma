/**
 * @file sklegendview_gl_test.cpp
 * @brief The legend drawn into llrender's hidden window and read back.
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

#include "llglheaders.h"

#include "aluigl_fixture.h"

#include "sklegendsamples.h"
#include "sklegendview.h"
#include "../lluictrlfactory.h"

#include "alheadlessui_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace
{
    constexpr S32 W = ll_test::HeadlessGL::WIDTH;
    constexpr S32 H = ll_test::HeadlessGL::HEIGHT;
    using Line      = SKLegendView::Line;

    struct Pixels
    {
        std::vector<U8> rgba;
        const U8*       at(S32 x, S32 y) const { return &rgba[((size_t)y * W + x) * 4]; }
    };

    Pixels drawn(const std::function<void()>& draw)
    {
        ll_test::uiGL().clearFramebuffer();
        glEnable(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        draw();
        gGL.flush();
        glDisable(GL_BLEND);
        glFinish();
        return { ll_test::readFramebufferRGBA(W, H) };
    }

    // With SK_VIZ_OUT set, the frame as a PPM in that directory, top row first.
    void writeOut(const Pixels& p, const std::string& name)
    {
        const char* dir = std::getenv("SK_VIZ_OUT");
        if (!dir || !*dir)
        {
            return;
        }
        const std::string path = std::string(dir) + "/" + name + ".ppm";
        FILE*             f    = fopen(path.c_str(), "wb");
        if (!f)
        {
            return;
        }
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (S32 y = H - 1; y >= 0; --y)
        {
            for (S32 x = 0; x < W; ++x)
            {
                fwrite(p.at(x, y), 1, 3, f);
            }
        }
        fclose(f);
    }

    bool within(const U8* px, const LLColor4& c, S32 tol)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            if (std::abs((S32)px[k] - (S32)SKColorScale::toByte(c.mV[k])) > tol)
            {
                return false;
            }
        }
        return true;
    }

    struct sklegendview_gl_data
    {
        sklegendview_gl_data()
        {
            ll_test::uiGL();
            ll_test::installWhiteTexture();
        }

        ll_test::HeadlessUI& ui = ll_test::HeadlessUI::get(/*gl_textures=*/true);

        static SKLegendView* make(S32 w, S32 h)
        {
            SKLegendView::Params p(LLUICtrlFactory::getDefaultParams<SKLegendView>());
            p.name = "legend";
            p.rect = LLRect(0, h, w, 0);
            return LLUICtrlFactory::create<SKLegendView>(p);
        }
    };

    TEST_CASE_METHOD(sklegendview_gl_data, "sklegendview_gl: a ramp's ends are its stops and nothing is drawn outside the rect", "[sklegendview_gl][gl]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegend l;
        l.title = "heat";
        l.sections.push_back(SKLegendSection::ramp(SKColorScales::heat(), "clearance", "m"));
        SKLegendView* v = make(200, 120);
        v->setLegend(l);
        const Pixels p = drawn([&] { v->draw(); });
        writeOut(p, "gl_heat_ramp");
        const Line* bar = nullptr;
        for (const Line& line : v->lines())
        {
            bar = line.kind == Line::Kind::RampBar ? &line : bar;
        }
        REQUIRE(bar != nullptr);
        const S32 y  = bar->rect.mBottom + 3;
        const S32 bw = std::min(bar->rect.getWidth(), 160);
        CHECK(within(p.at(bar->rect.mLeft, y), SKColorScales::heat().stops().front().c, 6));
        CHECK(within(p.at(bar->rect.mLeft + bw - 1, y), SKColorScales::heat().stops().back().c, 6));
        CHECK(within(p.at(bar->rect.mLeft + bw / 2, y), SKColorScales::heat().stops()[1].c, 6));
        for (S32 yy = 0; yy < H; ++yy)
        {
            for (S32 xx = 0; xx < W; ++xx)
            {
                if (xx >= 200 || yy >= 120)
                {
                    const U8* px = p.at(xx, yy);
                    if (px[0] || px[1] || px[2])
                    {
                        CAPTURE(xx, yy);
                        FAIL("drawn outside the rect");
                    }
                }
            }
        }
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_gl_data, "sklegendview_gl: swatches are their colours and a highlight changes only its row", "[sklegendview_gl][gl]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        SKLegendView* v = make(200, 160);
        const SKLegend l = SKLegendSamples::updateType();
        v->setLegend(l);
        const Pixels before = drawn([&] { v->draw(); });
        const Line*  row1   = nullptr;
        for (const Line& line : v->lines())
        {
            if (line.kind == Line::Kind::Row && line.section == 0)
            {
                const SKLegendMark& m = l.sections[0].rows[line.row].marks[0];
                CAPTURE(line.row);
                CHECK(within(before.at(line.rect.mLeft + 5, line.rect.getCenterY()), m.colour, 2));
                row1 = line.row == 1 ? &line : row1;
            }
        }
        REQUIRE(row1 != nullptr);
        SKVizKey k;
        k.scale    = "update-type";
        k.category = 1;
        v->setHighlight(k);
        const Pixels after = drawn([&] { v->draw(); });
        writeOut(after, "gl_update_type_highlight");
        S32 changed = 0;
        for (S32 yy = 0; yy < H; ++yy)
        {
            for (S32 xx = 0; xx < W; ++xx)
            {
                if (std::memcmp(before.at(xx, yy), after.at(xx, yy), 3) != 0)
                {
                    ++changed;
                    if (!row1->rect.pointInRect(xx, yy))
                    {
                        CAPTURE(xx, yy);
                        FAIL("the highlight changed a pixel outside its row");
                    }
                }
            }
        }
        CHECK(changed > 0);
        v->die();
    }

    TEST_CASE_METHOD(sklegendview_gl_data, "sklegendview_gl: every sample draws inside its rect for the visual record", "[sklegendview_gl][gl]")
    {
        if (!ui.ok())
        {
            SKIP("no UI");
        }
        auto complexity = SKLegendSamples::avatarComplexityScale();
        const std::vector<std::pair<std::string, SKLegend>> samples = {
            { "sample_avatar_draw_info", SKLegendSamples::avatarDrawInfo(complexity) },
            { "sample_update_type", SKLegendSamples::updateType() },
            { "sample_lod_info", SKLegendSamples::lodInfo() },
            { "sample_buffer_view", SKLegendSamples::bufferView() },
            { "sample_name_tag_distance", SKLegendSamples::nameTagDistance() },
            { "sample_beacons", SKLegendSamples::beacons() },
        };
        for (const auto& [name, legend] : samples)
        {
            SKLegendView* v = make(220, 256);
            v->setLegend(legend);
            CAPTURE(name);
            CHECK(v->hiddenRows() == 0);
            writeOut(drawn([&] { v->draw(); }), name);
            v->die();
        }
        // The built-ins, a few sections a frame.
        const SKLegend all = SKLegendSamples::builtInScales();
        for (size_t first = 0, page = 0; first < all.sections.size(); first += 3, ++page)
        {
            SKLegend part;
            part.title = all.title + " " + std::to_string(page + 1);
            for (size_t s = first; s < std::min(all.sections.size(), first + 3); ++s)
            {
                part.sections.push_back(all.sections[s]);
            }
            SKLegendView* v = make(220, 256);
            v->setLegend(part);
            writeOut(drawn([&] { v->draw(); }), "scales_" + std::to_string(page + 1));
            v->die();
        }
        // The collapse, drawn.
        SKLegendView* v = make(220, 90);
        v->setLegend(SKLegendSamples::beacons());
        writeOut(drawn([&] { v->draw(); }), "collapse_more");
        v->reshape(220, 34);
        writeOut(drawn([&] { v->draw(); }), "collapse_too_small");
        v->die();
    }
}
