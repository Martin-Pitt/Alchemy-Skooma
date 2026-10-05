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

#include <memory>
#include <vector>

#include "skmaplabels.h"
#include "skmappalette.h"
#include "skmapview.h"

class LLViewerTexture;
class SKMapObjects;
class SKMapParcels;
class SKPopupPanes;

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
    /// Whether nametags show; they bring the chrome with them.
    bool namesShown() const { return !mHover.empty(); }
    /// Whether the right-click panes are open.
    bool panesOpen() const;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    /// The floater decides; the map draws the tint, border and grip.
    void setChrome(bool chrome) { mChrome = chrome; }

    const skmap::MapView& view() const { return mView; }
    /// Ease the pan back to the agent over a quarter second.
    void recentre();
    bool isPanned() const { return mPan.lengthSquared() > 1e-4f; }

    /// The widget's short side in pixels, for seeding the zoom from the classic scale.
    F32 shortSide() const { return static_cast<F32>(llmin(getRect().getWidth(), getRect().getHeight())); }

    /// A person's dot as drawn this frame, in widget pixels.
    struct DrawnDot
    {
        LLUUID mId;
        LLVector2 mPos;
        F32 mRadius = 3.f;
    };
    const std::vector<DrawnDot>& drawnDots() const { return mDots; }

private:
    /// The view for this frame: the agent plus the pan, the zoom, the rotation.
    void updateView();
    void drawTerrain();
    void drawSelf();
    void drawSelfExtras();
    void drawPeople();
    void drawTracking();
    void drawTrackingAt(const LLVector3d& pos, const LLColor4& colour);
    void drawNorth();
    void drawChromeTint();
    void drawChromeFrame();
    void drawReadout();
    /// Every drawn person's dot this frame, before anything is drawn.
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
    std::unique_ptr<SKMapParcels> mParcels;
    std::vector<DrawnDot> mDots;
    bool mHovered = false;
    bool mChrome = false;
    skmap::HoverSet mHover;
    skmap::LabelLayout mLabels;
    bool mEscDown = false;
    /// What each shown tag says, in hover order.
    struct TagText
    {
        LLUUID mId;
        std::string mCode;
        std::string mName;
        std::string mDetail;
        F32 mWidth = 0.f;
        F32 mHeight = 0.f;
    };
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
    void disc(const LLVector2& centre, F32 radius, S32 steps = 16);
    void ring(const LLVector2& centre, F32 radius, F32 width, S32 steps = 32);
    /// The soft gaussian marker shadow, generated once.
    LLViewerTexture* shadowSprite();
    void shadow(const LLVector2& centre, F32 radius);
    void releaseTextures();
}

#endif // SK_NETMAP_H
