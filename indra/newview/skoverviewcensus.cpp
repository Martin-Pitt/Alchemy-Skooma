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
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llvovolume.h"
#include "skphysicsshape.h"
#include "sknavbuild.h"
#include "sknavinvisible.h"
#include "sknavtypes.h"
#include "skworldcensus.h"
#include "lltextureentry.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llgltfmaterial.h"
#include "threadpool.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <unordered_set>

namespace skoverview
{
    const size_t MAX_BATCH_VERTICES = 16u * 1024u * 1024u;

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
            case sknav::FeatureKind::ELEVATOR: return false; // drawn live, at their current pose (skoverviewmovers.h)
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

    // splitmix64's finaliser: hashes summed after it do not cancel as raw ones would.
    static U64 scramble(U64 h)
    {
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        return h ^ (h >> 31);
    }
}

SKOverviewCensus::SKOverviewCensus() = default;

SKOverviewCensus::~SKOverviewCensus()
{
    release();
    if (mWorkers)
    {
        mWorkers->close();
        mWorkers.reset();
    }
}

void SKOverviewCensus::release()
{
    // A build in flight is for buffers no longer wanted: its result never lands, and the pool stays for the next.
    ++mGeneration;
    mJob = {};
    mBuilt = false;
    mExtents.clear();
    mExtentsBuilt = false;
    mOccupantCuts.clear();
    mBatches.clear();
    mLargeLogged.clear();
    mCoverLogged.clear();
    mStamp = 0;
    mSignature = 0;
    mTriangles = 0;
}

bool SKOverviewCensus::isRestless(const LLUUID& part) const
{
    const auto it = mRestless.find(part);
    return it != mRestless.end() && it->second.mOn;
}

std::vector<LLUUID> SKOverviewCensus::restlessParts() const
{
    // Void surrounds stay in the buffers, signed by identity: their rendered shape is no mover's.
    std::vector<LLUUID> parts;
    for (const auto& r : mRestless)
    {
        if (r.second.mOn && !r.second.mSurround)
        {
            parts.push_back(r.first);
        }
    }
    return parts;
}

void SKOverviewCensus::logChurn(std::vector<std::pair<U64, std::pair<char, LLUUID>>>& entries,
                                std::vector<std::pair<U64, std::pair<char, LLUUID>>>& moves)
{
    // What came and went since the last rebuild, by kind (S sculpt, U surrounds, V void surround, R record, C a part's
    // linkset and cluster, L land): a part both gone and come is one that changed.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    std::sort(entries.begin(), entries.end());
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> gone, came;
    std::set_difference(mLastEntries.begin(), mLastEntries.end(), entries.begin(), entries.end(), std::back_inserter(gone));
    std::set_difference(entries.begin(), entries.end(), mLastEntries.begin(), mLastEntries.end(), std::back_inserter(came));
    const bool first = mLastEntries.empty();
    mLastEntries = entries;
    // Restlessness is judged on shape and place alone: an invisible part's look and cover churn as its neighbours load.
    std::sort(moves.begin(), moves.end());
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> moved_from, moved_to;
    std::set_difference(mLastMoves.begin(), mLastMoves.end(), moves.begin(), moves.end(), std::back_inserter(moved_from));
    std::set_difference(moves.begin(), moves.end(), mLastMoves.begin(), mLastMoves.end(), std::back_inserter(moved_to));
    mLastMoves = moves;
    if (first)
    {
        return;
    }
    std::set<std::pair<char, LLUUID>> moved;
    for (const auto& g : moved_from)
    {
        moved.insert(g.second);
    }
    for (const auto& c : moved_to)
    {
        // A part moved again within the window turns restless: drawn live, out of the static buffers.
        const char kind = c.second.first;
        if ((kind == 'R' || kind == 'S' || kind == 'V') && c.second.second.notNull() && moved.count(c.second))
        {
            Restless& r = mRestless[c.second.second];
            r.mChanges = now - r.mLast <= RESTLESS_WINDOW_S ? r.mChanges + 1 : 1;
            r.mLast = now;
            r.mSurround = kind == 'V';
            if (!r.mOn && r.mChanges >= 2)
            {
                r.mOn = true;
                LL_INFOS("SKOverview") << "census part " << c.second.second << " (" << kind
                                       << ") is restless: drawn live until it holds still" << LL_ENDL;
            }
        }
    }
    std::map<char, U32> gone_n, came_n, changed_n;
    std::map<char, std::vector<LLUUID>> changed;
    std::set<std::pair<char, LLUUID>> went;
    for (const auto& g : gone)
    {
        ++gone_n[g.second.first];
        went.insert(g.second);
    }
    for (const auto& c : came)
    {
        ++came_n[c.second.first];
        if (went.count(c.second))
        {
            ++changed_n[c.second.first];
            std::vector<LLUUID>& ids = changed[c.second.first];
            if (ids.size() < 4)
            {
                ids.push_back(c.second.second);
            }
        }
    }
    std::string what;
    for (const char kind : { 'S', 'U', 'V', 'R', 'C', 'L' })
    {
        if (!gone_n[kind] && !came_n[kind])
        {
            continue;
        }
        what += llformat(" %c -%u +%u (%u changed", kind, gone_n[kind], came_n[kind], changed_n[kind]);
        for (const LLUUID& id : changed[kind])
        {
            what += " " + id.asString();
        }
        what += ")";
    }
    LL_INFOS("SKOverview") << "census rebuild of " << entries.size() << " entries:" << what << LL_ENDL;
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
    using skoverview::Seen;

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

    // What a part's verdict is worked from, short of the image's own pixels: its object and each face's inputs.
    U64 lookKey(LLViewerObject* object)
    {
        U64 h = skoverview::scramble((U64)(uintptr_t)object ^ ((U64)object->getNumTEs() << 56));
        for (U8 i = 0; i < object->getNumTEs(); ++i)
        {
            const LLTextureEntry* te = object->getTE(i);
            if (!te)
            {
                h = skoverview::scramble(h ^ 0x51u);
                continue;
            }
            U32 alpha = 0;
            const F32 a = te->getColor().mV[VALPHA];
            memcpy(&alpha, &a, sizeof(alpha));
            h = skoverview::scramble(h ^ hash_value(te->getID()));
            h = skoverview::scramble(h ^ hash_value(te->getMaterialID()) ^ ((U64)alpha << 32));
            h = skoverview::scramble(h ^ (U64)(uintptr_t)te->getGLTFRenderMaterial());
        }
        return h;
    }
}

skoverview::Seen SKOverviewCensus::seenOf(const LLUUID& part)
{
    // Only verdicts are kept, never "not known yet"; a few kept ones are worked again each pass in case a face's image
    // changed under the same inputs.
    LLViewerObject* object = gObjectList.findObject(part);
    if (!object || object->getNumTEs() == 0)
    {
        return Seen::UNKNOWN;
    }
    const U64 key = lookKey(object);
    const auto it = mSeen.find(part);
    if (it != mSeen.end() && it->second.mKey == key &&
        (hash_value(part) + mSeenPass) % SEEN_REVERIFY_PASSES != 0)
    {
        it->second.mPass = mSeenPass;
        return it->second.mSeen;
    }
    const Seen seen = partSeen(part, nullptr);
    if (seen == Seen::UNKNOWN)
    {
        if (it != mSeen.end())
        {
            mSeen.erase(it);
        }
        return seen;
    }
    mSeen[part] = { key, mSeenPass, seen };
    return seen;
}

void SKOverviewCensus::pruneSeen()
{
    for (auto it = mSeen.begin(); it != mSeen.end();)
    {
        it = it->second.mPass != mSeenPass ? mSeen.erase(it) : std::next(it);
    }
    ++mSeenPass;
}

namespace
{
    // What a volume's top-LOD triangles are worked from, short of its faces' looks: its shape and its placement.
    U64 coverGeomKey(LLVOVolume* volume)
    {
        const LLVolumeParams& params = volume->getVolume()->getParams();
        U64 key = skoverview::scramble(((U64)params.getSculptType() << 48) ^ (volume->isMesh() ? 1ull << 56 : 0ull));
        key = skoverview::scramble(key ^ hash_value(params.getSculptID()) ^ volume->getRegion()->getHandle());
        const F32 shape[] = { params.getBeginS(),  params.getBeginT(),     params.getEndS(),       params.getEndT(),
                              params.getHollow(),  params.getTwist(),      params.getTwistBegin(), params.getRatioX(),
                              params.getRatioY(),  params.getShearX(),     params.getShearY(),     params.getTaperX(),
                              params.getTaperY(),  params.getRevolutions(), params.getRadiusOffset(), params.getSkew() };
        for (const F32 f : shape)
        {
            key = skoverview::scramble(key ^ (U64)(S64)std::llround(f * 4096.f));
        }
        key = skoverview::scramble(key ^ params.getProfileParams().getCurveType() ^
                                   ((U64)params.getPathParams().getCurveType() << 8));
        const LLVector3 pos = volume->getPositionRegion();
        const LLQuaternion rot = volume->getRotationRegion();
        const LLVector3 scale = volume->getScale();
        for (S32 a = 0; a < 3; ++a)
        {
            key = skoverview::scramble(key ^ (U64)(S64)std::llround(pos.mV[a] * 64.f) ^
                                       ((U64)(S64)std::llround(scale.mV[a] * 64.f) << 24) ^
                                       ((U64)(S64)std::llround(rot.mQ[a] * 4096.f) << 48));
        }
        return skoverview::scramble(key ^ (U64)(S64)std::llround(rot.mQ[VW] * 4096.f));
    }
}

