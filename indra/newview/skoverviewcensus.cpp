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

#include "lldir.h"
#include "llframetimer.h"
#include "llimagegl.h"
#include "llmaterial.h"
#include "llrender.h"
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
        // A visible-only record is a cover candidate, never drawn: it would put every phantom decoration on the map.
        if (rec.mRole != sknav::Role::NAV || !rec.mCollides || rec.mNavRole == sknav::NavRole::MATERIAL_VOLUME ||
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
    mRegions.clear();
    mLinksetIds.clear();
    mIds.clear();
    mCache = std::make_shared<const Cache>();
    mCutTable.release();
    mLargeLogged.clear();
    mCoverLogged.clear();
    mUploadLogged.clear();
    mStamp = 0;
    mSignature = 0;
    mTriangles = 0;
}

bool SKOverviewCensus::RoomFootprint::covers(S32 x, S32 y) const
{
    return x >= 0 && y >= 0 && x < mW && y < mH && (mMask.empty() || mMask[(size_t)y * mW + x]);
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

U32 SKOverviewCensus::logChurn(std::vector<std::pair<U64, std::pair<char, LLUUID>>>& entries,
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
        return 0;
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
    U32 moved_records = 0;
    for (const auto& c : moved_to)
    {
        const char kind = c.second.first;
        moved_records += (kind == 'R' || kind == 'C') && !isRestless(c.second.second) ? 1 : 0;
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
    return moved_records;
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
        mSeenUnknown[part] = mSeenPass;
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
        mSeenUnknown[part] = mSeenPass;
        return seen;
    }
    mSeenUnknown.erase(part);
    mSeen[part] = { key, mSeenPass, seen };
    return seen;
}

U8 SKOverviewCensus::lastSeen(const LLUUID& part) const
{
    const auto it = mSeen.find(part);
    if (it != mSeen.end())
    {
        return it->second.mSeen == Seen::VISIBLE ? 0 : 1;
    }
    return mSeenUnknown.count(part) ? 2 : 255;
}

void SKOverviewCensus::pruneSeen()
{
    for (auto it = mSeen.begin(); it != mSeen.end();)
    {
        it = it->second.mPass != mSeenPass ? mSeen.erase(it) : std::next(it);
    }
    for (auto it = mSeenUnknown.begin(); it != mSeenUnknown.end();)
    {
        it = it->second != mSeenPass ? mSeenUnknown.erase(it) : std::next(it);
    }
    ++mSeenPass;
}

SKOverviewCensus::CoverInput SKOverviewCensus::coverInputFor(const LLUUID& part, const LLVector3d& lo, const LLVector3d& hi,
                                                             const SKCensusSnapshot& snap)
{
    CoverInput in;
    in.mTest = true;
    const F64 reach = sknav::InvisibleParams().mProxyTolerance;
    std::vector<U32> nearby = mCoverLarge;
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
    U64 key = 0; // summed: the records follow the object list's order, which churns
    for (const U32 index : nearby)
    {
        const sknav::Record& rec = *snap.mRecords[index];
        if (rec.mPart == part)
        {
            continue;
        }
        bool meets = true;
        for (S32 a = 0; a < 3 && meets; ++a)
        {
            meets = rec.mMax.mdV[a] + reach >= lo.mdV[a] && rec.mMin.mdV[a] - reach <= hi.mdV[a];
        }
        if (!meets)
        {
            continue;
        }
        const Seen seen = mCoverSeen[index];
        if (seen == Seen::UNKNOWN)
        {
            // Its look may yet show it: no hatch until it is known.
            in.mPending = true;
            key += skoverview::scramble(rec.mPart.getCRC32() ^ 0x7u);
        }
        else if (seen == Seen::VISIBLE)
        {
            in.mCandidates.push_back(&rec);
            key += skoverview::scramble(rec.mSignature ^ (rec.mPhysicsKnown ? 0x1u : 0x5u) ^ (rec.mCollides ? 0x0u : 0x2u));
        }
    }
    // The verdict does not depend on their order, the pieces the worker cuts do: by part, a surround after its twin.
    std::sort(in.mCandidates.begin(), in.mCandidates.end(),
              [](const sknav::Record* a, const sknav::Record* b)
              { return a->mPart < b->mPart || (a->mPart == b->mPart && a->mRole < b->mRole); });
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
            return !mRegions.empty();
        }
        std::shared_ptr<Built> built = mJob.get();
        if (built && built->mGeneration == mGeneration)
        {
            mVerdicts = built->mVerdicts;
            mCache = built->mCache;
            swapIn(*built);
        }
    }
    if (!SKWorldCensus::instanceExists())
    {
        return !mRegions.empty();
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return !mRegions.empty();
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
        // The clustered linksets take their cluster's heights as it moves; whether it stands on the land waits for the
        // next rebuild.
        std::unordered_map<LLUUID, const sknav::Cluster*> cluster_of_root;
        for (const sknav::ClusterPtr& c : snap->mClusters.mClusters)
        {
            for (const sknav::RecordPtr& rec : c->mRecords)
            {
                cluster_of_root.emplace(rec->mRoot, c.get());
            }
        }
        bool heights_moved = false;
        for (Region& region : mRegions)
        {
            bool moved = false;
            for (Group& group : region.mGroups)
            {
                const auto it = cluster_of_root.find(group.mRoot);
                if (it == cluster_of_root.end() || !mClusterRoots.count(group.mRoot))
                {
                    continue;
                }
                const F32 lo = (F32)it->second->mMin.mdV[VZ];
                const F32 hi = (F32)it->second->mMax.mdV[VZ];
                moved |= lo != group.mMinZ || hi != group.mMaxZ;
                group.mMinZ = lo;
                group.mMaxZ = hi;
            }
            if (moved)
            {
                refreshSlots(region); // the command builders read the heights from the slots
                heights_moved = true;
            }
        }
        if (heights_moved)
        {
            mExtentHeightsMoved = true;
            mCutSignature = 0; // applyCut works the cut again with the same key
        }
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
        return !mRegions.empty();
    }
    const bool announced = mRecordsChanged || !mBuilt; // a walk the census stamp did not ask for checks the stamp
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
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            const LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
            if (isSculpt(volume))
            {
                mSculptCandidates.push_back(volume->getID());
            }
        }
    }
    // What an invisible part may stand in for or add to: the census records the overview keeps, and those it keeps
    // for this test alone (visible only, surrounds), filed by where they are once a pass, with their looks.
    mCoverGrid.clear();
    mCoverLarge.clear();
    mCoverSeen.assign(snap->mRecords.size(), Seen::UNKNOWN);
    for (U32 index = 0; index < (U32)snap->mRecords.size(); ++index)
    {
        const sknav::Record* rec = snap->mRecords[index].get();
        skoverview::CensusClass fixed;
        if (!rec || (!sknav::coverPoolRecord(*rec) && !skoverview::keepRecord(*rec, fixed)))
        {
            continue;
        }
        mCoverSeen[index] = seenOf(rec->mPart);
        mUnknownLook |= mCoverSeen[index] == Seen::UNKNOWN;
        // A restless part rekeys every pass; it is drawn live and covers nothing in the static buffers.
        if (!sknav::coverPoolRecord(*rec) || isRestless(rec->mPart))
        {
            continue;
        }
        const S64 x0 = (S64)std::floor(rec->mMin.mdV[VX] / COVER_CELL_M);
        const S64 x1 = (S64)std::floor(rec->mMax.mdV[VX] / COVER_CELL_M);
        const S64 y0 = (S64)std::floor(rec->mMin.mdV[VY] / COVER_CELL_M);
        const S64 y1 = (S64)std::floor(rec->mMax.mdV[VY] / COVER_CELL_M);
        if (x1 < x0 || y1 < y0 || (x1 - x0 + 1) * (y1 - y0 + 1) > 64)
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
        sc.mKey = cached.mKey;
        const Seen seen = seenOf(sc.mPart);
        mUnknownLook |= seen == Seen::UNKNOWN;
        CoverInput cover;
        if (seen == Seen::INVISIBLE)
        {
            // Judged by the visible records near it; the worker drops it when large and not a proxy.
            const F64 reach = 0.5 * scale.magVec();
            cover = coverInputFor(sc.mPart, centre - LLVector3d(reach, reach, reach), centre + LLVector3d(reach, reach, reach),
                                  *snap);
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
    for (size_t index = 0; index < snap->mRecords.size(); ++index)
    {
        const sknav::RecordPtr& rec = snap->mRecords[index];
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
                cover = coverInputFor(rec->mPart, rec->mMin, rec->mMax, *snap);
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
        const Seen seen = mCoverSeen[index]; // asked with the cover candidates
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
            cover = coverInputFor(rec->mPart, rec->mMin, rec->mMax, *snap);
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
    // A part's linkset, never its index, which follows the records' order; its cluster's heights are refreshed on a
    // new stamp instead, so one part moving does not re-sign every other part of its cluster.
    for (size_t i = 0; i < kept.size(); ++i)
    {
        mix(kept[i]->mPart.getCRC32());
        mix(roots[groupOf(i)].getCRC32());
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
        return !mRegions.empty();
    }
    const U32 missed = logChurn(entries, moves);
    if (!announced && missed)
    {
        LL_WARNS("SKOverview") << "census full walk found " << missed << " record entries the stamp missed" << LL_ENDL;
    }

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
    job->mCache = mCache;
    job->mGroupOf = std::move(group_of);
    mClusterRoots.clear();
    for (const auto& gc : group_cluster)
    {
        mClusterRoots.insert(roots[gc.first]);
    }
    job->mRoots = std::move(roots);
    // Each root keeps its id while it stays in the builds; a root first seen takes the lowest id free.
    job->mIds.reserve(job->mRoots.size());
    for (const LLUUID& root : job->mRoots)
    {
        const auto id = mLinksetIds.find(root);
        job->mIds.push_back(id != mLinksetIds.end() ? id->second : (mLinksetIds[root] = mIds.acquire()));
    }
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
            return buildLinksets(*job);
        });
    mJob = task->get_future();
    if (!mWorkers->getQueue().tryPost([task]() { (*task)(); }))
    {
        // A refused post would drop the build, so it is made here instead, and lands the same way.
        LL_WARNS("SKOverview") << "census overview queue refused the build; building on this thread" << LL_ENDL;
        (*task)();
    }
    return !mRegions.empty();
}

