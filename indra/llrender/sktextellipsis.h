/**
 * @file sktextellipsis.h
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

#ifndef SK_TEXTELLIPSIS_H
#define SK_TEXTELLIPSIS_H

#include "llfontgl.h"

#include <string>
#include <string_view>

// A string too wide for its room, shortened to fit with an ellipsis at the
// end, in the middle or at the start: measured in the font's own glyphs, cut
// only between grapheme clusters, and nudged onto a word boundary where that
// costs no more than the ellipsis's own width.
//
// Measure on the face that draws, font->faceFor(style), or a bold label is
// measured on the upright face. Where the renderer's own END cut and
// shorten(END) differ: this snaps to words and the renderer does not, and the
// renderer reserves an unscaled width out of a scaled budget, so under a UI
// scale other than 1 the two can be a pixel or so apart.
class SKTextEllipsis
{
public:
    // What is drawn where text was taken out, and the width reserved for it:
    // a fourth dot of padding, as the end mode has always had.
    static constexpr std::string_view ELLIPSIS         = "...";
    static constexpr std::string_view ELLIPSIS_RESERVE = "....";

    // The bytes of the text kept: [0, head_end) and [tail_begin, size).
    // Not shortened, head_end == tail_begin == size.
    struct Cut
    {
        S32  head_end   = 0;
        S32  tail_begin = 0;
        S32  size       = 0;    // bytes measured: up to the first NUL
        bool shortened  = false;

        // Bytes of the original on screen, what renderBytes returns for the cut.
        S32 drawnBytes() const { return head_end + (size - tail_begin); }
    };

    // Shortens `text` to `max_pixels` (virtual pixels, as getWidthF32 reports
    // them) into `out`, cleared first: the text up to its first NUL where it
    // fits, else head + ELLIPSIS + tail. Allocates nothing past `out`'s
    // capacity. ELLIPSIS_NONE never cuts. `text` must not be a view into `out`.
    static Cut shorten(const LLFontGL& font, std::string_view text, F32 max_pixels,
                       LLFontGL::EEllipsisMode mode, std::string& out);
};

#endif // SK_TEXTELLIPSIS_H
