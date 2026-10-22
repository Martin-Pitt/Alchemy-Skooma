/**
 * @file skoverviewcuttable_gl_test.cpp
 * @brief SKOverviewCutTable on the hidden window: what a flush publishes, and a vertex stage reading each row back
 *        through the buffer texture and, where the context computes, the storage block.
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

#include "../skoverviewcuttable.h"

#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"

#include "../../llrender/tests/llheadlessgl_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace
{
    using Row = SKOverviewCutTable::Row;

    // The overview's two reads of a row (overviewGeomV.glsl's skCutRow), here keyed on the instance, each point
    // drawn at its row's pixel with the row as its colour.
    const char* ROW_V =
        "#ifdef SK_CUT_TABLE_SSBO\n"
        "layout(std430) readonly buffer SKOverviewCutBlock { vec4 rows[]; };\n"
        "vec4 skCutRow(int group) { return rows[group]; }\n"
        "#else\n"
        "uniform samplerBuffer sk_cut_table;\n"
        "uniform int sk_cut_base;\n"
        "vec4 skCutRow(int group) { return texelFetch(sk_cut_table, sk_cut_base + group); }\n"
        "#endif\n"
        "uniform int sk_test_width;\n"
        "flat out vec4 vary_row;\n"
        "void main()\n"
        "{\n"
        "    int id = gl_InstanceID;\n"
        "    vary_row = skCutRow(id);\n"
        "    vec2 pixel = vec2(float(id % sk_test_width), float(id / sk_test_width)) + 0.5;\n"
        "    gl_Position = vec4(pixel / float(sk_test_width) * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";

    const char* ROW_F =
        "flat in vec4 vary_row;\n"
        "out vec4 frag_color;\n"
        "void main()\n"
        "{\n"
        "    frag_color = vary_row;\n"
        "}\n";

    constexpr U32 SIDE = 8;
    constexpr U32 ROWS = 50;

    // Distinct rows, whole and hidden among them; `salt` makes a second publish differ from the first everywhere.
    Row rowOf(U32 id, F32 salt)
    {
        Row row;
        row.mCut = id == 3 ? SKOverviewCutTable::WHOLE : id == 7 ? SKOverviewCutTable::HIDDEN : 20.f + F32(id) * 0.25f + salt;
        row.mFlags = F32(id % 4);
        row.mMinZ = F32(id) + salt;
        row.mMaxZ = F32(id) * 2.f + 1.f + salt;
        return row;
    }

    void fill(SKOverviewCutTable& table, F32 salt)
    {
        for (U32 id = 0; id < table.rows(); ++id)
        {
            table.row(id) = rowOf(id, salt);
        }
    }

    struct skoverviewcuttable_gl_data
    {
        skoverviewcuttable_gl_data()
        : mShaders(mDir.prefix())
        {
            mDir.write("test/cutRowV.glsl", ROW_V);
            mDir.write("test/cutRowF.glsl", ROW_F);
        }

        ~skoverviewcuttable_gl_data()
        {
            mTable.unbind();
            LLGLSLShader::unbind();
            glUseProgram(0);
            mProgram.unload();
            mTable.release();
            mTarget.release();
            mShaders.clearShaderObjects();
        }

        bool build(bool ssbo)
        {
            mProgram.unload();
            mProgram.mName = ssbo ? "cut row reader, storage block (test)" : "cut row reader, buffer texture (test)";
            mProgram.mShaderFiles.clear();
            mProgram.mShaderFiles.push_back(std::make_pair("test/cutRowV.glsl", GL_VERTEX_SHADER));
            mProgram.mShaderFiles.push_back(std::make_pair("test/cutRowF.glsl", GL_FRAGMENT_SHADER));
            mProgram.mShaderLevel = 1;
            mProgram.mFeatures.attachNothing = true;
            mProgram.mFeatures.mMinGLSLVersion = ssbo ? 430 : 0;
            mProgram.clearPermutations();
            if (ssbo)
            {
                mProgram.addPermutation("SK_CUT_TABLE_SSBO", "1");
            }
            return mProgram.createShader();
        }

        // Draws a point per row with the table bound for the program, and reads the rows back from the pixels.
        std::vector<Row> drawRows()
        {
            static const LLStaticHashedString WIDTH("sk_test_width");
            LLGLDisable blend(GL_BLEND);
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            mTarget.bindTarget();
            gGL.setClearColor(LLColor4::transparent);
            mTarget.clear();
            mProgram.bind();
            glUniform1i(mProgram.getUniformLocation(WIDTH), (GLint)SIDE);
            mTable.bind(mProgram);
            gGL.drawArraysInstanced(LLRender::POINTS, 0, 1, mTable.rows());
            std::vector<Row> read(SIDE * SIDE);
            glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_FLOAT, read.data());
            mTable.unbind();
            mTarget.flush();
            return read;
        }

        void checkRows(const std::vector<Row>& read, F32 salt)
        {
            for (U32 id = 0; id < ROWS; ++id)
            {
                const Row want = rowOf(id, salt);
                CAPTURE(id);
                CHECK(read[id].mCut == want.mCut);
                CHECK(read[id].mFlags == want.mFlags);
                CHECK(read[id].mMinZ == want.mMinZ);
                CHECK(read[id].mMaxZ == want.mMaxZ);
            }
        }

        ll_test::SharedGLScope mGL;
        ll_test::TestShaderDir mDir;
        ll_test::TestShaderMgr mShaders;
        LLGLSLShader mProgram;
        SKOverviewCutTable mTable;
        LLRenderTarget mTarget;
    };

    TEST_CASE_METHOD(skoverviewcuttable_gl_data, "skoverviewcuttable_gl: a flush publishes the rows at the offset it reports",
                     "[skoverviewcuttable][gl]")
    {
        mTable.resize(ROWS);
        fill(mTable, 0.f);
        REQUIRE(mTable.flush());
        CHECK(mTable.publishedRows() == ROWS);
        CHECK(mTable.offset() % sizeof(Row) == 0);
        std::vector<Row> read(ROWS);
        REQUIRE(mTable.buffer().read(mTable.offset(), read.data(), ROWS * sizeof(Row)));
        checkRows(read, 0.f);
        UNSCOPED_INFO("an unchanged table publishes nothing");
        CHECK_FALSE(mTable.flush());

        const size_t first = mTable.offset();
        fill(mTable, 100.f);
        REQUIRE(mTable.flush());
        UNSCOPED_INFO("the next publish goes to the next region");
        CHECK(mTable.offset() != first);
        REQUIRE(mTable.buffer().read(mTable.offset(), read.data(), ROWS * sizeof(Row)));
        checkRows(read, 100.f);

        SECTION("bound as the storage block where the session computes")
        {
            if (!ll_test::contextHasCompute() || gGLManager.mMaxVertexShaderStorageBlocks < 1)
            {
                SKIP("no compute");
            }
            LLRender::sCompute = true;
            CHECK(SKOverviewCutTable::useStorageBlock());
            REQUIRE(build(true));
            CHECK(mProgram.getStorageBinding(LLShaderMgr::SK_OVERVIEW_CUT_BLOCK) == 0);
            mProgram.bind();
            mTable.bind(mProgram);
            CHECK(gGL.getStorageBuffer(0) == mTable.buffer().getGLName());
            mTable.unbind();
            CHECK(gGL.getStorageBuffer(0) == 0u);
        }
    }

    // The data path both GL paths draw through, read back from what the vertex stage saw: each row at sk_cut_base plus
    // its id from the buffer texture (GL 4.1), and from the storage block's range where the session computes.
    TEST_CASE_METHOD(skoverviewcuttable_gl_data, "skoverviewcuttable_gl: the vertex stage reads every row the table published",
                     "[skoverviewcuttable][gl]")
    {
        REQUIRE(mTarget.allocate(SIDE, SIDE, GL_RGBA32F));
        mTable.resize(ROWS);
        fill(mTable, 0.f);
        REQUIRE(mTable.flush());
        // A second publish, so the rows read start past the buffer's head.
        fill(mTable, 100.f);
        REQUIRE(mTable.flush());
        REQUIRE(mTable.offset() > 0);

        SECTION("from the buffer texture")
        {
            LLRender::sCompute = false;
            CHECK_FALSE(SKOverviewCutTable::useStorageBlock());
            REQUIRE(build(false));
            const S32 channel = mProgram.getTextureChannel(LLShaderMgr::SK_OVERVIEW_CUT_TABLE);
            REQUIRE(channel >= 0);
            const std::vector<Row> read = drawRows();
            checkRows(read, 100.f);
            UNSCOPED_INFO("the unit is let go");
            CHECK(gGL.getTextureSlot(channel)->getCurrType() == ALTextureSlot::TT_NONE);
            CHECK(glGetError() == GLenum(GL_NO_ERROR));
        }
        SECTION("from the storage block")
        {
            if (!ll_test::contextHasCompute() || gGLManager.mMaxVertexShaderStorageBlocks < 1)
            {
                SKIP("no compute");
            }
            LLRender::sCompute = true;
            REQUIRE(build(true));
            CHECK(mProgram.getTextureChannel(LLShaderMgr::SK_OVERVIEW_CUT_TABLE) < 0);
            const std::vector<Row> read = drawRows();
            checkRows(read, 100.f);
            CHECK(glGetError() == GLenum(GL_NO_ERROR));
        }
    }

    // A table that outgrows its regions is reallocated, and the buffer texture follows it to the new buffer.
    TEST_CASE_METHOD(skoverviewcuttable_gl_data, "skoverviewcuttable_gl: the buffer texture follows a grown table",
                     "[skoverviewcuttable][gl]")
    {
        REQUIRE(mTarget.allocate(SIDE, SIDE, GL_RGBA32F));
        LLRender::sCompute = false;
        REQUIRE(build(false));
        mTable.resize(ROWS);
        fill(mTable, 0.f);
        REQUIRE(mTable.flush());
        checkRows(drawRows(), 0.f);
        const size_t region = mTable.buffer().size();
        mTable.resize(5000);
        fill(mTable, 7.f);
        REQUIRE(mTable.flush());
        CHECK(mTable.buffer().size() > region);
        checkRows(drawRows(), 7.f);
        CHECK(glGetError() == GLenum(GL_NO_ERROR));
    }
}