SKOverviewCensus::CoverInput SKOverviewCensus::coverInputFor(const LLUUID& part, const LLVector3d& lo, const LLVector3d& hi,
                                                             const SKCensusSnapshot& snap, U32& budget)
{
    CoverInput in;
    in.mTest = true;
    const F64 reach = sknav::InvisibleParams().mSlabMax;
    std::vector<U32> nearby;
    const S64 x0 = (S64)std::floor((lo.mdV[VX] - reach) / COVER_CELL_M);
    const S64 x1 = (S64)std::floor((hi.mdV[VX] + reach) / COVER_CELL_M);
    const S64 y0 = (S64)std::floor((lo.mdV[VY] - reach) / COVER_CELL_M);
    const S64 y1 = (S64)std::floor((hi.mdV[VY] + reach) / COVER_CELL_M);
    for (S64 x = x0; x <= x1; ++x)
    {
        for (S64 y = y0; y <= y1; ++y)
        {
            const auto it = mCoverGrid.find(((U64)(U32)(S32)x << 32) | (U32)(S32)y);
            if (it != mCoverGrid.end())
            {
                nearby.insert(nearby.end(), it->second.begin(), it->second.end());
            }
        }
    }
    std::sort(nearby.begin(), nearby.end());
    nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());
    U64 key = 0; // summed: the candidates follow the object list's order, which churns
    auto visit = [&](U32 index)
    {
        const CoverCandidate& candidate = mCoverCandidates[index];
        // A restless part rekeys every pass; it is drawn live and covers nothing in the static buffers.
        if (candidate.mId == part || isRestless(candidate.mId))
        {
            return;
        }
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.findObject(candidate.mId));
        if (!volume || volume->isDead() || !volume->getRegion())
        {
            return;
        }
        // Its bounding sphere, where it is now, against the part's bounds grown by the slab.
        const LLVector3d at = volume->getPositionGlobal();
        const F64 radius = 0.5 * volume->getScale().magVec();
        F64 d2 = 0.0;
        for (S32 a = 0; a < 3; ++a)
        {
            const F64 out = llmax(lo.mdV[a] - reach - at.mdV[a], at.mdV[a] - hi.mdV[a] - reach);
            d2 += out > 0.0 ? out * out : 0.0;
        }
        if (d2 > radius * radius)
        {
            return;
        }
        if (!volume->getVolume())
        {
            // There, its shape not yet: no barrier until it has one.
            in.mPending = in.mRetry = true;
            key += skoverview::scramble(candidate.mId.getCRC32() ^ 0x7u);
            return;
        }
        CoverEntry& entry = mCoverGeom[candidate.mId];
        // Worked once a pass, however many invisible parts it lies near.
        if (entry.mPass != mSeenPass)
        {
            entry.mPass = mSeenPass;
            const U64 geom_key = coverGeomKey(volume);
            if (entry.mGeomKey != geom_key)
            {
                entry.mGeomKey = geom_key;
                entry.mRaw.reset();
                entry.mGeom.reset();
                entry.mGeomPending = true;
                entry.mLookKey = 0;
                entry.mTries = 0;
            }
            // The top LOD is copied only near the scan, as the sculpt look does, and only so many a build.
            entry.mInRange = (at - snap.mCenter).magVec() <= snap.mRange + radius ||
                                  (at - snap.mCamera).magVec() <= snap.mRange + radius;
            // Past its retries, a pending top LOD is asked for again only now and then.
            if (entry.mGeomPending && entry.mInRange && budget &&
                (entry.mTries < skoverview::COVER_RETRIES || mSeenPass % 16 == 0))
            {
                std::vector<LLVector3> tris;
                std::vector<U32> starts;
                bool pending = false;
                skCensusRenderFaces(volume, tris, starts, pending);
                // A top LOD still on its way copies nothing and costs nothing.
                budget -= tris.empty() ? 0 : 1;
                ++entry.mTries;
                if (!tris.empty())
                {
                    const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
                    auto raw = std::make_shared<std::vector<LLVector3>>();
                    raw->reserve(tris.size());
                    for (const LLVector3& v : tris)
                    {
                        raw->push_back(volume->volumePositionToAgent(v) - region_agent);
                    }
                    entry.mRaw = std::move(raw);
                    entry.mStarts = std::move(starts);
                    entry.mOrigin = volume->getRegion()->getOriginGlobal();
                    entry.mLookKey = 0;
                }
                entry.mGeomPending = pending || tris.empty();
                if (!entry.mGeomPending)
                {
                    entry.mTries = 0;
                }
            }
            // Its faces' looks are worked again from the kept triangles alone.
            const U64 look = lookKey(volume);
            if (entry.mRaw && (look != entry.mLookKey || entry.mLookPending))
            {
                if (look != entry.mLookKey)
                {
                    entry.mTries = 0;
                }
                entry.mLookKey = look;
                entry.mLookPending = false;
                auto geom = std::make_shared<CoverGeom>();
                geom->mOrigin = entry.mOrigin;
                for (size_t f = 0; f < entry.mStarts.size(); ++f)
                {
                    const U32 end = f + 1 < entry.mStarts.size() ? entry.mStarts[f + 1] : (U32)entry.mRaw->size();
                    if (f >= volume->getNumTEs() || end <= entry.mStarts[f])
                    {
                        continue;
                    }
                    std::string detail;
                    const Seen seen = faceSeen(volume, (U8)f, detail);
                    entry.mLookPending |= seen == Seen::UNKNOWN;
                    if (seen == Seen::VISIBLE)
                    {
                        geom->mTris.insert(geom->mTris.end(), entry.mRaw->begin() + entry.mStarts[f],
                                           entry.mRaw->begin() + end);
                    }
                }
                entry.mTries += entry.mLookPending ? 1 : 0;
                entry.mGeom.reset();
                if (!geom->mTris.empty())
                {
                    entry.mGeom = std::move(geom);
                }
            }
            entry.mKey = skoverview::scramble(entry.mGeomKey ^ entry.mLookKey ^ (entry.mRaw ? 0x1u : 0x0u));
        }
        const bool pending = entry.mGeomPending || entry.mLookPending;
        in.mPending |= pending;
        // Retried at the recheck a few times, then left to the next change: an image off view may never load.
        in.mRetry |= (entry.mLookPending || (entry.mGeomPending && entry.mInRange)) && entry.mTries < skoverview::COVER_RETRIES;
        key += skoverview::scramble(entry.mKey ^ (pending ? 0x3u : 0x1u));
        if (entry.mGeom)
        {
            in.mGeoms.push_back(entry.mGeom);
        }
    };
    for (const U32 index : mCoverLarge)
    {
        visit(index);
    }
    for (const U32 index : nearby)
    {
        visit(index);
    }
    in.mKey = skoverview::scramble(key ^ part.getCRC32());
    return in;
}

