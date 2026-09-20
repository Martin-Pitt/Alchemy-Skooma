/**
 * @file skfloaternavmesh.h
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

#ifndef SK_FLOATERNAVMESH_H
#define SK_FLOATERNAVMESH_H

#include "llfloater.h"
#include "llframetimer.h"

/**
 * The navmesh's console, after the stock pathfinding view/test floater.
 *
 * The overlay draws a dozen colours and, on its own, explains none of them.
 * The legend here is built from the same table the overlay draws from
 * (@ref sknavpalette.h), so a colour cannot be changed in one place and
 * described wrongly in the other.
 *
 * Every switch but the first is an ordinary setting bound in XUI. The first
 * one is not a setting at all: the overlay hangs off the pipeline's debug mask
 * (@c LLPipeline::RENDER_DEBUG_NAVMESH), the same bit the Develop menu's
 * Navmesh entry toggles, so it is driven from code and read back each frame in
 * case the menu changed it.
 */
class SKFloaterNavMesh final : public LLFloater
{
public:
    SKFloaterNavMesh(const LLSD& key);
    ~SKFloaterNavMesh() override = default;

    bool postBuild() override;
    void draw() override;

private:
    void buildLegend();
    void onToggleOverlay();

    LLFrameTimer mRefresh;
};

#endif // SK_FLOATERNAVMESH_H
