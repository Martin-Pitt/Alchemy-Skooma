/**
 * @file skoverviewcapture.h
 * @brief A top-down render of the agent's region, shaded as a map, for the mini-map's base layer.
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

#ifndef SK_OVERVIEWCAPTURE_H
#define SK_OVERVIEWCAPTURE_H

#include "llsingleton.h"
#include "v3dmath.h"

#include <string>

// Renders the agent's region from straight above with the depth-only shadow programs, cut at the agent's height plus
// SKOverviewCutHeight, then resolves depth and a class per texel (terrain, object, invisible) into an RGBA8 map
// coloured by height relative to the agent. Re-captured every SKOverviewRefresh seconds while SKMapCaptureBase is on.
class SKOverviewCapture : public LLSingleton<SKOverviewCapture>
{
    LLSINGLETON(SKOverviewCapture);
    ~SKOverviewCapture() override;

public:
    struct Stats
    {
        S32 resolution = 0;
        F32 cpu_ms = 0.f;    // cull, sort, draw submission and resolve on the CPU
        F32 gpu_ms = -1.f;   // capture and resolve on the GPU, from the previous capture's timer query
        U32 draws = 0;       // draw infos sorted for the capture, plus the terrain pool
        U32 captures = 0;
    };

    /// Called once a frame after the world has rendered, before the UI.
    static void onFrame();
    static void cleanupGL();

    /// The resolved map, 0 until the first capture; it covers the region at originGlobal() by regionWidth() metres.
    U32 texture() const;
    const LLVector3d& originGlobal() const { return mOrigin; }
    F32 regionWidth() const { return mWidth; }
    const Stats& stats() const { return mStats; }

private:
    void frame();
    bool ensureGL(S32 res);
    void releaseGL();
    bool capture();
    void readTimer();

    LLVector3d mOrigin;
    F32 mWidth = 0.f;
    F64 mLastCapture = -1.0;
    F32 mAgentZ = 0.f;
    F32 mTop = 0.f;
    F32 mBottom = 0.f;
    Stats mStats;
    bool mGLFailed = false;
    bool mTiming = false;
    bool mValid = false;
};

#endif // SK_OVERVIEWCAPTURE_H
