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

#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llagent.h"
#include "llpanel.h"
#include "llscrolllistctrl.h"
#include "llviewercamera.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "skcaptureexport.h"
#include "skoverviewcapture.h"
#include "sknavmesh.h"
#include "skworldcensus.h"

#include <algorithm>
#include <set>

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
    getChild<LLUICtrl>("cluster_list")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        const LLScrollListItem* item = getChild<LLScrollListCtrl>("cluster_list")->getFirstSelected();
        SKWorldCensus::instance().selectCluster(item ? item->getValue().asUUID() : LLUUID::null);
    });
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
        // The stamped polygons exist only with polys on; a stored choice of them is left alone and draws nothing.
        getChild<LLComboBox>("overlay_air")->setEnabledByValue(LLSD(4), SKNavMesh::polysEnabled());
        getChild<LLUICtrl>("census_status")->setValue(SKWorldCensus::instance().statsLine());
        if (SKNavMesh::instanceExists() && !SKNavMesh::instance().exportStatus().empty())
        {
            getChild<LLUICtrl>("export_status")->setValue(SKNavMesh::instance().exportStatus());
        }
    }
    if (mClustersRefresh.getElapsedTimeF32() > 1.f && getChild<LLPanel>("nav_tab_clusters")->isInVisibleChain())
    {
        mClustersRefresh.reset();
        refreshClusters();
    }
    LLFloater::draw();
}

void SKFloaterNavMesh::refreshClusters()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("cluster_list");
    const SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        list->deleteAllItems();
        return;
    }
    // Within twice the overlay's range, nearest first, a few hundred at most.
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 reach = 2.0 * gSavedSettings.getF32("SKNavOverlayRange");
    std::vector<std::pair<F64, const sknav::Cluster*>> nearby;
    for (const sknav::ClusterPtr& c : snap->mClusters.mClusters)
    {
        const F64 d2 = dist_vec_squared2D((c->mMin + c->mMax) * 0.5, camera);
        if (d2 <= reach * reach)
        {
            nearby.emplace_back(d2, c.get());
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (nearby.size() > 300)
    {
        nearby.resize(300);
    }
    const LLUUID selected = SKWorldCensus::instance().selectedCluster();
    const S32 scroll = list->getScrollPos();
    list->deleteAllItems();
    for (const auto& entry : nearby)
    {
        const sknav::Cluster& c = *entry.second;
        std::set<LLUUID> roots;
        for (const sknav::RecordPtr& r : c.mRecords)
        {
            roots.insert(r->mRoot.notNull() ? r->mRoot : r->mPart);
        }
        // How the overview cuts it: the first reason among its linksets, and how many of them it holds for.
        std::string cut = "whole";
        if (SKOverviewCapture::instanceExists())
        {
            size_t count = 0;
            std::string first;
            for (const LLUUID& root : roots)
            {
                F32 height = 0.f;
                std::string why;
                if (SKOverviewCapture::instance().census().cutOf(root, height, why))
                {
                    if (!count++)
                    {
                        first = height > -1.0e29f ? llformat("%s at %.1f", why.c_str(), height) : why;
                    }
                }
            }
            if (count)
            {
                cut = count < roots.size() ? llformat("%s (%zu of %zu)", first.c_str(), count, roots.size()) : first;
            }
        }
        LLSD row;
        row["value"] = c.mId;
        row["columns"][0]["column"] = "id";
        row["columns"][0]["value"] = c.mId.asString().substr(0, 8);
        row["columns"][1]["column"] = "state";
        row["columns"][1]["value"] = SKWorldCensus::grounded(c.mMin, c.mMax) ? "grounded" : "floating";
        row["columns"][2]["column"] = "linksets";
        row["columns"][2]["value"] = (S32)roots.size();
        row["columns"][3]["column"] = "bottom";
        row["columns"][3]["value"] = llformat("%.1f", c.mMin.mdV[VZ]);
        row["columns"][4]["column"] = "top";
        row["columns"][4]["value"] = llformat("%.1f", c.mMax.mdV[VZ]);
        row["columns"][5]["column"] = "cut";
        row["columns"][5]["value"] = cut;
        list->addElement(row);
    }
    if (selected.notNull())
    {
        list->selectByValue(selected);
    }
    list->setScrollPos(scroll);
}
