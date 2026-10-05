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
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "llworldmapview.h"
#include "llavatarnamecache.h"
#include "llkeyboard.h"
#include "llparcel.h"
#include "llslurl.h"
#include "llviewerparcelmgr.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "skmapmarkers.h"
#include "skmapobjects.h"
#include "skmapparcels.h"
#include "skmappeople.h"
#include "skmapmenu.h"
#include "skmapscene.h"
#include "skpopuppanes.h"

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

    void shadow(const LLVector2& centre, F32 radius)
    {
        LLViewerTexture* sprite = shadowSprite();
        if (!sprite)
        {
            return;
        }
        const F32 r = radius + skmap::palette::SHADOW_BLUR_PX;
        gGL.getTextureSlot(0)->bindSampled(sprite, ALSamplers::BilinearClamp);
        colour(skmap::palette::SHADOW);
        gGL.begin(LLRender::TRIANGLES);
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
        gGL.end();
        gGL.getTextureSlot(0)->unbind();
    }

    void releaseTextures()
    {
        sShadowSprite = nullptr;
    }
}

SKNetMap::SKNetMap(const Params& p)
    : LLUICtrl(p),
      mObjects(std::make_unique<SKMapObjects>()),
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
    static LLCachedControl<bool> rotate(gSavedSettings, "SKMapRotate", false);

    if (mEaseStart >= 0.0)
    {
        const F64 t = (LLFrameTimer::getElapsedSeconds() - mEaseStart) / RECENTRE_S;
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

    F32 rotation = 0.f;
    if (rotate)
    {
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        rotation = skmap::MapView::rotationForHeading(std::atan2(at.mV[VY], at.mV[VX]));
    }
    const LLVector3d centre = gAgent.getPositionGlobal() + LLVector3d(mPan.mV[VX], mPan.mV[VY], 0.0);
    mView.set(centre, span, rotation, static_cast<F32>(getRect().getWidth()), static_cast<F32>(getRect().getHeight()));
}

void SKNetMap::recentre()
{
    if (isPanned())
    {
        mEaseFrom = mPan;
        mEaseStart = LLFrameTimer::getElapsedSeconds();
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
        drawTerrain();
        drawObjects();
        SKMapScene::instance().drawFloorplan(mView);
        SKMapScene::instance().drawVoid(mView);
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
    }
    // Tags sit wherever there is room in the window, over the map or the world.
    drawTags();
    mDrawMs = draw_timer.getElapsedTimeF32() * 1000.f;
    drawReadout();

    LLUICtrl::draw();
}

void SKNetMap::drawTerrain()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKMapLayerTerrain", true);
    if (!show)
    {
        return;
    }
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const LLVector3d origin = region->getOriginGlobal();
        const F64 w = region->getWidth();
        const LLVector2 sw = mView.toWidget(origin);
        const LLVector2 se = mView.toWidget(origin + LLVector3d(w, 0.0, 0.0));
        const LLVector2 ne = mView.toWidget(origin + LLVector3d(w, w, 0.0));
        const LLVector2 nw = mView.toWidget(origin + LLVector3d(0.0, w, 0.0));
        const F32 grey = skmap::palette::TERRAIN_GREY;
        // Dead regions keep the classic map's red tint.
        const LLColor4 tint = region->isAlive() ? LLColor4(grey, grey, grey, skmap::palette::TERRAIN_ALPHA)
                                                : LLColor4(grey, grey * 0.5f, grey * 0.5f, skmap::palette::TERRAIN_ALPHA);
        gGL.getTextureSlot(0)->bindSampled(region->getLand().getSTexture(), ALSamplers::BilinearClamp);
        gGL.color4fv(tint.mV);
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
}

void SKNetMap::drawObjects()
{
    static LLCachedControl<U32> mode(gSavedSettings, "SKMapLayerObjects", 1);
    if (mode == 0)
    {
        mObjects->release();
        return;
    }
    mObjects->update(mView, SKMapPeople::instance().combat());
    if (mode >= 2)
    {
        mObjects->draw(mView, nullptr);
        return;
    }
    // Only in the 32 m cells with no published navmesh tile.
    const SKMapScene& scene = SKMapScene::instance();
    mObjects->draw(mView, [&scene](S32 x, S32 y) { return scene.built(x, y); });
}