U64 SKOverviewCensus::LinksetGeom::vertices() const
{
    U64 n = 0;
    for (const auto& positions : mClass)
    {
        n += positions.size();
    }
    return n;
}

// static
std::shared_ptr<SKOverviewCensus::Built> SKOverviewCensus::buildLinksets(const Job& job)
{
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
    std::vector<LLVector3> tris;
    Built::CoverStats stats;
    const auto cover_began = std::chrono::steady_clock::now();
    const sknav::InvisibleParams cover_params;
    std::vector<LLVector3> cover_scratch;
    // The verdicts this build asked for; the rest are dropped with the parts they were for.
    std::unordered_map<LLUUID, Verdict> verdicts;
    auto regionKey = [](const LLVector3d& origin)
    { return std::make_pair((S64)std::llround(origin.mdV[VX]), (S64)std::llround(origin.mdV[VY])); };
    auto originOf = [&](size_t i) -> const LLVector3d&
    { return i >= kept.size() ? sculpts[i - kept.size()].mOrigin : kept[i]->mOrigin; };
    auto partOf = [&](size_t i) -> const LLUUID& { return i >= kept.size() ? sculpts[i - kept.size()].mPart : kept[i]->mPart; };
    // An invisible part's verdict is kept while its own geometry and its candidates are: its record's signature, or
    // the sculpt triangles it shares with the cache.
    auto verdictKey = [&](size_t i, const CoverInput& cover, const std::vector<LLVector3>& source)
    {
        U64 part_key = skoverview::scramble(cover.mKey ^ (U64)source.size());
        part_key = skoverview::scramble(part_key ^ (i >= kept.size() ? (U64)(uintptr_t)&source : kept[i]->mSignature));
        if (!source.empty())
        {
            const LLVector3& first = source.front();
            part_key = skoverview::scramble(part_key ^ (U64)(S64)std::llround(first.mV[VX] * 64.f) ^
                                            ((U64)(S64)std::llround(first.mV[VY] * 64.f) << 21) ^
                                            ((U64)(S64)std::llround(first.mV[VZ] * 64.f) << 42));
        }
        return part_key;
    };
    // Which land is connected decides what of a void-only part is drawn.
    U64 regions_key = 0;
    for (const auto& square : job.mRegions)
    {
        regions_key = skoverview::scramble(regions_key ^ (U64)(S64)std::llround(square.first.mdV[VX]) ^
                                           ((U64)(S64)std::llround(square.first.mdV[VY]) << 32) ^ (U64)square.second);
    }
    auto bitsOf = [](F64 v)
    {
        U64 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };

    // Emits one part into its linkset's triangles: the triangles of each class, the hatch copies, and its counts.
    auto emitPart = [&](size_t i, LinksetGeom& geom)
    {
        const bool is_sculpt = i >= kept.size();
        const LLVector3d& origin = originOf(i);
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
        // An invisible part a visible neighbour that collides with nothing holds, or that sticks out of the colliding
        // ones by no more than a sliver, stands in for them; one sticking out further is extra, hatched where it does;
        // one with nothing visible near it is a barrier, hatched whole. While a neighbour's type or look is not known
        // yet it is drawn unhatched: a missed barrier costs less than a hatched building. Any but a proxy, when large,
        // is a shell nobody plays against.
        const std::vector<LLVector3>* hatch = nullptr;
        bool pass_through = false;
        const CoverInput* cover = i < job.mCover.size() ? &job.mCover[i] : nullptr;
        if (cover && cover->mTest)
        {
            LL_PROFILE_ZONE_NAMED("sk overview invisible cover");
            const LLUUID& part = partOf(i);
            const U64 part_key = verdictKey(i, *cover, *source);
            ++geom.mCover.mParts;
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
                const auto verdict_began = std::chrono::steady_clock::now();
                // The neighbours' shapes go into a scratch for this part alone: nothing of theirs outlives the verdict.
                const std::vector<sknav::CoverCandidate> candidates = sknav::coverCandidates(cover->mCandidates, origin, cover_scratch);
                sknav::CoverResult result = sknav::coverVerdict(*source, candidates, cover_params);
                stats.mVerdictSeconds += std::chrono::duration<F64>(std::chrono::steady_clock::now() - verdict_began).count();
                judged.mKey = part_key;
                judged.mVerdict = (U8)result.mVerdict;
                judged.mTooFine = result.mTooFine;
                judged.mOutsideM2 = result.mOutsideArea;
                judged.mPartM2 = result.mPartArea;
                if (!result.mProtruding.empty())
                {
                    judged.mProtruding = std::make_shared<const std::vector<LLVector3>>(std::move(result.mProtruding));
                }
                LL_DEBUGS("SKOverviewCover") << part << " " << sknav::coverVerdictName(result.mVerdict) << " "
                                             << result.mOutsideArea << " " << result.mPartArea << " "
                                             << (result.mSurfaceClip ? 1 : 0) << LL_ENDL;
            }
            verdicts[part] = judged;
            geom.mJudged.emplace_back(part, part_key);
            sknav::CoverVerdict verdict = (sknav::CoverVerdict)judged.mVerdict;
            // A neighbour still showing its look may yet hold the part: a hatch waits for it.
            if (cover->mPending && (verdict == sknav::CoverVerdict::EXTRA || verdict == sknav::CoverVerdict::BARRIER))
            {
                verdict = sknav::CoverVerdict::UNKNOWN_PHYSICS;
            }
            if (verdict != sknav::CoverVerdict::PROXY)
            {
                LLVector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
                for (const LLVector3& v : *source)
                {
                    update_min_max(lo, hi, v);
                }
                const LLVector3 size = hi - lo;
                if (llmax(size.mV[VX], llmax(size.mV[VY], size.mV[VZ])) > skoverview::INVISIBLE_WALL_M)
                {
                    ++geom.mCover.mDropped;
                    return;
                }
                pass_through = true;
            }
            switch (verdict)
            {
                case sknav::CoverVerdict::PROXY: ++geom.mCover.mProxies; break;
                case sknav::CoverVerdict::EXTRA:
                    ++geom.mCover.mExtra;
                    geom.mCover.mHatchedM2 += judged.mOutsideM2;
                    hatch = judged.mProtruding.get();
                    break;
                case sknav::CoverVerdict::BARRIER:
                    ++geom.mCover.mBarriers;
                    geom.mCover.mHatchedM2 += judged.mPartM2;
                    hatch = source;
                    break;
                case sknav::CoverVerdict::UNKNOWN_PHYSICS:
                    ++geom.mCover.mUnknown;
                    geom.mCover.mTooFine += judged.mTooFine ? 1 : 0;
                    break;
            }
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
        U64 emitted = 0;
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
            std::vector<LLVector3>& out = geom.mClass[cls];
            for (const LLVector3* v : { &a, &b, &c })
            {
                update_min_max(geom.mMin, geom.mMax, *v);
                out.push_back(*v);
            }
            geom.mFlags[cls].push_back(tri_flags);
            ++emitted;
        }
        // What is hatched again in the invisible range, the capture's overlay pass: the barrier's every triangle, or
        // the extra part's pieces that stick out.
        for (size_t t = 0; hatch && t + 2 < hatch->size(); t += 3)
        {
            const LLVector3& a = (*hatch)[t];
            const LLVector3& b = (*hatch)[t + 1];
            const LLVector3& c = (*hatch)[t + 2];
            if (!a.isFinite() || !b.isFinite() || !c.isFinite() || ((b - a) % (c - a)).length() <= 1.0e-8f ||
                (in_void_only && overLand(origin + LLVector3d((a + b + c) / 3.f))))
            {
                continue;
            }
            std::vector<LLVector3>& over = geom.mClass[skoverview::INVISIBLE];
            for (const LLVector3* v : { &a, &b, &c })
            {
                update_min_max(geom.mMin, geom.mMax, *v);
                over.push_back(*v);
            }
            geom.mFlags[skoverview::INVISIBLE].push_back(tri_flags | skoverview::TRI_PASS_THROUGH);
            ++geom.mHatchTris;
        }
        U64& from = !is_sculpt ? geom.mRecordTris : sculpts[i - kept.size()].mSurround ? geom.mSurroundTris : geom.mSculptTris;
        from += emitted;
    };

    // Each linkset's parts in each region, in an order that does not follow the records', which churns.
    std::map<std::pair<U32, std::pair<S64, S64>>, std::vector<size_t>> linksets;
    for (size_t i = 0; i < kept.size() + sculpts.size(); ++i)
    {
        linksets[{ job.mGroupOf[i], regionKey(originOf(i)) }].push_back(i);
    }
    auto cache = std::make_shared<Cache>();
    std::vector<std::pair<U32, std::shared_ptr<const LinksetGeom>>> geoms; // by group
    geoms.reserve(linksets.size());
    U32 emitted_linksets = 0;
    {
        LL_PROFILE_ZONE_NAMED("sk census cache");
        for (auto& [where, parts] : linksets)
        {
            std::sort(parts.begin(), parts.end(),
                      [&](size_t a, size_t b)
                      {
                          const bool sa = a >= kept.size(), sb = b >= kept.size();
                          if (sa != sb || partOf(a) != partOf(b))
                          {
                              return sa != sb ? sb : partOf(a) < partOf(b);
                          }
                          if (sa)
                          {
                              return a < b;
                          }
                          const sknav::Record& ra = *kept[a];
                          const sknav::Record& rb = *kept[b];
                          return ra.mRole < rb.mRole || (ra.mRole == rb.mRole && ra.mSignature < rb.mSignature);
                      });
            // Everything the emitted triangles depend on, part by part: equal keys emit equal triangles.
            U64 key = skoverview::scramble(where.second.first ^ ((U64)where.second.second << 32));
            for (const size_t i : parts)
            {
                key = skoverview::scramble(key ^ partOf(i).getCRC32() ^ ((U64)(i >= kept.size()) << 40));
                key = skoverview::scramble(key ^ bitsOf(originOf(i).mdV[VX]) ^ skoverview::scramble(bitsOf(originOf(i).mdV[VY])));
                if (i >= kept.size())
                {
                    const RenderedPart& sc = sculpts[i - kept.size()];
                    key = skoverview::scramble(key ^ sc.mKey ^ ((U64)sc.mFixed << 48) ^ ((U64)sc.mSurround << 56));
                }
                else
                {
                    const sknav::Record& rec = *kept[i];
                    key = skoverview::scramble(key ^ rec.mSignature ^ ((U64)job.mFixed[i] << 48) ^ ((U64)rec.mRole << 56));
                    if (job.mVoidOnly[i])
                    {
                        key = skoverview::scramble(key ^ regions_key ^ 0x766f6964ull);
                    }
                }
                const CoverInput* cover = i < job.mCover.size() ? &job.mCover[i] : nullptr;
                if (cover && cover->mTest)
                {
                    // The verdict as emitted: its key and whether a pending neighbour holds the hatch back.
                    const std::vector<LLVector3>* source = &tris;
                    if (i >= kept.size())
                    {
                        source = sculpts[i - kept.size()].mTris.get();
                    }
                    else
                    {
                        tris.clear();
                        sknav::emitRecord(*kept[i], kept[i]->mOrigin, tris);
                    }
                    key = skoverview::scramble(key ^ verdictKey(i, *cover, *source) ^ (cover->mPending ? 0x9e11u : 0x0u));
                }
            }
            const LLUUID root = where.first < job.mRoots.size() ? job.mRoots[where.first] : LLUUID::null;
            const auto at = std::make_tuple(root, where.second.first, where.second.second);
            std::shared_ptr<const LinksetGeom> geom;
            if (job.mCache)
            {
                const auto it = job.mCache->mByLinkset.find(at);
                if (it != job.mCache->mByLinkset.end() && it->second->mKey == key)
                {
                    // Kept whole, its verdicts with it; one its parts' verdicts no longer hold is emitted again.
                    bool held = true;
                    for (const auto& judged : it->second->mJudged)
                    {
                        const auto v = job.mVerdicts ? job.mVerdicts->mByPart.find(judged.first)
                                                     : std::unordered_map<LLUUID, Verdict>::const_iterator();
                        held = job.mVerdicts && v != job.mVerdicts->mByPart.end() && v->second.mKey == judged.second;
                        if (!held)
                        {
                            break;
                        }
                        verdicts[judged.first] = v->second;
                    }
                    geom = held ? it->second : nullptr;
                }
            }
            if (!geom)
            {
                auto fresh = std::make_shared<LinksetGeom>();
                fresh->mKey = key;
                fresh->mRoot = root;
                fresh->mOrigin = originOf(parts.front());
                for (const size_t i : parts)
                {
                    emitPart(i, *fresh);
                }
                geom = std::move(fresh);
                ++emitted_linksets;
            }
            cache->mByLinkset[at] = geom;
            geoms.emplace_back(where.first, geom);
        }
    }
    stats.mBuildSeconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - cover_began).count();

    // The build's counts, from its linksets whether kept or emitted.
    Built::Sources sources;
    std::unordered_map<LLUUID, U64> root_tris;
    for (const auto& [group, geom] : geoms)
    {
        sources.mRecords += geom->mRecordTris;
        sources.mSculpts += geom->mSculptTris;
        sources.mSurrounds += geom->mSurroundTris;
        sources.mHatch += geom->mHatchTris;
        const U64 n = geom->mRecordTris + geom->mSculptTris + geom->mSurroundTris + geom->mHatchTris;
        if (n)
        {
            root_tris[geom->mRoot] += n;
        }
        const LinksetGeom::Cover& c = geom->mCover;
        stats.mParts += c.mParts;
        stats.mProxies += c.mProxies;
        stats.mExtra += c.mExtra;
        stats.mBarriers += c.mBarriers;
        stats.mUnknown += c.mUnknown;
        stats.mTooFine += c.mTooFine;
        stats.mDropped += c.mDropped;
        stats.mHatchedM2 += c.mHatchedM2;
    }
    sources.mLargest.assign(root_tris.begin(), root_tris.end());
    const size_t largest = llmin(sources.mLargest.size(), (size_t)10);
    std::partial_sort(sources.mLargest.begin(), sources.mLargest.begin() + largest, sources.mLargest.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    sources.mLargest.resize(largest);

    auto built = std::make_shared<Built>();
    built->mVerdicts = std::make_shared<const Verdicts>(Verdicts{ std::move(verdicts) });
    built->mCache = std::move(cache);
    built->mLinksetsEmitted = emitted_linksets;
    built->mSignature = job.mSignature;
    built->mGeneration = job.mGeneration;
    built->mCover = stats;
    built->mSources = std::move(sources);
    built->mRoots = job.mRoots;
    // Each linkset in each region it has triangles in, with its cluster's heights and the box its land is read over.
    built->mLinksets.reserve(geoms.size());
    for (const auto& [group, geom] : geoms)
    {
        const U64 vertices = geom->vertices();
        if (!vertices)
        {
            continue;
        }
        bool fits = true;
        for (const auto& positions : geom->mClass)
        {
            fits = fits && positions.size() <= skoverview::MAX_PAGE_VERTICES;
        }
        if (!fits)
        {
            built->mOverflow.emplace_back(geom->mRoot, vertices);
            continue;
        }
        Built::Linkset linkset;
        Group& g = linkset.mGroup;
        g.mMinZ = geom->mMin.mV[VZ];
        g.mMaxZ = geom->mMax.mV[VZ];
        g.mMin = geom->mOrigin + LLVector3d(geom->mMin);
        g.mMax = geom->mOrigin + LLVector3d(geom->mMax);
        linkset.mFootprint = { g.mMin, g.mMax };
        const auto cluster = job.mGroupCluster.find(group);
        if (cluster != job.mGroupCluster.end())
        {
            const sknav::Cluster& c = *job.mSnapshot->mClusters.mClusters[cluster->second];
            g.mMinZ = (F32)c.mMin.mdV[VZ];
            g.mMaxZ = (F32)c.mMax.mdV[VZ];
            linkset.mFootprint = { c.mMin, c.mMax };
        }
        g.mId = group < job.mIds.size() ? job.mIds[group] : 0;
        g.mRoot = geom->mRoot;
        if (group < job.mAnchors.size())
        {
            g.mAnchor = job.mAnchors[group].first;
            g.mNeverCut = job.mAnchors[group].second;
        }
        g.mGeom = geom;
        built->mLinksets.push_back(std::move(linkset));
    }
    return built;
}

