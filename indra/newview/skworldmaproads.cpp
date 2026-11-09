/**
 * @file skworldmaproads.cpp
 * @brief The 3D world map's road layer.
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

#include "skworldmaproads.h"

#include "llcontrol.h"
#include "llglslshader.h"
#include "llregionhandle.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "skworldmapscene.h"

#include <simdjson.h>

#include <algorithm>
#include <cmath>

using namespace skworldmap;

namespace
{
    // The style prototype's values (V:\Scratch\worldmap\map\proto-roads).
    const LLColor4 PUBLIC_FILL(0xff / 255.f, 0xd4 / 255.f, 0x8a / 255.f, 1.f);
    const LLColor4 PUBLIC_CASING(0xff / 255.f, 0xe1 / 255.f, 0xa8 / 255.f, 1.f);
    const LLColor4 PRIVATE_FILL(0xc7 / 255.f, 0xd1 / 255.f, 0xdc / 255.f, 1.f);
    constexpr F32 PUBLIC_MIN_PX = 2.8f;
    constexpr F32 PRIVATE_MIN_PX = 1.3f;
    constexpr F32 PRIVATE_OPACITY = 0.6f;
    constexpr F32 PRIVATE_FADE_FROM = 8.f;      ///< private roads fade out between these scales, in metres per pixel
    constexpr F32 PRIVATE_FADE_TO = 32.f;
    constexpr F32 CASING_PX = 1.5f;
    constexpr F32 CASING_OPACITY = 0.95f;
    constexpr F32 OUTLINED_FILL_OPACITY = 0.16f;
    constexpr F32 ASSUMED_WIDTH = 6.f;          ///< the schema's explicit visualisation width where none is known

    F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    F32 scaleLog2(F32 x) { return std::log2(std::max(x, 1.0e-6f)); }

    constexpr F32 MITER_LIMIT = 2.f;            ///< a sharper bend than this stretch is cut short rather than spiking out

    F64 planeDistanceSquared(const LLVector3d& a, const LLVector3d& b)
    {
        const F64 dx = b.mdV[VX] - a.mdV[VX];
        const F64 dy = b.mdV[VY] - a.mdV[VY];
        return dx * dx + dy * dy;
    }

    /// The unit vector across a segment on the ground, to its left.
    LLVector3 planeSide(const LLVector3d& a, const LLVector3d& b)
    {
        LLVector3 side(static_cast<F32>(-(b.mdV[VY] - a.mdV[VY])), static_cast<F32>(b.mdV[VX] - a.mdV[VX]), 0.f);
        side.normVec();
        return side;
    }

    /// Where two segments' sides meet at their shared point, scaled so both edges keep their width.
    LLVector3 miter(const LLVector3& before, const LLVector3& after)
    {
        LLVector3 m = before + after;
        if (m.normVec() < 1.0e-4f)
        {
            return after;   // a full turn back: no join to make
        }
        const F32 cosine = m * after;
        return m * std::min(1.f / std::max(cosine, 1.0e-3f), MITER_LIMIT);
    }

    /// One end of a segment as drawn: eye-relative position, its scale, and its weights.
    struct End
    {
        LLVector3 mPos;
        F32 mHalf = 0.f;        ///< half the fill's width, metres
        F32 mCasing = 0.f;      ///< the casing's width beyond the fill, metres
        F32 mFillAlpha = 0.f;
        F32 mCasingAlpha = 0.f;
    };

    void quad(const LLVector3& a0, const LLVector3& a1, const LLVector3& b0, const LLVector3& b1, const LLColor4& colour, F32 alpha_a,
              F32 alpha_b)
    {
        gGL.color4f(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE], alpha_a);
        gGL.vertex3fv(a0.mV);
        gGL.vertex3fv(a1.mV);
        gGL.color4f(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE], alpha_b);
        gGL.vertex3fv(b0.mV);
        gGL.vertex3fv(b0.mV);
        gGL.color4f(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE], alpha_a);
        gGL.vertex3fv(a1.mV);
        gGL.color4f(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE], alpha_b);
        gGL.vertex3fv(b1.mV);
    }
} // namespace

F32 RoadStyle::solidWeight(F32 metres_per_pixel) const
{
    const F32 half = std::max(mOctaves, 0.01f) * 0.5f;
    const F32 c = scaleLog2(mSolidCentre);
    return smoothstep(c - half, c + half, scaleLog2(metres_per_pixel));
}

SKWorldMapDatasetRoads::SKWorldMapDatasetRoads(const std::string& release_dir) : mDir(release_dir)
{
    mLoading = std::async(std::launch::async, &SKWorldMapDatasetRoads::load, mDir);
}

SKWorldMapDatasetRoads::~SKWorldMapDatasetRoads()
{
    if (mLoading.valid())
    {
        mLoading.wait();
    }
}

void SKWorldMapDatasetRoads::poll() { if (mLoading.valid() && mLoading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { mRegions = mLoading.get(); size_t segments = 0; for (const auto& [handle, region] : mRegions) { segments += region.mSegments.size(); } LL_INFOS("SKWorldMap") << "roads: " << segments << " segments in " << mRegions.size() << " regions from " << mDir << LL_ENDL; } }  void SKWorldMapDatasetRoads::forEachRegion(const std::function<void(U64, const RoadRegion&)>& fn) { for (const auto& [handle, region] : mRegions) { fn(handle, region); } }  const RoadRegion* SKWorldMapDatasetRoads::region(U64 handle) const { const auto it = mRegions.find(handle); return it == mRegions.end() ? nullptr : &it->second; }

SKWorldMapDatasetRoads::Regions SKWorldMapDatasetRoads::load(std::string dir)
{
    Regions regions;
    const struct
    {
        const char* mFile;
        RoadClass mClass;
    } layers[] = {
        { "/core/ways.geojson", RoadClass::Public },
        { "/private/ways.geojson", RoadClass::Private },
        { "/candidates/ways.geojson", RoadClass::Candidate },
    };

    simdjson::dom::parser parser;
    std::vector<LLVector3d> points;
    std::vector<LLVector3> sides;
    for (const auto& layer : layers)
    {
        const std::string path = dir + layer.mFile;
        simdjson::padded_string json;
        if (simdjson::padded_string::load(path).get(json))
        {
            LL_WARNS("SKWorldMap") << "roads: could not read " << path << LL_ENDL;
            continue;
        }
        simdjson::dom::array features;
        if (parser.parse(json)["features"].get_array().get(features))
        {
            LL_WARNS("SKWorldMap") << "roads: no features in " << path << LL_ENDL;
            continue;
        }
        for (simdjson::dom::element feature : features)
        {
            simdjson::dom::array coords;
            if (feature["geometry"]["coordinates"].get_array().get(coords))
            {
                continue;
            }
            F32 width = ASSUMED_WIDTH;
            F64 w = 0.0;
            if (!feature["properties"]["width_m"].get_double().get(w) && w > 0.0)
            {
                width = static_cast<F32>(w);
            }

            // The way's points, without repeats and without anything flat in plan, then its sides.
            points.clear();
            for (simdjson::dom::element c : coords)
            {
                simdjson::dom::array xyz;
                if (c.get_array().get(xyz) || xyz.size() < 2)
                {
                    continue;
                }
                LLVector3d p(0.0, 0.0, GROUND_Z);
                for (size_t i = 0; i < std::min<size_t>(xyz.size(), 3); ++i)
                {
                    F64 v = 0.0;
                    if (!xyz.at(i).get_double().get(v))
                    {
                        p.mdV[i] = v;
                    }
                }
                if (points.empty() || planeDistanceSquared(points.back(), p) > 1.0e-4)
                {
                    points.push_back(p);
                }
            }
            if (points.size() < 2)
            {
                continue;
            }
            sides.resize(points.size() - 1);
            for (size_t i = 0; i + 1 < points.size(); ++i)
            {
                sides[i] = planeSide(points[i], points[i + 1]);
            }
            for (size_t i = 0; i + 1 < points.size(); ++i)
            {
                const LLVector3d& a = points[i];
                const LLVector3d& b = points[i + 1];
                const LLVector3d mid = (a + b) * 0.5;
                if (mid.mdV[VX] < 0.0 || mid.mdV[VY] < 0.0)
                {
                    continue;
                }
                const U32 gx = static_cast<U32>(mid.mdV[VX] / REGION_METRES);
                const U32 gy = static_cast<U32>(mid.mdV[VY] / REGION_METRES);
                RoadRegion& region = regions[to_region_handle(gx * 256, gy * 256)];
                if (region.mSegments.empty())
                {
                    region.mMin = region.mMax = a;
                }
                for (const LLVector3d& q : { a, b })
                {
                    for (S32 k = 0; k < 3; ++k)
                    {
                        region.mMin.mdV[k] = std::min(region.mMin.mdV[k], q.mdV[k]);
                        region.mMax.mdV[k] = std::max(region.mMax.mdV[k], q.mdV[k]);
                    }
                }
                RoadSegment& s = region.mSegments.emplace_back();
                s.mA = a;
                s.mB = b;
                s.mSideA = i > 0 ? miter(sides[i - 1], sides[i]) : sides[i];
                s.mSideB = i + 2 < points.size() ? miter(sides[i], sides[i + 1]) : sides[i];
                s.mWidth = width;
                s.mClass = layer.mClass;
            }
        }
    }
    for (auto& [handle, region] : regions)
    {
        region.mRevision = 1;
    }
    return regions;
}

void SKWorldMapRoads::updateSource()
{
    static LLCachedControl<std::string> dataset(gSavedSettings, "SKWorldMapRoadsDataset", "");
    const std::string dir = dataset();
    if (dir.empty())
    {
        mSource.reset();
    }
    else if (!mSource || mSource->releaseDir() != dir)
    {
        mSource = std::make_unique<SKWorldMapDatasetRoads>(dir);
    }
    if (mSource)
    {
        mSource->poll();
    }
}

void SKWorldMapRoads::draw(const SKWorldMapFrame& frame, S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    updateSource();
    if (!mSource || height <= 0)
    {
        return;
    }

    static LLCachedControl<bool> show_candidates(gSavedSettings, "SKWorldMapRoadCandidates", true);
    static LLCachedControl<F32> strip_centre(gSavedSettings, "SKWorldMapRoadStripCentre", 0.5f);
    static LLCachedControl<F32> solid_centre(gSavedSettings, "SKWorldMapRoadSolidCentre", 2.f);
    static LLCachedControl<F32> octaves(gSavedSettings, "SKWorldMapRoadBlendOctaves", 6.f);
    RoadStyle style;
    style.mStripCentre = strip_centre;
    style.mSolidCentre = solid_centre;
    style.mOctaves = octaves;

    LLVector3d forward, right, up;
    viewBasis(frame.mYaw, frame.mPitch, forward, right, up);
    const F64 focal = height * 0.5 / std::tan(frame.mFovY * 0.5);
    const F64 near_depth = 0.5;

    // Segments and their ends, kept so the casings can go down before every fill.
    struct Drawn
    {
        End mA, mB;
        LLVector3 mSideA, mSideB;   ///< across the road on the ground at each end, mitred
        bool mPublicLook = true;
    };
    std::vector<Drawn> drawn;

    const auto make_end = [&](const LLVector3d& rel, F64 depth, const RoadSegment& s, bool public_look) {
        End e;
        e.mPos.setVec(rel);
        const F32 mpp = static_cast<F32>(depth / focal);
        const F32 solid = style.solidWeight(mpp);
        const F32 min_px = public_look ? PUBLIC_MIN_PX : PRIVATE_MIN_PX;
        F32 alpha = 1.f;
        if (!public_look)
        {
            alpha = PRIVATE_OPACITY * (1.f - smoothstep(scaleLog2(PRIVATE_FADE_FROM), scaleLog2(PRIVATE_FADE_TO), scaleLog2(mpp)));
        }
        e.mHalf = std::max(s.mWidth * 0.5f, min_px * 0.5f * mpp);
        e.mCasing = CASING_PX * (1.f - solid) * mpp;
        e.mFillAlpha = alpha * (OUTLINED_FILL_OPACITY + (1.f - OUTLINED_FILL_OPACITY) * solid);
        e.mCasingAlpha = alpha * CASING_OPACITY * (1.f - solid);
        return e;
    };

    mSource->forEachRegion([&](U64, const RoadRegion& region) {
        // Skip a region whose bounds lie wholly outside one side of the view: behind the eye, or past the
        // left, right, top or bottom of the screen (widened a little for the roads' minimum width).
        constexpr F64 SLACK = 1.05;
        const F64 tan_y = std::tan(frame.mFovY * 0.5) * SLACK;
        const F64 tan_x = tan_y * width / std::max(height, 1);
        bool behind = true, left = true, right_out = true, below = true, above = true;
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3d corner(i & 1 ? region.mMax.mdV[VX] : region.mMin.mdV[VX], i & 2 ? region.mMax.mdV[VY] : region.mMin.mdV[VY],
                                    i & 4 ? region.mMax.mdV[VZ] : region.mMin.mdV[VZ]);
            const LLVector3d rel = corner - frame.mEye;
            const F64 d = rel * forward;
            const F64 x = rel * right;
            const F64 y = rel * up;
            behind = behind && d <= near_depth;
            left = left && x < -tan_x * d;
            right_out = right_out && x > tan_x * d;
            below = below && y < -tan_y * d;
            above = above && y > tan_y * d;
        }
        if (behind || left || right_out || below || above)
        {
            return;
        }
        for (const RoadSegment& s : region.mSegments)
        {
            if (s.mClass == RoadClass::Candidate && !show_candidates)
            {
                continue;
            }
            LLVector3d a = s.mA - frame.mEye;
            LLVector3d b = s.mB - frame.mEye;
            F64 da = a * forward;
            F64 db = b * forward;
            if (da <= near_depth && db <= near_depth)
            {
                continue;
            }
            // Clip to the near plane.
            if (da < near_depth)
            {
                a = a + (b - a) * ((near_depth - da) / (db - da));
                da = near_depth;
            }
            else if (db < near_depth)
            {
                b = b + (a - b) * ((near_depth - db) / (da - db));
                db = near_depth;
            }
            // Candidates are unclassified; while they are all the data there is, they take the public look.
            const bool public_look = s.mClass != RoadClass::Private;
            Drawn& d = drawn.emplace_back();
            d.mA = make_end(a, da, s, public_look);
            d.mB = make_end(b, db, s, public_look);
            d.mSideA = s.mSideA;
            d.mSideB = s.mSideB;
            d.mPublicLook = public_look;
        }
    });
    if (drawn.empty())
    {
        return;
    }

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.bind();
    gGL.getTextureSlot(0)->unbind();

    gGL.begin(LLRender::TRIANGLES);
    for (const Drawn& d : drawn)
    {
        if (d.mA.mCasingAlpha <= 0.f && d.mB.mCasingAlpha <= 0.f)
        {
            continue;
        }
        const LLColor4& casing = d.mPublicLook ? PUBLIC_CASING : PRIVATE_FILL;
        for (F32 sign : { -1.f, 1.f })
        {
            const LLVector3 a0 = d.mA.mPos + d.mSideA * (sign * d.mA.mHalf);
            const LLVector3 a1 = d.mA.mPos + d.mSideA * (sign * (d.mA.mHalf + d.mA.mCasing));
            const LLVector3 b0 = d.mB.mPos + d.mSideB * (sign * d.mB.mHalf);
            const LLVector3 b1 = d.mB.mPos + d.mSideB * (sign * (d.mB.mHalf + d.mB.mCasing));
            quad(a0, a1, b0, b1, casing, d.mA.mCasingAlpha, d.mB.mCasingAlpha);
        }
    }
    for (const Drawn& d : drawn)
    {
        const LLColor4& fill = d.mPublicLook ? PUBLIC_FILL : PRIVATE_FILL;
        const LLVector3 a0 = d.mA.mPos - d.mSideA * d.mA.mHalf;
        const LLVector3 a1 = d.mA.mPos + d.mSideA * d.mA.mHalf;
        const LLVector3 b0 = d.mB.mPos - d.mSideB * d.mB.mHalf;
        const LLVector3 b1 = d.mB.mPos + d.mSideB * d.mB.mHalf;
        quad(a0, a1, b0, b1, fill, d.mA.mFillAlpha, d.mB.mFillAlpha);
    }
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}
