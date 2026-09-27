/**
 * @file sknavexport.h
 * @brief The navmesh export's JSON: one writer for the viewer and the headless replay, so their files cannot drift.
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

#ifndef SK_NAVEXPORT_H
#define SK_NAVEXPORT_H

#include "sknavbuild.h"
#include "sknavenclosure.h"
#include "sknavfeatures.h"
#include "sknavspaces.h"
#include "sknavwind.h"

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

class dtNavMesh;

namespace sknav
{
    struct ExportCounts
    {
        U32 mTiles = 0;
        U32 mPolys = 0;
        U32 mSegments = 0;
        U32 mLinks = 0;
        U32 mFaces = 0;
        U32 mAirs = 0;
        U32 mFeatures = 0;
        U32 mDoors = 0;
    };

    /// What one tile has published, by reference: anything null is left out.
    struct TileExport
    {
        TileKey mKey;
        LLVector3d mMin;                ///< global
        LLVector3d mMax;
        const std::vector<WallFace>* mFaces = nullptr;
        TileAirPtr mAir;
        const TileEnclosure* mEnclosure = nullptr;     ///< the answers for exactly this air
        U64 mEnclosureGeneration = 0;
        const dtNavMesh* mNav[NUM_CLASSES] = { nullptr, nullptr };
        const std::vector<WallSegment>* mSegments[NUM_CLASSES] = { nullptr, nullptr };
        const std::vector<DropLink>* mLinks[NUM_CLASSES] = { nullptr, nullptr };
        const std::vector<Flight>* mFlights[NUM_CLASSES] = { nullptr, nullptr };
    };

    /// The Recast area a Detour poly's flags stand for.
    U8 polyArea(unsigned short flags);

    /// The file's opening members, up to and including the meshes array's bracket.
    void writeExportHeader(std::ostream& file, const std::string& exported, const LLVector3d& agent, F32 height, F32 radius, F32 climb, F32 slope,
                           F32 cell);
    /// One tile as a JSON object.
    void writeExportTile(std::ostream& file, const LLVector3d& mesh_origin, const TileExport& tile, ExportCounts& counts);
    /// A tile's per-gap answers as a JSON member, leading comma included.
    void writeExportEnclosure(std::ostream& file, U64 generation, const TileEnclosure& tile);
    /// A mesh's spaces, portals and wind slabs as JSON members, leading comma included, global.
    void writeExportSpaces(std::ostream& file, const LLVector3d& origin, const RoomsProduct& rooms, const WindProduct* wind);
    /// One door as a JSON object; names are escaped.
    void writeExportDoor(std::ostream& file, const DoorEntity& door);
    /// One feature as a JSON object, with its span's exits and links per class when it has one.
    void writeExportFeature(std::ostream& file, const Feature& feature, LinkKind span_kind, const std::vector<LLVector3d>* exits,
                            const std::vector<std::pair<LLVector3d, LLVector3d>>* links);
}

#endif // SK_NAVEXPORT_H
