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

#include "llframetimer.h"
#include "llimagegl.h"
#include "llmaterial.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "skphysicsshape.h"
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
#include <set>

namespace skoverview
{
    U8 codeOf(CensusClass cls)
    {
        static const U8 codes[NUM_CENSUS_CLASSES] = { CODE_FLOOR, CODE_WALL,   CODE_COVER,       CODE_RAMP,
                                                      CODE_DOOR,  CODE_LADDER, CODE_VEHICLE,     CODE_PLACEHOLDER,
                                                      CODE_NONE };
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
        if (rec.mProvenance == sknav::Provenance::BOX)
        {
            fixed = PLACEHOLDER; // only its bounds are known: its own code, so the debug view can tell it apart
        }
        // Movers' census poses are a scan old; the markers show who is where.
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

namespace skoverview
{
    bool transparentTexture(const LLUUID& id)
    {
        static const LLUUID transparent("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");
        static const LLUUID invisiprim_a("38b86f85-2575-52a9-a531-23108d8da837");
        static const LLUUID invisiprim_b("e97cf410-8e61-7005-ec06-629eba4cd1fb");
        return id == transparent || id == invisiprim_a || id == invisiprim_b;
    }
}

namespace
{
    using skoverview::transparentTexture;

    enum class Seen : U8 { VISIBLE, INVISIBLE, UNKNOWN };

    // Whether the viewer draws a face so anyone sees it, from its own render inputs: colour alpha 0, an alpha-only
    // image (the invisiprim path), the default transparent or an invisiprim texture, an image whose alpha never rises
    // above the pick mask's threshold (32/255, below any mask cutoff), or a PBR base colour that is fully transparent.
    // UNKNOWN while the object, its face or its image is not loaded: never a verdict from data still on its way.
    Seen faceSeen(LLViewerObject* object, U8 i, std::string& detail)
    {
        const LLTextureEntry* te = object->getTE(i);
        if (!te)
        {
            detail = "no texture entry";
            return Seen::UNKNOWN;
        }
        const F32 alpha = te->getColor().mV[VALPHA];
        detail = llformat("alpha %.3f texture %s", alpha, te->getID().asString().c_str());
        const LLMaterial* material = te->getMaterialParams().get();
        if (material)
        {
            detail += llformat(" material %s alpha mode %d cutoff %d", te->getMaterialID().asString().c_str(),
                               (S32)material->getDiffuseAlphaMode(), (S32)material->getAlphaMaskCutoff());
        }
        if (const LLGLTFMaterial* pbr = te->getGLTFRenderMaterial())
        {
            detail += llformat(" PBR base alpha %.3f mode %d", pbr->mBaseColor.mV[VALPHA], (S32)pbr->mAlphaMode);
            if (pbr->mBaseColor.mV[VALPHA] <= 0.004f ||
                (pbr->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE &&
                 transparentTexture(pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR])))
            {
                detail += " -> invisible (PBR)";
                return Seen::INVISIBLE;
            }
            detail += " -> visible (PBR)";
            return Seen::VISIBLE;
        }
        if (alpha <= 0.004f)
        {
            detail += " -> invisible (alpha 0)";
            return Seen::INVISIBLE;
        }
        if (transparentTexture(te->getID()))
        {
            detail += " -> invisible (transparent texture)";
            return Seen::INVISIBLE;
        }
        LLViewerTexture* image = object->getTEImage(i);
        LLImageGL* gl = image ? image->getGLTexture() : nullptr;
        if (!gl || !gl->getHasGLTexture())
        {
            detail += " image not loaded -> unknown";
            return Seen::UNKNOWN;
        }
        detail += llformat(" components %d alpha-only %d mask %d", (S32)gl->getComponents(), gl->isAlphaOnly() ? 1 : 0,
                           gl->getIsAlphaMask() ? 1 : 0);
        if (gl->isAlphaOnly())
        {
            detail += " -> invisible (alpha-only image)";
            return Seen::INVISIBLE;
        }
        if (gl->getComponents() == 4)
        {
            // The pick mask holds a bit per cell for alpha above 32/255; none set means nothing shows at any cutoff.
            bool any = false;
            for (S32 y = 0; y < 16 && !any; ++y)
            {
                for (S32 x = 0; x < 16 && !any; ++x)
                {
                    any = gl->getMask(LLVector2((x + 0.5f) / 16.f, (y + 0.5f) / 16.f));
                }
            }
            if (!any)
            {
                detail += " -> invisible (alpha below the mask threshold everywhere)";
                return Seen::INVISIBLE;
            }
        }
        detail += " -> visible";
        return Seen::VISIBLE;
    }

    // A part is invisible when every face is; UNKNOWN while any face is still unknown and none is visible.
    Seen partSeen(const LLUUID& part, std::string* report)
    {
        LLViewerObject* object = gObjectList.findObject(part);
        if (!object || object->getNumTEs() == 0)
        {
            if (report)
            {
                *report = object ? "no faces" : "object not loaded";
            }
            return Seen::UNKNOWN;
        }
        Seen seen = Seen::INVISIBLE;
        for (U8 i = 0; i < object->getNumTEs(); ++i)
        {
            std::string detail;
            const Seen face = faceSeen(object, i, detail);
            if (report)
            {
                *report += llformat("%s[face %d: %s]", report->empty() ? "" : " ", i, detail.c_str());
            }
            if (face == Seen::VISIBLE)
            {
                seen = Seen::VISIBLE;
                if (!report)
                {
                    break;
                }
            }
            else if (face == Seen::UNKNOWN && seen == Seen::INVISIBLE)
            {
                seen = Seen::UNKNOWN;
            }
        }
        return seen;
    }
}

bool SKOverviewCensus::update()
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
    // Parts whose look was not known yet are asked again every couple of seconds, not only at the next scan.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (snap->mStamp == mStamp && !(mUnknownLook && now >= mRecheckAt))
    {
        return !mBatches.empty();
    }
    mStamp = snap->mStamp;
    mUnknownLook = false;
    mRecheckAt = now + 2.0;

