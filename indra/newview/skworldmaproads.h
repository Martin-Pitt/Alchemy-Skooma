/**
 * @file skworldmaproads.h
 * @brief The 3D world map's road layer: roads drawn at their own height, outlined while zoomed in and
 *        solid while zoomed out, blending smoothly with the scale.
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

#pragma once

#include "stdtypes.h"
#include "v3dmath.h"
#include "v3math.h"

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SKWorldMapFrame;

namespace skworldmap
{
    enum class RoadClass : U8
    {
        Public,
        Private,
        Candidate,      ///< unclassified; drawn only when asked for
    };

    struct RoadSegment
    {
        LLVector3d mA, mB;          ///< global metres, each at its own height
        LLVector3 mSideA, mSideB;   ///< across the road at each end, on the ground: unit, lengthened at a bend so the edges join
        F32 mWidth = 6.f;           ///< carriageway width in metres
        RoadClass mClass = RoadClass::Candidate;
    };

    /// One region's roads, cut into segments; the revision moves only when they change.
    struct RoadRegion
    {
        U32 mRevision = 0;
        LLVector3d mMin, mMax;      ///< bounds of the segments
        std::vector<RoadSegment> mSegments;
    };

    /// The road weights at a scale in metres per pixel: how solid the road is, against outlined.
    /// Both transitions are smoothsteps in log2 of the scale, `octaves` wide around their centres.
    struct RoadStyle
    {
        F32 mStripCentre = 0.5f;    ///< kit strips to outline; no strips exist yet, so outline stands in
        F32 mSolidCentre = 2.f;     ///< outline to solid
        F32 mOctaves = 6.f;

        F32 solidWeight(F32 metres_per_pixel) const;
    };
} // namespace skworldmap

/// Where the map's roads come from. The road foundation's store will answer this; until it exists the
/// dataset reader below does.
class SKWorldMapRoadSource
{
public:
    virtual ~SKWorldMapRoadSource() = default;

    /// Calls `fn` with every region that has roads, once its data is ready. Never blocks.
    virtual void forEachRegion(const std::function<void(U64 handle, const skworldmap::RoadRegion&)>& fn) = 0;

    /// One region's roads, or null when it has none or they are not ready yet.
    virtual const skworldmap::RoadRegion* region(U64 handle) const = 0;
};

/// The offline dataset's ways (schema 0.1.0: core, private and candidate `ways.geojson`), read once on
/// a worker thread and cut into regions by each segment's midpoint.
class SKWorldMapDatasetRoads : public SKWorldMapRoadSource
{
public:
    explicit SKWorldMapDatasetRoads(const std::string& release_dir);
    ~SKWorldMapDatasetRoads() override;

    void forEachRegion(const std::function<void(U64 handle, const skworldmap::RoadRegion&)>& fn) override;
    const skworldmap::RoadRegion* region(U64 handle) const override;

    /// Takes the regions once the worker has read them; never blocks.
    void poll();

    const std::string& releaseDir() const { return mDir; }

private:
    using Regions = std::unordered_map<U64, skworldmap::RoadRegion>;
    static Regions load(std::string dir);

    std::string mDir;
    std::future<Regions> mLoading;
    Regions mRegions;
};

/// Draws a source's roads into the map's scene, inside the ground pass's eye-relative matrices.
class SKWorldMapRoads
{
public:
    void draw(const SKWorldMapFrame& frame, S32 width, S32 height);
    void release() { mSource.reset(); }

    /// Where the roads come from this frame; null when none are set.
    const SKWorldMapRoadSource* source() const { return mSource.get(); }

    /// Reads the settings and starts or drops the source; called each frame before anything asks for roads.
    void updateSource();

private:
    std::unique_ptr<SKWorldMapDatasetRoads> mSource;
};
