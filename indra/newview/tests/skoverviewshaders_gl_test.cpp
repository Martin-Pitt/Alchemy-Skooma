/**
 * @file skoverviewshaders_gl_test.cpp
 * @brief Every program SKOverviewCapture::loadShaders builds, from the viewer's own shader files on the hidden window,
 *        under forward depth and reverse-Z: each geometry permutation, the resolve, the composite and the marker link.
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

#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"

#include "../skoverviewprograms.h"
#include "../../llrender/tests/llheadlessgl_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

namespace
{
    // The programs SKOverviewCapture::loadShaders builds, from the table it builds them from.
    using Program = skoverview::ProgramSpec;

    struct skoverviewshaders_data
    {
        skoverviewshaders_data()
        : mShaders(std::string(AL_TEST_SHADER_DIR) + "/class")
        {
            setConvention(false);
        }

        ~skoverviewshaders_data()
        {
            LLGLSLShader::unbind();
            glUseProgram(0);
            mProgram.unload();
            mShaders.clearShaderObjects();
            if (LLRender::sReverseZ)
            {
                setConvention(false);
            }
        }

        // Forward depth, and reverse-Z where the context can clip zero to one.
        static std::vector<bool> conventions()
        {
            std::vector<bool> reverse = { false };
            if (gGLManager.mHasClipControl)
            {
                reverse.push_back(true);
            }
            return reverse;
        }

        // The convention the shader manager compiles REVERSE_Z from.
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

        // The shared objects every program attaches (LLShaderMgr::attachShaderFeatures), compiled under the convention
        // as LLViewerShaderMgr::loadBasicShaders compiles them.
        void loadSharedObjects()
        {
            mProgram.unload();
            mShaders.clearShaderObjects();
            std::map<std::string, std::string> defines;
            for (const char* object : { "deferred/textureUtilV.glsl", "objects/nonindexedTextureV.glsl" })
            {
                CAPTURE(object);
                S32 level = 1;
                REQUIRE(mShaders.loadShaderFile(object, level, GL_VERTEX_SHADER, &defines) != 0);
            }
            S32 level = 1;
            REQUIRE(mShaders.loadShaderFile("deferred/globalF.glsl", level, GL_FRAGMENT_SHADER, &defines) != 0);
        }

        // The cut programs' two builds: the buffer texture, and the storage block where the context computes.
        static std::vector<bool> tableStores(const Program& program)
        {
            std::vector<bool> ssbo = { false };
            if (program.mCutTable && ll_test::contextHasCompute() && gGLManager.mMaxVertexShaderStorageBlocks >= 1)
            {
                ssbo.push_back(true);
            }
            return ssbo;
        }

        bool build(const Program& program, bool ssbo = false)
        {
            mProgram.unload();
            mProgram.mName = program.mName;
            mProgram.mShaderFiles.clear();
            mProgram.mShaderFiles.push_back(std::make_pair(program.mVertex, GL_VERTEX_SHADER));
            mProgram.mShaderFiles.push_back(std::make_pair(program.mFragment, GL_FRAGMENT_SHADER));
            mProgram.mShaderLevel = 1;
            mProgram.clearPermutations();
            for (const char* define : program.mDefines)
            {
                if (define)
                {
                    mProgram.addPermutation(define, "1");
                }
            }
            // As SKOverviewCapture::loadShaders builds them.
            if (ssbo)
            {
                mProgram.addPermutation("SK_CUT_TABLE_SSBO", "1");
            }
            mProgram.mFeatures.mMinGLSLVersion = ssbo ? 430 : 0;
            return mProgram.createShader();
        }

        ll_test::SharedGLScope mGL;
        ll_test::TestShaderMgr mShaders;
        LLGLSLShader mProgram;
    };

    TEST_CASE_METHOD(skoverviewshaders_data, "SKOverviewShaders: every overview program links under both depth conventions",
                     "[SKOverviewShaders]")
    {
        // Which path the section pass's conservative-depth qualifier takes on this context.
        bool conservative = gGLManager.mGLSLVersionMajor > 4 || (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 20);
        GLint extensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; i < extensions && !conservative; ++i)
        {
            const char* name = (const char*)glGetStringi(GL_EXTENSIONS, i);
            conservative = name && std::string(name) == "GL_ARB_conservative_depth";
        }
        WARN("GLSL " << gGLManager.mGLSLVersionMajor << "." << gGLManager.mGLSLVersionMinor << ", clip control "
                     << gGLManager.mHasClipControl << ", conservative depth qualifier " << (conservative ? "on" : "off"));
        if (!ll_test::contextHasCompute() || gGLManager.mMaxVertexShaderStorageBlocks < 1)
        {
            WARN("the cut programs' SK_CUT_TABLE_SSBO builds are skipped: the context has no compute or no vertex storage blocks");
        }
        for (bool reverse : conventions())
        {
            setConvention(reverse);
            loadSharedObjects();
            for (const Program& program : skoverview::PROGRAMS)
            {
                for (bool ssbo : tableStores(program))
                {
                    CAPTURE(reverse, program.mName, ssbo);
                    REQUIRE(build(program, ssbo));
                    CHECK(mProgram.isComplete());
                }
            }
        }
    }

    // The geometry programs keep the uniforms the overview sets by name and the attributes it feeds, so a rename is caught here.
    TEST_CASE_METHOD(skoverviewshaders_data, "SKOverviewShaders: the cut permutations keep their per-pass uniforms",
                     "[SKOverviewShaders]")
    {
        static const LLStaticHashedString CLIP_PLANE("clip_plane");
        static const LLStaticHashedString CEILING("sk_ceiling");
        static const LLStaticHashedString CAP_PASS("sk_cap_pass");
        static const LLStaticHashedString DISSOLVE("sk_dissolve");
        static const LLStaticHashedString CUT_BASE("sk_cut_base");
        loadSharedObjects();
        for (const Program& program : skoverview::PROGRAMS)
        {
            if (std::string(program.mVertex) != "skooma/overviewGeomV.glsl")
            {
                continue;
            }
            bool cut = false, dissolve = false;
            for (const char* define : program.mDefines)
            {
                cut = cut || (define && std::string(define) == "SK_CUT_ATTRIBUTE");
                dissolve = dissolve || (define && std::string(define) == "SK_DISSOLVE");
            }
            CHECK(program.mCutTable == cut);
            for (bool ssbo : tableStores(program))
            {
                CAPTURE(program.mName, ssbo);
                REQUIRE(build(program, ssbo));
                CHECK(mProgram.getUniformLocation(CLIP_PLANE) >= 0);
                CHECK(mProgram.getUniformLocation(CEILING) >= 0);
                CHECK((mProgram.getUniformLocation(CAP_PASS) >= 0) == cut);
                CHECK((mProgram.getUniformLocation(DISSOLVE) >= 0) == dissolve);
                // The census buffer carries texcoord0 for this: an open part's flag, which the cap pass's sign reads.
                CHECK(((mProgram.mAttributeMask & LLVertexBuffer::MAP_TEXCOORD0) != 0) == cut);
                // And weight for the linkset id, which reads the cut table: a buffer texture's unit and base, or the block.
                CHECK(((mProgram.mAttributeMask & LLVertexBuffer::MAP_WEIGHT) != 0) == cut);
                CHECK((mProgram.getTextureChannel(LLShaderMgr::SK_OVERVIEW_CUT_TABLE) >= 0) == (cut && !ssbo));
                CHECK((mProgram.getUniformLocation(CUT_BASE) >= 0) == (cut && !ssbo));
                CHECK((mProgram.getStorageBinding(LLShaderMgr::SK_OVERVIEW_CUT_BLOCK) == 0) == (cut && ssbo));
            }
        }
        CHECK(mShaders.getStorageBlockBinding(LLShaderMgr::SK_OVERVIEW_CUT_BLOCK) == 0);
    }
}