namespace
{
    static_assert(SKOverviewCutTable::WHOLE == skoverview::NO_CUT && SKOverviewCutTable::HIDDEN == skoverview::HIDDEN,
                  "the cut table and the census share their sentinels");

    /// Writes one linkset's run of one class into a page: positions, its id in the weight channel, which the cut table
    /// is read through, an open part's flag in texcoord0.x and a passed-through one's in texcoord0.y.
    bool uploadRun(LLVertexBuffer& buffer, U32 first, const std::vector<LLVector3>& positions, const std::vector<U8>& flags,
                   U32 id)
    {
        const U32 count = (U32)positions.size();
        LLStrider<LLVector3> verts;
        if (!buffer.getVertexStrider(verts, first, (S32)count) || !verts.get())
        {
            return false;
        }
        for (const LLVector3& p : positions)
        {
            *verts++ = p;
        }
        LLStrider<F32> ids;
        if (buffer.getWeightStrider(ids, first, (S32)count) && ids.get())
        {
            for (U32 v = 0; v < count; ++v)
            {
                ids[v] = (F32)id;
            }
        }
        LLStrider<LLVector2> open;
        if (buffer.getTexCoord0Strider(open, first, (S32)count) && open.get())
        {
            for (U32 v = 0; v < count; ++v)
            {
                const U8 flag = v / 3 < flags.size() ? flags[v / 3] : 0;
                const bool is_open = flag & skoverview::TRI_OPEN;
                const bool pass_through = flag & skoverview::TRI_PASS_THROUGH;
                *open++ = LLVector2(is_open ? 1.f : 0.f, pass_through ? 1.f : 0.f);
            }
        }
        return true;
    }

