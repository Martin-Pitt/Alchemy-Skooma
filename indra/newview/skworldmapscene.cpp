/**
 * @file skworldmapscene.cpp
 * @brief The 3D world map's own pass: the ground of map tiles, a light tilt-shift, and the agent dots.
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

#include "llviewerprecompiledheaders.h"

#include "skworldmapscene.h"

#include "llagent.h"
#include "llfontgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llregionhandle.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "llworldmap.h"
#include "pipeline.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <limits>

using namespace skworldmap;

namespace
{
    // The map server's open water, so missing tiles read as sea rather than as holes.
    const LLColor4 SEA_COLOUR(0.114f, 0.278f, 0.373f, 1.f);
    // Past the world's edge: space, and an icy rim where the sea spills off it.
    const LLColor4 SPACE_COLOUR(0.01f, 0.015f, 0.04f, 1.f);
    const LLColor4 RIM_COLOUR(0.78f, 0.86f, 0.96f, 1.f);
    constexpr S32 EDGE_SEGMENTS = 256;
    constexpr F64 RIM_FRACTION = 0.004;     ///< the rim's width, as a fraction of the disc's radius
    constexpr F64 SPACE_REACH = 1.0e7;      ///< space is painted out to the far plane
    constexpr F64 EDGE_EASE_SECONDS = 0.35;  ///< how quickly the shown disc closes on a new fit (time constant)

    /// A flat ring on the disc's plane from radius `inner` to `outer`, relative to the eye; an inner radius
    /// of zero fills the disc.
    void edgeRing(const WorldDisc& disc, const LLVector3d& eye, F64 inner, F64 outer, const LLColor4& colour)
    {
        const F32 cx = static_cast<F32>(disc.mCentreX - eye.mdV[VX]);
        const F32 cy = static_cast<F32>(disc.mCentreY - eye.mdV[VY]);
        const F32 z = static_cast<F32>(disc.mZ - eye.mdV[VZ]);
        gGL.color4fv(colour.mV);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        for (S32 i = 0; i <= EDGE_SEGMENTS; ++i)
        {
            const F64 a = F_TWO_PI * i / EDGE_SEGMENTS;
            const F32 c = static_cast<F32>(std::cos(a));
            const F32 sn = static_cast<F32>(std::sin(a));
            gGL.vertex3f(cx + c * static_cast<F32>(outer), cy + sn * static_cast<F32>(outer), z);
            gGL.vertex3f(cx + c * static_cast<F32>(inner), cy + sn * static_cast<F32>(inner), z);
        }
        gGL.end();
    }
    // Where raised terrain uncovers its own flat footprint: a dark grey, as the shaded side of the ground.
    const LLColor4 FOOTPRINT_COLOUR(0.16f, 0.17f, 0.16f, 1.f);
    // Water drawn over terrain below it: clear at the shore, at its most opaque this deep and deeper.
    constexpr F32 WATER_OPAQUE_DEPTH = 4.f;
    constexpr F32 WATER_MAX_ALPHA = 0.9f;

    /// The grid's continents, as grid-coordinate boxes (regions, inclusive) from the SL Geography wiki
    /// (slgi.fandom.com, List Of Continents and each continent's page). Hand kept: edit when the grid changes.
    struct Continent
    {
        const char* mName;
        S32 mX0, mX1, mY0, mY1;
    };
    constexpr Continent CONTINENTS[] = {
        { "Sansara", 982, 1038, 982, 1012 },
        { "Heterocera", 991, 1031, 1012, 1036 },
        { "Jeogeot", 1007, 1039, 897, 938 },
        { "Bellisseria", 1040, 1100, 940, 1024 },
        { "Satori", 1100, 1131, 1005, 1047 },
        { "Nautilus", 1106, 1141, 1047, 1081 },
        { "Blake Sea", 1134, 1153, 1041, 1060 },
        { "Playa del Sol", 1132, 1152, 1015, 1040 }, // Estimated: east of Satori, south of Blake Sea.
        { "Corsica", 1100, 1159, 1081, 1100 },
        { "Gaeta 1", 1140, 1155, 1112, 1130 },
        { "Gaeta 5", 1158, 1189, 1081, 1099 },
        { "Sharp", 1158, 1179, 988, 1001 },
        { "Zindra", 1797, 1821, 1179, 1202 },
        { "Azure Islands", 1143, 1155, 1065, 1072 },
        { "Eden", 459, 481, 1700, 1720 },
        { "Luxory", 621, 632, 1033, 1050 },
    };

    // The combat mini-map's people dots, so both maps read the same.
    const LLColor4 DOT_COLOUR(0x60 / 255.f, 0xdf / 255.f, 0x72 / 255.f, 1.f);
    const LLColor4 SELF_COLOUR(1.f, 1.f, 1.f, 1.f);
    const LLColor4 SHADOW_COLOUR(0.f, 0.f, 0.f, 0.55f);
    constexpr F32 DOT_RADIUS_PX = 3.f;
    constexpr F32 SELF_RADIUS_PX = 3.4f;
    constexpr F32 SHADOW_EXTRA_PX = 1.6f;
    constexpr F32 STACK_STEP_PX = 5.5f;

    // Tilt-shift: a sharp band through the focus, blurring over the ramp beyond it.
    constexpr F32 SHARP_HALF_BAND = 0.10f;
    constexpr F32 BLUR_RAMP = 0.32f;
    constexpr F32 BLUR_STRENGTH = 0.9f;
    constexpr S32 BLUR_BANDS = 32;

    struct Projector
    {
        LLVector3d mEye, mForward, mRight, mUp;
        F64 mFocal = 1.0;
        F64 mHalfW = 0.0, mHalfH = 0.0;

        Projector(const SKWorldMapFrame& frame, S32 width, S32 height)
        {
            viewBasis(frame.mYaw, frame.mPitch, mForward, mRight, mUp);
            mEye = frame.mEye;
            mHalfW = width * 0.5;
            mHalfH = height * 0.5;
            mFocal = mHalfH / std::tan(frame.mFovY * 0.5);
        }

        /// Window pixels from the world view's bottom left, and the depth along the look; false behind the eye.
        bool project(const LLVector3d& p, F32& x, F32& y, F64& depth) const
        {
            const LLVector3d rel = p - mEye;
            depth = rel * mForward;
            if (depth <= 1.0e-3)
            {
                return false;
            }
            x = static_cast<F32>(mHalfW + mFocal * (rel * mRight) / depth);
            y = static_cast<F32>(mHalfH + mFocal * (rel * mUp) / depth);
            return true;
        }

        /// World up in view space (x right, y up, z toward the viewer).
        LLVector3d upInView() const { return LLVector3d(mRight.mdV[VZ], mUp.mdV[VZ], -mForward.mdV[VZ]); }
    };

    void disc(F32 x, F32 y, F32 radius, const LLColor4& colour)
    {
        constexpr S32 STEPS = 14;
        gGL.color4fv(colour.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS;
            const F32 a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.vertex2f(x, y);
            gGL.vertex2f(x + radius * std::cos(a0), y + radius * std::sin(a0));
            gGL.vertex2f(x + radius * std::cos(a1), y + radius * std::sin(a1));
        }
        gGL.end();
    }

    void ring(F32 x, F32 y, F32 radius, const LLColor4& colour)
    {
        constexpr S32 STEPS = 14;
        gGL.color4fv(colour.mV);
        gGL.begin(LLRender::LINES);
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS;
            const F32 a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.vertex2f(x + radius * std::cos(a0), y + radius * std::sin(a0));
            gGL.vertex2f(x + radius * std::cos(a1), y + radius * std::sin(a1));
        }
        gGL.end();
    }

    // A coarse location's height is in 4 m steps up to 1020 m; at 1020 the simulator is saying "higher".
    constexpr F64 COARSE_HEIGHT_UNKNOWN = 1020.0;
    const LLColor4 DROP_COLOUR(1.f, 1.f, 1.f, 0.45f);

    /// A thin line from a dot down to the ground under it, so a tilted view shows how high it is.
    void dropLine(F32 x, F32 y, F32 ground_x, F32 ground_y)
    {
        gGL.color4fv(DROP_COLOUR.mV);
        gGL.begin(LLRender::LINES);
        gGL.vertex2f(x, y);
        gGL.vertex2f(ground_x, ground_y);
        gGL.end();
        disc(ground_x, ground_y, 1.5f, DROP_COLOUR);
    }

    void quad01(F32 alpha)
    {
        gGL.color4f(1.f, 1.f, 1.f, alpha);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.texCoord2f(0.f, 0.f);
        gGL.vertex2f(0.f, 0.f);
        gGL.texCoord2f(1.f, 0.f);
        gGL.vertex2f(1.f, 0.f);
        gGL.texCoord2f(0.f, 1.f);
        gGL.vertex2f(0.f, 1.f);
        gGL.texCoord2f(1.f, 1.f);
        gGL.vertex2f(1.f, 1.f);
        gGL.end();
    }

    void pushOrtho(F32 width, F32 height)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, width, 0.f, height, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
    }

    void popMatrices()
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
    }
} // namespace

bool SKWorldMapScene::allocateTargets(S32 width, S32 height)
{
    const U32 w = static_cast<U32>(width);
    const U32 h = static_cast<U32>(height);
    if (mScene.getWidth() != w || mScene.getHeight() != h)
    {
        // Float depth: the hippo hangs tens of kilometres off, past what 24 bits resolve over this range.
        if (!mScene.allocate(w, h, GL_RGBA8, true, false, ALTextureSlot::TT_TEXTURE, LLRenderTarget::MIPS_NONE, LLRenderTarget::DEPTH_FMT_32F)
            || !mBlurA.allocate(std::max(w / 2, 1u), std::max(h / 2, 1u), GL_RGBA8)
            || !mBlurB.allocate(std::max(w / 2, 1u), std::max(h / 2, 1u), GL_RGBA8))
        {
            release();
            return false;
        }
    }
    return true;
}

void SKWorldMapScene::render(const SKWorldMapFrame& frame, const LLRect& rect, F64 now)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("world map");

    const S32 width = rect.getWidth();
    const S32 height = rect.getHeight();
    if (width <= 0 || height <= 0 || !allocateTargets(width, height))
    {
        return;
    }

    View view;
    view.mEye = frame.mEye;
    view.mYaw = frame.mYaw;
    view.mPitch = frame.mPitch;
    view.mFovY = frame.mFovY;
    view.mAspect = static_cast<F64>(width) / height;
    view.mScreenHeight = height;
    mSelected = selectTiles(view);

    pollAgents(frame, now);
    mRoads.updateSource();
    mTerrain.update(now, mRoads.source());
    mHubs.update(now);

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);

    drawGround(frame, width, height, now);
    mTiles.update(now);
    tiltShift(frame, rect);
    drawRegionNames(frame, rect);
    drawHubs(frame, rect);
    drawContinentNames(frame, rect);
    drawAgents(frame, rect);

    // Leave the viewport as the UI expects it.
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
}

void SKWorldMapScene::drawGround(const SKWorldMapFrame& frame, S32 width, S32 height, F64 now)
{
    LLVector3d f, r, u;
    viewBasis(frame.mYaw, frame.mPitch, f, r, u);
    const F64 height_above = std::max(frame.mEye.mdV[VZ] - std::max(GROUND_Z, mTerrain.groundAt(frame.mEye, GROUND_Z)), 1.0);
    const F32 z_near = static_cast<F32>(std::max(0.5, 0.25 * height_above));

    // The sea stays endless until the first coarse tiles say where the regions are. After that each new fit
    // is eased into, frame rate aside, so tiles arriving grow and tighten the disc rather than snap it.
    WorldDisc fit;
    if (!mTiles.occupiedDisc(fit))
    {
        fit.mRadius = 0.0;
    }
    if (mShownDisc.mRadius <= 0.0 || fit.mRadius <= 0.0)
    {
        mShownDisc = fit;
    }
    else
    {
        const F64 t = 1.0 - std::exp(-std::max(0.0, now - mShownAt) / EDGE_EASE_SECONDS);
        mShownDisc.mCentreX += (fit.mCentreX - mShownDisc.mCentreX) * t;
        mShownDisc.mCentreY += (fit.mCentreY - mShownDisc.mCentreY) * t;
        mShownDisc.mRadius += (fit.mRadius - mShownDisc.mRadius) * t;
    }
    mShownAt = now;
    const WorldDisc disc = mShownDisc;
    const LLColor4& clear_colour = disc.mRadius > 0.0 ? SPACE_COLOUR : SEA_COLOUR;

    mScene.bindTarget();
    glClearColor(clear_colour.mV[VRED], clear_colour.mV[VGREEN], clear_colour.mV[VBLUE], 1.f);
    {
        // render() turned depth writes off, and a depth clear obeys the write mask: without this the depth
        // buffer kept the last frame's disc and hippo, and the hippo tested against them.
        LLGLDepthTest clear_depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        mScene.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glClearColor(0.f, 0.f, 0.f, 0.f);

    LLGLDisable blend(GL_BLEND);

    // Vertices are relative to the eye, so single precision holds across the whole grid.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(al_perspective(static_cast<F32>(frame.mFovY), static_cast<F32>(width) / height, z_near,
                                  LLRender::sReverseZ ? std::numeric_limits<F32>::infinity() : 1.0e7f));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    LLVector4a eye, at, up;
    eye.clear();
    at.load3(LLVector3(f).mV);
    up.load3(LLVector3(u).mV);
    gGL.loadMatrix(LLMatrix4a::lookAt(eye, at, up));

    gUIProgram.bind();
    if (disc.mRadius > 0.0)
    {
        gGL.getTextureSlot(0)->unbind();
        edgeRing(disc, frame.mEye, 0.0, disc.mRadius, SEA_COLOUR);
    }
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    mDraws.clear();
    for (const SelectedTile& tile : mSelected)
    {
        if (mSurfaces && tile.mKey.mLevel == 1)
        {
            const U64 handle = to_region_handle(static_cast<U32>(tile.mKey.mGridX * REGION_METRES), static_cast<U32>(tile.mKey.mGridY * REGION_METRES));
            if (LLViewerTexture* surface = mSurfaces->surfaceFor(handle))
            {
                SKWorldMapTiles::Draw& d = mDraws.emplace_back();
                d.mTexture = surface;
                d.mCover = tile.mKey;
                continue;
            }
        }
        mTiles.resolve(tile, now, mDraws);
    }
    for (const SKWorldMapTiles::Draw& draw : mDraws)
    {
        const F64 x0 = draw.mCover.mGridX * REGION_METRES - frame.mEye.mdV[VX];
        const F64 y0 = draw.mCover.mGridY * REGION_METRES - frame.mEye.mdV[VY];
        const F64 w = draw.mCover.sideMetres();
        const F32 z = static_cast<F32>(GROUND_Z - frame.mEye.mdV[VZ]);
        // The part of the tile from fractions (fa, ga) to (fb, gb) of its side, flat at the ground plane.
        const auto flat = [&](F64 fa, F64 ga, F64 fb, F64 gb) {
            const F32 xa = static_cast<F32>(x0 + w * fa), xb = static_cast<F32>(x0 + w * fb);
            const F32 ya = static_cast<F32>(y0 + w * ga), yb = static_cast<F32>(y0 + w * gb);
            const F32 ua = static_cast<F32>(draw.mUV[0] + (draw.mUV[2] - draw.mUV[0]) * fa);
            const F32 ub = static_cast<F32>(draw.mUV[0] + (draw.mUV[2] - draw.mUV[0]) * fb);
            const F32 va = static_cast<F32>(draw.mUV[1] + (draw.mUV[3] - draw.mUV[1]) * ga);
            const F32 vb = static_cast<F32>(draw.mUV[1] + (draw.mUV[3] - draw.mUV[1]) * gb);
            gGL.begin(LLRender::TRIANGLE_STRIP);
            gGL.texCoord2f(ua, va);
            gGL.vertex3f(xa, ya, z);
            gGL.texCoord2f(ub, va);
            gGL.vertex3f(xb, ya, z);
            gGL.texCoord2f(ua, vb);
            gGL.vertex3f(xa, yb, z);
            gGL.texCoord2f(ub, vb);
            gGL.vertex3f(xb, yb, z);
            gGL.end();
        };

        gGL.getTextureSlot(0)->bindSampled(draw.mTexture, ALSamplers::AnisoClamp);
        flat(0.0, 0.0, 1.0, 1.0);
    }
    drawTerrain(frame, height);
    gGL.flush();
    gGL.getTextureSlot(0)->unbind();
    gUIProgram.unbind();

    mRoads.draw(frame, width, height);

    if (disc.mRadius > 0.0)
    {
        // Tiles and roads past the rim are painted over: the world ends there.
        gUIProgram.bind();
        gGL.getTextureSlot(0)->unbind();
        const F64 rim = disc.mRadius * RIM_FRACTION;
        edgeRing(disc, frame.mEye, disc.mRadius + rim, SPACE_REACH, SPACE_COLOUR);
        edgeRing(disc, frame.mEye, disc.mRadius - rim, disc.mRadius + rim, RIM_COLOUR);
        gGL.flush();

        // The disc into depth alone, so it hides whatever hangs beneath it wherever the rim covers it.
        {
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
            gGL.setColorMask(false, false);
            edgeRing(disc, frame.mEye, 0.0, disc.mRadius + rim, SPACE_COLOUR);
            gGL.flush();
            gGL.setColorMask(true, true);
        }
        gUIProgram.unbind();

        // The stars over the space painted past the rim, behind the disc's depth, so they show only past the edge.
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
            mStars.draw();
        }

        mHippo.draw(frame.mEye, disc);
    }

    popMatrices();
    mScene.flush();
}

void SKWorldMapScene::drawTerrain(const SKWorldMapFrame& frame, S32 height)
{
    // Each region with heights is drawn again over its flat tile, as a grid at its heights, with the finest
    // texture this frame drew there. The flat tile under it never shows, as the grid covers its footprint.
    LLVector3d forward, right, up;
    viewBasis(frame.mYaw, frame.mPitch, forward, right, up);
    const F64 focal = height * 0.5 / std::tan(frame.mFovY * 0.5);
    std::vector<const SKWorldMapTerrain::Region*> order;
    for (const auto& [handle, r] : mTerrain.regions())
    {
        if (r.mHeights.empty())
        {
            continue;
        }
        // Skip a region wholly behind the eye.
        bool ahead = false;
        for (S32 c = 0; c < 4 && !ahead; ++c)
        {
            const LLVector3d corner(r.mOrigin.mdV[VX] + (c & 1 ? r.mWidth : 0.0), r.mOrigin.mdV[VY] + (c & 2 ? r.mWidth : 0.0), GROUND_Z);
            ahead = (corner - frame.mEye) * forward > 0.0;
        }
        if (ahead)
        {
            order.push_back(&r);
        }
    }
    const auto centre_distance = [&](const SKWorldMapTerrain::Region* r) {
        const F64 dx = r->mOrigin.mdV[VX] + r->mWidth * 0.5 - frame.mEye.mdV[VX];
        const F64 dy = r->mOrigin.mdV[VY] + r->mWidth * 0.5 - frame.mEye.mdV[VY];
        return dx * dx + dy * dy;
    };
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return centre_distance(a) > centre_distance(b); });

    // Each raised region's footprint on the flat plane is painted dark over its flat tile, so the tile is not
    // seen twice: the strip a tilted view uncovers between the two reads as the terrain's shaded side.
    gGL.getTextureSlot(0)->unbind();
    gGL.color4fv(FOOTPRINT_COLOUR.mV);
    const F32 ground_z = static_cast<F32>(GROUND_Z - frame.mEye.mdV[VZ]);
    for (const SKWorldMapTerrain::Region* r : order)
    {
        const F32 xa = static_cast<F32>(r->mOrigin.mdV[VX] - frame.mEye.mdV[VX]);
        const F32 ya = static_cast<F32>(r->mOrigin.mdV[VY] - frame.mEye.mdV[VY]);
        const F32 xb = xa + static_cast<F32>(r->mWidth);
        const F32 yb = ya + static_cast<F32>(r->mWidth);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.vertex3f(xa, ya, ground_z);
        gGL.vertex3f(xb, ya, ground_z);
        gGL.vertex3f(xa, yb, ground_z);
        gGL.vertex3f(xb, yb, ground_z);
        gGL.end();
    }
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    for (const SKWorldMapTerrain::Region* rp : order)
    {
        const SKWorldMapTerrain::Region& r = *rp;
        const SKWorldMapTiles::Draw* best = nullptr;
        const F64 cx = r.mOrigin.mdV[VX] + r.mWidth * 0.5;
        const F64 cy = r.mOrigin.mdV[VY] + r.mWidth * 0.5;
        for (const SKWorldMapTiles::Draw& d : mDraws)
        {
            const F64 x0 = d.mCover.mGridX * REGION_METRES;
            const F64 y0 = d.mCover.mGridY * REGION_METRES;
            const F64 side = d.mCover.sideMetres();
            if (cx >= x0 && cx < x0 + side && cy >= y0 && cy < y0 + side && (!best || side < best->mCover.sideMetres()))
            {
                best = &d;
            }
        }
        if (!best)
        {
            continue;
        }
        const F64 bx = best->mCover.mGridX * REGION_METRES;
        const F64 by = best->mCover.mGridY * REGION_METRES;
        const F64 side = best->mCover.sideMetres();
        const auto texcoord = [&](F64 x, F64 y, F32& u, F32& v) {
            const F64 fu = (x - bx) / side;
            const F64 fv = (y - by) / side;
            u = static_cast<F32>(best->mUV[0] + (best->mUV[2] - best->mUV[0]) * fu);
            v = static_cast<F32>(best->mUV[1] + (best->mUV[3] - best->mUV[1]) * fv);
        };

        // Coarser where a cell would cover few pixels: every s-th sample, s a power of two dividing 32.
        constexpr S32 N = SKWorldMapTerrain::SAMPLES;
        constexpr F64 MIN_CELL_PIXELS = 6.0;
        const F64 near_x = llclamp(frame.mEye.mdV[VX], r.mOrigin.mdV[VX], r.mOrigin.mdV[VX] + r.mWidth);
        const F64 near_y = llclamp(frame.mEye.mdV[VY], r.mOrigin.mdV[VY], r.mOrigin.mdV[VY] + r.mWidth);
        const F64 nearest =
            std::max(LLVector3d(near_x - frame.mEye.mdV[VX], near_y - frame.mEye.mdV[VY], GROUND_Z - frame.mEye.mdV[VZ]).magVec(), 1.0);
        S32 s = 1;
        while (s < N - 1 && (r.mWidth / (N - 1)) * s * focal / nearest < MIN_CELL_PIXELS)
        {
            s *= 2;
        }
        const S32 M = (N - 1) / s + 1;
        const F64 step = r.mWidth / (M - 1);

        gGL.getTextureSlot(0)->bindSampled(best->mTexture, ALSamplers::AnisoClamp);
        const auto vertex = [&](S32 i, S32 j) {
            const F64 x = r.mOrigin.mdV[VX] + i * step;
            const F64 y = r.mOrigin.mdV[VY] + j * step;
            F32 u, v;
            texcoord(x, y, u, v);
            gGL.texCoord2f(u, v);
            gGL.vertex3f(static_cast<F32>(x - frame.mEye.mdV[VX]), static_cast<F32>(y - frame.mEye.mdV[VY]),
                         static_cast<F32>(r.at(i * s, j * s) - frame.mEye.mdV[VZ]));
        };

        // No depth buffer: a heightfield draws correctly far to near. Strips run across the view, the
        // farthest first, and each strip's cells also go far to near, so nearer ground paints over farther.
        const bool strips_along_y = std::abs(forward.mdV[VX]) >= std::abs(forward.mdV[VY]);
        const F64 eye_i = (frame.mEye.mdV[VX] - r.mOrigin.mdV[VX]) / step;
        const F64 eye_j = (frame.mEye.mdV[VY] - r.mOrigin.mdV[VY]) / step;
        const F64 eye_outer = strips_along_y ? eye_i : eye_j;
        const F64 eye_inner = strips_along_y ? eye_j : eye_i;
        std::array<S32, N - 1> strips;
        for (S32 k = 0; k + 1 < M; ++k)
        {
            strips[k] = k;
        }
        std::sort(strips.begin(), strips.begin() + (M - 1),
                  [&](S32 a, S32 b) { return std::abs(a + 0.5 - eye_outer) > std::abs(b + 0.5 - eye_outer); });
        // Each strip is cut where the eye is beside it, and both halves are drawn from their far end in.
        const S32 split = llclamp(static_cast<S32>(std::lround(eye_inner)), 0, M - 1);

        // The water over a strip goes down right after the strip, so it keeps the same far-to-near order:
        // clear at the shore, deepening to nearly opaque, and nothing where the land is above it.
        const auto water_alpha = [&](S32 i, S32 j) {
            const F32 depth = r.mWater - r.at(i * s, j * s);
            return depth <= 0.f ? 0.f : WATER_MAX_ALPHA * llclamp(depth / WATER_OPAQUE_DEPTH, 0.f, 1.f);
        };
        const auto water_vertex = [&](S32 i, S32 j) {
            gGL.color4f(SEA_COLOUR.mV[VRED], SEA_COLOUR.mV[VGREEN], SEA_COLOUR.mV[VBLUE], water_alpha(i, j));
            gGL.vertex3f(static_cast<F32>(r.mOrigin.mdV[VX] + i * step - frame.mEye.mdV[VX]),
                         static_cast<F32>(r.mOrigin.mdV[VY] + j * step - frame.mEye.mdV[VY]),
                         static_cast<F32>(r.mWater - frame.mEye.mdV[VZ]));
        };
        const auto strip = [&](S32 o, S32 from, S32 to, S32 dir, bool water) {
            gGL.begin(LLRender::TRIANGLE_STRIP);
            for (S32 n = from;; n += dir)
            {
                for (S32 side : { o, o + 1 })
                {
                    const S32 i = strips_along_y ? side : n;
                    const S32 j = strips_along_y ? n : side;
                    water ? water_vertex(i, j) : vertex(i, j);
                }
                if (n == to)
                {
                    break;
                }
            }
            gGL.end();
        };
        const auto wet = [&](S32 o, S32 from, S32 to, S32 dir) {
            for (S32 n = from;; n += dir)
            {
                for (S32 side : { o, o + 1 })
                {
                    if (water_alpha(strips_along_y ? side : n, strips_along_y ? n : side) > 0.f)
                    {
                        return true;
                    }
                }
                if (n == to)
                {
                    return false;
                }
            }
        };

        for (S32 k = 0; k + 1 < M; ++k)
        {
            const S32 o = strips[k];
            for (const auto& [from, to, dir] : { std::tuple{ 0, split, 1 }, std::tuple{ M - 1, split, -1 } })
            {
                if (from == to)
                {
                    continue;
                }
                strip(o, from, to, dir, false);
                if (wet(o, from, to, dir))
                {
                    LLGLEnable blend(GL_BLEND);
                    gGL.setSceneBlendType(LLRender::BT_ALPHA);
                    gGL.getTextureSlot(0)->unbind();
                    strip(o, from, to, dir, true);
                    gGL.flush();
                    gGL.color4f(1.f, 1.f, 1.f, 1.f);
                    gGL.getTextureSlot(0)->bindSampled(best->mTexture, ALSamplers::AnisoClamp);
                }
            }
        }
    }
}

void SKWorldMapScene::tiltShift(const SKWorldMapFrame& frame, const LLRect& rect)
{
    // Alchemy's depth of field is driven by scene depth and the world camera's optics inside the deferred
    // post chain, on targets renderFinalize owns; the map has neither. A tilt-shift is a band in screen
    // space, so this blurs once with the probe gaussian and blends the sharp map over it by height.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();
    {
        LLGLDisable blend(GL_BLEND);
        gGaussianProgram.bind();
        const S32 channel = gGaussianProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        const F32 bw = static_cast<F32>(mBlurA.getWidth());
        const F32 bh = static_cast<F32>(mBlurA.getHeight());
        struct Pass
        {
            LLRenderTarget* mFrom;
            LLRenderTarget* mTo;
            F32 mDx, mDy, mScale;
        };
        const Pass passes[] = {
            { &mScene, &mBlurA, 1.f, 0.f, 2.f / mScene.getWidth() },
            { &mBlurA, &mBlurB, 0.f, 1.f, 1.5f / bh },
            { &mBlurB, &mBlurA, 1.f, 0.f, 1.5f / bw },
            { &mBlurA, &mBlurB, 0.f, 1.f, 1.5f / bh },
        };
        for (const Pass& p : passes)
        {
            gGaussianProgram.uniform1f(LLShaderMgr::RES_SCALE, p.mScale);
            gGaussianProgram.uniform2f(LLShaderMgr::DIRECTION, p.mDx, p.mDy);
            gGL.getTextureSlot(channel)->bind(p.mFrom);
            p.mTo->bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            p.mTo->flush();
        }
        gGL.getTextureSlot(channel)->unbind();
        gGaussianProgram.unbind();
    }
    popMatrices();

    // The band follows the orbit's focus on screen, the middle of the screen for the fly camera.
    F32 focus_y = 0.5f;
    if (frame.mHasFocus)
    {
        const Projector proj(frame, rect.getWidth(), rect.getHeight());
        F32 x, y;
        F64 depth;
        if (proj.project(frame.mFocus, x, y, depth))
        {
            focus_y = llclamp(y / rect.getHeight(), 0.15f, 0.85f);
        }
    }
    // Looking straight down there is no near and far to tell apart, so the effect fades out.
    const F32 strength = BLUR_STRENGTH * llclamp(2.f * static_cast<F32>(std::cos(frame.mPitch)), 0.f, 1.f);

    glViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    pushOrtho(1.f, 1.f);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    {
        LLGLDisable blend(GL_BLEND);
        gGL.getTextureSlot(0)->bind(&mBlurB);
        quad01(1.f);
        gGL.flush();
    }
    {
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.getTextureSlot(0)->bind(&mScene);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        for (S32 i = 0; i <= BLUR_BANDS; ++i)
        {
            const F32 y = static_cast<F32>(i) / BLUR_BANDS;
            const F32 t = llclamp((std::fabs(y - focus_y) - SHARP_HALF_BAND) / BLUR_RAMP, 0.f, 1.f);
            const F32 blur = strength * t * t * (3.f - 2.f * t);
            gGL.color4f(1.f, 1.f, 1.f, 1.f - blur);
            gGL.texCoord2f(0.f, y);
            gGL.vertex2f(0.f, y);
            gGL.texCoord2f(1.f, y);
            gGL.vertex2f(1.f, y);
        }
        gGL.end();
        gGL.flush();
    }
    gGL.getTextureSlot(0)->unbind();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    popMatrices();
}

void SKWorldMapScene::pollAgents(const SKWorldMapFrame& frame, F64 now)
{
    // The nearest regions under fine enough tiles, as LLWorldMapView polls the regions it shows.
    std::vector<std::pair<F64, U64>> regions;
    S32 x0 = S32_MAX, y0 = S32_MAX, x1 = S32_MIN, y1 = S32_MIN;
    for (const SelectedTile& t : mSelected)
    {
        if (t.mKey.mLevel > POLL_MAX_LEVEL)
        {
            continue;
        }
        const S32 n = t.mKey.regionsPerSide();
        for (S32 i = 0; i < n; ++i)
        {
            for (S32 j = 0; j < n; ++j)
            {
                const S32 gx = t.mKey.mGridX + i;
                const S32 gy = t.mKey.mGridY + j;
                const LLVector3d centre((gx + 0.5) * REGION_METRES, (gy + 0.5) * REGION_METRES, GROUND_Z);
                regions.emplace_back((centre - frame.mEye).magVecSquared(),
                                     to_region_handle(static_cast<U32>(gx * REGION_METRES), static_cast<U32>(gy * REGION_METRES)));
            }
        }
    }
    const size_t keep = std::min(regions.size(), static_cast<size_t>(POLLED_REGIONS));
    std::partial_sort(regions.begin(), regions.begin() + keep, regions.end());
    regions.resize(keep);

    mPolled.clear();
    for (const auto& [d2, handle] : regions)
    {
        U32 x, y;
        from_region_handle(handle, &x, &y);
        const S32 gx = static_cast<S32>(x / REGION_METRES);
        const S32 gy = static_cast<S32>(y / REGION_METRES);
        x0 = std::min(x0, gx);
        y0 = std::min(y0, gy);
        x1 = std::max(x1, gx);
        y1 = std::max(y1, gy);
        mPolled.push_back(handle);
    }
    if (mPolled.empty())
    {
        return;
    }

    LLWorldMap* world_map = LLWorldMap::getInstance();
    world_map->updateRegions(x0, y0, x1, y1);
    for (U64 handle : mPolled)
    {
        if (LLSimInfo* info = world_map->simInfoFromHandle(handle))
        {
            info->updateAgentCount(now);
        }
    }
}

void SKWorldMapScene::drawAgents(const SKWorldMapFrame& frame, const LLRect& rect)
{
    const Projector proj(frame, rect.getWidth(), rect.getHeight());
    const LLVector2 dir = stackDirection(proj.upInView());
    const F32 scale = LLUI::getScaleFactor().mV[VY];
    const F32 step = STACK_STEP_PX * scale;

    struct Cluster
    {
        F32 mX, mY;
        F32 mGroundX = 0.f, mGroundY = 0.f;  ///< the ground under the dot, where it is above it
        bool mLifted = false;
        S32 mCount;
    };
    std::vector<Cluster> clusters;
    std::vector<F64> distances;
    std::vector<S32> counts;
    const auto add = [&](const LLVector3d& p, S32 count) {
        F32 x, y;
        F64 depth;
        if (!proj.project(p, x, y, depth) || x < -8.f || y < -8.f || x > rect.getWidth() + 8.f || y > rect.getHeight() + 8.f)
        {
            return;
        }
        Cluster c{ x, y };
        c.mCount = count;
        LLVector3d ground = p;
        ground.mdV[VZ] = mTerrain.groundAt(p, GROUND_Z);
        F32 gx, gy;
        F64 gdepth;
        if (p.mdV[VZ] > ground.mdV[VZ] + 2.0 && proj.project(ground, gx, gy, gdepth))
        {
            c.mGroundX = gx;
            c.mGroundY = gy;
            c.mLifted = true;
        }
        clusters.push_back(c);
        distances.push_back(depth);
        counts.push_back(count);
    };

    // People in the regions this viewer is connected to: where the avatar object is, else the coarse location.
    // The coarse height stops at 1020 m; past it the height is unknown, so the dot goes on the ground.
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == gAgentID)
        {
            continue;
        }
        LLVector3d p = positions[i];
        if (p.mdV[VZ] >= COARSE_HEIGHT_UNKNOWN)
        {
            p.mdV[VZ] = mTerrain.groundAt(p, GROUND_Z);
        }
        add(p, 1);
    }

    // Everywhere else, the map server's clusters, on the ground.
    LLWorldMap* world_map = LLWorldMap::getInstance();
    for (U64 handle : mPolled)
    {
        const LLSimInfo* info = world_map->simInfoFromHandle(handle);
        if (!info || info->isDown() || LLWorld::getInstance()->getRegionFromHandle(handle))
        {
            continue;
        }
        for (const LLItemInfo& item : info->getAgentLocation())
        {
            LLVector3d p = item.getGlobalPosition();
            p.mdV[VZ] = mTerrain.groundAt(p, GROUND_Z);
            add(p, item.getCount());
        }
    }
    const std::vector<S32> shown = stackBudget(distances, counts, DOTS_PER_STACK, DOT_BUDGET);

    glViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    pushOrtho(static_cast<F32>(rect.getWidth()), static_cast<F32>(rect.getHeight()));
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    gGL.getTextureSlot(0)->unbind();

    // Farthest first, so nearer stacks draw over them.
    std::vector<size_t> order(clusters.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distances[a] > distances[b]; });
    for (size_t i : order)
    {
        const Cluster& c = clusters[i];
        if (c.mLifted)
        {
            dropLine(c.mX, c.mY, c.mGroundX, c.mGroundY);
        }
        for (S32 k = 0; k < shown[i]; ++k)
        {
            const F32 x = c.mX + dir.mV[VX] * step * k;
            const F32 y = c.mY + dir.mV[VY] * step * k;
            disc(x, y, (DOT_RADIUS_PX + SHADOW_EXTRA_PX) * scale, SHADOW_COLOUR);
            disc(x, y, DOT_RADIUS_PX * scale, DOT_COLOUR);
        }
        if (shown[i] > 0 && c.mCount > shown[i])
        {
            const S32 top = shown[i] - 1;
            ring(c.mX + dir.mV[VX] * step * top, c.mY + dir.mV[VY] * step * top, (DOT_RADIUS_PX + 1.5f) * scale, SELF_COLOUR);
        }
    }

    // You, in white, at your own height, wherever the camera is.
    const LLVector3d self = gAgent.getPositionGlobal();
    LLVector3d self_ground = self;
    self_ground.mdV[VZ] = mTerrain.groundAt(self, GROUND_Z);
    F32 sx, sy, gx, gy;
    F64 depth, gdepth;
    if (proj.project(self, sx, sy, depth))
    {
        if (self.mdV[VZ] > self_ground.mdV[VZ] + 2.0 && proj.project(self_ground, gx, gy, gdepth))
        {
            dropLine(sx, sy, gx, gy);
        }
        disc(sx, sy, (SELF_RADIUS_PX + SHADOW_EXTRA_PX) * scale, SHADOW_COLOUR);
        disc(sx, sy, SELF_RADIUS_PX * scale, SELF_COLOUR);
    }
    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    popMatrices();
}

void SKWorldMapScene::drawRegionNames(const SKWorldMapFrame& frame, const LLRect& rect)
{
    // 0 off; 1 flat on the tile; 2 along the tile's edge, pitched to face the camera; 3 facing the camera, riding
    // a squircle inside the region toward the screen's bottom left, as the 2D map puts names bottom left.
    enum : U32 { NAMES_OFF, NAMES_FLAT, NAMES_PITCHED, NAMES_SQUIRCLE };
    static LLCachedControl<U32> mode_setting(gSavedSettings, "SKWorldMapRegionNames", NAMES_SQUIRCLE);
    static LLCachedControl<F32> inset(gSavedSettings, "SKWorldMapRegionNameInset", 12.f);
    static LLCachedControl<F32> exponent(gSavedSettings, "SKWorldMapRegionNameSquircle", 4.f);
    static LLCachedControl<F32> fade_start(gSavedSettings, "SKWorldMapRegionNameFadeStart", 2000.f);
    static LLCachedControl<F32> fade_end(gSavedSettings, "SKWorldMapRegionNameFadeEnd", 3500.f);
    static LLCachedControl<F32> world_size(gSavedSettings, "SKWorldMapRegionNameWorldSize", 12.f);
    const U32 mode = mode_setting;
    if (mode == NAMES_OFF || mode > NAMES_SQUIRCLE)
    {
        return;
    }
    const F64 above = frame.mEye.mdV[VZ] - std::max(GROUND_Z, mTerrain.groundAt(frame.mEye, GROUND_Z));
    // Each name fades with its distance from the camera, so zooming out thins them from the horizon in.
    const F64 end = std::max(static_cast<F64>(fade_end), 1.0);
    const F64 start = llclamp(static_cast<F64>(fade_start), 0.0, end - 1.0);

    constexpr size_t MAX_NAMES = 256;
    constexpr F64 LIFT = 1.0;
    const F64 half = REGION_METRES * 0.5;
    const F64 in = llclamp(static_cast<F64>(inset), 0.0, half);
    const Projector proj(frame, rect.getWidth(), rect.getHeight());
    // The ground direction toward the screen's bottom left: back along the view, and left.
    const F64 bl_x = -std::cos(frame.mYaw) - std::sin(frame.mYaw);
    const F64 bl_y = -std::sin(frame.mYaw) + std::cos(frame.mYaw);

    struct Name
    {
        const std::string* mText;
        LLVector3d mAnchor;
        F32 mX, mY;
        F64 mDepth;
        F32 mAlpha;
    };
    std::vector<Name> names;
    for (const auto& [handle, info] : LLWorldMap::getInstance()->getRegionMap())
    {
        if (!info || info->getName().empty())
        {
            continue;
        }
        const LLVector3d origin = info->getGlobalOrigin();
        const F64 cx = origin.mdV[VX] + half - frame.mEye.mdV[VX];
        const F64 cy = origin.mdV[VY] + half - frame.mEye.mdV[VY];
        if (cx * cx + cy * cy > end * end)
        {
            continue;
        }
        LLVector3d anchor;
        if (mode == NAMES_SQUIRCLE)
        {
            const LLVector2 off = squircleOffset(bl_x, bl_y, half - in, exponent);
            anchor.setVec(origin.mdV[VX] + half + off.mV[VX], origin.mdV[VY] + half + off.mV[VY], GROUND_Z);
        }
        else
        {
            anchor.setVec(origin.mdV[VX] + in, origin.mdV[VY] + in, GROUND_Z);
        }
        anchor.mdV[VZ] = std::max(GROUND_Z, mTerrain.groundAt(anchor, GROUND_Z)) + LIFT;
        const F64 distance = (anchor - frame.mEye).magVec();
        if (distance >= end)
        {
            continue;
        }
        const F32 alpha = static_cast<F32>(llclamp((end - distance) / (end - start), 0.0, 1.0));
        F32 x, y;
        F64 depth;
        if (!proj.project(anchor, x, y, depth) || x < -400.f || y < -40.f || x > rect.getWidth() + 40.f || y > rect.getHeight() + 40.f)
        {
            continue;
        }
        names.push_back({ &info->getName(), anchor, x, y, depth, alpha });
    }
    if (names.empty())
    {
        return;
    }
    std::sort(names.begin(), names.end(), [](const Name& a, const Name& b) { return a.mDepth < b.mDepth; });
    if (names.size() > MAX_NAMES)
    {
        names.resize(MAX_NAMES);
    }

    const LLFontGL::UIOrigin saved_origin = LLFontGL::sCurOrigin;
    const F32 saved_depth = LLFontGL::sCurDepth;
    LLFontGL::sCurOrigin.set(0.f, 0.f);
    LLFontGL::sCurDepth = 0.f;

    glViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    gUIProgram.bind();

    if (mode == NAMES_SQUIRCLE)
    {
        // Screen sized, nearest first; a name that would overlap a nearer one is left out.
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const F32 line = static_cast<F32>(font->getLineHeight()) * LLFontGL::sScaleY;
        pushOrtho(static_cast<F32>(rect.getWidth()), static_cast<F32>(rect.getHeight()));
        std::vector<LLRectf> placed;
        placed.reserve(names.size());
        for (const Name& n : names)
        {
            const F32 w = font->getWidthF32(*n.mText) * LLFontGL::sScaleX;
            const LLRectf box(n.mX - 2.f, n.mY + line + 2.f, n.mX + w + 2.f, n.mY - 2.f);
            if (std::any_of(placed.begin(), placed.end(), [&](const LLRectf& r) { return r.overlaps(box); }))
            {
                continue;
            }
            placed.push_back(box);
            const LLColor4 colour(1.f, 1.f, 1.f, 0.92f * n.mAlpha);
            font->renderUTF8(*n.mText, 0, n.mX / LLFontGL::sScaleX, n.mY / LLFontGL::sScaleY, colour, LLFontGL::LEFT, LLFontGL::BOTTOM,
                             LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        }
        popMatrices();
    }
    else
    {
        // In the world at a fixed size in metres, so names shrink with distance as the tiles do.
        const F32 z_near = static_cast<F32>(std::max(0.5, 0.25 * std::max(above, 1.0)));
        LLVector3d f, r, u;
        viewBasis(frame.mYaw, frame.mPitch, f, r, u);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadMatrix(al_perspective(static_cast<F32>(frame.mFovY), static_cast<F32>(rect.getWidth()) / rect.getHeight(), z_near,
                                      LLRender::sReverseZ ? std::numeric_limits<F32>::infinity() : 1.0e7f));
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        LLVector4a eye, at, up;
        eye.clear();
        at.load3(LLVector3(f).mV);
        up.load3(LLVector3(u).mV);
        gGL.loadMatrix(LLMatrix4a::lookAt(eye, at, up));

        const LLFontGL* font = LLFontGL::getFontSansSerif();
        const F32 scale = static_cast<F32>(world_size) / (static_cast<F32>(font->getLineHeight()) * LLFontGL::sScaleY);
        for (const Name& n : names)
        {
            // The text runs east along the tile: flat it lies on the ground, pitched it turns about that edge to the camera.
            const LLVector3 x_axis(1.f, 0.f, 0.f);
            LLVector3 y_axis(0.f, 1.f, 0.f);
            LLVector3 normal(0.f, 0.f, 1.f);
            if (mode == NAMES_PITCHED)
            {
                const LLVector3d to_eye = frame.mEye - n.mAnchor;
                normal.set(0.f, static_cast<F32>(to_eye.mdV[VY]), static_cast<F32>(to_eye.mdV[VZ]));
                if (normal.normVec() <= 0.f)
                {
                    normal.set(0.f, 0.f, 1.f);
                }
                y_axis = normal % x_axis;
            }
            const LLVector3 rel(n.mAnchor - frame.mEye);
            const F32 m[16] = {
                x_axis.mV[VX] * scale, x_axis.mV[VY] * scale, x_axis.mV[VZ] * scale, 0.f,
                y_axis.mV[VX] * scale, y_axis.mV[VY] * scale, y_axis.mV[VZ] * scale, 0.f,
                normal.mV[VX] * scale, normal.mV[VY] * scale, normal.mV[VZ] * scale, 0.f,
                rel.mV[VX], rel.mV[VY], rel.mV[VZ], 1.f,
            };
            const LLColor4 colour(1.f, 1.f, 1.f, 0.92f * n.mAlpha);
            gGL.pushMatrix();
            gGL.multMatrix(m);
            font->renderUTF8(*n.mText, 0, 0.f, 0.f, colour, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            gGL.popMatrix();
        }
        popMatrices();
    }

    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    LLFontGL::sCurOrigin = saved_origin;
    LLFontGL::sCurDepth = saved_depth;
}

void SKWorldMapScene::drawHubs(const SKWorldMapFrame& frame, const LLRect& rect)
{
    // A row of service glyphs over each hub region's middle, each as big as the largest craft it takes, and the
    // hub's name under it once near enough for region names. Nearest first; a row overlapping a nearer one is left out.
    static LLCachedControl<bool> enabled(gSavedSettings, "SKWorldMapHubs", true);
    static LLCachedControl<F32> fade_start(gSavedSettings, "SKWorldMapHubFadeStart", 4000.f);
    static LLCachedControl<F32> fade_end(gSavedSettings, "SKWorldMapHubFadeEnd", 8000.f);
    static LLCachedControl<F32> name_fade_start(gSavedSettings, "SKWorldMapRegionNameFadeStart", 2000.f);
    static LLCachedControl<F32> name_fade_end(gSavedSettings, "SKWorldMapRegionNameFadeEnd", 3500.f);
    if (!enabled)
    {
        return;
    }
    const std::vector<SKWorldMapHubs::Placed>& placed = mHubs.placed();
    if (placed.empty())
    {
        return;
    }
    const F64 end = std::max(static_cast<F64>(fade_end), 1.0);
    const F64 start = llclamp(static_cast<F64>(fade_start), 0.0, end - 1.0);
    const F64 name_end = std::max(static_cast<F64>(name_fade_end), 1.0);
    const F64 name_start = llclamp(static_cast<F64>(name_fade_start), 0.0, name_end - 1.0);

    constexpr size_t MAX_HUBS = 256;
    constexpr F64 LIFT = 1.0;
    const F64 half = REGION_METRES * 0.5;
    const Projector proj(frame, rect.getWidth(), rect.getHeight());
    struct Row
    {
        U32 mHub;
        F32 mX, mY;
        F64 mDepth;
        F32 mAlpha, mNameAlpha;
    };
    std::vector<Row> rows;
    for (const SKWorldMapHubs::Placed& p : placed)
    {
        const LLVector3d origin = from_region_handle(p.mHandle);
        LLVector3d centre(origin.mdV[VX] + half, origin.mdV[VY] + half, GROUND_Z);
        const F64 dx = centre.mdV[VX] - frame.mEye.mdV[VX];
        const F64 dy = centre.mdV[VY] - frame.mEye.mdV[VY];
        if (dx * dx + dy * dy > end * end)
        {
            continue;
        }
        centre.mdV[VZ] = std::max(GROUND_Z, mTerrain.groundAt(centre, GROUND_Z)) + LIFT;
        const F64 distance = (centre - frame.mEye).magVec();
        if (distance >= end)
        {
            continue;
        }
        F32 x, y;
        F64 depth;
        if (!proj.project(centre, x, y, depth) || x < -200.f || y < -60.f || x > rect.getWidth() + 200.f || y > rect.getHeight() + 60.f)
        {
            continue;
        }
        const F32 alpha = static_cast<F32>(llclamp((end - distance) / (end - start), 0.0, 1.0));
        const F32 name_alpha = static_cast<F32>(llclamp((name_end - distance) / (name_end - name_start), 0.0, 1.0));
        rows.push_back({ p.mHub, x, y, depth, alpha, name_alpha });
    }
    if (rows.empty())
    {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mDepth < b.mDepth; });
    if (rows.size() > MAX_HUBS)
    {
        rows.resize(MAX_HUBS);
    }

    // Air, heli, ship, seaplane, truck, steam train, rocket; a drop zone, which serves none of them, is a parachute.
    static const std::string GLYPHS[skworldmap::HUB_SERVICES] = {
        "\xE2\x9C\x88\xEF\xB8\x8F", "\xF0\x9F\x9A\x81", "\xF0\x9F\x9A\xA2", "\xF0\x9F\x9B\xA9\xEF\xB8\x8F",
        "\xF0\x9F\x9A\x9A", "\xF0\x9F\x9A\x82", "\xF0\x9F\x9A\x80",
    };
    static const std::string DROP_ZONE = "\xF0\x9F\xAA\x82";
    auto fontFor = [](skworldmap::HubSize size) -> const LLFontGL*
    {
        switch (size)
        {
            case skworldmap::HubSize::Large: return LLFontGL::getFontEmojiHuge();
            case skworldmap::HubSize::Medium: return LLFontGL::getFontEmojiLarge();
            default: return LLFontGL::getFontEmojiMedium();
        }
    };
    struct Glyph
    {
        const std::string* mText;
        const LLFontGL* mFont;
        F32 mWidth;
    };
    constexpr F32 GAP = 2.f;
    const LLFontGL* name_font = LLFontGL::getFontSansSerifSmall();
    const F32 name_line = static_cast<F32>(name_font->getLineHeight()) * LLFontGL::sScaleY;

    const LLFontGL::UIOrigin saved_origin = LLFontGL::sCurOrigin;
    const F32 saved_depth = LLFontGL::sCurDepth;
    LLFontGL::sCurOrigin.set(0.f, 0.f);
    LLFontGL::sCurDepth = 0.f;
    glViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    pushOrtho(static_cast<F32>(rect.getWidth()), static_cast<F32>(rect.getHeight()));

    const std::vector<skworldmap::Hub>& hubs = mHubs.table().hubs();
    std::vector<LLRectf> taken;
    std::array<Glyph, skworldmap::HUB_SERVICES> glyphs;
    for (const Row& r : rows)
    {
        const skworldmap::Hub& hub = hubs[r.mHub];
        size_t count = 0;
        F32 width = 0.f, height = 0.f;
        auto add = [&](const std::string& text, skworldmap::HubSize size)
        {
            const LLFontGL* font = fontFor(size);
            const F32 w = font->getWidthF32(text) * LLFontGL::sScaleX;
            glyphs[count++] = { &text, font, w };
            width += w + (count > 1 ? GAP : 0.f);
            height = std::max(height, static_cast<F32>(font->getLineHeight()) * LLFontGL::sScaleY);
        };
        for (size_t s = 0; s < skworldmap::HUB_SERVICES; ++s)
        {
            if (hub.mSizes[s] != skworldmap::HubSize::None)
            {
                add(GLYPHS[s], hub.mSizes[s]);
            }
        }
        if (count == 0)
        {
            add(DROP_ZONE, skworldmap::HubSize::Small);
        }
        const bool named = r.mNameAlpha > 0.f && !hub.mName.empty();
        const F32 name_w = named ? name_font->getWidthF32(hub.mName) * LLFontGL::sScaleX : 0.f;
        const F32 box_w = std::max(width, name_w);
        const LLRectf box(r.mX - box_w * 0.5f - 2.f, r.mY + height * 0.5f + 2.f, r.mX + box_w * 0.5f + 2.f,
                          r.mY - height * 0.5f - (named ? name_line : 0.f) - 2.f);
        if (std::any_of(taken.begin(), taken.end(), [&](const LLRectf& t) { return t.overlaps(box); }))
        {
            continue;
        }
        taken.push_back(box);

        const LLColor4 colour(1.f, 1.f, 1.f, r.mAlpha);
        F32 x = r.mX - width * 0.5f;
        for (size_t g = 0; g < count; ++g)
        {
            glyphs[g].mFont->renderUTF8(*glyphs[g].mText, 0, x / LLFontGL::sScaleX, r.mY / LLFontGL::sScaleY, colour, LLFontGL::LEFT,
                                        LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            x += glyphs[g].mWidth + GAP;
        }
        if (named)
        {
            const LLColor4 name_colour(1.f, 0.9f, 0.6f, 0.92f * std::min(r.mAlpha, r.mNameAlpha));
            name_font->renderUTF8(hub.mName, 0, r.mX / LLFontGL::sScaleX, (r.mY - height * 0.5f) / LLFontGL::sScaleY, name_colour,
                                  LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        }
    }

    popMatrices();
    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    LLFontGL::sCurOrigin = saved_origin;
    LLFontGL::sCurDepth = saved_depth;
}

void SKWorldMapScene::drawContinentNames(const SKWorldMapFrame& frame, const LLRect& rect)
{
    // Large names over each continent's middle, fading in as the camera climbs past where region names thin out.
    static LLCachedControl<bool> enabled(gSavedSettings, "SKWorldMapContinentNames", true);
    static LLCachedControl<F32> fade_in(gSavedSettings, "SKWorldMapContinentNameFadeIn", 2500.f);
    static LLCachedControl<F32> full(gSavedSettings, "SKWorldMapContinentNameFull", 4500.f);
    if (!enabled)
    {
        return;
    }
    const F64 above = frame.mEye.mdV[VZ] - std::max(GROUND_Z, mTerrain.groundAt(frame.mEye, GROUND_Z));
    const F64 hi = std::max(static_cast<F64>(full), 1.0);
    const F64 lo = llclamp(static_cast<F64>(fade_in), 0.0, hi - 1.0);
    const F32 alpha = static_cast<F32>(llclamp((above - lo) / (hi - lo), 0.0, 1.0));
    if (alpha <= 0.f)
    {
        return;
    }

    const Projector proj(frame, rect.getWidth(), rect.getHeight());
    const LLFontGL* font = LLFontGL::getFontSansSerifHuge();
    struct Label
    {
        const char* mText;
        F32 mX, mY;
        F64 mDepth;
    };
    std::vector<Label> labels;
    for (const Continent& c : CONTINENTS)
    {
        LLVector3d centre((c.mX0 + c.mX1 + 1) * 0.5 * REGION_METRES, (c.mY0 + c.mY1 + 1) * 0.5 * REGION_METRES, GROUND_Z);
        centre.mdV[VZ] = std::max(GROUND_Z, mTerrain.groundAt(centre, GROUND_Z));
        F32 x, y;
        F64 depth;
        if (proj.project(centre, x, y, depth) && x > -600.f && y > -60.f && x < rect.getWidth() + 600.f && y < rect.getHeight() + 60.f)
        {
            labels.push_back({ c.mName, x, y, depth });
        }
    }
    if (labels.empty())
    {
        return;
    }
    std::sort(labels.begin(), labels.end(), [](const Label& a, const Label& b) { return a.mDepth < b.mDepth; });

    const LLFontGL::UIOrigin saved_origin = LLFontGL::sCurOrigin;
    const F32 saved_depth = LLFontGL::sCurDepth;
    LLFontGL::sCurOrigin.set(0.f, 0.f);
    LLFontGL::sCurDepth = 0.f;
    glViewport(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    gUIProgram.bind();
    pushOrtho(static_cast<F32>(rect.getWidth()), static_cast<F32>(rect.getHeight()));

    const LLColor4 colour(1.f, 0.96f, 0.86f, 0.9f * alpha);
    const F32 line = static_cast<F32>(font->getLineHeight()) * LLFontGL::sScaleY;
    std::vector<LLRectf> placed;
    for (const Label& l : labels)
    {
        const F32 w = font->getWidthF32(l.mText) * LLFontGL::sScaleX;
        const LLRectf box(l.mX - w * 0.5f - 4.f, l.mY + line * 0.5f + 4.f, l.mX + w * 0.5f + 4.f, l.mY - line * 0.5f - 4.f);
        if (std::any_of(placed.begin(), placed.end(), [&](const LLRectf& r) { return r.overlaps(box); }))
        {
            continue;
        }
        placed.push_back(box);
        font->renderUTF8(l.mText, 0, l.mX / LLFontGL::sScaleX, l.mY / LLFontGL::sScaleY, colour, LLFontGL::HCENTER, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
    }

    popMatrices();
    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, true);
    LLFontGL::sCurOrigin = saved_origin;
    LLFontGL::sCurDepth = saved_depth;
}

void SKWorldMapScene::release()
{
    mHippo.release();
    mStars.release();
    mScene.release();
    mBlurA.release();
    mBlurB.release();
    mTiles.dropPriorities();
    mPolled.clear();
    mSelected.clear();
}

void SKWorldMapScene::shutdown()
{
    release();
    mTiles.clear();
    mRoads.release();
    mTerrain.clear();
}
