/**
 * @file skmapobjects.h
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

#ifndef SK_MAPOBJECTS_H
#define SK_MAPOBJECTS_H

#include "llpointer.h"
#include "v3dmath.h"

#include <functional>

class LLColor4U;
class LLImageRaw;
class LLViewerTexture;

namespace skmap
{
    class MapView;
}

/// Rasterised at 10 Hz into an image centred on the map, up to 512 px; texels per metre follow
/// from the image's actual size.
class SKMapObjects
{
public:
    SKMapObjects();
    ~SKMapObjects();

    /// Redraw the image when due, sized for the view.
    void update(const skmap::MapView& view, bool combat);
    /// Draw it, over every cell for which built() is false, or everywhere when built is null.
    void draw(const skmap::MapView& view, const std::function<bool(S32, S32)>& built) const;
    void release();

private:
    void resize(const skmap::MapView& view);
    void point(const LLVector3d& pos, const LLColor4U& colour, F32 radius_m);

    LLPointer<LLImageRaw> mRaw;
    LLPointer<LLViewerTexture> mImage;
    LLVector3d mCentre;             ///< global, at the last raster
    F32 mTexelsPerMetre = 0.f;
    F64 mLastUpdate = -1.0e9;
    bool mLastCombat = false;       ///< the combat flag of the last raster
    S32 mSize = 0;
};

#endif // SK_MAPOBJECTS_H
