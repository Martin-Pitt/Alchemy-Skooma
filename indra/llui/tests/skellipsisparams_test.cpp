/**
 * @file skellipsisparams_test.cpp
 * @brief Tests for the ellipsis mode as XUI names it and as text, buttons
 *        and tab containers take it from their parameters.
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

#include "../llbutton.h"
#include "../llpanel.h"
#include "../lltabcontainer.h"
#include "../lltextbox.h"
#include "../llui.h"
#include "../lluictrlfactory.h"
#include "../llxuiparser.h"
#include "llxmlnode.h"

#include "alheadlessui_fixture.h"
#include "aluistatescope.h"

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <memory>
#include <string>

namespace
{
    struct skellipsisparams_data
    {
        ll_test::HeadlessUI& ui = ll_test::HeadlessUI::get();
        ll_test::FocusScope  focus;

        void requireUI() const
        {
            if (!ui.ok())
            {
                SKIP("no UI: LLUI_TEST_APP_DIR does not point at the source tree");
            }
        }

        // Reads one XUI element into a widget's params, as a layout file would.
        template <typename PARAMS>
        static void read(const std::string& source, PARAMS& p)
        {
            LLXMLNodePtr node;
            REQUIRE(LLXMLNode::parseBuffer(source.data(), source.size(), node));
            LLXUIParser parser;
            parser.readXUI(node, p, "skellipsisparams_test.xml");
        }

        template <typename WIDGET>
        static std::unique_ptr<WIDGET> build(const std::string& source)
        {
            typename WIDGET::Params p(LLUICtrlFactory::getDefaultParams<WIDGET>());
            read(source, p);
            p.rect = LLRect(0, 20, 120, 0);
            return std::unique_ptr<WIDGET>(LLUICtrlFactory::create<WIDGET>(p));
        }
    };

    TEST_CASE_METHOD(skellipsisparams_data, "skellipsisparams: XUI names every ellipsis mode both ways", "[skellipsisparams]")
    {
        requireUI();
        using Names = LLInitParam::TypeValues<LLFontGL::EEllipsisMode>;
        const std::pair<const char*, LLFontGL::EEllipsisMode> names[] = {
            { "none", LLFontGL::ELLIPSIS_NONE },
            { "end", LLFontGL::ELLIPSIS_END },
            { "middle", LLFontGL::ELLIPSIS_MIDDLE },
            { "start", LLFontGL::ELLIPSIS_START },
        };
        for (const auto& [name, mode] : names)
        {
            CAPTURE(name);
            LLFontGL::EEllipsisMode read = LLFontGL::ELLIPSIS_NONE;
            CHECK(Names::getValueFromName(name, read));
            CHECK(read == mode);
        }
        CHECK(Names::getValueNames()->size() == std::size(names));
        LLFontGL::EEllipsisMode unknown = LLFontGL::ELLIPSIS_END;
        CHECK_FALSE(Names::getValueFromName("centre", unknown));
    }

    TEST_CASE_METHOD(skellipsisparams_data, "skellipsisparams: a button takes its mode from ellipsis before use_ellipses", "[skellipsisparams]")
    {
        requireUI();
        CHECK(build<LLButton>("<button name=\"b\" label=\"Hair v2 - RIGGED\" ellipsis=\"middle\"/>")->getEllipsis()
              == LLFontGL::ELLIPSIS_MIDDLE);
        CHECK(build<LLButton>("<button name=\"b\" label=\"x\" use_ellipses=\"true\"/>")->getEllipsis()
              == LLFontGL::ELLIPSIS_END);
        CHECK(build<LLButton>("<button name=\"b\" label=\"x\"/>")->getEllipsis() == LLFontGL::ELLIPSIS_NONE);
        CHECK(build<LLButton>("<button name=\"b\" label=\"x\" use_ellipses=\"true\" ellipsis=\"start\"/>")->getEllipsis()
              == LLFontGL::ELLIPSIS_START);

        std::unique_ptr<LLButton> button = build<LLButton>("<button name=\"b\" label=\"x\"/>");
        button->setUseEllipses(true);
        CHECK(button->getEllipsis() == LLFontGL::ELLIPSIS_END);
        button->setEllipsis(LLFontGL::ELLIPSIS_MIDDLE);
        CHECK(button->getEllipsis() == LLFontGL::ELLIPSIS_MIDDLE);
    }

    TEST_CASE_METHOD(skellipsisparams_data, "skellipsisparams: a text box takes its mode from ellipsis before use_ellipses", "[skellipsisparams]")
    {
        requireUI();
        std::unique_ptr<LLTextBox> middle = build<LLTextBox>("<text name=\"t\" ellipsis=\"middle\">Hair v2</text>");
        CHECK(middle->getEllipsis() == LLFontGL::ELLIPSIS_MIDDLE);
        CHECK(middle->getUseEllipses());

        std::unique_ptr<LLTextBox> end = build<LLTextBox>("<text name=\"t\" use_ellipses=\"true\">Hair v2</text>");
        CHECK(end->getEllipsis() == LLFontGL::ELLIPSIS_END);
        CHECK(end->getUseEllipses());

        std::unique_ptr<LLTextBox> none = build<LLTextBox>("<text name=\"t\">Hair v2</text>");
        CHECK(none->getEllipsis() == LLFontGL::ELLIPSIS_NONE);
        CHECK_FALSE(none->getUseEllipses());
    }

    TEST_CASE_METHOD(skellipsisparams_data, "skellipsisparams: a tab container hands its mode to its tab buttons", "[skellipsisparams]")
    {
        requireUI();
        LLTabContainer::Params p(LLUICtrlFactory::getDefaultParams<LLTabContainer>());
        read("<tab_container name=\"tabs\" ellipsis=\"start\"/>", p);
        p.rect = LLRect(0, 300, 400, 0);
        std::unique_ptr<LLTabContainer> tabs(LLUICtrlFactory::create<LLTabContainer>(p));

        LLPanel::Params pp(LLUICtrlFactory::getDefaultParams<LLPanel>());
        pp.name = "page";
        pp.rect = LLRect(0, 260, 400, 0);
        tabs->addTabPanel(LLUICtrlFactory::create<LLPanel>(pp));
        tabs->selectTab(0);

        const LLButton* button = tabs->findChild<LLButton>("htab_page", true);
        REQUIRE(button != nullptr);
        CHECK(button->getEllipsis() == LLFontGL::ELLIPSIS_START);
    }
}