void SKNetMap::drawSelf()
{
    const LLVector2 self = mView.toWidget(gAgent.getPositionGlobal());
    // The sight cone follows the camera's horizontal field of view, a fixed size on screen.
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
            gGL.vertex2f(self.mV[VX], self.mV[VY]);
            gGL.color4fv(outer.mV);
            gGL.vertex2f(self.mV[VX] + std::cos(a0) * r, self.mV[VY] + std::sin(a0) * r);
            gGL.vertex2f(self.mV[VX] + std::cos(a1) * r, self.mV[VY] + std::sin(a1) * r);
        }
        gGL.end();
    }
    skmapdraw::shadow(self, skmap::palette::SELF_DOT_RADIUS_PX);
    gGL.begin(LLRender::TRIANGLES);
    skmapdraw::colour(skmap::palette::SELF_DOT);
    skmapdraw::disc(self, skmap::palette::SELF_DOT_RADIUS_PX);
    gGL.end();
}

void SKNetMap::drawSelfExtras()
{
    static LLCachedControl<bool> chat_rings(gSavedSettings, "SKMapLayerChatRings", false);
    static LLCachedControl<bool> guide_line(gSavedSettings, "SKMapLayerGuideLine", false);
    const LLVector2 self = mView.toWidget(gAgent.getPositionGlobal());
    const sknav::Cover cover = SKMapPeople::instance().coverAhead();
    if (cover != sknav::Cover::NONE)
    {
        // An arc in front of the dot, in the colour of the cover a wall ahead gives.
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        LLVector2 ahead = mView.toWidgetOffset(at.mV[VX], at.mV[VY]);
        if (ahead.normVec() > 1e-6f)
        {
            const F32 heading = std::atan2(ahead.mV[VY], ahead.mV[VX]);
            const F32 r = skmap::palette::COVER_AHEAD_OFFSET_PX;
            const F32 half = skmap::palette::COVER_AHEAD_PX / r;
            gGL.begin(LLRender::TRIANGLES);
            skmapdraw::colour(cover == sknav::Cover::FULL ? skmap::palette::COVER_AHEAD_FULL : skmap::palette::COVER_AHEAD_HALF);
            const S32 steps = 8;
            for (S32 i = 0; i < steps; ++i)
            {
                const F32 a0 = heading - half + 2.f * half * i / steps, a1 = heading - half + 2.f * half * (i + 1) / steps;
                skmapdraw::line(self + LLVector2(std::cos(a0), std::sin(a0)) * r, self + LLVector2(std::cos(a1), std::sin(a1)) * r,
                                cover == sknav::Cover::FULL ? skmap::palette::COVER_FULL_PX : skmap::palette::COVER_HALF_PX);
            }
            gGL.end();
        }
    }
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
    if (guide_line)
    {
        // Along the camera, out to its far clip.
        static LLUIColor line_colour = LLUIColorTable::instance().getColor("MapLineColor", LLColor4::red);
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 at = camera->getAtAxis();
        LLVector2 dir = mView.toWidgetOffset(at.mV[VX], at.mV[VY]);
        if (dir.normVec() > 1e-6f)
        {
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(line_colour.get().mV);
            skmapdraw::line(self, self + dir * (camera->getFar() * mView.pixelsPerMetre()), 1.f);
            gGL.end();
        }
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
    std::vector<skmap::EventRing::Draw> draws;
    people.events().visible(LLFrameTimer::getElapsedSeconds(), line_s, icon_s, people.combat(), draws);
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
    gGL.begin(LLRender::TRIANGLES);
    for (const SKMapPeople::Projectile& p : shots)
    {
        const LLVector2 head = mView.toWidget(p.mPos);
        if (p.mFast)
        {
            // A streak and a bright head.
            skmapdraw::colour(skmap::palette::PROJECTILE, 0.5f);
            skmapdraw::line(mView.toWidget(p.mTail), head, 1.f);
            skmapdraw::colour(skmap::palette::PROJECTILE);
            skmapdraw::disc(head, 1.5f, 8);
        }
        else
        {
            // A thrown object: a dot and a ring.
            skmapdraw::colour(skmap::palette::PROJECTILE);
            skmapdraw::disc(head, 1.5f, 8);
            skmapdraw::colour(skmap::palette::PROJECTILE, 0.6f);
            skmapdraw::ring(head, 4.f, 1.f, 16);
        }
    }
    gGL.end();
}

void SKNetMap::drawPings()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
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
    gGL.begin(LLRender::TRIANGLES);
    for (const SKMapPeople::Vehicle& v : SKMapPeople::instance().vehicles())
    {
        if (SKMapVehicleGlyphs::glyphFor(v.mRoot))
        {
            continue;   // no glyph is drawn yet; the seam is for the outline capture
        }
        // The linkset's box at its live heading: hull, and a nose tick at its front.
        auto at = [&](F32 x, F32 y) {
            const LLVector3 p = LLVector3(x, y, 0.f) * v.mRot;
            return mView.toWidget(v.mPos + LLVector3d(p.mV[VX], p.mV[VY], 0.0));
        };
        const LLVector2 a = at(v.mMin.mV[VX], v.mMin.mV[VY]), b = at(v.mMax.mV[VX], v.mMin.mV[VY]);
        const LLVector2 c = at(v.mMax.mV[VX], v.mMax.mV[VY]), d = at(v.mMin.mV[VX], v.mMax.mV[VY]);
        const bool ally = v.mLook == skmap::VehicleLook::ALLY;
        const bool neutral = v.mLook == skmap::VehicleLook::NEUTRAL;
        skmapdraw::colour(ally ? skmap::palette::VEH_ALLY_HULL : neutral ? skmap::palette::VEH_NEUTRAL_HULL : skmap::palette::VEH_CIVIL_HULL);
        skmapdraw::triangle(a, b, c);
        skmapdraw::triangle(a, c, d);
        const F32 mid_y = (v.mMin.mV[VY] + v.mMax.mV[VY]) * 0.5f;
        const F32 length = v.mMax.mV[VX] - v.mMin.mV[VX];
        skmapdraw::colour(ally ? skmap::palette::VEH_ALLY_TOP : neutral ? skmap::palette::VEH_NEUTRAL_TOP : skmap::palette::VEH_CIVIL_TOP);
        skmapdraw::line(at(v.mMax.mV[VX] - length * 0.25f, mid_y), at(v.mMax.mV[VX], mid_y), 2.f);
    }
    gGL.end();
}

