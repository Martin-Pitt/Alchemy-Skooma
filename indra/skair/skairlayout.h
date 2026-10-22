/**
 * @file skairlayout.h
 * @brief What a mesh tile of air is built from, and the key its air gives the sky map.
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

#ifndef SK_AIRLAYOUT_H
#define SK_AIRLAYOUT_H

#include "skairraster.h"
#include "skcensuslattice.h"

#include <functional>
#include <vector>

namespace skworld
{
    /// How far past a tile geometry and terrain are taken: AVATAR's border
    /// or the air's pad, whichever is wider, plus a metre of terrain grid.
    F32 tilePad(const TileInput& params);

    /// The key a mesh tile's air gives the sky map under.
    U64 skyContributor(const LLUUID& mesh, const TileKey& key);

    /// Split a tile's records into shapes and cuts, and set the z range the
    /// tile rasterises from them and the terrain already in the input.
    void finishTileInput(const std::vector<RecordPtr>& records, const LLVector3d& mesh_origin, TileInput& input);

    /// The tile's linkset roots that cue_of gives a NameCue, into mRootCues.
    void tileRootCues(const std::function<U8(const LLUUID&)>& cue_of, TileInput& input);
}

#endif // SK_AIRLAYOUT_H
