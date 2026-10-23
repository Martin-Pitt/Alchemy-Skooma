/**
 * @file skroomsexport.cpp
 * @brief The rooms layer's export JSON: the enclosure per tile, and a mesh's spaces, portals and wind.
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
#include "linden_common.h"

#include "skroomsexport.h"

#include "skcensusprofile.h"

#include "llbase64.h"

#include <ostream>

namespace skworld
{
    void writeExportEnclosure(std::ostream& file, U64 generation, const TileEnclosure& tile)
    {
        SK_WORLD_ZONE("rooms.export.enclosure");
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
        SK_WORLD_ZONE("rooms.export.spaces");
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
                 << ",\"frozen\":" << (s.mFrozen ? "true" : "false") << ",\"roof\":" << (s.mRoof ? "true" : "false") << ",\"over_roofed\":" << s.mOverRoofed
                 << ",\"building\":" << s.mBuilding << ",\"covers\":[";
            for (size_t k = 0; s.mRoof && k < s.mCovers.size(); ++k)
            {
                file << (k ? "," : "") << (s.mCovers[k] < rooms.mSpaces.size() ? rooms.mSpaces[s.mCovers[k]].mId : 0u);
            }
            file << "],\"portals\":[";
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

}