const LLFontGL* SKNetMap::codeFont()
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

void SKNetMap::collectDots()
{
    static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
    static LLCachedControl<bool> labels(gSavedSettings, "SKMapMarkerLabels", true);
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    static LLCachedControl<F32> padding(gSavedSettings, "SKMapMarkerPaddingPx", 0.5f);
    const F32 px = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
    const F32 pad = llclamp(static_cast<F32>(padding), 0.f, 3.f);
    mDots.clear();
    for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
    {
        const bool chevron = chevrons && (p.mFloor == skmap::FloorRelation::ABOVE || p.mFloor == skmap::FloorRelation::BELOW);
        const bool coded = labels && !p.mCode.empty();
        const F32 text_w = coded ? codeFont()->getWidthF32(p.mCode) : 0.f;
        mDots.push_back({ p.mId, mView.toWidget(p.mPos),
                          skmap::markerRadius(chevron, coded, static_cast<U32>(p.mCode.size()), text_w, px, pad) });
    }
}

void SKNetMap::drawPeople()
{
    static LLCachedControl<bool> triangles(gSavedSettings, "SKMapHeadingTriangles", true);
    static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
    static LLCachedControl<U32> font_px(gSavedSettings, "SKMapMarkerFontPx", 8);
    const F32 FONT_PX = llclamp(static_cast<F32>(font_px), 8.f, 11.f);
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    for (size_t i = 0; i < people.size() && i < mDots.size(); ++i)
    {
        const SKMapPeople::Person& p = people[i];
        const LLVector2 at = mDots[i].mPos;
        const F32 radius = mDots[i].mRadius;
        const bool chevron = chevrons && (p.mFloor == skmap::FloorRelation::ABOVE || p.mFloor == skmap::FloorRelation::BELOW);
        if (at.mV[VX] < -radius - 8.f || at.mV[VY] < -radius - 8.f || at.mV[VX] > mView.width() + radius + 8.f
            || at.mV[VY] > mView.height() + radius + 8.f)
        {
            continue;
        }
        skmapdraw::shadow(at, radius);
        gGL.begin(LLRender::TRIANGLES);
        // Allies in combat are cyan; everyone else keeps the general colours.
        const bool ally_coded = p.mVisibility == skmap::Visibility::DOT_CODED;
        if (ally_coded)
        {
            skmapdraw::colour(skmap::palette::DOT_COMBAT);
        }
        else
        {
            gGL.color4f(p.mColour.mV[0], p.mColour.mV[1], p.mColour.mV[2], 1.f);
        }
        if (triangles && p.mHasHeading)
        {
            LLVector2 ahead = mView.toWidgetOffset(std::cos(p.mHeading), std::sin(p.mHeading));
            ahead.normVec();
            const LLVector2 across(-ahead.mV[VY], ahead.mV[VX]);
            const LLVector2 base = at + ahead * (radius + skmap::palette::HEADING_BASE_PX);
            skmapdraw::triangle(at + ahead * (radius + skmap::palette::HEADING_TIP_PX), base + across * skmap::palette::HEADING_HALF_PX,
                                base - across * skmap::palette::HEADING_HALF_PX);
        }
        skmapdraw::disc(at, radius);
        if (chevron)
        {
            // Upright whatever the rotation: up for the floor above, down for the one below.
            const F32 half = llmin(radius - skmap::palette::CHEVRON_STROKE_PX, FONT_PX * 0.3f);
            const F32 dy = half * 0.55f;
            const F32 sign = p.mFloor == skmap::FloorRelation::ABOVE ? 1.f : -1.f;
            skmapdraw::colour(skmap::palette::CODE_INK);
            const LLVector2 left = at + LLVector2(-half, -sign * dy), apex = at + LLVector2(0.f, sign * dy), right = at + LLVector2(half, -sign * dy);
            skmapdraw::line(left, apex, skmap::palette::CHEVRON_STROKE_PX);
            skmapdraw::line(apex, right, skmap::palette::CHEVRON_STROKE_PX);
        }
        gGL.end();
    }
}

