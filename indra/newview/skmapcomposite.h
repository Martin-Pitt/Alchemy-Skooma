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
#include "threadpool.h"

#include <string>
#include <vector>

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

    /// How a plan drawn as an overlay weighs its lines and its surfaces, each 0 to 1.
    struct Overlay
    {
        F32 mLines = 1.f;
        F32 mSurface = 1.f;
    };

    /// Bring the plan up to date for this view and draw it over the widget, in widget coordinates.
    /// With an overlay the bound viewport must be exactly the view, as a target sized to it is.
    void draw(const skmap::MapView& view, const Overlay* overlay = nullptr);
    void release();

    struct Stats
    {
        F32 mSnapshotMs = 0.f;      ///< main thread
        F32 mUploadMs = 0.f;        ///< main thread
        F32 mGatherMs = 0.f;        ///< the rest on the plan's thread
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
        U32 mJobs = 0;
        U32 mTilesRead = 0;         ///< tiles whose border links the last snapshot read again
        // Coverage, for the owner's question of how much of the view the plan reaches.
        U32 mRegions = 0;           ///< regions meeting the frame
        U32 mMeshes = 0;            ///< region meshes in the plan (those with spaces)
        U32 mPolysSpaced = 0;       ///< polys in some space
        U32 mPolysDrawn = 0;        ///< polys of drawn spaces
        U32 mSpacesAll = 0;
    };
    const Stats& stats() const { return mStats; }
    std::vector<std::string> statsLines() const;

private:
    bool ensureGL(U32 side, U32 samples);
    void drawOverlay(const skmap::MapView& view, const LLVector2* uv, const F32 (*corner)[2], const Overlay& overlay);
    /// The overlay renders surfaces and lines into separate textures, so each takes its own weight.
    enum class RenderPass
    {
        ALL,
        SURFACES,
        LINES
    };
    void render(F32 vz, RenderPass pass, U32 target_fbo);
    bool ensureLineTarget();

    struct Job;
    struct Want
    {
        U64 mDataKey = 0;
        LLVector3d mOrigin;
        skmap::plan::SpaceRef mViewer;
        skmap::plan::Options mOptions;
        F32 mMpp = 0.f;
    };
    void land();
    void post(const Want& want, const LLVector3d& gmin, const LLVector3d& gmax, bool new_data);

    skmap::plan::Gatherer mGatherer;
    std::unique_ptr<LL::ThreadPool> mPool;
    std::shared_ptr<Job> mJob;      ///< in flight on the plan's thread
    bool mHasPosted = false;
    Want mPosted;                   ///< what the last job was posted for
    U64 mNavRevision = ~0ULL;
    const void* mViewSeen = nullptr;
    U64 mViewRevision = ~0ULL;
    U64 mDataKey = 0;               ///< the gather key at the last check
    LLVector3d mKeyOrigin;
    bool mHasKey = false;

    // What is shown: the last job that landed.
    skmap::plan::DataPtr mData;
    skmap::plan::Options mOptions;
    skmap::plan::Selection mSelection;
    LLVector3d mFrameOrigin;

    // The frame the composite covers, plan-local metres.
    LLVector2 mFrameMin;
    F32 mFrameSize = 0.f;
    F32 mMpp = 0.f;
    F32 mSpan = 0.f;                ///< metres across the widget's short side, as the view has it
    F32 mDrawnVz = F32_MAX;
    bool mRenderDirty = true;
    bool mDrawnSplit = false;       ///< the last render was the overlay's two layers

    // GL objects.
    U32 mProgram = 0;
    U32 mShadeProgram = 0;
    U32 mBlitProgram = 0;
    U32 mBlitVao = 0;
    S32 mLocPos = -1, mLocUv = -1, mLocWeight = -1;
    U32 mVao = 0;
    U32 mVbo = 0;
    U32 mTerrainVbo = 0;
    U32 mTerrainIbo = 0;
    U32 mTerrainCount = 0;
    U32 mMsaaFbo = 0, mMsaaColour = 0, mMsaaDepth = 0;
    U32 mResolveFbo = 0, mResolveTex = 0;
    U32 mLineFbo = 0, mLineTex = 0; ///< the overlay's lines; the resolve target then holds its surfaces
    U32 mShadeFbo = 0, mShadeTex = 0, mShadeDepth = 0;
    U32 mSide = 0;
    U32 mSamples = 0;
    bool mGLFailed = false;

    Stats mStats;
};

#endif // SK_MAPCOMPOSITE_H
