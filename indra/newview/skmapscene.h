/**
 * @file skmapscene.h
 * @brief The mini-map's hold on the navmesh: its demand while a map draws, and which tiles it has built.
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

#ifndef SK_MAPSCENE_H
#define SK_MAPSCENE_H

#include "llsingleton.h"
#include "v3dmath.h"

#include <map>
#include <utility>

class SKNetMap;

/// Main thread only. Costs nothing while no map has drawn for a couple of seconds.
class SKMapScene : public LLSingleton<SKMapScene>
{
    LLSINGLETON(SKMapScene);
    ~SKMapScene() override;

public:
    void initSingleton() override;
    void cleanupSingleton() override;

    /// A map drew this frame: hold the navmesh demand, and follow its centre and shape.
    void noteDrawn(const SKNetMap& map);
    bool active() const;

    /// Whether a navmesh tile (a 32 m lattice cell) is published, or was within the last few seconds.
    bool built(S32 x, S32 y) const { return mBuilt.count({ x, y }) > 0; }
    U32 builtTiles() const { return static_cast<U32>(mBuilt.size()); }

    static void onIdle(void*);

private:
    void idle();
    void updateDemand(bool want);
    void updateBuilt();

    bool mDemanding = false;
    F64 mLastDrawn = -1.0e9;
    LLVector3d mMapCentre;
    F32 mMapLong = 200.f;
    F32 mMapShort = 200.f;

    /// Each built tile's lattice cell and when it was last seen published, carried while the navmesh rebuilds it.
    std::map<std::pair<S32, S32>, F64> mBuilt;
    U64 mBuiltRevision = 0;
    LLVector3d mBuiltMin, mBuiltMax;
    F64 mBuiltAt = -1.0e9;
};

#endif // SK_MAPSCENE_H
