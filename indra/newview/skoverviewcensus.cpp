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
#include "lltextureentry.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llgltfmaterial.h"

#include <cmath>
#include <map>

namespace skoverview
{
    U8 codeOf(CensusClass cls)
    {
        static const U8 codes[NUM_CENSUS_CLASSES] = { CODE_FLOOR, CODE_WALL,   CODE_COVER,   CODE_RAMP,
                                                      CODE_DOOR,  CODE_LADDER, CODE_VEHICLE, CODE_NONE };
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

    bool encloses(const LLVector3d& min, const LLVector3d& max, const std::vector<LLVector3d>& points)
    {
        const LLVector3d size = max - min;
        const S32 large = (size.mdV[VX] > ENCLOSURE_M) + (size.mdV[VY] > ENCLOSURE_M) + (size.mdV[VZ] > ENCLOSURE_M);
        if (large < 2)
        {
            return false;
        }
        for (const LLVector3d& p : points)
        {
            if (p.mdV[VX] >= min.mdV[VX] && p.mdV[VX] <= max.mdV[VX] && p.mdV[VY] >= min.mdV[VY] && p.mdV[VY] <= max.mdV[VY] &&
                p.mdV[VZ] >= min.mdV[VZ] && p.mdV[VZ] <= max.mdV[VZ])
            {
                return true;
            }
        }
        return false;
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

namespace
{
    // Textures nobody sees through to: the default transparent texture and the old invisiprim pair.
    bool transparentTexture(const LLUUID& id)
    {
        static const LLUUID transparent("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");
        static const LLUUID invisiprim_a("38b86f85-2575-52a9-a531-23108d8da837");
        static const LLUUID invisiprim_b("e97cf410-8e61-7005-ec06-629eba4cd1fb");
        return id == transparent || id == invisiprim_a || id == invisiprim_b;
    }

    // A face is invisible at alpha 0, with a transparent or invisiprim texture at any alpha, or as a PBR face with
    // base colour alpha 0 or a transparent base colour texture under blend or mask. A part is when all its faces are.
    bool fullyTransparent(const LLUUID& part)
    {
        LLViewerObject* object = gObjectList.findObject(part);
        if (!object || object->getNumTEs() == 0)
        {
            return false;
        }
        for (U8 i = 0; i < object->getNumTEs(); ++i)
        {
            const LLTextureEntry* te = object->getTE(i);
            if (!te)
            {
                return false;
            }
            bool invisible = te->getColor().mV[VALPHA] <= 0.004f || transparentTexture(te->getID());
            if (!invisible)
            {
                const LLViewerTexture* image = object->getTEImage(i);
                invisible = image && image->isAlphaOnly();
            }
            if (const LLGLTFMaterial* pbr = te->getGLTFRenderMaterial())
            {
                invisible = pbr->mBaseColor.mV[VALPHA] <= 0.004f ||
                            (pbr->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE &&
                             transparentTexture(pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]));
            }
            if (!invisible)
            {
                return false;
            }
        }
        return true;
    }
}

bool SKOverviewCensus::update(const std::vector<LLVector3d>& viewpoints)
{
    if (!SKWorldCensus::instanceExists())
    {
        return !mBatches.empty();
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return !mBatches.empty();
    }
    // Which of the scan's large parts hold a viewpoint now: moving into or out of one rebuilds as a new scan does.
    U64 enclosed = 1469598103934665603ull;
    for (const sknav::RecordPtr& rec : snap->mRecords)
    {
        if (rec && skoverview::encloses(rec->mMin, rec->mMax, viewpoints))
        {
            enclosed = (enclosed ^ rec->mPart.getCRC32()) * 1099511628211ull;
        }
    }
    if (snap->mStamp == mStamp && enclosed == mEnclosed)
    {
        return !mBatches.empty();
    }
    mStamp = snap->mStamp;
    mEnclosed = enclosed;

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
        if (!rec || !skoverview::keepRecord(*rec, fixed) || skoverview::encloses(rec->mMin, rec->mMax, viewpoints))
        {
            continue;
        }
        const bool transparent = fullyTransparent(rec->mPart);
        mix(transparent ? 0x7a11u : 0x0u);
        if (transparent)
        {
            // Small, a plausible invisible wall: hatched. Larger: an invisible shell nobody plays against.
            const LLVector3d size = rec->mMax - rec->mMin;
            if (size.mdV[VX] > skoverview::INVISIBLE_WALL_M || size.mdV[VY] > skoverview::INVISIBLE_WALL_M ||
                size.mdV[VZ] > skoverview::INVISIBLE_WALL_M)
            {
                continue;
            }
            fixed = skoverview::INVISIBLE;
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
