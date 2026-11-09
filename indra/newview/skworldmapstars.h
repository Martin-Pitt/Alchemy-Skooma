/**
 * @file skworldmapstars.h
 * @brief The stars past the 3D world map's edge: the sky's own star field, fixed in the map's frame.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_WORLDMAPSTARS_H
#define SK_WORLDMAPSTARS_H

#include "llpointer.h"

class LLVertexBuffer;

/// ALStarField's stars at RenderStarCount, the field the sky draws, as points at infinity in the map's frame: they
/// stay put as the map pans and turn only with its heading and pitch. The field covers the upper hemisphere, the sky's,
/// and the map looks down into space past the rim, so each star is drawn mirrored below as well.
class SKWorldMapStars
{
public:
    SKWorldMapStars();
    ~SKWorldMapStars();

    /// Draws them under the current projection and a view with the eye at the origin, depth untouched, before the
    /// ground goes over them.
    void draw();

    void release();

private:
    bool build(U32 count);

    LLPointer<LLVertexBuffer> mBuffer;
    U32 mCount = 0;         ///< the RenderStarCount the buffer was built for
    U32 mVertices = 0;
};

#endif // SK_WORLDMAPSTARS_H
