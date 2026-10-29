/**
 * @file sktextellipsis_gl_test.cpp
 * @brief Tests for the middle and start ellipsis as LLFontGL and
 *        LLFontTextCache draw them, on the shared context.
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

#include "../llfontgl.h"
#include "../llfonttextcache.h"
#include "../sktextellipsis.h"

#include "llheadlessgl_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

namespace
{
#ifndef LLFONT_TEST_APP_DIR
#  define LLFONT_TEST_APP_DIR ""
#endif
#ifndef LLFONT_TEST_FONTS_XML
#  define LLFONT_TEST_FONTS_XML ""
#endif

    constexpr const char* kAppDir   = LLFONT_TEST_APP_DIR;
    constexpr const char* kFontsXml = LLFONT_TEST_FONTS_XML;

    bool fileExists(const std::string& path)
    {
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            std::fclose(f);
            return true;
        }
        return false;
    }

    // The shared context and the fonts in it, made once for the process as the other font suites make them.
    struct sktextellipsis_gl_data
    {
        ll_test::HeadlessGL& gl = ll_test::sharedHeadlessGL();
        ll_test::FontStateScope scope;

        sktextellipsis_gl_data()
        {
            LLFontTextCache::enableBufferCollection(true);
            LLFontTextCache::enableColorOnlyRegen(true);
            static bool initialized = false;
            if (!initialized && fileExists(kFontsXml))
            {
                LLFontGL::initClass(96.f, 1.f, 1.f, kAppDir, kFontsXml, LLSD(), /*create_gl_textures=*/true);
                initialized = true;
            }
        }
    };

    const std::string kLabel = "Hair v2 - RIGGED (no copy) Lumipro Body #12";

    TEST_CASE_METHOD(sktextellipsis_gl_data, "SKTextEllipsisGL: a middle cut draws within its room and counts the original's bytes", "[SKTextEllipsisGL]")
    {
        if (!fileExists(kFontsXml))
            SKIP("fonts.xml not present in test data dir");
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        REQUIRE(font != nullptr);

        const S32 max_pixels = (S32)(font->getWidthF32(kLabel) * 0.5f);
        for (LLFontGL::EEllipsisMode mode : { LLFontGL::ELLIPSIS_MIDDLE, LLFontGL::ELLIPSIS_START })
        {
            CAPTURE(mode);
            F32 right_x = 0.f;
            const S32 drawn = font->renderBytes(kLabel, 0, 10.f, 100.f, LLColor4::white, LLFontGL::LEFT,
                                                LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                                S32_MAX, max_pixels, &right_x, mode);
            CHECK(drawn > 0);
            CHECK(drawn < (S32)kLabel.size());
            CHECK(right_x - 10.f <= (F32)max_pixels + 1.f);

            std::string out;
            const SKTextEllipsis::Cut cut = SKTextEllipsis::shorten(*font, kLabel, (F32)max_pixels, mode, out);
            CHECK(drawn == cut.drawnBytes());
        }
    }

    TEST_CASE_METHOD(sktextellipsis_gl_data, "SKTextEllipsisGL: true draws exactly what ELLIPSIS_END draws", "[SKTextEllipsisGL]")
    {
        if (!fileExists(kFontsXml))
            SKIP("fonts.xml not present in test data dir");
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        REQUIRE(font != nullptr);

        const S32 max_pixels = (S32)(font->getWidthF32(kLabel) * 0.5f);
        F32 right_bool = 0.f;
        F32 right_mode = 0.f;
        const S32 by_bool = font->renderBytes(kLabel, 0, 10.f, 100.f, LLColor4::white, LLFontGL::LEFT,
                                              LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                              S32_MAX, max_pixels, &right_bool, true);
        const S32 by_mode = font->renderBytes(kLabel, 0, 10.f, 100.f, LLColor4::white, LLFontGL::LEFT,
                                              LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                              S32_MAX, max_pixels, &right_mode, LLFontGL::ELLIPSIS_END);
        CHECK(by_bool < (S32)kLabel.size());
        CHECK(by_bool == by_mode);
        CHECK(right_bool == right_mode);
    }

    TEST_CASE_METHOD(sktextellipsis_gl_data, "SKTextEllipsisGL: a cache rebuilds when the mode changes and replays when it does not", "[SKTextEllipsisGL]")
    {
        if (!fileExists(kFontsXml))
            SKIP("fonts.xml not present in test data dir");
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        REQUIRE(font != nullptr);

        const S32 max_pixels = (S32)(font->getWidthF32(kLabel) * 0.5f);
        LLFontTextCache cache;
        auto draw = [&](LLFontGL::Ellipsis ellipsis)
        {
            return cache.renderBytes(font, kLabel, 0, 10.f, 100.f, LLColor4::white, LLFontGL::LEFT,
                                     LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                                     max_pixels, nullptr, ellipsis);
        };
        // A first draw may rasterize glyphs and rebuild again on the next, so each mode is settled first.
        for (int i = 0; i < 8; ++i)
        {
            draw(LLFontGL::ELLIPSIS_END);
        }

        U64 before = LLFontTextCache::regenCount();
        draw(LLFontGL::ELLIPSIS_MIDDLE);
        CHECK(LLFontTextCache::regenCount() > before);
        for (int i = 0; i < 8; ++i)
        {
            draw(LLFontGL::ELLIPSIS_MIDDLE);
        }

        before = LLFontTextCache::regenCount();
        const S32 replayed = draw(LLFontGL::ELLIPSIS_MIDDLE);
        CHECK(LLFontTextCache::regenCount() == before);

        std::string out;
        CHECK(replayed == SKTextEllipsis::shorten(*font, kLabel, (F32)max_pixels, LLFontGL::ELLIPSIS_MIDDLE, out).drawnBytes());
    }

    TEST_CASE_METHOD(sktextellipsis_gl_data, "SKTextEllipsisGL: a shadowed middle cut captures one shadow list and one foreground list", "[SKTextEllipsisGL]")
    {
        if (!fileExists(kFontsXml))
            SKIP("fonts.xml not present in test data dir");
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        REQUIRE(font != nullptr);

        const S32 max_pixels = (S32)(font->getWidthF32(kLabel) * 0.5f);
        LLFontTextCache cache;
        cache.renderBytes(font, kLabel, 0, 10.f, 100.f, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE,
                          LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT, S32_MAX, max_pixels, nullptr,
                          LLFontGL::ELLIPSIS_MIDDLE);
        const auto counts = ll_test::VertexBufferProbe::count(cache);
        CHECK(counts.foreground_quads > 0);
        CHECK(counts.shadow_quads > 0);
        // The dots are drawn in the same run as the text, so they cast a shadow as every other glyph does.
        if (counts.foreground_quads > 0)
        {
            CHECK(counts.shadow_quads % counts.foreground_quads == 0);
        }
    }
}
