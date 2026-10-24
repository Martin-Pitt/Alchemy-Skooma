/**
 * @file skscrolldistance.cpp
 * @brief How far a scroll gesture moves a widget's content, and the smooth scroll's settings.
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

#include "linden_common.h"

#include "skscrolldistance.h"

#include "llframetimer.h"
#include "llui.h"

SKScrollAnimator::Config SKScrollDistance::config()
{
    static LLUICachedControl<bool> smooth("SkoomaScrollSmooth", true);
    static LLUICachedControl<S32> mode("SkoomaScrollSmoothMode", 0);
    static LLUICachedControl<F32> duration_scale("SkoomaScrollSmoothDurationScale", 1.f);
    static LLUICachedControl<F32> top_speed("SkoomaScrollWarpTopSpeed", 6000.f);
    static LLUICachedControl<F32> ramp("SkoomaScrollWarpRamp", 0.04f);
    static LLUICachedControl<F32> brake("SkoomaScrollWarpBrake", 0.14f);

    SKScrollAnimator::Config config;
    config.mSmooth = smooth;
    config.mMode = mode == SKScrollAnimator::WARP ? SKScrollAnimator::WARP : SKScrollAnimator::CLASSIC;
    config.mDurationScale = llmax((F32)duration_scale, 0.f);
    config.mWarp.mTopSpeed = llmax((F32)top_speed, 1.f);
    config.mWarp.mRampSeconds = llmax((F32)ramp, 0.001f);
    config.mWarp.mBrakeSeconds = llmax((F32)brake, 0.001f);
    return config;
}

F64 SKScrollDistance::now()
{
    return LLFrameTimer::getUptimeSeconds();
}