bool SKOverviewCensus::update()
{
    // One build in flight: the passes wait for it, and what changed meanwhile is still marked when it lands.
    if (mJob.valid())
    {
        if (mJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return !mBatches.empty();
        }
        std::shared_ptr<Built> built = mJob.get();
        if (built && built->mGeneration == mGeneration)
        {
            mVerdicts = built->mVerdicts;
            swapIn(*built);
        }
    }
    if (!SKWorldCensus::instanceExists())
    {
        return !mBatches.empty();
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return !mBatches.empty();
    }
    // Work only on change. The census rescans every few frames, but a part it did not see change keeps its record
    // object, so the records' identities say whether anything moved. Parts whose look was not known yet are asked
    // again every couple of seconds, and sculpts (which have no record when phantom) every few.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (snap->mStamp != mStamp)
    {
        mStamp = snap->mStamp;
        // A sum, blind to order: the records follow the object list, which reorders whenever an object comes or goes.
        U64 records = 0;
        for (const sknav::RecordPtr& rec : snap->mRecords)
        {
            if (rec && isRestless(rec->mPart))
            {
                continue; // re-signed every scan: watched at the passes the rest call for, not a reason for one
            }
            records += skoverview::scramble((U64)(uintptr_t)rec.get());
        }
        mRecordsChanged |= records != mRecordsKey;
        mRecordsKey = records;
    }
    const bool recheck = mUnknownLook && now >= mRecheckAt;
    const bool sculpts_due = now >= mSculptsAt;
    // The surrounds are looked at least often: large rendered-only parts anywhere in the loaded regions.
    static LLCachedControl<F32> surrounds_every(gSavedSettings, "SKOverviewSurroundsRefresh", 15.f);
    const bool surrounds_due = now >= mSurroundsAt;
    if (surrounds_due)
    {
        mSurroundsAt = now + llmax((F32)surrounds_every, 1.f);
        const U64 before = mSurroundsSig;
        gatherSurrounds(*snap);
        mRecordsChanged |= mSurroundsSig != before;
    }
    if (!mRecordsChanged && !recheck && !sculpts_due && mBuilt)
    {
        return !mBatches.empty();
    }
    mRecordsChanged = false;
    mUnknownLook = false;
    mRecheckAt = now + 2.0;
    mSculptsAt = now + 5.0;
    // A restless part that has held still a while rejoins the census.
    for (auto it = mRestless.begin(); it != mRestless.end();)
    {
        const bool settled = now - it->second.mLast > (it->second.mOn ? RESTLESS_SETTLE_S : RESTLESS_WINDOW_S);
        it = settled ? mRestless.erase(it) : std::next(it);
    }

    // A new scan most often holds the same parts where they were: rebuild only when the geometry moved. Each part
    // hashes on its own and the parts sum, so the object list's order, which churns, never reads as a change.
    U64 sig = 0;
    U64 entry = 0;
    U64 motion = 0; // the entry without its look and cover, which change while the part stays put
    auto mixLook = [&entry](U64 v)
    {
        entry ^= v + 0x9e3779b97f4a7c15ull + (entry << 6) + (entry >> 2);
    };
    auto mix = [&motion, &mixLook](U64 v)
    {
        mixLook(v);
        motion ^= v + 0x9e3779b97f4a7c15ull + (motion << 6) + (motion >> 2);
    };
    // Each entry's kind and part as well, so a rebuild can say what moved.
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> entries;
    std::vector<std::pair<U64, std::pair<char, LLUUID>>> moves;
    // A restless part is signed by who it is alone, so its changes rebuild nothing; true for one.
    auto endEntry = [&sig, &entry, &motion, &entries, &moves, now, this](char kind, const LLUUID& part) -> bool
    {
        U64 h = skoverview::scramble(entry);
        const U64 m = skoverview::scramble(motion);
        entry = motion = 0;
        moves.push_back({ m, { kind, part } });
        bool restless = false;
        const auto it = part.notNull() ? mRestless.find(part) : mRestless.end();
        if (it != mRestless.end() && it->second.mOn)
        {
            if (m != it->second.mHash)
            {
                it->second.mHash = m;
                it->second.mLast = now;
            }
            h = skoverview::scramble(((U64)(U8)kind << 32) ^ part.getCRC32());
            restless = true;
        }
        sig += h;
        entries.push_back({ h, { kind, part } });
        return restless;
    };
    // Sculpted parts draw from what is rendered, over land and void alike: a megasculpt's physics is a poor stand-in
    // for the ground it shows, and a phantom one has no physics record at all. Their census records are not drawn.
    std::vector<RenderedPart> sculpts;
    std::unordered_set<LLUUID> sculpted;
    auto isSculpt = [](const LLVOVolume* volume)
    {
        return volume && !volume->isDead() && !volume->isAttachment() && !volume->isMesh() && volume->isSculpted() &&
               volume->getRegion() && volume->getVolume();
    };
    // The whole object list is walked for sculpts only every few seconds; between, the ones it found are looked at.
    if (sculpts_due)
    {
        mSculptCandidates.clear();
        // The same walk files every static volume by where it is, for the invisible parts' overlap test.
        mCoverCandidates.clear();
        mCoverGrid.clear();
        mCoverLarge.clear();
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            const LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
            if (isSculpt(volume))
            {
                mSculptCandidates.push_back(volume->getID());
            }
            if (!volume || volume->isDead() || volume->isAttachment() || volume->getAvatar() || !volume->getRegion() ||
                !volume->getVolume() || volume->flagUsePhysics())
            {
                continue;
            }
            const U32 index = (U32)mCoverCandidates.size();
            const LLVector3d centre = volume->getPositionGlobal();
            const F32 radius = 0.5f * volume->getScale().magVec();
            mCoverCandidates.push_back({ volume->getID(), centre, radius });
            const S64 x0 = (S64)std::floor((centre.mdV[VX] - radius) / COVER_CELL_M);
            const S64 x1 = (S64)std::floor((centre.mdV[VX] + radius) / COVER_CELL_M);
            const S64 y0 = (S64)std::floor((centre.mdV[VY] - radius) / COVER_CELL_M);
            const S64 y1 = (S64)std::floor((centre.mdV[VY] + radius) / COVER_CELL_M);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) > 64)
            {
                mCoverLarge.push_back(index);
                continue;
            }
            for (S64 x = x0; x <= x1; ++x)
            {
                for (S64 y = y0; y <= y1; ++y)
                {
                    mCoverGrid[((U64)(U32)(S32)x << 32) | (U32)(S32)y].push_back(index);
                }
            }
        }
    }
    U32 cover_budget = skoverview::COVER_EXTRACTS_PER_BUILD;
    std::vector<CoverInput> sculpt_cover;
    for (const LLUUID& candidate : mSculptCandidates)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.findObject(candidate));
        if (!isSculpt(volume))
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
        // Its triangles are worked again only when what they come from moved: the volume (recycled, so with its map
        // and level), its placement in its region to 1/64 m, and the region.
        const LLVolume* lod = volume->getVolume();
        const LLVolumeParams& params = lod->getParams();
        const LLVector3 pos = volume->getPositionRegion();
        const LLQuaternion rot = volume->getRotationRegion();
        const LLVector3 scale = volume->getScale();
        // Not the volume itself: a LOD switch replaces it and the top-LOD shape does not change with it.
        U64 key = skoverview::scramble(((U64)(U8)lod->getSculptLevel() << 56) ^ ((U64)params.getSculptType() << 48));
        key = skoverview::scramble(key ^ hash_value(params.getSculptID()) ^ volume->getRegion()->getHandle());
        for (S32 a = 0; a < 3; ++a)
        {
            key = skoverview::scramble(key ^ (U64)(S64)std::llround(pos.mV[a] * 64.f) ^
                                       ((U64)(S64)std::llround(scale.mV[a] * 64.f) << 24) ^
                                       ((U64)(S64)std::llround(rot.mQ[a] * 4096.f) << 48));
        }
        key = skoverview::scramble(key ^ (U64)(S64)std::llround(rot.mQ[VW] * 4096.f));
        SculptEntry& cached = mSculptGeom[candidate];
        cached.mPass = mSeenPass;
        if (cached.mKey != key)
        {
            cached.mKey = key;
            cached.mTris.reset();
            SKShapeGeometry geom;
            cached.mValid = skCensusRenderGeometry(volume, geom);
            if (cached.mValid)
            {
                const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
                auto tris = std::make_shared<std::vector<LLVector3>>();
                tris->reserve(geom.mTriangles.size());
                F32 lo = 1e30f, hi = -1e30f;
                for (const LLVector3& v : geom.mTriangles)
                {
                    const LLVector3 local = volume->volumePositionToAgent(v) - region_agent;
                    lo = llmin(lo, local.mV[VZ]);
                    hi = llmax(hi, local.mV[VZ]);
                    tris->push_back(local);
                }
                cached.mHeight = hi - lo;
                cached.mTris = std::move(tris);
            }
        }
        if (!cached.mValid)
        {
            continue;
        }
        RenderedPart sc;
        sc.mPart = volume->getID();
        sc.mOrigin = volume->getRegion()->getOriginGlobal();
        sc.mAnchor = volume->getRootEdit()->getPositionGlobal();
        sc.mRoot = volume->getRootEdit()->getID();
        sc.mTris = cached.mTris;
        sc.mHeight = cached.mHeight;
        const Seen seen = seenOf(sc.mPart);
        mUnknownLook |= seen == Seen::UNKNOWN;
        CoverInput cover;
        if (seen == Seen::INVISIBLE)
        {
            // Judged by what visible geometry it lies on; the worker drops it when large and not a stand-in.
            const F64 reach = 0.5 * scale.magVec();
            cover = coverInputFor(sc.mPart, centre - LLVector3d(reach, reach, reach), centre + LLVector3d(reach, reach, reach),
                                  *snap, cover_budget);
            mUnknownLook |= cover.mRetry;
        }
        mix(sc.mPart.getCRC32());
        mixLook(seen == Seen::INVISIBLE ? 0x7a11u ^ cover.mKey ^ (cover.mPending ? 0x9e11u : 0x0u) : 0x0u);
        mix((U64)sc.mTris->size());
        mix((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42));
        if (endEntry('S', sc.mPart))
        {
            continue; // drawn live by the movers
        }
        sculpts.push_back(std::move(sc));
        sculpt_cover.push_back(std::move(cover));
    }
    for (auto it = mSculptGeom.begin(); it != mSculptGeom.end();)
    {
        it = it->second.mPass != mSeenPass ? mSculptGeom.erase(it) : std::next(it);
    }

    // The surrounds the near look did not already take.
    std::unordered_set<LLUUID> rendered;
    for (const RenderedPart& sc : sculpts)
    {
        rendered.insert(sc.mPart);
    }
    for (const RenderedPart& part : mSurrounds)
    {
        if (rendered.insert(part.mPart).second)
        {
            sculpts.push_back(part);
        }
    }
    sculpt_cover.resize(sculpts.size());
    mix(mSurroundsSig);
    endEntry('U', LLUUID::null);

    std::vector<const sknav::Record*> kept;
    std::vector<U8> fixed_classes;
    std::vector<bool> void_only;   // a surround record: only its triangles over no connected region are drawn
    std::vector<CoverInput> kept_cover;
    kept.reserve(snap->mRecords.size());
    std::unordered_map<LLUUID, Seen> large_logged; // this pass's large parts, so dropped parts leave the log's memory
    for (const sknav::RecordPtr& rec : snap->mRecords)
    {
        skoverview::CensusClass fixed;
        if (!rec || sculpted.count(rec->mPart))
        {
            continue;
        }
        if (rec->mRole == sknav::Role::SURROUND)
        {
            // A sim surround's rendered shape, in the void only: over land a phantom megasculpt would roof the region.
            if (rendered.count(rec->mPart))
            {
                continue;
            }
            CoverInput cover;
            if (seenOf(rec->mPart) == Seen::INVISIBLE)
            {
                cover = coverInputFor(rec->mPart, rec->mMin, rec->mMax, *snap, cover_budget);
                mUnknownLook |= cover.mRetry;
                mixLook(0x7a11u ^ cover.mKey ^ (cover.mPending ? 0x9e11u : 0x0u));
            }
            kept.push_back(rec.get());
            fixed_classes.push_back(skoverview::NUM_CENSUS_CLASSES);
            void_only.push_back(true);
            kept_cover.push_back(std::move(cover));
            mix(rec->mSignature);
            mix(rec->mPart.getCRC32());
            endEntry('V', rec->mPart);
            continue;
        }
        if (!skoverview::keepRecord(*rec, fixed))
        {
            continue;
        }
        const LLVector3d extent = rec->mMax - rec->mMin;
        const bool large = llmax(extent.mdV[VX], llmax(extent.mdV[VY], extent.mdV[VZ])) > skoverview::INVISIBLE_WALL_M;
        const Seen seen = seenOf(rec->mPart);
        mUnknownLook |= seen == Seen::UNKNOWN;
        const bool transparent = seen == Seen::INVISIBLE;
        if (large)
        {
            // Once per part and verdict: the large ones are those that cover the view when misjudged.
            large_logged[rec->mPart] = seen;
            const auto it = mLargeLogged.find(rec->mPart);
            if (it == mLargeLogged.end() || it->second != seen)
            {
                std::string report; // only when logged: formatting every face of every large part each pass is not free
                partSeen(rec->mPart, &report);
                LL_INFOS("SKOverview") << "large census part " << rec->mPart << " " << extent << " m: "
                                       << (seen == Seen::INVISIBLE ? "invisible, drawn only if it stands in" : seen == Seen::VISIBLE ? "visible, drawn" : "not known yet, drawn")
                                       << " " << report << LL_ENDL;
            }
        }
        // An invisible part keeps its class: the worker tells a stand-in on visible geometry from a barrier to hatch.
        CoverInput cover;
        if (transparent)
        {
            cover = coverInputFor(rec->mPart, rec->mMin, rec->mMax, *snap, cover_budget);
            mUnknownLook |= cover.mRetry;
        }
        mixLook(transparent ? 0x7a11u ^ cover.mKey ^ (cover.mPending ? 0x9e11u : 0x0u) : 0x0u);
        kept.push_back(rec.get());
        fixed_classes.push_back(fixed);
        void_only.push_back(false);
        kept_cover.push_back(std::move(cover));
        mix(rec->mSignature);
        mix(rec->mPart.getCRC32());
        mix((U64)(S64)std::llround(rec->mMin.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(rec->mMin.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(rec->mMin.mdV[VZ] * 64.0) << 42));
        mix((U64)rec->mFeature | ((U64)rec->mExclude << 8));
        if (endEntry('R', rec->mPart))
        {
            // Drawn live by the movers.
            kept.pop_back();
            fixed_classes.pop_back();
            void_only.pop_back();
            kept_cover.pop_back();
        }
    }
    mLargeLogged = std::move(large_logged);
    for (auto it = mCoverGeom.begin(); it != mCoverGeom.end();)
    {
        it = it->second.mPass != mSeenPass ? mCoverGeom.erase(it) : std::next(it);
    }
    pruneSeen();
    // Each linkset is a group, so the tactical cut can take a building's linksets and leave its neighbours whole: a
    // cluster joins whatever touches, often a whole block. A level still keeps whole clusters, by the heights of the
    // cluster a linkset is in.
    std::unordered_map<const sknav::Record*, U32> cluster_of;
    cluster_of.reserve(snap->mRecords.size());
    const U32 clusters = (U32)snap->mClusters.mClusters.size();
    for (U32 c = 0; c < clusters; ++c)
    {
        for (const sknav::RecordPtr& rec : snap->mClusters.mClusters[c]->mRecords)
        {
            cluster_of[rec.get()] = c;
        }
    }
    std::unordered_map<LLUUID, U32> linkset_ids;
    std::vector<LLUUID> roots;
    std::vector<U32> group_of(kept.size() + sculpts.size());
    std::map<U32, U32> group_cluster;
    for (size_t i = 0; i < group_of.size(); ++i)
    {
        const LLUUID root = i >= kept.size() ? sculpts[i - kept.size()].mRoot
                                             : kept[i]->mRoot.notNull() ? kept[i]->mRoot : kept[i]->mPart;
        const auto ins = linkset_ids.emplace(root, (U32)linkset_ids.size());
        if (ins.second)
        {
            roots.push_back(root);
        }
        group_of[i] = ins.first->second;
        if (i < kept.size())
        {
            const auto it = cluster_of.find(kept[i]);
            if (it != cluster_of.end())
            {
                group_cluster.emplace(group_of[i], it->second);
            }
        }
    }
    auto groupOf = [&](size_t i) -> U32 { return group_of[i]; };
    // A part's linkset and its cluster's extent, never their indices: those follow the records' order.
    auto rounded = [](const LLVector3d& v)
    {
        return (U64)(S64)std::llround(v.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(v.mdV[VY] * 64.0) << 21) ^
               ((U64)(S64)std::llround(v.mdV[VZ] * 64.0) << 42);
    };
    for (size_t i = 0; i < kept.size(); ++i)
    {
        mix(kept[i]->mPart.getCRC32());
        mix(roots[groupOf(i)].getCRC32());
        const auto cluster = group_cluster.find(groupOf(i));
        if (cluster != group_cluster.end())
        {
            const sknav::Cluster& c = *snap->mClusters.mClusters[cluster->second];
            mix(rounded(c.mMin));
            mix(rounded(c.mMax));
        }
        endEntry('C', kept[i]->mPart);
    }
    // Which land is connected decides what of a surround is in the void.
    for (const LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        mix(region->getHandle());
        endEntry('L', LLUUID::null);
    }
    if (sig == mSignature && mBuilt)
    {
        return !mBatches.empty();
    }
    logChurn(entries, moves);

    // The worker gets values only: what is drawn, whose linkset each part is and where, and the regions' squares.
    auto job = std::make_shared<Job>();
    job->mSnapshot = snap;
    job->mSignature = sig;
    job->mGeneration = mGeneration;
    job->mKept = std::move(kept);
    job->mFixed = std::move(fixed_classes);
    job->mVoidOnly = std::move(void_only);
    job->mSculpts = std::move(sculpts);
    job->mCover = std::move(kept_cover);
    job->mCover.insert(job->mCover.end(), std::make_move_iterator(sculpt_cover.begin()),
                       std::make_move_iterator(sculpt_cover.end()));
    job->mVerdicts = mVerdicts;
    job->mGroupOf = std::move(group_of);
    job->mRoots = std::move(roots);
    job->mGroupCluster = std::move(group_cluster);
    job->mAnchors.resize(job->mRoots.size());
    std::vector<bool> anchored(job->mRoots.size(), false);
    for (size_t i = 0; i < job->mGroupOf.size(); ++i)
    {
        const U32 group = job->mGroupOf[i];
        if (anchored[group])
        {
            // A linkset is whole only when all of it is a surround in the void: a part's physics record beside its
            // surround record is land.
            job->mAnchors[group].second = job->mAnchors[group].second && i < job->mKept.size() && job->mVoidOnly[i];
            continue; // once per linkset
        }
        anchored[group] = true;
        // Where each linkset is, for whose region it is in: its root's position, so a linkset straddling a border has
        // one answer.
        LLVector3d anchor;
        if (i >= job->mKept.size())
        {
            anchor = job->mSculpts[i - job->mKept.size()].mAnchor;
        }
        else
        {
            const sknav::Record& rec = *job->mKept[i];
            const LLViewerObject* root = gObjectList.findObject(rec.mRoot);
            anchor = root && !root->isDead() ? root->getPositionGlobal() : (rec.mMin + rec.mMax) * 0.5;
        }
        job->mAnchors[group] = { anchor, i < job->mKept.size() && job->mVoidOnly[i] };
    }
    for (const LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        job->mRegions.push_back({ region->getOriginGlobal(), (F64)region->getWidth() });
    }

    if (!mWorkers)
    {
        mWorkers = std::make_unique<LL::ThreadPool>("CensusOverview", 1);
        mWorkers->start();
    }
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<Built>()>>(
        [job]()
        {
            LL_PROFILE_ZONE_NAMED("sk overview census build");
            return buildBatches(*job);
        });
    mJob = task->get_future();
    if (!mWorkers->getQueue().tryPost([task]() { (*task)(); }))
    {
        // A refused post would drop the build, so it is made here instead, and lands the same way.
        LL_WARNS("SKOverview") << "census overview queue refused the build; building on this thread" << LL_ENDL;
        (*task)();
    }
    return !mBatches.empty();
}

