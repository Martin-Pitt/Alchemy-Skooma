/**
 * @file skworldmaphubs.cpp
 * @brief The 3D world map's freight hubs: the hub list fetched at most once a refresh period, kept on disk,
 *        and matched to the regions the world map knows.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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
#include "skworldmaphubs.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "llworldmap.h"

namespace
{
    constexpr F64 RETRY_SECONDS = 3600.0;     // a failed fetch waits this long, so a site that is down is asked once an hour
    constexpr F64 DAY_SECONDS = 86400.0;
    constexpr F64 PLACES_CHECK_SECONDS = 30.0;  // how often the places file is looked at for changes
    const char* const CACHE_FILE = "sk_worldmap_hubs.html";

    std::string cachePath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, CACHE_FILE);
    }
}

SKWorldMapHubs::~SKWorldMapHubs()
{
    if (mSelf)
    {
        *mSelf = nullptr;
    }
}

void SKWorldMapHubs::update(F64 now)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKWorldMapHubs", true);
    static LLCachedControl<std::string> url(gSavedSettings, "SKWorldMapHubsURL", "");
    static LLCachedControl<F32> refresh_days(gSavedSettings, "SKWorldMapHubsRefreshDays", 7.f);
    if (!enabled)
    {
        return;
    }
    if (!mLoaded)
    {
        loadCache();
    }
    updatePlaces(now);
    const std::string source = url();
    if (mFetching || source.empty() || now < mRetryAt)
    {
        return;
    }
    const F64 age = LLDate::now().secondsSinceEpoch() - mFetchedAt;
    if (mFetchedAt <= 0.0 || age >= std::max(static_cast<F64>(refresh_days), 1.0) * DAY_SECONDS)
    {
        fetch(source, now);
    }
}

void SKWorldMapHubs::loadCache()
{
    mLoaded = true;
    const std::string path = cachePath();
    llstat status;
    if (LLFile::stat(path, &status) != 0)
    {
        return;
    }
    skworldmap::HubTable table = skworldmap::HubTable::parse(LLFile::getContents(path));
    if (table.empty())
    {
        // A copy that reads as nothing is as good as none: fetch again.
        return;
    }
    mFetchedAt = static_cast<F64>(status.st_mtime);
    LL_INFOS("WorldMap") << "Read " << table.hubs().size() << " hubs from " << path << LL_ENDL;
    adopt(std::move(table));
}

void SKWorldMapHubs::fetch(const std::string& url, F64 now)
{
    LL_INFOS("WorldMap") << "Fetching the hub list from " << url << LL_ENDL;
    mFetching = true;
    // Set before the reply, so a reply that never comes still waits out the retry.
    mRetryAt = now + RETRY_SECONDS;
    if (!mSelf)
    {
        mSelf = std::make_shared<SKWorldMapHubs*>(this);
    }
    std::weak_ptr<SKWorldMapHubs*> self = mSelf;
    LLCoros::instance().launch("SKWorldMapHubs::fetch", [self, url]()
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("SKWorldMapHubs", policy);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        const LLSD result = adapter->getRawAndSuspend(request, url);

        std::shared_ptr<SKWorldMapHubs*> owner = self.lock();
        if (!owner || !*owner)
        {
            return;
        }
        SKWorldMapHubs* hubs = *owner;
        hubs->mFetching = false;
        const LLCore::HttpStatus status =
            LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        const std::string page(raw.begin(), raw.end());
        skworldmap::HubTable table = status ? skworldmap::HubTable::parse(page) : skworldmap::HubTable();
        if (table.empty())
        {
            // Keep whatever list we had; a page that changed its layout should not wipe the map's hubs.
            LL_WARNS("WorldMap") << "Could not read a hub list from " << url << ": " << status.toString() << LL_ENDL;
            return;
        }

        const std::string path = cachePath();
        const std::string temp = path + ".tmp";
        LLFile::remove(temp, ENOENT);
        if (LLFile::write(temp, page.data(), 0, static_cast<S64>(page.size())) == static_cast<S64>(page.size()))
        {
            LLFile::remove(path, ENOENT);
            LLFile::rename(temp, path);
        }
        hubs->mFetchedAt = LLDate::now().secondsSinceEpoch();
        hubs->mRetryAt = 0.0;
        LL_INFOS("WorldMap") << "Fetched " << table.hubs().size() << " hubs" << LL_ENDL;
        hubs->adopt(std::move(table));
    });
}

void SKWorldMapHubs::adopt(skworldmap::HubTable table)
{
    mTable = std::move(table);
    mPlacedStale = true;
}

void SKWorldMapHubs::updatePlaces(F64 now)
{
    // Read again whenever the file changes, so places worked out while the map is open show at once.
    static LLCachedControl<std::string> places_path(gSavedSettings, "SKWorldMapHubPlaces", "");
    const std::string path = places_path();
    if (path == mPlacesPath && now < mPlacesCheckAt)
    {
        return;
    }
    mPlacesCheckAt = now + PLACES_CHECK_SECONDS;
    llstat status;
    const S64 written = !path.empty() && LLFile::stat(path, &status) == 0 ? static_cast<S64>(status.st_mtime) : -1;
    if (path == mPlacesPath && written == mPlacesWritten)
    {
        return;
    }
    mPlacesPath = path;
    mPlacesWritten = written;
    mPlaces = written >= 0 ? skworldmap::HubPlaces::parse(LLFile::getContents(path)) : skworldmap::HubPlaces();
    mPlacedStale = true;
    if (!path.empty())
    {
        LL_INFOS("WorldMap") << "Read " << mPlaces.size() << " hub places from " << path << LL_ENDL;
    }
}

const std::vector<SKWorldMapHubs::Placed>& SKWorldMapHubs::placed()
{
    // The world map only ever adds regions, so its count moving is when names may have arrived.
    const auto& regions = LLWorldMap::getInstance()->getRegionMap();
    if (!mPlacedStale && regions.size() == mPlacedRegions)
    {
        return mPlaced;
    }
    mPlacedStale = false;
    mPlacedRegions = regions.size();
    mPlaced.clear();
    if (mTable.empty())
    {
        return mPlaced;
    }
    const skworldmap::Hub* first = mTable.hubs().data();
    for (const auto& [handle, info] : regions)
    {
        if (info && !info->getName().empty())
        {
            if (const skworldmap::Hub* hub = mTable.find(info->getName()))
            {
                const F32 middle = REGION_WIDTH_METERS * 0.5f;
                const skworldmap::HubPlace* place = mPlaces.find(info->getName());
                mPlaced.push_back({ handle, static_cast<U32>(hub - first), place ? place->mX : middle, place ? place->mY : middle });
            }
        }
    }
    return mPlaced;
}
