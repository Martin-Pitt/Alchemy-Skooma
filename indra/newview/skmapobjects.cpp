/**
 * @file skmapobjects.cpp
 * @brief The mini-map's object dots, the classic map's object layer, where no floorplan exists.
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

#include "skmapobjects.h"

#include "llimage.h"
#include "llrender.h"
#include "lluicolortable.h"

#include "llagent.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "skmapview.h"

#include <cstring>

namespace
{
    constexpr F64 UPDATE_S = 0.1;
    constexpr S32 MIN_SIZE = 64;
    constexpr S32 MAX_SIZE = 512;
    constexpr F64 CELL_M = 32.0;
}

SKMapObjects::SKMapObjects()
{
}

SKMapObjects::~SKMapObjects()
{
}

void SKMapObjects::release()
{
    mImage = nullptr;
    mRaw = nullptr;
    mSize = 0;
}

void SKMapObjects::resize(const skmap::MapView& view)
{
    // The least power of two at or over the widget's diagonal, as the classic map sizes it.
    const F32 diagonal = std::sqrt(view.width() * view.width() + view.height() * view.height());
    S32 size = MIN_SIZE;
    while (size * 2 < diagonal && size < MAX_SIZE)
    {
        size <<= 1;
    }
    if (size != mSize || mRaw.isNull())
    {
        mSize = size;
        mRaw = new LLImageRaw(size, size, 4);
        std::memset(mRaw->getData(), 0, static_cast<size_t>(size) * size * 4);
        mImage = LLViewerTextureManager::getLocalTexture(mRaw.get(), false);
        mLastUpdate = -1.0e9;
    }
}

void SKMapObjects::point(const LLVector3d& pos, const LLColor4U& colour, F32 radius_m)
{
    const S32 diameter = ll_round(2.f * radius_m * mTexelsPerMetre);
    if (diameter <= 0)
    {
        return;
    }
    const S32 cx = ll_round(static_cast<F32>(pos.mdV[VX] - mCentre.mdV[VX]) * mTexelsPerMetre + mSize / 2);
    const S32 cy = ll_round(static_cast<F32>(pos.mdV[VY] - mCentre.mdV[VY]) * mTexelsPerMetre + mSize / 2);
    if (cx < 0 || cx >= mSize || cy < 0 || cy >= mSize)
    {
        return;
    }
    U8* data = mRaw->getData();
    const U32 rgba = colour.asRGBA();
    const S32 neg = diameter / 2, pos_r = diameter - neg;
    for (S32 y = cy - neg; y < cy + pos_r; ++y)
    {
        if (y < 0 || y >= mSize)
        {
            continue;
        }
        for (S32 x = cx - neg; x < cx + pos_r; ++x)
        {
            if (x >= 0 && x < mSize)
            {
                std::memcpy(data + (static_cast<size_t>(y) * mSize + x) * 4, &rgba, sizeof(U32));
            }
        }
    }
}

void SKMapObjects::update(const skmap::MapView& view, bool combat)
{
    resize(view);
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    // A combat change redraws at once, so physical objects never linger into combat.
    if (now - mLastUpdate < UPDATE_S && combat == mLastCombat)
    {
        return;
    }
    mLastUpdate = now;
    mLastCombat = combat;
    mCentre = view.centre();
    // The image covers the widget's diagonal at the current scale.
    const F32 diagonal = std::sqrt(view.width() * view.width() + view.height() * view.height());
    mTexelsPerMetre = mSize / llmax(diagonal * view.mpp(), 1.f);

    // The classic map's colours and thresholds (LLViewerObjectList::renderObjectsForMap).
    static const LLUIColor above_water_color = LLUIColorTable::instance().getColor("NetMapOtherOwnAboveWater");
    static const LLUIColor below_water_color = LLUIColorTable::instance().getColor("NetMapOtherOwnBelowWater");
    static const LLUIColor you_own_above_water_color = LLUIColorTable::instance().getColor("NetMapYouOwnAboveWater");
    static const LLUIColor you_own_below_water_color = LLUIColorTable::instance().getColor("NetMapYouOwnBelowWater");
    static const LLUIColor group_own_above_water_color = LLUIColorTable::instance().getColor("NetMapGroupOwnAboveWater");
    static const LLUIColor group_own_below_water_color = LLUIColorTable::instance().getColor("NetMapGroupOwnBelowWater");
    static const LLUIColor you_own_physical_color = LLUIColorTable::instance().getColor("NetMapYouPhysical", LLColor4::red);
    static const LLUIColor group_own_physical_color = LLUIColorTable::instance().getColor("NetMapGroupPhysical", LLColor4::green);
    static const LLUIColor other_own_physical_color = LLUIColorTable::instance().getColor("NetMapOtherPhysical", LLColor4::green);
    static const LLUIColor scripted_object_color = LLUIColorTable::instance().getColor("NetMapScripted", LLColor4::orange);
    static const LLUIColor temp_on_rez_object_color = LLUIColorTable::instance().getColor("NetMapTempOnRez", LLColor4::orange);
    static LLCachedControl<F32> max_radius(gSavedSettings, "MiniMapPrimMaxRadius", 16.f);
    static LLCachedControl<F32> max_zdistance(gSavedSettings, "MiniMapPrimMaxVertDistance", 256.f);
    static LLCachedControl<bool> show_scripted(gSavedSettings, "MiniMapPrimScripted", false);
    static LLCachedControl<bool> show_physical(gSavedSettings, "MiniMapPrimPhysical", true);
    static LLCachedControl<bool> show_temp(gSavedSettings, "MiniMapPrimTempOnRez", false);
    static LLCachedControl<U32> phantom_opacity(gSavedSettings, "MiniMapPrimPhantomOpacity", 100U);
    constexpr F32 MIN_RADIUS_FOR_ACCENTED_OBJECTS = 2.f;
    constexpr F32 MIN_RADIUS_FOR_OWNED_OBJECTS = 2.f;

    LLImageDataLock lock(mRaw);
    std::memset(mRaw->getData(), 0, static_cast<size_t>(mSize) * mSize * 4);
    const F64 agent_z = gAgent.getPositionGlobal().mdV[VZ];
    for (const LLPointer<LLViewerObject>& ptr : gObjectList.getMapObjects())
    {
        LLViewerObject* objectp = ptr.get();
        if (!objectp || objectp->isDead() || !objectp->getRegion() || objectp->isOrphaned() || objectp->isAttachment())
        {
            continue;
        }
        // In combat a moving physical object could be someone's vehicle or shot: leave it out.
        if (combat && objectp->flagUsePhysics())
        {
            continue;
        }
        const LLVector3& scale = objectp->getScale();
        const LLVector3d pos = objectp->getPositionGlobal();
        const F64 water_height = objectp->getRegion()->getWaterHeight();
        if (max_zdistance > 0.f && std::fabs(pos.mdV[VZ] - agent_z) > max_zdistance)
        {
            continue;
        }
        F32 radius = llmin((scale.mV[VX] + scale.mV[VY]) * 0.5f * 0.5f * 1.3f, static_cast<F32>(max_radius));
        LLColor4U colour = above_water_color.get();
        if (objectp->permYouOwner())
        {
            radius = llmax(radius, MIN_RADIUS_FOR_OWNED_OBJECTS);
            if (pos.mdV[VZ] >= water_height)
            {
                colour = objectp->permGroupOwner() ? group_own_above_water_color.get() : you_own_above_water_color.get();
            }
            else
            {
                colour = objectp->permGroupOwner() ? group_own_below_water_color.get() : you_own_below_water_color.get();
            }
        }
        else if (pos.mdV[VZ] < water_height)
        {
            colour = below_water_color.get();
        }
        if (show_scripted && objectp->flagScripted())
        {
            colour = scripted_object_color.get();
            radius = llmax(radius, MIN_RADIUS_FOR_ACCENTED_OBJECTS);
        }
        if (show_physical && objectp->flagUsePhysics())
        {
            colour = objectp->permYouOwner() ? you_own_physical_color.get()
                   : objectp->permGroupOwner() ? group_own_physical_color.get()
                                               : other_own_physical_color.get();
            radius = llmax(radius, MIN_RADIUS_FOR_ACCENTED_OBJECTS);
        }
        if (show_temp && objectp->flagTemporaryOnRez())
        {
            colour = temp_on_rez_object_color.get();
            radius = llmax(radius, MIN_RADIUS_FOR_ACCENTED_OBJECTS);
        }
        if (objectp->flagPhantom())
        {
            colour.setAlpha(llclampb(phantom_opacity()));
        }
        point(pos, colour, radius);
    }
    mImage->setSubImage(mRaw, 0, 0, mSize, mSize);
}

void SKMapObjects::draw(const skmap::MapView& view, const std::function<bool(S32, S32)>& built) const
{
    if (mImage.isNull() || mTexelsPerMetre <= 0.f)
    {
        return;
    }
    const F64 half = mSize * 0.5 / mTexelsPerMetre;
    const LLVector3d lo = mCentre - LLVector3d(half, half, 0.0);
    const LLVector3d hi = mCentre + LLVector3d(half, half, 0.0);
    auto quad = [&](F64 x0, F64 y0, F64 x1, F64 y1) {
        const F32 u0 = static_cast<F32>((x0 - lo.mdV[VX]) / (2.0 * half)), u1 = static_cast<F32>((x1 - lo.mdV[VX]) / (2.0 * half));
        const F32 v0 = static_cast<F32>((y0 - lo.mdV[VY]) / (2.0 * half)), v1 = static_cast<F32>((y1 - lo.mdV[VY]) / (2.0 * half));
        const LLVector2 a = view.toWidget(LLVector3d(x0, y0, 0.0)), b = view.toWidget(LLVector3d(x1, y0, 0.0));
        const LLVector2 c = view.toWidget(LLVector3d(x1, y1, 0.0)), d = view.toWidget(LLVector3d(x0, y1, 0.0));
        gGL.texCoord2f(u0, v0);
        gGL.vertex2f(a.mV[VX], a.mV[VY]);
        gGL.texCoord2f(u1, v0);
        gGL.vertex2f(b.mV[VX], b.mV[VY]);
        gGL.texCoord2f(u1, v1);
        gGL.vertex2f(c.mV[VX], c.mV[VY]);
        gGL.texCoord2f(u0, v0);
        gGL.vertex2f(a.mV[VX], a.mV[VY]);
        gGL.texCoord2f(u1, v1);
        gGL.vertex2f(c.mV[VX], c.mV[VY]);
        gGL.texCoord2f(u0, v1);
        gGL.vertex2f(d.mV[VX], d.mV[VY]);
    };
    gGL.getTextureSlot(0)->bindSampled(mImage, ALSamplers::PointClamp);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.begin(LLRender::TRIANGLES);
    if (!built)
    {
        quad(lo.mdV[VX], lo.mdV[VY], hi.mdV[VX], hi.mdV[VY]);
    }
    else
    {
        // Only the 32 m cells the navmesh has not built: the floorplan stands for the rest.
        const S32 cx0 = static_cast<S32>(std::floor(lo.mdV[VX] / CELL_M)), cx1 = static_cast<S32>(std::floor(hi.mdV[VX] / CELL_M));
        const S32 cy0 = static_cast<S32>(std::floor(lo.mdV[VY] / CELL_M)), cy1 = static_cast<S32>(std::floor(hi.mdV[VY] / CELL_M));
        for (S32 cy = cy0; cy <= cy1; ++cy)
        {
            for (S32 cx = cx0; cx <= cx1; ++cx)
            {
                if (built(cx, cy))
                {
                    continue;
                }
                quad(llmax(lo.mdV[VX], cx * CELL_M), llmax(lo.mdV[VY], cy * CELL_M), llmin(hi.mdV[VX], (cx + 1) * CELL_M),
                     llmin(hi.mdV[VY], (cy + 1) * CELL_M));
            }
        }
    }
    gGL.end();
    gGL.getTextureSlot(0)->unbind();
}