// static
std::shared_ptr<SKOverviewCensus::Built> SKOverviewCensus::buildBatches(const Job& job)
{
    // Per region, per class, per group, region-local triangles; and each group's bounds.
    struct Bounds
    {
        LLVector3 mMin{ 1e30f, 1e30f, 1e30f };
        LLVector3 mMax{ -1e30f, -1e30f, -1e30f };
    };
    struct Bins
    {
        LLVector3d mOrigin;
        std::map<U32, std::vector<LLVector3>> mClass[skoverview::NUM_CENSUS_CLASSES];
        std::map<U32, std::vector<U8>> mInvisible[skoverview::NUM_CENSUS_CLASSES]; ///< per triangle, as Batch's
        std::map<U32, Bounds> mBounds;
    };
    // Whether a point lies over a connected region, as LLViewerRegion::pointInRegionGlobal has it.
    auto overLand = [&job](const LLVector3d& p)
    {
        for (const auto& square : job.mRegions)
        {
            const F32 x = (F32)(p.mdV[VX] - square.first.mdV[VX]);
            const F32 y = (F32)(p.mdV[VY] - square.first.mdV[VY]);
            if (x >= 0.f && x < (F32)square.second && y >= 0.f && y < (F32)square.second)
            {
                return true;
            }
        }
        return false;
    };
    const std::vector<const sknav::Record*>& kept = job.mKept;
    const std::vector<RenderedPart>& sculpts = job.mSculpts;
    std::map<std::pair<S64, S64>, Bins> regions;
    std::vector<LLVector3> tris;
    Built::CoverStats stats;
    const auto cover_began = std::chrono::steady_clock::now();
    const sknav::InvisibleParams cover_params;
    // The verdicts this build asked for; the rest are dropped with the parts they were for.
    std::unordered_map<LLUUID, Verdict> verdicts;
    for (size_t i = 0; i < kept.size() + sculpts.size(); ++i)
    {
        const bool is_sculpt = i >= kept.size();
        const LLVector3d origin = is_sculpt ? sculpts[i - kept.size()].mOrigin : kept[i]->mOrigin;
        const std::pair<S64, S64> key((S64)std::llround(origin.mdV[VX]), (S64)std::llround(origin.mdV[VY]));
        Bins& bins = regions[key];
        bins.mOrigin = origin;
        F32 part_height = 0.f;
        U8 fixed_class = skoverview::NUM_CENSUS_CLASSES;
        const std::vector<LLVector3>* source = &tris;
        if (is_sculpt)
        {
            const RenderedPart& sc = sculpts[i - kept.size()];
            source = sc.mTris.get();
            part_height = sc.mHeight;
            fixed_class = sc.mFixed;
        }
        else
        {
            const sknav::Record& rec = *kept[i];
            tris.clear();
            sknav::emitRecord(rec, rec.mOrigin, tris);
            part_height = (F32)(rec.mMax.mdV[VZ] - rec.mMin.mdV[VZ]);
            fixed_class = job.mFixed[i];
        }
        const bool in_void_only = i < kept.size() && job.mVoidOnly[i];
        const U32 group = job.mGroupOf[i];
        // An invisible part on visible geometry stands in for it; one with nothing on it is a barrier, drawn as its
        // class and hatched over every face. Not known yet, it is drawn unhatched: a missed barrier costs less than a
        // hatched building. Either, when large, is a shell nobody plays against.
        bool hatch = false;
        bool pass_through = false;
        const CoverInput* cover = i < job.mCover.size() ? &job.mCover[i] : nullptr;
        if (cover && cover->mTest)
        {
            LL_PROFILE_ZONE_NAMED("sk overview invisible cover");
            const LLUUID& part = is_sculpt ? sculpts[i - kept.size()].mPart : kept[i]->mPart;
            // The part's own geometry: its record's signature, or the sculpt triangles it shares with the cache.
            U64 part_key = skoverview::scramble(cover->mKey ^ (U64)source->size());
            part_key = skoverview::scramble(part_key ^ (is_sculpt ? (U64)(uintptr_t)source : kept[i]->mSignature));
            if (!source->empty())
            {
                const LLVector3& first = source->front();
                part_key = skoverview::scramble(part_key ^ (U64)(S64)std::llround(first.mV[VX] * 64.f) ^
                                                ((U64)(S64)std::llround(first.mV[VY] * 64.f) << 21) ^
                                                ((U64)(S64)std::llround(first.mV[VZ] * 64.f) << 42));
            }
            ++stats.mParts;
            Verdict judged;
            bool known = false;
            if (job.mVerdicts)
            {
                const auto it = job.mVerdicts->mByPart.find(part);
                if (it != job.mVerdicts->mByPart.end() && it->second.mKey == part_key)
                {
                    judged = it->second;
                    known = true;
                }
            }
            if (!known)
            {
                ++stats.mJudged;
                std::vector<sknav::InvisibleSpan> spans;
                for (const auto& geom : cover->mGeoms)
                {
                    // A neighbour region's part is brought into this one's frame by the span's offset.
                    sknav::InvisibleSpan span{ geom->mTris.data(), geom->mTris.size() };
                    span.mOffset = LLVector3(geom->mOrigin - origin);
                    spans.push_back(span);
                }
                const auto verdict_began = std::chrono::steady_clock::now();
                const sknav::InvisibleResult result = sknav::invisibleVerdict(*source, spans, cover_params);
                stats.mVerdictSeconds += std::chrono::duration<F64>(std::chrono::steady_clock::now() - verdict_began).count();
                judged.mKey = part_key;
                judged.mVerdict = (U8)result.mVerdict;
                judged.mSteepM2 = result.mSteepArea;
                for (const F32 share : result.mShares)
                {
                    if (share >= 0.f)
                    {
                        ++(share >= cover_params.mCoveredShare ? judged.mCovered : judged.mBare);
                    }
                }
            }
            verdicts[part] = judged;
            sknav::InvisibleVerdict verdict = (sknav::InvisibleVerdict)judged.mVerdict;
            stats.mFacesCovered += judged.mCovered;
            stats.mFacesBare += judged.mBare;
            // More geometry can only cover more: a stand-in is final, a barrier waits for what is on its way.
            if (verdict == sknav::InvisibleVerdict::BARRIER && cover->mPending)
            {
                verdict = sknav::InvisibleVerdict::UNJUDGED;
            }
            if (verdict != sknav::InvisibleVerdict::STAND_IN)
            {
                LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
                for (const LLVector3& v : *source)
                {
                    update_min_max(lo, hi, v);
                }
                const LLVector3 size = hi - lo;
                if (llmax(size.mV[VX], llmax(size.mV[VY], size.mV[VZ])) > skoverview::INVISIBLE_WALL_M)
                {
                    ++stats.mDropped;
                    continue;
                }
                pass_through = true;
                hatch = verdict == sknav::InvisibleVerdict::BARRIER;
                stats.mHatchedSteepM2 += hatch ? judged.mSteepM2 : 0.f;
            }
            ++(verdict == sknav::InvisibleVerdict::STAND_IN ? stats.mStandIns
               : hatch                                       ? stats.mBarriers
                                                             : stats.mUnknown);
        }
        // A part with an edge not shared by an even count of its triangles is open (single-sided planes, often a cheap
        // triangle-mesh physics shape): it has no inside, so the cut draws its edge but no section fill.
        bool open = false;
        {
            auto vertexKey = [](const LLVector3& v)
            {
                return skoverview::scramble((U64)(S64)std::llround(v.mV[VX] * 1000.f) ^
                                            ((U64)(S64)std::llround(v.mV[VY] * 1000.f) << 21) ^
                                            ((U64)(S64)std::llround(v.mV[VZ] * 1000.f) << 42));
            };
            std::unordered_map<U64, U32> edges;
            edges.reserve(source->size());
            for (size_t t = 0; t + 2 < source->size(); t += 3)
            {
                const U64 k[3] = { vertexKey((*source)[t]), vertexKey((*source)[t + 1]), vertexKey((*source)[t + 2]) };
                if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2])
                {
                    continue; // degenerate, skipped below as well
                }
                for (S32 e = 0; e < 3; ++e)
                {
                    const U64 lo = llmin(k[e], k[(e + 1) % 3]);
                    const U64 hi = llmax(k[e], k[(e + 1) % 3]);
                    ++edges[skoverview::scramble(lo ^ skoverview::scramble(hi))];
                }
            }
            for (const auto& edge : edges)
            {
                if (edge.second & 1)
                {
                    open = true;
                    break;
                }
            }
        }
        const U8 tri_flags = (pass_through ? skoverview::TRI_PASS_THROUGH : 0) | (open ? skoverview::TRI_OPEN : 0);
        for (size_t t = 0; t + 2 < source->size(); t += 3)
        {
            const LLVector3& a = (*source)[t];
            const LLVector3& b = (*source)[t + 1];
            const LLVector3& c = (*source)[t + 2];
            if (!a.isFinite() || !b.isFinite() || !c.isFinite())
            {
                continue;
            }
            LLVector3 n = (b - a) % (c - a);
            const F32 len = n.length();
            if (len <= 1.0e-8f)
            {
                continue;
            }
            if (in_void_only && overLand(origin + LLVector3d((a + b + c) / 3.f)))
            {
                continue;
            }
            const skoverview::CensusClass cls = fixed_class < skoverview::NUM_CENSUS_CLASSES
                                                    ? (skoverview::CensusClass)fixed_class
                                                    : skoverview::classifyTriangle(n.mV[VZ] / len, part_height);
            Bounds& bounds = bins.mBounds[group];
            for (const LLVector3* v : { &a, &b, &c })
            {
                update_min_max(bounds.mMin, bounds.mMax, *v);
            }
            std::vector<LLVector3>& out = bins.mClass[cls][group];
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            bins.mInvisible[cls][group].push_back(tri_flags);
            if (hatch)
            {
                // The same triangle again in the invisible range: the capture's overlay pass hatches it.
                std::vector<LLVector3>& over = bins.mClass[skoverview::INVISIBLE][group];
                over.push_back(a);
                over.push_back(b);
                over.push_back(c);
                bins.mInvisible[skoverview::INVISIBLE][group].push_back(tri_flags | skoverview::TRI_PASS_THROUGH);
            }
        }
    }
    stats.mBuildSeconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - cover_began).count();

    auto built = std::make_shared<Built>();
    built->mVerdicts = std::make_shared<const Verdicts>(Verdicts{ std::move(verdicts) });
    built->mSignature = job.mSignature;
    built->mGeneration = job.mGeneration;
    built->mCover = stats;
    for (auto& entry : regions)
    {
        Bins& bins = entry.second;
        size_t total = 0;
        for (const auto& groups : bins.mClass)
        {
            for (const auto& g : groups)
            {
                total += g.second.size();
            }
        }
        if (!total)
        {
            continue;
        }
        Built::Region region;
        region.mOrigin = bins.mOrigin;
        std::map<U32, size_t> group_index;
        for (const auto& b : bins.mBounds)
        {
            group_index[b.first] = region.mGroups.size();
            Group group;
            group.mMinZ = b.second.mMin.mV[VZ];
            group.mMaxZ = b.second.mMax.mV[VZ];
            std::pair<LLVector3d, LLVector3d> footprint(bins.mOrigin + LLVector3d(b.second.mMin), bins.mOrigin + LLVector3d(b.second.mMax));
            const auto cluster = job.mGroupCluster.find(b.first);
            if (cluster != job.mGroupCluster.end())
            {
                const sknav::Cluster& c = *job.mSnapshot->mClusters.mClusters[cluster->second];
                group.mMinZ = (F32)c.mMin.mdV[VZ];
                group.mMaxZ = (F32)c.mMax.mdV[VZ];
                footprint = { c.mMin, c.mMax };
            }
            group.mId = b.first;
            group.mRoot = b.first < job.mRoots.size() ? job.mRoots[b.first] : LLUUID::null;
            group.mMin = bins.mOrigin + LLVector3d(b.second.mMin);
            group.mMax = bins.mOrigin + LLVector3d(b.second.mMax);
            if (b.first < job.mAnchors.size())
            {
                group.mAnchor = job.mAnchors[b.first].first;
                group.mNeverCut = job.mAnchors[b.first].second;
            }
            region.mGroups.push_back(group);
            region.mFootprints.push_back(footprint);
        }
        // A buffer holds at most MAX_BATCH_VERTICES, so a larger region is laid out over several batches with the
        // same origin, each a class-ordered run of whole groups where they fit and pieces of a group where one does not.
        const size_t limit = skoverview::MAX_BATCH_VERTICES - skoverview::MAX_BATCH_VERTICES % 3;
        const Built::Region blank = region;
        Built::Region piece = blank;
        std::vector<bool> present(piece.mGroups.size(), false);
        size_t pieces = 0;
        U32 cls = 0;
        auto openClasses = [&](U32 upto)
        {
            for (; cls <= upto; ++cls)
            {
                piece.mStart[cls] = (U32)piece.mPositions.size();
            }
        };
        auto finish = [&]()
        {
            openClasses(skoverview::NUM_CENSUS_CLASSES);
            if (piece.mPositions.empty())
            {
                return;
            }
            Built::Region done = blank;
            done.mPositions = std::move(piece.mPositions);
            U64 hash = 1469598103934665603ull;
            for (const LLVector3& p : done.mPositions)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    U32 bits;
                    std::memcpy(&bits, &p.mV[k], sizeof(bits));
                    hash = (hash ^ bits) * 1099511628211ull;
                }
            }
            done.mHash = hash;
            done.mInvisible = std::move(piece.mInvisible);
            std::copy(std::begin(piece.mStart), std::end(piece.mStart), std::begin(done.mStart));
            done.mGroups.clear();
            done.mFootprints.clear();
            for (size_t i = 0; i < present.size(); ++i)
            {
                if (!present[i])
                {
                    continue;
                }
                Group group = piece.mGroups[i];
                if (total > limit)
                {
                    // Its bounds in this batch alone.
                    LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
                    for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
                    {
                        for (U32 v = group.mStart[c]; v < group.mStart[c] + group.mCount[c]; ++v)
                        {
                            update_min_max(lo, hi, done.mPositions[v]);
                        }
                    }
                    group.mMin = bins.mOrigin + LLVector3d(lo);
                    group.mMax = bins.mOrigin + LLVector3d(hi);
                }
                done.mGroups.push_back(group);
                done.mFootprints.push_back(blank.mFootprints[i]);
            }
            built->mRegions.push_back(std::move(done));
            ++pieces;
            piece = blank;
            present.assign(present.size(), false);
            cls = 0;
        };
        piece.mPositions.reserve(llmin(total, limit));
        for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
        {
            for (const auto& g : bins.mClass[c])
            {
                const size_t index = group_index[g.first];
                const std::vector<U8>& flags = bins.mInvisible[c][g.first];
                size_t from = 0;
                while (from < g.second.size())
                {
                    if (piece.mPositions.size() + (g.second.size() - from) > limit && !piece.mPositions.empty())
                    {
                        finish();
                    }
                    const size_t take = llmin(g.second.size() - from, limit - piece.mPositions.size());
                    openClasses(c);
                    Group& group = piece.mGroups[index];
                    group.mStart[c] = (U32)piece.mPositions.size();
                    group.mCount[c] = (U32)take;
                    present[index] = true;
                    piece.mPositions.insert(piece.mPositions.end(), g.second.begin() + from, g.second.begin() + from + take);
                    piece.mInvisible.insert(piece.mInvisible.end(), flags.begin() + from / 3, flags.begin() + (from + take) / 3);
                    from += take;
                }
            }
        }
        finish();
        if (pieces > 1)
        {
            built->mSplit.emplace_back(total, pieces);
        }
    }
    return built;
}

