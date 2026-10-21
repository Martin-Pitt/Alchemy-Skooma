/**
 * @file skoverviewprograms.h
 * @brief The overview's programs, their stage files and defines: what SKOverviewCapture::loadShaders builds and the
 *        shader tests link, from one table.
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

#ifndef SK_OVERVIEWPROGRAMS_H
#define SK_OVERVIEWPROGRAMS_H

#include <array>
#include <cstddef>

namespace skoverview
{
    /// One of the overview's programs: its stage files and the defines it is built with, unused ones null.
    struct ProgramSpec
    {
        const char* mName;
        const char* mVertex;
        const char* mFragment;
        std::array<const char*, 3> mDefines;
        bool mCutTable = false; ///< reads SKOverviewCutTable: built with SK_CUT_TABLE_SSBO at GLSL 4.30 where it says useStorageBlock()
    };

    /// In this order; SKOverviewCapture pairs each with its LLGLSLShader by position.
    inline constexpr ProgramSpec PROGRAMS[] = {
        { "SK Overview Resolve", "deferred/postDeferredNoTCV.glsl", "skooma/overviewResolveF.glsl", {} },
        { "SK Overview Composite", "deferred/postDeferredNoTCV.glsl", "skooma/overviewCompositeF.glsl", {} },
        { "SK Overview Geometry", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl", {} },
        { "SK Overview Geometry Cut", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl", { "SK_CUT_ATTRIBUTE" }, true },
        { "SK Overview Geometry Cut Dissolve", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl",
          { "SK_CUT_ATTRIBUTE", "SK_DISSOLVE" }, true },
        { "SK Overview Geometry Cap Lift", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl",
          { "SK_CUT_ATTRIBUTE", "SK_CAP_LIFT" }, true },
        { "SK Overview Geometry Cap Lift Dissolve", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl",
          { "SK_CUT_ATTRIBUTE", "SK_CAP_LIFT", "SK_DISSOLVE" }, true },
        // The probes' id render (SKOverviewProbes): the linkset's id per fragment, reading no table.
        { "SK Overview Geometry Id", "skooma/overviewGeomV.glsl", "skooma/overviewGeomF.glsl",
          { "SK_CUT_ATTRIBUTE", "SK_ID_WRITE" } },
        { "SK Overview Marker", "skooma/overviewMarkerV.glsl", "skooma/overviewMarkerF.glsl", {} },
    };

    inline constexpr size_t PROGRAM_COUNT = sizeof(PROGRAMS) / sizeof(PROGRAMS[0]);

    /// The census pages' command lists, a compute program built only where the session computes (LLRender::sCompute);
    /// apart from PROGRAMS, whose entries are vertex and fragment pairs.
    inline constexpr const char* COMMANDS_NAME = "SK Overview Commands";
    inline constexpr const char* COMMANDS_COMPUTE = "skooma/overviewCommandsC.glsl";
}

#endif
