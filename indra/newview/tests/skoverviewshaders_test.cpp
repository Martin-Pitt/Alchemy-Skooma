/**
 * @file skoverviewshaders_test.cpp
 * @brief The overview geometry shaders' permutation structure: discard only under SK_DISSOLVE, gl_FragDepth only
 *        under SK_CAP_LIFT, the clip plane, ceiling and cuts as the vertex stage's four clip distances.
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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // A shader's source, found from this file so the test reads what the viewer compiles.
    std::string shader_source(const std::string& rel)
    {
        const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / ".." / "app_settings" / "shaders" / rel;
        std::ifstream in(path);
        std::stringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    // The source with its comments blanked, line breaks kept.
    std::string strip_comments(const std::string& src)
    {
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (src.compare(i, 2, "//") == 0)
            {
                while (i < src.size() && src[i] != '\n')
                {
                    ++i;
                }
                if (i < src.size())
                {
                    out += '\n';
                }
            }
            else if (src.compare(i, 2, "/*") == 0)
            {
                const size_t end = src.find("*/", i + 2);
                const size_t stop = end == std::string::npos ? src.size() : end + 2;
                for (; i < stop; ++i)
                {
                    if (src[i] == '\n')
                    {
                        out += '\n';
                    }
                }
                --i;
            }
            else
            {
                out += src[i];
            }
        }
        return out;
    }

    // One source line and the defines its preprocessor branches require: +NAME defined, -NAME not, ? for any other
    // condition.
    struct Line
    {
        std::string mText;
        std::vector<std::string> mConditions;
        size_t mNumber = 0;
    };

    std::vector<Line> lines_with_conditions(const std::string& src)
    {
        static const std::regex IFDEF(R"(^\s*#\s*ifdef\s+(\w+))");
        static const std::regex IFNDEF(R"(^\s*#\s*ifndef\s+(\w+))");
        static const std::regex IF_DEFINED(R"(^\s*#\s*if\s+defined\s*\(\s*(\w+)\s*\)\s*$)");
        static const std::regex IF(R"(^\s*#\s*if\b)");
        static const std::regex ELIF(R"(^\s*#\s*elif\b)");
        static const std::regex ELSE(R"(^\s*#\s*else\b)");
        static const std::regex ENDIF(R"(^\s*#\s*endif\b)");
        std::vector<Line> out;
        std::vector<std::string> stack;
        std::istringstream in(strip_comments(src));
        std::string text;
        size_t number = 0;
        std::smatch m;
        while (std::getline(in, text))
        {
            ++number;
            if (std::regex_search(text, m, IFDEF) || std::regex_search(text, m, IF_DEFINED))
            {
                stack.push_back("+" + m[1].str());
            }
            else if (std::regex_search(text, m, IFNDEF))
            {
                stack.push_back("-" + m[1].str());
            }
            else if (std::regex_search(text, IF))
            {
                stack.push_back("?");
            }
            else if (std::regex_search(text, ELIF))
            {
                REQUIRE(!stack.empty());
                stack.back() = "?";
            }
            else if (std::regex_search(text, ELSE))
            {
                REQUIRE(!stack.empty());
                std::string& top = stack.back();
                top = top[0] == '+' ? "-" + top.substr(1) : top[0] == '-' ? "+" + top.substr(1) : "?";
            }
            else if (std::regex_search(text, ENDIF))
            {
                REQUIRE(!stack.empty());
                stack.pop_back();
            }
            else
            {
                out.push_back({ text, stack, number });
            }
        }
        CHECK(stack.empty());
        return out;
    }

    bool requires_define(const Line& line, const std::string& define)
    {
        for (const std::string& c : line.mConditions)
        {
            if (c == "+" + define)
            {
                return true;
            }
        }
        return false;
    }

    const char* GEOM_F = "class1/skooma/overviewGeomF.glsl";
    const char* GEOM_V = "class1/skooma/overviewGeomV.glsl";

    TEST_CASE("SKOverviewShaders: discard appears only under SK_DISSOLVE", "[SKOverviewShaders]")
    {
        const std::string src = shader_source(GEOM_F);
        REQUIRE(!src.empty());
        static const std::regex DISCARD(R"(\bdiscard\b)");
        for (const Line& line : lines_with_conditions(src))
        {
            if (std::regex_search(line.mText, DISCARD))
            {
                CAPTURE(line.mNumber, line.mText);
                CHECK(requires_define(line, "SK_DISSOLVE"));
            }
        }
    }

    TEST_CASE("SKOverviewShaders: gl_FragDepth is written only under SK_CAP_LIFT", "[SKOverviewShaders]")
    {
        const std::string src = shader_source(GEOM_F);
        REQUIRE(!src.empty());
        static const std::regex WRITE(R"(\bgl_FragDepth\s*(=[^=]|[-+*/]=))");
        size_t writes = 0;
        for (const Line& line : lines_with_conditions(src))
        {
            if (std::regex_search(line.mText, WRITE))
            {
                CAPTURE(line.mNumber, line.mText);
                CHECK(requires_define(line, "SK_CAP_LIFT"));
                ++writes;
            }
        }
        CHECK(writes > 0);
    }

    TEST_CASE("SKOverviewShaders: the vertex stage writes all four clip distances", "[SKOverviewShaders]")
    {
        const std::string src = strip_comments(shader_source(GEOM_V));
        REQUIRE(!src.empty());
        CHECK(std::regex_search(src, std::regex(R"(\bout\s+float\s+gl_ClipDistance\s*\[\s*4\s*\])")));
        const std::vector<Line> lines = lines_with_conditions(shader_source(GEOM_V));
        for (int i = 0; i < 4; ++i)
        {
            CAPTURE(i);
            // Written in every permutation: unconditionally, or under a define and under its absence alike.
            const std::regex write(R"(\bgl_ClipDistance\s*\[\s*)" + std::to_string(i) + R"(\s*\]\s*=[^=])");
            std::vector<std::vector<std::string>> under;
            for (const Line& line : lines)
            {
                if (std::regex_search(line.mText, write))
                {
                    under.push_back(line.mConditions);
                }
            }
            bool everywhere = false;
            for (const std::vector<std::string>& c : under)
            {
                if (c.empty())
                {
                    everywhere = true;
                }
                else if (c.size() == 1 && c[0][0] == '+')
                {
                    const std::vector<std::string> absent = { "-" + c[0].substr(1) };
                    everywhere = everywhere || std::find(under.begin(), under.end(), absent) != under.end();
                }
            }
            CHECK(everywhere);
        }
    }

    // An open part has no inside, so the cap pass's sign keeps it out of the section passes (texcoord0.x, the census's flag).
    TEST_CASE("SKOverviewShaders: an open part is never sectioned", "[SKOverviewShaders]")
    {
        static const std::regex CUT_PART(R"(\bcut_part\s*=.*\btexcoord0\.x\s*<\s*0\.5)");
        size_t found = 0;
        for (const Line& line : lines_with_conditions(shader_source(GEOM_V)))
        {
            if (std::regex_search(line.mText, CUT_PART))
            {
                CAPTURE(line.mNumber, line.mText);
                CHECK(requires_define(line, "SK_CUT_ATTRIBUTE"));
                ++found;
            }
        }
        CHECK(found == 1);
    }

    // The cut is the linkset's row of SKOverviewCutTable: a storage block where the session computes, a buffer texture elsewhere.
    TEST_CASE("SKOverviewShaders: the vertex stage reads the cut row from one store per permutation", "[SKOverviewShaders]")
    {
        static const std::regex BLOCK(R"(\brows\s*\[)");
        static const std::regex TEXTURE(R"(\btexelFetch\s*\(\s*sk_cut_table\b)");
        size_t blocks = 0;
        size_t textures = 0;
        for (const Line& line : lines_with_conditions(shader_source(GEOM_V)))
        {
            CAPTURE(line.mNumber, line.mText);
            bool ssbo = false;
            bool not_ssbo = false;
            for (const std::string& c : line.mConditions)
            {
                ssbo = ssbo || c == "+SK_CUT_TABLE_SSBO";
                not_ssbo = not_ssbo || c == "-SK_CUT_TABLE_SSBO";
            }
            if (std::regex_search(line.mText, BLOCK))
            {
                CHECK(ssbo);
                CHECK(requires_define(line, "SK_CUT_ATTRIBUTE"));
                ++blocks;
            }
            if (std::regex_search(line.mText, TEXTURE))
            {
                CHECK(not_ssbo);
                CHECK(requires_define(line, "SK_CUT_ATTRIBUTE"));
                ++textures;
            }
        }
        CHECK(blocks > 0);
        CHECK(textures == 1);
    }

    // The weight channel carries the linkset id, which nothing may read as a height.
    TEST_CASE("SKOverviewShaders: weight is read once as the group index", "[SKOverviewShaders]")
    {
        static const std::regex READ(R"(\bint\s*\(\s*weight\b)");
        static const std::regex WEIGHT(R"(\bweight\b)");
        static const std::regex DECLARATION(R"(^\s*in\s+float\s+weight\s*;)");
        size_t reads = 0;
        for (const Line& line : lines_with_conditions(shader_source(GEOM_V)))
        {
            if (!std::regex_search(line.mText, WEIGHT))
            {
                continue;
            }
            CAPTURE(line.mNumber, line.mText);
            CHECK(requires_define(line, "SK_CUT_ATTRIBUTE"));
            if (std::regex_search(line.mText, READ))
            {
                ++reads;
            }
            else
            {
                CHECK(std::regex_search(line.mText, DECLARATION));
            }
        }
        CHECK(reads == 1);
        const std::string frag = strip_comments(shader_source(GEOM_F));
        REQUIRE(!frag.empty());
        CHECK_FALSE(std::regex_search(frag, WEIGHT));
        CHECK_FALSE(std::regex_search(frag, std::regex(R"(\brows\s*\[)")));
        CHECK_FALSE(std::regex_search(frag, std::regex(R"(\bsk_cut_table\b)")));
    }

    TEST_CASE("SKOverviewShaders: the fragment stage tests neither the clip plane nor the ceiling", "[SKOverviewShaders]")
    {
        const std::string src = strip_comments(shader_source(GEOM_F));
        REQUIRE(!src.empty());
        CHECK_FALSE(std::regex_search(src, std::regex(R"(\bclip_plane\b)")));
        CHECK_FALSE(std::regex_search(src, std::regex(R"(\bsk_ceiling\b)")));
    }
}
