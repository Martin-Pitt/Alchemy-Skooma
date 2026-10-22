/**
 * @file skfloatertacticaloptions.h
 * @brief The tactical overview's options: its camera mode and view settings, bound to their settings.
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

#ifndef SK_FLOATERTACTICALOPTIONS_H
#define SK_FLOATERTACTICALOPTIONS_H

#include "llfloater.h"

class SKFloaterTacticalOptions : public LLFloater
{
public:
    AL_VIEW_TYPE(SKFloaterTacticalOptions, LLFloater);

    SKFloaterTacticalOptions(const LLSD& key);
    ~SKFloaterTacticalOptions() override = default;

    bool postBuild() override;

private:
    /// Enables the controls that belong to the chosen camera mode.
    void refreshMode();
};

#endif // SK_FLOATERTACTICALOPTIONS_H