    std::pair<S64, S64> regionKeyOf(const LLVector3d& origin)
    {
        return { (S64)std::llround(origin.mdV[VX]), (S64)std::llround(origin.mdV[VY]) };
    }
}

SKOverviewCensus::Group::Group()
{
    std::fill(std::begin(mPage), std::end(mPage), NO_PAGE);
}

U32 SKOverviewCensus::pageFor(Region& region, U8 cls, U32 count, U32 want, U32& first)
{
    for (U32 p = 0; p < (U32)region.mPages.size(); ++p)
    {
        Page& page = region.mPages[p];
        if (page.mClass == cls && page.mBuffer && (first = page.mRuns.allocate(count)) != skoverview::SlotAllocator::NONE)
        {
            return p;
        }
    }
    // None has room: a new page for what this build still has to place, up to the setting's size, or of the run's own
    // size when it is larger; a page of the full size for every region's every class would mostly hold nothing.
    static LLCachedControl<U32> page_vertices(gSavedSettings, "SKOverviewPageVertices", 3u * 1024u * 1024u);
    const U32 most = llclamp((U32)page_vertices, MIN_PAGE_VERTICES, skoverview::MAX_PAGE_VERTICES);
    const U64 wanted = (U64)want + want / 4;
    U32 capacity = (U32)llclamp(wanted, (U64)MIN_PAGE_VERTICES, (U64)most);
    capacity = llmax(capacity - capacity % 3, count);
    Page page;
    page.mClass = cls;
    page.mBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_WEIGHT | LLVertexBuffer::MAP_TEXCOORD0);
    if (!page.mBuffer->allocateBuffer(capacity, 0))
    {
        LL_WARNS("SKOverview") << "census page of " << capacity << " vertices could not be allocated" << LL_ENDL;
        first = skoverview::SlotAllocator::NONE;
        return Group::NO_PAGE;
    }
    page.mRuns = skoverview::SlotAllocator(capacity);
    first = page.mRuns.allocate(count);
    region.mPages.push_back(std::move(page));
    ++mPagesMade;
    return (U32)region.mPages.size() - 1;
}

void SKOverviewCensus::refreshSlots(Region& region)
{
    std::vector<std::vector<skoverview::SlotCull>> before(region.mPages.size());
    for (size_t p = 0; p < region.mPages.size(); ++p)
    {
        before[p].swap(region.mPages[p].mSlots);
    }
    for (const Group& group : region.mGroups)
    {
        skoverview::SlotCull slot;
        slot.mGroup = group.mId;
        slot.mGround = group.mGround ? 1 : 0;
        const LLVector3 lo(group.mMin - region.mOrigin);
        const LLVector3 hi(group.mMax - region.mOrigin);
        for (S32 a = 0; a < 3; ++a)
        {
            slot.mLo[a] = lo.mV[a];
            slot.mHi[a] = hi.mV[a];
        }
        slot.mLo[3] = group.mMinZ;
        slot.mHi[3] = group.mMaxZ;
        for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
        {
            if (group.mPage[c] < region.mPages.size() && group.mCount[c])
            {
                slot.mFirst = group.mStart[c];
                slot.mCount = group.mCount[c];
                region.mPages[group.mPage[c]].mSlots.push_back(slot);
            }
        }
    }
    for (size_t p = 0; p < region.mPages.size(); ++p)
    {
        Page& page = region.mPages[p];
        std::sort(page.mSlots.begin(), page.mSlots.end(),
                  [](const skoverview::SlotCull& a, const skoverview::SlotCull& b) { return a.mFirst < b.mFirst; });
        // The GPU's copy is written again only when the slots moved.
        page.mTableDirty |= page.mSlots.size() != before[p].size() ||
                            (!page.mSlots.empty() &&
                             std::memcmp(page.mSlots.data(), before[p].data(), page.mSlots.size() * sizeof(skoverview::SlotCull)) != 0);
    }
}

