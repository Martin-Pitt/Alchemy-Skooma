/**
 * @file sknetmap.cpp
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

#include "llviewerprecompiledheaders.h"

#include "sknetmap.h"

#include "skoverviewcapture.h"

#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llresmgr.h"
#include "llrootview.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "llurlmatch.h"
#include "llurlregistry.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcallingcard.h"
#include "llfloaterworldmap.h"
#include "llsurface.h"
#include "lltracker.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "llvoavatarself.h"
#include "llworldmapview.h"
#include "llavatarnamecache.h"
#include "llkeyboard.h"
#include "llparcel.h"
#include "llslurl.h"
#include "llviewerparcelmgr.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "skmapmarkers.h"
#include "skmapparcels.h"
#include "skmappeople.h"
#include "alavatargroups.h"
#include "skmapmenu.h"
#include "skmapscene.h"
#include "skpopuppanes.h"
#include "skworldcensus.h"

static LLDefaultChildRegistry::Register<SKNetMap> r("sk_net_map");

namespace
{
    constexpr S32 MOUSE_DRAG_SLOP = 2;
    constexpr F64 RECENTRE_S = 0.25;

    LLPointer<LLViewerTexture> sShadowSprite;

    LLColor4 toColor4(const skmap::PaletteColour& c, F32 alpha = 1.f)
    {
        return LLColor4(c.mR, c.mG, c.mB, c.mA * alpha);
    }

    // Half the avatar's wider horizontal extent on the map, so a close zoom draws its true footprint.
    F32 footprintRadius(const LLViewerObject* avatar, F32 pixels_per_metre)
    {
        if (!avatar || !avatar->isAvatar())
        {
            return 0.f;
        }
        const LLVector3& scale = avatar->getScale();
        return 0.5f * llmax(scale.mV[VX], scale.mV[VY]) * pixels_per_metre;
    }
}

namespace skmapdraw
{
    void colour(const skmap::PaletteColour& c, F32 alpha)
    {
        gGL.color4f(c.mR, c.mG, c.mB, c.mA * alpha);
    }

    void triangle(const LLVector2& a, const LLVector2& b, const LLVector2& c)
    {
        gGL.vertex2f(a.mV[VX], a.mV[VY]);
        gGL.vertex2f(b.mV[VX], b.mV[VY]);
        gGL.vertex2f(c.mV[VX], c.mV[VY]);
    }

    void line(const LLVector2& a, const LLVector2& b, F32 width)
    {
        LLVector2 d = b - a;
        if (d.normVec() < 1e-4f)
        {
            return;
        }
        const LLVector2 n(-d.mV[VY] * width * 0.5f, d.mV[VX] * width * 0.5f);
        triangle(a - n, b - n, b + n);
        triangle(a - n, b + n, a + n);
    }

    void disc(const LLVector2& centre, F32 radius, S32 steps)
    {
        for (S32 i = 0; i < steps; ++i)
        {
            const F32 a0 = F_TWO_PI * i / steps, a1 = F_TWO_PI * (i + 1) / steps;
            triangle(centre, centre + LLVector2(std::cos(a0), std::sin(a0)) * radius, centre + LLVector2(std::cos(a1), std::sin(a1)) * radius);
        }
    }

    void ring(const LLVector2& centre, F32 radius, F32 width, S32 steps)
    {
        const F32 inner = llmax(0.f, radius - width * 0.5f), outer = radius + width * 0.5f;
        for (S32 i = 0; i < steps; ++i)
        {
            const F32 a0 = F_TWO_PI * i / steps, a1 = F_TWO_PI * (i + 1) / steps;
            const LLVector2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
            triangle(centre + d0 * inner, centre + d0 * outer, centre + d1 * outer);
            triangle(centre + d0 * inner, centre + d1 * outer, centre + d1 * inner);
        }
    }

    void personMarker(const SKMapPeople::Person& p, const LLVector2& at, F32 radius, const LLVector2* ahead,
                      skmap::FloorRelation floor, F32 font_px)
    {
        static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
        const bool chevron = chevrons && (floor == skmap::FloorRelation::ABOVE || floor == skmap::FloorRelation::BELOW);
        // Allies in combat are cyan; everyone else keeps the general colours.
        if (p.mVisibility == skmap::Visibility::DOT_CODED)
        {
            colour(skmap::palette::DOT_COMBAT);
        }
        else
        {
            gGL.color4f(p.mColour.mV[0], p.mColour.mV[1], p.mColour.mV[2], 1.f);
        }
        if (ahead)
        {
            const LLVector2 across(-ahead->mV[VY], ahead->mV[VX]);
            const LLVector2 base = at + *ahead * (radius + skmap::palette::HEADING_BASE_PX);
            triangle(at + *ahead * (radius + skmap::palette::HEADING_TIP_PX), base + across * skmap::palette::HEADING_HALF_PX,
                     base - across * skmap::palette::HEADING_HALF_PX);
        }
        disc(at, radius);
        if (chevron)
        {
            // Upright whatever the rotation: up for the floor above, down for the one below.
            const F32 half = llmin(radius - skmap::palette::CHEVRON_STROKE_PX, font_px * 0.3f);
            const F32 dy = half * 0.55f;
            const F32 sign = floor == skmap::FloorRelation::ABOVE ? 1.f : -1.f;
            colour(skmap::palette::CODE_INK);
            const LLVector2 left = at + LLVector2(-half, -sign * dy), apex = at + LLVector2(0.f, sign * dy), right = at + LLVector2(half, -sign * dy);
            line(left, apex, skmap::palette::CHEVRON_STROKE_PX);
            line(apex, right, skmap::palette::CHEVRON_STROKE_PX);
        }
    }

    F32 plainMarkerRadius(skmap::FloorRelation floor)
    {
        static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
        static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
        static LLCachedControl<F32> padding(gSavedSettings, "SKMapMarkerPaddingPx", 0.5f);
        const bool chevron = chevrons && (floor == skmap::FloorRelation::ABOVE || floor == skmap::FloorRelation::BELOW);
        return skmap::markerRadius(chevron, false, 0, 0.f, llclamp(static_cast<F32>(font_px), 8.f, 11.f),
                                   llclamp(static_cast<F32>(padding), 0.f, 3.f));
    }

    bool vehicleGlyphQuads(const SKMapPeople::Vehicle& v, std::vector<GlyphQuad>& out)
    {
        static LLCachedControl<bool> glyphs(gSavedSettings, "SKMapVehicleGlyphs", true);
        out.clear();
        auto drawable = [](const SKOutlineLayer& layer) { return layer.texture && layer.x1 > layer.x0 && layer.y1 > layer.y0; };
        const SKMapVehicleGlyphs::Glyph* glyph = glyphs ? SKMapVehicleGlyphs::glyphFor(v.mRoot) : nullptr;
        if (!glyph || glyph->layers.empty() || glyph->layers.front().pose_source != SKOutlineLayer::PoseSource::ROOT ||
            !drawable(glyph->layers.front()))
        {
            return false;
        }
        const LLViewerObject* root = gObjectList.findObject(v.mRoot);
        for (const SKOutlineLayer& layer : glyph->layers)
        {
            if (!drawable(layer))
            {
                continue;
            }
            // The layer frame in the root's frame, plan view: a yaw and an x/y offset.
            LLVector3 rel_pos;
            LLQuaternion rel_rot;
            if (layer.pose_source == SKOutlineLayer::PoseSource::CREW_ATTACHMENT)
            {
                const LLViewerObject* attachment = root ? gObjectList.findObject(layer.pose_id) : nullptr;
                if (attachment && !attachment->isDead() && attachment->mDrawable.notNull())
                {
                    const LLQuaternion root_inv = ~root->getRenderRotation();
                    rel_pos = (attachment->getRenderPosition() - root->getRenderPosition()) * root_inv;
                    rel_rot = attachment->getRenderRotation() * root_inv;
                }
                else
                {
                    rel_pos = layer.rel_pos;
                    rel_rot = layer.rel_rot;
                }
            }
            const LLVector3 fwd = LLVector3::x_axis * rel_rot;
            const F32 yaw = atan2f(fwd.mV[VY], fwd.mV[VX]);
            const F32 cy = cosf(yaw), sy = sinf(yaw);
            auto at = [&](F32 x, F32 y) {
                const LLVector3 in_root(x * cy - y * sy + rel_pos.mV[VX], x * sy + y * cy + rel_pos.mV[VY], 0.f);
                const LLVector3 p = in_root * v.mRot;
                return LLVector2(p.mV[VX], p.mV[VY]);
            };
            GlyphQuad q;
            q.mTexture = layer.texture;
            q.mCorner[0] = at(layer.x0, layer.y0);
            q.mCorner[1] = at(layer.x1, layer.y0);
            q.mCorner[2] = at(layer.x1, layer.y1);
            q.mCorner[3] = at(layer.x0, layer.y1);
            const F32 uv[4][2] = { { layer.u0, layer.v0 }, { layer.u1, layer.v0 }, { layer.u1, layer.v1 }, { layer.u0, layer.v1 } };
            memcpy(q.mUV, uv, sizeof(uv));
            out.push_back(q);
        }
        return !out.empty();
    }

    const skmap::PaletteColour& vehicleHull(skmap::VehicleLook look)
    {
        return look == skmap::VehicleLook::ALLY      ? skmap::palette::VEH_ALLY_HULL
             : look == skmap::VehicleLook::NEUTRAL   ? skmap::palette::VEH_NEUTRAL_HULL
                                                     : skmap::palette::VEH_CIVIL_HULL;
    }

    const skmap::PaletteColour& vehicleTop(skmap::VehicleLook look)
    {
        return look == skmap::VehicleLook::ALLY      ? skmap::palette::VEH_ALLY_TOP
             : look == skmap::VehicleLook::NEUTRAL   ? skmap::palette::VEH_NEUTRAL_TOP
                                                     : skmap::palette::VEH_CIVIL_TOP;
    }

    LLViewerTexture* shadowSprite()
    {
        if (sShadowSprite.isNull())
        {
            // A gaussian disc whose falloff is the palette's blur, black with the shadow's alpha.
            const S32 n = skmap::palette::SHADOW_SPRITE_PX;
            LLPointer<LLImageRaw> raw = new LLImageRaw(n, n, 4);
            U8* data = raw->getData();
            const F32 sigma = n / 6.f;
            for (S32 y = 0; y < n; ++y)
            {
                for (S32 x = 0; x < n; ++x)
                {
                    const F32 dx = x + 0.5f - n * 0.5f, dy = y + 0.5f - n * 0.5f;
                    const F32 a = std::exp(-(dx * dx + dy * dy) / (2.f * sigma * sigma));
                    U8* p = &data[(y * n + x) * 4];
                    p[0] = p[1] = p[2] = 0;
                    p[3] = static_cast<U8>(llclamp(a, 0.f, 1.f) * 255.f);
                }
            }
            sShadowSprite = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        }
        return sShadowSprite;
    }

    /// One shadow quad, inside a TRIANGLES batch with the sprite bound.
    void shadowQuad(const LLVector2& centre, F32 radius)
    {
        const F32 r = radius + skmap::palette::SHADOW_BLUR_PX;
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(centre.mV[VX] - r, centre.mV[VY] - r);
        gGL.texCoord2f(1.f, 0.f);
        gGL.vertex2f(centre.mV[VX] + r, centre.mV[VY] - r);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(centre.mV[VX] + r, centre.mV[VY] + r);
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(centre.mV[VX] - r, centre.mV[VY] - r);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(centre.mV[VX] + r, centre.mV[VY] + r);
        gGL.texCoord2f(0.f, 1.f);
        gGL.vertex2f(centre.mV[VX] - r, centre.mV[VY] + r);
    }

    /// Every shadow in one batch: one bind, and no shadow darkens a disc drawn before it.
    template<typename Each>
    void shadows(Each&& each)
    {
        LLViewerTexture* sprite = shadowSprite();
        if (!sprite)
        {
            return;
        }
        gGL.getTextureSlot(0)->bindSampled(sprite, ALSamplers::BilinearClamp);
        colour(skmap::palette::SHADOW);
        gGL.begin(LLRender::TRIANGLES);
        each();
        gGL.end();
        gGL.getTextureSlot(0)->unbind();
    }

    void shadow(const LLVector2& centre, F32 radius)
    {
        shadows([&]() { shadowQuad(centre, radius); });
    }

    void releaseTextures()
    {
        sShadowSprite = nullptr;
    }
}

SKNetMap::SKNetMap(const Params& p)
    : LLUICtrl(p),
      mParcels(std::make_unique<SKMapParcels>()),
      mFloorAboveLabel(p.floor_above_label),
      mFloorBelowLabel(p.floor_below_label),
      mFloorUnknownLabel(p.floor_unknown_label)
{
}

SKNetMap::~SKNetMap()
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    skmapdraw::releaseTextures();
}

void SKNetMap::updateView()
{
    static LLCachedControl<F32> span(gSavedSettings, "SKMapZoomMeters", skmap::ZOOM_DEFAULT_M);

    if (mEaseStart >= 0.0)
    {
        const F64 t = (LLFrameTimer::getUptimeSeconds().value() - mEaseStart) / RECENTRE_S;
        if (t >= 1.0 || mPanning)
        {
            mPan = mPanning ? mPan : LLVector2::zero;
            mEaseStart = -1.0;
        }
        else
        {
            const F32 k = static_cast<F32>(1.0 - t);
            mPan = mEaseFrom * (k * k * (3.f - 2.f * k));
        }
    }

    if (regionMode())
    {
        // North up over the agent's region, its whole width and a margin of the neighbours around it.
        static LLCachedControl<F32> margin(gSavedSettings, "SKMapRegionMargin", 0.1f);
        const LLViewerRegion* region = gAgent.getRegion();
        if (region->getHandle() != mRegionHandle)
        {
            mRegionHandle = region->getHandle();
            refitRegion();
        }
        const F32 fit = region->getWidth() * (1.f + 2.f * llclamp((F32)margin, 0.05f, 0.2f));
        const LLVector3d centre = viewAnchor() + LLVector3d(mPan.mV[VX], mPan.mV[VY], 0.0);
        mView.set(centre, mRegionSpan > 0.f ? mRegionSpan : fit, 0.f, static_cast<F32>(getRect().getWidth()),
                  static_cast<F32>(getRect().getHeight()));
        return;
    }
    if (mRegionHandle)
    {
        // Leaving region mode drops its pan and zoom.
        refitRegion();
        mRegionHandle = 0;
    }

    // The map centres on its anchor and turns so the camera's facing is up.
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 rotation = skmap::MapView::rotationForHeading(std::atan2(at.mV[VY], at.mV[VX]));
    const LLVector3d centre = anchorGlobal() + LLVector3d(mPan.mV[VX], mPan.mV[VY], 0.0);
    mView.set(centre, span, rotation, static_cast<F32>(getRect().getWidth()), static_cast<F32>(getRect().getHeight()));
}

// The camera follows the avatar unless an alt-cam or the flycam has taken it away.
bool SKNetMap::followsAvatar()
{
    return gAgentCamera.cameraMouselook() || (gAgentCamera.getFocusOnAvatar() && !LLViewerJoystick::getInstance()->getOverrideCamera());
}

// The avatar while the camera follows it, so a shoulder offset does not shift the map; the camera once detached.
LLVector3d SKNetMap::anchorGlobal()
{
    return followsAvatar() ? gAgent.getPositionGlobal() : gAgentCamera.getCameraPositionGlobal();
}

// static
bool SKNetMap::regionMode()
{
    static LLCachedControl<U32> mode(gSavedSettings, "SKMapViewMode", 0);
    return mode == 1;
}

LLVector3d SKNetMap::viewAnchor() const
{
    const LLViewerRegion* region = regionMode() ? gAgent.getRegion() : nullptr;
    if (!region)
    {
        return anchorGlobal();
    }
    const F64 half = region->getWidth() * 0.5;
    return region->getOriginGlobal() + LLVector3d(half, half, 0.0);
}

void SKNetMap::refitRegion()
{
    mPan.setZero();
    mEaseStart = -1.0;
    mRegionSpan = 0.f;
}

void SKNetMap::recentre()
{
    if (isPanned())
    {
        mEaseFrom = mPan;
        mEaseStart = LLFrameTimer::getUptimeSeconds().value();
    }
}

void SKNetMap::draw()
{
    if (!LLWorld::instanceExists() || !gAgent.getRegion())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    LLTimer draw_timer;
    updateView();
    SKMapPeople::instance().noteDrawn(mView);
    SKMapScene::instance().noteDrawn(*this);

    // The classic map's matrix setup: widget pixels, the UI scale and offset applied by hand.
    gGL.pushMatrix();
    gGL.pushUIMatrix();
    const LLVector3 offset = gGL.getUITranslation();
    const LLVector3 scale = gGL.getUIScale();
    gGL.loadIdentity();
    gGL.loadUIIdentity();
    gGL.scalef(scale.mV[0], scale.mV[1], scale.mV[2]);
    gGL.translatef(offset.mV[0], offset.mV[1], offset.mV[2]);
    {
        LLLocalClipRect clip(getLocalRect());
        gGL.getTextureSlot(0)->unbind();
        drawChromeTint();
        static LLCachedControl<bool> floorplan(gSavedSettings, "SKMapLayerFloorplan", true);
        if (floorplan)
        {
            drawCaptureBase(1.f);
        }
        drawElevators();
        mParcels->draw(mView);
        drawEvents();
        drawPings();
        drawProjectiles();
        drawFiringLines();
        drawVehicles();
        drawSelf();
        drawSelfExtras();
    }
    // Connectors run under the dots, and may leave the map for tags outside it.
    collectDots();
    updateHover();
    drawConnectors();
    {
        LLLocalClipRect clip(getLocalRect());
        drawPeople();
        mEdgeCircle = 0.f;
        drawEdgeChevrons();
        drawTracking();
        drawNorth();
        drawChromeFrame();
    }
    gGL.popMatrix();
    gGL.popUIMatrix();
    // Text after the classic block: LLFontGL adds the view's origin itself, so inside it every
    // glyph would land offset by the widget's own position.
    {
        LLLocalClipRect clip(getLocalRect());
        drawPeopleCodes();
        drawNorthText();
        drawCombatNotice();
    }
    // Tags sit wherever there is room in the window, over the map or the world.
    drawTags();
    mDrawMs = draw_timer.getElapsedTimeF32() * 1000.f;
    drawReadout();

    LLUICtrl::draw();
}

void SKNetMap::drawOverlay(OverlayLayer layer, F32 span, F32 rotation, F32 side, F32 surface)
{
    if (!LLWorld::instanceExists() || !gAgent.getRegion())
    {
        return;
    }
    switch (layer)
    {
        case OverlayLayer::PLAN:
        {
            static LLCachedControl<bool> show(gSavedSettings, "SKMapMLFloorplan", true);
            mView.set(anchorGlobal(), span, rotation, side, side);
            SKMapPeople::instance().noteDrawn(mView);
            SKMapScene::instance().noteDrawn(*this);
            if (show)
            {
                drawCaptureBase(surface);
            }
            drawElevators();
            break;
        }
        case OverlayLayer::MARKERS:
            drawEvents();
            drawPings();
            drawProjectiles();
            drawFiringLines();
            drawVehicles();
            collectDots();
            drawPeople();
            {
                // Inside the overlay's outer fade, where a marker still shows at about two thirds.
                static LLCachedControl<F32> outer_fade(gSavedSettings, "SKMapMLOuterFade", 0.38f);
                mEdgeCircle = side * 0.45f * (1.f - llclamp(static_cast<F32>(outer_fade), 0.05f, 1.f) * 0.65f);
                drawEdgeChevrons();
                mEdgeCircle = 0.f;
            }
            break;
        case OverlayLayer::SELF:
            drawSelf();
            break;
    }
}

bool SKNetMap::drawCaptureBase(F32 alpha)
{
    SKOverviewCapture::noteMapsWanted();
    if (!SKOverviewCapture::instanceExists())
    {
        return false;
    }
    const std::vector<SKOverviewCapture::Tile> tiles = SKOverviewCapture::instance().tiles();
    if (tiles.empty())
    {
        return false;
    }
    gGL.color4f(1.f, 1.f, 1.f, llclamp(alpha, 0.f, 1.f));
    for (const SKOverviewCapture::Tile& tile : tiles)
    {
        const LLVector3d origin = tile.mOrigin;
        const F64 w = tile.mWidth;
        const LLVector2 sw = mView.toWidget(origin);
        const LLVector2 se = mView.toWidget(origin + LLVector3d(w, 0.0, 0.0));
        const LLVector2 ne = mView.toWidget(origin + LLVector3d(w, w, 0.0));
        const LLVector2 nw = mView.toWidget(origin + LLVector3d(0.0, w, 0.0));
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, tile.mTexture, gGL.getSampler(ALSamplers::BilinearClamp));
        gGL.begin(LLRender::TRIANGLES);
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(sw.mV[VX], sw.mV[VY]);
        gGL.texCoord2f(1.f, 0.f);
        gGL.vertex2f(se.mV[VX], se.mV[VY]);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(ne.mV[VX], ne.mV[VY]);
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(sw.mV[VX], sw.mV[VY]);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(ne.mV[VX], ne.mV[VY]);
        gGL.texCoord2f(0.f, 1.f);
        gGL.vertex2f(nw.mV[VX], nw.mV[VY]);
        gGL.end();
    }
    gGL.getTextureSlot(0)->unbind();
    return true;
}

void SKNetMap::drawElevators()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerElevators", false);
    if (!show || !SKWorldCensus::instanceExists())
    {
        return;
    }
    const SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return;
    }
    // Each car's linkset bounds at its live pose: the census only says which roots are elevators.
    gGL.getTextureSlot(0)->unbind();
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::ELEVATOR);
    for (const sknav::Feature& feature : snap->mFeatures)
    {
        if (feature.mKind != sknav::FeatureKind::ELEVATOR)
        {
            continue;
        }
        for (const LLUUID& id : feature.mRoots)
        {
            LLViewerObject* root = gObjectList.findObject(id);
            if (!root || root->isDead())
            {
                continue;
            }
            LLVector3 lo(F32_MAX, F32_MAX, F32_MAX), hi(-F32_MAX, -F32_MAX, -F32_MAX);
            auto grow = [&lo, &hi](LLViewerObject* part)
            {
                const LLBBox box = part->getBoundingBoxAgent();
                const LLVector3 bmin = box.getMinAgent(), bmax = box.getMaxAgent();
                for (S32 k = 0; k < 3; ++k)
                {
                    lo.mV[k] = llmin(lo.mV[k], bmin.mV[k]);
                    hi.mV[k] = llmax(hi.mV[k], bmax.mV[k]);
                }
            };
            grow(root);
            for (LLViewerObject* child : root->getChildren())
            {
                grow(child);
            }
            const LLVector2 a = mView.toWidget(gAgent.getPosGlobalFromAgent(LLVector3(lo.mV[VX], lo.mV[VY], 0.f)));
            const LLVector2 b = mView.toWidget(gAgent.getPosGlobalFromAgent(LLVector3(hi.mV[VX], lo.mV[VY], 0.f)));
            const LLVector2 c = mView.toWidget(gAgent.getPosGlobalFromAgent(LLVector3(hi.mV[VX], hi.mV[VY], 0.f)));
            const LLVector2 d = mView.toWidget(gAgent.getPosGlobalFromAgent(LLVector3(lo.mV[VX], hi.mV[VY], 0.f)));
            const F32 w = skmap::palette::ELEVATOR_PX;
            skmapdraw::line(a, b, w);
            skmapdraw::line(b, c, w);
            skmapdraw::line(c, d, w);
            skmapdraw::line(d, a, w);
        }
    }
    gGL.end();
}

void SKNetMap::drawSelf()
{
    const LLVector2 eye = mView.toWidget(anchorGlobal());
    // The sight cone sits at the anchor and spans its horizontal field of view, a fixed size on screen.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 at = camera->getAtAxis();
    LLVector2 ahead = mView.toWidgetOffset(at.mV[VX], at.mV[VY]);
    if (ahead.normVec() > 1e-6f)
    {
        const F32 half = std::atan(std::tan(camera->getView() * 0.5f) * camera->getAspect());
        const F32 heading = std::atan2(ahead.mV[VY], ahead.mV[VX]);
        const S32 steps = llmax(4, static_cast<S32>(half * 2.f / (F_PI / 24.f)));
        const LLColor4 inner = toColor4(skmap::palette::SELF_CONE);
        const LLColor4 outer(inner.mV[0], inner.mV[1], inner.mV[2], 0.f);
        const F32 r = skmap::palette::SELF_CONE_PX;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < steps; ++i)
        {
            const F32 a0 = heading - half + 2.f * half * i / steps;
            const F32 a1 = heading - half + 2.f * half * (i + 1) / steps;
            gGL.color4fv(inner.mV);
            gGL.vertex2f(eye.mV[VX], eye.mV[VY]);
            gGL.color4fv(outer.mV);
            gGL.vertex2f(eye.mV[VX] + std::cos(a0) * r, eye.mV[VY] + std::sin(a0) * r);
            gGL.vertex2f(eye.mV[VX] + std::cos(a1) * r, eye.mV[VY] + std::sin(a1) * r);
        }
        gGL.end();
    }

    // Our avatar; when the camera has left it off the map, pinned to the edge with a triangle pointing to it.
    const LLVector2 self = mView.toWidget(gAgent.getPositionGlobal());
    const F32 inset = skmap::palette::SELF_EDGE_INSET_PX;
    const LLVector2 lo(inset, inset), hi(mView.width() - inset, mView.height() - inset);
    const bool off_map = self.mV[VX] < lo.mV[VX] || self.mV[VY] < lo.mV[VY] || self.mV[VX] > hi.mV[VX] || self.mV[VY] > hi.mV[VY];
    LLVector2 dot = self;
    if (off_map)
    {
        // Along the line from the map's centre, stopped at the inset edge.
        const LLVector2 mid(mView.width() * 0.5f, mView.height() * 0.5f);
        const LLVector2 d = self - mid;
        F32 t = 1.f;
        if (std::fabs(d.mV[VX]) > 1e-6f)
        {
            t = llmin(t, (hi.mV[VX] - mid.mV[VX]) / std::fabs(d.mV[VX]));
        }
        if (std::fabs(d.mV[VY]) > 1e-6f)
        {
            t = llmin(t, (hi.mV[VY] - mid.mV[VY]) / std::fabs(d.mV[VY]));
        }
        dot = mid + d * t;
    }
    const F32 dot_radius = llmax(skmap::palette::SELF_DOT_RADIUS_PX, footprintRadius(gAgentAvatarp, mView.pixelsPerMetre()));
    skmapdraw::shadow(dot, dot_radius);
    gGL.begin(LLRender::TRIANGLES);
    static LLCachedControl<bool> avatar_colours(gSavedSettings, "SKMapAvatarColors", true);
    if (avatar_colours && !SKMapPeople::instance().combat())
    {
        // Preferences > Colors' own minimap colour (MapAvatarSelfColor), as the others' dots take theirs.
        const skmap::PaletteColour& c = skmap::palette::SELF_DOT;
        const LLColor4 self = ALAvatarGroups::instance().getAvatarColor(gAgentID, LLColor4(c.mR, c.mG, c.mB, c.mA), ALAvatarGroups::COLOR_MINIMAP);
        gGL.color4f(self.mV[VRED], self.mV[VGREEN], self.mV[VBLUE], c.mA);
    }
    else
    {
        skmapdraw::colour(skmap::palette::SELF_DOT);
    }
    skmapdraw::disc(dot, dot_radius);
    if (off_map)
    {
        LLVector2 dir = self - dot;
        if (dir.normVec() > 1e-6f)
        {
            const LLVector2 side(-dir.mV[VY], dir.mV[VX]);
            const F32 base = dot_radius + 1.f;
            const F32 tip = base + skmap::palette::SELF_EDGE_TRIANGLE_PX;
            const F32 w = skmap::palette::SELF_EDGE_TRIANGLE_PX * 0.6f;
            skmapdraw::triangle(dot + dir * tip, dot + dir * base + side * w, dot + dir * base - side * w);
        }
    }
    else if (!gAgentCamera.cameraMouselook())
    {
        // Out of mouselook the camera's facing is not ours, so a triangle shows where the avatar faces.
        const LLVector3 facing = gAgent.getAtAxis();
        LLVector2 ahead = mView.toWidgetOffset(facing.mV[VX], facing.mV[VY]);
        if (ahead.normVec() > 1e-6f)
        {
            const LLVector2 across(-ahead.mV[VY], ahead.mV[VX]);
            const LLVector2 base = dot + ahead * (dot_radius + skmap::palette::HEADING_BASE_PX);
            skmapdraw::triangle(dot + ahead * (dot_radius + skmap::palette::HEADING_TIP_PX), base + across * skmap::palette::HEADING_HALF_PX,
                                base - across * skmap::palette::HEADING_HALF_PX);
        }
    }
    gGL.end();
}

void SKNetMap::drawSelfExtras()
{
    static LLCachedControl<bool> chat_rings(gSavedSettings, "SKMapLayerChatRings", false);
    const LLVector2 self = mView.toWidget(gAgent.getPositionGlobal());
    if (chat_rings)
    {
        // The classic map's three rings.
        static LLUIColor whisper_colour = LLUIColorTable::instance().getColor("MapWhisperRingColor", LLColor4::white);
        static LLUIColor chat_colour = LLUIColorTable::instance().getColor("MapChatRingColor", LLColor4::white);
        static LLUIColor shout_colour = LLUIColorTable::instance().getColor("MapShoutRingColor", LLColor4::white);
        LLViewerRegion* region = gAgent.getRegion();
        const F32 ppm = mView.pixelsPerMetre();
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(whisper_colour.get().mV);
        skmapdraw::ring(self, region->getWhisperRange() * ppm, 1.75f, 100);
        gGL.color4fv(chat_colour.get().mV);
        skmapdraw::ring(self, region->getChatRange() * ppm, 1.75f, 100);
        gGL.color4fv(shout_colour.get().mV);
        skmapdraw::ring(self, region->getShoutRange() * ppm, 1.75f, 100);
        gGL.end();
    }
}

namespace
{
    SKNetMap::ping_signal_t sPingSignal;
}

// static
boost::signals2::connection SKNetMap::setPingCallback(const ping_signal_t::slot_type& cb)
{
    return sPingSignal.connect(cb);
}

// static
void SKNetMap::firePing(const LLVector3d& pos)
{
    sPingSignal(pos);
}

// static
void SKNetMap::addEvent(const SKCombatEvent& event)
{
    SKMapPeople::instance().addEvent(event);
}

void SKNetMap::drawEvents()
{
    static LLCachedControl<F32> line_s(gSavedSettings, "SKMapKillLineSeconds", 0.5f);
    static LLCachedControl<F32> icon_s(gSavedSettings, "SKMapDeathIconSeconds", 2.f);
    const SKMapPeople& people = SKMapPeople::instance();
    std::vector<skmap::EventRing::Draw>& draws = mEventDraws;
    draws.clear();
    people.events().visible(LLFrameTimer::getUptimeSeconds().value(), line_s, icon_s, people.combat(), draws);
    if (draws.empty())
    {
        return;
    }
    gGL.begin(LLRender::TRIANGLES);
    for (const skmap::EventRing::Draw& d : draws)
    {
        const LLVector2 target = mView.toWidget(d.mEvent->targetPos);
        if (d.mLineAlpha > 0.f)
        {
            skmapdraw::colour(skmap::palette::EVENT_LINE, d.mLineAlpha);
            skmapdraw::line(mView.toWidget(d.mEvent->actorPos), target, 1.f);
        }
        if (d.mIconAlpha > 0.f)
        {
            const F32 r = skmap::palette::EVENT_ICON_PX;
            skmapdraw::colour(skmap::palette::EVENT_ICON, d.mIconAlpha);
            skmapdraw::line(target + LLVector2(-r, -r), target + LLVector2(r, r), 2.f);
            skmapdraw::line(target + LLVector2(-r, r), target + LLVector2(r, -r), 2.f);
        }
    }
    gGL.end();
}

void SKNetMap::drawFiringLines()
{
    const std::map<LLUUID, SKMapPeople::FiringLine>& lines = SKMapPeople::instance().firingLines();
    if (lines.empty())
    {
        return;
    }
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& entry : lines)
    {
        // Brighter at the shooter, faint at the far end: 20% down to 5% over the 64 m.
        const LLVector2 a = mView.toWidget(entry.second.mFrom), b = mView.toWidget(entry.second.mTo);
        const F32 reach = static_cast<F32>(dist_vec(entry.second.mFrom, entry.second.mTo)) / skmap::palette::FIRING_REACH_M;
        const F32 far_alpha = lerp(skmap::palette::FIRING_ALPHA_NEAR, skmap::palette::FIRING_ALPHA_FAR, llclamp(reach, 0.f, 1.f));
        LLVector2 d = b - a;
        if (d.normVec() < 1e-3f)
        {
            continue;
        }
        const LLVector2 n(-d.mV[VY] * 0.5f, d.mV[VX] * 0.5f);
        const LLColor4 near_c(1.f, 1.f, 1.f, skmap::palette::FIRING_ALPHA_NEAR);
        const LLColor4 far_c(1.f, 1.f, 1.f, far_alpha);
        gGL.color4fv(near_c.mV);
        gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
        gGL.color4fv(far_c.mV);
        gGL.vertex2f((b - n).mV[VX], (b - n).mV[VY]);
        gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
        gGL.color4fv(near_c.mV);
        gGL.vertex2f((a - n).mV[VX], (a - n).mV[VY]);
        gGL.color4fv(far_c.mV);
        gGL.vertex2f((b + n).mV[VX], (b + n).mV[VY]);
        gGL.color4fv(near_c.mV);
        gGL.vertex2f((a + n).mV[VX], (a + n).mV[VY]);
    }
    gGL.end();
}

void SKNetMap::drawProjectiles()
{
    const std::vector<SKMapPeople::Projectile>& shots = SKMapPeople::instance().projectiles();
    if (shots.empty())
    {
        return;
    }
    // A shot traced to someone the map draws takes their colour; anyone else's keeps the plain one.
    std::map<LLUUID, LLColor4> drawn;
    for (const SKMapPeople::Person& person : SKMapPeople::instance().people())
    {
        drawn[person.mId] = person.mColour;
    }
    const LLColor4 plain = toColor4(skmap::palette::PROJECTILE);
    gGL.begin(LLRender::TRIANGLES);
    for (const SKMapPeople::Projectile& p : shots)
    {
        const auto shooter = p.mShooter.notNull() ? drawn.find(p.mShooter) : drawn.end();
        const LLColor4 c = shooter != drawn.end() ? shooter->second : plain;
        const LLVector2 head = mView.toWidget(p.mPos);
        if (p.mFast)
        {
            // A streak and a bright head.
            gGL.color4f(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 0.5f);
            skmapdraw::line(mView.toWidget(p.mTail), head, 1.f);
            gGL.color4fv(c.mV);
            skmapdraw::disc(head, 1.5f, 8);
        }
        else
        {
            // A thrown object: a dot and a ring.
            gGL.color4fv(c.mV);
            skmapdraw::disc(head, 1.5f, 8);
            gGL.color4f(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 0.6f);
            skmapdraw::ring(head, 4.f, 1.f, 16);
        }
    }
    gGL.end();
}

void SKNetMap::drawPings()
{
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    gGL.begin(LLRender::TRIANGLES);
    for (const SKMapPeople::Ping& ping : SKMapPeople::instance().pings())
    {
        const F32 t = static_cast<F32>((now - ping.mTime) / skmap::palette::PING_SECONDS);
        if (t < 0.f || t >= 1.f)
        {
            continue;
        }
        // A ring that grows and fades.
        skmapdraw::colour(skmap::palette::DOT_COMBAT, 1.f - t);
        skmapdraw::ring(mView.toWidget(ping.mPos), lerp(skmap::palette::PING_FROM_PX, skmap::palette::PING_TO_PX, t), 1.5f, 32);
    }
    gGL.end();
}

void SKNetMap::drawVehicles()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerVehicles", true);
    if (!show)
    {
        return;
    }
    const std::vector<SKMapPeople::Vehicle>& vehicles = SKMapPeople::instance().vehicles();
    // Glyphs: every layer's atlas cell at its live pose, its coverage tinting the hull colour (premultiplied).
    std::vector<bool> drew(vehicles.size(), false);
    std::vector<skmapdraw::GlyphQuad> quads;
    for (size_t i = 0; i < vehicles.size(); ++i)
    {
        const SKMapPeople::Vehicle& v = vehicles[i];
        if (!skmapdraw::vehicleGlyphQuads(v, quads))
        {
            continue;
        }
        drew[i] = true;
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        skmapdraw::colour(skmapdraw::vehicleHull(v.mLook));
        for (const skmapdraw::GlyphQuad& q : quads)
        {
            gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, q.mTexture, 0);
            gGL.begin(LLRender::TRIANGLES);
            for (S32 k : { 0, 1, 2, 0, 2, 3 })
            {
                const LLVector2 at = mView.toWidget(v.mPos + LLVector3d(q.mCorner[k].mV[VX], q.mCorner[k].mV[VY], 0.0));
                gGL.texCoord2f(q.mUV[k][0], q.mUV[k][1]);
                gGL.vertex2f(at.mV[VX], at.mV[VY]);
            }
            gGL.end();
        }
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.getTextureSlot(0)->unbind();
    }

    gGL.begin(LLRender::TRIANGLES);
    for (size_t i = 0; i < vehicles.size(); ++i)
    {
        const SKMapPeople::Vehicle& v = vehicles[i];
        // The linkset's box at its live heading, unless its glyph drew: hull, and a nose tick at its front.
        auto at = [&](F32 x, F32 y) {
            const LLVector3 p = LLVector3(x, y, 0.f) * v.mRot;
            return mView.toWidget(v.mPos + LLVector3d(p.mV[VX], p.mV[VY], 0.0));
        };
        if (!drew[i])
        {
            const LLVector2 a = at(v.mMin.mV[VX], v.mMin.mV[VY]), b = at(v.mMax.mV[VX], v.mMin.mV[VY]);
            const LLVector2 c = at(v.mMax.mV[VX], v.mMax.mV[VY]), d = at(v.mMin.mV[VX], v.mMax.mV[VY]);
            skmapdraw::colour(skmapdraw::vehicleHull(v.mLook));
            skmapdraw::triangle(a, b, c);
            skmapdraw::triangle(a, c, d);
        }
        const F32 mid_y = (v.mMin.mV[VY] + v.mMax.mV[VY]) * 0.5f;
        const F32 length = v.mMax.mV[VX] - v.mMin.mV[VX];
        skmapdraw::colour(skmapdraw::vehicleTop(v.mLook));
        skmapdraw::line(at(v.mMax.mV[VX] - length * 0.25f, mid_y), at(v.mMax.mV[VX], mid_y), 2.f);
    }
    gGL.end();
}

const LLFontGL* skmapdraw::codeFont()
{
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    static U32 made_for = 0;
    static const LLFontGL* font = nullptr;
    if (!font || made_for != font_px)
    {
        // Pixels to points at 96 dpi.
        made_for = font_px;
        font = LLFontGL::getFontAtPoints("SansSerif", llclamp(static_cast<F32>(font_px), 8.f, 11.f) * 0.75f, LLFontGL::NORMAL);
        if (!font)
        {
            font = LLFontGL::getFontSansSerifSmall();
        }
    }
    return font;
}

F32 skmapdraw::markerRadius(const SKMapPeople::Person& p, skmap::FloorRelation floor)
{
    static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
    static LLCachedControl<bool> labels(gSavedSettings, "SKMapMarkerLabels", true);
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    static LLCachedControl<F32> padding(gSavedSettings, "SKMapMarkerPaddingPx", 0.5f);
    const bool chevron = chevrons && (floor == skmap::FloorRelation::ABOVE || floor == skmap::FloorRelation::BELOW);
    const bool coded = labels && !p.mCode.empty();
    const F32 text_w = coded ? codeFont()->getWidthF32(p.mCode) : 0.f;
    return skmap::markerRadius(chevron, coded, static_cast<U32>(p.mCode.size()), text_w, llclamp(static_cast<F32>(font_px), 8.f, 11.f),
                               llclamp(static_cast<F32>(padding), 0.f, 3.f));
}

void skmapdraw::personCode(const SKMapPeople::Person& p, const LLVector2& at, skmap::FloorRelation floor)
{
    static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
    static LLCachedControl<bool> labels(gSavedSettings, "SKMapMarkerLabels", true);
    const bool chevron = chevrons && (floor == skmap::FloorRelation::ABOVE || floor == skmap::FloorRelation::BELOW);
    // The chevron replaces the code on the dot; the tag keeps it.
    if (!labels || chevron || p.mVisibility != skmap::Visibility::DOT_CODED || p.mCode.empty())
    {
        return;
    }
    const LLColor4 ink(skmap::palette::CODE_INK.mR, skmap::palette::CODE_INK.mG, skmap::palette::CODE_INK.mB, 1.f);
    codeFont()->renderUTF8(p.mCode, 0, at.mV[VX], at.mV[VY], ink, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL,
                           LLFontGL::NO_SHADOW);
}

std::vector<skmapdraw::TagText> skmapdraw::tagTexts(const std::vector<LLUUID>& members,
                                                    const std::function<skmap::FloorRelation(const LLUUID&)>& floor_of,
                                                    const FloorLabels& floors, F32 widest)
{
    static LLCachedControl<bool> code_labels(gSavedSettings, "SKMapMarkerLabels", true);
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const F32 text_room = llmax(8.f, widest - 2.f * skmap::palette::TAG_PAD_X_PX);
    auto fit = [&](const std::string& text, F32 room) {
        if (font->getWidthF32(text) <= room)
        {
            return text;
        }
        // Cut with an ellipsis.
        const std::string ellipsis = "\xe2\x80\xa6";
        std::string cut = text;
        while (!cut.empty() && font->getWidthF32(cut + ellipsis) > room)
        {
            // Drop one whole UTF-8 character: its continuation bytes, then its lead.
            while (!cut.empty() && (static_cast<U8>(cut.back()) & 0xC0) == 0x80)
            {
                cut.pop_back();
            }
            if (!cut.empty())
            {
                cut.pop_back();
            }
        }
        return cut + ellipsis;
    };

    std::vector<TagText> texts;
    std::map<std::string, S32> name_counts;
    for (const LLUUID& id : members)
    {
        TagText t;
        t.mId = id;
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(id, &av_name))
        {
            t.mName = RlvActions::canShowName(RlvActions::SNC_DEFAULT, id) ? av_name.getDisplayName() : RlvStrings::getAnonym(av_name);
            t.mDetail = av_name.getUserName();
        }
        else
        {
            t.mName = LLTrans::getString("LoadingData");
        }
        name_counts[t.mName]++;
        texts.push_back(t);
    }
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    for (TagText& t : texts)
    {
        // In combat the tag leads with the ally's code, as the dot shows it.
        for (const SKMapPeople::Person& p : people)
        {
            if (p.mId == t.mId && code_labels)
            {
                t.mCode = p.mCode;
                break;
            }
        }
        // A second line only when there is something to say: two people sharing a name show
        // their usernames, else the floor they are on.
        const bool shared = name_counts[t.mName] > 1 && RlvActions::canShowName(RlvActions::SNC_DEFAULT, t.mId);
        std::string detail;
        if (shared)
        {
            detail = t.mDetail;
        }
        else
        {
            const skmap::FloorRelation floor = floor_of(t.mId);
            detail = floor == skmap::FloorRelation::ABOVE   ? floors.mAbove
                   : floor == skmap::FloorRelation::BELOW   ? floors.mBelow
                   : floor == skmap::FloorRelation::UNKNOWN ? floors.mUnknown
                                                            : std::string();
        }
        const F32 code_w = t.mCode.empty() ? 0.f : font->getWidthF32(t.mCode) + skmap::palette::TAG_CODE_GAP_PX;
        t.mName = fit(t.mName, text_room - code_w);
        t.mDetail = detail.empty() ? std::string() : fit(detail, text_room);
        const F32 text_w = llmax(code_w + font->getWidthF32(t.mName), t.mDetail.empty() ? 0.f : font->getWidthF32(t.mDetail));
        t.mWidth = std::ceil(text_w + 2.f * skmap::palette::TAG_PAD_X_PX);
        t.mHeight = std::ceil(font->getLineHeight() * (t.mDetail.empty() ? 1.f : 2.f) + 2.f * skmap::palette::TAG_PAD_Y_PX);
    }
    return texts;
}

void skmapdraw::drawTag(const LLRect& r, const TagText& t)
{
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLColor4 bg(skmap::palette::TAG_BG.mR, skmap::palette::TAG_BG.mG, skmap::palette::TAG_BG.mB, skmap::palette::TAG_BG.mA);
    const LLColor4 text(skmap::palette::TAG_TEXT.mR, skmap::palette::TAG_TEXT.mG, skmap::palette::TAG_TEXT.mB, 1.f);
    const LLColor4 code(skmap::palette::TAG_CODE.mR, skmap::palette::TAG_CODE.mG, skmap::palette::TAG_CODE.mB, 1.f);
    const LLColor4 detail(skmap::palette::TAG_DETAIL.mR, skmap::palette::TAG_DETAIL.mG, skmap::palette::TAG_DETAIL.mB, 1.f);
    gl_rect_2d(r, bg, true);
    F32 x = r.mLeft + skmap::palette::TAG_PAD_X_PX;
    const F32 y = r.mTop - skmap::palette::TAG_PAD_Y_PX;
    if (!t.mCode.empty())
    {
        font->renderUTF8(t.mCode, 0, x, y, code, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        x += font->getWidthF32(t.mCode) + skmap::palette::TAG_CODE_GAP_PX;
    }
    font->renderUTF8(t.mName, 0, x, y, text, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    if (!t.mDetail.empty())
    {
        font->renderUTF8(t.mDetail, 0, r.mLeft + skmap::palette::TAG_PAD_X_PX, y - font->getLineHeight(), detail, LLFontGL::LEFT,
                         LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
}

void SKNetMap::collectDots()
{
    mDots.clear();
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    mFloorOf.resize(people.size());
    for (size_t i = 0; i < people.size(); ++i)
    {
        mFloorOf[i] = people[i].mFloor;
    }
    for (size_t i = 0; i < people.size(); ++i)
    {
        const SKMapPeople::Person& p = people[i];
        const skmap::FloorRelation floor = mFloorOf[i];
        const F32 radius = llmax(skmapdraw::markerRadius(p, floor), footprintRadius(gObjectList.findObject(p.mId), mView.pixelsPerMetre()));
        const LLVector2 at = mView.toWidget(p.mPos);
        // Only dots the clip leaves some of: hover, tags and picks never name an unseen one.
        if (at.mV[VX] < -radius || at.mV[VY] < -radius || at.mV[VX] > mView.width() + radius || at.mV[VY] > mView.height() + radius)
        {
            continue;
        }
        mDots.push_back({ p.mId, at, radius, i });
    }
}

void SKNetMap::drawPeople()
{
    static LLCachedControl<bool> triangles(gSavedSettings, "SKMapHeadingTriangles", true);
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    const F32 FONT_PX = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    skmapdraw::shadows([&]() {
        for (const DrawnDot& d : mDots)
        {
            if (d.mPerson < people.size())
            {
                skmapdraw::shadowQuad(d.mPos, d.mRadius);
            }
        }
    });
    gGL.begin(LLRender::TRIANGLES);
    for (const DrawnDot& d : mDots)
    {
        if (d.mPerson >= people.size())
        {
            continue;
        }
        const SKMapPeople::Person& p = people[d.mPerson];
        const LLVector2 at = d.mPos;
        const F32 radius = d.mRadius;
        const skmap::FloorRelation floor = d.mPerson < mFloorOf.size() ? mFloorOf[d.mPerson] : skmap::FloorRelation::SAME;
        LLVector2 ahead;
        bool has_ahead = false;
        if (triangles && p.mHasHeading)
        {
            ahead = mView.toWidgetOffset(std::cos(p.mHeading), std::sin(p.mHeading));
            has_ahead = ahead.normVec() > 1e-6f;
        }
        skmapdraw::personMarker(p, at, radius, has_ahead ? &ahead : nullptr, floor, FONT_PX);
    }
    gGL.end();
}

void SKNetMap::drawEdgeChevrons()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapEdgeChevrons", true);
    if (!show)
    {
        return;
    }
    using namespace skmap::palette;
    const LLVector2 mid(mView.width() * 0.5f, mView.height() * 0.5f);
    const F32 ppm = llmax(mView.pixelsPerMetre(), 1e-4f);
    const F32 half_x = mid.mV[VX] - EDGE_CHEVRON_INSET_PX, half_y = mid.mV[VY] - EDGE_CHEVRON_INSET_PX;
    const F32 circle = mEdgeCircle - EDGE_CHEVRON_INSET_PX;
    if (half_x <= 0.f || half_y <= 0.f || (mEdgeCircle > 0.f && circle <= 0.f))
    {
        return;
    }

    struct Edge
    {
        LLVector2 mAt;          ///< on the inset edge
        LLVector2 mDir;         ///< unit, from the centre towards the person
        F32 mAngle;
        F32 mBeyondPx;          ///< how far past the edge the person is
        F32 mAlpha;
        LLColor4 mColour;
    };
    std::vector<Edge> edges;
    for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
    {
        const LLVector2 at = mView.toWidget(p.mPos);
        LLVector2 dir = at - mid;
        const F32 dist = dir.normVec();
        if (dist < 1e-3f)
        {
            continue;
        }
        // Where the line from the centre meets the map's inset shape; nobody still on the map.
        F32 reach = circle;
        if (mEdgeCircle > 0.f)
        {
            if (dist <= mEdgeCircle)
            {
                continue;
            }
        }
        else
        {
            if (at.mV[VX] >= 0.f && at.mV[VY] >= 0.f && at.mV[VX] <= mView.width() && at.mV[VY] <= mView.height())
            {
                continue;
            }
            reach = F32_MAX;
            if (std::fabs(dir.mV[VX]) > 1e-6f)
            {
                reach = llmin(reach, half_x / std::fabs(dir.mV[VX]));
            }
            if (std::fabs(dir.mV[VY]) > 1e-6f)
            {
                reach = llmin(reach, half_y / std::fabs(dir.mV[VY]));
            }
        }
        const F32 beyond = llmax(0.f, dist - reach);
        const F32 fade = llclamp(beyond / ppm / EDGE_CHEVRON_FADE_M, 0.f, 1.f);
        // Allies in combat are cyan, as their dots are.
        const LLColor4 colour = p.mVisibility == skmap::Visibility::DOT_CODED ? LLColor4(DOT_COMBAT.mR, DOT_COMBAT.mG, DOT_COMBAT.mB, 1.f)
                                                                              : LLColor4(p.mColour.mV[0], p.mColour.mV[1], p.mColour.mV[2], 1.f);
        edges.push_back({ mid + dir * reach, dir, std::atan2(dir.mV[VY], dir.mV[VX]), beyond, 1.f - (1.f - EDGE_CHEVRON_FADE_MIN) * fade, colour });
    }
    if (edges.empty())
    {
        return;
    }

    // Round the edge in order; neighbours closer than the cluster spacing stack, the nearest person outermost.
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.mAngle < b.mAngle; });
    std::vector<std::vector<size_t>> clusters;
    for (size_t i = 0; i < edges.size(); ++i)
    {
        if (!clusters.empty() && dist_vec(edges[clusters.back().front()].mAt, edges[i].mAt) < EDGE_CHEVRON_CLUSTER_PX)
        {
            clusters.back().push_back(i);
        }
        else
        {
            clusters.push_back({ i });
        }
    }
    if (clusters.size() > 1 && dist_vec(edges[clusters.front().front()].mAt, edges[clusters.back().front()].mAt) < EDGE_CHEVRON_CLUSTER_PX)
    {
        clusters.front().insert(clusters.front().end(), clusters.back().begin(), clusters.back().end());
        clusters.pop_back();
    }

    gGL.begin(LLRender::TRIANGLES);
    for (std::vector<size_t>& cluster : clusters)
    {
        std::sort(cluster.begin(), cluster.end(), [&edges](size_t a, size_t b) { return edges[a].mBeyondPx < edges[b].mBeyondPx; });
        const Edge& lead = edges[cluster.front()];
        const LLVector2 across(-lead.mDir.mV[VY], lead.mDir.mV[VX]);
        const S32 stack = llmin(static_cast<S32>(cluster.size()), EDGE_CHEVRON_MAX_STACK);
        for (S32 k = stack - 1; k >= 0; --k)
        {
            const Edge& e = edges[cluster[k]];
            const LLVector2 tip = lead.mAt - lead.mDir * (EDGE_CHEVRON_STACK_PX * k);
            const LLVector2 back = tip - lead.mDir * EDGE_CHEVRON_DEPTH_PX;
            const LLVector2 left = back + across * EDGE_CHEVRON_HALF_PX, right = back - across * EDGE_CHEVRON_HALF_PX;
            skmapdraw::colour(EDGE_CHEVRON_CASING, e.mAlpha);
            skmapdraw::line(left, tip, EDGE_CHEVRON_CASING_PX);
            skmapdraw::line(tip, right, EDGE_CHEVRON_CASING_PX);
            gGL.color4f(e.mColour.mV[0], e.mColour.mV[1], e.mColour.mV[2], e.mAlpha);
            skmapdraw::line(left, tip, EDGE_CHEVRON_STROKE_PX);
            skmapdraw::line(tip, right, EDGE_CHEVRON_STROKE_PX);
        }
    }
    gGL.end();
}

void SKNetMap::drawPeopleCodes()
{
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    for (const DrawnDot& d : mDots)
    {
        if (d.mPerson < people.size())
        {
            skmapdraw::personCode(people[d.mPerson], d.mPos,
                                  d.mPerson < mFloorOf.size() ? mFloorOf[d.mPerson] : skmap::FloorRelation::SAME);
        }
    }
}

LLRect SKNetMap::tagRect(const skmap::LabelLayout::Tag& tag) const
{
    const LLRect screen = calcScreenRect();
    const S32 height = gViewerWindow->getRootView()->getRect().getHeight();
    const S32 left = ll_round(static_cast<F32>(tag.mX)) - screen.mLeft;
    const S32 top = height - ll_round(static_cast<F32>(tag.mY)) - screen.mBottom;
    return LLRect(left, top, left + ll_round(static_cast<F32>(tag.mW)), top - ll_round(static_cast<F32>(tag.mH)));
}

void SKNetMap::updateHover()
{
    static LLCachedControl<F32> enter(gSavedSettings, "SKMapHoverEnterPx", skmap::HOVER_ENTER_PX);
    static LLCachedControl<F32> exit(gSavedSettings, "SKMapHoverExitPx", skmap::HOVER_EXIT_PX);
    static LLCachedControl<U32> max_width(gSavedSettings, "SKMapNametagMaxWidth", 190);

    S32 mx = 0, my = 0;
    LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
    const LLVector2 pointer(static_cast<F32>(mx), static_cast<F32>(my));
    mHover.notePointer(pointer);

    // Escape clears the names until the pointer moves; it keeps its usual meaning too.
    const bool esc = gKeyboard && gKeyboard->getKeyDown(KEY_ESCAPE);
    if (esc && !mEscDown && !mHover.empty() && !panesOpen())
    {
        mHover.suppress(pointer);
    }
    mEscDown = esc;

    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    if (!mHovered || gAgentCamera.cameraMouselook() || panesOpen() || (captor && captor != this) || mHover.suppressed())
    {
        mHover.clear();
        mLabels.clear();
        mTagTexts.clear();
        return;
    }

    std::vector<skmap::HoverDot> dots;
    dots.reserve(mDots.size());
    for (const DrawnDot& d : mDots)
    {
        dots.push_back({ d.mId, d.mPos });
    }
    mHover.update(dots, pointer, enter, exit);
    if (mHover.empty())
    {
        mLabels.clear();
        mTagTexts.clear();
        return;
    }

    // The solver works in window pixels, y down, over the whole window.
    const LLRect window = gViewerWindow->getRootView()->getRect();
    const F32 win_w = static_cast<F32>(window.getWidth()), win_h = static_cast<F32>(window.getHeight());
    const LLRect screen = calcScreenRect();
    auto down = [&](const LLVector2& local) { return LLVector2(screen.mLeft + local.mV[VX], win_h - (screen.mBottom + local.mV[VY])); };

    const skmapdraw::FloorLabels floors{ mFloorAboveLabel, mFloorBelowLabel, mFloorUnknownLabel };
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    auto floor_of = [&](const LLUUID& id) {
        for (size_t i = 0; i < people.size(); ++i)
        {
            if (people[i].mId == id)
            {
                return i < mFloorOf.size() ? mFloorOf[i] : people[i].mFloor;
            }
        }
        return skmap::FloorRelation::SAME;
    };
    mTagTexts = skmapdraw::tagTexts(mHover.members(), floor_of, floors, skmap::tagMaxWidth(win_w, static_cast<F32>(max_width)));

    skmap::LabelLayout::Input in;
    in.mPointer = down(pointer);
    in.mWidth = win_w;
    in.mHeight = win_h;
    in.mChrome = mChrome;
    for (const TagText& t : mTagTexts)
    {
        for (const DrawnDot& d : mDots)
        {
            if (d.mId == t.mId)
            {
                in.mMembers.push_back({ t.mId, down(d.mPos), t.mWidth, t.mHeight });
                break;
            }
        }
    }
    // Every drawn dot, then the cog and the grip, as the study keeps them clear.
    for (const DrawnDot& d : mDots)
    {
        const LLVector2 p = down(d.mPos);
        in.mObstacles.push_back({ p.mV[VX] - 5.0, p.mV[VY] - 5.0, 10.0, 10.0 });
    }
    const LLVector2 top_right = down(LLVector2(mView.width(), mView.height()));
    const LLVector2 bottom_right = down(LLVector2(mView.width(), 0.f));
    in.mObstacles.push_back({ top_right.mV[VX] - 28.0, top_right.mV[VY] + 2.0, 26.0, 26.0 });
    in.mObstacles.push_back({ bottom_right.mV[VX] - 26.0, bottom_right.mV[VY] - 26.0, 26.0, 26.0 });
    mLabels.update(in, LLFrameTimer::getFrameDeltaTimeF32());
}

void SKNetMap::drawConnectors()
{
    if (mLabels.tags().empty())
    {
        return;
    }
    const LLRect screen = calcScreenRect();
    const F32 win_h = static_cast<F32>(gViewerWindow->getRootView()->getRect().getHeight());
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::CONNECTOR);
    for (const skmap::LabelLayout::Tag& tag : mLabels.tags())
    {
        for (const DrawnDot& d : mDots)
        {
            if (d.mId != tag.mId)
            {
                continue;
            }
            const LLVector2 anchor(screen.mLeft + d.mPos.mV[VX], win_h - (screen.mBottom + d.mPos.mV[VY]));
            const LLVector2 end = skmap::LabelLayout::connectorEnd(tag, anchor);
            skmapdraw::line(d.mPos, LLVector2(end.mV[VX] - screen.mLeft, win_h - end.mV[VY] - screen.mBottom), 1.f);
            break;
        }
    }
    gGL.end();
}

void SKNetMap::drawTags()
{
    for (const skmap::LabelLayout::Tag& tag : mLabels.tags())
    {
        const TagText* t = nullptr;
        for (const TagText& candidate : mTagTexts)
        {
            if (candidate.mId == tag.mId)
            {
                t = &candidate;
                break;
            }
        }
        if (!t)
        {
            continue;
        }
        skmapdraw::drawTag(tagRect(tag), *t);
    }
}

void SKNetMap::drawTrackingAt(const LLVector3d& pos, const LLColor4& colour)
{
    const LLVector2 at = mView.toWidget(pos);
    const LLRect rect = getLocalRect();
    if (at.mV[VX] < 0.f || at.mV[VY] < 0.f || at.mV[VX] >= rect.getWidth() || at.mV[VY] >= rect.getHeight())
    {
        const S32 x = ll_round(at.mV[VX]), y = ll_round(at.mV[VY]);
        LLWorldMapView::drawTrackingCircle(rect, x, y, colour, 1, 10);
        LLWorldMapView::drawTrackingArrow(rect, x, y, colour);
    }
    else
    {
        LLWorldMapView::drawTrackingDot(at.mV[VX], at.mV[VY], colour, static_cast<F32>(pos.mdV[VZ] - gAgent.getPositionGlobal().mdV[VZ]));
    }
}

void SKNetMap::drawTracking()
{
    static LLUIColor track_colour = LLUIColorTable::instance().getColor("MapTrackColor", LLColor4::white);
    if (gAgent.getAutoPilot())
    {
        drawTrackingAt(gAgent.getAutoPilotTargetGlobal(), track_colour);
        return;
    }
    const LLTracker::ETrackingStatus status = LLTracker::getTrackingStatus();
    if (status == LLTracker::TRACKING_AVATAR)
    {
        // In combat only resolved allies are drawn, so a tracked avatar is too only when one.
        const SKMapPeople& people = SKMapPeople::instance();
        bool shown = !people.combat();
        if (!shown)
        {
            const LLUUID& tracked = LLAvatarTracker::instance().getAvatarID();
            for (const SKMapPeople::Person& p : people.people())
            {
                if (p.mId == tracked)
                {
                    shown = p.mAlly;
                    break;
                }
            }
        }
        if (shown)
        {
            drawTrackingAt(LLAvatarTracker::instance().getGlobalPos(), track_colour);
        }
    }
    else if (status == LLTracker::TRACKING_LANDMARK || status == LLTracker::TRACKING_LOCATION)
    {
        drawTrackingAt(LLTracker::getTrackedPositionGlobal(), track_colour);
    }
}

void SKNetMap::drawNorth()
{
    static LLCachedControl<bool> compass(gSavedSettings, "SKMapCompassRing", false);
    if (compass)
    {
        // A ring round the middle; drawNorthText puts the four cardinal letters on it.
        const LLVector2 centre(mView.width() * 0.5f, mView.height() * 0.5f);
        gGL.begin(LLRender::TRIANGLES);
        skmapdraw::colour(skmap::palette::CHROME_BORDER);
        skmapdraw::ring(centre, mView.shortSide() * 0.5f - 8.f, 1.f, 64);
        gGL.end();
    }
}

void SKNetMap::drawNorthText()
{
    static LLCachedControl<bool> compass(gSavedSettings, "SKMapCompassRing", false);
    const LLVector2 centre(mView.width() * 0.5f, mView.height() * 0.5f);
    LLVector2 north = mView.toWidgetOffset(0.f, 1.f);
    north.normVec();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLColor4 text = toColor4(skmap::palette::NORTH);
    if (compass)
    {
        const F32 radius = mView.shortSide() * 0.5f - 8.f;
        static const char* LETTERS[4] = { "N", "E", "S", "W" };
        for (S32 k = 0; k < 4; ++k)
        {
            const F32 ax = k == 1 ? 1.f : k == 3 ? -1.f : 0.f;
            const F32 ay = k == 0 ? 1.f : k == 2 ? -1.f : 0.f;
            LLVector2 dir = mView.toWidgetOffset(ax, ay);
            dir.normVec();
            const LLVector2 at = centre + dir * radius;
            font->renderUTF8(LETTERS[k], 0, at.mV[VX], at.mV[VY], text, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::DROP_SHADOW);
        }
        return;
    }
    // One N at the north edge, wherever the rotation puts it.
    const F32 hx = mView.width() * 0.5f - 8.f, hy = mView.height() * 0.5f - 8.f;
    const F32 t = llmin(std::fabs(north.mV[VX]) > 1e-4f ? hx / std::fabs(north.mV[VX]) : 1e9f,
                        std::fabs(north.mV[VY]) > 1e-4f ? hy / std::fabs(north.mV[VY]) : 1e9f);
    const LLVector2 at = centre + north * t;
    font->renderUTF8("N", 0, at.mV[VX], at.mV[VY], text, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
}

void SKNetMap::drawCombatNotice()
{
    // Shown for a moment when the map enters or leaves combat, fading out over the last half second.
    constexpr F64 SHOW_S = 2.5, FADE_S = 0.5;
    const SKMapPeople& people = SKMapPeople::instance();
    const F64 age = LLFrameTimer::getUptimeSeconds().value() - people.combatChangedAt();
    if (age < 0.0 || age >= SHOW_S)
    {
        return;
    }
    LLColor4 text = toColor4(skmap::palette::NORTH);
    text.mV[VALPHA] *= static_cast<F32>(llclamp((SHOW_S - age) / FADE_S, 0.0, 1.0));
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    font->renderUTF8(people.combat() ? "Combat" : "Peace", 0, mView.width() * 0.5f, mView.height() - 24.f, text, LLFontGL::HCENTER,
                     LLFontGL::VCENTER, LLFontGL::BOLD, LLFontGL::DROP_SHADOW);
}

void SKNetMap::drawReadout()
{
    static LLCachedControl<bool> readout(gSavedSettings, "SKMapDebugReadout", false);
    if (!readout)
    {
        return;
    }
    std::vector<std::string> lines;
    lines.push_back(llformat("draw %.2f ms", mDrawMs));
    if (SKOverviewCapture::instanceExists())
    {
        const SKOverviewCapture::Stats& stats = SKOverviewCapture::instance().stats();
        lines.push_back(llformat("capture %d px, %.2f ms cpu, %.2f ms gpu, %u captures", stats.resolution, stats.cpu_ms, stats.gpu_ms,
                                 stats.captures));
    }
    lines.push_back(llformat("navmesh %u tiles built", SKMapScene::instance().builtTiles()));
    const SKMapAllyResolver& resolver = SKMapPeople::instance().resolver();
    lines.push_back(llformat("probes %u in flight, %u queued", resolver.probesInFlight(), resolver.probesQueued()));
    const LLFontGL* font = LLFontGL::getFontMonospace();
    F32 y = 4.f;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
    {
        font->renderUTF8(*it, 0, 4.f, y, LLColor4::white, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        y += font->getLineHeight();
    }
}

void SKNetMap::drawChromeTint()
{
    if (!mChrome)
    {
        return;
    }
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::CHROME_TINT);
    const LLVector2 a(0.f, 0.f), b(mView.width(), 0.f), c(mView.width(), mView.height()), d(0.f, mView.height());
    skmapdraw::triangle(a, b, c);
    skmapdraw::triangle(a, c, d);
    gGL.end();
}

void SKNetMap::drawChromeFrame()
{
    if (!mChrome)
    {
        return;
    }
    const F32 w = mView.width(), h = mView.height(), px = skmap::palette::CHROME_BORDER_PX;
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::CHROME_BORDER);
    skmapdraw::line(LLVector2(0.f, px * 0.5f), LLVector2(w, px * 0.5f), px);
    skmapdraw::line(LLVector2(0.f, h - px * 0.5f), LLVector2(w, h - px * 0.5f), px);
    skmapdraw::line(LLVector2(px * 0.5f, 0.f), LLVector2(px * 0.5f, h), px);
    skmapdraw::line(LLVector2(w - px * 0.5f, 0.f), LLVector2(w - px * 0.5f, h), px);
    // The grip: diagonal stripes in the bottom-right corner, over the floater's resize handle.
    skmapdraw::colour(skmap::palette::GRIP);
    for (F32 k = skmap::palette::GRIP_PITCH_PX; k <= skmap::palette::GRIP_PX; k += skmap::palette::GRIP_PITCH_PX)
    {
        skmapdraw::line(LLVector2(w - k, 1.f), LLVector2(w - 1.f, k), 1.f);
    }
    gGL.end();
}

void SKNetMap::onMouseEnter(S32 x, S32 y, MASK mask)
{
    mHovered = true;
    LLUICtrl::onMouseEnter(x, y, mask);
}

void SKNetMap::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKNetMap::handleToolTip(S32 x, S32 y, MASK mask)
{
    // Nametags say who is under the cursor; the tooltip only says where it is.
    if (gDisconnected || namesShown() || mouseNearPanes())
    {
        return false;
    }
    LLRect sticky_rect;
    constexpr S32 SLOP = 4;
    localPointToScreen(x - SLOP, y - SLOP, &sticky_rect.mLeft, &sticky_rect.mBottom);
    sticky_rect.mRight = sticky_rect.mLeft + 2 * SLOP;
    sticky_rect.mTop = sticky_rect.mBottom + 2 * SLOP;

    std::string region_name_msg, parcel_name_msg, parcel_owner_msg, parcel_sale_price_msg, parcel_sale_area_msg;
    const LLVector3d pos_global = mView.toGlobal(static_cast<F32>(x), static_cast<F32>(y));
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        const std::string region_name = RlvActions::canShowLocation() ? region->getName() : RlvStrings::getString(RlvStringKeys::Hidden::Region);
        if (!region_name.empty())
        {
            region_name_msg = LLTrans::getString("MinimapRegionNameMsg");
            LLStringUtil::format(region_name_msg, { { "[REGION_NAME]", region_name } });
        }
        // As on the classic map, the parcel is named only while its lines show; else which
        // parcel the tooltip means is ambiguous.
        static LLCachedControl<bool> parcel_lines(gSavedSettings, "SKMapLayerParcels", false);
        if (parcel_lines && RlvActions::canShowLocation())
        {
            LLViewerParcelMgr::getInstance()->setHoverParcel(pos_global);
            if (LLParcel* parcel = LLViewerParcelMgr::getInstance()->getHoverParcel())
            {
                if (!parcel->getName().empty())
                {
                    parcel_name_msg = LLTrans::getString("MinimapParcelNameMsg");
                    LLStringUtil::format(parcel_name_msg, { { "[PARCEL_NAME]", parcel->getName() } });
                }
                const LLUUID owner = parcel->getOwnerID();
                static LLUrlMatch owner_match;
                LLUrlRegistry::getInstance()->findUrl(LLSLURL("agent", owner, "inspect").getSLURLString(), owner_match);
                if (!owner_match.empty())
                {
                    parcel_owner_msg = LLTrans::getString("MinimapParcelOwnerMsg");
                    LLStringUtil::format(parcel_owner_msg, { { "[PARCEL_OWNER]", owner_match.getLabel() } });
                }
                if (parcel->getForSale())
                {
                    const LLUUID buyer = parcel->getAuthorizedBuyerID();
                    if (buyer.isNull() || buyer == gAgent.getID() || owner == gAgent.getID())
                    {
                        const S32 price = parcel->getSalePrice();
                        const S32 area = parcel->getArea();
                        parcel_sale_price_msg = LLTrans::getString("MinimapParcelSalePriceMsg");
                        LLStringUtil::format(parcel_sale_price_msg,
                                             { { "[PRICE]", LLResMgr::getInstance()->getMonetaryString(price) },
                                               { "[PRICE_PER_SQM]", llformat("%.1f", area > 0 ? F32(price) / area : 0.f) } });
                        parcel_sale_area_msg = LLTrans::getString("MinimapParcelSaleAreaMsg");
                        LLStringUtil::format(parcel_sale_area_msg, { { "[AREA]", llformat("%d", area) } });
                    }
                }
            }
        }
    }
    std::string hint;
    if (gSavedSettings.getBOOL("DoubleClickTeleport"))
    {
        hint = LLTrans::getString("MinimapAltToolTipHintMsg");
    }
    else if (gSavedSettings.getBOOL("DoubleClickShowWorldMap"))
    {
        hint = LLTrans::getString("MinimapToolTipHintMsg");
    }
    auto line = [](const std::string& s) { return s.empty() ? std::string() : s + '\n'; };
    LLStringUtil::format_map_t args;
    args["[AGENT]"] = "";
    args["[PARCEL_NAME_MSG]"] = line(parcel_name_msg);
    args["[PARCEL_SALE_PRICE_MSG]"] = line(parcel_sale_price_msg);
    args["[PARCEL_SALE_AREA_MSG]"] = line(parcel_sale_area_msg);
    args["[PARCEL_OWNER_MSG]"] = line(parcel_owner_msg);
    args["[REGION_NAME_MSG]"] = line(region_name_msg);
    args["[TOOL_TIP_HINT_MSG]"] = line(hint);
    std::string msg = LLTrans::getString("MinimapToolTipMsg");
    LLStringUtil::format(msg, args);
    LLStringUtil::trim(msg);
    if (msg.empty())
    {
        return false;
    }
    LLToolTipMgr::instance().show(LLToolTip::Params().message(msg).sticky_rect(sticky_rect));
    return true;
}

bool SKNetMap::panesOpen() const
{
    const SKPopupPanes* panes = mPopup.get();
    return panes && panes->getVisible();
}

bool SKNetMap::mouseNearPanes() const
{
    SKPopupPanes* panes = mPopup.get();
    if (!panes || !panes->getVisible())
    {
        return false;
    }
    // The classic map's tolerance: a few pixels round the popup count as on it.
    constexpr S32 TOLERANCE = 10;
    S32 px = 0, py = 0;
    LLUI::getInstance()->getMousePositionLocal(panes, &px, &py);
    for (S32 sx = -1; sx <= 1; sx += 2)
    {
        for (S32 sy = -1; sy <= 1; sy += 2)
        {
            if (panes->pointInView(px + sx * TOLERANCE, py + sy * TOLERANCE))
            {
                return true;
            }
        }
    }
    return false;
}

bool SKNetMap::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    // Freeze the names on screen, or failing that the dots by the click, by value.
    std::vector<LLUUID> ids = mHover.members();
    const LLVector2 click(static_cast<F32>(x), static_cast<F32>(y));
    if (ids.empty())
    {
        std::vector<std::pair<F32, LLUUID>> near_click;
        for (const DrawnDot& d : mDots)
        {
            const F32 dist = dist_vec(d.mPos, click);
            if (dist <= skmap::HOVER_ENTER_PX)
            {
                near_click.emplace_back(dist, d.mId);
            }
        }
        std::sort(near_click.begin(), near_click.end());
        for (const auto& n : near_click)
        {
            ids.push_back(n.second);
        }
    }
    const std::vector<SKMapMenu::Person> people = SKMapMenu::freeze(ids);
    mHover.suppress(click);
    mLabels.clear();
    mTagTexts.clear();
    mPopupWorldPos = mView.toGlobal(click.mV[VX], click.mV[VY]);
    if (SKPopupPanes* old = mPopup.get())
    {
        old->closeAll();
    }
    mPopup = SKMapMenu::open(this, x, y, mPopupWorldPos, people);
    if (SKPopupPanes* panes = mPopup.get())
    {
        LLHandle<LLView> self = getHandle();
        panes->setClosedCallback([self]() {
            if (SKNetMap* map = dynamic_cast<SKNetMap*>(self.get()))
            {
                map->onPanesClosed();
            }
        });
    }
    return true;
}

void SKNetMap::onPanesClosed()
{
    S32 mx = 0, my = 0;
    LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
    mHover.suppress(LLVector2(static_cast<F32>(mx), static_cast<F32>(my)));
    mLabels.clear();
    mTagTexts.clear();
}

bool SKNetMap::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    // Positive clicks zoom out, as the classic map; region mode's zoom lasts until it refits.
    if (regionMode())
    {
        mRegionSpan = skmap::zoomByNotches(mView.span(), delta.mPrecise);
        return true;
    }
    const F32 span = skmap::zoomByNotches(gSavedSettings.getF32("SKMapZoomMeters"), delta.mPrecise);
    gSavedSettings.setF32("SKMapZoomMeters", span);
    return true;
}

bool SKNetMap::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // Only a shift-drag pans; a plain click goes to the floater, which drags it.
    if (!(mask & MASK_SHIFT))
    {
        return false;
    }
    gFocusMgr.setMouseCapture(this);
    mMouseDown.set(x, y);
    mEaseStart = -1.0;
    return true;
}

bool SKNetMap::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return false;
    }
    if (mPanning)
    {
        // Put the cursor back where the drag began, inside the map.
        LLRect clip = getLocalRect();
        clip.stretch(-8);
        S32 local_x = llclamp(mMouseDown.mX, clip.mLeft, clip.mRight);
        S32 local_y = llclamp(mMouseDown.mY, clip.mBottom, clip.mTop);
        LLUI::getInstance()->setMousePositionLocal(this, local_x, local_y);
        mPanning = false;
        static LLCachedControl<bool> auto_centre(gSavedSettings, "SKMapAutoCenter", true);
        if (auto_centre && !regionMode())
        {
            recentre();
        }
    }
    gViewerWindow->showCursor();
    gFocusMgr.setMouseCapture(nullptr);
    return true;
}

bool SKNetMap::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (mPanning || std::abs(x - mMouseDown.mX) >= MOUSE_DRAG_SLOP || std::abs(y - mMouseDown.mY) >= MOUSE_DRAG_SLOP)
        {
            if (!mPanning)
            {
                mPanning = true;
                gViewerWindow->hideCursor();
            }
            // The content follows the pointer, so the centre moves the other way.
            const F32 dx = static_cast<F32>(gViewerWindow->getCurrentMouseDX());
            const F32 dy = static_cast<F32>(gViewerWindow->getCurrentMouseDY());
            const LLVector3d now = mView.toGlobal(mView.width() * 0.5f - dx, mView.height() * 0.5f - dy);
            // Relative to the anchor, as updateView centres the map on it.
            const LLVector3d eye = viewAnchor();
            mPan.set(static_cast<F32>(now.mdV[VX] - eye.mdV[VX]), static_cast<F32>(now.mdV[VY] - eye.mdV[VY]));
            mView.set(eye + LLVector3d(mPan.mV[VX], mPan.mV[VY], 0.0), mView.span(), mView.rotation(), mView.width(), mView.height());
            gViewerWindow->moveCursorToCenter();
        }
    }
    gViewerWindow->setCursor((mask & MASK_SHIFT) || mPanning ? UI_CURSOR_TOOLPAN : UI_CURSOR_CROSS);
    return true;
}

bool SKNetMap::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // In region mode a double click first brings a zoomed or panned view back to the fit.
    if (regionMode() && regionAdjusted())
    {
        refitRegion();
        return true;
    }
    const LLVector3d pos_global = mView.toGlobal(static_cast<F32>(x), static_cast<F32>(y));
    const bool teleport = gSavedSettings.getBOOL("DoubleClickTeleport");
    const bool world_map = gSavedSettings.getBOOL("DoubleClickShowWorldMap");
    if (teleport || world_map)
    {
        // Track the spot unless something is tracked already, as the classic map does.
        if (!LLTracker::isTracking(nullptr))
        {
            if (LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance())
            {
                map->trackLocation(pos_global);
            }
        }
    }
    if (teleport)
    {
        gAgent.teleportViaLocationLookAt(pos_global);
    }
    else if (world_map)
    {
        LLFloaterReg::showInstance("world_map");
    }
    return true;
}
