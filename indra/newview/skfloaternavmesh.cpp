/**
 * @file skfloaternavmesh.cpp
 * @brief The navmesh console: what the census and the mesh are doing, and the overlay's switches.
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

#include "llviewerprecompiledheaders.h"

#include "skfloaternavmesh.h"

#include "llfloaterreg.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "skcaptureexport.h"
#include "sknavmesh.h"
#include "skworldcensus.h"

SKFloaterNavMesh::SKFloaterNavMesh(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterNavMesh::postBuild()
{
    getChild<LLUICtrl>("overlay_on")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onToggleOverlay(); });
    getChild<LLUICtrl>("physics_shapes")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTogglePhysicsShapes(); });
    getChild<LLUICtrl>("mark_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        getChild<LLUICtrl>("mark_status")->setValue(SKNavMesh::instance().markUnderCamera());
    });
    getChild<LLUICtrl>("export_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        getChild<LLUICtrl>("export_status")->setValue(SKNavMesh::instance().exportAll());
    });
    getChild<LLUICtrl>("capture_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        const S32 kind = llclamp(getChild<LLUICtrl>("capture_kind")->getValue().asInteger(), 0, 2);
        getChild<LLUICtrl>("capture_status")->setValue(skCaptureExport(static_cast<SKCaptureKind>(kind)));
    });
    getChild<LLUICtrl>("export_census_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        getChild<LLUICtrl>("export_census_status")->setValue(SKNavMesh::instance().exportCensus());
    });
    getChild<LLUICtrl>("export_vehicle_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        getChild<LLUICtrl>("export_vehicle_status")->setValue(SKNavMesh::instance().exportObject());
    });
    getChild<LLUICtrl>("annotate_button")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("sk_nav_annotate"); });
    return true;
}

void SKFloaterNavMesh::onToggleOverlay()
{
    const bool want = getChild<LLUICtrl>("overlay_on")->getValue().asBoolean();
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH) != want)
    {
        LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_NAVMESH);
    }
}

void SKFloaterNavMesh::onTogglePhysicsShapes()
{
    const bool want = getChild<LLUICtrl>("physics_shapes")->getValue().asBoolean();
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES) != want)
    {
        LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES);
    }
}

void SKFloaterNavMesh::draw()
{
    // Twice a second: the numbers move every frame, but nobody reads them
    // that fast and a redrawn text box is not free.
    if (mRefresh.getElapsedTimeF32() > 0.5f)
    {
        mRefresh.reset();
        // The Develop menu toggles the same bits, so the boxes follow the
        // masks rather than the other way about.
        const bool on = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);
        getChild<LLUICtrl>("overlay_on")->setValue(on);
        getChild<LLUICtrl>("physics_shapes")->setValue(gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES));
        // The mesh runs for a layer on its own too, so the line follows what
        // is being built, not the surface switch.
        std::string mesh_line = getString("overlay_off");
        if (SKNavMesh::overlayActive() || gSavedSettings.getBOOL("SKNavMeshAlways"))
        {
            mesh_line = SKNavMesh::instance().statsLine();
        }
        getChild<LLUICtrl>("mesh_status")->setValue(mesh_line);
        getChild<LLUICtrl>("overlay_wind_heading")->setEnabled(gSavedSettings.getBOOL("SKNavOverlayWind"));
        getChild<LLUICtrl>("census_status")->setValue(SKWorldCensus::instance().statsLine());
        if (SKNavMesh::instanceExists() && !SKNavMesh::instance().exportStatus().empty())
        {
            getChild<LLUICtrl>("export_status")->setValue(SKNavMesh::instance().exportStatus());
        }
    }
    LLFloater::draw();
}
