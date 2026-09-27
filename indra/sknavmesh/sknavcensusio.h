/**
 * @file sknavcensusio.h
 * @brief A census export: everything the navmesh builds from, to a file and back, so a place replays offline.
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

#ifndef SK_NAVCENSUSIO_H
#define SK_NAVCENSUSIO_H

#include "sknavbuild.h"
#include "sknavenclosure.h"
#include "sknavfeatures.h"
#include "sknavtypes.h"

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace sknav
{
    constexpr U32 CENSUS_FORMAT_VERSION = 1;

    /// One region as the build saw it. Its land is kept as the heights the
    /// terrain windows sample: global z at (origin + phase + i, origin + phase + j)
    /// for i, j below mSamples, row-major along y. The phase is the fraction of
    /// a metre every window's samples sit at, so a window reads them exactly.
    struct CensusRegion
    {
        LLUUID mId;                 ///< the region's, which names its own mesh
        U64 mHandle = 0;
        LLVector3d mOrigin;
        std::string mName;
        F32 mWidth = 256.f;
        F32 mWater = 0.f;           ///< global
        F32 mLandMin = 0.f;         ///< the land's height range, what a region tile's box spans
        F32 mLandMax = 0.f;
        F32 mPhase = 0.f;
        S32 mSamples = 0;
        std::vector<F32> mZ;

        /// Whether a global point is in the region, as the viewer asks: half open.
        bool contains(F64 x, F64 y) const;
        /// The sample at a global point on the phase grid; false off it or outside.
        bool heightAt(F64 x, F64 y, F32& z) const;
    };

    /// A ladder's or a working elevator's span, with the exits and links the
    /// viewer found for it, global.
    struct CensusSpan
    {
        LLUUID mId;
        LinkKind mKind = LinkKind::LADDER;
        LLVector3d mMin;
        LLVector3d mMax;
        std::vector<LLVector3d> mExits[NUM_CLASSES];
        std::vector<std::pair<LLVector3d, LLVector3d>> mLinks[NUM_CLASSES];
    };

    /// The viewer's clustering: members as indices into the records.
    struct CensusCluster
    {
        LLUUID mId;
        bool mGrounded = false;     ///< meshed with its region rather than on its own
        std::vector<U32> mRecords;
    };

    struct CensusFile
    {
        std::string mExported;      ///< UTC, ISO 8601
        U64 mStamp = 0;             ///< the census scan
        LLVector3d mAgent;
        LLVector3d mCamera;
        F64 mRange = 0.0;
        /// The build parameters as the viewer makes them for every tile; the
        /// per-tile fields (origin, index, z range, records, terrain) are unset.
        TileBuildInput mParams;
        F32 mAirRadius = 128.f;     ///< tiles further from the camera build their air only on demand
        bool mBuildWeather = false; ///< SKNavBuildWeather: WEATHER is built past the air radius too
        F64 mClusterGap = 0.5;
        F32 mGroundedMargin = 5.f;
        F32 mVoidMargin = 256.f;
        EnclosureParams mEnclosure;

        std::vector<CensusRegion> mRegions;
        std::vector<RecordPtr> mRecords;
        std::vector<CensusCluster> mClusters;
        std::vector<Feature> mFeatures;
        std::vector<DoorEntity> mDoors;
        std::vector<CensusSpan> mSpans;

        const CensusRegion* regionAt(F64 x, F64 y) const;
    };

    /// JSON with the records, vertices and terrain as base64 blobs of packed
    /// little-endian records, the layouts named in the file itself.
    void writeCensus(std::ostream& out, const CensusFile& census);
    /// False with a reason when the text is not a census this reader knows.
    bool readCensus(const std::string& text, CensusFile& out, std::string* error = nullptr);
    bool readCensusFile(const std::string& path, CensusFile& out, std::string* error = nullptr);

    /// A tile's terrain window from the census's regions, sampled as the
    /// viewer samples it: every metre from the padded corner, the void deep
    /// below, water from the region under the tile's centre.
    void censusTerrain(const CensusFile& census, const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad,
                       TerrainWindow& out);
}

#endif // SK_NAVCENSUSIO_H
