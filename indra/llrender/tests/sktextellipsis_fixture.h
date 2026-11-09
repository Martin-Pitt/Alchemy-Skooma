/**
 * @file sktextellipsis_fixture.h
 * @brief The fonts SKTextEllipsis's test and benchmark measure with: the
 *        source tree's, made once for the process without GL textures.
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

#ifndef SK_TEXTELLIPSIS_FIXTURE_H
#define SK_TEXTELLIPSIS_FIXTURE_H

#include "linden_common.h"

#include "../llfontfreetype.h"
#include "../llfontgl.h"

#include "alteststateguard.h"
#include "altestteardown.h"

#include <cstdio>
#include <string>

#ifndef LLFONT_TEST_APP_DIR
#  define LLFONT_TEST_APP_DIR ""
#endif
#ifndef LLFONT_TEST_FONTS_XML
#  define LLFONT_TEST_FONTS_XML ""
#endif

namespace ll_test
{
    // The UI's sans serif, measured without a GL context: fonts made without
    // GL textures measure as they would draw, and draw nothing. Null where the
    // source tree's fonts are not where the build said. Made once for the
    // process, as LLFontGL's explicit-path initClass allows, and ended by the
    // teardown, so nothing here may share a process with another font bring-up.
    inline const LLFontGL* ellipsisTestFont()
    {
        static const LLFontGL* font = []() -> const LLFontGL*
        {
            const std::string fonts_xml = LLFONT_TEST_FONTS_XML;
            FILE* f = fonts_xml.empty() ? nullptr : std::fopen(fonts_xml.c_str(), "rb");
            if (!f)
            {
                return nullptr;
            }
            std::fclose(f);

            LLFontManager::initClass();
            LLFontGL::initClass(96.f, 1.f, 1.f, LLFONT_TEST_APP_DIR, fonts_xml, LLSD(),
                                /*create_gl_textures=*/false);
            ll_test::Teardown::add([]
            {
                LLFontGL::destroyDefaultFonts();
                LLFontManager::cleanupClass();
            });
            ll_test::StateGuard::accept();
            return LLFontGL::getFontSansSerif();
        }();
        return font;
    }
}

#endif // SK_TEXTELLIPSIS_FIXTURE_H
