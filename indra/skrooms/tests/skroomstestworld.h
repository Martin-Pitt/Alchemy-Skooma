/**
 * @file skroomstestworld.h
 * @brief A census file built offline into air and rooms, as the viewer would: for tests and tools that evaluate on real worlds.
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

#ifndef SK_ROOMSTESTWORLD_H
#define SK_ROOMSTESTWORLD_H

#include "skaircensus.h"
#include "skairlayout.h"
#include "skairraster.h"
#include "skairskymap.h"
#include "skcensusio.h"
#include "../skroomsenclosure.h"
#include "../skroomsspaces.h"

#include <map>
#include <memory>
#include <vector>

namespace skrooms_test
{
    using namespace skworld;

    /// Which tiles get air of their own.
    enum class AirMode : U8
    {
        RADIUS,         ///< those within the census's air radius of its camera, as the viewer builds them
        EVERYWHERE,     ///< every tile, for experiments over the whole export
    };

    /// One mesh of the world: its origin, the box its air covers, and its passes' products.
    struct WorldMesh
    {
        LLUUID mId;
        LLVector3d mOrigin;
        LLVector3d mAirMin;
        LLVector3d mAirMax;
        std::vector<TileKey> mKeys;
        std::vector<TileAirPtr> mAirs;      ///< with mKeys
        std::shared_ptr<EnclosureProduct> mEnclosure;
        RoomsProductPtr mRooms;
    };

    struct World
    {
        std::shared_ptr<SkyMap> mSky;
        std::vector<std::shared_ptr<WorldMesh>> mMeshes;
        U32 mTiles = 0;                     ///< rasterised
        U32 mRefused = 0;                   ///< meshes refused for their size
    };

    /// A census file read with its air parameters, laid out as the viewer lays it, each tile rasterised with
    /// its air, every tile's tops on one sky map, then each mesh's enclosure and spaces. False when it does not read.
    inline bool buildWorld(const std::string& path, AirMode mode, CensusFile& census, World& out, std::string* error = nullptr)
    {
        AirCensusParams params;
        if (!readCensusFile(path, census, error, { airParamsSection(params) }))
        {
            return false;
        }
        std::vector<CensusTile> tiles;
        out.mRefused = layCensusTiles(census, params, tiles);
        out.mSky = std::make_shared<SkyMap>();
        std::map<LLUUID, std::shared_ptr<WorldMesh>> meshes;
        for (const CensusTile& t : tiles)
        {
            if (mode == AirMode::RADIUS && !t.mInAirRadius)
            {
                continue;
            }
            TileAirResult result;
            if (!rasteriseTile(*t.mInput, result) || !result.mAir)
            {
                continue;
            }
            ++out.mTiles;
            std::shared_ptr<WorldMesh>& m = meshes[t.mMesh];
            if (!m)
            {
                m = std::make_shared<WorldMesh>();
                m->mId = t.mMesh;
                m->mOrigin = t.mInput->mMeshOrigin;
                m->mAirMin = t.mMin;
                m->mAirMax = t.mMax;
            }
            for (S32 k = 0; k < 3; ++k)
            {
                m->mAirMin.mdV[k] = std::min(m->mAirMin.mdV[k], t.mMin.mdV[k]);
                m->mAirMax.mdV[k] = std::max(m->mAirMax.mdV[k], t.mMax.mdV[k]);
            }
            m->mKeys.push_back(t.mKey);
            m->mAirs.push_back(result.mAir);
            out.mSky->set(skyContributor(t.mMesh, t.mKey), m->mOrigin + LLVector3d(result.mAir->mOrigin), *result.mAir);
        }
        for (auto& [id, m] : meshes)
        {
            auto enc = std::make_shared<EnclosureInput>();
            enc->mGeneration = 1;
            enc->mMeshOrigin = m->mOrigin;
            enc->mSky = out.mSky;
            for (size_t i = 0; i < m->mKeys.size(); ++i)
            {
                enc->mTiles.push_back({ m->mKeys[i], m->mAirs[i], skyContributor(id, m->mKeys[i]) });
            }
            std::sort(enc->mTiles.begin(), enc->mTiles.end(), [](const EnclosureInput::Tile& a, const EnclosureInput::Tile& b) { return a.mKey < b.mKey; });
            EnclosurePass pass(enc);
            while (!pass.step(1u << 30))
            {
            }
            m->mEnclosure = pass.takeProduct();
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mGeneration = 1;
            rooms->mMeshOrigin = m->mOrigin;
            rooms->mMeshKey = mix(SEED, std::hash<LLUUID>()(id));
            for (const DoorEntity& d : census.mDoors)
            {
                rooms->mDoors.push_back(d);
            }
            m->mRooms = computeSpaces(rooms, m->mEnclosure);
            out.mMeshes.push_back(m);
        }
        return true;
    }
}

#endif // SK_ROOMSTESTWORLD_H
