/**
 * @file skfloaternavmesh.cpp
 * @brief The navmesh console: what the census and the mesh are doing, the overlay's switches, and its legend.
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

#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "sknavmesh.h"
#include "sknavpalette.h"
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
    buildLegend();
    return true;
}

void SKFloaterNavMesh::buildLegend()
{
    LLScrollListCtrl* legend = getChild<LLScrollListCtrl>("legend");
    legend->deleteAllItems();
    for (S32 i = 0; i < static_cast<S32>(SKNavSwatch::COUNT); ++i)
    {
        const SKNavSwatch swatch = static_cast<SKNavSwatch>(i);
        LLSD row;
        row["columns"][0]["column"] = "what";
        row["columns"][0]["type"] = "text";
        row["columns"][0]["value"] = getString(skNavSwatchName(swatch));
        if (LLScrollListItem* item = legend->addElement(row))
        {
            // The row wears the colour it describes: reading the legend and
            // reading the overlay are then the same act.
            LLColor4 color = skNavSwatchColor(swatch);
            color.mV[VALPHA] = 1.f;
            item->getColumn(0)->setColor(color);
        }
    }
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
            // Asking for a class nothing is building shows an empty world and
            // no reason for it.
            const bool weather = gSavedSettings.getU32("SKNavOverlayClass") == 1;
            mesh_line = (weather && !gSavedSettings.getBOOL("SKNavBuildWeather")) ? getString("weather_off")
                                                                                  : SKNavMesh::instance().statsLine();
        }
        getChild<LLUICtrl>("mesh_status")->setValue(mesh_line);
        getChild<LLUICtrl>("census_status")->setValue(SKWorldCensus::instance().statsLine());
    }
    LLFloater::draw();
}
