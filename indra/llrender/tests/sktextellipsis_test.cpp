/**
 * @file sktextellipsis_test.cpp
 * @brief Tests for SKTextEllipsis: text shortened to a width with the
 *        ellipsis at its end, in its middle or at its start, measured on the
 *        UI's own font without a GL context.
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

#include "../sktextellipsis.h"

#include "sktextellipsis_fixture.h"

#include "llmath.h"
#include "llstring.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

namespace
{
    using Cut = SKTextEllipsis::Cut;

    constexpr LLFontGL::EEllipsisMode kCutModes[] = { LLFontGL::ELLIPSIS_END, LLFontGL::ELLIPSIS_MIDDLE,
                                                      LLFontGL::ELLIPSIS_START };

    const LLFontGL& fontOrSkip()
    {
        const LLFontGL* font = ll_test::ellipsisTestFont();
        if (!font)
        {
            SKIP("the source tree's fonts.xml is not where the build said");
        }
        return *font;
    }

    std::string utf8(std::initializer_list<llwchar> codepoints)
    {
        std::string out;
        char buf[8];
        for (llwchar c : codepoints)
        {
            out.append(buf, (size_t)wchar_to_utf8chars(c, buf));
        }
        return out;
    }

    // The renderer's overflow test, which the shortener promises to meet.
    bool drawsWhole(F32 width, F32 max_pixels)
    {
        return ll_round(width * LLFontGL::sScaleX) <= llceil(max_pixels * LLFontGL::sScaleX);
    }

    bool isClusterStart(std::string_view text, S32 pos)
    {
        return utf8str_grapheme_align_backward(text, (size_t)pos) == (size_t)pos;
    }

    // What every cut owes: `out` is what the ranges say, the ranges are in
    // order and on cluster starts, and a shortened string fits unless only
    // the dots are left.
    void checkCut(const LLFontGL& font, std::string_view text, F32 max_pixels, const Cut& cut,
                  const std::string& out)
    {
        CAPTURE(text, max_pixels, cut.head_end, cut.tail_begin, cut.size, cut.shortened, out);
        REQUIRE(cut.head_end >= 0);
        REQUIRE(cut.head_end <= cut.tail_begin);
        REQUIRE(cut.tail_begin <= cut.size);
        REQUIRE(cut.size <= (S32)text.size());
        if (!cut.shortened)
        {
            CHECK(out == text.substr(0, (size_t)cut.size));
            CHECK(cut.drawnBytes() == cut.size);
            return;
        }
        std::string expected(text.substr(0, (size_t)cut.head_end));
        expected += SKTextEllipsis::ELLIPSIS;
        expected += text.substr((size_t)cut.tail_begin, (size_t)(cut.size - cut.tail_begin));
        CHECK(out == expected);
        CHECK(cut.drawnBytes() < cut.size);
        CHECK(isClusterStart(text, cut.head_end));
        CHECK(isClusterStart(text, cut.tail_begin));
        const bool dots_only = cut.head_end == 0 && cut.tail_begin == cut.size;
        CHECK((dots_only || drawsWhole(font.getWidthF32(out), max_pixels)));
    }

    TEST_CASE("SKTextEllipsis: text that fits is copied whole in every mode", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "Hair v2";
        const F32 full = font.getWidthF32(text);
        for (LLFontGL::EEllipsisMode mode : { LLFontGL::ELLIPSIS_NONE, LLFontGL::ELLIPSIS_END,
                                              LLFontGL::ELLIPSIS_MIDDLE, LLFontGL::ELLIPSIS_START })
        {
            std::string out;
            const Cut cut = SKTextEllipsis::shorten(font, text, full + 10.f, mode, out);
            CHECK_FALSE(cut.shortened);
            CHECK(out == text);
        }
    }

    TEST_CASE("SKTextEllipsis: text exactly as wide as its room is not cut", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "Exactly as wide as this";
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            std::string out;
            const Cut cut = SKTextEllipsis::shorten(font, text, font.getWidthF32(text), mode, out);
            CHECK_FALSE(cut.shortened);
            CHECK(out == text);
        }
    }

    TEST_CASE("SKTextEllipsis: an empty string or a NUL first is not cut", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string nul_first("\0abcdefghijklmnopqrstuvwxyz", 27);
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            std::string out = "stale";
            Cut cut = SKTextEllipsis::shorten(font, "", 5.f, mode, out);
            CHECK_FALSE(cut.shortened);
            CHECK(out.empty());
            cut = SKTextEllipsis::shorten(font, nul_first, 5.f, mode, out);
            CHECK_FALSE(cut.shortened);
            CHECK(cut.size == 0);
            CHECK(out.empty());
        }
    }

    TEST_CASE("SKTextEllipsis: nothing past an embedded NUL is measured or kept", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text("Short\0and a long hidden remainder that would never fit", 54);
        std::string out;
        const Cut cut = SKTextEllipsis::shorten(font, text, font.getWidthF32("Short") + 2.f,
                                                LLFontGL::ELLIPSIS_MIDDLE, out);
        CHECK_FALSE(cut.shortened);
        CHECK(cut.size == 5);
        CHECK(out == "Short");
    }

    TEST_CASE("SKTextEllipsis: an end cut keeps the most of the start that fits", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ";
        const F32 max_pixels = (F32)(S32)(font.getWidthF32(text) / 2.f);
        std::string out;
        const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, LLFontGL::ELLIPSIS_END, out);
        checkCut(font, text, max_pixels, cut, out);
        REQUIRE(cut.shortened);
        CHECK(cut.tail_begin == cut.size);
        CHECK(out.size() > SKTextEllipsis::ELLIPSIS.size());
        // Without a word boundary in reach it is the renderer's own walk.
        const S32 walk = font.maxDrawableBytes(text, max_pixels - font.getWidthF32(SKTextEllipsis::ELLIPSIS_RESERVE));
        CHECK(cut.head_end == walk);
    }

    TEST_CASE("SKTextEllipsis: a middle cut keeps both ends and leans to the end", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "TheQuickBrownFoxJumpsOverTheLazyDogAgainAndAgain";
        const F32 full    = font.getWidthF32(text);
        const F32 reserve = font.getWidthF32(SKTextEllipsis::ELLIPSIS_RESERVE);
        const F32 slack   = font.getWidthF32(SKTextEllipsis::ELLIPSIS);
        const F32 wide    = font.getWidthF32("W");
        const F32 room_for_both = reserve + 3.f * wide;

        F32 heads = 0.f;
        F32 tails = 0.f;
        for (F32 max_pixels = reserve; max_pixels < full - 1.f; max_pixels += 1.f)
        {
            std::string out;
            const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, LLFontGL::ELLIPSIS_MIDDLE, out);
            checkCut(font, text, max_pixels, cut, out);
            CHECK(cut.shortened);
            CAPTURE(max_pixels, out);
            if (max_pixels >= room_for_both)
            {
                CHECK(cut.head_end > 0);
                CHECK(cut.tail_begin < cut.size);
            }
            // Each side stops short of its share by less than a glyph, so the head
            // outgrows the tail by two glyphs at most, and the room is used but
            // for those and the reserve.
            const F32 head_w = font.getWidthF32Bytes(text, 0, cut.head_end);
            const F32 tail_w = font.getWidthF32Bytes(text, cut.tail_begin, cut.size - cut.tail_begin);
            CHECK(tail_w + 2.f * wide >= head_w);
            CHECK(head_w + tail_w + 2.f * (wide + slack) >= max_pixels - reserve);
            heads += head_w;
            tails += tail_w;
        }
        CHECK(tails > heads);
    }

    TEST_CASE("SKTextEllipsis: a start cut keeps the most of the end that fits", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ";
        const F32 max_pixels = font.getWidthF32(text) / 2.f;
        std::string out;
        const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, LLFontGL::ELLIPSIS_START, out);
        checkCut(font, text, max_pixels, cut, out);
        REQUIRE(cut.shortened);
        CHECK(cut.head_end == 0);
        CHECK(out.rfind(SKTextEllipsis::ELLIPSIS, 0) == 0);
        CHECK(out.size() > SKTextEllipsis::ELLIPSIS.size());
    }

    TEST_CASE("SKTextEllipsis: an end cut inside a word moves back to the word's start within slack", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "Hair v2 - RIGGED (no copy) #12";
        const F32 reserve = font.getWidthF32(SKTextEllipsis::ELLIPSIS_RESERVE);
        const F32 slack   = font.getWidthF32(SKTextEllipsis::ELLIPSIS);
        const F32 full    = font.getWidthF32(text);
        for (F32 max_pixels = reserve + 1.f; max_pixels < full; max_pixels += 1.f)
        {
            std::string out;
            const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, LLFontGL::ELLIPSIS_END, out);
            checkCut(font, text, max_pixels, cut, out);
            CAPTURE(max_pixels, out);

            // Where the plain walk ends, and the boundary a snap would move it to.
            const S32 walk = font.maxDrawableBytes(text, max_pixels - reserve);
            S32 boundary = walk - 1;
            while (boundary > 0 && std::string_view(" _-./\\").find(text[boundary]) == std::string_view::npos)
            {
                --boundary;
            }
            const bool inside_word = walk < (S32)text.size()
                && std::string_view(" _-./\\").find(text[walk]) == std::string_view::npos;
            if (inside_word && boundary > 0 && font.getWidthF32Bytes(text, boundary, walk - boundary) <= slack)
            {
                CHECK(cut.head_end <= boundary);
            }
            else
            {
                CHECK(cut.head_end <= walk);
            }
            CHECK(out.find(" ...") == std::string::npos);
        }
    }

    TEST_CASE("SKTextEllipsis: the dots never meet a space or another dot", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        for (const std::string text : { std::string("My Great Model v2.blend"), std::string("Hair v2 - RIGGED (no copy) #12"),
                                        std::string("Wait. Then. Go. Now. Stop. Done."),
                                        std::string("assets/textures/rock_diffuse.v3.png") })
        {
            const F32 full = font.getWidthF32(text);
            for (LLFontGL::EEllipsisMode mode : kCutModes)
            {
                for (F32 max_pixels = 0.f; max_pixels < full; max_pixels += 1.f)
                {
                    std::string out;
                    const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                    checkCut(font, text, max_pixels, cut, out);
                    CAPTURE(text, mode, max_pixels, out);
                    CHECK(out.find("....") == std::string::npos);
                    CHECK(out.find(" ...") == std::string::npos);
                    CHECK(out.find("... ") == std::string::npos);
                }
            }
        }
    }

    TEST_CASE("SKTextEllipsis: a room narrower than the dots leaves the dots alone", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "Something far too long for no room at all";
        const F32 reserve = font.getWidthF32(SKTextEllipsis::ELLIPSIS_RESERVE);
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            for (F32 max_pixels : { reserve - 1.f, 0.f, -5.f })
            {
                std::string out;
                const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                CHECK(cut.shortened);
                CHECK(cut.head_end == 0);
                CHECK(cut.tail_begin == cut.size);
                CHECK(out == SKTextEllipsis::ELLIPSIS);
            }
        }
    }

    TEST_CASE("SKTextEllipsis: no mode cuts inside a grapheme cluster", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string family = utf8({ 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 });
        const std::string flag   = utf8({ 0x1F1EF, 0x1F1F5 });
        const std::string texts[] = {
            "Cafe" + utf8({ 0x301 }) + " au lait e" + utf8({ 0x301 }) + "te" + utf8({ 0x301 }) + " encore",
            "Family " + family + family + " and " + flag + flag + " flags " + family,
            utf8({ 0xD55C, 0xAD6D, 0xC5B4, 0x20, 0xD14D, 0xC2A4, 0xD2B8, 0x20, 0xD55C, 0xAD6D, 0xC5B4 }),
            utf8({ 0x0915, 0x094D, 0x0937, 0x0924, 0x094D, 0x0930, 0x093F, 0x092F, 0x20, 0x0936, 0x094D,
                   0x0930, 0x0940, 0x20, 0x0915, 0x094D, 0x0937 }),
        };
        for (const std::string& text : texts)
        {
            const F32 full = font.getWidthF32(text);
            for (LLFontGL::EEllipsisMode mode : kCutModes)
            {
                for (F32 max_pixels = 0.f; max_pixels < full; max_pixels += 1.f)
                {
                    std::string out;
                    const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                    checkCut(font, text, max_pixels, cut, out);
                    CHECK(utf8str_is_valid(out));
                }
            }
        }
    }

    TEST_CASE("SKTextEllipsis: a right-to-left string is cut into a logical head and tail", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = utf8({ 0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x20, 0x05E2, 0x05D5, 0x05DC, 0x05DD, 0x20,
                                        0x05D8, 0x05D5, 0x05D1, 0x20, 0x05DE, 0x05D0, 0x05D5, 0x05D3 });
        const F32 max_pixels = font.getWidthF32(text) * 0.6f;
        std::string out;
        const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, LLFontGL::ELLIPSIS_MIDDLE, out);
        checkCut(font, text, max_pixels, cut, out);
        CHECK(cut.shortened);
        CHECK(utf8str_is_valid(out));
        CHECK(cut.head_end > 0);
        CHECK(cut.tail_begin < cut.size);
    }

    TEST_CASE("SKTextEllipsis: malformed UTF-8 is cut without a fault", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "A label that ends in a lone lead byte \xE2";
        const F32 full = font.getWidthF32(text);
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            for (F32 max_pixels = 0.f; max_pixels < full; max_pixels += 3.f)
            {
                std::string out;
                const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                CAPTURE(mode, max_pixels, out);
                REQUIRE(cut.head_end <= cut.tail_begin);
                const bool dots_only = cut.head_end == 0 && cut.tail_begin == cut.size;
                CHECK((!cut.shortened || dots_only || drawsWhole(font.getWidthF32(out), max_pixels)));
            }
        }
    }

    TEST_CASE("SKTextEllipsis: kerning across the cut still fits", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "AVAVAVToToToWAWAWAVAVAToTaTeTyYoYaLTLTAVAV";
        const F32 full = font.getWidthF32(text);
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            for (F32 max_pixels = 0.f; max_pixels < full; max_pixels += 1.f)
            {
                std::string out;
                const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                checkCut(font, text, max_pixels, cut, out);
            }
        }
    }

    TEST_CASE("SKTextEllipsis: a kept output string is not allocated again", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string text = "Inventory item name of forty characters!";
        const F32 full = font.getWidthF32(text);
        std::string out;
        out.reserve(text.size() + SKTextEllipsis::ELLIPSIS.size());
        const size_t capacity = out.capacity();
        for (LLFontGL::EEllipsisMode mode : kCutModes)
        {
            for (F32 max_pixels = 0.f; max_pixels < full + 4.f; max_pixels += 2.f)
            {
                SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                CHECK(out.capacity() == capacity);
            }
        }
    }

    TEST_CASE("SKTextEllipsis: a last cluster wider than the tail's room drops the tail", "[SKTextEllipsis]")
    {
        const LLFontGL& font = fontOrSkip();
        const std::string texts[] = {
            "A label ending in a wide glyph " + utf8({ 0x1F600 }),
            "A label ending in a wide glyph " + utf8({ 0x6F22, 0x5B57 }),
        };
        for (const std::string& text : texts)
        {
            const F32 full = font.getWidthF32(text);
            for (LLFontGL::EEllipsisMode mode : { LLFontGL::ELLIPSIS_MIDDLE, LLFontGL::ELLIPSIS_START })
            {
                for (F32 max_pixels = 0.f; max_pixels < full; max_pixels += 1.f)
                {
                    std::string out;
                    const Cut cut = SKTextEllipsis::shorten(font, text, max_pixels, mode, out);
                    checkCut(font, text, max_pixels, cut, out);
                }
            }
        }
    }
}