void SKOverviewCensus::swapIn(Built& built)
{
    LL_PROFILE_ZONE_SCOPED;
    mPagesMade = 0;
    mPagesFreed = 0;
    // A root no build holds any longer gives its id back; the next new root takes the lowest free one.
    {
        std::unordered_set<LLUUID> live(built.mRoots.begin(), built.mRoots.end());
        for (auto it = mLinksetIds.begin(); it != mLinksetIds.end();)
        {
            if (live.count(it->first))
            {
                ++it;
                continue;
            }
            mIds.release(it->second);
            it = mLinksetIds.erase(it);
        }
    }
    // Warned once per set of overflowing linksets, not on every rebuild that still holds them.
    if (!built.mOverflow.empty() && built.mOverflow != mOverflowLogged)
    {
        std::string largest;
        for (const auto& l : built.mSources.mLargest)
        {
            largest += llformat("%s%s %llu", largest.empty() ? "" : ", ", l.first.asString().c_str(), (unsigned long long)l.second);
        }
        for (const auto& over : built.mOverflow)
        {
            LL_WARNS("SKOverview") << "census linkset " << over.first << " " << over.second << " vertices exceeds MAX_PAGE_VERTICES ("
                                   << skoverview::MAX_PAGE_VERTICES << "); ten largest: " << largest << LL_ENDL;
        }
    }
    mOverflowLogged = built.mOverflow;

    // Every region of a build lands in the same frame, as one census; a linkset whose triangles did not move keeps its
    // runs, and only the others are written.
    std::map<std::pair<S64, S64>, Region> landed;
    for (Region& region : mRegions)
    {
        landed.emplace(regionKeyOf(region.mOrigin), std::move(region));
    }
    mRegions.clear();
    std::map<std::pair<S64, S64>, std::vector<Built::Linkset*>> incoming;
    for (Built::Linkset& linkset : built.mLinksets)
    {
        incoming[regionKeyOf(linkset.mGroup.mGeom->mOrigin)].push_back(&linkset);
    }
    U32 changed = 0;
    U64 uploaded = 0;
    U64 total = 0;
    U32 triangles = 0;
    std::vector<Region> regions;
    regions.reserve(incoming.size());
    for (auto& [key, linksets] : incoming)
    {
        Region region;
        const auto was = landed.find(key);
        if (was != landed.end())
        {
            region = std::move(was->second);
            landed.erase(was);
        }
        region.mOrigin = linksets.front()->mGroup.mGeom->mOrigin;
        std::vector<Group> before = std::move(region.mGroups);
        region.mGroups.clear();
        std::unordered_map<LLUUID, size_t> before_of;
        for (size_t g = 0; g < before.size(); ++g)
        {
            before_of[before[g].mRoot] = g;
        }
        std::vector<bool> kept(before.size(), false);
        std::vector<bool> fresh;
        region.mGroups.reserve(linksets.size());
        for (Built::Linkset* linkset : linksets)
        {
            Group group = std::move(linkset->mGroup);
            const auto it = before_of.find(group.mRoot);
            bool same = it != before_of.end() && before[it->second].mGeom == group.mGeom && before[it->second].mId == group.mId;
            for (U32 c = 0; same && c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                // A run a full page refused last time is placed again.
                same = group.mGeom->mClass[c].empty() || before[it->second].mPage[c] != Group::NO_PAGE;
            }
            if (same)
            {
                const Group& old = before[it->second];
                std::copy(std::begin(old.mPage), std::end(old.mPage), std::begin(group.mPage));
                std::copy(std::begin(old.mStart), std::end(old.mStart), std::begin(group.mStart));
                std::copy(std::begin(old.mCount), std::end(old.mCount), std::begin(group.mCount));
                kept[it->second] = true;
            }
            // On the ground when its cluster's base is no higher above the highest land under its footprint than a step or two.
            group.mGround = group.mMinZ <= SKWorldCensus::landUnder(linkset->mFootprint.first, linkset->mFootprint.second) +
                                               skoverview::GROUND_BASE_M;
            region.mGroups.push_back(std::move(group));
            fresh.push_back(!same);
        }
        // The runs of linksets gone or changed are free before the changed ones take theirs.
        for (size_t g = 0; g < before.size(); ++g)
        {
            for (U32 c = 0; !kept[g] && c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (before[g].mPage[c] < region.mPages.size())
                {
                    region.mPages[before[g].mPage[c]].mRuns.release(before[g].mStart[c], before[g].mCount[c]);
                }
            }
        }
        U32 want[skoverview::NUM_CENSUS_CLASSES] = {};
        for (size_t g = 0; g < region.mGroups.size(); ++g)
        {
            for (U32 c = 0; fresh[g] && c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                want[c] += (U32)region.mGroups[g].mGeom->mClass[c].size();
            }
        }
        {
            LL_PROFILE_ZONE_NAMED("sk census slot upload");
            std::set<U32> written;
            for (size_t g = 0; g < region.mGroups.size(); ++g)
            {
                if (!fresh[g])
                {
                    continue;
                }
                ++changed;
                Group& group = region.mGroups[g];
                for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
                {
                    const std::vector<LLVector3>& positions = group.mGeom->mClass[c];
                    const U32 count = (U32)positions.size();
                    group.mPage[c] = Group::NO_PAGE;
                    group.mStart[c] = 0;
                    group.mCount[c] = 0;
                    if (!count)
                    {
                        continue;
                    }
                    U32 first = 0;
                    const U32 p = pageFor(region, (U8)c, count, want[c], first);
                    want[c] -= count;
                    if (p == Group::NO_PAGE || first == skoverview::SlotAllocator::NONE)
                    {
                        continue;
                    }
                    if (!uploadRun(*region.mPages[p].mBuffer, first, positions, group.mGeom->mFlags[c], group.mId))
                    {
                        LL_WARNS("SKOverview") << "census page run of " << count << " vertices could not be mapped" << LL_ENDL;
                        region.mPages[p].mRuns.release(first, count);
                        continue;
                    }
                    group.mPage[c] = p;
                    group.mStart[c] = first;
                    group.mCount[c] = count;
                    written.insert(p);
                    uploaded += count;
                }
            }
            for (const U32 p : written)
            {
                region.mPages[p].mBuffer->unmapBuffer();
            }
        }
        // Empty pages are freed, the rest kept in class order, as the classes are drawn.
        std::vector<U32> order;
        for (U32 p = 0; p < (U32)region.mPages.size(); ++p)
        {
            if (region.mPages[p].mRuns.empty())
            {
                ++mPagesFreed;
            }
            else
            {
                order.push_back(p);
            }
        }
        std::stable_sort(order.begin(), order.end(),
                         [&region](U32 a, U32 b) { return region.mPages[a].mClass < region.mPages[b].mClass; });
        std::vector<U32> moved(region.mPages.size(), Group::NO_PAGE);
        std::vector<Page> pages;
        pages.reserve(order.size());
        for (const U32 p : order)
        {
            moved[p] = (U32)pages.size();
            pages.push_back(std::move(region.mPages[p]));
        }
        region.mPages = std::move(pages);
        for (Group& group : region.mGroups)
        {
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                group.mPage[c] = group.mPage[c] < moved.size() ? moved[group.mPage[c]] : Group::NO_PAGE;
            }
        }
        refreshSlots(region);
        for (const Page& page : region.mPages)
        {
            total += page.mRuns.used();
            triangles += page.mRuns.used() / 3;
        }
        regions.push_back(std::move(region));
    }
    for (const auto& gone : landed)
    {
        mPagesFreed += (U32)gone.second.mPages.size();
    }
    landed.clear();
    mRegions = std::move(regions);
    mTriangles = triangles;
    // One row per linkset id in use, every region's, drawn whole until applyCut writes the cuts; a free id's row is
    // a hole no vertex reads.
    mCutTable.resize(mIds.bound());
    mSignature = built.mSignature;
    mBuilt = true;
    // applyCut's extents point into the groups just replaced.
    mExtentsBuilt = false;
    mOccupantCuts.clear();
    mCuts.clear();
    mCutSignature = 0;
    ++mCutStamp;
    {
        // Where the triangles come from: logged when the totals move, so a region's millions can be told apart.
        const Built::Sources& s = built.mSources;
        const std::string totals = llformat("%u regions, %u triangles (records %llu, sculpts %llu, surrounds %llu, hatch copies %llu)",
                                            (U32)mRegions.size(), mTriangles, (unsigned long long)s.mRecords,
                                            (unsigned long long)s.mSculpts, (unsigned long long)s.mSurrounds,
                                            (unsigned long long)s.mHatch);
        std::string largest;
        for (const auto& l : s.mLargest)
        {
            largest += llformat("%s%s %llu", largest.empty() ? "" : ", ", l.first.asString().c_str(), (unsigned long long)l.second);
        }
        if (totals != mUploadLogged)
        {
            mUploadLogged = totals;
            LL_INFOS("SKOverview") << "census upload: " << totals << "; ten largest linksets: " << largest << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("SKOverview") << "census upload: " << totals << "; ten largest linksets: " << largest << LL_ENDL;
        }
        LL_INFOS("SKOverview") << "census upload: " << changed << " linksets (" << built.mLinksetsEmitted << " emitted), "
                               << uploaded << " vertices sub-uploaded of " << total << LL_ENDL;
        LL_PROFILE_PLOT("census vertices uploaded", (int64_t)uploaded);
        LL_PROFILE_PLOT("census vertices total", (int64_t)total);
        LL_PROFILE_PLOT("census linksets emitted", (int64_t)built.mLinksetsEmitted);
    }
    {
        // Each page's fill: told when a page was made or freed, kept to the debug log otherwise.
        const bool news = mPagesMade || mPagesFreed;
        size_t pages = 0, slots = 0;
        U64 page_used = 0, page_capacity = 0;
        for (const Region& region : mRegions)
        {
            for (size_t p = 0; p < region.mPages.size(); ++p)
            {
                const Page& page = region.mPages[p];
                const std::string line = llformat("census pages: %.0f,%.0f %u page %u: %u/%u vertices, %u linksets, %u free",
                                                  region.mOrigin.mdV[VX], region.mOrigin.mdV[VY], (U32)page.mClass, (U32)p,
                                                  page.mRuns.used(), page.mRuns.capacity(), (U32)page.mSlots.size(),
                                                  (U32)page.mRuns.freeRuns().size());
                if (news)
                {
                    LL_INFOS("SKOverview") << line << LL_ENDL;
                }
                else
                {
                    LL_DEBUGS("SKOverview") << line << LL_ENDL;
                }
                ++pages;
                slots += page.mSlots.size();
                page_used += page.mRuns.used();
                page_capacity += page.mRuns.capacity();
            }
        }
        const std::string summary = llformat("census pages: %u regions, %u pages (%u made, %u freed), %u slots; ids %u in use "
                                             "below %u, %u free",
                                             (U32)mRegions.size(), (U32)pages, mPagesMade, mPagesFreed, (U32)slots,
                                             (U32)mLinksetIds.size(), mIds.bound(), (U32)mIds.freeCount());
        static const bool fill_configured = []
        {
            LL_PROFILE_PLOT_CONFIG("census page fill %", tracy::PlotFormatType::Percentage, false, true, 0);
            return true;
        }();
        (void)fill_configured;
        LL_PROFILE_PLOT("census pages", (int64_t)pages);
        LL_PROFILE_PLOT("census page fill %", page_capacity ? 100.0 * (F64)page_used / (F64)page_capacity : 0.0);
        if (news)
        {
            LL_INFOS("SKOverview") << summary << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("SKOverview") << summary << LL_ENDL;
        }
    }
    const Built::CoverStats& c = built.mCover;
    LL_PROFILE_PLOT("cover unknown", (int64_t)c.mUnknown);
    LL_PROFILE_PLOT("cover judged", (int64_t)c.mJudged);
    LL_PROFILE_PLOT("cover proxies", (int64_t)c.mProxies);
    LL_PROFILE_PLOT("cover verdict ms", c.mVerdictSeconds * 1000.0);
    if (c.mParts)
    {
        // Logged only when the counts move: the same parts judged the same way are no news.
        const std::string counts = llformat("%u invisible parts: %u proxies, %u extra (%.0f m2 hatched), %u barriers, "
                                            "%u not known yet (%u too fine), %u large dropped",
                                            c.mParts, c.mProxies, c.mExtra, c.mHatchedM2, c.mBarriers, c.mUnknown,
                                            c.mTooFine, c.mDropped);
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
    for (const Region& region : mRegions)
    {
        for (const Group& group : region.mGroups)
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
        if (rec && rec->mCollides) // a visible-only record is a box, no shape
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
        part.mSurround = true;
        const LLVector3d centre = volume->getPositionGlobal();
        U64 h = 1469598103934665603ull;
        h = (h ^ part.mPart.getCRC32()) * 1099511628211ull;
        h = (h ^ (U64)part.mTris->size()) * 1099511628211ull;
        h = (h ^ ((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
                  ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42))) * 1099511628211ull;
        part.mKey = skoverview::scramble(h);
        sig += part.mKey;
        mSurrounds.push_back(std::move(part));
    }
    mSurroundsSig = sig;
}

