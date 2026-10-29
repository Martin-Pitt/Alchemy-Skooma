/**
 * @file sktextellipsis.cpp
 * @brief Text shortened to a pixel width, the ellipsis at its end, in its
 *        middle or at its start.
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

#include "sktextellipsis.h"

#include "llmath.h"
#include "llstring.h"

namespace
{
    // The share of the room a middle cut gives the end, since names differ there: "Hair v2", "#12".
    constexpr F32 TAIL_SHARE = 0.55f;

    // Where a cut may move to for a word; all ASCII, so a byte scan never stops inside a character.
    bool is_word_boundary(char c)
    {
        return c == ' ' || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\';
    }

    // What the ellipsis must not sit against: a space, or a dot that would make four.
    bool is_edge_filler(char c)
    {
        return c == ' ' || c == '.';
    }

    // The renderer's own overflow test, in its rounded pixels, so nothing it draws whole is cut.
    bool fits(F32 width, F32 max_pixels)
    {
        return ll_round(width * LLFontGL::sScaleX) <= llceil(max_pixels * LLFontGL::sScaleX);
    }

    // Whether pos plainly falls between two grapheme clusters: an end, or ASCII on both sides but for CR LF.
    bool is_ascii_boundary(std::string_view text, S32 pos)
    {
        if (pos <= 0 || pos >= (S32)text.size())
        {
            return true;
        }
        const unsigned char prev = (unsigned char)text[pos - 1];
        const unsigned char next = (unsigned char)text[pos];
        return prev < 0x80 && next < 0x80 && !(prev == '\r' && next == '\n');
    }

    bool is_cluster_start(std::string_view text, S32 pos)
    {
        return is_ascii_boundary(text, pos) || utf8str_grapheme_align_backward(text, (size_t)pos) == (size_t)pos;
    }

    // The font's walks stop on its shaping clusters, which split a grapheme the font draws as several glyphs.
    S32 align_head(std::string_view text, S32 pos)
    {
        return is_ascii_boundary(text, pos) ? pos : (S32)utf8str_grapheme_align_backward(text, (size_t)pos);
    }

    S32 align_tail(std::string_view text, S32 pos)
    {
        return is_ascii_boundary(text, pos) ? pos : (S32)utf8str_grapheme_align_forward(text, (size_t)pos);
    }
}

// static
SKTextEllipsis::Cut SKTextEllipsis::shorten(const LLFontGL& font, std::string_view text, F32 max_pixels,
                                            LLFontGL::EEllipsisMode mode, std::string& out)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    // Every LLFontGL walk stops at an embedded NUL, so the cut does too.
    text = text.substr(0, std::min(text.find('\0'), text.size()));
    const S32 size = (S32)text.size();

    Cut cut;
    cut.head_end = cut.tail_begin = cut.size = size;
    if (size == 0 || mode == LLFontGL::ELLIPSIS_NONE || max_pixels >= (F32)S32_MAX
        || fits(font.getWidthF32(text), max_pixels))
    {
        out.assign(text);
        return cut;
    }

    S32 head_end   = 0;
    S32 tail_begin = size;
    const F32 budget = max_pixels - font.getWidthF32(ELLIPSIS_RESERVE);
    if (budget > 0.f)
    {
        // The walk sums advances shaped in the whole run; the tail shaped alone can be wider, or be a last cluster that never fit.
        auto tail_in = [&](F32 room) -> S32
        {
            S32 tail = align_tail(text, font.firstDrawableByte(text, room, size - 1));
            while (tail < size && font.getWidthF32Bytes(text, tail, size - tail) > room)
            {
                tail = (S32)utf8str_step_grapheme_forward(text, (size_t)tail);
            }
            return tail;
        };

        switch (mode)
        {
        case LLFontGL::ELLIPSIS_START:
            tail_begin = tail_in(budget);
            break;
        case LLFontGL::ELLIPSIS_MIDDLE:
        {
            tail_begin = tail_in(budget * TAIL_SHARE);
            const F32 head_room = budget - font.getWidthF32Bytes(text, tail_begin, size - tail_begin);
            head_end = head_room > 0.f ? align_head(text, font.maxDrawableBytes(text, head_room, tail_begin)) : 0;
            break;
        }
        default:
            head_end = align_head(text, font.maxDrawableBytes(text, budget, size));
            break;
        }

        // A cut inside a word moves back to the nearest boundary if that loses no more than the ellipsis is wide.
        const F32 slack = font.getWidthF32(ELLIPSIS);
        if (head_end > 0 && head_end < size && !is_word_boundary(text[head_end]))
        {
            S32 b = head_end - 1;
            while (b > 0 && !is_word_boundary(text[b]))
            {
                --b;
            }
            if (b > 0 && is_cluster_start(text, b)
                && font.getWidthF32Bytes(text, b, head_end - b) <= slack)
            {
                head_end = b;
            }
        }
        if (tail_begin > 0 && tail_begin < size && !is_word_boundary(text[tail_begin - 1]))
        {
            S32 q = tail_begin;
            while (q < size && !is_word_boundary(text[q]))
            {
                ++q;
            }
            // The tail keeps a separator ("-RIGGED", "/textures") but starts past a space or a dot.
            const S32 start = (q < size && is_edge_filler(text[q])) ? q + 1 : q;
            if (start < size && is_cluster_start(text, start)
                && font.getWidthF32Bytes(text, tail_begin, start - tail_begin) <= slack)
            {
                tail_begin = start;
            }
        }
    }
    llassert(head_end <= tail_begin);

    auto tidy_edges = [&]()
    {
        while (head_end > 0 && is_edge_filler(text[head_end - 1]))
        {
            head_end = (S32)utf8str_step_grapheme_backward(text, (size_t)head_end);
        }
        while (tail_begin < size && is_edge_filler(text[tail_begin]))
        {
            tail_begin = (S32)utf8str_step_grapheme_forward(text, (size_t)tail_begin);
        }
    };
    auto assemble = [&]()
    {
        out.clear();
        out.append(text.substr(0, (size_t)head_end));
        out.append(ELLIPSIS);
        out.append(text.substr((size_t)tail_begin));
    };

    // The pieces were measured apart; the string is drawn as one run, kerned and ligated across the cut.
    tidy_edges();
    assemble();
    while ((head_end > 0 || tail_begin < size) && !fits(font.getWidthF32(out), max_pixels))
    {
        const bool from_head = tail_begin >= size
            || (head_end > 0
                && font.getWidthF32Bytes(text, 0, head_end)
                   >= font.getWidthF32Bytes(text, tail_begin, size - tail_begin));
        if (from_head)
        {
            head_end = (S32)utf8str_step_grapheme_backward(text, (size_t)head_end);
        }
        else
        {
            tail_begin = (S32)utf8str_step_grapheme_forward(text, (size_t)tail_begin);
        }
        tidy_edges();
        assemble();
    }

    cut.head_end   = head_end;
    cut.tail_begin = tail_begin;
    cut.shortened  = true;
    return cut;
}