namespace
{
    /// Writes each vertex's linkset id (Group::mId) into the weight channel, which the cut table is read through.
    void writeGroupIds(LLStrider<F32> ids, U32 total, const std::vector<SKOverviewCensus::Group>& groups)
    {
        for (U32 v = 0; v < total; ++v)
        {
            ids[v] = 0.f;
        }
        for (const SKOverviewCensus::Group& group : groups)
        {
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                const U32 end = llmin(group.mStart[c] + group.mCount[c], total);
                for (U32 v = llmin(group.mStart[c], end); v < end; ++v)
                {
                    ids[v] = (F32)group.mId;
                }
            }
        }
    }

    /// Whether two builds of a region give its vertices the same linkset ids.
    bool sameGroupIds(const std::vector<SKOverviewCensus::Group>& a, const std::vector<SKOverviewCensus::Group>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const SKOverviewCensus::Group& x, const SKOverviewCensus::Group& y)
                          {
                              return x.mId == y.mId && std::equal(std::begin(x.mStart), std::end(x.mStart), std::begin(y.mStart)) &&
                                     std::equal(std::begin(x.mCount), std::end(x.mCount), std::begin(y.mCount));
                          });
    }
}

void SKOverviewCensus::swapIn(Built& built)
{
    LL_PROFILE_ZONE_SCOPED;
    for (const auto& split : built.mSplit)
    {
        LL_INFOS("SKOverview") << "census region of " << split.first << " vertices split into " << split.second
                               << " batches of at most " << skoverview::MAX_BATCH_VERTICES << LL_ENDL;
    }
    // Every region of a build lands in the same frame, as one census.
    std::vector<Batch> batches;
    U32 triangles = 0;
    for (Built::Region& region : built.mRegions)
    {
        const U32 total = (U32)region.mPositions.size();
        Batch batch;
        batch.mOrigin = region.mOrigin;
        batch.mHash = region.mHash;
        // A region whose triangles are the same keeps its buffer: a rebuild moves a few linksets, and uploading every
        // region's tens of megabytes again stalled the frame for tens of milliseconds.
        const auto same = std::find_if(mBatches.begin(), mBatches.end(),
                                       [&](const Batch& old)
                                       {
                                           return old.mBuffer && old.mOrigin == region.mOrigin && old.mHash == region.mHash &&
                                                  old.mPositions.size() == region.mPositions.size() &&
                                                  old.mInvisible == region.mInvisible;
                                       });
        if (same != mBatches.end())
        {
            LL_PROFILE_ZONE_NAMED("sk census keep buffer");
            batch.mBuffer = std::move(same->mBuffer);
            batch.mCutValues = std::move(same->mCutValues);
            // A linkset added elsewhere shifts every later id while this region's bytes stay the same.
            LLStrider<F32> ids;
            if (!sameGroupIds(same->mGroups, region.mGroups) && batch.mBuffer->getWeightStrider(ids, 0, (S32)total) && ids.get())
            {
                writeGroupIds(ids, total, region.mGroups);
                batch.mBuffer->unmapBuffer();
            }
        }
        else
        {
            LL_PROFILE_ZONE_NAMED("sk census upload buffer");
            batch.mBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_WEIGHT | LLVertexBuffer::MAP_TEXCOORD0); // weight: the linkset id; texcoord0.x: an open part's
            if (!batch.mBuffer->allocateBuffer(total, 0))
            {
                LL_WARNS("SKOverview") << "census buffer of " << total << " vertices could not be allocated" << LL_ENDL;
                continue;
            }
            LLStrider<LLVector3> verts;
            if (!batch.mBuffer->getVertexStrider(verts) || !verts.get() || batch.mBuffer->getNumVerts() < total)
            {
                LL_WARNS("SKOverview") << "census batch of " << total << " vertices could not be mapped (buffer holds "
                                       << batch.mBuffer->getNumVerts() << ")" << LL_ENDL;
                continue;
            }
            for (const LLVector3& p : region.mPositions)
            {
                *verts++ = p;
            }
            LLStrider<F32> ids;
            if (batch.mBuffer->getWeightStrider(ids) && ids.get())
            {
                writeGroupIds(ids, total, region.mGroups);
            }
            LLStrider<LLVector2> open;
            if (batch.mBuffer->getTexCoord0Strider(open) && open.get())
            {
                for (U32 v = 0; v < total; ++v)
                {
                    const bool is_open = v / 3 < region.mInvisible.size() && (region.mInvisible[v / 3] & skoverview::TRI_OPEN);
                    *open++ = LLVector2(is_open ? 1.f : 0.f, 0.f);
                }
            }
            batch.mBuffer->unmapBuffer();
        }
        // On the ground when its cluster's base is no higher above the highest land under its footprint than a step or two.
        for (size_t g = 0; g < region.mGroups.size(); ++g)
        {
            Group& group = region.mGroups[g];
            group.mGround = group.mMinZ <= SKWorldCensus::landUnder(region.mFootprints[g].first, region.mFootprints[g].second) +
                                               skoverview::GROUND_BASE_M;
        }
        std::copy(std::begin(region.mStart), std::end(region.mStart), std::begin(batch.mStart));
        batch.mGroups = std::move(region.mGroups);
        batch.mPositions = std::move(region.mPositions);
        batch.mInvisible = std::move(region.mInvisible);
        triangles += total / 3;
        batches.push_back(std::move(batch));
    }
    mBatches = std::move(batches);
    mTriangles = triangles;
    mSignature = built.mSignature;
    mBuilt = true;
    // applyCut's extents point into the batches just replaced.
    mExtentsBuilt = false;
    mOccupantCuts.clear();
    mCuts.clear();
    mCutSignature = 0;
    ++mCutStamp;
    LL_DEBUGS("SKOverview") << "census upload: " << mBatches.size() << " regions, " << mTriangles << " triangles" << LL_ENDL;
    const Built::CoverStats& c = built.mCover;
    if (c.mParts)
    {
        // Logged only when the counts move: the same parts judged the same way are no news.
        const std::string counts = llformat("%u invisible parts: %u stand-ins, %u barriers (%.0f m2 of steep faces hatched), "
                                            "%u not known yet, %u large dropped; faces %u covered %u bare",
                                            c.mParts, c.mStandIns, c.mBarriers, c.mHatchedSteepM2, c.mUnknown, c.mDropped,
                                            c.mFacesCovered, c.mFacesBare);
        if (counts != mCoverLogged)
        {
            mCoverLogged = counts;
            std::string where;
            for (const LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
            {
                where += (where.empty() ? "" : ", ") + region->getName();
            }
            LL_INFOS("SKOverview") << "census cover in " << where << ": " << counts << ", " << c.mJudged << " judged in "
                                   << llformat("%.2f ms of verdicts, build %.2f ms", c.mVerdictSeconds * 1000.0,
                                               c.mBuildSeconds * 1000.0) << LL_ENDL;
        }
    }
}

