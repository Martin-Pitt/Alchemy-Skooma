/**
 * @file skmapcomposite.h
 * @brief The mini-map plan drawn once into a multisampled offscreen target, then shown as one quad per frame.
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

#ifndef SK_MAPCOMPOSITE_H
#define SK_MAPCOMPOSITE_H

#include "skmapplan.h"

#include <string>

namespace skmap
{
    class MapView;
}

/// Main thread, inside the UI pass. Owns its GL objects; release() frees them.
class SKMapComposite
{
public:
    SKMapComposite() = default;
    ~SKMapComposite();
    SKMapComposite(const SKMapComposite&) = delete;
    SKMapComposite& operator=(const SKMapComposite&) = delete;

    /// Bring the plan up to date for this view and draw it over the widget, in widget coordinates.
    void draw(const skmap::MapView& view);
    void release();

    struct Stats
    {
        F32 mGatherMs = 0.f;
        F32 mSelectMs = 0.f;
        F32 mGeometryMs = 0.f;
        F32 mRenderMs = 0.f;
        U32 mTriangles = 0;
        U32 mRanks = 0;
        U32 mSpaces = 0;
        U32 mPolys = 0;
        U32 mWallRuns = 0;
        U32 mSamples = 0;
        U32 mSide = 0;
        U32 mRedraws = 0;
    };
    const Stats& stats() const { return mStats; }
    std::string statsLine() const;

private:
    bool ensureGL(U32 side, U32 samples);
    void render(F32 vz);
    void buildTerrain();

    skmap::plan::DataPtr mData;
    U64 mDataKey = 0;
    U64 mNavRevision = ~0ULL;
    const void* mViewSeen = nullptr;
    U64 mViewRevision = ~0ULL;
    S32 mViewerSpace = -2;
    skmap::plan::Options mOptions;
    skmap::plan::Selection mSelection;
    bool mHasSelection = false;
    skmap::plan::Geometry mGeometry;
    U64 mGeometryKey = 0;
    bool mGeometryDirty = true;

    // The frame the composite covers, plan-local metres.
    LLVector2 mFrameMin;
    F32 mFrameSize = 0.f;
    F32 mMpp = 0.f;
    F32 mSpan = 0.f;                ///< metres across the widget's short side, as the view has it
    F32 mDrawnVz = F32_MAX;
    bool mRenderDirty = true;

    // GL objects.
    U32 mProgram = 0;
    U32 mShadeProgram = 0;
    U32 mVao = 0;
    U32 mVbo = 0;
    U32 mTerrainVbo = 0;
    U32 mTerrainIbo = 0;
    U32 mTerrainCount = 0;
    const void* mTerrainData = nullptr;
    U32 mMsaaFbo = 0, mMsaaColour = 0, mMsaaDepth = 0;
    U32 mResolveFbo = 0, mResolveTex = 0;
    U32 mShadeFbo = 0, mShadeTex = 0, mShadeDepth = 0;
    U32 mSide = 0;
    U32 mSamples = 0;
    bool mGLFailed = false;

    Stats mStats;
};

#endif // SK_MAPCOMPOSITE_H
