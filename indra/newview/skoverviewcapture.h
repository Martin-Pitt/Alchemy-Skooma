/**
 * @file skoverviewcapture.h
 * @brief The overview: the census as styled shapes over the terrain, for the mini-map's base and
 *        the tactical view.
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

#include "alquerypool.h"
#include "llcamera.h"
#include "llhandle.h"
#include "llmatrix4a.h"
#include "llrect.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "v2math.h"
#include "skmaplabels.h"
#include "skmodalview.h"
#include "skmapplan.h"
#include "skoverviewcensus.h"
#include "skoverviewmovers.h"
#include "v3dmath.h"
#include "v4math.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <set>
#include <vector>

class LLRenderTarget;
class LLViewerRegion;
class LLVOAvatar;
namespace skmapdraw
{
    struct TagText;
}
class SKPopupPanes;

// Renders terrain and the census's shapes below a cut plane at the
// navmesh floor under the agent plus SKOverviewCutHeight, each writing a class code, then resolves depth and codes
// into an RGBA8 map. Two kinds of shot share the passes: straight-down maps of every connected region for the mini-map
// (while a map draws them; the agent's region cut, every SKOverviewRefresh seconds, its neighbours uncut and less often,
// regions further off rarer still) and the tactical view (SKTacticalOverview), a modal view drawn from the viewer's
// own camera, which it tilts over the agent as it opens, so alt-cam and the wheel move it as they move the world camera.
class SKOverviewCapture : public LLSingleton<SKOverviewCapture>, public SKModalMode
{
    LLSINGLETON(SKOverviewCapture);
    ~SKOverviewCapture() override;

public:
    struct Stats
    {
        S32 resolution = 0;
        F32 cpu_ms = 0.f;    // cull, sort, draw submission and resolve on the CPU
        F32 gpu_ms = -1.f;   // the agent's map: capture and resolve on the GPU, a few frames late
        F32 gpu_tactical_ms = -1.f; // the tactical view's static render
        F32 gpu_patch_ms = -1.f;    // its see-through circles' patch
        F32 gpu_movers_ms = -1.f;   // its movers
        F32 gpu_resolve_ms = -1.f;  // one of its resolves
        U32 draws = 0;       // draw infos sorted for the capture, plus the terrain pool
        U32 census_tris = 0;
        U32 captures = 0;
    };

    /// Called once a frame after the world has rendered, before the UI.
    static void onFrame();
    static void cleanupGL();
    /// Builds the overview's programs, from LLViewerShaderMgr; a failure leaves the overview off until the next load.
    static bool loadShaders(S32 level);
    static void unloadShaders();
    /// Whether the tactical overview is the modal view on top, drawn in place of the world.
    static bool tacticalShown();

    /// A world right-click at the root view's (x, y): opens the mini-map's menus on the overview; false when it is off.
    static bool handleRightClick(S32 x, S32 y);

    /// The tactical overview key: held, it shows the view until released; tapped, it latches it until the next tap.
    static void handleTacticalKey(bool down);
    /// A held movement binding while the overview is shown: it pans the map on the ground instead of moving the avatar.
    static bool handleKeyPan(const std::string& action, bool fast);

    /// A map drew the region maps this frame: they are captured while one keeps asking.
    static void noteMapsWanted();
    static bool mapsWanted();

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

    // SKModalMode
    const char* getModalName() const override { return "tactical overview"; }
    /// Escape, a teleport or RLVa turn the overview off.
    void closeModal() override;
    void onTopChanged(bool on_top) override;
    void renderModal(const LLRect& world_rect_raw) override;
    bool pickGround(S32 x, S32 y, LLVector3d& ground) const override;
    /// SKToolMapPan's drag: the ground point grabbed at the press stays under the cursor.
    bool beginPan(S32 x, S32 y) override;
    void pan(S32 x, S32 y) override;
    void endPan() override { mPan = Pan(); }
    /// The flycam holds the camera, so a drag cannot move it.
    bool canPan() const override;
    /// The overview is the mini-map writ large.
    std::vector<std::string> coveredFloaters() const override { return { "mini_map" }; }
    /// Clicks reach the pie, which picks the people and objects the overview draws.
    bool passesClicks() const override { return true; }
    /// The flycam's height above the agent over a person's scale, so it crosses the view quickly from high up.
    F64 flycamScale(const LLVector3d& eye) const override;

private:
    struct Shot
    {
        LLMatrix4a mView;
        LLMatrix4a mProj;
        LLCamera mCull;
        LLVector3 mEye;     // agent space
        F32 mCut = 0.f;     // agent-space z
        LLVector3 mFocus;   // agent space: what the view centres on; distance fade and height colours are relative to it
        bool mPartCuts = false; // each census part is cut at its own height (the tactical view), not at mCut
        F32 mLevelTop = 0.f; // the agent's cut height, the brightest the height colours go
        F32 mWater = 0.f;   // the water surface's height
        SKOverviewCensus::Level mLevel; // the census groups it draws
        LLVector4 mRegion;  // the region styled as the agent's: min x, min y, max x, max y, agent space
        std::vector<LLVector4> mTerritory;  // the regions styled as inside, same form; empty means mRegion alone
        bool mTopDown = false;
        std::vector<LLVector4> mDissolve;   // the see-through circles: window x, y, eye depth, radius in pixels
        std::vector<F32> mDissolveFloor;    // each one's floor, agent z
    };
    /// A region's top-down map and when it was last taken.
    struct RegionMap;

    void frame();
    void updateDemand(bool wanted);
    /// Caps Lock is taken from the OS while the tactical action is bound to it and the world has the keyboard.
    static void updateCapsLockCapture();
    /// The floor under the agent from the navmesh and the cut above it, or the agent's height and the old offset.
    void cutHeights(const LLVector3d& at, F32 fallback_z, F32& floor_z, F32& cut_z) const;
    /// What the tactical orbit centres on: the avatar, the alt-cam focus target, or the surface under a flycam.
    void tacticalCentre(LLVector3d& centre) const;
    bool ensureGL(LLRenderTarget& capture, LLRenderTarget& resolved, S32 w, S32 h);
    /// Whether every program loadShaders() builds is there; it builds nothing.
    bool ensureShaders();
    void releaseGL();
    /// The terrain and the census into capture; with a patch, only inside that window rect, over what is there.
    U32 render(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, bool resolve_now = true,
               const LLRect* patch = nullptr);
    /// The capture's depth and class codes to the styled map, whole or inside a rect.
    void resolve(LLRenderTarget& capture, LLRenderTarget& resolved, const Shot& shot, const LLRect* rect = nullptr);
    /// The work target at the tactical capture's size: false when it would not allocate.
    bool ensureWork();
    /// The movers into the work target, over the static scene restored under them.
    void drawMovers(const Shot& shot);
    /// The tactical work target brought up to this frame: the static capture with the see-through circles' patch and
    /// the movers, repaired and resolved by rect; static_new says the static capture was just rendered.
    void updateWork(const Shot& shot, bool static_new, U64 patch_key, const LLRect& patch_rect, bool movers);
    /// The avatar markers as their lit physics shapes, hidden by the census (a faint silhouette where they are), giving
    /// way zoomed out to the mini-map's flat markers; fills the marks and labels' anchors with each marker's top.
    template <typename Project>
    void drawMarkers3D(const Project& project, S32 vw, S32 vh, std::vector<std::pair<size_t, LLVector2>>& drawn);
    /// Captures the region maps that are due: the agent's on its timer, then the most overdue other one.
    void captureMaps(F64 now);
    bool captureMap(RegionMap& map, LLViewerRegion* region, bool agent);
    void tactical();
    void composite();
    bool rightClick(S32 x, S32 y);
    /// What the overview draws under a pixel, by the tactical camera's ray: the census as cut, terrain, else the agent's floor plane.
    bool groundAt(const LLVector2& at, LLVector3d& world) const;
    /// What a GPU span times; each is read a few frames on into Stats and the cost line.
    enum class SpanKind : U8
    {
        Map,
        Tactical,
        Patch,
        Movers,
        Resolve
    };
    /// Ends a span begun with gGL.queries().beginSpan() and keeps it to be read; a span that began nothing is dropped.
    void endSpan(SpanKind kind, ALQuerySpan& span);
    void readSpans();
    std::vector<std::pair<SpanKind, ALQuerySpan>> mSpans;

    SKOverviewCensus mCensus;
    SKOverviewMovers mMovers;
    LLRenderTarget* mSceneDepth = nullptr;   ///< the capture whose depth this frame's markers test against
    bool mWorkValid = false;    ///< the work target holds the static capture, with the rects below drawn over it
    LLRect mPatchRect;          ///< the window rect the circles' patch covers on the work target, empty for none
    LLRect mMoverRect;          ///< and the movers drawn last frame
    U64 mPatchKey = 0;          ///< the circles the patch was drawn for
    F32 mTacticalDistance = 0.f;
    std::vector<std::unique_ptr<RegionMap>> mMaps;
    size_t mCursor = 0;     // where the next round-robin turn starts in mMaps
    F64 mNextSlot = 0.0;    // when the next turn may capture
    Stats mStats;
    F32 mDemandRange = 0.f;
    U32 mClassMask = 0;     // the layers the region maps were last styled with
    bool mGLFailed = false;
    bool mDemanding = false;
    LLMatrix4a mTacticalView;
    LLMatrix4a mTacticalProj;
    S32 mTacticalViewport[4] = {};
    bool mTacticalReady = false;    ///< rendered this frame, for the composite
    bool mTacticalPickable = false; ///< the tactical view and viewport are a shown frame's, for clicks
    LLVector3 mTacticalCentre;   // agent space
    F32 mTacticalReach = 0.f;
    F32 mTacticalShadeZ = 0.f;      ///< agent-space z the height colours are relative to: the agent's floor, eased
    F64 mTacticalShadeTime = -1.0;  ///< when it was last eased; negative until the first shot
    /// A person's see-through circle on the tactical view, eased in and out so it never pops.
    struct Dissolve
    {
        LLVector3 mFloor;       ///< agent space, their feet
        F32 mAmount = 0.f;      ///< 0 closed, 1 open
        bool mSelf = false;
    };
    std::map<LLUUID, Dissolve> mDissolves;
    F64 mDissolveTime = -1.0;
    /// Fills the shot's see-through circles round the agent and the people the overview marks, and the window rect they
    /// cover; the key of what it drew, which moves the patch, never the static capture.
    U64 tacticalDissolve(Shot& shot, S32 w, S32 h, LLRect& rect);
    bool mTacticalOpen = false;     ///< claims the modal view
    /// Claims or releases the modal view to match SKTacticalOverview.
    static void syncTactical();
    void openTactical();
    void closeTactical();
    /// Puts the viewer camera at the overview's settings: tilted over the agent, looking the way the camera did.
    void placeTacticalCamera(bool over_other_mode);
    /// Battlefield mode: a fixed north-up camera over the region's centre, far enough to fit it in the window.
    void placeBattlefieldCamera();
    /// Puts the viewer camera there at once, without the ease LLAgentCamera gives a focus change.
    void setTacticalCamera(const LLVector3d& eye, const LLVector3d& focus);
    /// The agent's region, and with SKTacticalBattlefieldNeighbours the connected regions touching it, global.
    bool battlefieldBounds(LLVector3d& lo, LLVector3d& hi) const;
    /// A drag over the overview, from the press SKToolMapPan began it with.
    struct Pan
    {
        bool mActive = false;
        LLVector3 mGrab;        // agent space
        LLMatrix4a mView;       // the tactical camera at the press
        LLMatrix4a mProj;
        S32 mViewport[4] = {};
        LLVector3d mEye;
        LLVector3d mFocus;
    } mPan;
    /// The movement keys held this frame, as forward and left steps on the ground, applied and cleared each frame.
    void applyKeyPan();
    F32 mKeyForward = 0.f;
    F32 mKeyLeft = 0.f;
    bool mKeyFast = false;
    /// Works out the per-building cut (SKOverviewCensus::applyCut) for the agent and the people the overview marks.
    void updateCut();
    /// An enclosed room's floor polys' bounds, global, cached a few seconds.
    /// The enclosed room around a floor point, flooded through the indoor air at that floor: its air cells on the
    /// ROOM_CELL_M lattice; null when the point is not indoors. Rooms are kept a few seconds and reused while the
    /// point stays in one.
    std::shared_ptr<const std::set<std::pair<S32, S32>>> roomAt(const LLVector3d& floor);
    struct Room
    {
        F64 mAt = 0.0;
        F64 mFloor = 0.0;
        U64 mRevision = 0;      ///< the world view's, so a rebuilt or teleported-from room is flooded afresh
        U64 mRegion = 0;
        std::shared_ptr<std::set<std::pair<S32, S32>>> mCells;
    };
    std::vector<Room> mRooms;
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
    const SKNavWorldView::AirColumn* columnOf(const SKNavWorldViewPtr& world, const LLUUID& id, const LLVector3d& feet, F64 now);
    struct HeldColumn
    {
        F64 mAt = -1.0;
        U64 mRevision = 0;
        LLVector3d mXY;
        SKNavWorldView::AirColumn mColumn;
    };
    std::map<LLUUID, HeldColumn> mColumns;
    /// What the last tactical capture showed: the same again reuses its image.
    struct TacticalKey
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
        const void* mRegion = nullptr;
    } mTacticalKey;
    /// CPU time of the tactical view, summed over a second and logged.
    struct Cost
    {
        F64 mSince = 0.0;
        F32 mCaptureMs = 0.f;
        F32 mResolveMs = 0.f;
        F32 mCompositeMs = 0.f;
        F32 mGpuTacticalMs = 0.f;   // GPU spans read in this second, summed
        F32 mGpuPatchMs = 0.f;
        F32 mGpuMoversMs = 0.f;
        F32 mGpuResolveMs = 0.f;
        U32 mFrames = 0;
        U32 mRenders = 0;
        U32 mReuses = 0;
        U32 mScansAtStart = 0;
    } mCost;
    struct TacticalMark
    {
        LLUUID mId;
        LLVector2 mAt;      // overview pixels
    };
    std::map<LLUUID, F32> mFlatBlend;       // each marker's crossfade from its body (0) to its flat marker (1)
    F64 mMarkerFadeTime = -1.0;
    std::vector<TacticalMark> mTacticalMarks;  // the markers drawn last frame, for picks
    // The mini-map's hover nametags, laid out in UI pixels over the overview.
    /// tagged runs alongside drawn: an avatar there shows its own name tag instead of the marker label.
    void drawTacticalLabels(const std::vector<std::pair<size_t, LLVector2>>& drawn, const std::vector<LLVOAvatar*>& tagged,
                            S32 vw, S32 vh);
    skmap::HoverSet mTacticalHover;
    skmap::LabelLayout mTacticalLabels;
    std::vector<skmapdraw::TagText> mTacticalTagTexts;
    bool mTacticalEscDown = false;
    LLHandle<SKPopupPanes> mPopup;

};

#endif // SK_OVERVIEWCAPTURE_H
