/**
 * @file skfloaterlegenddemo.h
 * @brief Develop floater showing every built-in colour scale and the sample legends.
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

#ifndef SK_FLOATERLEGENDDEMO_H
#define SK_FLOATERLEGENDDEMO_H

#include "llfloater.h"

#include <memory>

class SKColorScale;

class SKFloaterLegendDemo : public LLFloater
{
public:
    SKFloaterLegendDemo(const LLSD& key);
    bool postBuild() override;
    void onClose(bool app_quitting) override;

private:
    std::shared_ptr<SKColorScale> mComplexity;
    bool                          mInHud = false;
};

#endif // SK_FLOATERLEGENDDEMO_H
