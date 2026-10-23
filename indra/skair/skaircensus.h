/**
 * @file skaircensus.h
 * @brief The air's section of a census file, and a census's tiles laid out as the viewer lays them, offline.
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

#ifndef SK_AIRCENSUS_H
#define SK_AIRCENSUS_H

#include "skairraster.h"
#include "skcensusio.h"

#include <iosfwd>
#include <memory>
#include <vector>

class LLSD;

namespace skworld
{
    /// What the air was built with when a census was written: every tile's
    /// parameters, and the air's own settings.
    struct AirCensusParams
    {
        TileInput mParams;                  ///< records, cuts, terrain and tile empty
        F32 mAirRadius = 128.f;             ///< tiles further from the camera build no air of their own
        F32 mGroundedMargin = 5.f;          ///< a cluster this close over its land is meshed with its region
    };

    /// The "air.params" section, written from and read into params. A file
    /// older than version 7 hands over its "params" object, which held the
    /// tile parameters alone: the air's settings then keep their defaults.
    CensusSection airParamsSection(AirCensusParams& params);
    void writeAirParams(std::ostream& out, const AirCensusParams& params);
    bool readAirParams(const LLSD& value, U32 version, AirCensusParams& params);

    /// One tile of a mesh, with what it rasterises.
    struct CensusTile
    {
        LLUUID mMesh;                       ///< the cluster's, or the region's for its own mesh
        bool mTerrain = false;              ///< a region's mesh
        TileKey mKey;                       ///< within the mesh
        LLVector3d mMin;                    ///< global, the box the viewer gives the tile
        LLVector3d mMax;
        bool mOverLand = true;
        bool mInAirRadius = true;           ///< within the air radius of the census's camera
        std::shared_ptr<TileInput> mInput;
    };

    /// Every mesh tile of a census, laid out as the viewer lays them: a mesh per
    /// airborne cluster, one per region with its grounded records, the terrain
    /// sampled from the census's land. Every tile is given air (mAir) and the
    /// census's own parameters otherwise; mInAirRadius says which the viewer
    /// would have built air for. Returns the meshes refused for their size.
    U32 layCensusTiles(const CensusFile& census, const AirCensusParams& params, std::vector<CensusTile>& out);
}

#endif // SK_AIRCENSUS_H
