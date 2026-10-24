/**
 * @file skoverviewslots_gl_test.cpp
 * @brief The census pages' command lists on the hidden window: overviewCommandsC.glsl builds the commands
 *        skoverview::buildCommands builds, and an indirect multi-draw of them draws what the CPU's merged runs draw.
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

#include "../skoverviewprograms.h"
#include "../skoverviewslots.h"

#include "algpubuffer.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"

#include "../../llrender/tests/llheadlessgl_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using skoverview::SlotCull;

    // A point a slot, at its own pixel: what a run of the page draws shows where it lands.
    const char* POINT_V =
        "in vec3 position;\n"
        "void main()\n"
        "{\n"
        "    gl_Position = vec4(position.xy, 0.0, 1.0);\n"
        "}\n";

    const char* POINT_F =
        "out vec4 frag_color;\n"
        "void main()\n"
        "{\n"
        "    frag_color = vec4(1.0);\n"
        "}\n";

    constexpr U32 SIDE = 32;
    constexpr U32 SLOTS = SIDE * SIDE;
    const U32 SLOT_TABLE_BINDING = 6;
    const U32 COMMANDS_BINDING = 7;

    std::string readFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // Slots of one vertex each, every box somewhere in a 200 m cube, with heights and ground flags all over.
    std::vector<SlotCull> randomSlots(std::mt19937& rng)
    {
        std::uniform_real_distribution<F32> where(-50.f, 150.f);
        std::uniform_real_distribution<F32> size(0.f, 20.f);
        std::vector<SlotCull> slots(SLOTS);
        for (U32 i = 0; i < SLOTS; ++i)
        {
            SlotCull& slot = slots[i];
            slot.mFirst = i;
            slot.mCount = 1;
            slot.mGroup = i * 7;
            slot.mGround = rng() % 4 == 0 ? 1 : 0;
            for (S32 a = 0; a < 3; ++a)
            {
                slot.mLo[a] = where(rng);
                slot.mHi[a] = slot.mLo[a] + size(rng);
            }
            slot.mLo[3] = slot.mLo[2] - size(rng);
            slot.mHi[3] = slot.mHi[2] + size(rng);
        }
        return slots;
    }

    // Six planes through the cube, a pair an axis facing each other, tilted as a frustum's are.
    void randomPlanes(std::mt19937& rng, F32 planes[6][4])
    {
        std::uniform_real_distribution<F32> tilt(-0.4f, 0.4f);
        std::uniform_real_distribution<F32> low(-50.f, 20.f);
        std::uniform_real_distribution<F32> high(80.f, 150.f);
        for (S32 p = 0; p < 6; ++p)
        {
            F32 n[3] = { tilt(rng), tilt(rng), tilt(rng) };
            n[p / 2] = p % 2 ? 1.f : -1.f;
            const F32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            // Inside is n . (x - point) <= 0: the low side's plane keeps what is above it, the high side's what is below.
            F32 point[3] = { 50.f, 50.f, 50.f };
            point[p / 2] = p % 2 ? high(rng) : low(rng);
            planes[p][3] = 0.f;
            for (S32 a = 0; a < 3; ++a)
            {
                planes[p][a] = n[a] / len;
                planes[p][3] -= planes[p][a] * point[a];
            }
        }
    }

    // How far the slot's box is from deciding a plane differently; the two builders round differently right on one.
    F32 margin(const SlotCull& slot, const F32 planes[6][4])
    {
        F32 nearest = 1e30f;
        for (S32 p = 0; p < 6; ++p)
        {
            F32 d = planes[p][3];
            for (S32 a = 0; a < 3; ++a)
            {
                d += planes[p][a] * 0.5f * (slot.mLo[a] + slot.mHi[a]) - std::fabs(planes[p][a]) * 0.5f * (slot.mHi[a] - slot.mLo[a]);
            }
            nearest = llmin(nearest, std::fabs(d));
        }
        return nearest;
    }

    // A patch capture's inputs: column-major view-projection, the page's agent offset, the capture size and the patch
    // rectangle (left, bottom, right, top).
    struct Patch
    {
        F32 mViewProj[16];
        F32 mOffset[3];
        S32 mW, mH;
        S32 mRect[4];
    };

    // SKOverviewMovers::screenRectOf on plain floats: the box's screen rectangle with a pixel's margin, the whole view
    // when it straddles the eye's plane, none when wholly behind it. The x0/y0/x1/y1 it floors are returned for the
    // edge tolerance.
    bool screenRect(const Patch& p, const F32 lo[3], const F32 hi[3], S32 rect[4], F32 edges[4], bool& straddles)
    {
        U32 behind = 0;
        F32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (U32 corner = 0; corner < 8; ++corner)
        {
            const F32 at[4] = { corner & 1 ? hi[0] : lo[0], corner & 2 ? hi[1] : lo[1], corner & 4 ? hi[2] : lo[2], 1.f };
            F32 c[4] = {};
            for (S32 r = 0; r < 4; ++r)
            {
                for (S32 k = 0; k < 4; ++k)
                {
                    c[r] += p.mViewProj[k * 4 + r] * at[k];
                }
            }
            if (c[3] <= 1.0e-4f)
            {
                ++behind;
                continue;
            }
            const F32 x = (c[0] / c[3] * 0.5f + 0.5f) * p.mW;
            const F32 y = (c[1] / c[3] * 0.5f + 0.5f) * p.mH;
            x0 = llmin(x0, x);
            y0 = llmin(y0, y);
            x1 = llmax(x1, x);
            y1 = llmax(y1, y);
        }
        straddles = behind > 0 && behind < 8;
        edges[0] = x0; edges[1] = y0; edges[2] = x1; edges[3] = y1;
        if (behind == 8)
        {
            return false;
        }
        if (behind)
        {
            rect[0] = 0; rect[1] = 0; rect[2] = p.mW; rect[3] = p.mH;
            return true;
        }
        rect[0] = llclamp((S32)std::floor(x0) - 1, 0, p.mW);
        rect[1] = llclamp((S32)std::floor(y0) - 1, 0, p.mH);
        rect[2] = llclamp((S32)std::ceil(x1) + 1, 0, p.mW);
        rect[3] = llclamp((S32)std::ceil(y1) + 1, 0, p.mH);
        return rect[0] != rect[2] && rect[1] != rect[3];
    }

    // LLRect::overlaps: edges inclusive.
    bool overlaps(const S32 a[4], const S32 b[4])
    {
        return !(a[0] > b[2] || a[2] < b[0] || a[1] > b[3] || a[3] < b[1]);
    }

    struct skoverviewslots_gl_data
    {
        skoverviewslots_gl_data()
        : mShaders(mDir.prefix())
        {
            mDir.write(skoverview::COMMANDS_COMPUTE,
                       readFile(std::string(AL_TEST_SHADER_DIR) + "/class1/" + skoverview::COMMANDS_COMPUTE));
            mDir.write("test/slotPointV.glsl", POINT_V);
            mDir.write("test/slotPointF.glsl", POINT_F);
        }

        ~skoverviewslots_gl_data()
        {
            // A skipped case never raised sCompute, and the storage points assert it.
            if (ll_test::contextHasCompute())
            {
                gGL.unbindStorageBuffer(SLOT_TABLE_BINDING);
                gGL.unbindStorageBuffer(COMMANDS_BINDING);
            }
            LLGLSLShader::unbind();
            glUseProgram(0);
            mCommandsProgram.unload();
            mPointProgram.unload();
            mTable.release();
            mCommands.release();
            mPoints = nullptr;
            mTarget.release();
            mShaders.clearShaderObjects();
        }

        // The viewer's command program, built as SKOverviewCapture::loadShaders builds it.
        bool buildCommands()
        {
            mCommandsProgram.unload();
            mCommandsProgram.mName = skoverview::COMMANDS_NAME;
            mCommandsProgram.mShaderFiles.clear();
            mCommandsProgram.mShaderFiles.push_back(std::make_pair(skoverview::COMMANDS_COMPUTE, GL_COMPUTE_SHADER));
            mCommandsProgram.mShaderLevel = 1;
            mCommandsProgram.clearPermutations();
            return mCommandsProgram.createShader() && mCommandsProgram.isCompute();
        }

        bool buildPoints()
        {
            mPointProgram.unload();
            mPointProgram.mName = "census slot points (test)";
            mPointProgram.mShaderFiles.clear();
            mPointProgram.mShaderFiles.push_back(std::make_pair("test/slotPointV.glsl", GL_VERTEX_SHADER));
            mPointProgram.mShaderFiles.push_back(std::make_pair("test/slotPointF.glsl", GL_FRAGMENT_SHADER));
            mPointProgram.mShaderLevel = 1;
            mPointProgram.mFeatures.attachNothing = true;
            mPointProgram.clearPermutations();
            return mPointProgram.createShader();
        }

        // The GPU's commands for the slots, read back.
        std::vector<ALDrawArraysIndirectCommand> gpuCommands(const std::vector<SlotCull>& slots, const skoverview::LevelCull& level,
                                                             const F32 planes[6][4], const Patch* patch = nullptr)
        {
            static const LLStaticHashedString SK_SLOT_COUNT("sk_slot_count");
            static const LLStaticHashedString SK_LEVEL("sk_level");
            static const LLStaticHashedString SK_PLANES("sk_planes");
            static const LLStaticHashedString SK_PATCH_ON("sk_patch_on");
            static const LLStaticHashedString SK_VIEW_PROJ("sk_view_proj");
            static const LLStaticHashedString SK_OFFSET_AGENT("sk_offset_agent");
            static const LLStaticHashedString SK_SIZE("sk_size");
            static const LLStaticHashedString SK_PATCH("sk_patch");
            const size_t table_bytes = slots.size() * sizeof(SlotCull);
            const size_t command_bytes = slots.size() * sizeof(ALDrawArraysIndirectCommand);
            if (mTable.size() != table_bytes)
            {
                REQUIRE(mTable.allocate(table_bytes, ALGPUBuffer::DYNAMIC, slots.data(), "test slots"));
            }
            else
            {
                mTable.upload(0, slots.data(), table_bytes);
            }
            if (mCommands.size() != command_bytes)
            {
                REQUIRE(mCommands.allocate(command_bytes, 0, nullptr, "test commands"));
            }
            mCommands.clear(0xFFFFFFFFu);
            mCommandsProgram.bind();
            const F32 l[4] = { level.mAll ? 1.f : 0.f, level.mGround ? 1.f : 0.f, level.mLo, level.mHi };
            glUniform4fv(mCommandsProgram.getUniformLocation(SK_LEVEL), 1, l);
            glUniform4fv(mCommandsProgram.getUniformLocation(SK_PLANES), 6, &planes[0][0]);
            glUniform1ui(mCommandsProgram.getUniformLocation(SK_SLOT_COUNT), (GLuint)slots.size());
            glUniform1ui(mCommandsProgram.getUniformLocation(SK_PATCH_ON), patch ? 1u : 0u);
            if (patch)
            {
                const GLint size[4] = { patch->mW, patch->mH, 0, 0 };
                glUniformMatrix4fv(mCommandsProgram.getUniformLocation(SK_VIEW_PROJ), 1, GL_FALSE, patch->mViewProj);
                glUniform3fv(mCommandsProgram.getUniformLocation(SK_OFFSET_AGENT), 1, patch->mOffset);
                glUniform4iv(mCommandsProgram.getUniformLocation(SK_SIZE), 1, size);
                glUniform4iv(mCommandsProgram.getUniformLocation(SK_PATCH), 1, patch->mRect);
            }
            gGL.bindStorageBuffer(SLOT_TABLE_BINDING, mTable);
            gGL.bindStorageBuffer(COMMANDS_BINDING, mCommands);
            mCommandsProgram.dispatchThreads((U32)slots.size());
            gGL.memoryBarrier(LLRender::BARRIER_COMMAND | LLRender::BARRIER_BUFFER_UPDATE);
            LLGLSLShader::unbind();
            std::vector<ALDrawArraysIndirectCommand> commands(slots.size());
            REQUIRE(mCommands.read(0, commands.data(), command_bytes));
            return commands;
        }

        // The points the slots' runs light, drawn one of the two ways.
        std::vector<U8> drawn(const std::vector<S32>* first, const std::vector<S32>* count, size_t indirect_count)
        {
            LLGLDisable blend(GL_BLEND);
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            mTarget.bindTarget();
            gGL.setClearColor(LLColor4::transparent);
            mTarget.clear();
            mPointProgram.bind();
            mPoints->setBuffer();
            if (first)
            {
                mPoints->multiDrawArrays(LLRender::POINTS, first->data(), count->data(), (GLsizei)first->size());
            }
            else
            {
                mPoints->multiDrawArraysIndirect(LLRender::POINTS, mCommands, 0, (GLsizei)indirect_count);
            }
            std::vector<U8> pixels(SIDE * SIDE * 4);
            glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            mTarget.flush();
            LLGLSLShader::unbind();
            std::vector<U8> lit(SLOTS);
            for (U32 i = 0; i < SLOTS; ++i)
            {
                lit[i] = pixels[i * 4] > 127 ? 1 : 0;
            }
            return lit;
        }

        ll_test::SharedGLScope mGL;
        ll_test::TestShaderDir mDir;
        ll_test::TestShaderMgr mShaders;
        LLGLSLShader mCommandsProgram;
        LLGLSLShader mPointProgram;
        ALGPUBuffer mTable;
        ALGPUBuffer mCommands;
        LLPointer<LLVertexBuffer> mPoints;
        LLRenderTarget mTarget;
    };

    TEST_CASE_METHOD(skoverviewslots_gl_data, "skoverviewslots_gl: the GPU builds the commands the CPU builds",
                     "[skoverviewslots][gl]")
    {
        if (!ll_test::contextHasCompute())
        {
            WARN("the context has no compute: the census builds its command lists on the CPU");
            SKIP("no compute");
        }
        LLRender::sCompute = true;
        REQUIRE(buildCommands());
        std::mt19937 rng(20261021u);
        U32 compared = 0, kept = 0, near_plane = 0;
        for (S32 trial = 0; trial < 24; ++trial)
        {
            CAPTURE(trial);
            const std::vector<SlotCull> slots = randomSlots(rng);
            F32 planes[6][4];
            randomPlanes(rng, planes);
            skoverview::LevelCull level;
            level.mAll = trial % 3 == 0;
            level.mGround = trial % 2 == 0;
            level.mLo = 20.f + F32(trial);
            level.mHi = level.mLo + 15.f;
            std::vector<U32> first, count;
            skoverview::buildCommands(slots, level, planes, first, count);
            const std::vector<ALDrawArraysIndirectCommand> gpu = gpuCommands(slots, level, planes);
            for (U32 i = 0; i < SLOTS; ++i)
            {
                CAPTURE(i);
                CHECK(gpu[i].mFirst == first[i]);
                CHECK(gpu[i].mInstanceCount == 1u);
                CHECK(gpu[i].mBaseInstance == 0u);
                if (margin(slots[i], planes) < 1.0e-3f)
                {
                    ++near_plane; // a box touching a plane: either answer draws the same pixels
                    continue;
                }
                CHECK(gpu[i].mCount == count[i]);
                ++compared;
                kept += count[i] ? 1 : 0;
            }
        }
        INFO("compared " << compared << ", kept " << kept << ", on a plane " << near_plane);
        CHECK(near_plane < compared / 1000 + 1);
        // Both kept and culled slots in numbers, so neither answer passes by itself.
        CHECK(kept > compared / 20);
        CHECK(kept < compared - compared / 20);
        CHECK(glGetError() == GLenum(GL_NO_ERROR));
    }

    TEST_CASE_METHOD(skoverviewslots_gl_data, "skoverviewslots_gl: a patch keeps the slots the CPU's screen test keeps",
                     "[skoverviewslots][gl]")
    {
        if (!ll_test::contextHasCompute())
        {
            WARN("the context has no compute: a patch builds its command lists on the CPU");
            SKIP("no compute");
        }
        LLRender::sCompute = true;
        REQUIRE(buildCommands());
        // Planes that hold everything and a level that keeps everything, so only the patch decides.
        F32 planes[6][4] = {};
        for (S32 p = 0; p < 6; ++p)
        {
            planes[p][3] = -1.f;
        }
        skoverview::LevelCull level;
        level.mAll = true;
        std::mt19937 rng(20261024u);
        std::uniform_real_distribution<F32> shift(-10.f, 10.f);
        std::uniform_int_distribution<S32> corner(0, 40);
        U32 compared = 0, kept = 0, straddled = 0, on_edge = 0;
        for (S32 trial = 0; trial < 16; ++trial)
        {
            CAPTURE(trial);
            const std::vector<SlotCull> slots = randomSlots(rng);
            // An eye at (50, 50, -20) looking up +z, a 90 degree view: boxes below it are behind or straddle it.
            Patch patch = {};
            patch.mViewProj[0] = 1.f;
            patch.mViewProj[12] = -50.f;
            patch.mViewProj[5] = 1.f;
            patch.mViewProj[13] = -50.f;
            patch.mViewProj[10] = 1.f;
            patch.mViewProj[11] = 1.f;
            patch.mViewProj[15] = 20.f;
            patch.mOffset[0] = shift(rng);
            patch.mOffset[1] = shift(rng);
            patch.mOffset[2] = shift(rng);
            patch.mW = 96;
            patch.mH = 64;
            patch.mRect[0] = corner(rng);
            patch.mRect[1] = corner(rng) / 2;
            patch.mRect[2] = patch.mRect[0] + 8 + corner(rng);
            patch.mRect[3] = patch.mRect[1] + 6 + corner(rng) / 2;
            const std::vector<ALDrawArraysIndirectCommand> gpu = gpuCommands(slots, level, planes, &patch);
            for (U32 i = 0; i < SLOTS; ++i)
            {
                CAPTURE(i);
                const F32 lo[3] = { slots[i].mLo[0] + patch.mOffset[0], slots[i].mLo[1] + patch.mOffset[1],
                                    slots[i].mLo[2] + patch.mOffset[2] };
                const F32 hi[3] = { slots[i].mHi[0] + patch.mOffset[0], slots[i].mHi[1] + patch.mOffset[1],
                                    slots[i].mHi[2] + patch.mOffset[2] };
                S32 rect[4];
                F32 edges[4];
                bool straddles = false;
                const bool keep = screenRect(patch, lo, hi, rect, edges, straddles) && overlaps(rect, patch.mRect);
                CHECK(gpu[i].mFirst == slots[i].mFirst);
                CHECK(gpu[i].mInstanceCount == 1u);
                straddled += straddles ? 1 : 0;
                // A rectangle edge that floors or ceils right on a pixel boundary rounds either way on the two.
                bool rounding = false;
                for (S32 e = 0; !straddles && e < 4; ++e)
                {
                    rounding = rounding || std::fabs(edges[e] - std::round(edges[e])) < 1.0e-3f + 1.0e-5f * std::fabs(edges[e]);
                }
                if (rounding)
                {
                    ++on_edge;
                    continue;
                }
                CHECK(gpu[i].mCount == (keep ? slots[i].mCount : 0u));
                ++compared;
                kept += keep ? 1 : 0;
            }
        }
        INFO("compared " << compared << ", kept " << kept << ", straddling " << straddled << ", on an edge " << on_edge);
        CHECK(on_edge < compared / 100 + 1);
        CHECK(straddled > 0u);
        CHECK(kept > compared / 20);
        CHECK(kept < compared - compared / 20);
        CHECK(glGetError() == GLenum(GL_NO_ERROR));
    }

    TEST_CASE_METHOD(skoverviewslots_gl_data, "skoverviewslots_gl: an indirect multi-draw draws what the merged runs draw",
                     "[skoverviewslots][gl]")
    {
        if (!ll_test::contextHasCompute())
        {
            WARN("the context has no compute: the census draws the CPU's merged runs");
            SKIP("no compute");
        }
        LLRender::sCompute = true;
        REQUIRE(buildCommands());
        REQUIRE(buildPoints());
        REQUIRE(mTarget.allocate(SIDE, SIDE, GL_RGBA8));
        // A point at each slot's pixel centre, in the slot's vertex order.
        mPoints = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        REQUIRE(mPoints->allocateBuffer(SLOTS, 0));
        LLStrider<LLVector3> verts;
        REQUIRE(mPoints->getVertexStrider(verts));
        for (U32 i = 0; i < SLOTS; ++i)
        {
            *verts++ = LLVector3(((i % SIDE) + 0.5f) / SIDE * 2.f - 1.f, ((i / SIDE) + 0.5f) / SIDE * 2.f - 1.f, 0.f);
        }
        mPoints->unmapBuffer();

        std::mt19937 rng(7u);
        for (S32 trial = 0; trial < 4; ++trial)
        {
            CAPTURE(trial);
            const std::vector<SlotCull> slots = randomSlots(rng);
            F32 planes[6][4];
            randomPlanes(rng, planes);
            skoverview::LevelCull level;
            level.mAll = false;
            level.mGround = trial % 2 == 0;
            level.mLo = 30.f;
            level.mHi = 60.f;
            std::vector<U32> first, count;
            skoverview::buildCommands(slots, level, planes, first, count);
            std::vector<S32> runs_first, runs_count;
            skoverview::mergeRuns(first, count, runs_first, runs_count);
            gpuCommands(slots, level, planes);
            const std::vector<U8> by_cpu = drawn(&runs_first, &runs_count, 0);
            const std::vector<U8> by_gpu = drawn(nullptr, nullptr, slots.size());
            U32 lit = 0;
            for (U32 i = 0; i < SLOTS; ++i)
            {
                if (margin(slots[i], planes) < 1.0e-3f)
                {
                    continue;
                }
                CAPTURE(i);
                CHECK(by_gpu[i] == by_cpu[i]);
                CHECK(by_cpu[i] == (count[i] ? 1 : 0));
                lit += by_cpu[i];
            }
            CHECK(lit > 0u);
            CHECK(lit < SLOTS);
        }
        CHECK(glGetError() == GLenum(GL_NO_ERROR));
    }
}
