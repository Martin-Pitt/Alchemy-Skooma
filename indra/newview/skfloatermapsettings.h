/**
 * @file skfloatermapsettings.h
 * @brief The mini-map's settings: every SKMap switch, bound to its setting.
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

#ifndef SK_FLOATERMAPSETTINGS_H
#define SK_FLOATERMAPSETTINGS_H

#include "llfloater.h"

class SKFloaterMapSettings : public LLFloater
{
public:
    AL_VIEW_TYPE(SKFloaterMapSettings, LLFloater);

    SKFloaterMapSettings(const LLSD& key);
    ~SKFloaterMapSettings() override = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    /// Greys out what the view mode leaves unused.
    void updateModeControls();
    /// Beside the map: to its left if there is room, else to its right.
    void placeBesideMap();

    bool mHadSavedRect = false;
    bool mPlaced = false;
    boost::signals2::scoped_connection mModeConnection;
};

#endif // SK_FLOATERMAPSETTINGS_H
