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
            gGL.unbindStorageBuffer(SLOT_TABLE_BINDING);
            gGL.unbindStorageBuffer(COMMANDS_BINDING);
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
                                                             const F32 planes[6][4])
        {
            static const LLStaticHashedString SK_SLOT_COUNT("sk_slot_count");
            static const LLStaticHashedString SK_LEVEL("sk_level");
            static const LLStaticHashedString SK_PLANES("sk_planes");
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