    // A new scan most often holds the same parts where they were: rebuild only when the geometry moved.
    U64 sig = 1469598103934665603ull;
    auto mix = [&sig](U64 v)
    {
        sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2);
    };
    // Sculpted parts draw from what is rendered, over land and void alike: a megasculpt's physics is a poor stand-in
    // for the ground it shows, and a phantom one has no physics record at all. Their census records are not drawn.
    struct Sculpt
    {
        LLUUID mPart;
        LLVector3d mOrigin;             ///< its region's origin, global; triangles are region-local
        std::vector<LLVector3> mTris;
        F32 mHeight = 0.f;
        skoverview::CensusClass mFixed = skoverview::NUM_CENSUS_CLASSES;
    };
    std::vector<Sculpt> sculpts;
    std::set<LLUUID> sculpted;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
        if (!volume || volume->isDead() || volume->isAttachment() || volume->isMesh() || !volume->isSculpted() ||
            !volume->getRegion() || !volume->getVolume())
        {
            continue;
        }
        sculpted.insert(volume->getID());
        // A sculpt whose map has not loaded is a stretched placeholder sphere: nothing until it has.
        if (volume->getVolume()->getSculptLevel() != 0)
        {
            mUnknownLook = true;
            continue;
        }
        const LLVector3d centre = volume->getPositionGlobal();
        const F64 reach = snap->mRange + volume->getScale().magVec();
        if ((centre - snap->mCenter).magVec() > reach && (centre - snap->mCamera).magVec() > reach)
        {
            continue;
        }
        SKShapeGeometry geom;
        if (!skRenderShapeGeometry(volume, geom))
        {
            continue;
        }
        Sculpt sc;
        sc.mPart = volume->getID();
        sc.mOrigin = volume->getRegion()->getOriginGlobal();
        const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
        sc.mTris.reserve(geom.mTriangles.size());
        F32 lo = 1e30f, hi = -1e30f;
        for (const LLVector3& v : geom.mTriangles)
        {
            const LLVector3 local = volume->volumePositionToAgent(v) - region_agent;
            lo = llmin(lo, local.mV[VZ]);
            hi = llmax(hi, local.mV[VZ]);
            sc.mTris.push_back(local);
        }
        sc.mHeight = hi - lo;
        const LLVector3 scale = volume->getScale();
        const bool large = llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ])) > skoverview::INVISIBLE_WALL_M;
        const Seen seen = partSeen(sc.mPart, nullptr);
        mUnknownLook |= seen == Seen::UNKNOWN;
        if (seen == Seen::INVISIBLE)
        {
            if (large)
            {
                continue;
            }
            sc.mFixed = skoverview::INVISIBLE;
        }
        mix(sc.mPart.getCRC32());
        mix(seen == Seen::INVISIBLE ? 0x7a11u : 0x0u);
        mix((U64)sc.mTris.size());
        mix((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42));
        sculpts.push_back(std::move(sc));
    }

    std::vector<const sknav::Record*> kept;
    std::vector<U8> fixed_classes;
    kept.reserve(snap->mRecords.size());
    for (const sknav::RecordPtr& rec : snap->mRecords)
    {
        skoverview::CensusClass fixed;
        if (!rec || !skoverview::keepRecord(*rec, fixed) || sculpted.count(rec->mPart))
        {
            continue;
        }
        const LLVector3d extent = rec->mMax - rec->mMin;
        const bool large = llmax(extent.mdV[VX], llmax(extent.mdV[VY], extent.mdV[VZ])) > skoverview::INVISIBLE_WALL_M;
        std::string report;
        const Seen seen = partSeen(rec->mPart, large ? &report : nullptr);
        mUnknownLook |= seen == Seen::UNKNOWN;
        const bool transparent = seen == Seen::INVISIBLE;
        if (large)
        {
            // Once per part and verdict: the large ones are those that cover the view when misjudged.
            static std::map<LLUUID, Seen> logged;
            auto it = logged.find(rec->mPart);
            if (it == logged.end() || it->second != seen)
            {
                logged[rec->mPart] = seen;
                LL_INFOS("SKOverview") << "large census part " << rec->mPart << " " << extent << " m: "
                                       << (seen == Seen::INVISIBLE ? "invisible, dropped" : seen == Seen::VISIBLE ? "visible, drawn" : "not known yet, drawn")
                                       << " " << report << LL_ENDL;
            }
        }
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
    for (size_t i = 0; i < kept.size() + sculpts.size(); ++i)
    {
        const bool is_sculpt = i >= kept.size();
        const LLVector3d origin = is_sculpt ? sculpts[i - kept.size()].mOrigin : kept[i]->mOrigin;
        const std::pair<S64, S64> key((S64)std::llround(origin.mdV[VX]), (S64)std::llround(origin.mdV[VY]));
        Bins& bins = regions[key];
        bins.mOrigin = origin;
        F32 part_height = 0.f;
        U8 fixed_class = skoverview::NUM_CENSUS_CLASSES;
        if (is_sculpt)
        {
            const Sculpt& sc = sculpts[i - kept.size()];
            tris = sc.mTris;
            part_height = sc.mHeight;
            fixed_class = sc.mFixed;
        }
        else
        {
            const sknav::Record& rec = *kept[i];
            tris.clear();
            sknav::emitRecord(rec, rec.mOrigin, tris);
            part_height = (F32)(rec.mMax.mdV[VZ] - rec.mMin.mdV[VZ]);
            fixed_class = fixed_classes[i];
        }
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
            const skoverview::CensusClass cls = fixed_class < skoverview::NUM_CENSUS_CLASSES
                                                    ? (skoverview::CensusClass)fixed_class
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
