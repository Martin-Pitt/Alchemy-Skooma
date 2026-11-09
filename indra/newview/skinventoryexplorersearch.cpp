/**
 * @file skinventoryexplorersearch.cpp
 * @brief Inventory Explorer's search: the query parser, matching and the time-sliced walk
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skinventoryexplorersearch.h"

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventorysettings.h"
#include "lltimer.h"
#include "llviewerinventory.h"
#include "llwearabletype.h"
#include "skinventoryflatview.h"

#include <set>

namespace
{
    constexpr F64 SLICE_SECONDS = 0.002;

    constexpr S32 TIMER_CHECK_EVERY = 64;   // objects visited between looks at the clock
    constexpr F32 CREATOR_RETRY_SECONDS = 1.f;

    // A case-insensitive find of an already lower-case needle, with no allocation.
    bool contains(std::string_view haystack, const std::string& needle_lower)
    {
        return std::search(haystack.begin(), haystack.end(), needle_lower.begin(), needle_lower.end(),
                           [](char a, char b) { return (char)tolower((unsigned char)a) == b; })
               != haystack.end();
    }

    std::string lower(std::string text)
    {
        LLStringUtil::toLower(text);
        return text;
    }

    // The item a link stands for, else the item itself; null for folders.
    const LLViewerInventoryItem* item_info(const LLInventoryObject* obj)
    {
        const LLViewerInventoryItem* item = dynamic_cast<const LLViewerInventoryItem*>(obj);
        if (!item)
        {
            return nullptr;
        }
        if (item->getIsLinkType())
        {
            if (const LLViewerInventoryItem* target = gInventory.getItem(item->getLinkedUUID()))
            {
                return target;
            }
        }
        return item;
    }

    bool is_folder(const LLInventoryObject* obj)
    {
        if (obj->getType() == LLAssetType::AT_CATEGORY)
        {
            return true;
        }
        const LLViewerInventoryItem* item = dynamic_cast<const LLViewerInventoryItem*>(obj);
        return item && item->getIsLinkType() && gInventory.getCategory(item->getLinkedUUID());
    }

    // The subtype a type: value may also name: a wearable's kind or a settings kind.
    std::string_view subtype_of(const LLViewerInventoryItem* info)
    {
        if (info->isWearableType())
        {
            return LLWearableType::getTypeName(info->getWearableType());
        }
        if (info->getType() == LLAssetType::AT_SETTINGS)
        {
            switch (LLSettingsType::fromInventoryFlags(info->getFlags()))
            {
                case LLSettingsType::ST_SKY:      return "sky";
                case LLSettingsType::ST_WATER:    return "water";
                case LLSettingsType::ST_DAYCYCLE: return "daycycle";
                default:                          break;
            }
        }
        return std::string_view();
    }

    // A creator: value: a UUID, "you", or a username, legacy "first.last" or "First Last" included.
    LLUUID resolve_creator(const std::string& value)
    {
        LLUUID id;
        if (LLUUID::validate(value) && id.set(value, false))
        {
            return id;
        }
        const std::string low = lower(value);
        if (low == "you" || low == "me")
        {
            return gAgentID;
        }
        std::string spaced = value;
        std::replace(spaced.begin(), spaced.end(), '.', ' ');
        std::vector<std::string> tries = { value, spaced };
        // The cache keys legacy names capitalised, and single names with "Resident".
        std::string capital = lower(spaced);
        bool start = true;
        for (char& c : capital)
        {
            if (start)
            {
                c = (char)toupper(c);
            }
            start = c == ' ';
        }
        tries.push_back(capital);
        if (capital.find(' ') == std::string::npos)
        {
            tries.push_back(capital + " Resident");
        }
        for (const std::string& name : tries)
        {
            id = LLAvatarNameCache::getInstance()->findIdByName(name);
            if (id.notNull())
            {
                return id;
            }
        }
        return LLUUID::null;
    }

    bool perm_test(const std::string& value, U32 mask, bool& known)
    {
        known = true;
        const bool copy = mask & PERM_COPY;
        const bool mod = mask & PERM_MODIFY;
        const bool trans = mask & PERM_TRANSFER;
        if (value == "full")    return copy && mod && trans;
        if (value == "copy")    return copy;
        if (value == "nocopy")  return !copy;
        if (value == "mod")     return mod;
        if (value == "nomod")   return !mod;
        if (value == "trans")   return trans;
        if (value == "notrans") return !trans;
        known = false;
        return false;
    }

    bool is_known(const std::string& value)
    {
        static const std::set<std::string> IS_VALUES = { "worn", "link", "fav", "favorite", "favourite", "folder", "item" };
        return IS_VALUES.contains(value);
    }
}

//
// SKInventorySearchQuery
//

std::vector<std::pair<size_t, size_t>> SKInventorySearchQuery::termSpans(const std::string& text)
{
    // A term runs to the next space outside quotes.
    std::vector<std::pair<size_t, size_t>> spans;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && text[i] == ' ')
        {
            ++i;
        }
        if (i >= text.size())
        {
            break;
        }
        const size_t start = i;
        bool quoted = false;
        while (i < text.size() && (quoted || text[i] != ' '))
        {
            if (text[i] == '"')
            {
                quoted = !quoted;
            }
            ++i;
        }
        spans.emplace_back(start, i);
    }
    return spans;
}

const char* SKInventorySearchQuery::typeKey(const LLInventoryObject* obj)
{
    if (is_folder(obj))
    {
        return "folder";
    }
    const LLViewerInventoryItem* info = item_info(obj);
    switch (info ? info->getType() : LLAssetType::AT_NONE)
    {
        case LLAssetType::AT_OBJECT:      return "object";
        case LLAssetType::AT_CLOTHING:    return "clothing";
        case LLAssetType::AT_BODYPART:    return "bodypart";
        case LLAssetType::AT_TEXTURE:     return "texture";
        case LLAssetType::AT_SOUND:       return "sound";
        case LLAssetType::AT_ANIMATION:   return "animation";
        case LLAssetType::AT_GESTURE:     return "gesture";
        case LLAssetType::AT_LANDMARK:    return "landmark";
        case LLAssetType::AT_NOTECARD:    return "notecard";
        case LLAssetType::AT_LSL_TEXT:    return "script";
        case LLAssetType::AT_MATERIAL:    return "material";
        case LLAssetType::AT_SETTINGS:    return "settings";
        case LLAssetType::AT_CALLINGCARD: return "callingcard";
        default:                          return "other";
    }
}

time_t SKInventorySearchQuery::parseDay(const std::string& value)
{
    tm parts{};
    const time_t now = time(nullptr);
#if LL_WINDOWS
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    parts.tm_hour = 0;
    parts.tm_min = 0;
    parts.tm_sec = 0;
    parts.tm_isdst = -1;
    if (value == "today")
    {
        return mktime(&parts);
    }
    S32 days = 0;
    char unit = 0;
    char rest = 0;
    if (sscanf(value.c_str(), "%d%c%c", &days, &unit, &rest) == 2 && unit == 'd' && days >= 0)
    {
        parts.tm_mday -= days;
        return mktime(&parts);
    }
    S32 year = 0;
    S32 month = 0;
    S32 day = 0;
    if (sscanf(value.c_str(), "%d-%d-%d%c", &year, &month, &day, &rest) == 3 && year >= 1970 && month >= 1 && month <= 12
        && day >= 1 && day <= 31)
    {
        tm date{};
        date.tm_year = year - 1900;
        date.tm_mon = month - 1;
        date.tm_mday = day;
        date.tm_isdst = -1;
        return mktime(&date);
    }
    return 0;
}

SKInventorySearchQuery::SKInventorySearchQuery(const std::string& text)
{
    static const std::pair<const char*, SKSearchTerm::EKind> KEYS[] = {
        { "type", SKSearchTerm::KIND_TYPE },       { "brand", SKSearchTerm::KIND_BRAND },
        { "creator", SKSearchTerm::KIND_CREATOR }, { "perm", SKSearchTerm::KIND_PERM },
        { "is", SKSearchTerm::KIND_IS },           { "after", SKSearchTerm::KIND_AFTER },
        { "before", SKSearchTerm::KIND_BEFORE } };

    for (const auto& [start, end] : termSpans(text))
    {
        SKSearchTerm term;
        term.mSource = text.substr(start, end - start);
        std::string raw = term.mSource;
        const size_t colon = raw.find(':');
        std::string key = colon != std::string::npos ? lower(raw.substr(0, colon)) : std::string();
        bool keyed = false;
        if (colon != std::string::npos && !key.empty() && raw.front() != '"')
        {
            for (const auto& [name, kind] : KEYS)
            {
                if (key == name)
                {
                    term.mKind = kind;
                    raw = raw.substr(colon + 1);
                    keyed = true;
                    break;
                }
            }
        }
        std::erase(raw, '"');
        // A key with nothing after it yet is still being typed.
        if (raw.empty())
        {
            continue;
        }
        term.mValue = lower(raw);
        if (!keyed && term.mValue == "worn")
        {
            term.mKind = SKSearchTerm::KIND_IS;
        }

        bool known = true;
        switch (term.mKind)
        {
            case SKSearchTerm::KIND_CREATOR:
                term.mCreator = resolve_creator(raw);
                term.mFlagged = term.mCreator.isNull();
                break;
            case SKSearchTerm::KIND_PERM:
                perm_test(term.mValue, 0, known);
                term.mFlagged = !known;
                break;
            case SKSearchTerm::KIND_IS:
                term.mFlagged = !is_known(term.mValue);
                break;
            case SKSearchTerm::KIND_AFTER:
            case SKSearchTerm::KIND_BEFORE:
                term.mTime = parseDay(term.mValue);
                term.mFlagged = term.mTime == 0;
                break;
            default:
                break;
        }
        mTerms.push_back(term);
    }
}

std::string SKInventorySearchQuery::flaggedTerm() const
{
    for (const SKSearchTerm& term : mTerms)
    {
        if (term.mFlagged)
        {
            return term.mSource;
        }
    }
    return std::string();
}

S32 SKInventorySearchQuery::match(const LLInventoryObject* obj) const
{
    if (!obj || mTerms.empty())
    {
        return NO_MATCH;
    }
    const LLViewerInventoryItem* info = item_info(obj);
    const bool folder = is_folder(obj);
    S32 rank = RANK_NAME;
    for (const SKSearchTerm& term : mTerms)
    {
        if (term.mFlagged)
        {
            return NO_MATCH;
        }
        switch (term.mKind)
        {
            case SKSearchTerm::KIND_TEXT:
            {
                if (contains(obj->getName(), term.mValue))
                {
                    break;
                }
                if (!info || !contains(info->getDescription(), term.mValue))
                {
                    return NO_MATCH;
                }
                rank = RANK_DESCRIPTION;
                break;
            }
            case SKSearchTerm::KIND_TYPE:
                if (!contains(typeKey(obj), term.mValue) && (folder || !info || !contains(subtype_of(info), term.mValue)))
                {
                    return NO_MATCH;
                }
                break;
            case SKSearchTerm::KIND_BRAND:
                if (!contains(SKInventoryFlatView::brandOf(obj->getName()), term.mValue))
                {
                    return NO_MATCH;
                }
                break;
            case SKSearchTerm::KIND_CREATOR:
                if (folder || !info || info->getCreatorUUID() != term.mCreator)
                {
                    return NO_MATCH;
                }
                break;
            case SKSearchTerm::KIND_PERM:
            {
                bool known = true;
                if (folder || !info || !perm_test(term.mValue, info->getPermissions().getMaskOwner(), known))
                {
                    return NO_MATCH;
                }
                break;
            }
            case SKSearchTerm::KIND_IS:
            {
                const LLViewerInventoryItem* item = dynamic_cast<const LLViewerInventoryItem*>(obj);
                bool hit = false;
                if (term.mValue == "worn")
                {
                    hit = info && !folder && get_is_item_worn(info->getUUID());
                }
                else if (term.mValue == "link")
                {
                    hit = item && item->getIsLinkType();
                }
                else if (term.mValue == "folder")
                {
                    hit = folder;
                }
                else if (term.mValue == "item")
                {
                    hit = !folder;
                }
                else
                {
                    hit = get_is_favorite(obj);
                }
                if (!hit)
                {
                    return NO_MATCH;
                }
                break;
            }
            case SKSearchTerm::KIND_AFTER:
            case SKSearchTerm::KIND_BEFORE:
            {
                // Folders carry no date.
                const LLViewerInventoryItem* item = dynamic_cast<const LLViewerInventoryItem*>(obj);
                const time_t acquired = (item && !folder) ? item->getCreationDate() : 0;
                if (acquired <= 0)
                {
                    return NO_MATCH;
                }
                if (term.mKind == SKSearchTerm::KIND_AFTER ? acquired < term.mTime : acquired >= term.mTime)
                {
                    return NO_MATCH;
                }
                break;
            }
        }
    }
    return rank;
}

bool SKInventorySearchQuery::waitsForCreator() const
{
    return std::any_of(mTerms.begin(), mTerms.end(),
                       [](const SKSearchTerm& term) { return term.mFlagged && term.mKind == SKSearchTerm::KIND_CREATOR; });
}

//
// SKInventoryWalk
//

void SKInventoryWalk::start(const LLUUID& root_id)
{
    mRoot = root_id;
    mPending.assign(1, root_id);
    mCurrent.setNull();
    mLoading.clear();
    // The Trash is left out, unless the walk starts inside it.
    mTrash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (mTrash == root_id || gInventory.isObjectDescendentOf(root_id, mTrash))
    {
        mTrash.setNull();
    }
}

void SKInventoryWalk::stop()
{
    mRoot.setNull();
    mPending.clear();
    mCurrent.setNull();
    mLoading.clear();
}

bool SKInventoryWalk::covers(const LLUUID& id) const
{
    if (mRoot.isNull() || (id != mRoot && !gInventory.isObjectDescendentOf(id, mRoot)))
    {
        return false;
    }
    return mTrash.isNull() || (id != mTrash && !gInventory.isObjectDescendentOf(id, mTrash));
}

void SKInventoryWalk::add(const LLUUID& folder_id)
{
    if (gInventory.getCategory(folder_id) && covers(folder_id))
    {
        mPending.push_back(folder_id);
    }
}

bool SKInventoryWalk::step(F64 seconds, const visit_t& visit)
{
    // Folders asked for once a session, so walking a folder again does not ask again.
    static std::set<LLUUID> s_fetched;
    LLTimer timer;
    S32 visited = 0;
    auto out_of_time = [&]() { return ++visited % TIMER_CHECK_EVERY == 0 && timer.getElapsedTimeF64() >= seconds; };
    while (!isDone())
    {
        if (mCurrent.isNull())
        {
            mCurrent = mPending.back();
            mPending.pop_back();
            mCatIndex = 0;
            mItemIndex = 0;
            const LLViewerInventoryCategory* folder = gInventory.getCategory(mCurrent);
            if (!folder)
            {
                mLoading.erase(mCurrent);
                mCurrent.setNull();
                continue;
            }
            if (folder->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN || !gInventory.isCategoryComplete(mCurrent))
            {
                mLoading.insert(mCurrent);
                if (s_fetched.insert(mCurrent).second)
                {
                    LLInventoryModelBackgroundFetch::instance().start(mCurrent, true);
                }
            }
            else
            {
                mLoading.erase(mCurrent);
            }
        }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(mCurrent, cats, items);
        for (; cats && mCatIndex < cats->size(); ++mCatIndex)
        {
            LLViewerInventoryCategory* cat = cats->at(mCatIndex);
            if (cat->getUUID() == mTrash)
            {
                continue;
            }
            visit(cat);
            mPending.push_back(cat->getUUID());
            if (out_of_time())
            {
                ++mCatIndex;
                return false;
            }
        }
        for (; items && mItemIndex < items->size(); ++mItemIndex)
        {
            visit(items->at(mItemIndex));
            if (out_of_time())
            {
                ++mItemIndex;
                return false;
            }
        }
        mCurrent.setNull();
        if (timer.getElapsedTimeF64() >= seconds)
        {
            break;
        }
    }
    return isDone();
}

//
// SKInventorySearch
//

void SKInventorySearch::start(const LLUUID& folder_id, const std::string& text)
{
    mFolder = folder_id;
    mText = text;
    mQuery = SKInventorySearchQuery(text);
    mHits.clear();
    mHitIDs.clear();
    ++mRemovals;
    mActive = !mQuery.empty() && folder_id.notNull();
    mCreatorRetry.reset();
    startWalk();
    touchState();
    ++mStateGeneration;
}

void SKInventorySearch::startWalk()
{
    // A term naming nothing matches nothing: no walk until it names something.
    if (mActive && mQuery.flaggedTerm().empty())
    {
        mWalk.start(mFolder);
    }
    else
    {
        mWalk.stop();
    }
}

void SKInventorySearch::stop()
{
    mWalk.stop();
    mHits.clear();
    mHitIDs.clear();
    ++mRemovals;
    mText.clear();
    mQuery = SKInventorySearchQuery();
    mFolder.setNull();
    mActive = false;
    ++mStateGeneration;
}

void SKInventorySearch::removeHit(const LLUUID& id)
{
    mHitIDs.erase(id);
    std::erase_if(mHits, [&id](const hit_t& hit) { return hit.first == id; });
    ++mRemovals;
}

void SKInventorySearch::noteChanges(const std::set<LLUUID>& changed_ids)
{
    if (!mActive || !mWalk.isStarted())
    {
        return;
    }
    bool moved_out = false;
    for (const LLUUID& id : changed_ids)
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        const bool covered = obj && mWalk.covers(id);
        if (mHitIDs.contains(id))
        {
            // A hit that changed is checked again where it stands.
            const S32 rank = covered ? mQuery.match(obj) : SKInventorySearchQuery::NO_MATCH;
            if (rank == SKInventorySearchQuery::NO_MATCH)
            {
                removeHit(id);
            }
            else
            {
                for (hit_t& hit : mHits)
                {
                    if (hit.first == id && hit.second != rank)
                    {
                        hit.second = rank;
                        ++mRemovals;
                    }
                }
            }
        }
        if (!obj)
        {
            continue;
        }
        if (obj->getType() == LLAssetType::AT_CATEGORY)
        {
            if (covered)
            {
                mWalk.add(id);
            }
            else
            {
                moved_out = true;
            }
        }
        else
        {
            mWalk.add(obj->getParentUUID());
        }
    }
    // A folder that left the searched folder takes its hits with it.
    if (moved_out)
    {
        std::vector<LLUUID> gone;
        for (const hit_t& hit : mHits)
        {
            if (!gInventory.getObject(hit.first) || !mWalk.covers(hit.first))
            {
                gone.push_back(hit.first);
            }
        }
        for (const LLUUID& id : gone)
        {
            removeHit(id);
        }
    }
    touchState();
}

void SKInventorySearch::step()
{
    if (!mActive)
    {
        return;
    }
    // A creator not yet in the name cache is looked for again once a second.
    if (!mWalk.isStarted())
    {
        if (mQuery.waitsForCreator() && mCreatorRetry.getElapsedTimeF32() >= CREATOR_RETRY_SECONDS)
        {
            mCreatorRetry.reset();
            mQuery = SKInventorySearchQuery(mText);
            if (mQuery.flaggedTerm().empty())
            {
                startWalk();
                ++mStateGeneration;
            }
        }
        return;
    }
    if (!mWalk.isDone())
    {
        mWalk.step(SLICE_SECONDS, [this](const LLInventoryObject* obj)
                   {
                       if (mHitIDs.contains(obj->getUUID()))
                       {
                           return;
                       }
                       const S32 rank = mQuery.match(obj);
                       if (rank != SKInventorySearchQuery::NO_MATCH)
                       {
                           mHits.emplace_back(obj->getUUID(), rank);
                           mHitIDs.insert(obj->getUUID());
                       }
                   });
    }
    touchState();
}

void SKInventorySearch::touchState()
{
    const bool done = mWalk.isDone();
    const S32 loading = mWalk.getLoadingCount();
    if (done != mLastDone || loading != mLastLoading)
    {
        mLastDone = done;
        mLastLoading = loading;
        ++mStateGeneration;
    }
}