F32 SKOverviewCensus::levelTop(const Level& level, F32 floor) const
{
    F32 top = floor;
    for (const Batch& batch : mBatches)
    {
        for (const Group& group : batch.mGroups)
        {
            if (level.keeps(group))
            {
                top = llmax(top, group.mMaxZ);
            }
        }
    }
    return top;
}

namespace
{
    // Whether a part reaching this far from its centre crosses into the void: no region under one of its sides.
    bool reachesVoid(const LLVector3d& centre, F32 reach)
    {
        static const F64 SIDES[4][2] = { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 } };
        for (const auto& side : SIDES)
        {
            const LLVector3d at = centre + LLVector3d(side[0] * reach, side[1] * reach, 0.0);
            if (!LLWorld::getInstance()->getRegionFromPosGlobal(at))
            {
                return true;
            }
        }
        return false;
    }
}

void SKOverviewCensus::gatherSurrounds(const SKCensusSnapshot& snap)
{
    LL_PROFILE_ZONE_SCOPED;
    // Mega parts, at least a region on some axis or reaching into the void, that the census holds no shape for
    // (megasculpts, phantom or physics-less meshes and megaprims) are drawn from what is rendered, wherever they are
    // in the loaded regions, so the map shows the land they make.
    std::set<LLUUID> recorded;
    for (const sknav::RecordPtr& rec : snap.mRecords)
    {
        if (rec)
        {
            recorded.insert(rec->mPart);
        }
    }
    mSurrounds.clear();
    U64 sig = 0; // summed per part: the object list's order churns
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
        if (!volume || volume->isDead() || volume->isAttachment() || volume->isAvatar() || !volume->getRegion() ||
            !volume->getVolume() || volume->getAvatar())
        {
            continue;
        }
        const LLVector3 scale = volume->getScale();
        const F32 size = llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ]));
        if (size < volume->getRegion()->getWidth() && !reachesVoid(volume->getPositionGlobal(), size * 0.5f))
        {
            continue;
        }
        const bool sculpt = volume->isSculpted() && !volume->isMesh();
        if (!sculpt && recorded.count(volume->getID()))
        {
            continue;
        }
        if (volume->isSculpted() && volume->getVolume()->getSculptLevel() != 0)
        {
            continue; // its map or mesh has not loaded: a placeholder, not its shape
        }
        if (seenOf(volume->getID()) != Seen::VISIBLE)
        {
            continue;
        }
        SKShapeGeometry geom;
        if (!skCensusRenderGeometry(volume, geom))
        {
            continue;
        }
        RenderedPart part;
        part.mPart = volume->getID();
        part.mOrigin = volume->getRegion()->getOriginGlobal();
        part.mAnchor = volume->getRootEdit()->getPositionGlobal();
        part.mRoot = volume->getRootEdit()->getID();
        const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
        auto tris = std::make_shared<std::vector<LLVector3>>();
        tris->reserve(geom.mTriangles.size());
        F32 lo = 1e30f, hi = -1e30f;
        for (const LLVector3& v : geom.mTriangles)
        {
            const LLVector3 local = volume->volumePositionToAgent(v) - region_agent;
            lo = llmin(lo, local.mV[VZ]);
            hi = llmax(hi, local.mV[VZ]);
            tris->push_back(local);
        }
        part.mHeight = hi - lo;
        part.mTris = std::move(tris);
        const LLVector3d centre = volume->getPositionGlobal();
        U64 h = 1469598103934665603ull;
        h = (h ^ part.mPart.getCRC32()) * 1099511628211ull;
        h = (h ^ (U64)part.mTris->size()) * 1099511628211ull;
        h = (h ^ ((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
                  ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42))) * 1099511628211ull;
        sig += skoverview::scramble(h);
        mSurrounds.push_back(std::move(part));
    }
    mSurroundsSig = sig;
}

namespace
{
    // The distance in plan from p to the triangle (a, b, c): 0 inside it; a wall's edge-on triangle is its segment.
    F32 planDistance(const LLVector2& p, const LLVector3& a3, const LLVector3& b3, const LLVector3& c3)
    {
        const LLVector2 a(a3.mV[VX], a3.mV[VY]), b(b3.mV[VX], b3.mV[VY]), c(c3.mV[VX], c3.mV[VY]);
        auto cross = [](const LLVector2& o, const LLVector2& u, const LLVector2& v)
        {
            return (u.mV[VX] - o.mV[VX]) * (v.mV[VY] - o.mV[VY]) - (u.mV[VY] - o.mV[VY]) * (v.mV[VX] - o.mV[VX]);
        };
        const F32 area = cross(a, b, c);
        if (std::fabs(area) > 1.0e-6f)
        {
            const F32 s0 = cross(a, b, p) / area, s1 = cross(b, c, p) / area, s2 = cross(c, a, p) / area;
            if (s0 >= 0.f && s1 >= 0.f && s2 >= 0.f)
            {
                return 0.f;
            }
        }
        auto segment = [&p](const LLVector2& u, const LLVector2& v)
        {
            const LLVector2 d = v - u;
            const F32 len2 = d * d;
            const F32 t = len2 > 0.f ? llclamp(((p - u) * d) / len2, 0.f, 1.f) : 0.f;
            return (u + d * t - p).length();
        };
        return llmin(segment(a, b), llmin(segment(b, c), segment(c, a)));
    }
}

std::string SKOverviewCensus::Reason::text() const
{
    std::string out = mFixed ? std::string(mFixed)
                             : (mSelf ? std::string("agent") : "occupant " + mWho.asString()) + " " + mRule;
    return mReleasing ? out + " (releasing)" : out;
}

