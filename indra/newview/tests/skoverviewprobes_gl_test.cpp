/**
 * @file skoverviewprobes_gl_test.cpp
 * @brief The tactical cut's probes on the GPU: membership queries over a footprint mask, the feet texel, the pick, the
 *        pack-buffer readback, with the overview's own id program.
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

#include "../skoverviewprobes.h"
#include "../skoverviewprograms.h"

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

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
    using Run = SKOverviewProbes::Run;
    using Box = SKOverviewProbes::Box;

    /// A test shape: four corners (a quad, two triangles), the linkset id it carries, and whether it is passed through.
    struct Shape
    {
        std::array<LLVector3, 4> mCorner;
        U32 mId = 0;
        bool mPassThrough = false;
    };

    Shape flat(F32 x0, F32 y0, F32 x1, F32 y1, F32 z, U32 id, bool pass_through = false)
    {
        return { { LLVector3(x0, y0, z), LLVector3(x1, y0, z), LLVector3(x1, y1, z), LLVector3(x0, y1, z) }, id, pass_through };
    }

    /// A vertical wall along (x0, y0)-(x1, y1), z0 to z1: edge-on to a probe looking along z.
    Shape wall(F32 x0, F32 y0, F32 x1, F32 y1, F32 z0, F32 z1, U32 id)
    {
        return { { LLVector3(x0, y0, z0), LLVector3(x1, y1, z0), LLVector3(x1, y1, z1), LLVector3(x0, y0, z1) }, id, false };
    }

    struct skoverviewprobes_gl_data
    {
        skoverviewprobes_gl_data()
        : mShaders(std::string(AL_TEST_SHADER_DIR) + "/class")
        {
            setConvention(false);
        }

        ~skoverviewprobes_gl_data()
        {
            mProbes.release();
            LLGLSLShader::unbind();
            glUseProgram(0);
            mProgram.unload();
            mShaders.clearShaderObjects();
            mBuffer = nullptr;
            mDepth.release();
            if (LLRender::sReverseZ)
            {
                setConvention(false);
            }
        }

        static std::vector<bool> conventions()
        {
            std::vector<bool> reverse = { false };
            if (gGLManager.mHasClipControl)
            {
                reverse.push_back(true);
            }
            return reverse;
        }

        void setConvention(bool reverse)
        {
            LLRender::sReverseZ = reverse;
            if (gGLManager.mHasClipControl)
            {
                glClipControl(GL_LOWER_LEFT, reverse ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
            }
            glClearDepth(reverse ? 0.0 : 1.0);
            LLGLDepthTest::rebase();
        }

        // The id program, built from the table SKOverviewCapture::loadShaders builds it from, over the shared objects.
        void buildProgram(bool reverse)
        {
            setConvention(reverse);
            mProbes.release();
            mProgram.unload();
            mShaders.clearShaderObjects();
            std::map<std::string, std::string> defines;
            for (const char* object : { "deferred/textureUtilV.glsl", "objects/nonindexedTextureV.glsl" })
            {
                S32 level = 1;
                REQUIRE(mShaders.loadShaderFile(object, level, GL_VERTEX_SHADER, &defines) != 0);
            }
            S32 level = 1;
            REQUIRE(mShaders.loadShaderFile("deferred/globalF.glsl", level, GL_FRAGMENT_SHADER, &defines) != 0);
            const skoverview::ProgramSpec* spec = nullptr;
            for (const skoverview::ProgramSpec& program : skoverview::PROGRAMS)
            {
                if (std::string(program.mName) == "SK Overview Geometry Id")
                {
                    spec = &program;
                }
            }
            REQUIRE(spec);
            mProgram.mName = spec->mName;
            mProgram.mShaderFiles.clear();
            mProgram.mShaderFiles.push_back(std::make_pair(spec->mVertex, GL_VERTEX_SHADER));
            mProgram.mShaderFiles.push_back(std::make_pair(spec->mFragment, GL_FRAGMENT_SHADER));
            mProgram.mShaderLevel = 1;
            mProgram.clearPermutations();
            for (const char* define : spec->mDefines)
            {
                if (define)
                {
                    mProgram.addPermutation(define, "1");
                }
            }
            REQUIRE(mProgram.createShader());
        }

        // The shapes in the census's vertex format (position, the id as weight, the flags as texcoord0), a run each.
        std::vector<Run> upload(const std::vector<Shape>& shapes)
        {
            mBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_WEIGHT | LLVertexBuffer::MAP_TEXCOORD0);
            const U32 count = (U32)shapes.size() * 6;
            REQUIRE(mBuffer->allocateBuffer(count, 0));
            LLStrider<LLVector3> verts;
            LLStrider<F32> weights;
            LLStrider<LLVector2> flags;
            REQUIRE(mBuffer->getVertexStrider(verts));
            REQUIRE(mBuffer->getWeightStrider(weights));
            REQUIRE(mBuffer->getTexCoord0Strider(flags));
            std::vector<Run> runs;
            for (size_t s = 0; s < shapes.size(); ++s)
            {
                const Shape& shape = shapes[s];
                for (S32 i : { 0, 1, 2, 0, 2, 3 })
                {
                    *verts++ = shape.mCorner[i];
                    *weights++ = (F32)shape.mId;
                    *flags++ = LLVector2(0.f, shape.mPassThrough ? 1.f : 0.f);
                }
                runs.push_back({ mBuffer.get(), (U32)s * 6, 6, LLVector3::zero });
            }
            mBuffer->unmapBuffer();
            LLVertexBuffer::unbind();
            return runs;
        }

        // What each run's own query counted, inside one volume.
        std::vector<U64> counts(const Box& box, const std::vector<LLVector4>& outs, const std::vector<Run>& runs, bool edges)
        {
            REQUIRE(mProbes.beginFrame(mProgram, 8));
            REQUIRE(mProbes.beginVolume(box, outs));
            std::vector<ALQueryTicket> tickets;
            for (const Run& run : runs)
            {
                tickets.push_back(mProbes.query({ run }, edges));
            }
            mProbes.endVolume();
            mProbes.endFrame();
            std::vector<U64> out;
            for (const ALQueryTicket& ticket : tickets)
            {
                REQUIRE(ticket.isValid());
                U64 value = 0;
                REQUIRE(gGL.queries().wait(ticket, value));
                out.push_back(value);
            }
            return out;
        }

        // The texel and height straight down through the point, from z to z - depth.
        bool feet(F32 x, F32 y, F32 top, F32 bottom, const std::vector<Run>& runs, U32& texel, F32& z)
        {
            REQUIRE(mProbes.beginFrame(mProgram, 8));
            Box box;
            box.mMinX = x - 0.01f;
            box.mMaxX = x + 0.01f;
            box.mMinY = y - 0.01f;
            box.mMaxY = y + 0.01f;
            box.mFrom = top;
            box.mTo = bottom;
            const S32 slot = mProbes.sample(box, runs);
            REQUIRE(slot == 0);
            const U32 frame = mProbes.frame();
            F32 depth = 0.f;
            CHECK(mProbes.sampleResult(frame, slot, texel, depth) == SKOverviewProbes::Status::Pending);
            mProbes.endFrame();
            mProbes.collect(true);
            REQUIRE(mProbes.sampleResult(frame, slot, texel, depth) == SKOverviewProbes::Status::Ready);
            U32 id = 0;
            return skoverview::feetSurface(texel, depth, box.nearZ(), box.farZ(), LLRender::sReverseZ, id, z);
        }

        static std::string dumpDir()
        {
            const char* dir = std::getenv("SK_PROBE_DUMP_DIR");
            return dir && *dir ? std::string(dir) : std::string();
        }

        ll_test::SharedGLScope mGL;
        ll_test::TestShaderMgr mShaders;
        LLGLSLShader mProgram;
        SKOverviewProbes mProbes;
        LLPointer<LLVertexBuffer> mBuffer;
        LLRenderTarget mDepth;
    };

    // A 4 x 4 m box of 1 m cells, four texels a cell, the last cell left out of the room.
    Box roomBox()
    {
        Box box;
        box.mMaxX = 4.f;
        box.mMaxY = 4.f;
        box.mFrom = 0.5f;
        box.mTo = 6.f;
        box.mWidth = 16;
        box.mHeight = 16;
        return box;
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data,
                     "skoverviewprobes: a quad over a masked cell passes its query and one over the unmasked cell does not",
                     "[skoverviewprobes][gl]")
    {
        if (!ALQueryPool::supports(ALQueryKind::OcclusionExact))
        {
            SKIP("the context has no exact occlusion queries");
        }
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ flat(0.2f, 0.2f, 0.8f, 0.8f, 2.f, 11), flat(3.2f, 3.2f, 3.8f, 3.8f, 2.f, 12) });
            const std::vector<U64> passed = counts(roomBox(), { LLVector4(3.f, 3.f, 4.f, 4.f) }, runs, true);
            CHECK(passed[0] > 0);
            CHECK(passed[1] == 0);
            CHECK(glGetError() == GLenum(GL_NO_ERROR));
        }
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: two stacked quads both pass the roof behind the ceiling included",
                     "[skoverviewprobes][gl]")
    {
        if (!ALQueryPool::supports(ALQueryKind::OcclusionExact))
        {
            SKIP("the context has no exact occlusion queries");
        }
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ flat(0.f, 0.f, 4.f, 4.f, 2.f, 3), flat(0.f, 0.f, 4.f, 4.f, 3.f, 7) });
            const std::vector<U64> passed = counts(roomBox(), {}, runs, false);
            CHECK(passed[0] > 0);
            CHECK(passed[1] > 0);
        }
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: a wall seen edge-on passes only with its edges drawn",
                     "[skoverviewprobes][gl]")
    {
        if (!ALQueryPool::supports(ALQueryKind::OcclusionExact))
        {
            SKIP("the context has no exact occlusion queries");
        }
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ wall(1.3f, 0.2f, 1.3f, 3.8f, 0.f, 3.f, 5) });
            CHECK(counts(roomBox(), {}, runs, false)[0] == 0);
            CHECK(counts(roomBox(), {}, runs, true)[0] > 0);
            // Over the cell left out it fails either way.
            const std::vector<Run> outside = upload({ wall(3.5f, 3.1f, 3.5f, 3.9f, 0.f, 3.f, 6) });
            CHECK(counts(roomBox(), { LLVector4(3.f, 3.f, 4.f, 4.f) }, outside, true)[0] == 0);
        }
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: the feet texel reads the upper quad's id and height", "[skoverviewprobes][gl]")
    {
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ flat(0.f, 0.f, 4.f, 4.f, 2.f, 3), flat(0.f, 0.f, 4.f, 4.f, 3.f, 7) });
            U32 texel = 0;
            F32 z = 0.f;
            REQUIRE(feet(1.f, 1.f, 4.f, 0.f, runs, texel, z));
            CHECK(texel == 8);
            CHECK(std::fabs(z - 3.f) < 1.0e-3f);
            // A band that stops above the upper quad finds nothing; one that starts under it finds the lower.
            CHECK_FALSE(feet(1.f, 1.f, 4.f, 3.2f, runs, texel, z));
            REQUIRE(feet(1.f, 1.f, 2.9f, 0.f, runs, texel, z));
            CHECK(texel == 4);
            CHECK(std::fabs(z - 2.f) < 1.0e-3f);
        }
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: a feet texel under nothing reads zero", "[skoverviewprobes][gl]")
    {
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ flat(0.f, 0.f, 4.f, 4.f, 2.f, 3) });
            U32 texel = 99;
            F32 z = 0.f;
            CHECK_FALSE(feet(10.f, 10.f, 4.f, 0.f, runs, texel, z));
            CHECK(texel == 0);
        }
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: collect waits for the fence and returns the request's bytes",
                     "[skoverviewprobes][gl]")
    {
        buildProgram(false);
        const std::vector<Run> runs = upload({ flat(0.f, 0.f, 4.f, 4.f, 2.f, 41) });
        REQUIRE(mDepth.allocate(8, 8, GL_RGBA8, true, false, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE,
                                LLRenderTarget::DEPTH_FMT_32F));
        mDepth.bindTarget();
        {
            LLGLDepthTest depth_write(GL_TRUE, GL_TRUE, GL_ALWAYS);
            glClearDepth(0.25);
            mDepth.clear();
            glClearDepth(1.0);
        }
        mDepth.flush();
        REQUIRE(mProbes.beginFrame(mProgram, 4));
        const U32 frame = mProbes.frame();
        Box box;
        box.mMinX = 0.99f;
        box.mMaxX = 1.01f;
        box.mMinY = 0.99f;
        box.mMaxY = 1.01f;
        box.mFrom = 4.f;
        box.mTo = 0.f;
        REQUIRE(mProbes.sample(box, runs) == 0);
        REQUIRE(mProbes.sample(box, {}) == 1);
        REQUIRE(mProbes.readDepth(mDepth, 3, 4));
        mProbes.endFrame();
        U32 texel = 0;
        F32 depth = 0.f;
        F32 pick = 0.f;
        CHECK(mProbes.sampleResult(frame, 0, texel, depth) == SKOverviewProbes::Status::Pending);
        mProbes.collect(true);
        REQUIRE(mProbes.sampleResult(frame, 0, texel, depth) == SKOverviewProbes::Status::Ready);
        CHECK(texel == 42);
        REQUIRE(mProbes.sampleResult(frame, 1, texel, depth) == SKOverviewProbes::Status::Ready);
        CHECK(texel == 0);
        CHECK(depth == 1.f);
        REQUIRE(mProbes.pickResult(frame, pick) == SKOverviewProbes::Status::Ready);
        CHECK(std::fabs(pick - 0.25f) < 1.0e-6f);
        // A frame long gone is lost, never pending.
        CHECK(mProbes.sampleResult(frame + 100, 0, texel, depth) == SKOverviewProbes::Status::Lost);
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data, "skoverviewprobes: a pass-through triangle is clipped", "[skoverviewprobes][gl]")
    {
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload({ flat(0.f, 0.f, 4.f, 4.f, 3.f, 7), flat(0.f, 0.f, 4.f, 4.f, 3.5f, 9, true),
                                                   flat(0.f, 0.f, 4.f, 4.f, 3.5f, 9) });
            U32 texel = 0;
            F32 z = 0.f;
            REQUIRE(feet(1.f, 1.f, 4.f, 0.f, { runs[0], runs[1] }, texel, z));
            CHECK(texel == 8);
            REQUIRE(feet(1.f, 1.f, 4.f, 0.f, { runs[0], runs[2] }, texel, z));
            CHECK(texel == 10);
            if (ALQueryPool::supports(ALQueryKind::OcclusionExact))
            {
                const std::vector<U64> passed = counts(roomBox(), {}, { runs[1], runs[2] }, true);
                CHECK(passed[0] == 0);
                CHECK(passed[1] > 0);
            }
        }
    }

    /// The picture's scene: a room of 0.5 m cells, 12 x 8 with its north-east corner left out, and the shapes standing
    /// round, over and under it.
    struct Scene
    {
        Box mBox;
        std::vector<LLVector4> mOuts;
        std::vector<Shape> mShapes;
        std::vector<bool> mExpect;
        std::vector<const char*> mNames;
    };

    Scene roomScene()
    {
        Scene s;
        s.mBox.mMaxX = 6.f;
        s.mBox.mMaxY = 4.f;
        s.mBox.mFrom = 0.5f;
        s.mBox.mTo = 6.f;
        s.mBox.mWidth = 48;
        s.mBox.mHeight = 32;
        s.mOuts = { LLVector4(4.f, 2.5f, 6.f, 4.f) };
        auto add = [&s](const Shape& shape, bool expect, const char* name)
        {
            s.mShapes.push_back(shape);
            s.mExpect.push_back(expect);
            s.mNames.push_back(name);
        };
        add(flat(0.2f, 0.2f, 5.8f, 2.3f, 3.f, 1), true, "the floor above");
        add(wall(0.3f, 0.2f, 0.3f, 3.8f, 0.f, 3.f, 2), true, "the west wall, edge-on");
        add({ { LLVector3(2.5f, 1.f, 0.f), LLVector3(5.5f, 1.f, 2.f), LLVector3(5.5f, 3.5f, 2.f), LLVector3(2.5f, 3.5f, 0.f) }, 7, false }, true,
            "a ramp rising into the corner");
        add(wall(5.f, 2.7f, 5.f, 3.8f, 0.f, 3.f, 3), false, "a wall in the corner left out");
        add(flat(4.2f, 2.7f, 5.8f, 3.8f, 3.2f, 4), false, "a roof over the corner");
        add(flat(1.f, 1.f, 2.f, 2.f, 0.3f, 5), false, "cover under the probe's plane");
        add(flat(1.f, 2.4f, 3.f, 3.8f, 2.5f, 6, true), false, "an invisible part passed through");
        return s;
    }

    TEST_CASE_METHOD(skoverviewprobes_gl_data,
                     "skoverviewprobes: a room's walls floors above and ramps pass and what lies outside below or passed through does not",
                     "[skoverviewprobes][gl]")
    {
        if (!ALQueryPool::supports(ALQueryKind::OcclusionExact))
        {
            SKIP("the context has no exact occlusion queries");
        }
        const Scene scene = roomScene();
        for (bool reverse : conventions())
        {
            CAPTURE(reverse);
            buildProgram(reverse);
            const std::vector<Run> runs = upload(scene.mShapes);
            REQUIRE(mProbes.beginFrame(mProgram, 8));
            REQUIRE(mProbes.beginVolume(scene.mBox, scene.mOuts));
            std::vector<ALQueryTicket> tickets;
            for (const Run& run : runs)
            {
                tickets.push_back(mProbes.query({ run }, true));
            }
            mProbes.endVolume();
            REQUIRE(mProbes.requestDump());
            mProbes.endFrame();
            std::vector<U64> passed;
            for (size_t i = 0; i < tickets.size(); ++i)
            {
                CAPTURE(scene.mNames[i]);
                U64 value = 0;
                REQUIRE(gGL.queries().wait(tickets[i], value));
                CHECK((value > 0) == scene.mExpect[i]);
                passed.push_back(value);
            }
            std::vector<U32> ids;
            std::vector<F32> depths;
            S32 w = 0, h = 0;
            REQUIRE(mProbes.collectDump(ids, depths, w, h, true));
            REQUIRE(w == scene.mBox.mWidth);
            REQUIRE(h == scene.mBox.mHeight);
            const std::vector<bool> out = SKOverviewProbes::outTexels(scene.mBox, scene.mOuts);
            // Nothing is written where the mask left the room out, and only the passing shapes' ids are written at all.
            U32 written = 0;
            for (size_t t = 0; t < ids.size(); ++t)
            {
                CAPTURE(t);
                if (out[t])
                {
                    CHECK(ids[t] == 0);
                }
                if (ids[t])
                {
                    ++written;
                    const U32 id = ids[t] - 1;
                    CHECK((id == 1 || id == 2 || id == 7));
                }
            }
            CHECK(written > 0);
            const std::string dir = dumpDir();
            if (!dir.empty() && !reverse)
            {
                CHECK(SKOverviewProbes::writeImage(dir + "/probe-test.ppm", ids, w, h, out, 16));
                std::ofstream counts_file(dir + "/probe-test-queries.txt");
                for (size_t i = 0; i < passed.size(); ++i)
                {
                    counts_file << scene.mShapes[i].mId << "\t" << passed[i] << "\t" << scene.mNames[i] << "\n";
                }
            }
        }
    }
}
