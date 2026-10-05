/**
 * @file skmapparcels.cpp
 * @brief The mini-map's parcel lines and for-sale and collision fills, as vertex arrays.
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

#include "skmapparcels.h"

#include "llparcel.h"
#include "llrender.h"
#include "lluicolortable.h"

#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "sknetmap.h"
#include "skmapview.h"

SKMapParcels::SKMapParcels()
{
    mOverlayConnection = LLViewerParcelOverlay::setUpdateCallback([this](const LLViewerRegion*) { mDirty = true; });
    mCollisionConnection = LLViewerParcelMgr::instance().setCollisionUpdateCallback([this](const LLViewerRegion*) { mDirty = true; });
}

SKMapParcels::~SKMapParcels()
{
    mOverlayConnection.disconnect();
    mCollisionConnection.disconnect();
}

void SKMapParcels::rebuild()
{
    // The classic map's rules (LLNetMap::renderPropertyLinesForRegion), as world-space geometry.
    static const LLUIColor outline = LLUIColorTable::instance().getColor("MapParcelOutlineColor", LLColor4::white);
    static LLCachedControl<bool> show_sale(gSavedSettings, "SKMapLayerParcelsForSale", false);
    static LLCachedControl<bool> show_collision(gSavedSettings, "SKMapLayerParcelsCollision", false);
    mLines.clear();
    mFills.clear();
    mLastSale = show_sale;
    mLastCollision = show_collision;
    const LLColor4U dead(255, 128, 128, 255);
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const LLColor4U line_colour = region->isAlive() ? LLColor4U(outline.get()) : dead;
        const LLVector3d origin = region->getOriginGlobal();
        const F64 width = region->getWidth();
        // The north and east borders.
        mLines.push_back({ origin + LLVector3d(0.0, width, 0.0), origin + LLVector3d(width, width, 0.0), line_colour });
        mLines.push_back({ origin + LLVector3d(width, 0.0, 0.0), origin + LLVector3d(width, width, 0.0), line_colour });

        const LLViewerParcelOverlay* overlay = region->getParcelOverlay();
        if (!overlay)
        {
            continue;
        }
        const F64 step = PARCEL_GRID_STEP_METERS;
        const S32 per_edge = static_cast<S32>(width / step);
        const U8* ownership = overlay->getOwnership();
        const U8* collision = region->getHandle() == LLViewerParcelMgr::instance().getCollisionRegionHandle()
                                ? LLViewerParcelMgr::instance().getCollisionBitmap()
                                : nullptr;
        for (S32 row = 0; row < per_edge; ++row)
        {
            for (S32 col = 0; col < per_edge; ++col)
            {
                const S32 idx = row * per_edge + col;
                const S32 cell = ownership[idx];
                const bool for_sale = (cell & PARCEL_COLOR_MASK) == PARCEL_FOR_SALE;
                const bool auction = (cell & PARCEL_COLOR_MASK) == PARCEL_AUCTION;
                const bool collides = collision && (collision[idx / 8] & (1 << (idx % 8)));
                const LLVector3d sw = origin + LLVector3d(col * step, row * step, 0.0);
                if ((show_sale && (for_sale || auction)) || (show_collision && collides))
                {
                    const LLColor4U fill = for_sale ? LLColor4U(255, 255, 128, 192) : auction ? LLColor4U(128, 0, 255, 102) : LLColor4U(255, 128, 128, 192);
                    mFills.push_back({ sw, sw + LLVector3d(step, step, 0.0), fill });
                }
                if (cell & PARCEL_SOUTH_LINE)
                {
                    mLines.push_back({ sw, sw + LLVector3d(step, 0.0, 0.0), line_colour });
                }
                if (cell & PARCEL_WEST_LINE)
                {
                    mLines.push_back({ sw, sw + LLVector3d(0.0, step, 0.0), line_colour });
                }
            }
        }
    }
    mDirty = false;
}

void SKMapParcels::draw(const skmap::MapView& view)
{
    static LLCachedControl<bool> show_lines(gSavedSettings, "SKMapLayerParcels", false);
    static LLCachedControl<bool> show_sale(gSavedSettings, "SKMapLayerParcelsForSale", false);
    static LLCachedControl<bool> show_collision(gSavedSettings, "SKMapLayerParcelsCollision", false);
    if (!show_lines && !show_sale && !show_collision)
    {
        return;
    }
    if (mDirty || mLastSale != show_sale || mLastCollision != show_collision)
    {
        rebuild();
    }
    LLVector3d lo, hi;
    view.visibleBox(lo, hi);
    gGL.begin(LLRender::TRIANGLES);
    for (const Fill& f : mFills)
    {
        if (f.mMax.mdV[VX] < lo.mdV[VX] || f.mMin.mdV[VX] > hi.mdV[VX] || f.mMax.mdV[VY] < lo.mdV[VY] || f.mMin.mdV[VY] > hi.mdV[VY])
        {
            continue;
        }
        const LLVector2 a = view.toWidget(f.mMin), b = view.toWidget(LLVector3d(f.mMax.mdV[VX], f.mMin.mdV[VY], 0.0));
        const LLVector2 c = view.toWidget(f.mMax), d = view.toWidget(LLVector3d(f.mMin.mdV[VX], f.mMax.mdV[VY], 0.0));
        gGL.color4ubv(f.mColour.mV);
        skmapdraw::triangle(a, b, c);
        skmapdraw::triangle(a, c, d);
    }
    if (show_lines)
    {
        for (const Line& l : mLines)
        {
            if (llmax(l.mA.mdV[VX], l.mB.mdV[VX]) < lo.mdV[VX] || llmin(l.mA.mdV[VX], l.mB.mdV[VX]) > hi.mdV[VX]
                || llmax(l.mA.mdV[VY], l.mB.mdV[VY]) < lo.mdV[VY] || llmin(l.mA.mdV[VY], l.mB.mdV[VY]) > hi.mdV[VY])
            {
                continue;
            }
            gGL.color4ubv(l.mColour.mV);
            skmapdraw::line(view.toWidget(l.mA), view.toWidget(l.mB), 1.f);
        }
    }
    gGL.end();
}
