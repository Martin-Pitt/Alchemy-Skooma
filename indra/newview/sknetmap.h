/**
 * @file sknetmap.h
 * @brief The mini-map widget: view state, input and the draw, over the navmesh floorplan.
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

#ifndef SK_NETMAP_H
#define SK_NETMAP_H

#include "lluictrl.h"
#include "llpointer.h"
#include "lluuid.h"

#include <boost/signals2.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "skmaplabels.h"
#include "skmappalette.h"
#include "skmappeople.h"
#include "skmappolicy.h"
#include "skmapview.h"

class LLFontGL;
class LLViewerTexture;
class SKMapComposite;
class SKMapObjects;
class SKMapParcels;
class SKPopupPanes;

/// Labels shared by every view that draws the mini-map's people: the codes on their markers and the hover nametags.
namespace skmapdraw
{
    /// What one nametag says.
    struct TagText
    {
        LLUUID mId;
        std::string mCode;
        std::string mName;
        std::string mDetail;
        F32 mWidth = 0.f;
        F32 mHeight = 0.f;
    };
    /// The second line a tag gives someone on another floor or an unknown one.
    struct FloorLabels
    {
        std::string mAbove;
        std::string mBelow;
        std::string mUnknown;
    };

    /// The font the markers' codes are drawn in, at SKMapMarkerFontPx.
    const LLFontGL* codeFont();
    /// A marker's radius in pixels, grown to hold the person's code when SKMapMarkerLabels shows it.
    F32 markerRadius(const SKMapPeople::Person& p, skmap::FloorRelation floor);
    /// The person's code centred on their marker when it shows there, in the current UI space; outside any batch.
    void personCode(const SKMapPeople::Person& p, const LLVector2& at, skmap::FloorRelation floor);
    /// The tags for the hovered people in hover order, each at most widest pixels; floor_of gives a member's storey.
    std::vector<TagText> tagTexts(const std::vector<LLUUID>& members, const std::function<skmap::FloorRelation(const LLUUID&)>& floor_of,
                                  const FloorLabels& floors, F32 widest);
    /// One nametag in rect r, in the current UI space.
    void drawTag(const LLRect& r, const TagText& t);
}

class SKNetMap : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKNetMap, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        /// The nametag's second line for someone on another floor.
        Optional<std::string> floor_above_label;
        Optional<std::string> floor_below_label;
        Optional<std::string> floor_unknown_label;

        Params()
            : floor_above_label("floor_above_label", "Floor above"),
              floor_below_label("floor_below_label", "Floor below"),
              floor_unknown_label("floor_unknown_label", "Floor unknown")
        {
        }
    };

protected:
    SKNetMap(const Params& p);
    friend class LLUICtrlFactory;

public:
    ~SKNetMap() override;

    void draw() override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseEnter(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    bool isHovered() const { return mHovered; }
    /// Forget the pointer, for when the map stops taking it without a mouse-leave.
    void clearHover() { mHovered = false; }
    /// Whether nametags show; they bring the chrome with them.
    bool namesShown() const { return !mHover.empty(); }
    /// Whether the right-click panes are open.
    bool panesOpen() const;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    /// The floater decides; the map draws the tint, border and grip.
    void setChrome(bool chrome) { mChrome = chrome; }

    const skmap::MapView& view() const { return mView; }

    /// The Mouselook map's layers, each drawn into its own target so each can fade on its own.
    enum class OverlayLayer
    {
        PLAN,
        MARKERS,
        SELF
    };
    /// Draw one layer around the camera, span metres across a side px square; PLAN sets the view first and the
    /// next draw() puts the widget's own view back.
    void drawOverlay(OverlayLayer layer, F32 span, F32 rotation, F32 side, F32 lines, F32 surface);
    /// Free the plan's GL objects; they are made again on the next draw.
    void releaseGL();

    /// Told of every ping the user places on a map, for whatever will carry it to allies.
    typedef boost::signals2::signal<void(const LLVector3d&)> ping_signal_t;
    static boost::signals2::connection setPingCallback(const ping_signal_t::slot_type& cb);
    static void firePing(const LLVector3d& pos);
    /// A combat event to draw: a line from actor to target, and a cross for a kill or a death.
    static void addEvent(const SKCombatEvent& event);
    /// Ease the pan back to the agent over a quarter second.
    void recentre();
    static bool followsAvatar();
    static LLVector3d anchorGlobal();
    bool isPanned() const { return mPan.lengthSquared() > 1e-4f; }

    /// The widget's short side in pixels, for seeding the zoom from the classic scale.
    F32 shortSide() const { return static_cast<F32>(llmin(getRect().getWidth(), getRect().getHeight())); }

    /// A person's dot as drawn this frame, in widget pixels.
    struct DrawnDot
    {
        LLUUID mId;
        LLVector2 mPos;
        F32 mRadius = 3.f;
        size_t mPerson = 0;     ///< its index in SKMapPeople::people()
    };
    const std::vector<DrawnDot>& drawnDots() const { return mDots; }

private:
    /// The view for this frame: the agent plus the pan, the zoom, the rotation.
    void updateView();
    void drawPlan();
    /// The top-down capture as the base, in place of terrain, objects and floorplan; false when it is off or not ready.
    bool drawCaptureBase(F32 alpha);
    void drawTerrain();
    void drawSelf();
    void drawSelfExtras();
    void drawPeople();
    /// Chevrons on the edge pointing to people off the map, on a circle of radius mEdgeCircle when it is set.
    void drawEdgeChevrons();
    /// The allies' codes on their dots, drawn after the classic matrix block as all text is.
    void drawPeopleCodes();
    void drawVehicles();
    void drawPings();
    void drawEvents();
    void drawFiringLines();
    void drawProjectiles();
    void drawTracking();
    void drawTrackingAt(const LLVector3d& pos, const LLColor4& colour);
    void drawNorth();
    /// The N glyph or the compass letters, drawn after the classic matrix block.
    void drawCombatNotice();
    void drawNorthText();
    void drawChromeTint();
    void drawChromeFrame();
    void drawReadout();
    /// Every person's dot that shows in the widget this frame, before anything is drawn.
    void collectDots();
    /// Who is under the pointer, and where their tags go.
    void updateHover();
    void drawConnectors();
    void drawTags();
    /// A tag rectangle from the solver's window pixels, y down, to this widget's pixels.
    LLRect tagRect(const skmap::LabelLayout::Tag& tag) const;
    /// The panes closed: the frozen people go, and names stay off until the pointer moves.
    void onPanesClosed();
    /// Within a few pixels of the open panes, where the tooltip keeps out of their way.
    bool mouseNearPanes() const;

    void drawObjects();

    std::unique_ptr<SKMapObjects> mObjects;
    std::unique_ptr<SKMapComposite> mPlan;
    std::unique_ptr<SKMapParcels> mParcels;
    std::vector<DrawnDot> mDots;
    std::vector<skmap::FloorRelation> mFloorOf;    ///< per person of SKMapPeople::people(), set by collectDots
    F32 mEdgeCircle = 0.f;      ///< the mouselook overlay's visible radius, 0 for the floater's rectangle
    std::vector<skmap::EventRing::Draw> mEventDraws;   ///< reused each frame
    bool mHovered = false;
    bool mChrome = false;
    skmap::HoverSet mHover;
    skmap::LabelLayout mLabels;
    bool mEscDown = false;
    using TagText = skmapdraw::TagText;
    std::vector<TagText> mTagTexts;
    LLHandle<SKPopupPanes> mPopup;
    LLVector3d mPopupWorldPos;
    std::string mFloorAboveLabel;
    std::string mFloorBelowLabel;
    std::string mFloorUnknownLabel;
    F32 mDrawMs = 0.f;

    skmap::MapView mView;
    LLVector2 mPan;                     ///< metres east and north of the agent
    LLVector2 mEaseFrom;
    F64 mEaseStart = -1.0;
    bool mPanning = false;
    LLCoordGL mMouseDown;
};

/// Small draw helpers shared by the mini-map's layers, in widget pixels.
namespace skmapdraw
{
    void colour(const skmap::PaletteColour& c, F32 alpha = 1.f);
    void triangle(const LLVector2& a, const LLVector2& b, const LLVector2& c);
    void line(const LLVector2& a, const LLVector2& b, F32 width);
    /// A thick segment fading from opaque at the head to clear at the tail.
    void streak(const LLVector2& tail, const LLVector2& head, F32 width, const LLColor4& colour);
    /// A projectile's streak in widget pixels: its physical length and width, held within the settings' pixel limits.
    void projectile(const LLVector2& head, const LLVector2& dir, F32 length_px, F32 width_px, const LLColor4& colour);
    void disc(const LLVector2& centre, F32 radius, S32 steps = 16);
    void ring(const LLVector2& centre, F32 radius, F32 width, S32 steps = 32);
    /// The soft gaussian marker shadow, generated once.
    LLViewerTexture* shadowSprite();
    void shadow(const LLVector2& centre, F32 radius);
    void releaseTextures();
    /// One person's marker inside a TRIANGLES batch: a disc in their colour (cyan for coded allies in combat), the
    /// heading triangle along ahead (a unit vector in the same space; none when null) and the floor chevron.
    void personMarker(const SKMapPeople::Person& p, const LLVector2& at, F32 radius, const LLVector2* ahead,
                      skmap::FloorRelation floor, F32 font_px);
    /// A marker's radius in pixels for a person drawn without a code label.
    F32 plainMarkerRadius(skmap::FloorRelation floor);
    /// One layer of a vehicle's captured outline as a ground quad: its atlas texture, its corners as plan offsets in
    /// metres from the vehicle's live position (x east, y north), in the order of uv.
    struct GlyphQuad
    {
        U32 mTexture = 0;
        LLVector2 mCorner[4];
        F32 mUV[4][2];
    };
    /// The vehicle's glyph layers at their live pose; false when it has no drawable glyph (SKMapVehicleGlyphs off,
    /// none captured yet, or its base layer is not posed by the root).
    bool vehicleGlyphQuads(const SKMapPeople::Vehicle& v, std::vector<GlyphQuad>& out);
    /// A vehicle's hull and top colours by its look, as the mini-map draws them.
    const skmap::PaletteColour& vehicleHull(skmap::VehicleLook look);
    const skmap::PaletteColour& vehicleTop(skmap::VehicleLook look);
}

#endif // SK_NETMAP_H