std::string SKOverviewCensus::Reason::text() const
{
    std::string out = mFixed ? std::string(mFixed)
                             : (mSelf ? std::string("agent") : "occupant " + mWho.asString()) + " " + mRule;
    return mReleasing ? out + " (releasing)" : out;
}

void SKOverviewCensus::applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, U64 key,
                                SKOverviewProbes* probes)
{
    LL_PROFILE_ZONE_SCOPED;
    if (key == mCutKey && mCutSignature == mSignature)
    {
        return;
    }
    mCutKey = key;
    std::vector<std::pair<F64, F64>> moved_bottoms; // a floating linkset's base before and after a heights refresh
    if (!mExtentsBuilt || mExtentsSignature != mSignature)
    {
        LL_PROFILE_ZONE_NAMED("sk cut extents");
        // A linkset spans regions when it crosses a border: its bounds over all of them, and where its triangles are.
        mExtents.clear();
        mOccupantCuts.clear();
        for (const Region& region : mRegions)
        {
            for (const Group& group : region.mGroups)
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
                e.mParts.emplace_back(&region, &group);
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
    else if (mExtentHeightsMoved)
    {
        // Only a base moved: the triangles a person's share was found from did not.
        for (auto& entry : mExtents)
        {
            Extent& e = entry.second;
            F64 bottom = 1e30;
            for (const auto& part : e.mParts)
            {
                bottom = llmin(bottom, (F64)part.second->mMinZ);
            }
            if (bottom != e.mBottom && !e.mGround)
            {
                moved_bottoms.emplace_back(e.mBottom, bottom);
            }
            e.mBottom = bottom;
        }
    }
    mExtentHeightsMoved = false;
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
    // Whether a moved base crossed a height's overhead line, so what was found against that height may differ.
    auto baseCrossed = [&](F64 z)
    {
        for (const auto& b : moved_bottoms)
        {
            if ((b.first > z + OVERHEAD_M) != (b.second > z + OVERHEAD_M))
            {
                return true;
            }
        }
        return false;
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
    // The agent first: their probes are issued first and their cut wins. The rest are probed longest-served first, so a
    // crowd past the budget takes turns, and marked in census order, so a tie between two of them does not move.
    std::vector<bool> self;
    std::vector<U32> issued;
    for (const Occupant& occupant : people)
    {
        self.push_back(occupant.mSelf);
        const auto cut = mOccupantCuts.find(occupant.mId);
        issued.push_back(cut == mOccupantCuts.end() ? 0 : llmax(cut->second.mRoofProbe.mIssued, cut->second.mMarksProbe.mIssued));
    }
    std::vector<Occupant*> ordered;
    for (const size_t i : skoverview::probeOrder(self, issued))
    {
        ordered.push_back(&people[i]);
    }
    std::vector<const Occupant*> marking;
    for (const size_t i : skoverview::probeOrder(self, {}))
    {
        marking.push_back(&people[i]);
    }
    // The GPU's probes (SKOverviewProbes): a person's answer is asked for when their key moves and read when it lands.
    if (probes && !probes->active())
    {
        probes = nullptr;
    }
    static LLCachedControl<U32> probe_people(gSavedSettings, "SKOverviewProbeBudget", 8);
    static LLCachedControl<U32> probe_queries(gSavedSettings, "SKOverviewProbeQueries", 256);
    skoverview::ProbeBudget budget(llmax((U32)probe_people, 1u), llmax((U32)probe_queries, 1u));
    bool probing = false; // an answer is in flight: worked again each frame until it lands
    auto personOf = [&people](const Occupant& occupant) { return (U32)(&occupant - people.data()); };
    // A linkset's runs in every region it spans, from a probe's origin; its overlay class is not drawn, as the cut never
    // read it.
    auto runsOf = [](const Extent& e, const LLVector3d& origin)
    {
        std::vector<SKOverviewProbes::Run> runs;
        for (const auto& part : e.mParts)
        {
            const Region& in = *part.first;
            const Group& group = *part.second;
            const LLVector3 offset(in.mOrigin - origin);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE || group.mPage[c] == Group::NO_PAGE || !group.mCount[c])
                {
                    continue;
                }
                runs.push_back({ in.mPages[group.mPage[c]].mBuffer.get(), group.mStart[c], group.mCount[c], offset });
            }
        }
        return runs;
    };
    // Whether every query has landed, with their counts, and whether any was lost (refused or expired) and is asked again.
    auto landed = [](const std::vector<ALQueryTicket>& tickets, std::vector<U64>& counts, bool& lost)
    {
        counts.assign(tickets.size(), 0);
        bool ready = true;
        lost = false;
        for (size_t i = 0; i < tickets.size(); ++i)
        {
            const ALQueryPool::Status status = gGL.queries().result(tickets[i], counts[i]);
            ready = ready && status == ALQueryPool::Status::Ready;
            lost = lost || status == ALQueryPool::Status::Expired;
        }
        return ready;
    };
    // Room: an upward look over the room's footprint from just above its floor, the cells outside it drawn into the
    // depth, and a query per linkset near it; a wall seen edge-on counts by its edges.
    auto issueRoom = [&](OccupantCut& cache, const Occupant& occupant, U64 marks_key) -> bool
    {
        LL_PROFILE_ZONE_NAMED("sk cut probes issue");
        const RoomFootprint& room = *occupant.mRoom;
        const F64 cell_m = occupant.mRoomCellM;
        const LLVector3d& p = occupant.mFloor;
        const LLVector3d lo = room.mOrigin;
        const LLVector3d hi = lo + LLVector3d(room.mW * cell_m, room.mH * cell_m, 0.0);
        std::vector<std::pair<U32, LLUUID>> candidates;
        F64 top = p.mdV[VZ] + ABOVE_M;
        for (const auto& entry : extents)
        {
            const Extent& e = entry.second;
            if (!e.mCuttable || e.mMax.mdV[VZ] < p.mdV[VZ] + ABOVE_M || overhead(e, occupant) || e.mMax.mdV[VX] < lo.mdV[VX] ||
                e.mMin.mdV[VX] > hi.mdV[VX] || e.mMax.mdV[VY] < lo.mdV[VY] || e.mMin.mdV[VY] > hi.mdV[VY])
            {
                continue;
            }
            candidates.emplace_back(entry.first, e.mRoot);
            top = llmax(top, e.mMax.mdV[VZ]);
        }
        if (!budget.fits(personOf(occupant), (U32)candidates.size()))
        {
            return false;
        }
        // Four texels a cell, fewer for a room too wide for the target.
        F64 texels = 4.0;
        while (texels > 1.0 / 64.0 && (room.mW * texels > SKOverviewProbes::SIZE || room.mH * texels > SKOverviewProbes::SIZE))
        {
            texels *= 0.5;
        }
        SKOverviewProbes::Box box;
        box.mMaxX = (F32)(room.mW * cell_m);
        box.mMaxY = (F32)(room.mH * cell_m);
        box.mFrom = (F32)ABOVE_M;
        box.mTo = (F32)(top - p.mdV[VZ] + 1.0);
        box.mWidth = llclamp((S32)std::ceil(room.mW * texels), 1, SKOverviewProbes::SIZE);
        box.mHeight = llclamp((S32)std::ceil(room.mH * texels), 1, SKOverviewProbes::SIZE);
        // The cells outside the room, a rect per run along each row; a room with no mask is covered whole.
        std::vector<LLVector4> outs;
        for (S32 y = 0; y < room.mH && !room.mMask.empty(); ++y)
        {
            for (S32 x = 0; x < room.mW;)
            {
                if (room.covers(x, y))
                {
                    ++x;
                    continue;
                }
                S32 end = x;
                while (end < room.mW && !room.covers(end, y))
                {
                    ++end;
                }
                outs.emplace_back((F32)(x * cell_m), (F32)(y * cell_m), (F32)(end * cell_m), (F32)((y + 1) * cell_m));
                x = end;
            }
        }
        if (!probes->beginVolume(box, outs))
        {
            return false;
        }
        budget.admit(personOf(occupant), (U32)candidates.size());
        const LLVector3d origin(lo.mdV[VX], lo.mdV[VY], p.mdV[VZ]);
        cache.mMarkTickets.clear();
        for (const auto& candidate : candidates)
        {
            cache.mMarkTickets.push_back(probes->query(runsOf(extents.at(candidate.first), origin), true));
        }
        probes->endVolume();
        // The room's id render, ids coloured and the mask outlined, on its way back to a file.
        static LLCachedControl<bool> dump(gSavedSettings, "SKOverviewProbeDump", false);
        if (dump && !mProbeDump.mPending && probes->requestDump())
        {
            mProbeDump.mPending = true;
            mProbeDump.mWho = occupant.mId;
            mProbeDump.mBox = box;
            mProbeDump.mOuts = outs;
        }
        cache.mMarkCandidates = std::move(candidates);
        cache.mMarkRule = "room";
        cache.mMarksProbe.issue(marks_key, mExtentsSignature, probes->frame());
        return true;
    };
    // Column: an upward look over the slack's square round the floor point, from just above the floor.
    auto issueColumn = [&](OccupantCut& cache, const Occupant& occupant, U64 marks_key) -> bool
    {
        LL_PROFILE_ZONE_NAMED("sk cut probes issue");
        const LLVector3d& p = occupant.mFloor;
        std::vector<std::pair<U32, LLUUID>> candidates;
        F64 top = p.mdV[VZ] + ABOVE_M;
        for (const auto& entry : extents)
        {
            const Extent& e = entry.second;
            if (!e.mCuttable || e.mMax.mdV[VZ] < p.mdV[VZ] + ABOVE_M || overhead(e, occupant) ||
                p.mdV[VX] < e.mMin.mdV[VX] - SLACK_M || p.mdV[VX] > e.mMax.mdV[VX] + SLACK_M ||
                p.mdV[VY] < e.mMin.mdV[VY] - SLACK_M || p.mdV[VY] > e.mMax.mdV[VY] + SLACK_M)
            {
                continue;
            }
            candidates.emplace_back(entry.first, e.mRoot);
            top = llmax(top, e.mMax.mdV[VZ]);
        }
        if (!budget.fits(personOf(occupant), (U32)candidates.size()))
        {
            return false;
        }
        SKOverviewProbes::Box box;
        box.mMinX = box.mMinY = (F32)-SLACK_M;
        box.mMaxX = box.mMaxY = (F32)SLACK_M;
        box.mFrom = (F32)ABOVE_M;
        box.mTo = (F32)(top - p.mdV[VZ] + 1.0);
        box.mWidth = box.mHeight = 8;
        if (!probes->beginVolume(box, {}))
        {
            return false;
        }
        budget.admit(personOf(occupant), (U32)candidates.size());
        cache.mMarkTickets.clear();
        for (const auto& candidate : candidates)
        {
            cache.mMarkTickets.push_back(probes->query(runsOf(extents.at(candidate.first), p), true));
        }
        probes->endVolume();
        cache.mMarkCandidates = std::move(candidates);
        cache.mMarkRule = "column";
        cache.mMarksProbe.issue(marks_key, mExtentsSignature, probes->frame());
        return true;
    };
    // Who stands on a roof: the highest upward-facing surface under their feet, of a linkset with nothing over them.
    const F64 STAND_BELOW_M = 0.6;  // how far under the feet a surface still bears them
    const F64 STAND_ABOVE_M = 0.4;  // how far over them, for a sunk or estimated foot
    const F64 HOLD_S = 1.0;         // a roof stays kept this long after its stander leaves it, through a hop
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    // The feet: a downward look at the one point under them, through the bearing band; the texel is the highest
    // surface's linkset, its depth the surface's height.
    auto feetBox = [&]()
    {
        SKOverviewProbes::Box box;
        box.mMinX = box.mMinY = -0.01f;
        box.mMaxX = box.mMaxY = 0.01f;
        box.mFrom = (F32)STAND_ABOVE_M;
        box.mTo = (F32)-STAND_BELOW_M;
        return box;
    };
    // Cover: an upward look at the one point over the feet, from just above them, a query per linkset that may be
    // over them; a face seen edge-on does not count, as the CPU's walls never did.
    auto issueRoof = [&](OccupantCut& cache, const Occupant& occupant, U64 roof_key) -> bool
    {
        LL_PROFILE_ZONE_NAMED("sk cut probes issue");
        const LLVector3d& feet = occupant.mFeet;
        std::vector<SKOverviewProbes::Run> under;
        for (const auto& entry : extents)
        {
            const Extent& e = entry.second;
            if (!e.mCuttable || feet.mdV[VX] < e.mMin.mdV[VX] || feet.mdV[VX] > e.mMax.mdV[VX] ||
                feet.mdV[VY] < e.mMin.mdV[VY] || feet.mdV[VY] > e.mMax.mdV[VY] ||
                feet.mdV[VZ] < e.mMin.mdV[VZ] - STAND_ABOVE_M || feet.mdV[VZ] > e.mMax.mdV[VZ] + STAND_BELOW_M)
            {
                continue;
            }
            const std::vector<SKOverviewProbes::Run> runs = runsOf(e, feet);
            under.insert(under.end(), runs.begin(), runs.end());
        }
        std::vector<std::pair<U32, LLUUID>> candidates;
        F64 top = feet.mdV[VZ] + STAND_ABOVE_M;
        for (const auto& entry : extents)
        {
            const Extent& e = entry.second;
            if (feet.mdV[VX] < e.mMin.mdV[VX] || feet.mdV[VX] > e.mMax.mdV[VX] || feet.mdV[VY] < e.mMin.mdV[VY] ||
                feet.mdV[VY] > e.mMax.mdV[VY] || e.mMax.mdV[VZ] <= feet.mdV[VZ] + STAND_ABOVE_M ||
                (!e.mGround && e.mBottom > feet.mdV[VZ] + OVERHEAD_M))
            {
                continue;
            }
            candidates.emplace_back(entry.first, e.mRoot);
            top = llmax(top, e.mMax.mdV[VZ]);
        }
        if (!budget.fits(personOf(occupant), (U32)candidates.size()))
        {
            return false;
        }
        const S32 slot = probes->sample(feetBox(), under);
        if (slot < 0)
        {
            return false;
        }
        SKOverviewProbes::Box box;
        box.mMinX = box.mMinY = -0.01f;
        box.mMaxX = box.mMaxY = 0.01f;
        box.mFrom = (F32)STAND_ABOVE_M;
        box.mTo = (F32)(top - feet.mdV[VZ] + 1.0);
        if (!probes->beginVolume(box, {}))
        {
            return false;
        }
        budget.admit(personOf(occupant), (U32)candidates.size());
        cache.mFeetSlot = slot;
        cache.mFeetZ = feet.mdV[VZ];
        cache.mCoverTickets.clear();
        for (const auto& candidate : candidates)
        {
            cache.mCoverTickets.push_back(probes->query(runsOf(extents.at(candidate.first), feet), false));
        }
        probes->endVolume();
        cache.mCoverCandidates = std::move(candidates);
        cache.mRoofProbe.issue(roof_key, mExtentsSignature, probes->frame());
        return true;
    };
    std::map<U32, F32> roof_of;     // linkset -> the highest cut of those standing on its roof
    bool holding = false;
    {
        LL_PROFILE_ZONE_NAMED("sk cut roofs");
        for (Occupant* who : ordered)
        {
            Occupant& occupant = *who;
            const LLVector3d& feet = occupant.mFeet;
            OccupantCut& cache = mOccupantCuts[occupant.mId];
            cache.mSeen = true;
            U64 roof_key = 1469598103934665603ull;
            for (S32 i = 0; i < 3; ++i)
            {
                quantise(roof_key, feet.mdV[i]);
            }
            if (baseCrossed(feet.mdV[VZ]))
            {
                cache.mRoofProbe.invalidate();
            }
            if (probes)
            {
                // The highest surface under the feet within the bearing band, and whether anything covers them: a
                // surface is a roof only with nothing over the feet, of any linkset, as a storey's slab is often one
                // linkset and the storey above it another. What stood is kept until the answer lands; the texel names
                // an id, which holds only under the buffers it was drawn from.
                LL_PROFILE_ZONE_NAMED("sk cut probes collect");
                std::vector<U64> counts;
                bool lost = false;
                bool ready = landed(cache.mCoverTickets, counts, lost);
                U32 texel = 0;
                F32 depth = 0.f;
                if (cache.mRoofProbe.mPending)
                {
                    const SKOverviewProbes::Status feet_status =
                        probes->sampleResult(cache.mRoofProbe.mIssued, cache.mFeetSlot, texel, depth);
                    ready = ready && feet_status == SKOverviewProbes::Status::Ready;
                    lost = lost || feet_status == SKOverviewProbes::Status::Lost;
                }
                if (cache.mRoofProbe.settle(ready, lost, mExtentsSignature, true) == skoverview::ProbeSlot::Outcome::Apply)
                {
                    const SKOverviewProbes::Box box = feetBox();
                    U32 id = 0;
                    F32 z = 0.f;
                    const bool surface = skoverview::feetSurface(texel, depth, box.nearZ(), box.farZ(), LLRender::sReverseZ, id, z) &&
                                         extents.count(id) && extents.at(id).mCuttable;
                    cache.mBest = surface ? id : 0;
                    cache.mBestZ = surface ? cache.mFeetZ + z : -1e30;
                    cache.mCovered = surface && !skoverview::passedExtents(cache.mCoverCandidates, counts, mRootExtent).empty();
                    ++mProbeStats.mApplied;
                    mProbeStats.mLatencyFrames += probes->frame() - cache.mRoofProbe.mIssued;
                }
                if (cache.mRoofProbe.wants(roof_key))
                {
                    issueRoof(cache, occupant, roof_key);
                }
                probing = probing || cache.mRoofProbe.mPending;
            }
            const U32 best = cache.mBest;
            const F64 best_z = cache.mBestZ;
            const bool best_covered = cache.mCovered;
            U32 roof = 0;
            bool has_roof = false;
            F64 roof_z = 0.0;
            const bool on_surface = best_z > -1e29;
            const auto held = mStanding.find(occupant.mId);
            if (on_surface && !best_covered)
            {
                roof = best;
                has_roof = true;
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
                        has_roof = true;
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
            if (has_roof)
            {
                const F64 above = occupant.mCut - occupant.mFloor.mdV[VZ];
                occupant.mFloor.setVec(feet.mdV[VX], feet.mdV[VY], roof_z);
                occupant.mCut = (F32)(roof_z + above);
                occupant.mRoom.reset();
                const auto it = roof_of.find(roof);
                roof_of[roof] = it == roof_of.end() ? occupant.mCut : llmax(it->second, occupant.mCut);
            }
        }
    }
    for (auto it = mStanding.begin(); it != mStanding.end();)
    {
        it = now - it->second.mAt >= HOLD_S ? mStanding.erase(it) : std::next(it);
    }
    {
        LL_PROFILE_ZONE_NAMED("sk cut marks");
        for (const Occupant* occupant : ordered)
        {
            if (occupant->mOutdoors)
            {
                continue;
            }
            const LLVector3d& p = occupant->mFloor;
            OccupantCut& cache = mOccupantCuts[occupant->mId];
            cache.mSeen = true;
            // What their room or column takes depends on their floor, their cut and their room alone.
            U64 marks_key = 1469598103934665603ull;
            for (S32 i = 0; i < 3; ++i)
            {
                quantise(marks_key, p.mdV[i]);
            }
            quantise(marks_key, occupant->mCut);
            quantise(marks_key, occupant->mRoomCellM);
            if (occupant->mRoom)
            {
                marks_key = (marks_key ^ occupant->mRoom->mHash) * 1099511628211ull;
                marks_key = (marks_key ^ occupant->mRoom->mId) * 1099511628211ull;
                marks_key = (marks_key ^ occupant->mRoom->mMesh.getCRC32()) * 1099511628211ull;
            }
            if (baseCrossed(occupant->mCut))
            {
                cache.mMarksProbe.invalidate();
            }
            if (probes)
            {
                // Their room's building, by its geometry: the linksets standing round its cells (walls) or over them
                // (floors above, the roof); with no room, those standing over their column. What stood is kept until
                // the answer lands.
                LL_PROFILE_ZONE_NAMED("sk cut probes collect");
                std::vector<U64> counts;
                bool lost = false;
                const bool ready = landed(cache.mMarkTickets, counts, lost);
                if (cache.mMarksProbe.settle(ready, lost, mExtentsSignature, false) == skoverview::ProbeSlot::Outcome::Apply)
                {
                    const std::vector<U32> passed = skoverview::passedExtents(cache.mMarkCandidates, counts, mRootExtent);
                    bool changed = passed.size() != cache.mMarks.size();
                    for (size_t i = 0; i < passed.size() && !changed; ++i)
                    {
                        changed = passed[i] != cache.mMarks[i].first || cache.mMarks[i].second != cache.mMarkRule;
                    }
                    const U32 late = probes->frame() - cache.mMarksProbe.mIssued;
                    if (changed)
                    {
                        std::string ids;
                        for (const U32 id : passed)
                        {
                            ids += llformat("%s%u", ids.empty() ? "" : " ", id);
                        }
                        LL_INFOS("SKOverview") << "probe " << cache.mMarkRule << " for " << occupant->mId << ": " << passed.size() << " of "
                                               << cache.mMarkCandidates.size() << " linksets [" << ids << "], " << late << " frames late"
                                               << LL_ENDL;
                    }
                    cache.mMarks.clear();
                    for (const U32 id : passed)
                    {
                        cache.mMarks.emplace_back(id, cache.mMarkRule);
                    }
                    ++mProbeStats.mApplied;
                    mProbeStats.mLatencyFrames += late;
                }
                if (cache.mMarksProbe.wants(marks_key))
                {
                    if (occupant->mRoom)
                    {
                        issueRoom(cache, *occupant, marks_key);
                    }
                    else
                    {
                        issueColumn(cache, *occupant, marks_key);
                    }
                }
                probing = probing || cache.mMarksProbe.mPending;
            }
        }
        for (const Occupant* occupant : marking)
        {
            const auto cut = mOccupantCuts.find(occupant->mId);
            if (occupant->mOutdoors || cut == mOccupantCuts.end())
            {
                continue;
            }
            for (const auto& m : cut->second.mMarks)
            {
                mark(m.first, *occupant, m.second);
            }
        }
    }
    for (auto it = mOccupantCuts.begin(); it != mOccupantCuts.end();)
    {
        it = it->second.mSeen ? std::next(it) : mOccupantCuts.erase(it);
    }
    if (probes)
    {
        mProbeStats.mIssued += budget.people();
        mProbeStats.mQueries += probes->queriesIssued();
        mProbeStats.mMostQueries = llmax(mProbeStats.mMostQueries, probes->queriesIssued());
        mProbeStats.mRefused += probes->queriesRefused();
        LL_PROFILE_PLOT("census probe queries", (int64_t)probes->queriesIssued());
        LL_PROFILE_PLOT("census probe refused", (int64_t)probes->queriesRefused());
        std::vector<U32> ids;
        std::vector<F32> depths;
        S32 w = 0, h = 0;
        if (mProbeDump.mPending && probes->collectDump(ids, depths, w, h))
        {
            mProbeDump.mPending = false;
            const char* dir = getenv("SK_PROBE_DUMP_DIR");
            const std::string name = "room-ids-" + mProbeDump.mWho.asString() + ".ppm";
            const std::string path = dir && *dir ? std::string(dir) + gDirUtilp->getDirDelimiter() + name
                                                 : gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
            SKOverviewProbes::Box box = mProbeDump.mBox;
            box.mWidth = w;
            box.mHeight = h;
            const bool written = SKOverviewProbes::writeImage(path, ids, w, h, SKOverviewProbes::outTexels(box, mProbeDump.mOuts),
                                                              llmax(1, 800 / llmax(w, h)));
            LL_INFOS("SKOverview") << "probe dump " << (written ? "written to " : "not written to ") << path << LL_ENDL;
        }
        if (now - mProbeStats.mSince >= 1.0)
        {
            U32 pending = 0;
            for (const auto& entry : mOccupantCuts)
            {
                pending += entry.second.mMarksProbe.mPending || entry.second.mRoofProbe.mPending ? 1 : 0;
            }
            const F32 latency = mProbeStats.mApplied ? (F32)mProbeStats.mLatencyFrames / mProbeStats.mApplied : 0.f;
            LL_PROFILE_PLOT("census readback latency", (F64)latency);
            if (mProbeStats.mIssued || pending || mProbeStats.mRefused)
            {
                LL_INFOS("SKOverview") << "probe: " << mProbeStats.mIssued << " issued, " << pending << " pending, "
                                       << mProbeStats.mQueries << " queries (" << mProbeStats.mMostQueries
                                       << " in a frame at most, " << mProbeStats.mRefused << " refused), "
                                       << llformat("%.1f", latency)
                                       << " frames latency" << LL_ENDL;
            }
            mProbeStats = ProbeStats();
            mProbeStats.mSince = now;
        }
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
        if (releasing || probing)
        {
            mCutKey = 0; // worked again until the release holds and the probes land
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
        for (const auto& [id, e] : extents)
        {
            if (id >= mCutTable.rows())
            {
                continue;
            }
            const auto it = cut_of.find(id);
            SKOverviewCutTable::Row& row = mCutTable.row(id);
            // A linkset wholly under its cut is left whole, so an open mesh there (a desk with no underside) is never
            // counted as a solid the cut went through.
            const bool whole = it == cut_of.end() || (it->second != skoverview::HIDDEN && e.mMax.mdV[VZ] <= it->second);
            row.mCut = whole ? SKOverviewCutTable::WHOLE : it->second;
            row.mMinZ = (F32)e.mMin.mdV[VZ];
            row.mMaxZ = (F32)e.mMax.mdV[VZ];
        }
        mCutTable.flush();
    }
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