void SKOverviewCensus::applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, U64 key)
{
    LL_PROFILE_ZONE_SCOPED;
    if (key == mCutKey && mCutSignature == mSignature)
    {
        return;
    }
    mCutKey = key;
    if (!mExtentsBuilt || mExtentsSignature != mSignature)
    {
        LL_PROFILE_ZONE_NAMED("sk cut extents");
        // A linkset spans batches when it crosses a border: its bounds over all of them, and where its triangles are.
        mExtents.clear();
        mOccupantCuts.clear();
        for (const Batch& batch : mBatches)
        {
            for (const Group& group : batch.mGroups)
            {
                Extent& e = mExtents[group.mId];
                for (S32 i = 0; i < 3; ++i)
                {
                    e.mMin.mdV[i] = llmin(e.mMin.mdV[i], group.mMin.mdV[i]);
                    e.mMax.mdV[i] = llmax(e.mMax.mdV[i], group.mMax.mdV[i]);
                }
                e.mAnchor = group.mAnchor;
                e.mNeverCut = group.mNeverCut;
                e.mRoot = group.mRoot;
                e.mGround |= group.mGround;
                e.mBottom = llmin(e.mBottom, (F64)group.mMinZ);
                e.mParts.emplace_back(&batch, &group);
            }
        }
        mRootExtent.clear();
        mFloating.clear();
        for (const auto& entry : mExtents)
        {
            mRootExtent[entry.second.mRoot] = entry.first;
            if (!entry.second.mGround)
            {
                mFloating.push_back(entry.first);
            }
        }
        mExtentsSignature = mSignature;
        mExtentsBuilt = true;
        mExtentsRegion = ~0ull;
    }
    const U64 region_handle = region ? region->getHandle() : 0;
    if (mExtentsRegion != region_handle)
    {
        for (auto& entry : mExtents)
        {
            Extent& e = entry.second;
            e.mCuttable = !e.mNeverCut && region && region->pointInRegionGlobal(e.mAnchor);
        }
        mExtentsRegion = region_handle;
        mOccupantCuts.clear();
    }
    const std::map<U32, Extent>& extents = mExtents;
    for (auto& entry : mOccupantCuts)
    {
        entry.second.mSeen = false;
    }
    // A person's own work is kept while they stay within a quarter metre, as the key the caller gives is.
    auto quantise = [](U64& k, F64 v) { k = (k ^ (U64)(S64)std::llround(v * 4.0)) * 1099511628211ull; };

    const F64 SLACK_M = 0.1;    // column: how far outside a linkset's bounds still counts as under it
    const F64 ABOVE_M = 0.5;    // what rises less than this over the floor bounds nothing
    const F64 OVERHEAD_M = 1.0; // a floating cluster whose base is this far over an occupant's cut is out of their space
    auto overhead = [&](const Extent& e, const Occupant& occupant)
    {
        return !e.mGround && e.mBottom > occupant.mCut + OVERHEAD_M;
    };
    std::map<U32, F32> cut_of;
    std::map<U32, Reason> why;
    std::set<U32> for_agent;
    std::map<U32, F32> priority_of;
    std::set<U32> agent_under;  // linksets standing over the agent, whose cut no one on their roof overrides
    auto reasonOf = [](const Occupant& occupant, const std::string& rule)
    {
        Reason r;
        r.mWho = occupant.mId;
        r.mSelf = occupant.mSelf;
        r.mRule = rule;
        return r;
    };
    auto fixedReason = [](const char* rule)
    {
        Reason r;
        r.mFixed = rule;
        return r;
    };
    auto mark = [&](U32 id, const Occupant& occupant, const std::string& rule)
    {
        if (occupant.mSelf)
        {
            agent_under.insert(id);
            if (for_agent.insert(id).second)
            {
                cut_of[id] = occupant.mCut;
                why[id] = reasonOf(occupant, rule);
            }
            return;
        }
        if (for_agent.count(id))
        {
            return;
        }
        auto it = cut_of.find(id);
        if (it == cut_of.end() || occupant.mPriority > priority_of[id])
        {
            cut_of[id] = occupant.mCut;
            priority_of[id] = occupant.mPriority;
            why[id] = reasonOf(occupant, rule);
        }
    };
    std::vector<Occupant> people(occupants);
    std::vector<const Occupant*> ordered;
    for (const Occupant& occupant : people)
    {
        if (occupant.mSelf)
        {
            ordered.insert(ordered.begin(), &occupant);
        }
        else
        {
            ordered.push_back(&occupant);
        }
    }
    // Whether a linkset has a triangle above p's floor whose plan footprint holds p (within the slack): it stands
    // over them, not merely its box.
    auto overColumn = [&](const Extent& e, const LLVector3d& p) -> bool
    {
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const LLVector3 local(p - batch.mOrigin);
            const LLVector2 at(local.mV[VX], local.mV[VY]);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    if (v / 3 < batch.mInvisible.size() && (batch.mInvisible[v / 3] & skoverview::TRI_PASS_THROUGH))
                    {
                        continue;
                    }
                    const LLVector3& a = batch.mPositions[v];
                    const LLVector3& b = batch.mPositions[v + 1];
                    const LLVector3& d = batch.mPositions[v + 2];
                    if (llmax(a.mV[VZ], llmax(b.mV[VZ], d.mV[VZ])) >= local.mV[VZ] + ABOVE_M &&
                        planDistance(at, a, b, d) <= SLACK_M)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    // Who stands on a roof: the highest upward-facing surface under their feet, of a linkset with nothing over them.
    const F64 STAND_BELOW_M = 0.6;  // how far under the feet a surface still bears them
    const F64 STAND_ABOVE_M = 0.4;  // how far over them, for a sunk or estimated foot
    const F64 HOLD_S = 1.0;         // a roof stays kept this long after its stander leaves it, through a hop
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    auto heightAt = [](const LLVector2& at, const LLVector3& a, const LLVector3& b, const LLVector3& d, F32& z) -> bool
    {
        const LLVector3 n = (b - a) % (d - a);
        if (std::fabs(n.mV[VZ]) < 0.5f * n.length() || planDistance(at, a, b, d) > 0.f)
        {
            return false; // a wall, or not under the point
        }
        z = a.mV[VZ] - (n.mV[VX] * (at.mV[VX] - a.mV[VX]) + n.mV[VY] * (at.mV[VY] - a.mV[VY])) / n.mV[VZ];
        return true;
    };
    // The highest surface of the linkset under the feet within the bearing band, and whether any of it covers them.
    auto standsOn = [&](const Extent& e, const LLVector3d& feet, F64& surface, bool& covered) -> bool
    {
        bool found = false;
        bool cover = false;
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const LLVector3 local(feet - batch.mOrigin);
            const LLVector2 at(local.mV[VX], local.mV[VY]);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    if (v / 3 < batch.mInvisible.size() && (batch.mInvisible[v / 3] & skoverview::TRI_PASS_THROUGH))
                    {
                        continue;
                    }
                    F32 z = 0.f;
                    if (!heightAt(at, batch.mPositions[v], batch.mPositions[v + 1], batch.mPositions[v + 2], z))
                    {
                        continue;
                    }
                    if (z >= local.mV[VZ] - STAND_BELOW_M && z <= local.mV[VZ] + STAND_ABOVE_M)
                    {
                        const F64 global_z = batch.mOrigin.mdV[VZ] + z;
                        surface = found ? llmax(surface, global_z) : global_z;
                        found = true;
                    }
                    else if (z > local.mV[VZ] + STAND_ABOVE_M)
                    {
                        cover = true;
                    }
                }
            }
        }
        covered = cover;
        return found;
    };
    std::map<U32, F32> roof_of;     // linkset -> the highest cut of those standing on its roof
    bool holding = false;
    {
        LL_PROFILE_ZONE_NAMED("sk cut roofs");
        for (Occupant& occupant : people)
        {
            const LLVector3d& feet = occupant.mFeet;
            OccupantCut& cache = mOccupantCuts[occupant.mId];
            cache.mSeen = true;
            U64 roof_key = 1469598103934665603ull;
            for (S32 i = 0; i < 3; ++i)
            {
                quantise(roof_key, feet.mdV[i]);
            }
            if (!cache.mRoofValid || cache.mRoofKey != roof_key)
            {
                cache.mBest = 0;
                cache.mBestZ = -1e30;
                cache.mCovered = false;
                for (const auto& entry : extents)
                {
                    const Extent& e = entry.second;
                    if (!e.mCuttable || feet.mdV[VX] < e.mMin.mdV[VX] || feet.mdV[VX] > e.mMax.mdV[VX] ||
                        feet.mdV[VY] < e.mMin.mdV[VY] || feet.mdV[VY] > e.mMax.mdV[VY] ||
                        feet.mdV[VZ] < e.mMin.mdV[VZ] - STAND_ABOVE_M || feet.mdV[VZ] > e.mMax.mdV[VZ] + STAND_BELOW_M)
                    {
                        continue;
                    }
                    F64 z = 0.0;
                    bool covered = false;
                    if (standsOn(e, feet, z, covered) && z > cache.mBestZ)
                    {
                        cache.mBest = entry.first;
                        cache.mBestZ = z;
                        cache.mCovered = covered;
                    }
                }
                // A surface is a roof only with nothing over the feet, of any linkset: a storey's slab is often one
                // linkset and the storey above it another. Skyboxes floating out of their space do not count.
                for (const auto& entry : extents)
                {
                    const Extent& e = entry.second;
                    if (cache.mCovered || cache.mBestZ <= -1e29)
                    {
                        break;
                    }
                    if (entry.first == cache.mBest || feet.mdV[VX] < e.mMin.mdV[VX] || feet.mdV[VX] > e.mMax.mdV[VX] ||
                        feet.mdV[VY] < e.mMin.mdV[VY] || feet.mdV[VY] > e.mMax.mdV[VY] || e.mMax.mdV[VZ] <= feet.mdV[VZ] + STAND_ABOVE_M ||
                        (!e.mGround && e.mBottom > feet.mdV[VZ] + OVERHEAD_M))
                    {
                        continue;
                    }
                    F64 z = 0.0;
                    bool covered = false;
                    standsOn(e, feet, z, covered);
                    cache.mCovered = covered;
                }
                cache.mRoofKey = roof_key;
                cache.mRoofValid = true;
            }
            const U32 best = cache.mBest;
            const F64 best_z = cache.mBestZ;
            const bool best_covered = cache.mCovered;
            U32 roof = 0;
            F64 roof_z = 0.0;
            const bool on_surface = best_z > -1e29;
            const auto held = mStanding.find(occupant.mId);
            if (on_surface && !best_covered)
            {
                roof = best;
                roof_z = best_z;
                mStanding[occupant.mId] = { extents.at(best).mRoot, best_z, now };
            }
            else if (!on_surface && held != mStanding.end() && now - held->second.mAt < HOLD_S)
            {
                // Off every surface, in a hop: the roof they left is kept a moment longer.
                for (const auto& entry : extents)
                {
                    if (entry.second.mRoot == held->second.mRoot && entry.second.mCuttable)
                    {
                        roof = entry.first;
                        roof_z = held->second.mZ;
                        holding = true;
                        break;
                    }
                }
            }
            else if (held != mStanding.end())
            {
                mStanding.erase(held);
            }
            if (roof)
            {
                const F64 above = occupant.mCut - occupant.mFloor.mdV[VZ];
                occupant.mFloor.setVec(feet.mdV[VX], feet.mdV[VY], roof_z);
                occupant.mCut = (F32)(roof_z + above);
                occupant.mRoomCells.reset();
                const auto it = roof_of.find(roof);
                roof_of[roof] = it == roof_of.end() ? occupant.mCut : llmax(it->second, occupant.mCut);
            }
        }
    }
    for (auto it = mStanding.begin(); it != mStanding.end();)
    {
        it = now - it->second.mAt >= HOLD_S ? mStanding.erase(it) : std::next(it);
    }
    // Whether a linkset has a triangle above the floor whose plan footprint comes within a cell of one of the room's
    // dilated cells; how says which (wall or cover) and the cell, for the log.
    std::unordered_set<U64> dilated;
    LLVector3d room_min, room_max;
    auto cellKey = [](S32 x, S32 y) { return ((U64)(U32)x << 32) | (U64)(U32)y; };
    auto touchesRoom = [&](const Extent& e, const Occupant& occupant, const std::unordered_set<U64>& cells, F64 cell_m,
                           std::string& how) -> bool
    {
        const F32 reach = (F32)(cell_m * 0.7072);
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const F32 floor_local = (F32)(occupant.mFloor.mdV[VZ] - batch.mOrigin.mdV[VZ]);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    if (v / 3 < batch.mInvisible.size() && (batch.mInvisible[v / 3] & skoverview::TRI_PASS_THROUGH))
                    {
                        continue;
                    }
                    const LLVector3& a = batch.mPositions[v];
                    const LLVector3& b = batch.mPositions[v + 1];
                    const LLVector3& d = batch.mPositions[v + 2];
                    if (llmax(a.mV[VZ], llmax(b.mV[VZ], d.mV[VZ])) < floor_local + ABOVE_M)
                    {
                        continue;
                    }
                    // The cells under the triangle's plan box, as far as the room has any.
                    const F64 lo_x = batch.mOrigin.mdV[VX] + llmin(a.mV[VX], llmin(b.mV[VX], d.mV[VX]));
                    const F64 hi_x = batch.mOrigin.mdV[VX] + llmax(a.mV[VX], llmax(b.mV[VX], d.mV[VX]));
                    const F64 lo_y = batch.mOrigin.mdV[VY] + llmin(a.mV[VY], llmin(b.mV[VY], d.mV[VY]));
                    const F64 hi_y = batch.mOrigin.mdV[VY] + llmax(a.mV[VY], llmax(b.mV[VY], d.mV[VY]));
                    const S32 x0 = (S32)std::floor(llmax(lo_x, room_min.mdV[VX]) / cell_m);
                    const S32 x1 = (S32)std::floor(llmin(hi_x, room_max.mdV[VX]) / cell_m);
                    const S32 y0 = (S32)std::floor(llmax(lo_y, room_min.mdV[VY]) / cell_m);
                    const S32 y1 = (S32)std::floor(llmin(hi_y, room_max.mdV[VY]) / cell_m);
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        for (S32 y = y0; y <= y1; ++y)
                        {
                            if (!cells.count(cellKey(x, y)))
                            {
                                continue;
                            }
                            const LLVector2 centre((F32)((x + 0.5) * cell_m - batch.mOrigin.mdV[VX]),
                                                   (F32)((y + 0.5) * cell_m - batch.mOrigin.mdV[VY]));
                            if (planDistance(centre, a, b, d) <= reach)
                            {
                                const LLVector3 n = (b - a) % (d - a);
                                const bool wall = n.length() > 0.f && std::fabs(n.mV[VZ]) < 0.5f * n.length();
                                how = llformat("%s at cell %d,%d", wall ? "wall" : "cover", x, y);
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    };
    {
        LL_PROFILE_ZONE_NAMED("sk cut marks");
        static const std::string COLUMN("column");
        for (const Occupant* occupant : ordered)
        {
            if (occupant->mOutdoors)
            {
                continue;
            }
            const LLVector3d& p = occupant->mFloor;
            OccupantCut& cache = mOccupantCuts[occupant->mId];
            cache.mSeen = true;
            // What their room or column takes depends on their floor, their cut and their room's cells alone.
            U64 marks_key = 1469598103934665603ull;
            for (S32 i = 0; i < 3; ++i)
            {
                quantise(marks_key, p.mdV[i]);
            }
            quantise(marks_key, occupant->mCut);
            quantise(marks_key, occupant->mRoomCellM);
            if (occupant->mRoomCells)
            {
                U64 cells = 0;
                for (const auto& cell : *occupant->mRoomCells)
                {
                    cells += skoverview::scramble(cellKey(cell.first, cell.second));
                }
                marks_key = (marks_key ^ cells) * 1099511628211ull;
                marks_key = (marks_key ^ (U64)occupant->mRoomCells->size()) * 1099511628211ull;
            }
            if (!cache.mMarksValid || cache.mMarksKey != marks_key)
            {
                cache.mMarks.clear();
                dilated.clear();
                if (occupant->mRoomCells && !occupant->mRoomCells->empty())
                {
                    // The room's cells grown by one, so a wall standing just past its last cell of air still touches it.
                    S32 min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
                    for (const auto& cell : *occupant->mRoomCells)
                    {
                        for (S32 dx = -1; dx <= 1; ++dx)
                        {
                            for (S32 dy = -1; dy <= 1; ++dy)
                            {
                                dilated.insert(cellKey(cell.first + dx, cell.second + dy));
                            }
                        }
                        min_x = llmin(min_x, cell.first - 1);
                        min_y = llmin(min_y, cell.second - 1);
                        max_x = llmax(max_x, cell.first + 1);
                        max_y = llmax(max_y, cell.second + 1);
                    }
                    const F64 cell_m = occupant->mRoomCellM;
                    room_min.setVec(min_x * cell_m, min_y * cell_m, 0.0);
                    room_max.setVec((max_x + 1) * cell_m, (max_y + 1) * cell_m, 0.0);
                }
                for (const auto& entry : extents)
                {
                    const Extent& e = entry.second;
                    if (!e.mCuttable || e.mMax.mdV[VZ] < p.mdV[VZ] + ABOVE_M || overhead(e, *occupant))
                    {
                        continue;
                    }
                    if (occupant->mRoomCells && !dilated.empty())
                    {
                        // The room's building, by its geometry: a linkset whose triangles above the floor come within a
                        // cell of the room's own cells, standing round them (walls) or over them (floors above, the roof).
                        const F64 cell_m = occupant->mRoomCellM;
                        if (e.mMax.mdV[VX] < room_min.mdV[VX] || e.mMin.mdV[VX] > room_max.mdV[VX] ||
                            e.mMax.mdV[VY] < room_min.mdV[VY] || e.mMin.mdV[VY] > room_max.mdV[VY])
                        {
                            continue; // its box is nowhere near the room's
                        }
                        std::string how;
                        if (touchesRoom(e, *occupant, dilated, cell_m, how))
                        {
                            cache.mMarks.emplace_back(entry.first, "room " + how);
                        }
                    }
                    else if (p.mdV[VX] >= e.mMin.mdV[VX] - SLACK_M && p.mdV[VX] <= e.mMax.mdV[VX] + SLACK_M &&
                             p.mdV[VY] >= e.mMin.mdV[VY] - SLACK_M && p.mdV[VY] <= e.mMax.mdV[VY] + SLACK_M &&
                             overColumn(e, p))
                    {
                        cache.mMarks.emplace_back(entry.first, COLUMN);
                    }
                }
                cache.mMarksKey = marks_key;
                cache.mMarksValid = true;
            }
            for (const auto& m : cache.mMarks)
            {
                mark(m.first, *occupant, m.second);
            }
        }
    }
    for (auto it = mOccupantCuts.begin(); it != mOccupantCuts.end();)
    {
        it = it->second.mSeen ? std::next(it) : mOccupantCuts.erase(it);
    }
    {
        LL_PROFILE_ZONE_NAMED("sk cut resolve");
        F32 agent_cut = skoverview::NO_CUT;
        for (const Occupant& occupant : people)
        {
            if (occupant.mSelf)
            {
                agent_cut = occupant.mCut;
            }
        }
        // A roof someone stands on is kept up to their level, unless the agent is under it or it floats over the agent.
        std::set<U32> kept;
        for (const auto& roof : roof_of)
        {
            const Extent& roofed = extents.at(roof.first);
            if (agent_under.count(roof.first) || (!roofed.mGround && roofed.mBottom > agent_cut + OVERHEAD_M))
            {
                continue;
            }
            kept.insert(roof.first);
            auto it = cut_of.find(roof.first);
            if (it != cut_of.end() && it->second < roof.second)
            {
                it->second = roof.second;
                why[roof.first] = fixedReason("kept for someone on its roof");
            }
        }
        if (holding)
        {
            mCutKey = 0; // worked again until the hold runs out
        }
        // A floating cluster (a skybox) wholly above the agent's level would roof the view of the ground: hidden, unless
        // someone in it already had it cut. The agent's own skybox is the agent's building, cut as any other.
        for (const U32 id : mFloating)
        {
            const Extent& e = extents.at(id);
            if (e.mBottom > agent_cut && !cut_of.count(id) && !kept.count(id))
            {
                cut_of[id] = skoverview::HIDDEN;
                why[id] = fixedReason("hidden floating");
            }
        }
        // A cut or hidden linkset is uncut or shown only once that has held a moment: one evaluation's dropout no longer
        // flashes it.
        static LLCachedControl<F32> release_s(gSavedSettings, "SKOverviewCutReleaseSeconds", 0.5f);
        // Only the cut linksets want anything; a root with no linkset left is gone.
        std::map<LLUUID, F32> wanted;
        for (const auto& cut : cut_of)
        {
            const auto w = wanted.emplace(extents.at(cut.first).mRoot, cut.second);
            w.first->second = llmin(w.first->second, cut.second);
        }
        bool releasing = false;
        for (auto it = mRelease.begin(); it != mRelease.end();)
        {
            const bool present = mRootExtent.count(it->first) > 0;
            const auto w = wanted.find(it->first);
            Release& r = it->second;
            const F32 v = w == wanted.end() ? skoverview::NO_CUT : w->second;
            const bool relaxes = (r.mCut == skoverview::HIDDEN && v != skoverview::HIDDEN) || v == skoverview::NO_CUT;
            if (!present || !relaxes)
            {
                r.mSince = -1.0;
            }
            else if (r.mSince < 0.0)
            {
                r.mSince = now;
            }
            if (!present || !relaxes || now - r.mSince >= release_s)
            {
                if (v == skoverview::NO_CUT)
                {
                    it = mRelease.erase(it);
                    continue;
                }
                r.mCut = v;
                r.mSince = -1.0;
            }
            else
            {
                releasing = true;
            }
            ++it;
        }
        for (const auto& w : wanted)
        {
            if (w.second != skoverview::NO_CUT && !mRelease.count(w.first))
            {
                mRelease[w.first].mCut = w.second;
            }
        }
        for (auto& r : mRelease)
        {
            const auto found = mRootExtent.find(r.first);
            if (found == mRootExtent.end())
            {
                continue;
            }
            const U32 id = found->second;
            if (r.second.mSince >= 0.0)
            {
                cut_of[id] = r.second.mCut;
                Reason held = r.second.mWhy;
                held.mReleasing = true;
                why[id] = std::move(held);
            }
            else if (cut_of.count(id))
            {
                r.second.mWhy = why[id];
            }
        }
        if (releasing)
        {
            mCutKey = 0; // worked again until the release holds
        }
    }
    std::vector<std::pair<U32, F32>> cuts(cut_of.begin(), cut_of.end());
    if (cuts == mCuts && mCutSignature == mSignature)
    {
        return;
    }
    {
        LL_PROFILE_ZONE_NAMED("sk cut write");
        mCuts = std::move(cuts);
        mCutSignature = mSignature;
        ++mCutStamp;
        mCutRoots.clear();
        for (const auto& cut : cut_of)
        {
            mCutRoots[extents.at(cut.first).mRoot] = std::make_pair(cut.second, why[cut.first].text());
        }
        {
            // A count per reason, logged when it changes: the full list ran to hundreds of kilobytes a line zoomed out,
            // and the debug console wrapped every one of them each frame.
            std::map<std::string, U32> reasons;
            for (const auto& root : mCutRoots)
            {
                ++reasons[root.second.second];
            }
            std::string what;
            for (const auto& [reason, count] : reasons)
            {
                what += llformat("%s%u %s", what.empty() ? "" : ", ", count, reason.c_str());
            }
            if (what != mCutLogged)
            {
                mCutLogged = what;
                LL_DEBUGS("SKOverview") << "tactical cut: " << (what.empty() ? std::string("nothing cut") : what) << LL_ENDL;
            }
        }
        for (Batch& batch : mBatches)
        {
            const U32 total = batch.mStart[skoverview::NUM_CENSUS_CLASSES];
            if (!total)
            {
                continue;
            }
            std::vector<F32> values(total, skoverview::NO_CUT);
            for (const Group& group : batch.mGroups)
            {
                const auto it = cut_of.find(group.mId);
                // A part wholly under its cut is left whole, so an open mesh there (a desk with no underside) is
                // never counted as a solid the cut went through.
                if (it == cut_of.end() || (it->second != skoverview::HIDDEN && group.mMax.mdV[VZ] <= it->second))
                {
                    continue;
                }
                for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
                {
                    const U32 end = llmin(group.mStart[c] + group.mCount[c], total);
                    std::fill(values.begin() + llmin(group.mStart[c], end), values.begin() + end, it->second);
                }
            }
            // Only the span that changed is written: a cut moves a building or two, not the region.
            U32 lo = total;
            U32 hi = 0;
            for (U32 v = 0; v < total; ++v)
            {
                const F32 held = v < batch.mCutValues.size() ? batch.mCutValues[v] : skoverview::NO_CUT;
                if (values[v] != held)
                {
                    lo = llmin(lo, v);
                    hi = v + 1;
                }
            }
            if (lo >= hi)
            {
                continue;
            }
            LLStrider<F32> strider;
            if (!batch.mBuffer->getWeightStrider(strider, lo, (S32)(hi - lo)) || !strider.get())
            {
                continue;
            }
            for (U32 v = lo; v < hi; ++v)
            {
                *strider++ = values[v];
            }
            batch.mBuffer->unmapBuffer();
            batch.mCutValues = std::move(values);
        }
    }
}

bool SKOverviewCensus::raycast(const LLVector3d& origin, const LLVector3& dir, F32 max_m, LLVector3d& hit) const
{
    F32 best = max_m;
    bool found = false;
    for (const Batch& batch : mBatches)
    {
        const LLVector3 from(origin - batch.mOrigin);
        const U32 total = (U32)batch.mPositions.size();
        for (const Group& group : batch.mGroups)
        {
            // The group's bounds first, by the slab test.
            const LLVector3 lo(group.mMin - batch.mOrigin);
            const LLVector3 hi(group.mMax - batch.mOrigin);
            F32 t_lo = 0.f;
            F32 t_hi = best;
            bool miss = false;
            for (S32 i = 0; i < 3 && !miss; ++i)
            {
                if (std::fabs(dir.mV[i]) < 1e-9f)
                {
                    miss = from.mV[i] < lo.mV[i] || from.mV[i] > hi.mV[i];
                    continue;
                }
                const F32 a = (lo.mV[i] - from.mV[i]) / dir.mV[i];
                const F32 b = (hi.mV[i] - from.mV[i]) / dir.mV[i];
                t_lo = llmax(t_lo, llmin(a, b));
                t_hi = llmin(t_hi, llmax(a, b));
                miss = t_lo > t_hi;
            }
            if (miss)
            {
                continue;
            }
            F32 cut = skoverview::NO_CUT;
            std::string why;
            if (cutOf(group.mRoot, cut, why) && cut <= skoverview::HIDDEN * 0.5f)
            {
                continue;
            }
            // Cuts are agent z, which is global z; positions are region-local.
            const F32 cut_local = cut >= skoverview::NO_CUT * 0.5f ? cut : cut - (F32)batch.mOrigin.mdV[VZ];
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(group.mStart[c] + group.mCount[c], total);
                for (U32 v = group.mStart[c]; v + 2 < end; v += 3)
                {
                    if (v / 3 < batch.mInvisible.size() && (batch.mInvisible[v / 3] & skoverview::TRI_PASS_THROUGH))
                    {
                        continue;
                    }
                    // Moller-Trumbore, either face, as the view draws both.
                    const LLVector3& p0 = batch.mPositions[v];
                    const LLVector3 e1 = batch.mPositions[v + 1] - p0;
                    const LLVector3 e2 = batch.mPositions[v + 2] - p0;
                    const LLVector3 pv = dir % e2;
                    const F32 det = e1 * pv;
                    if (std::fabs(det) < 1e-12f)
                    {
                        continue;
                    }
                    const F32 inv = 1.f / det;
                    const LLVector3 tv = from - p0;
                    const F32 u = (tv * pv) * inv;
                    if (u < 0.f || u > 1.f)
                    {
                        continue;
                    }
                    const LLVector3 qv = tv % e1;
                    const F32 w = (dir * qv) * inv;
                    const F32 t = (e2 * qv) * inv;
                    if (w < 0.f || u + w > 1.f || t <= 0.f || t >= best)
                    {
                        continue;
                    }
                    // Above its part's cut nothing is drawn, so the ray goes on through it.
                    if ((from + dir * t).mV[VZ] > cut_local)
                    {
                        continue;
                    }
                    best = t;
                    found = true;
                    hit = origin + LLVector3d(dir * t);
                }
            }
        }
    }
    return found;
}

bool SKOverviewCensus::cutOf(const LLUUID& root, F32& height, std::string& why) const
{
    const auto it = mCutRoots.find(root);
    if (it == mCutRoots.end())
    {
        return false;
    }
    height = it->second.first;
    why = it->second.second;
    return true;
}
