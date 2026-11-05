/**
 * @file skoverviewcapture.h
 * @brief The overview capture: the census as styled shapes over the terrain, as top-down region maps and as a
 *        view from a camera.
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

#ifndef SK_OVERVIEWCAPTURE_H
#define SK_OVERVIEWCAPTURE_H

#include "alquerypool.h"
#include "llcamera.h"
#include "llmatrix4a.h"
#include "llrect.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "v2math.h"
#include "skoverviewcensus.h"
#include "skoverviewmovers.h"
#include "skoverviewplace.h"
#include "skrooms.h"
#include "v3color.h"
#include "v3dmath.h"
#include "v4math.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class LLGLSLShader;
class LLRenderTarget;
class LLViewerRegion;

namespace skoverview
{
    /// Someone besides the agent the cut is taken for, as a people layer knows them. Main thread.
    struct Occupant
    {
        LLUUID mId;
        LLVector3d mFeet;           ///< global; z means something only with mZKnown
        bool mZKnown = false;       ///< false for someone placed only coarsely, who is left out
        place::Whereabouts mWhere;  ///< their air gap, from place::whereabouts at their feet
        bool mHidden = false;       ///< not shown by the caller, and so left out
    };

    /// What the region maps style: the opt-in layers (overviewResolveF.glsl's MASK_*) and whether the void past the
    /// regions is mapped too; with the people the agent's region is cut for.
    struct MapParams
    {
        U32 mClassMask = 0;
        bool mVoidTiles = true;
        std::vector<Occupant> mOccupants;
    };

    /// The see-through circles the camera view opens round people: one over each within mReach metres of the eye in
    /// outdoor air, mRadius times their height on screen, its edge mSoftness of its radius.
    struct DissolveParams
    {
        bool mEnabled = true;
        F32 mRadius = 1.25f;
        F32 mReach = 64.f;
        F32 mSoftness = 0.25f;
    };

    /// One frame of the camera view: the camera, what it centres on, the people it cuts and opens circles for, and the
    /// window viewport it covers.
    struct ViewShotParams
    {
        LLVector3 mEye;             ///< agent space
        LLVector3 mAt;              ///< the camera's axes, agent space
        LLVector3 mLeft;
        LLVector3 mUp;
        F32 mFov = 1.f;             ///< vertical, radians
        F32 mDrawDistance = 256.f;  ///< how far past the centre the census is culled
        LLVector3d mCentre;         ///< global: the fade and the cut's priorities follow it
        S32 mViewport[4] = {};      ///< window pixels: x, y, width, height
        std::vector<Occupant> mOccupants;
        DissolveParams mDissolve;

        /// The viewer camera and the GL viewport, centred on centre.
        static ViewShotParams fromViewerCamera(const LLVector3d& centre);
    };

    /// The colours and depths the overview styles with beside its shaders' own; a caller may set others.
    struct Palette
    {
        F32 mWaterDepthM = 6.f;                                                      ///< water this deep reads as deep
        LLColor3 mInvisible = LLColor3(240.f / 255.f, 180.f / 255.f, 76.f / 255.f);  ///< invisible barriers' hatch
        LLColor3 mGround = LLColor3(16.f / 255.f, 16.f / 255.f, 20.f / 255.f);       ///< behind the camera view
    };
}

// Renders terrain and the census's shapes below a cut plane at the air cells' floor under the agent plus
// SKOverviewCutHeight, each writing a class code, then resolves depth and codes into an RGBA8 map. Two kinds of shot
// share the passes: straight-down maps of every connected region, while a map draws them (the agent's region cut,
// every SKOverviewRefresh seconds, its neighbours uncut and less often, regions further off rarer still), and a view
// from a camera its caller places, composited over the window in place of the world. Main thread.
class SKOverviewCapture : public LLSingleton<SKOverviewCapture>
{
    LLSINGLETON(SKOverviewCapture);
    ~SKOverviewCapture() override;

public:
    struct Stats
    {
        S32 resolution = 0;
        F32 cpu_ms = 0.f;           // cull, sort, draw submission and resolve on the CPU
        F32 gpu_ms = -1.f;          // the agent's map: capture and resolve on the GPU, a few frames late
        F32 gpu_view_ms = -1.f;     // the camera view's static render
        F32 gpu_patch_ms = -1.f;    // its see-through circles' patch
        F32 gpu_movers_ms = -1.f;   // its movers
        F32 gpu_resolve_ms = -1.f;  // one of its resolves
        U32 draws = 0;              // draw infos sorted for the capture, plus the terrain pool, plus the census's page draws
        U32 census_tris = 0;
        U32 captures = 0;
    };

    /// Called once a frame after the world has rendered, before the UI: the per-frame work while maps or a view want it.
    static void onFrame();
    static void cleanupGL();
    /// Builds the overview's programs, from LLViewerShaderMgr; a failure leaves the overview off until the next load.
    static bool loadShaders(S32 level);
    static void unloadShaders();

    /// A map drew the region maps this frame, styled by params: they are captured while one keeps asking.
    static void noteMapsWanted(const skoverview::MapParams& params);
    static bool mapsWanted();
    /// A camera view is shown this frame: the census is kept round it while one keeps asking.
    static void noteViewWanted();
    static bool viewWanted();
    /// How far from the agent the camera view looks, metres: the census reaches that far, in 64 m steps.
    static void setViewReach(F32 reach_m);
    /// Whether the world is hidden behind the camera view, so the census may scan slowly.
    static void setWorldHidden(bool hidden);

    /// The per-frame work: the census demand and update, and the maps that are due. onFrame runs it while the world
    /// is drawn; a caller that hides the world runs it itself before renderView.
    void frame();

    /// A region's resolved map: it covers the region at mOrigin by mWidth metres.
    struct Tile
    {
        U32 mTexture = 0;
        LLVector3d mOrigin;
        F32 mWidth = 0.f;
    };
    /// The captured regions' maps, the agent's last so it draws over its neighbours' edges.
    std::vector<Tile> tiles() const;
    const Stats& stats() const { return mStats; }
    /// The census the overview draws, with the cut it last worked out.
    const SKOverviewCensus& census() const { return mCensus; }
    /// The vehicles and their looks, or none; the caller keeps the source alive until it sets another.
    void setMoverSource(const skoverview::MoverSource* source) { mMovers.setSource(source); }
    /// The movers the camera view draws, live.
    const SKOverviewMovers& movers() const { return mMovers; }
    const skoverview::Palette& palette() const { return mPalette; }
    void setPalette(const skoverview::Palette& palette) { mPalette = palette; }

    /// The floor under the agent from the air cells and the cut above it, or the agent's height and the old offset.
    void cutHeights(const LLVector3d& at, F32 fallback_z, F32& floor_z, F32& cut_z) const;
    /// Works out the per-building cut (SKOverviewCensus::applyCut) for the agent and the occupants, those nearest the
    /// centre first. The camera view runs it every render; the maps, with the last map's occupants, otherwise.
    void updateCut(const LLVector3d& centre, const std::vector<skoverview::Occupant>& occupants);

    /// Renders the camera view for this frame: the cut, the circles, the movers and the pick. False when it could not.
    bool renderView(const skoverview::ViewShotParams& params);
    /// Draws the last rendered view over rect (window pixels) of the bound target, blurred toward its top and bottom
    /// over tilt_px; once a render, false when there is none.
    bool compositeView(const LLRect& rect, F32 tilt_px);
    /// Whether a rendered view's camera and viewport are there for picks.
    bool viewPickable() const { return mViewPickable; }
    const LLMatrix4a& viewMatrix() const { return mViewView; }
    const LLMatrix4a& projMatrix() const { return mViewProj; }
    /// Window pixels: x, y, width, height.
    const S32* viewport() const { return mViewViewport; }
    /// The view's centre, agent space, and how far its census reaches round it.
    const LLVector3& viewCentre() const { return mViewCentre; }
    F32 viewReach() const { return mViewReach; }
    /// The target holding the shown frame's depth, which markers drawn over the view test against; null before one.
    LLRenderTarget* sceneDepth() const { return mSceneDepth; }
    /// The program the overview builds for depth-tested markers over the view (overviewMarkerF.glsl).
    static LLGLSLShader& markerProgram();
    /// What the view draws under a viewport pixel: the shown frame's depth there, read back a frame or two later,
    /// while the cursor is still over it; else the terrain under the view camera's ray, else the agent's floor plane.
    bool groundAt(const LLVector2& at, LLVector3d& world) const;

private:
    struct Shot
    {
        LLMatrix4a mView;
        LLMatrix4a mProj;
        LLCamera mCull;
        LLVector3 mEye;     // agent space
        F32 mCut = 0.f;     // agent-space z
        LLVector3 mFocus;   // agent space: what the view centres on; distance fade and height colours are relative to it
        bool mPartCuts = false; // each census part is cut at its own height (the camera view), not at mCut
        F32 mLevelTop = 0.f; // the agent's cut height, the brightest the height colours go
        F32 mWater = 0.f;   // the water surface's height
        SKOverviewCensus::Level mLevel; // the census groups it draws
        LLVector4 mRegion;  // the region styled as the agent's: min x, min y, max x, max y, agent space
        std::vector<LLVector4> mTerritory;  // the regions styled as inside, same form; empty means mRegion alone
        bool mTopDown = false;
        std::vector<LLVector4> mDissolve;   // the see-through circles: window x, y, eye depth, radius in pixels
        std::vector<F32> mDissolveFloor;    // each one's floor, agent z
        F32 mDissolveSoft = 0.25f;          // their edge, a fraction of the radius
    };
    /// A region's top-down map and when it was last taken.
    struct RegionMap;

    void updateDemand(bool wanted);
    bool ensureGL(LLRenderTarget& capture, LLRenderTarget& resolved, S32 w, S32 h);
    /// Whether every program loadShaders() builds is there; it builds nothing.
    bool ensureShaders();
    void releaseGL();
    /// The terrain and the census into capture; with a patch, only inside that window rect, over what is there.
    U32 render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, bool resolve_now = true,
               const LLRect* patch = nullptr);
    /// The capture's depth and class codes to the styled map, whole or inside a rect.
    void resolve(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, const LLRect* rect = nullptr);
    /// The work target at the view capture's size: false when it would not allocate.
    bool ensureWork();
    /// The movers into the work target, over the static scene restored under them.
    void drawMovers(const Shot& shot);
    /// The view's work target brought up to this frame: the static capture with the see-through circles' patch and
    /// the movers, repaired and resolved by rect; static_new says the static capture was just rendered.
    void updateWork(const Shot& shot, bool static_new, U64 patch_key, const LLRect& patch_rect, bool movers);
    /// Captures the region maps that are due: the agent's on its timer, then the most overdue other one, the void tiles
    /// beside the regions among them.
    void captureMaps(F64 now);
    /// One map's top-down capture; region is none for a void tile, which takes its square from the map.
    bool captureMap(RegionMap& map, LLViewerRegion* region, bool agent);
    /// The depth texel under the cursor, with the camera the frame was drawn with.
    struct PickSample
    {
        S32 mX = 0;
        S32 mY = 0;
        F32 mDepth = 0.f;
        LLMatrix4a mView;
        LLMatrix4a mProj;
        S32 mViewport[4] = {};
        LLVector3d mAgentOrigin;    ///< agent space's origin, global, as the frame was drawn
        U32 mFrame = 0;             ///< the probes' frame it was read in
        bool mValid = false;
    };
    /// Reads this frame's texel under the cursor, and takes in the ones that landed.
    void updatePick();
    std::vector<PickSample> mPicksInFlight;
    PickSample mPick;               ///< the newest landed
    /// How picks were answered over a second, for the log.
    mutable U32 mPicksByTexel = 0;
    mutable U32 mPicksByFallback = 0;
    mutable F64 mPicksSince = 0.0;
    /// What a GPU span times; each is read a few frames on into Stats and the cost line.
    enum class SpanKind : U8
    {
        Map,
        View,
        Patch,
        Movers,
        Resolve
    };
    /// Ends a span begun with gGL.queries().beginSpan() and keeps it to be read; a span that began nothing is dropped.
    void endSpan(SpanKind kind, ALQuerySpan& span);
    void readSpans();
    std::vector<std::pair<SpanKind, ALQuerySpan>> mSpans;

    SKOverviewCensus mCensus;
    SKOverviewProbes mProbes;   ///< the view cut's and the pick's questions to the GPU
    SKOverviewMovers mMovers;
    skoverview::Palette mPalette;
    LLRenderTarget* mSceneDepth = nullptr;   ///< the capture whose depth this frame's markers test against
    bool mWorkValid = false;    ///< the work target holds the static capture, with the rects below drawn over it
    LLRect mPatchRect;          ///< the window rect the circles' patch covers on the work target, empty for none
    LLRect mMoverRect;          ///< and the movers drawn last frame
    U64 mPatchKey = 0;          ///< the circles the patch was drawn for
    std::vector<std::unique_ptr<RegionMap>> mMaps;
    size_t mCursor = 0;     // where the next round-robin turn starts in mMaps
    F64 mNextSlot = 0.0;    // when the next turn may capture
    Stats mStats;
    F32 mDemandRange = 0.f;
    U32 mClassMask = 0;     // the layers the region maps were last styled with
    bool mGLFailed = false;
    bool mDemanding = false;
    LLMatrix4a mViewView;
    LLMatrix4a mViewProj;
    S32 mViewViewport[4] = {};
    bool mViewReady = false;        ///< rendered this frame, for the composite
    bool mViewPickable = false;     ///< the view and viewport are a shown frame's, for picks
    LLVector3 mViewCentre;          // agent space
    F32 mViewReach = 0.f;
    F32 mViewShadeZ = 0.f;          ///< agent-space z the height colours are relative to: the agent's floor, eased
    F64 mViewShadeTime = -1.0;      ///< when it was last eased; negative until the first shot
    /// A person's see-through circle on the camera view, eased in and out so it never pops.
    struct Dissolve
    {
        LLVector3 mFloor;       ///< agent space, their feet
        F32 mAmount = 0.f;      ///< 0 closed, 1 open
        bool mSelf = false;
    };
    std::map<LLUUID, Dissolve> mDissolves;
    F64 mDissolveTime = -1.0;
    /// Fills the shot's see-through circles round the agent and the occupants, and the window rect they cover; the key
    /// of what it drew, which moves the patch, never the static capture.
    U64 viewDissolve(Shot& shot, const skoverview::ViewShotParams& params, S32 w, S32 h, LLRect& rect);
    /// The air's room around a floor point, from its start gap's space; null where that space is in no room.
    /// A room's footprint is built once per room hash and kept a few seconds while it is in use.
    std::shared_ptr<const SKOverviewCensus::RoomFootprint> roomAt(const LLVector3d& floor);
    struct HeldRoom
    {
        SKWorldView::RoomId mId;
        U64 mHash = 0;          ///< the room's when its footprint was built
        U64 mRevision = 0;      ///< the world view's air revision then
        F64 mAt = 0.0;
        std::shared_ptr<const SKOverviewCensus::RoomFootprint> mFootprint;
    };
    std::vector<HeldRoom> mRooms;
    /// Whether someone stands in outdoor air, which cuts nothing for them, held through a doorway's flicker.
    bool outdoorsAt(const LLUUID& id, const LLVector3d& feet, F64 now);
    struct Outdoors
    {
        bool mOut = false;      ///< the held answer
        F64 mSince = -1.0;      ///< when the air first read otherwise, or -1
        F64 mSeen = 0.0;
    };
    std::map<LLUUID, Outdoors> mOutdoors;
    /// Someone's air column, read once a frame for the cut, the dissolve and their whereabouts; null with no air.
    const SKWorldView::AirColumn* columnOf(const SKWorldViewPtr& world, const LLUUID& id, const LLVector3d& feet, F64 now);
    struct HeldColumn
    {
        F64 mAt = -1.0;
        U64 mRevision = 0;
        LLVector3d mXY;
        SKWorldView::AirColumn mColumn;
    };
    std::map<LLUUID, HeldColumn> mColumns;
    /// What the last view capture showed: the same again reuses its image.
    struct ViewKey
    {
        bool mValid = false;
        LLVector3 mEye;
        LLVector3 mDir;
        F32 mFov = 0.f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        F32 mCut = 0.f;
        U64 mCensus = 0;
        U64 mCutStamp = 0;
        U32 mDebugView = 0;
        U64 mRegionHandle = 0;
    } mViewKey;
    /// CPU time of the camera view, summed over a second and logged.
    struct Cost
    {
        F64 mSince = 0.0;
        F32 mCaptureMs = 0.f;
        F32 mResolveMs = 0.f;
        F32 mCompositeMs = 0.f;
        F32 mGpuViewMs = 0.f;   // GPU spans read in this second, summed
        F32 mGpuPatchMs = 0.f;
        F32 mGpuMoversMs = 0.f;
        F32 mGpuResolveMs = 0.f;
        U32 mFrames = 0;
        U32 mRenders = 0;
        U32 mReuses = 0;
        U32 mScansAtStart = 0;
    } mCost;
};

#endif // SK_OVERVIEWCAPTURE_H
