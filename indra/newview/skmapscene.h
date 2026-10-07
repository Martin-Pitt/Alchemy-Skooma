/**
 * @file skmapscene.h
 * @brief The mini-map floorplan: navmesh demand, the scene worker, cell bakes and their draw.
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

#include "llframetimer.h"
#include "llsingleton.h"
#include "threadpool.h"

#include "llpointer.h"

#include "skmapgeom.h"
#include "skmapraster.h"
#include "skmapribbons.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

class LLVertexBuffer;
class LLViewerTexture;
class SKNetMap;

/// Main thread only; scene analysis runs on a one-thread pool of its own. Costs nothing while
/// no map has drawn for a couple of seconds.
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

    /// The latest scene, or null before the first lands.
    skmap::ScenePtr scene() const { return mScene; }
    /// Whether a 32 m lattice cell holds a published navmesh tile.
    bool built(S32 x, S32 y) const;

    /// Draw the floorplan: the selected layers' baked cells, back to front.
    void drawFloorplan(const skmap::MapView& view);
    /// Hatch outside every region, then the region and void edges.
    void drawVoid(const skmap::MapView& view);

    /// Where the agent stands in the scene, refreshed each frame the map draws, whatever the layers.
    const skmap::AgentFloor& agentFloor() const { return mAgent; }
    /// Whether agentFloor() was refreshed this frame or the one before.
    bool agentFloorCurrent() const { return mAgentFrame && LLFrameTimer::getFrameCount() - mAgentFrame <= 1; }

    struct Stats
    {
        U32 mJobs = 0;
        F32 mJobMs = 0.f;
        U32 mPolys = 0;
        U32 mLayers = 0;
        U32 mCells = 0;             ///< baked cells drawn last frame
        U32 mQueued = 0;            ///< cells wanted and not baked yet
        U32 mUploads = 0;           ///< uploads last frame
        U32 mBakes = 0;             ///< bakes since start
        F32 mCacheMB = 0.f;
    };
    const Stats& stats() const { return mStats; }

    static void onIdle(void*);

private:
    void idle();
    void updateDemand(bool want);
    void maybePost();
    void gather(skmap::SceneInput& in);
    void land(const skmap::ScenePtr& scene, U32 generation, F32 millis);

    /// What a scene was built from: a new job goes out when any of it moves.
    struct Trigger
    {
        LLVector3d mMin, mMax;
        U64 mNavRevision = 0;
        U64 mViewRevision = 0;
        U64 mCensus = 0;
        U64 mRegions = 0;
        bool operator==(const Trigger& o) const
        {
            return mMin == o.mMin && mMax == o.mMax && mNavRevision == o.mNavRevision && mViewRevision == o.mViewRevision
                && mCensus == o.mCensus && mRegions == o.mRegions;
        }
    };
    Trigger currentTrigger() const;

    LL::WorkQueue::ptr_t queue();
    /// Where the agent stands in the scene, at most one navmesh read.
    void updateAgentFloor();
    /// The layers to draw and their cuts, recomputed when the agent's cell or floor or the scene moved.
    void updateSelection();
    /// A cell's bake key: its content hash with the tier, the baked settings and the raster version.
    U64 bakeKey(S32 layer, const skmap::CellId& cell) const;
    void requestBake(S32 layer, const skmap::CellId& cell, U64 key, F32 priority);
    void postBake();
    void uploadBakes();
    void evictBakes();
    /// Draw one layer's cell texture over a world rectangle, 2 x 2 sub-quads with per-vertex colour.
    void drawCell(const skmap::MapView& view, LLViewerTexture* texture, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0,
                  F32 u1, F32 v1, const LLColor4& tint) const;
    /// The same with the texture already bound.
    void drawCellQuads(const skmap::MapView& view, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0, F32 u1, F32 v1,
                       const LLColor4& tint) const;
    /// The same, leaving out the layer's cut cells.
    void drawCellCut(const skmap::MapView& view, LLViewerTexture* texture, const LLVector3d& min, const LLVector3d& max, F32 u0, F32 v0,
                     F32 u1, F32 v1, const LLColor4& tint, S32 layer) const;
    /// Draw the cached bakes of a cell's children, or failing those their children, down two tiers.
    void drawFinerBakes(const skmap::MapView& view, S32 layer, const skmap::CellId& cell, const LLColor4& tint, F64 now);

    /// The ribbon parameters for this frame: the zoom bucket, the eye height and the toggles.
    skmap::RibbonParams ribbonParams(const skmap::MapView& view) const;
    static U64 ribbonKey(U64 mark_hash, const skmap::RibbonParams& params);
    /// What a layer's lines are built from: its mark hash, and its cut when it has one.
    U64 ribbonIdentity(S32 layer) const;
    /// The buffer the previous scene drew for the layer that is now this one, or 0.
    U64 carriedRibbon(S32 layer);
    /// Draw a layer's ribbon buffer, or the last one built for its marks while a new one is made.
    void drawRibbons(const skmap::MapView& view, S32 layer, const skmap::RibbonParams& params);
    /// Draw a buffer of scene-local metres built for the scene whose origin is given.
    void drawBuffer(const skmap::MapView& view, LLVertexBuffer* vb, const LLVector3d& origin) const;
    void postRibbons();
    void uploadRibbons();

    struct RibbonBuffer
    {
        LLPointer<LLVertexBuffer> mBuffer;
        LLVector3d mOrigin;         ///< the origin of the scene it was built in
        F64 mLastDrawn = 0.0;
    };
    struct RibbonRequest
    {
        S32 mLayer = -1;
        U64 mKey = 0;
        U64 mMarkHash = 0;                  ///< the ribbon identity: marks, and the cut if any
        skmap::RibbonParams mParams;
    };
    struct RibbonResult
    {
        U64 mKey = 0;
        U64 mMarkHash = 0;
        LLVector3d mOrigin;
        skmap::Ribbons mRibbons;
    };

    struct Bake
    {
        LLPointer<LLViewerTexture> mTexture;
        F64 mLastDrawn = 0.0;
    };
    struct BakeRequest
    {
        S32 mLayer = -1;
        skmap::CellId mCell;
        U64 mKey = 0;
        F32 mPriority = 0.f;        ///< distance from the map centre: lower first
    };
    struct BakeResult
    {
        U64 mKey = 0;
        std::vector<U8> mPixels;
    };

    std::unique_ptr<LL::ThreadPool> mPool;
    std::shared_ptr<bool> mAlive;
    U32 mGeneration = 0;
    bool mInFlight = false;
    F64 mLastPost = -1.0e9;
    Trigger mPosted;
    bool mHasPosted = false;
    skmap::ScenePtr mScene;
    /// Each tile's last published copy and when it was last seen, carried while the navmesh rebuilds it.
    std::map<std::pair<LLUUID, sknav::TileKey>, std::pair<skmap::TileInput, F64>> mLastTiles;
    bool mDemanding = false;
    F64 mLastDrawn = -1.0e9;
    LLVector3d mMapCentre;
    F32 mMapLong = 200.f;
    F32 mMapShort = 200.f;
    Stats mStats;

    skmap::RasterParams mRasterParams;
    S32 mTier = -1;
    std::map<U64, Bake> mBakes;
    std::vector<BakeRequest> mWanted;       ///< this frame's missing cells
    bool mBakeInFlight = false;
    U64 mBakeKeyInFlight = 0;
    std::deque<BakeResult> mUploads;

    std::map<U64, RibbonBuffer> mRibbons;
    std::map<U64, U64> mRibbonLatest;       ///< ribbon identity -> the key last built for it
    /// A layer of the previous scene that drew lines, kept so its successor can draw them while its own builds.
    struct CarrySource
    {
        bool mGround = false;
        F32 mBaseZ = 0.f;
        U64 mCut = 0;                       ///< its cut hash, 0 when uncut
        U64 mKey = 0;                       ///< the buffer it drew
        std::vector<std::pair<S32, S32>> mSample;   ///< up to 64 of its footprint cells
    };
    std::map<S32, std::pair<U64, U64>> mShownRibbons;   ///< layer -> (buffer key, cut hash) drawn for it last
    std::vector<CarrySource> mCarrySources;
    std::map<S32, U64> mCarried;            ///< layer -> its carried buffer, 0 for none, resolved once
    std::vector<RibbonRequest> mRibbonWanted;
    bool mRibbonInFlight = false;
    std::deque<RibbonResult> mRibbonUploads;

    LLPointer<LLViewerTexture> mHatch;
    LLPointer<LLVertexBuffer> mBoundaryBuffer;
    LLVector3d mBoundaryOrigin;
    U64 mBoundaryKey = 0;
    U64 mBoundaryHash = 0;                  ///< the scene's region and void edges, in global centimetres

    skmap::AgentFloor mAgent;
    U32 mAgentFrame = 0;            ///< the frame count mAgent was last refreshed in
    skmap::LayerSelection mSelection;
    /// Weak, so a later scene allocated where a freed one was is never taken for it.
    std::weak_ptr<const skmap::Scene> mSelectedScene;
    std::pair<S32, S32> mSelectedCell = { 0, 0 };
    S32 mSelectedLayer = -2;
    sknav::EnclosureBucket mSelectedBucket = sknav::EnclosureBucket::UNKNOWN;
    S32 mSelectedZ = 0;                     ///< the agent's height in 0.5 m steps
    F32 mSelectedFade = -1.f;
    std::map<S32, std::pair<LLVector3d, LLVector3d>> mCutBoxes;
    std::map<S32, U64> mCutHashes;          ///< per cut layer, a hash of its cut cells
};

#endif // SK_MAPSCENE_H
