/**
 * @file skoverviewcensus.cpp
 * @brief The census's physical shapes as vertex buffers, a range per map class, for the overview.
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

#include "skoverviewcensus.h"

#include "llvertexbuffer.h"
#include "sknavbuild.h"
#include "sknavtypes.h"
#include "skworldcensus.h"

#include <cmath>
#include <map>

namespace skoverview
{
    U8 codeOf(CensusClass cls)
    {
        static const U8 codes[NUM_CENSUS_CLASSES] = { CODE_FLOOR, CODE_WALL,   CODE_COVER,  CODE_RAMP,
                                                      CODE_DOOR,  CODE_LADDER, CODE_VEHICLE };
        return cls < NUM_CENSUS_CLASSES ? codes[cls] : CODE_NONE;
    }

    bool keepRecord(const sknav::Record& rec, CensusClass& fixed)
    {
        fixed = NUM_CENSUS_CLASSES;
        if (rec.mRole != sknav::Role::NAV || rec.mNavRole == sknav::NavRole::MATERIAL_VOLUME ||
            rec.mNavRole == sknav::NavRole::EXCLUSION_VOLUME)
        {
            return false;
        }
        switch (rec.mFeature)
        {
            case sknav::FeatureKind::DOOR: fixed = DOOR; return true;
            case sknav::FeatureKind::LADDER: fixed = LADDER; return true;
            case sknav::FeatureKind::VEHICLE:
            case sknav::FeatureKind::ELEVATOR: fixed = VEHICLE; return true;
            default: break;
        }
        if (rec.mExclude & sknav::EXCLUDE_AVATAR)
        {
            fixed = DOOR; // door leaves are the only parts left out of the avatar class alone
            return true;
        }
        // Movers' census poses are a scan old; the ghost layer has them where they are.
        return !rec.mDynamic && !(rec.mExclude & sknav::EXCLUDE_SOLID);
    }

    CensusClass classifyTriangle(F32 nz, F32 part_height)
    {
        const F32 a = std::fabs(nz);
        if (a >= FLOOR_NZ)
        {
            return FLOOR;
        }
        if (a >= WALL_NZ)
        {
            return RAMP;
        }
        return part_height <= COVER_TOP_M ? COVER : WALL;
    }
}

SKOverviewCensus::SKOverviewCensus() = default;

SKOverviewCensus::~SKOverviewCensus()
{
    release();
}

void SKOverviewCensus::release()
{
    mBatches.clear();
    mStamp = 0;
    mSignature = 0;
    mTriangles = 0;
}

bool SKOverviewCensus::update()
{
    if (!SKWorldCensus::instanceExists())
    {
        return !mBatches.empty();
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap || snap->mStamp == mStamp)
    {
        return !mBatches.empty();
    }
    mStamp = snap->mStamp;

    // A new scan most often holds the same parts where they were: rebuild only when the geometry moved.
    U64 sig = 1469598103934665603ull;
    auto mix = [&sig](U64 v)
    {
        sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2);
    };
    std::vector<const sknav::Record*> kept;
    std::vector<U8> fixed_classes;
    kept.reserve(snap->mRecords.size());
    for (const sknav::RecordPtr& rec : snap->mRecords)
    {
        skoverview::CensusClass fixed;
        if (!rec || !skoverview::keepRecord(*rec, fixed))
        {
            continue;
        }
        kept.push_back(rec.get());
        fixed_classes.push_back(fixed);
        mix(rec->mSignature);
        mix(rec->mPart.getCRC32());
        mix((U64)(S64)std::llround(rec->mMin.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(rec->mMin.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(rec->mMin.mdV[VZ] * 64.0) << 42));
        mix((U64)rec->mFeature | ((U64)rec->mExclude << 8));
    }
    if (sig == mSignature && !mBatches.empty())
    {
        return true;
    }
    mSignature = sig;
    LL_PROFILE_ZONE_NAMED("sk overview census upload");

    // Per region, per class, region-local triangles.
    struct Bins
    {
        LLVector3d mOrigin;
        std::vector<LLVector3> mClass[skoverview::NUM_CENSUS_CLASSES];
    };
    std::map<std::pair<S64, S64>, Bins> regions;
    std::vector<LLVector3> tris;
    for (size_t i = 0; i < kept.size(); ++i)
    {
        const sknav::Record& rec = *kept[i];
        const std::pair<S64, S64> key((S64)std::llround(rec.mOrigin.mdV[VX]), (S64)std::llround(rec.mOrigin.mdV[VY]));
        Bins& bins = regions[key];
        bins.mOrigin = rec.mOrigin;
        tris.clear();
        sknav::emitRecord(rec, rec.mOrigin, tris);
        const F32 part_height = (F32)(rec.mMax.mdV[VZ] - rec.mMin.mdV[VZ]);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            const LLVector3& a = tris[t];
            const LLVector3& b = tris[t + 1];
            const LLVector3& c = tris[t + 2];
            LLVector3 n = (b - a) % (c - a);
            const F32 len = n.length();
            if (len <= 1.0e-8f)
            {
                continue;
            }
            const skoverview::CensusClass cls = fixed_classes[i] < skoverview::NUM_CENSUS_CLASSES
                                                    ? (skoverview::CensusClass)fixed_classes[i]
                                                    : skoverview::classifyTriangle(n.mV[VZ] / len, part_height);
            std::vector<LLVector3>& out = bins.mClass[cls];
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    }

    mBatches.clear();
    mTriangles = 0;
    for (auto& entry : regions)
    {
        Bins& bins = entry.second;
        U32 total = 0;
        for (const auto& v : bins.mClass)
        {
            total += (U32)v.size();
        }
        if (!total)
        {
            continue;
        }
        Batch batch;
        batch.mOrigin = bins.mOrigin;
        batch.mBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        if (!batch.mBuffer->allocateBuffer(total, 0))
        {
            LL_WARNS("SKOverview") << "census buffer of " << total << " vertices could not be allocated" << LL_ENDL;
            continue;
        }
        LLStrider<LLVector3> verts;
        if (!batch.mBuffer->getVertexStrider(verts))
        {
            continue;
        }
        U32 at = 0;
        for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
        {
            batch.mStart[c] = at;
            for (const LLVector3& p : bins.mClass[c])
            {
                *verts++ = p;
            }
            at += (U32)bins.mClass[c].size();
        }
        batch.mStart[skoverview::NUM_CENSUS_CLASSES] = at;
        batch.mBuffer->unmapBuffer();
        mTriangles += at / 3;
        mBatches.push_back(std::move(batch));
    }
    LL_DEBUGS("SKOverview") << "census upload: " << mBatches.size() << " regions, " << mTriangles << " triangles" << LL_ENDL;
    return !mBatches.empty();
}
