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

#include "sknavannotate.h"
#include "sknavbuild.h"
#include "sknavenclosure.h"
#include "sknavfeatures.h"
#include "sknavtypes.h"

#include "llquaternion.h"

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace sknav
{
    /// Version 2 adds the root table and the clusters' dominant roots, version 3 the reflection probes, version 5 the SIM
    /// provenance, version 6 a record's collides and physics_known; older versions still read.
    constexpr U32 CENSUS_FORMAT_VERSION = 6;
    constexpr size_t ROOT_DESCRIPTION_MAX = 256;

    /// Where a root's name came from: the region's linkset reply, or the object properties the viewer had cached.
    enum class NameSource : U8
    {
        NONE,
        LINKSET,
        PROPERTIES,
    };
    const char* nameSourceName(NameSource source);

    /// What the viewer knew about a linkset root, written once per root rather than per record.
    struct CensusRoot
    {
        LLUUID mId;
        std::string mName;
        std::string mDescription;   ///< at most ROOT_DESCRIPTION_MAX bytes, cut on a character boundary
        bool mScriptedKnown = false;
        bool mScripted = false;
        NameSource mSource = NameSource::NONE;
    };

    /// A root's share of a cluster's records.
    struct RootShare
    {
        LLUUID mId;
        U32 mParts = 0;
    };

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

    /// What a reflection probe is; the values are the viewer's probe kinds (LLReflectionMapManager::SKProbeKind).
    enum class ProbeKind : U8
    {
        DEFAULT,        ///< the sky probe every pixel falls back to
        PLACED_SPHERE,  ///< a reflection probe object
        PLACED_BOX,
        OCTREE,         ///< automatic, one per octree node
        NAV_SPHERE,     ///< automatic, held by a navmesh room part
        NAV_BOX,        ///< reserved: navmesh room boxes do not exist yet
        CLUSTER,        ///< automatic, a fill over one cell of a census cluster
        COUNT
    };
    const char* probeKindName(ProbeKind kind);

    /// A reflection probe the viewer held at export time, global.
    struct CensusProbe
    {
        ProbeKind mKind = ProbeKind::OCTREE;
        LLUUID mObject;             ///< the probe object, placed probes only
        LLVector3d mOrigin;
        F32 mRadius = 0.f;          ///< a box's is its half diagonal
        S32 mPriority = 0;
        bool mRelevant = true;      ///< false: settings, a manual probe swallowing it or a room part leave it unused
        bool mSuppressed = false;   ///< an octree probe a finished room part replaces
        bool mInsideManual = false; ///< an automatic probe a manual probe swallows
        bool mResident = false;     ///< held a cube slot
        /// Boxes: the object's rotation and full scale; the volume is the unit cube scaled by half the scale.
        LLQuaternion mRotation;
        LLVector3 mScale;
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
        F64 mClusterGap = 0.5;
        F32 mGroundedMargin = 5.f;
        F32 mVoidMargin = 256.f;
        EnclosureParams mEnclosure;

        std::vector<CensusRegion> mRegions;
        std::vector<RecordPtr> mRecords;
        /// Per record, the overview's look of its part: 0 visible, 1 invisible, 2 not known yet, 255 not asked; empty when
        /// the overview was not running.
        std::vector<U8> mSeen;
        std::vector<CensusCluster> mClusters;
        std::vector<Feature> mFeatures;
        std::vector<DoorEntity> mDoors;
        std::vector<CensusSpan> mSpans;
        /// Every root the records name, known or not; empty from a version 1 file.
        std::vector<CensusRoot> mRoots;
        /// Every reflection probe the viewer held; empty from a version 1 or 2 file.
        std::vector<CensusProbe> mProbes;
        /// The session's annotation marks, the replay's answer key; written, not read back.
        std::vector<Mark> mMarks;

        const CensusRegion* regionAt(F64 x, F64 y) const;
        const CensusRoot* rootOf(const LLUUID& id) const;
    };

    /// A description cut to ROOT_DESCRIPTION_MAX bytes without splitting a UTF-8 character.
    std::string capDescription(const std::string& text);

    /// A cluster's roots by part count, most first, ties by id; at most `limit`.
    std::vector<RootShare> dominantRoots(const CensusFile& census, const CensusCluster& cluster, size_t limit);
    /// "Hotel lobby (+2)": the first named dominant root, and how many other roots the cluster holds.
    std::string clusterLabel(const CensusFile& census, const CensusCluster& cluster);

    /// The root table as a JSON array, for the census and the navmesh export alike.
    void writeRootTable(std::ostream& out, const std::vector<CensusRoot>& roots);

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
