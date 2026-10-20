/**
 * @file sknavprofile.h
 * @brief Tracy zones for the navmesh passes, compiled out unless the build profiles with Tracy.
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

#ifndef SK_NAVPROFILE_H
#define SK_NAVPROFILE_H

#include "llprofiler.h"

// LL_PROFILE_ZONE_NAMED stringifies its name under the Tracy + fast timer configuration and declares one fixed
// variable, so two in a scope collide; these name the zone as given and number the variable by line.
#if defined(TRACY_ENABLE) && LL_PROFILER_CONFIGURATION >= LL_PROFILER_CONFIG_TRACY
#define SK_NAV_ZONE_CAT2(a, b) a##b
#define SK_NAV_ZONE_CAT(a, b) SK_NAV_ZONE_CAT2(a, b)
#define SK_NAV_ZONE(name) ZoneNamedN(SK_NAV_ZONE_CAT(sk_nav_zone_, __LINE__), name, true)
#else
#define SK_NAV_ZONE(name)
#endif

#endif // SK_NAVPROFILE_H
