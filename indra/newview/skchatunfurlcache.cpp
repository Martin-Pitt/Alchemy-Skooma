/**
 * @file skchatunfurlcache.cpp
 * @brief Previews of the links in local chat, looked up once per link and kept for reuse.
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

#include "skchatunfurlcache.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llregionhandle.h"
#include "llremoteparcelrequest.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"

namespace
{
constexpr size_t MAX_ENTRIES = 200;
constexpr F64 REGION_TIMEOUT = 10.0;
constexpr F64 POLL_SECONDS = 0.25;
constexpr unsigned int PAGE_TIMEOUT = 5;
constexpr size_t MAX_LINE_BYTES = 240;

// The first line of free text, for a one-line description.
std::string first_line(const std::string& text)
{
    std::string line = text.substr(0, text.find_first_of("\r\n"));
    LLStringUtil::trim(line);
    if (line.size() > MAX_LINE_BYTES)
    {
        size_t cut = MAX_LINE_BYTES;
        while (cut > 0 && ((U8)line[cut] & 0xC0) == 0x80)
        {
            --cut;
        }
        line.resize(cut);
    }
    return line;
}

LLViewerFetchedTexture* fetch_texture(const LLUUID& id)
{
    return id.isNull() ? nullptr : LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE);
}
} // namespace

// One place's parcel lookup, an observer of its own so concurrent replies are told apart.
class SKChatUnfurlCache::ParcelLookup final : public LLRemoteParcelInfoObserver
{
public:
    ParcelLookup(SKChatUnfurlCache* cache, const SKChatEmbedPtr& embed)
        : mCache(cache), mEmbed(embed)
    {
    }

    ~ParcelLookup() override
    {
        if (mParcelID.notNull() && LLRemoteParcelInfoProcessor::instanceExists())
        {
            LLRemoteParcelInfoProcessor::getInstance()->removeObserver(mParcelID, this);
        }
    }

    void setParcelID(const LLUUID& parcel_id) override
    {
        if (parcel_id.isNull())
        {
            done(false);
            return;
        }
        mParcelID = parcel_id;
        // ParcelInfoRequest has no capability; RemoteParcelRequest only names the parcel.
        LLRemoteParcelInfoProcessor::getInstance()->addObserver(parcel_id, this);
        LLRemoteParcelInfoProcessor::getInstance()->sendParcelInfoRequest(parcel_id);
    }

    void processParcelInfo(const LLParcelData& data) override
    {
        if (SKChatEmbedPtr embed = mEmbed.lock())
        {
            if (!data.name.empty())
            {
                embed->mTitle = data.name;
            }
            std::string where = data.sim_name.empty() ? embed->mTarget.mRegion : data.sim_name;
            std::string desc = first_line(data.desc);
            embed->mDescription = desc.empty() ? where : where.empty() ? desc : where + " \xE2\x80\x94 " + desc;
            embed->mImage = fetch_texture(data.snapshot_id);
        }
        done(true);
    }

    void setErrorStatus(S32 status, const std::string& reason) override
    {
        LL_DEBUGS("SKChatUnfurl") << "parcel lookup failed: " << status << " " << reason << LL_ENDL;
        done(false);
    }

    bool finished() const { return mFinished; }

private:
    void done(bool found)
    {
        if (mFinished)
        {
            return;
        }
        mFinished = true;
        if (SKChatEmbedPtr embed = mEmbed.lock())
        {
            // A place whose parcel cannot be read still shows its region.
            const bool usable = found || !embed->mTitle.empty();
            mCache->finish(*embed, usable ? SKChatEmbed::READY : SKChatEmbed::FAILED);
        }
    }

    SKChatUnfurlCache* mCache;
    std::weak_ptr<SKChatEmbed> mEmbed;
    LLUUID mParcelID;
    bool mFinished = false;
};

SKChatUnfurlCache::SKChatUnfurlCache()
    : mGeneration(std::make_shared<U32>(0))
{
}

SKChatUnfurlCache::~SKChatUnfurlCache()
{
    if (LLAvatarPropertiesProcessor::instanceExists())
    {
        for (const auto& [id, embed] : mWaitingAgents)
        {
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(id, this);
        }
    }
    if (LLGroupMgr::instanceExists())
    {
        for (const auto& [id, embed] : mWaitingGroups)
        {
            LLGroupMgr::getInstance()->removeObserver(id, this);
        }
    }
    mParcelLookups.clear();
}

SKChatEmbedPtr SKChatUnfurlCache::find(const std::string& url)
{
    SKChatUnfurl::Target target = SKChatUnfurl::classify(url);
    if (target.mKind == SKChatUnfurl::EKind::NONE)
    {
        return nullptr;
    }
    auto found = mByKey.find(target.mKey);
    if (found != mByKey.end())
    {
        mLru.splice(mLru.begin(), mLru, found->second);
        return *found->second;
    }
    auto embed = std::make_shared<SKChatEmbed>();
    embed->mUrl = url;
    embed->mTarget = std::move(target);
    mLru.push_front(embed);
    mByKey.emplace(embed->mTarget.mKey, mLru.begin());
    while (mLru.size() > MAX_ENTRIES)
    {
        // Messages still holding the dropped preview keep it; only the cache forgets it.
        mByKey.erase(mLru.back()->mTarget.mKey);
        mLru.pop_back();
    }
    return embed;
}

void SKChatUnfurlCache::finish(SKChatEmbed& embed, SKChatEmbed::EState state)
{
    embed.mState = state;
    ++*mGeneration;
}

void SKChatUnfurlCache::request(const SKChatEmbedPtr& embed)
{
    if (!embed || embed->mState != SKChatEmbed::IDLE)
    {
        return;
    }
    embed->mState = SKChatEmbed::PENDING;
    using SKChatUnfurl::EKind;
    switch (embed->mTarget.mKind)
    {
    case EKind::PLACE:
        requestPlace(embed);
        break;
    case EKind::PARCEL:
    {
        // A parcel by id needs no region; the lookup answers as if RemoteParcelRequest had.
        mParcelLookups.push_back(std::make_unique<ParcelLookup>(this, embed));
        mParcelLookups.back()->setParcelID(embed->mTarget.mID);
        break;
    }
    case EKind::AGENT:
    {
        const LLUUID& id = embed->mTarget.mID;
        LLAvatarName name;
        if (LLAvatarNameCache::get(id, &name))
        {
            embed->mTitle = name.getCompleteName();
        }
        else
        {
            std::weak_ptr<SKChatEmbed> weak = embed;
            std::weak_ptr<U32> generation = mGeneration;
            LLAvatarNameCache::get(id, [weak, generation](const LLUUID&, const LLAvatarName& av_name)
            {
                SKChatEmbedPtr e = weak.lock();
                std::shared_ptr<U32> g = generation.lock();
                if (e && g)
                {
                    e->mTitle = av_name.getCompleteName();
                    ++*g;
                }
            });
        }
        embed->mRoundImage = true;
        const bool asked = mWaitingAgents.count(id) > 0;
        mWaitingAgents.emplace(id, embed);
        if (!asked)
        {
            // The AgentProfile capability, not the UDP request that truncates the about text.
            LLAvatarPropertiesProcessor::getInstance()->addObserver(id, this);
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(id);
        }
        break;
    }
    case EKind::GROUP:
    {
        const LLUUID& id = embed->mTarget.mID;
        const bool asked = mWaitingGroups.count(id) > 0;
        mWaitingGroups.emplace(id, embed);
        LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(id);
        if (data && data->isGroupPropertiesDataComplete())
        {
            fillGroup(id);
        }
        else if (!asked)
        {
            // Group profiles have no capability; this is the request the group panels make.
            LLGroupMgr::getInstance()->addObserver(id, this);
            LLGroupMgr::getInstance()->sendGroupPropertiesRequest(id);
        }
        break;
    }
    case EKind::MARKETPLACE:
        requestPage(embed);
        break;
    default:
        finish(*embed, SKChatEmbed::FAILED);
        break;
    }
}

void SKChatUnfurlCache::requestPlace(const SKChatEmbedPtr& embed)
{
    embed->mTitle = embed->mTarget.mRegion;
    embed->mDeadline = LLFrameTimer::getTotalSeconds() + REGION_TIMEOUT;
    if (!LLWorldMap::getInstance()->simInfoFromName(embed->mTarget.mRegion))
    {
        // No callback: the map's own single callback slot belongs to teleports and the map floater.
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(embed->mTarget.mRegion);
    }
    mWaitingRegions.push_back(embed);
    mNextPoll = 0.0;
}

void SKChatUnfurlCache::requestParcel(const SKChatEmbedPtr& embed, const LLUUID& region_id, const LLVector3& pos, const LLVector3d& global)
{
    LLViewerRegion* region = gAgent.getRegion();
    const std::string cap = region ? region->getCapability("RemoteParcelRequest") : std::string();
    mParcelLookups.push_back(std::make_unique<ParcelLookup>(this, embed));
    if (cap.empty() ||
        !LLRemoteParcelInfoProcessor::getInstance()->requestRegionParcelInfo(cap, region_id, pos, global, mParcelLookups.back()->getObserverHandle()))
    {
        finish(*embed, SKChatEmbed::READY); // the region alone
    }
}

void SKChatUnfurlCache::update()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mNextPoll)
    {
        return;
    }
    mNextPoll = now + POLL_SECONDS;

    if (!mParcelLookups.empty())
    {
        std::erase_if(mParcelLookups, [](const std::unique_ptr<ParcelLookup>& lookup) { return lookup->finished(); });
    }

    for (size_t i = 0; i < mWaitingRegions.size();)
    {
        SKChatEmbedPtr embed = mWaitingRegions[i].lock();
        LLSimInfo* info = embed ? LLWorldMap::getInstance()->simInfoFromName(embed->mTarget.mRegion) : nullptr;
        if (embed && !info && now < embed->mDeadline)
        {
            ++i;
            continue;
        }
        mWaitingRegions[i] = std::move(mWaitingRegions.back());
        mWaitingRegions.pop_back();
        if (!embed)
        {
            continue;
        }
        if (!info)
        {
            finish(*embed, SKChatEmbed::FAILED);
            continue;
        }
        embed->mTitle = info->getName();
        const LLVector3 pos(embed->mTarget.mX, embed->mTarget.mY, embed->mTarget.mZ);
        requestParcel(embed, LLUUID::null, pos, info->getGlobalOrigin() + LLVector3d(pos));
    }
}

void SKChatUnfurlCache::processProperties(void* data, EAvatarProcessorType type)
{
    if (type != APT_PROPERTIES || !data)
    {
        return;
    }
    const LLAvatarData* props = static_cast<const LLAvatarData*>(data);
    const LLUUID id = props->avatar_id;
    auto [begin, end] = mWaitingAgents.equal_range(id);
    for (auto it = begin; it != end; ++it)
    {
        if (SKChatEmbedPtr embed = it->second.lock())
        {
            embed->mDescription = first_line(props->about_text);
            embed->mImage = fetch_texture(props->image_id);
            finish(*embed, SKChatEmbed::READY);
        }
    }
    mWaitingAgents.erase(id);
    LLAvatarPropertiesProcessor::getInstance()->removeObserver(id, this);
}

void SKChatUnfurlCache::changed(const LLUUID& group_id, LLGroupChange gc)
{
    if (gc == GC_PROPERTIES || gc == GC_ALL)
    {
        LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id);
        if (data && data->isGroupPropertiesDataComplete())
        {
            fillGroup(group_id);
            LLGroupMgr::getInstance()->removeObserver(group_id, this);
        }
    }
}

void SKChatUnfurlCache::fillGroup(const LLUUID& group_id)
{
    LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id);
    auto [begin, end] = mWaitingGroups.equal_range(group_id);
    for (auto it = begin; it != end; ++it)
    {
        if (SKChatEmbedPtr embed = it->second.lock())
        {
            embed->mTitle = data ? data->mName : std::string();
            embed->mDescription = data ? first_line(data->mCharter) : std::string();
            embed->mImage = data ? fetch_texture(data->mInsigniaID) : nullptr;
            finish(*embed, embed->mTitle.empty() ? SKChatEmbed::FAILED : SKChatEmbed::READY);
        }
    }
    mWaitingGroups.erase(group_id);
}

void SKChatUnfurlCache::requestPage(const SKChatEmbedPtr& embed)
{
    std::weak_ptr<SKChatEmbed> weak = embed;
    std::weak_ptr<U32> generation = mGeneration;
    const std::string url = embed->mTarget.mKey;
    LLCoros::instance().launch("SKChatUnfurl::page", [weak, generation, url]()
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("SKChatUnfurl", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        // A redirect could lead to another host, so none is followed; listings answer directly.
        options->setFollowRedirects(false);
        options->setTimeout(PAGE_TIMEOUT);
        options->setTransferTimeout(PAGE_TIMEOUT);
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "text/html");
        headers->append(HTTP_OUT_HEADER_RANGE, llformat("bytes=0-%u", (U32)SKChatUnfurl::MAX_PAGE_BYTES - 1));

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);

        SKChatEmbedPtr embed = weak.lock();
        std::shared_ptr<U32> gen = generation.lock();
        if (!embed || !gen)
        {
            return;
        }
        const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        const S32 code = http["status"].asInteger();
        embed->mState = SKChatEmbed::FAILED;
        if (status && code >= 200 && code < 300)
        {
            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            const std::string_view page(reinterpret_cast<const char*>(raw.data()), std::min(raw.size(), SKChatUnfurl::MAX_PAGE_BYTES));
            SKChatUnfurl::OpenGraph og = SKChatUnfurl::parseOpenGraph(page);
            if (!og.mTitle.empty())
            {
                embed->mTitle = std::move(og.mTitle);
                embed->mDescription = first_line(og.mDescription);
                const std::string image = SKChatUnfurl::imageUrl(og.mImage);
                if (!image.empty())
                {
                    embed->mImage = LLViewerTextureManager::getFetchedTextureFromUrl(image, FTT_MAP_TILE, true, LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE);
                }
                embed->mState = SKChatEmbed::READY;
            }
        }
        ++*gen;
    });
}
