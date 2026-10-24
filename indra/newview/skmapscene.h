/**
 * @file skmapscene.h
 * @brief The mini-map's hold on the census and air cells: their demand while a map draws.
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

class SKNetMap;

/// Main thread only. Costs nothing while no map has drawn for a couple of seconds.
class SKMapScene : public LLSingleton<SKMapScene>
{
    LLSINGLETON(SKMapScene);
    ~SKMapScene() override;

public:
    void initSingleton() override;
    void cleanupSingleton() override;

    /// A map drew this frame: hold the census and air demand.
    void noteDrawn(const SKNetMap& map);
    bool active() const;

    static void onIdle(void*);

private:
    void idle();
    void updateDemand(bool want);

    bool mDemanding = false;
    F64 mLastDrawn = -1.0e9;
};

#endif // SK_MAPSCENE_H