void SKNetMap::drawPeopleCodes()
{
    static LLCachedControl<bool> chevrons(gSavedSettings, "SKMapFloorChevrons", true);
    static LLCachedControl<bool> labels(gSavedSettings, "SKMapMarkerLabels", true);
    if (!labels)
    {
        return;
    }
    const LLColor4 ink(skmap::palette::CODE_INK.mR, skmap::palette::CODE_INK.mG, skmap::palette::CODE_INK.mB, 1.f);
    const std::vector<SKMapPeople::Person>& people = SKMapPeople::instance().people();
    for (size_t i = 0; i < people.size() && i < mDots.size(); ++i)
    {
        const SKMapPeople::Person& p = people[i];
        const bool chevron = chevrons && (p.mFloor == skmap::FloorRelation::ABOVE || p.mFloor == skmap::FloorRelation::BELOW);
        // The chevron replaces the code on the dot; the tag keeps it.
        if (chevron || p.mVisibility != skmap::Visibility::DOT_CODED || p.mCode.empty())
        {
            continue;
        }
        const LLVector2 at = mDots[i].mPos;
        codeFont()->renderUTF8(p.mCode, 0, at.mV[VX], at.mV[VY], ink, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL,
                               LLFontGL::NO_SHADOW);
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

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const F32 widest = skmap::tagMaxWidth(win_w, static_cast<F32>(max_width));
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
    for (const LLUUID& id : mHover.members())
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
    static LLCachedControl<bool> code_labels(gSavedSettings, "SKMapMarkerLabels", true);
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
            for (const SKMapPeople::Person& p : people)
            {
                if (p.mId != t.mId)
                {
                    continue;
                }
                detail = p.mFloor == skmap::FloorRelation::ABOVE   ? mFloorAboveLabel
                       : p.mFloor == skmap::FloorRelation::BELOW   ? mFloorBelowLabel
                       : p.mFloor == skmap::FloorRelation::UNKNOWN ? mFloorUnknownLabel
                                                                    : std::string();
                break;
            }
        }
        const F32 code_w = t.mCode.empty() ? 0.f : font->getWidthF32(t.mCode) + skmap::palette::TAG_CODE_GAP_PX;
        t.mName = fit(t.mName, text_room - code_w);
        t.mDetail = detail.empty() ? std::string() : fit(detail, text_room);
        const F32 text_w = llmax(code_w + font->getWidthF32(t.mName), t.mDetail.empty() ? 0.f : font->getWidthF32(t.mDetail));
        t.mWidth = std::ceil(text_w + 2.f * skmap::palette::TAG_PAD_X_PX);
        t.mHeight = std::ceil(font->getLineHeight() * (t.mDetail.empty() ? 1.f : 2.f) + 2.f * skmap::palette::TAG_PAD_Y_PX);
    }
    mTagTexts.swap(texts);

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
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLColor4 bg(skmap::palette::TAG_BG.mR, skmap::palette::TAG_BG.mG, skmap::palette::TAG_BG.mB, skmap::palette::TAG_BG.mA);
    const LLColor4 text(skmap::palette::TAG_TEXT.mR, skmap::palette::TAG_TEXT.mG, skmap::palette::TAG_TEXT.mB, 1.f);
    const LLColor4 code(skmap::palette::TAG_CODE.mR, skmap::palette::TAG_CODE.mG, skmap::palette::TAG_CODE.mB, 1.f);
    const LLColor4 detail(skmap::palette::TAG_DETAIL.mR, skmap::palette::TAG_DETAIL.mG, skmap::palette::TAG_DETAIL.mB, 1.f);
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
        const LLRect r = tagRect(tag);
        gl_rect_2d(r, bg, true);
        F32 x = r.mLeft + skmap::palette::TAG_PAD_X_PX;
        const F32 y = r.mTop - skmap::palette::TAG_PAD_Y_PX;
        if (!t->mCode.empty())
        {
            font->renderUTF8(t->mCode, 0, x, y, code, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            x += font->getWidthF32(t->mCode) + skmap::palette::TAG_CODE_GAP_PX;
        }
        font->renderUTF8(t->mName, 0, x, y, text, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        if (!t->mDetail.empty())
        {
            font->renderUTF8(t->mDetail, 0, r.mLeft + skmap::palette::TAG_PAD_X_PX, y - font->getLineHeight(), detail, LLFontGL::LEFT,
                             LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
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

void SKNetMap::drawReadout()
{
    static LLCachedControl<bool> readout(gSavedSettings, "SKMapDebugReadout", false);
    if (!readout)
    {
        return;
    }
    const SKMapScene::Stats& stats = SKMapScene::instance().stats();
    std::vector<std::string> lines;
    lines.push_back(llformat("draw %.2f ms, cells %u, queue %u, uploads %u, cache %.1f MB", mDrawMs, stats.mCells, stats.mQueued,
                             stats.mUploads, stats.mCacheMB));
    lines.push_back(llformat("scene %u polys, %u layers, job %.1f ms (%u)", stats.mPolys, stats.mLayers, stats.mJobMs, stats.mJobs));
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
    std::vector<SKMapMenu::Person> people;
    for (const LLUUID& id : ids)
    {
        SKMapMenu::Person person;
        person.mId = id;
        person.mNameHidden = !RlvActions::canShowName(RlvActions::SNC_DEFAULT, id);
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(id, &av_name))
        {
            person.mLabel = person.mNameHidden ? RlvStrings::getAnonym(av_name) : av_name.getDisplayName();
        }
        for (const SKMapPeople::Person& p : SKMapPeople::instance().people())
        {
            if (p.mId == id)
            {
                person.mPos = p.mPos;
                break;
            }
        }
        people.push_back(person);
    }
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
    // Positive clicks zoom out, as the classic map.
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
        if (auto_centre)
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
            const LLVector3d agent = gAgent.getPositionGlobal();
            mPan.set(static_cast<F32>(now.mdV[VX] - agent.mdV[VX]), static_cast<F32>(now.mdV[VY] - agent.mdV[VY]));
            mView.set(agent + LLVector3d(mPan.mV[VX], mPan.mV[VY], 0.0), mView.span(), mView.rotation(), mView.width(), mView.height());
            gViewerWindow->moveCursorToCenter();
        }
    }
    gViewerWindow->setCursor((mask & MASK_SHIFT) || mPanning ? UI_CURSOR_TOOLPAN : UI_CURSOR_CROSS);
    return true;
}

bool SKNetMap::handleDoubleClick(S32 x, S32 y, MASK mask)
{
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
