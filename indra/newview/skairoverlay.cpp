/**
 * @file skairoverlay.cpp
 * @brief The air cells' debug overlays: the air by mode, wall faces, ledges and run-off, and the sky map's tops.
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

#include "skaircells.h"

#include "llagent.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "pipeline.h"
#include "skairdebug.h"
#include "skworldpalette.h"

#include <algorithm>

using namespace skworld;

namespace
{
    /// Mesh-local geometry into the overlay in agent coordinates, counting vertices against a budget.
    struct AirSink : AirDebugSink
    {
        SKWorldOverlayGeom& mGeom;
        LLVector3 mOffset;
        U32 mBudget;
        U32 mUsed = 0;

        AirSink(SKWorldOverlayGeom& geom, U32 budget) : mGeom(geom), mBudget(budget) {}

        bool spend(U32 verts)
        {
            if (mUsed + verts > mBudget)
            {
                return false;
            }
            mUsed += verts;
            return true;
        }
        bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d, const LLColor4& color) override
        {
            if (!spend(6))
            {
                return false;
            }
            mGeom.tri(a + mOffset, b + mOffset, c + mOffset, color);
            mGeom.tri(a + mOffset, c + mOffset, d + mOffset, color);
            return true;
        }
        bool line(const LLVector3& a, const LLVector3& b, const LLColor4& color) override
        {
            if (!spend(2))
            {
                return false;
            }
            mGeom.line(a + mOffset, b + mOffset, color);
            return true;
        }
    };

    F64 planDistance2(const LLVector3d& p, const LLVector3d& min, const LLVector3d& max)
    {
        const F64 dx = llmax(min.mdV[VX] - p.mdV[VX], 0.0, p.mdV[VX] - max.mdV[VX]);
        const F64 dy = llmax(min.mdV[VY] - p.mdV[VY], 0.0, p.mdV[VY] - max.mdV[VY]);
        return dx * dx + dy * dy;
    }
}

void SKAirCells::gatherAirOverlay(const LLVector3d& camera, F64 reach, U32 mode, bool opaque)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - gather air overlay");
    static LLCachedControl<U32> budget(gSavedSettings, "SKAirOverlayBudget", 1000000);
    static LLCachedControl<F32> climb(gSavedSettings, "SKAirClimb", 0.75f);

    mStats.mAirOverlayVerts = 0;
    mStats.mAirOverlayTiles = 0;
    mStats.mAirOverlayCut = 0;
    const AirDebugMode which = static_cast<AirDebugMode>(llmin(mode, static_cast<U32>(AirDebugMode::COUNT) - 1));
    // The space, stamp and indoor modes read the rooms layer, which draws them itself.
    if (which == AirDebugMode::SPACE || which == AirDebugMode::STAMP || which == AirDebugMode::INDOOR)
    {
        return;
    }

    struct Near
    {
        F64 mDistance2;
        const Mesh* mMesh;
        const Tile* mTile;
        bool mCoarse;
    };
    std::vector<Near> tiles;
    const F64 reach2 = reach * reach;
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        for (const auto& t : mesh.mTiles)
        {
            const Tile& tile = t.second;
            const F64 d2 = planDistance2(camera, tile.mMin, tile.mMax);
            if (d2 > reach2 || !tile.mAir)
            {
                continue;
            }
            // Air with no build signature is the coarse air a far tile reads.
            tiles.push_back({ d2, &mesh, &tile, tile.mAirBuilt == 0 });
        }
    }
    std::sort(tiles.begin(), tiles.end(), [](const Near& a, const Near& b) { return a.mDistance2 < b.mDistance2; });

    AirSink sink(mOverlay, budget);
    const LLColor4 far_frame(0.65f, 0.45f, 1.f, opaque ? 1.f : 0.6f);
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const Near& n = tiles[i];
        const TileAir& air = *n.mTile->mAir;
        AirDebugInput in;
        in.mMode = which;
        in.mCoarse = n.mCoarse;
        in.mClimb = air.mUnit > 0.f ? static_cast<S32>(climb / air.mUnit) : 0;
        in.mCentre = LLVector3(camera - n.mMesh->mOrigin);
        in.mReach = static_cast<F32>(reach);
        sink.mOffset = gAgent.getPosAgentFromGlobal(n.mMesh->mOrigin);
        bool ok = gatherAirDebug(air, in, sink);
        if (ok && n.mCoarse)
        {
            // Far tiles framed under the camera, so coarse air never passes for detail.
            const F32 z = static_cast<F32>(camera.mdV[VZ] - 2.0 - n.mMesh->mOrigin.mdV[VZ]);
            const LLVector3 lo(LLVector3(n.mTile->mMin - n.mMesh->mOrigin)), hi(LLVector3(n.mTile->mMax - n.mMesh->mOrigin));
            const LLVector3 c[4] = { LLVector3(lo.mV[VX], lo.mV[VY], z), LLVector3(hi.mV[VX], lo.mV[VY], z), LLVector3(hi.mV[VX], hi.mV[VY], z),
                                     LLVector3(lo.mV[VX], hi.mV[VY], z) };
            for (S32 k = 0; ok && k < 4; ++k)
            {
                ok = sink.line(c[k], c[(k + 1) % 4], far_frame);
            }
        }
        ++mStats.mAirOverlayTiles;
        if (!ok)
        {
            mStats.mAirOverlayCut = static_cast<U32>(tiles.size() - i);
            break;
        }
    }
    mStats.mAirOverlayVerts = sink.mUsed;
}

void SKAirCells::gatherSkyOverlay(const LLVector3d& camera, F64 reach)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - gather sky overlay");
    // The highest top of every metre cell a tile gives the sky map, as a translucent tile at that height.
    const LLColor4 roof(0.95f, 0.85f, 0.35f, 0.35f);
    const F64 reach2 = reach * reach;
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        const LLVector3 offset = gAgent.getPosAgentFromGlobal(mesh.mOrigin);
        for (const auto& t : mesh.mTiles)
        {
            const Tile& tile = t.second;
            const TileAirPtr& air = tile.mAir ? tile.mAir : tile.mAirCoarse;
            if (!air || planDistance2(camera, tile.mMin, tile.mMax) > reach2)
            {
                continue;
            }
            const TileTops& tops = *air->mTops;
            const F32 metres = tops.mTopCell * tops.mCell;
            for (S32 ty = 0; ty < tops.mTopHeight; ++ty)
            {
                for (S32 tx = 0; tx < tops.mTopWidth; ++tx)
                {
                    const S32 c = tx + ty * tops.mTopWidth;
                    if (tops.mTopStart[c] == tops.mTopStart[c + 1])
                    {
                        continue;
                    }
                    U16 top = 0;
                    for (U32 k = tops.mTopStart[c]; k < tops.mTopStart[c + 1]; ++k)
                    {
                        top = std::max(top, tops.mTop[k].mTop);
                    }
                    const LLVector3 lo = offset + tops.mOrigin + LLVector3(tx * metres, ty * metres, top * tops.mUnit + 0.02f);
                    const LLVector3 hi = lo + LLVector3(metres, metres, 0.f);
                    if (dist_vec_squared2D(gAgent.getPosGlobalFromAgent(lo), camera) > reach2)
                    {
                        continue;
                    }
                    mOverlay.tri(lo, LLVector3(hi.mV[VX], lo.mV[VY], lo.mV[VZ]), hi, roof);
                    mOverlay.tri(lo, hi, LLVector3(lo.mV[VX], hi.mV[VY], lo.mV[VZ]), roof);
                }
            }
        }
    }
}

void SKAirCells::refreshOverlay()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKWorldOverlayRange", 96.f);
    static LLCachedControl<U32> show_air(gSavedSettings, "SKAirOverlay", 0);
    static LLCachedControl<U32> air_budget(gSavedSettings, "SKAirOverlayBudget", 1000000);
    static LLCachedControl<bool> show_faces(gSavedSettings, "SKAirOverlayFaces", false);
    static LLCachedControl<bool> show_runoff(gSavedSettings, "SKAirOverlayRunoff", false);
    static LLCachedControl<bool> show_sky(gSavedSettings, "SKAirOverlaySky", false);

    // Drawn around the camera alone, its last fifth falling away so the edge reads as distance.
    LLViewerCamera* view = LLViewerCamera::getInstance();
    const LLVector3 camera_agent = view->getOrigin();
    const LLVector3 camera_forward = view->getAtAxis();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(camera_agent);
    const F64 reach = range;
    const F32 fov_y = view->getView();
    const S32 viewport_height = gViewerWindow ? gViewerWindow->getWorldViewHeightRaw() : 0;
    auto within = [&](const LLVector3d& p) { return dist_vec_squared2D(p, camera) <= reach * reach; };

    U64 key = SEED;
    key = mix(key, mRevision);
    key = mix(key, (show_faces ? 1ull : 0ull) | (show_runoff ? 2ull : 0ull) | (show_sky ? 4ull : 0ull));
    key = mix(key, static_cast<U64>(show_air));
    key = mix(key, static_cast<U64>(air_budget));
    key = mixF32(key, static_cast<F32>(reach));
    key = mixF32(key, fov_y);
    key = mix(key, static_cast<U64>(viewport_height));

    // The camera may move a metre and a half or turn ten degrees before the overlay is gathered again:
    // a distance and a dot, not buckets, so a camera parked at a boundary does not rebuild every frame.
    static const F32 MOVE_M = 1.5f;
    static const F32 TURN_DOT = 0.985f;
    const bool rebuild = !mOverlayFresh || key != mOverlayKey || dist_vec_squared(camera_agent, mOverlayCamera) > MOVE_M * MOVE_M
        || camera_forward * mOverlayForward < TURN_DOT;
    if (!rebuild)
    {
        return;
    }

    mOverlay.clear();
    mOverlay.setEdgeWidth(3.f, camera_agent, fov_y, viewport_height);
    mOverlay.setFade(camera_agent, static_cast<F32>(reach), static_cast<F32>(reach * 0.8));
    if (show_air)
    {
        gatherAirOverlay(camera, reach, show_air, false);
    }
    else
    {
        mStats.mAirOverlayVerts = mStats.mAirOverlayTiles = mStats.mAirOverlayCut = 0;
    }
    if (show_sky)
    {
        gatherSkyOverlay(camera, reach);
    }

    LLVector3d qmin, qmax;
    for (S32 axis = 0; axis < 3; ++axis)
    {
        qmin.mdV[axis] = camera.mdV[axis] - reach;
        qmax.mdV[axis] = camera.mdV[axis] + reach;
    }
    if (show_faces)
    {
        // The vertical surfaces themselves, the whole exposed height of them.
        const LLColor4 base = skWorldSwatchColor(SKWorldSwatch::WALL_FACE);
        wallFacesIn(qmin, qmax, [&](const LLVector3d& fmin, const LLVector3d& fmax, U8 side, U8) {
            if (!within((fmin + fmax) * 0.5))
            {
                return;
            }
            const LLVector3 a = gAgent.getPosAgentFromGlobal(fmin), b = gAgent.getPosAgentFromGlobal(fmax);
            // A face is a thin box; the thin axis is the one it faces along.
            const LLVector3 size = b - a;
            S32 thin = 0;
            for (S32 k = 1; k < 3; ++k)
            {
                thin = (size.mV[k] < size.mV[thin]) ? k : thin;
            }
            const S32 u = (thin + 1) % 3, v = (thin + 2) % 3;
            LLVector3 corner[4];
            for (S32 c = 0; c < 4; ++c)
            {
                corner[c] = a;
                corner[c].mV[thin] = (a.mV[thin] + b.mV[thin]) * 0.5f;
                corner[c].mV[u] = (c == 1 || c == 2) ? b.mV[u] : a.mV[u];
                corner[c].mV[v] = (c >= 2) ? b.mV[v] : a.mV[v];
            }
            // Which way it faces, by brightness, so a corner reads as two surfaces rather than one.
            LLColor4 color = base;
            const F32 tint = 0.7f + 0.1f * static_cast<F32>(side & 3);
            for (S32 k = 0; k < 3; ++k)
            {
                color.mV[k] *= tint;
            }
            mOverlay.tri(corner[0], corner[1], corner[2], color);
            mOverlay.tri(corner[0], corner[2], corner[3], color);
            LLColor4 line = color;
            line.mV[VALPHA] = 0.9f;
            for (S32 c = 0; c < 4; ++c)
            {
                mOverlay.line(corner[c], corner[(c + 1) % 4], line);
            }
        });
    }
    if (show_runoff)
    {
        // Where the air's floors end over a drop, and the run-off down to where it lands.
        const LLVector3 lift(0.f, 0.f, 0.06f);
        LLColor4 ledge = skWorldSwatchColor(SKWorldSwatch::LEDGE);
        ledge.mV[VALPHA] *= 0.8f;
        const LLColor4 runoff = skWorldSwatchColor(SKWorldSwatch::SUBMERGED);
        runoffIn(
            qmin, qmax,
            [&](const LLVector3d& a, const LLVector3d& b, EdgeKind, F32) {
                if (within(a))
                {
                    mOverlay.line(gAgent.getPosAgentFromGlobal(a) + lift, gAgent.getPosAgentFromGlobal(b) + lift, ledge);
                }
            },
            [&](const LLVector3d& a, const LLVector3d& b, F32 drop_a, F32 drop_b, U8) {
                if (within(a))
                {
                    const LLVector3 pa = gAgent.getPosAgentFromGlobal(a), pb = gAgent.getPosAgentFromGlobal(b);
                    mOverlay.line(pa + lift, pb + lift, runoff);
                    mOverlay.line(pa, pa - LLVector3(0.f, 0.f, drop_a), runoff);
                    mOverlay.line(pb, pb - LLVector3(0.f, 0.f, drop_b), runoff);
                }
            });
    }

    mOverlay.upload();
    mOverlayKey = key;
    mOverlayCamera = camera_agent;
    mOverlayForward = camera_forward;
    mOverlayFresh = true;
}

void SKAirCells::renderDebug()
{
    if (!overlayActive())
    {
        if (mOverlayFresh)
        {
            mOverlay.clear();
            mOverlayFresh = false;
        }
        return;
    }
    refreshOverlay();
    if (mOverlay.empty())
    {
        return;
    }
    SKWorldOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        mOverlay.draw();
    }
    SKWorldOverlayGeom::unbindShader();
}
