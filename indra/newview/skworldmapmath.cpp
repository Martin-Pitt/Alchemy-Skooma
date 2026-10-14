/**
 * @file skworldmapmath.cpp
 * @brief The 3D world map's pure geometry: its two cameras, which map tile level to draw where,
 *        and how agent clusters stack into columns of dots.
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

#include "linden_common.h"

#include "skworldmapmath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>

namespace skworldmap
{
namespace
{
    constexpr F64 PI_D = 3.141592653589793;

    F64 clampd(F64 v, F64 lo, F64 hi)
    {
        return std::max(lo, std::min(hi, v));
    }

    struct Plane
    {
        LLVector3d mNormal;
        F64 mOffset = 0.0;  ///< inside where dot(normal, p) + offset >= 0
    };

    /// The view's four side planes and the plane through the eye facing forward, all facing inward.
    std::array<Plane, 5> frustumPlanes(const View& view)
    {
        LLVector3d f, r, u;
        viewBasis(view.mYaw, view.mPitch, f, r, u);
        const F64 ty = std::tan(view.mFovY * 0.5);
        const F64 tx = ty * view.mAspect;
        const std::array<LLVector3d, 5> normals = { f * tx + r, f * tx - r, f * ty + u, f * ty - u, f };
        std::array<Plane, 5> planes;
        for (size_t i = 0; i < normals.size(); ++i)
        {
            LLVector3d n = normals[i];
            n.normalize();
            planes[i].mNormal = n;
            planes[i].mOffset = -(n * view.mEye);
        }
        return planes;
    }

    bool squareOutside(const std::array<Plane, 5>& planes, F64 x0, F64 y0, F64 w, F64 z)
    {
        const LLVector3d corners[4] = { { x0, y0, z }, { x0 + w, y0, z }, { x0, y0 + w, z }, { x0 + w, y0 + w, z } };
        for (const Plane& p : planes)
        {
            bool all_out = true;
            for (const LLVector3d& c : corners)
            {
                if (p.mNormal * c + p.mOffset >= 0.0)
                {
                    all_out = false;
                    break;
                }
            }
            if (all_out)
            {
                return true;
            }
        }
        return false;
    }

    F64 distanceToSquare(const LLVector3d& eye, F64 x0, F64 y0, F64 w, F64 z)
    {
        const F64 dx = eye.mdV[VX] - clampd(eye.mdV[VX], x0, x0 + w);
        const F64 dy = eye.mdV[VY] - clampd(eye.mdV[VY], y0, y0 + w);
        const F64 dz = eye.mdV[VZ] - z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
} // namespace

LLVector3d lookDirection(F64 yaw, F64 pitch)
{
    const F64 cp = std::cos(pitch);
    return LLVector3d(cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch));
}

void viewBasis(F64 yaw, F64 pitch, LLVector3d& forward, LLVector3d& right, LLVector3d& up)
{
    forward = lookDirection(yaw, pitch);
    right = LLVector3d(std::sin(yaw), -std::cos(yaw), 0.0);
    up = right % forward;
}

void OrbitCamera::clamp()
{
    mElevation = clampd(mElevation, ELEVATION_MIN, ELEVATION_MAX);
    mDistance = clampd(mDistance, DISTANCE_MIN, DISTANCE_MAX);
    mYaw = std::remainder(mYaw, 2.0 * PI_D);
}

LLVector3d OrbitCamera::eye() const
{
    const F64 elevation = clampd(mElevation, ELEVATION_MIN, ELEVATION_MAX);
    const F64 distance = clampd(mDistance, DISTANCE_MIN, DISTANCE_MAX);
    return mFocus - lookDirection(mYaw, -elevation) * distance;
}

void FlyCamera::clamp(F64 ground_z)
{
    mPitch = clampd(mPitch, -ELEVATION_MAX, 80.0 * PI_D / 180.0);
    mYaw = std::remainder(mYaw, 2.0 * PI_D);
    mPosition.mdV[VZ] = clampd(mPosition.mdV[VZ], ground_z + FLY_MIN_ABOVE_GROUND, ground_z + FLY_MAX_ABOVE_GROUND);
}

FlyCamera toFly(const OrbitCamera& orbit)
{
    FlyCamera fly;
    fly.mPosition = orbit.eye();
    fly.mYaw = orbit.mYaw;
    fly.mPitch = -clampd(orbit.mElevation, ELEVATION_MIN, ELEVATION_MAX);
    return fly;
}

OrbitCamera toOrbit(const FlyCamera& fly, F64 ground_z)
{
    OrbitCamera orbit;
    orbit.mYaw = fly.mYaw;
    orbit.mElevation = clampd(-fly.mPitch, ELEVATION_MIN, ELEVATION_MAX);
    const F64 height = std::max(fly.mPosition.mdV[VZ] - ground_z, FLY_MIN_ABOVE_GROUND);
    orbit.mDistance = clampd(height / std::sin(orbit.mElevation), DISTANCE_MIN, DISTANCE_MAX);
    orbit.mFocus = fly.mPosition + lookDirection(orbit.mYaw, -orbit.mElevation) * orbit.mDistance;
    return orbit;
}

F64 flySpeed(F64 eye_z, bool boost, F64 ground_z)
{
    const F64 speed = std::max(std::max(eye_z - ground_z, FLY_MIN_ABOVE_GROUND), 5.0);
    return boost ? speed * 4.0 : speed;
}

std::vector<SelectedTile> selectTiles(const View& view, const TileSelectParams& params)
{
    const std::array<Plane, 5> planes = frustumPlanes(view);
    const F64 focal = (view.mScreenHeight * 0.5) / std::tan(view.mFovY * 0.5);
    const F64 threshold = TILE_PIXELS * params.mDetail;

    auto make = [&](S32 level, S32 gx, S32 gy, SelectedTile& out) -> bool
    {
        out.mKey = { level, gx, gy };
        const F64 w = out.mKey.sideMetres();
        const F64 x0 = gx * REGION_METRES;
        const F64 y0 = gy * REGION_METRES;
        if (squareOutside(planes, x0, y0, w, params.mGroundZ))
        {
            return false;
        }
        out.mDistance = distanceToSquare(view.mEye, x0, y0, w, params.mGroundZ);
        out.mProjectedPixels = w * focal / std::max(out.mDistance, 1.0e-3);
        return true;
    };

    auto smaller = [](const SelectedTile& a, const SelectedTile& b) { return a.mProjectedPixels < b.mProjectedPixels; };
    std::priority_queue<SelectedTile, std::vector<SelectedTile>, decltype(smaller)> heap(smaller);

    const S32 root = 1 << (TILE_LEVELS - 1);
    for (S32 gy = 0; gy < params.mGridRegions; gy += root)
    {
        for (S32 gx = 0; gx < params.mGridRegions; gx += root)
        {
            SelectedTile t;
            if (make(TILE_LEVELS, gx, gy, t))
            {
                heap.push(t);
            }
        }
    }

    std::vector<SelectedTile> out;
    S32 count = static_cast<S32>(heap.size());
    while (!heap.empty())
    {
        const SelectedTile t = heap.top();
        heap.pop();
        if (t.mKey.mLevel > 1 && t.mProjectedPixels > threshold)
        {
            const S32 half = 1 << (t.mKey.mLevel - 2);
            SelectedTile kids[4];
            S32 n = 0;
            for (S32 ox : { 0, half })
            {
                for (S32 oy : { 0, half })
                {
                    if (make(t.mKey.mLevel - 1, t.mKey.mGridX + ox, t.mKey.mGridY + oy, kids[n]))
                    {
                        ++n;
                    }
                }
            }
            if (count - 1 + n <= params.mCap)
            {
                count += n - 1;
                for (S32 i = 0; i < n; ++i)
                {
                    heap.push(kids[i]);
                }
                continue;
            }
        }
        out.push_back(t);
    }
    return out;
}

TileKey ancestor(const TileKey& key, S32 up, F32 uv[4])
{
    TileKey a;
    a.mLevel = std::min(key.mLevel + up, TILE_LEVELS);
    const S32 n = a.regionsPerSide();
    a.mGridX = key.mGridX - key.mGridX % n;
    a.mGridY = key.mGridY - key.mGridY % n;
    const F32 m = static_cast<F32>(key.regionsPerSide());
    uv[0] = static_cast<F32>(key.mGridX - a.mGridX) / n;
    uv[1] = static_cast<F32>(key.mGridY - a.mGridY) / n;
    uv[2] = uv[0] + m / n;
    uv[3] = uv[1] + m / n;
    return a;
}

LLVector2 stackDirection(const LLVector3d& up_in_view)
{
    const F64 ux = up_in_view.mdV[VX];
    const F64 uy = up_in_view.mdV[VY];
    const F64 s = std::sqrt(ux * ux + uy * uy);
    F64 t = clampd((s - 0.15) / (0.40 - 0.15), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t);
    const F64 dx = s > 1.0e-6 ? ux / s : 0.0;
    const F64 dy = s > 1.0e-6 ? uy / s : 1.0;
    const F64 bx = t * dx;
    const F64 by = (1.0 - t) + t * dy;
    const F64 n = std::sqrt(bx * bx + by * by);
    if (n < 1.0e-6)
    {
        return LLVector2(0.f, 1.f);
    }
    return LLVector2(static_cast<F32>(bx / n), static_cast<F32>(by / n));
}

std::vector<S32> stackBudget(const std::vector<F64>& distances, const std::vector<S32>& counts, S32 per_stack_cap, S32 budget)
{
    const size_t n = std::min(distances.size(), counts.size());
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distances[a] < distances[b]; });
    std::vector<S32> out(n, 0);
    S32 left = budget;
    for (size_t i : order)
    {
        if (left <= 0)
        {
            break;
        }
        const S32 shown = std::max(0, std::min({ counts[i], per_stack_cap, left }));
        out[i] = shown;
        left -= shown;
    }
    return out;
}
} // namespace skworldmap
