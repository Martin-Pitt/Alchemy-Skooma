/**
 * @file sknavexport.cpp
 * @brief The navmesh export's JSON: one writer for the viewer and the headless replay, so their files cannot drift.
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

#include "sknavexport.h"

#include "sknavairdistil.h"
#include "sknavannotate.h"

#include "llbase64.h"

#include "DetourNavMesh.h"

#include <ostream>

namespace sknav
{
    namespace
    {
        void writeVec3d(std::ostream& file, const LLVector3d& v)
        {
            file << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
        }

        void writeVec3(std::ostream& file, const LLVector3& v)
        {
            file << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
        }
    }

    U8 polyArea(unsigned short flags)
    {
        for (U8 a = 1; a < 8; ++a)
        {
            if (flags & (1u << a))
            {
                return a;
            }
        }
        return 0;
    }

    void writeExportHeader(std::ostream& file, const std::string& exported, const LLVector3d& agent, F32 height, F32 radius, F32 climb, F32 slope,
                           F32 cell)
    {
        file << "{\"format\":\"sknavmesh-export\",\"version\":3,\"exported\":\"" << exported << '"';
        file << ",\"agent\":";
        writeVec3d(file, agent);
        file << ",\"params\":{\"height\":" << height << ",\"radius\":" << radius << ",\"climb\":" << climb << ",\"slope\":" << slope
             << ",\"cell\":" << cell << '}';
        file << ",\"classes\":[\"avatar\"]";
        file << ",\"edgeKinds\":[\"open\",\"wall\",\"ledge\",\"overhang\"]";
        file << ",\"linkKinds\":[\"drop\",\"ladder\",\"elevator\"]";
        file << ",\"areas\":[\"none\",\"ground\",\"object\",\"submerged\",\"drop\",\"5\",\"6\",\"7\"]";
        file << ",\"meshes\":[";
    }

    void writeExportTile(std::ostream& file, const LLVector3d& mesh_origin, const TileExport& tile, ExportCounts& counts)
    {
        ++counts.mTiles;
        file << "{\"key\":[" << tile.mKey.mX << ',' << tile.mKey.mY << "],\"min\":";
        writeVec3d(file, tile.mMin);
        file << ",\"max\":";
        writeVec3d(file, tile.mMax);
        if (tile.mFaces)
        {
            file << ",\"faces\":[";
            bool first = true;
            for (const WallFace& f : *tile.mFaces)
            {
                if (!first)
                {
                    file << ',';
                }
                first = false;
                ++counts.mFaces;
                file << "{\"min\":";
                writeVec3(file, f.mMin);
                file << ",\"max\":";
                writeVec3(file, f.mMax);
                file << ",\"side\":" << static_cast<S32>(f.mSide) << ",\"area\":" << static_cast<S32>(f.mArea) << ",\"spaces\":[" << f.mSpace[0] << ','
                     << f.mSpace[1] << "]}";
            }
            file << ']';
        }
        if (tile.mLedges)
        {
            file << ",\"ledges\":[";
            for (size_t i = 0; i < tile.mLedges->size(); ++i)
            {
                const WallSegment& seg = (*tile.mLedges)[i];
                file << (i ? "," : "") << "{\"a\":";
                writeVec3d(file, mesh_origin + LLVector3d(seg.mA));
                file << ",\"b\":";
                writeVec3d(file, mesh_origin + LLVector3d(seg.mB));
                file << ",\"drop\":" << seg.mValue << '}';
            }
            file << ']';
        }
        if (tile.mRunoff)
        {
            file << ",\"runoff\":[";
            for (size_t i = 0; i < tile.mRunoff->size(); ++i)
            {
                const DropLink& link = (*tile.mRunoff)[i];
                file << (i ? "," : "") << "{\"from\":";
                writeVec3d(file, mesh_origin + LLVector3d(link.mFrom));
                file << ",\"to\":";
                writeVec3d(file, mesh_origin + LLVector3d(link.mTo));
                file << ",\"height\":" << link.mHeight << '}';
            }
            file << ']';
        }
        if (tile.mAir)
        {
            // The distilled records, packed little-endian and base64'd; the
            // layout names each section and record so a reader needs no code.
            const TileAir& air = *tile.mAir;
            const std::vector<U8> blob = packTileAir(air);
            file << ",\"air\":{\"origin\":";
            writeVec3(file, air.mOrigin);
            file << ",\"cell\":" << air.mCell << ",\"unit\":" << air.mUnit << ",\"width\":" << air.mWidth << ",\"height\":" << air.mHeight
                 << ",\"top_cell\":" << air.mTopCell << ",\"top_width\":" << air.mTopWidth << ",\"top_height\":" << air.mTopHeight
                 << ",\"layout\":" << tileAirLayout(air) << ",\"blob\":\"" << LLBase64::encode(blob.data(), blob.size()) << "\"}";
            ++counts.mAirs;
            if (tile.mEnclosure && tile.mEnclosure->mAir == tile.mAir && tile.mEnclosure->mGaps.size() == air.mGaps.size())
            {
                writeExportEnclosure(file, tile.mEnclosureGeneration, *tile.mEnclosure);
            }
        }
        for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
        {
            file << ",\"avatar\":{";
            const dtNavMesh* nav = tile.mNav[cls];
            // Detour x runs along Recast x, which is SL y, so the tile at
            // this lattice key sits at (mY, mX).
            const dtMeshTile* dt = nav ? nav->getTileAt(tile.mKey.mY, tile.mKey.mX, 0) : nullptr;
            file << "\"polys\":[";
            if (dt && dt->header)
            {
                bool first = true;
                for (S32 p = 0; p < dt->header->polyCount; ++p)
                {
                    const dtPoly& poly = dt->polys[p];
                    if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    {
                        continue;
                    }
                    if (!first)
                    {
                        file << ',';
                    }
                    first = false;
                    ++counts.mPolys;
                    file << "{\"v\":[";
                    for (S32 v = 0; v < static_cast<S32>(poly.vertCount); ++v)
                    {
                        if (v)
                        {
                            file << ',';
                        }
                        writeVec3d(file, mesh_origin + LLVector3d(fromRecast(&dt->verts[poly.verts[v] * 3])));
                    }
                    file << "],\"area\":" << static_cast<S32>(polyArea(poly.flags)) << '}';
                }
            }
            file << "],\"segments\":[";
            if (tile.mSegments[cls])
            {
                bool first = true;
                for (const WallSegment& seg : *tile.mSegments[cls])
                {
                    if (!first)
                    {
                        file << ',';
                    }
                    first = false;
                    ++counts.mSegments;
                    file << "{\"a\":";
                    writeVec3d(file, mesh_origin + LLVector3d(seg.mA));
                    file << ",\"b\":";
                    writeVec3d(file, mesh_origin + LLVector3d(seg.mB));
                    file << ",\"kind\":" << static_cast<S32>(seg.mKind) << ",\"value\":" << seg.mValue << ",\"area\":" << static_cast<S32>(seg.mArea)
                         << ",\"spaces\":[" << seg.mSpace[0] << ',' << seg.mSpace[1] << "]}";
                }
            }
            file << "],\"links\":[";
            if (tile.mLinks[cls])
            {
                bool first = true;
                for (const DropLink& link : *tile.mLinks[cls])
                {
                    if (!first)
                    {
                        file << ',';
                    }
                    first = false;
                    ++counts.mLinks;
                    file << "{\"from\":";
                    writeVec3d(file, mesh_origin + LLVector3d(link.mFrom));
                    file << ",\"to\":";
                    writeVec3d(file, mesh_origin + LLVector3d(link.mTo));
                    file << ",\"height\":" << link.mHeight << ",\"kind\":" << static_cast<S32>(link.mKind) << '}';
                }
            }
            file << "],\"flights\":[";
            if (tile.mFlights[cls])
            {
                bool first = true;
                for (const Flight& f : *tile.mFlights[cls])
                {
                    if (!first)
                    {
                        file << ',';
                    }
                    first = false;
                    static const char* const KINDS[] = { "stair", "ramp", "climb" };
                    static const char* const RULES[] = { "name", "treads", "grade" };
                    file << "{\"key\":\"" << std::hex << f.mKey << std::dec << "\",\"kind\":\"" << KINDS[static_cast<S32>(f.mKind)]
                         << "\",\"rule\":\"" << RULES[static_cast<S32>(f.mRule)] << "\",\"treads\":" << static_cast<S32>(f.mTreads) << ",\"eroded\":" << (f.mEroded ? "true" : "false")
                         << ",\"rise\":" << f.mRise << ",\"run\":" << f.mRun << ",\"plane\":[" << f.mPlane[0] << ',' << f.mPlane[1] << ','
                         << mesh_origin.mdV[VZ] + f.mPlane[2] - f.mPlane[0] * mesh_origin.mdV[VX] - f.mPlane[1] * mesh_origin.mdV[VY]
                         << "],\"bottom\":";
                    writeVec3d(file, mesh_origin + LLVector3d(f.mBottom));
                    file << ",\"top\":";
                    writeVec3d(file, mesh_origin + LLVector3d(f.mTop));
                    file << ",\"bottom_edge\":[";
                    writeVec3d(file, mesh_origin + LLVector3d(f.mBottomA));
                    file << ',';
                    writeVec3d(file, mesh_origin + LLVector3d(f.mBottomB));
                    file << "],\"top_edge\":[";
                    writeVec3d(file, mesh_origin + LLVector3d(f.mTopA));
                    file << ',';
                    writeVec3d(file, mesh_origin + LLVector3d(f.mTopB));
                    file << "],\"footprint\":[";
                    for (size_t i = 0; i < f.mFootprint.size(); ++i)
                    {
                        file << (i ? "," : "") << '[' << mesh_origin.mdV[VX] + f.mFootprint[i].mV[0] << ','
                             << mesh_origin.mdV[VY] + f.mFootprint[i].mV[1] << ']';
                    }
                    file << "],\"polys\":[";
                    for (size_t i = 0; i < f.mPolys.size(); ++i)
                    {
                        file << (i ? "," : "") << f.mPolys[i];
                    }
                    file << "]}";
                }
            }
            file << ']';
            // The stamp's side tables: each poly's space id (0 for none), poly edges on portals, flight polys.
            if (cls == static_cast<S32>(AgentClass::AVATAR) && tile.mStamp && tile.mStamp->mStamped && tile.mBinding)
            {
                const TileStampGeom& g = *tile.mStamp;
                const TileBinding& b = *tile.mBinding;
                file << ",\"poly_space\":[";
                for (size_t p = 0; p < g.mPolyComponent.size(); ++p)
                {
                    const U16 c = g.mPolyComponent[p];
                    file << (p ? "," : "") << (c < b.mComponentSpaceId.size() ? b.mComponentSpaceId[c] : 0u);
                }
                file << "],\"portal_edges\":[";
                for (size_t e = 0; e < g.mPortalEdges.size(); ++e)
                {
                    const StampPortalEdge& pe = g.mPortalEdges[e];
                    file << (e ? "," : "") << '[' << pe.mPoly << ',' << static_cast<S32>(pe.mEdge) << ',' << static_cast<S32>(pe.mSide) << ",\""
                         << std::hex << (e < b.mPortalKeys.size() ? b.mPortalKeys[e] : 0) << std::dec << "\"]";
                }
                file << "],\"flight_polys\":[";
                bool first = true;
                if (tile.mFlights[cls])
                {
                    for (const Flight& f : *tile.mFlights[cls])
                    {
                        for (const U16 p : f.mPolys)
                        {
                            file << (first ? "" : ",") << p;
                            first = false;
                        }
                    }
                }
                file << ']';
            }
            file << '}';
        }
        file << '}';
    }

    void writeExportEnclosure(std::ostream& file, U64 generation, const TileEnclosure& tile)
    {
        std::vector<U8> blob;
        blob.reserve(tile.mGaps.size() * 9);
        for (const GapEnclosure& g : tile.mGaps)
        {
            blob.push_back(g.mS);
            blob.push_back(g.mE);
            blob.push_back(g.mD);
            blob.push_back(g.mBucket);
            blob.push_back(g.mFlags);
            blob.push_back(static_cast<U8>(g.mSpace & 0xff));
            blob.push_back(static_cast<U8>(g.mSpace >> 8));
            blob.push_back(g.mOpen);
            blob.push_back(g.mCover);
        }
        file << ",\"enclosure\":{\"generation\":" << generation
             << ",\"layout\":{\"record\":[\"S\",\"e\",\"D\",\"bucket\",\"flags\",\"space\",\"O\",\"cover\"],\"order\":\"the air's gaps\",\"S\":\"u8 / 255\",\"e\":\"u8 / 255\","
                "\"D\":\"u8 * 0.25 m, 255 unknown\",\"bucket\":[\"outdoors\",\"sheltered\",\"indoors\",\"unknown\"],\"flags\":{\"sealed\":1,\"canopy\":2},\"space\":\"u16, index + 1 into the mesh's spaces, 0 for none\",\"O\":\"u8 / 255, 0 unread\",\"cover\":\"u8 / 255\"},\"blob\":\""
             << LLBase64::encode(blob.data(), blob.size()) << "\"}";
    }

    void writeExportSpaces(std::ostream& file, const LLVector3d& origin, const RoomsProduct& rooms, const WindProduct* wind)
    {
        static const char* buckets[] = { "outdoors", "sheltered", "indoors", "unknown" };
        auto vec = [&file](const LLVector3d& v) { file << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']'; };
        const F64 oz = origin.mdV[VZ];
        file << ",\"spaces\":[";
        for (size_t i = 0; i < rooms.mSpaces.size(); ++i)
        {
            const Space& s = rooms.mSpaces[i];
            file << (i ? "," : "") << "{\"index\":" << i << ",\"id\":" << s.mId << ",\"bucket\":\"" << buckets[static_cast<S32>(s.mBucket)]
                 << "\",\"min\":";
            vec(origin + LLVector3d(s.mMin));
            file << ",\"max\":";
            vec(origin + LLVector3d(s.mMax));
            file << ",\"centre\":";
            vec(origin + LLVector3d(s.mCentre));
            file << ",\"area\":" << s.mFloorArea << ",\"volume\":" << s.mVolume << ",\"surface\":" << s.mSurface << ",\"mfp\":" << s.mMeanFreePath
                 << ",\"ceiling\":[" << s.mCeilingMin + oz << ',' << s.mCeilingMean + oz << ',' << s.mCeilingMax + oz << "],\"floor\":["
                 << s.mFloorMin + oz << ',' << s.mFloorMax + oz << "],\"elongation\":" << s.mElongation << ",\"S\":" << s.mSMean << ",\"cover\":" << s.mCoverMean << ",\"O\":" << s.mSideOpenMean << ",\"e\":["
                 << s.mEMin << ',' << s.mEMean << ',' << s.mEMax << "],\"L\":" << s.mL << ",\"unknown\":" << s.mUnknown
                 << ",\"lift\":" << (s.mLift ? "true" : "false") << ",\"flight\":" << (s.mFlight ? "true" : "false") << ",\"under_flight\":" << (s.mUnderFlight ? "true" : "false")
                 << ",\"frozen\":" << (s.mFrozen ? "true" : "false") << ",\"portals\":[";
            for (size_t k = 0; k < s.mPortals.size(); ++k)
            {
                file << (k ? "," : "") << s.mPortals[k];
            }
            file << "]}";
        }
        file << "],\"portals\":[";
        for (size_t i = 0; i < rooms.mPortals.size(); ++i)
        {
            const Portal& p = rooms.mPortals[i];
            file << (i ? "," : "") << "{\"kind\":\"" << portalKindName(p.mKind) << "\",\"spaces\":[" << p.mSpaces[0] << ',' << p.mSpaces[1]
                 << "],\"a\":";
            vec(origin + LLVector3d(p.mA));
            file << ",\"b\":";
            vec(origin + LLVector3d(p.mB));
            file << ",\"normal\":[" << p.mNormal.mV[VX] << ',' << p.mNormal.mV[VY] << "],\"width\":" << p.mWidth << ",\"sill\":" << p.mSill + oz
                 << ",\"head\":" << (p.lintel() ? p.mHead + oz : 0.0) << ",\"lintel\":" << (p.lintel() ? "true" : "false")
                 << ",\"walled\":" << (p.walled() ? "true" : "false") << ",\"cues\":" << static_cast<S32>(p.mCues) << ",\"door\":\"" << p.mDoor
                 << "\"}";
        }
        file << ']';
        if (wind && !wind->mSlabs.empty())
        {
            // Per band its heights, per slab its cells: factor, deflection x and y, flags, a byte each.
            // No timings: the same air exports the same bytes.
            const WindProduct& w = *wind;
            file << ",\"wind\":{\"headings\":" << WIND_HEADINGS << ",\"bands\":[";
            for (size_t b = 0; b < w.mBands.size(); ++b)
            {
                const WindSlabPtr s = w.slab(0, static_cast<S32>(b));
                std::vector<F32> z;
                if (s && s->mZ)
                {
                    z = *s->mZ;
                    for (F32& v : z)
                    {
                        v += static_cast<F32>(oz);
                    }
                }
                file << (b ? "," : "") << "{\"offset\":" << w.mBands[b].mOffsetM << ",\"absolute\":" << (w.mBands[b].mAbsolute ? "true" : "false")
                     << ",\"z\":\"" << LLBase64::encode(reinterpret_cast<const U8*>(z.data()), z.size() * sizeof(F32)) << "\"}";
            }
            file << "],\"slabs\":[";
            for (size_t i = 0; i < w.mSlabs.size(); ++i)
            {
                const WindSlab& s = *w.mSlabs[i];
                file << (i ? "," : "") << "{\"heading\":" << windHeadingDeg(s.mHeading) << ",\"band\":" << s.mBand << ",\"origin\":["
                     << origin.mdV[VX] + s.mOrigin.mV[VX] << ',' << origin.mdV[VY] + s.mOrigin.mV[VY] << "],\"cell\":" << s.mCell
                     << ",\"width\":" << s.mWidth << ",\"height\":" << s.mHeight << ",\"cells\":\""
                     << LLBase64::encode(reinterpret_cast<const U8*>(s.mCells.data()), s.mCells.size() * sizeof(WindCell)) << "\"}";
            }
            file << "]}";
        }
    }

    void writeExportDoor(std::ostream& file, const DoorEntity& d)
    {
        file << "{\"id\":\"" << d.mId << "\",\"region\":" << d.mRegion << ",\"min\":";
        writeVec3d(file, d.mMin);
        file << ",\"max\":";
        writeVec3d(file, d.mMax);
        file << ",\"plane\":";
        writeVec3d(file, d.mPlanePoint);
        file << ",\"planeKnown\":" << (d.mPlaneKnown ? "true" : "false") << ",\"normal\":";
        writeVec3(file, d.mNormal);
        file << ",\"width\":" << d.mWidth << ",\"height\":" << d.mHeight << ",\"sill\":" << d.mSill << ",\"double\":" << (d.mDouble ? "true" : "false")
             << ",\"confidence\":" << d.mConfidence << ",\"damageable\":" << (d.mDamageable ? "true" : "false") << ",\"name\":\"" << jsonEscape(d.mName)
             << "\",\"leaves\":[";
        for (size_t i = 0; i < d.mLeaves.size(); ++i)
        {
            file << (i ? ",\"" : "\"") << d.mLeaves[i] << '"';
        }
        file << "]}";
    }

    void writeExportFeature(std::ostream& file, const Feature& f, LinkKind span_kind, const std::vector<LLVector3d>* exits,
                            const std::vector<std::pair<LLVector3d, LLVector3d>>* links)
    {
        file << "{\"kind\":\"" << featureName(f.mKind) << "\",\"confidence\":" << f.mConfidence << ",\"evidence\":\"" << f.mEvidence << "\",\"min\":";
        writeVec3d(file, f.mMin);
        file << ",\"max\":";
        writeVec3d(file, f.mMax);
        file << ",\"foot\":" << f.mFoot << ",\"top\":" << f.mTop << ",\"working\":" << (f.mWorking ? "true" : "false")
             << ",\"combat\":" << (f.mCombat ? "true" : "false") << ",\"air\":" << (f.mAir ? "true" : "false") << ",\"roots\":[";
        for (size_t i = 0; i < f.mRoots.size(); ++i)
        {
            file << (i ? ",\"" : "\"") << f.mRoots[i] << '"';
        }
        file << "],\"parts\":[";
        for (size_t i = 0; i < f.mParts.size(); ++i)
        {
            file << (i ? ",\"" : "\"") << f.mParts[i] << '"';
        }
        file << ']';
        if (exits && links)
        {
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
            {
                file << ",\"avatar\":{\"exits\":[";
                for (size_t i = 0; i < exits[cls].size(); ++i)
                {
                    if (i)
                    {
                        file << ',';
                    }
                    writeVec3d(file, exits[cls][i]);
                }
                file << "],\"links\":[";
                for (size_t i = 0; i < links[cls].size(); ++i)
                {
                    file << (i ? "," : "") << "{\"from\":";
                    writeVec3d(file, links[cls][i].first);
                    file << ",\"to\":";
                    writeVec3d(file, links[cls][i].second);
                    file << ",\"kind\":" << static_cast<S32>(span_kind) << '}';
                }
                file << "]}";
            }
        }
        file << '}';
    }
}
