/**
 * @file skinventoryflatview.cpp
 * @brief Inventory Explorer's flat model and the base of the views drawn from it (List, Icons)
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

#include "skinventoryflatview.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llinventoryfunctions.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llwindow.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "llwearabletype.h"
#include "skinventoryexplorermenu.h"
#include "skinventoryrenameeditor.h"

#include <algorithm>
#include <ctime>

namespace
{
    constexpr F32 SLOW_CLICK_SECONDS = 0.55f;
    const char* const COLUMN_KEYS[SKInventoryFlatView::COL_COUNT] = { "name", "folder", "type", "creator", "acquired", "perms", "worn", "description" };
    const char* const GROUP_KEYS[SKInventoryFlatView::GROUP_COUNT] = { "none", "type", "date", "brand", "perms", "worn", "creator" };
    const char* const TYPE_ORDER[] = { "folders", "objects", "clothing", "bodyparts", "textures", "sounds", "animations", "gestures",
                                       "landmarks", "notecards", "scripts", "materials", "settings", "callingcards", "other" };
    const char* const PERM_KEYS[] = { "full_perm", "copy_modify", "copy_transfer", "copy_only", "no_copy" };

    S32 perm_rank(U32 mask)
    {
        const bool copy = mask & PERM_COPY;
        const bool mod = mask & PERM_MODIFY;
        const bool trans = mask & PERM_TRANSFER;
        if (!copy)
        {
            return 4;
        }
        if (mod && trans)
        {
            return 0;
        }
        return mod ? 1 : (trans ? 2 : 3);
    }

    std::string perm_letters(U32 mask)
    {
        std::string letters;
        letters += (mask & PERM_MODIFY) ? "M" : "";
        letters += (mask & PERM_COPY) ? "C" : "";
        letters += (mask & PERM_TRANSFER) ? "T" : "";
        return letters;
    }

    // A trailing "(w:h)" in the description, each one or two digits; 1:1 otherwise.
    F32 aspect_of(const std::string& desc)
    {
        const size_t open = desc.rfind('(');
        if (open == std::string::npos || desc.empty() || desc.back() != ')')
        {
            return 1.f;
        }
        const std::string inner = desc.substr(open + 1, desc.size() - open - 2);
        const size_t colon = inner.find(':');
        if (colon == std::string::npos || colon < 1 || colon > 2 || inner.size() - colon - 1 < 1 || inner.size() - colon - 1 > 2)
        {
            return 1.f;
        }
        if (!std::all_of(inner.begin(), inner.end(), [](char c) { return c == ':' || (c >= '0' && c <= '9'); }))
        {
            return 1.f;
        }
        const S32 w = atoi(inner.substr(0, colon).c_str());
        const S32 h = atoi(inner.substr(colon + 1).c_str());
        return (w > 0 && h > 0) ? (F32)w / (F32)h : 1.f;
    }

    const char* type_group(LLAssetType::EType type)
    {
        switch (type)
        {
            case LLAssetType::AT_OBJECT:      return "objects";
            case LLAssetType::AT_CLOTHING:    return "clothing";
            case LLAssetType::AT_BODYPART:    return "bodyparts";
            case LLAssetType::AT_TEXTURE:     return "textures";
            case LLAssetType::AT_SOUND:       return "sounds";
            case LLAssetType::AT_ANIMATION:   return "animations";
            case LLAssetType::AT_GESTURE:     return "gestures";
            case LLAssetType::AT_LANDMARK:    return "landmarks";
            case LLAssetType::AT_NOTECARD:    return "notecards";
            case LLAssetType::AT_LSL_TEXT:    return "scripts";
            case LLAssetType::AT_MATERIAL:    return "materials";
            case LLAssetType::AT_SETTINGS:    return "settings";
            case LLAssetType::AT_CALLINGCARD: return "callingcards";
            default:                          return "other";
        }
    }

    // A store tag at the start of a name: [Brand], (Brand), {Brand}, <Brand>, «Brand», :: Brand ::, ~Brand~.
    std::string brand_of(const std::string& name)
    {
        static const std::pair<std::string, std::string> TAGS[] = {
            { "[", "]" }, { "(", ")" }, { "{", "}" }, { "<", ">" }, { "\xC2\xAB", "\xC2\xBB" }, { "::", "::" }, { "~", "~" } };
        size_t start = name.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            return std::string();
        }
        for (const auto& [open, close] : TAGS)
        {
            if (name.compare(start, open.size(), open) != 0)
            {
                continue;
            }
            const size_t from = start + open.size();
            const size_t end = name.find(close, from);
            if (end == std::string::npos)
            {
                return std::string();
            }
            std::string brand = name.substr(from, end - from);
            LLStringUtil::trim(brand);
            if (brand.empty() || brand.size() > 32 || (close.size() == 1 && brand.find(close[0]) != std::string::npos))
            {
                return std::string();
            }
            return brand;
        }
        return std::string();
    }

    time_t local_midnight(time_t t)
    {
        tm parts{};
#if LL_WINDOWS
        localtime_s(&parts, &t);
#else
        localtime_r(&t, &parts);
#endif
        parts.tm_hour = 0;
        parts.tm_min = 0;
        parts.tm_sec = 0;
        parts.tm_isdst = -1;
        return mktime(&parts);
    }

    S32 local_year(time_t t)
    {
        tm parts{};
#if LL_WINDOWS
        localtime_s(&parts, &t);
#else
        localtime_r(&t, &parts);
#endif
        return parts.tm_year + 1900;
    }
}

const char* SKInventoryFlatView::columnKey(EColumn col)
{
    return COLUMN_KEYS[llclamp((S32)col, 0, (S32)COL_COUNT - 1)];
}

SKInventoryFlatView::EColumn SKInventoryFlatView::columnFromKey(const std::string& key)
{
    for (S32 i = 0; i < COL_COUNT; ++i)
    {
        if (key == COLUMN_KEYS[i])
        {
            return (EColumn)i;
        }
    }
    return COL_ACQUIRED;
}

const char* SKInventoryFlatView::groupKey(EGroup group)
{
    return GROUP_KEYS[llclamp((S32)group, 0, (S32)GROUP_COUNT - 1)];
}

SKInventoryFlatView::EGroup SKInventoryFlatView::groupFromKey(const std::string& key)
{
    for (S32 i = 0; i < GROUP_COUNT; ++i)
    {
        if (key == GROUP_KEYS[i])
        {
            return (EGroup)i;
        }
    }
    return GROUP_NONE;
}

SKInventoryFlatView::SKInventoryFlatView(const LLUICtrl::Params& p)
:   LLUICtrl(p),
    mNamesArrived(false),
    mGeneration(0),
    mSelectedCount(0),
    mEntryCount(0),
    mFolderCount(0),
    mItemCount(0),
    mCursor(-1),
    mAnchor(-1),
    mSortKey(COL_ACQUIRED),
    mSortAscending(false),
    mGroup(GROUP_NONE),
    mShowAll(false),
    mDirty(false),
    mLoading(false),
    mSearching(false),
    mSlowClickRow(-1),
    mSlowClickArmed(false),
    mRenameRow(-1),
    mPress(PRESS_NONE),
    mPressX(0),
    mPressY(0),
    mPressDown(0),
    mPressRow(-1),
    mPressMask(MASK_NONE),
    mPressCollapse(false),
    mPressToggle(false),
    mLassoOn(false),
    mLassoX(0),
    mLassoY(0),
    mDropRow(-1),
    mScrollbar(nullptr)
{
    const S32 bar = LLScrollbar::THIN_THICKNESS;
    LLScrollbar::Params sbparams;
    sbparams.name("flat_scrollbar");
    sbparams.rect(LLRect(getRect().getWidth() - bar, getRect().getHeight(), getRect().getWidth(), 0));
    sbparams.orientation(LLScrollbar::VERTICAL);
    sbparams.doc_size(0);
    sbparams.doc_pos(0);
    sbparams.page_size(1);
    sbparams.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
    sbparams.visible(false);
    sbparams.thin(true); // drawn over the rows, which keep the full width
    mScrollbar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
    addChild(mScrollbar);
}

SKInventoryFlatView::~SKInventoryFlatView()
{
    // A rename in progress keeps what was typed when the floater closes.
    endRename();
    for (auto& request : mNameRequests)
    {
        request.second.disconnect();
    }
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
}

bool SKInventoryFlatView::readRows(const LLUUID& folder_id, const SKInventoryListLabels& labels, std::vector<SKInventoryRow>& rows, bool fetch)
{
    rows.clear();
    LLViewerInventoryCategory* folder = gInventory.getCategory(folder_id);
    if (!folder)
    {
        return false;
    }

    bool loading = false;
    if (folder->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN || !gInventory.isCategoryComplete(folder_id))
    {
        if (fetch)
        {
            LLInventoryModelBackgroundFetch::instance().start(folder_id, false);
        }
        loading = true;
    }

    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, cats, items);

    if (cats)
    {
        for (LLViewerInventoryCategory* cat : *cats)
        {
            rows.emplace_back();
            readFolderRow(cat, labels, rows.back());
        }
    }
    if (items)
    {
        for (LLViewerInventoryItem* item : *items)
        {
            rows.emplace_back();
            readItemRow(item, labels, rows.back());
        }
    }
    return loading;
}

bool SKInventoryFlatView::readRow(const LLUUID& id, const SKInventoryListLabels& labels, SKInventoryRow& row)
{
    if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
    {
        readFolderRow(cat, labels, row);
        return true;
    }
    if (LLViewerInventoryItem* item = gInventory.getItem(id))
    {
        readItemRow(item, labels, row);
        return true;
    }
    return false;
}

std::string SKInventoryFlatView::brandOf(const std::string& name)
{
    return brand_of(name);
}

void SKInventoryFlatView::readFolderRow(LLViewerInventoryCategory* cat, const SKInventoryListLabels& labels, SKInventoryRow& row)
{
    row.mID = cat->getUUID();
    row.mTargetID = row.mID;
    row.mIsFolder = true;
    row.mAssetType = LLAssetType::AT_CATEGORY;
    row.mIsSystem = LLFolderType::lookupIsProtectedType(cat->getPreferredType());
    row.mName = cat->getName();
    row.mImageID = cat->getThumbnailUUID();
    const S32 count = cat->getDescendentCount();
    if (count >= 0 && !labels.mFolderCount.empty())
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", count);
        row.mType = labels.mFolderCount;
        LLStringUtil::format(row.mType, args);
    }
    else
    {
        row.mType = labels.mFolder;
    }
    row.mIcon = LLUI::getUIImage(LLViewerFolderType::lookupIconName(cat->getPreferredType(), false));
}

void SKInventoryFlatView::readItemRow(LLViewerInventoryItem* item, const SKInventoryListLabels& labels, SKInventoryRow& row)
{
    row.mID = item->getUUID();
    row.mIsLink = item->getIsLinkType();
    row.mTargetID = row.mIsLink ? item->getLinkedUUID() : row.mID;
    row.mName = item->getName();

    LLViewerInventoryCategory* linked_cat = row.mIsLink ? gInventory.getCategory(row.mTargetID) : nullptr;
    if (linked_cat)
    {
        // A folder link lists as the folder it opens.
        row.mIsFolder = true;
        row.mAssetType = LLAssetType::AT_CATEGORY;
        row.mType = labels.mFolder;
        row.mImageID = linked_cat->getThumbnailUUID();
        row.mIcon = LLUI::getUIImage(LLViewerFolderType::lookupIconName(linked_cat->getPreferredType(), false));
        return;
    }

    const LLViewerInventoryItem* target = row.mIsLink ? gInventory.getItem(row.mTargetID) : item;
    const LLViewerInventoryItem* info = target ? target : item;
    row.mLinkBroken = row.mIsLink && !target;
    row.mAssetType = info->getType();
    row.mType = std::string(LLInventoryType::lookupHumanReadable(info->getInventoryType()));
    if (!row.mType.empty())
    {
        row.mType[0] = toupper(row.mType[0]);
    }
    row.mCreatorID = info->getCreatorUUID();
    row.mAcquired = item->getCreationDate();
    if (row.mAcquired > 0 && !labels.mDateFormat.empty())
    {
        row.mAcquiredLabel = labels.mDateFormat;
        LLSD args;
        args["datetime"] = (S32)row.mAcquired;
        LLStringUtil::format(row.mAcquiredLabel, args);
    }
    const U32 mask = info->getPermissions().getMaskOwner();
    row.mPermRank = perm_rank(mask);
    row.mPerms = perm_letters(mask);
    row.mDescription = info->getDescription();
    row.mAspect = aspect_of(row.mDescription);
    // A thumbnail wins; a texture or photo is its own picture.
    row.mImageID = info->getThumbnailUUID();
    if (row.mImageID.isNull() && row.mAssetType == LLAssetType::AT_TEXTURE)
    {
        row.mImageID = info->getAssetUUID();
        row.mImageIsAsset = row.mImageID.notNull();
    }
    row.mIsWorn = get_is_item_worn(row.mTargetID);
    if (row.mIsWorn)
    {
        row.mWornOn = labels.mWorn;
        if (info->getType() == LLAssetType::AT_OBJECT && isAgentAvatarValid())
        {
            const LLViewerJointAttachment* point = gAgentAvatarp->getWornAttachmentPoint(row.mTargetID);
            if (point)
            {
                row.mWornOn = point->getName();
            }
        }
        else if (info->isWearableType())
        {
            row.mWornOn = LLWearableType::getInstance()->getTypeLabel(info->getWearableType());
        }
    }
    row.mIcon = LLInventoryIcon::getIcon(info->getType(), info->getInventoryType(), info->getFlags(), false);
}

void SKInventoryFlatView::sortRows(std::vector<SKInventoryRow>& rows, EColumn key, bool ascending)
{
    auto by_name = [](const SKInventoryRow& a, const SKInventoryRow& b)
    {
        return LLStringUtil::compareDict(a.mName, b.mName) < 0;
    };

    std::stable_sort(rows.begin(), rows.end(), [&](const SKInventoryRow& a, const SKInventoryRow& b)
    {
        if (a.mIsFolder != b.mIsFolder)
        {
            return a.mIsFolder;
        }
        if (a.mIsFolder)
        {
            // Folders have no date: system folders first, then by name.
            if (a.mIsSystem != b.mIsSystem)
            {
                return a.mIsSystem;
            }
            return by_name(a, b);
        }

        S32 order = 0;
        switch (key)
        {
            case COL_FOLDER:      order = LLStringUtil::compareDict(a.mFolderPath, b.mFolderPath); break;
            case COL_TYPE:        order = LLStringUtil::compareDict(a.mType, b.mType); break;
            case COL_CREATOR:     order = LLStringUtil::compareDict(a.mCreatorName, b.mCreatorName); break;
            case COL_ACQUIRED:    order = (a.mAcquired < b.mAcquired) ? -1 : (a.mAcquired > b.mAcquired ? 1 : 0); break;
            case COL_PERMS:       order = a.mPermRank - b.mPermRank; break;
            case COL_WORN:        order = (a.mIsWorn != b.mIsWorn) ? (a.mIsWorn ? -1 : 1) : LLStringUtil::compareDict(a.mWornOn, b.mWornOn); break;
            case COL_DESCRIPTION: order = LLStringUtil::compareDict(a.mDescription, b.mDescription); break;
            default:              order = LLStringUtil::compareDict(a.mName, b.mName); break;
        }
        if (order == 0)
        {
            return by_name(a, b);
        }
        return ascending ? order < 0 : order > 0;
    });
}

void SKInventoryFlatView::groupRows(std::vector<SKInventoryRow>& rows, EGroup group, const SKInventoryListLabels& labels)
{
    if (group == GROUP_NONE)
    {
        return;
    }
    auto label = [&](const std::string& key)
    {
        auto it = labels.mGroups.find(key);
        return it != labels.mGroups.end() ? it->second : key;
    };

    const time_t now = time(nullptr);
    const time_t today = local_midnight(now);
    const S32 this_year = local_year(now);
    std::map<std::string, std::string> brand_case;  // tags differing only in case are one brand, under the first spelling met

    for (SKInventoryRow& row : rows)
    {
        // Except under Brand, folders sit under "Folders" on top.
        if (row.mIsFolder && group != GROUP_BRAND)
        {
            row.mGroup = label("folders");
            row.mGroupRank = -1;
            continue;
        }
        switch (group)
        {
            case GROUP_TYPE:
            {
                const std::string key = type_group(row.mAssetType);
                row.mGroup = label(key);
                row.mGroupRank = (S32)(std::find(std::begin(TYPE_ORDER), std::end(TYPE_ORDER), key) - std::begin(TYPE_ORDER));
                break;
            }
            case GROUP_DATE:
            {
                if (row.mAcquired <= 0)
                {
                    row.mGroup = label("unknown");
                    row.mGroupRank = S32_MAX;
                    break;
                }
                const F64 age = (F64)(today - local_midnight(row.mAcquired)) / 86400.0;
                const S32 year = local_year(row.mAcquired);
                static const char* const RECENT[] = { "today", "yesterday", "this_week", "this_month", "earlier_this_year" };
                S32 rank = age < 1.0 ? 0 : age < 2.0 ? 1 : age < 7.0 ? 2 : age < 31.0 ? 3 : (year == this_year ? 4 : -1);
                if (rank >= 0)
                {
                    row.mGroup = label(RECENT[rank]);
                    row.mGroupRank = rank;
                }
                else
                {
                    row.mGroup = llformat("%d", year);
                    row.mGroupRank = 5 + (3000 - year);
                }
                break;
            }
            case GROUP_BRAND:
            {
                if (row.mIsFolder && row.mIsSystem)
                {
                    row.mGroup = label("system_folders");
                    row.mGroupRank = -1;
                    break;
                }
                const std::string brand = brand_of(row.mName);
                if (brand.empty())
                {
                    row.mGroup = label("no_brand");
                    row.mGroupRank = 1;
                    break;
                }
                std::string lower = brand;
                LLStringUtil::toLower(lower);
                auto it = brand_case.emplace(lower, brand).first;
                row.mGroup = it->second;
                row.mGroupRank = 0;
                break;
            }
            case GROUP_PERMS:
                row.mGroup = label(PERM_KEYS[llclamp(row.mPermRank, 0, 4)]);
                row.mGroupRank = row.mPermRank;
                break;
            case GROUP_WORN:
                row.mGroup = label(row.mIsWorn ? "worn" : "not_worn");
                row.mGroupRank = row.mIsWorn ? 0 : 1;
                break;
            case GROUP_CREATOR:
                if (row.mCreatorID.notNull() && row.mCreatorID == gAgentID)
                {
                    row.mGroup = label("you");
                    row.mGroupRank = 0;
                }
                else
                {
                    row.mGroup = row.mCreatorName.empty() ? label("unknown") : row.mCreatorName;
                    row.mGroupRank = 1;
                }
                break;
            default:
                break;
        }
    }

    std::map<std::string, S32> counts;
    for (const SKInventoryRow& row : rows)
    {
        ++counts[row.mGroup];
    }
    // Group rank, then (Brand) the biggest first, then the heading's name; the sort stays within.
    const bool by_count = group == GROUP_BRAND;
    std::stable_sort(rows.begin(), rows.end(), [&](const SKInventoryRow& a, const SKInventoryRow& b)
    {
        if (a.mGroupRank != b.mGroupRank)
        {
            return a.mGroupRank < b.mGroupRank;
        }
        if (a.mGroup == b.mGroup)
        {
            return false;
        }
        if (by_count)
        {
            const S32 ca = counts[a.mGroup];
            const S32 cb = counts[b.mGroup];
            if (ca != cb)
            {
                return ca > cb;
            }
        }
        return LLStringUtil::compareDict(a.mGroup, b.mGroup) < 0;
    });

    std::vector<SKInventoryRow> grouped;
    grouped.reserve(rows.size() + counts.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (i == 0 || rows[i].mGroup != rows[i - 1].mGroup)
        {
            SKInventoryRow header;
            header.mKind = SKInventoryRow::KIND_HEADER;
            header.mName = rows[i].mGroup;
            header.mGroup = rows[i].mGroup;
            header.mGroupCount = counts[rows[i].mGroup];
            grouped.push_back(std::move(header));
        }
        grouped.push_back(std::move(rows[i]));
    }
    rows = std::move(grouped);
}

void SKInventoryFlatView::setFolder(const LLUUID& folder_id, const uuid_vec_t& selection, S32 scroll)
{
    endRename();
    if (folder_id != mFolderID)
    {
        mShowAll = false;
    }
    mFolderID = folder_id;
    mCursor = -1;
    mAnchor = -1;
    rebuild();
    setSelection(selection);
    mScrollbar->setDocPos(scroll);
}

void SKInventoryFlatView::setSort(EColumn key, bool ascending)
{
    if (key == mSortKey && ascending == mSortAscending)
    {
        return;
    }
    mSortKey = key;
    mSortAscending = ascending;
    const uuid_vec_t selection = getSelection();
    sortAndGroup();
    setSelection(selection);
}

void SKInventoryFlatView::setGroup(EGroup group)
{
    if (group == mGroup)
    {
        return;
    }
    mGroup = group;
    const uuid_vec_t selection = getSelection();
    sortAndGroup();
    setSelection(selection);
}

uuid_vec_t SKInventoryFlatView::getSelection() const
{
    uuid_vec_t ids;
    for (size_t i = 0; i < mRows.size() && i < mSelected.size(); ++i)
    {
        if (mSelected[i])
        {
            ids.push_back(mRows[i].mID);
        }
    }
    return ids;
}

void SKInventoryFlatView::setSelection(const uuid_vec_t& ids)
{
    mSelected.assign(mRows.size(), false);
    mCursor = -1;
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        if (mRows[i].isEntry() && std::find(ids.begin(), ids.end(), mRows[i].mID) != ids.end())
        {
            mSelected[i] = true;
            mCursor = (S32)i;
        }
    }
    mAnchor = mCursor;
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
    if (mCursor >= 0)
    {
        scrollToRow(mCursor);
    }
}

S32 SKInventoryFlatView::getScroll() const
{
    return mScrollbar->getDocPos();
}

void SKInventoryFlatView::drawLinkBadge(const LLRect& icon)
{
    static LLPointer<LLUIImage> badge = LLUI::getUIImage("Inv_Link");
    if (badge.isNull())
    {
        return;
    }
    // Half the icon, at its bottom-right corner.
    const S32 size = llmax(8, icon.getWidth() / 2);
    badge->draw(icon.mRight - size + 2, icon.mBottom - 2, size, size);
}

S32 SKInventoryFlatView::contentWidth() const
{
    const bool scrolls = mScrollbar && mScrollbar->getVisible();
    return llmax(0, getRect().getWidth() - (scrolls ? LLScrollbar::THIN_THICKNESS : 0));
}

void SKInventoryFlatView::setScroll(S32 pos)
{
    mScrollbar->setDocPos(pos);
}

LLUUID SKInventoryFlatView::firstItem() const
{
    for (const SKInventoryRow& row : mRows)
    {
        if (row.isEntry() && !row.mIsFolder)
        {
            return row.mID;
        }
    }
    return LLUUID::null;
}

void SKInventoryFlatView::rebuild()
{
    mDirty = false;
    const uuid_vec_t selection = getSelection();
    const LLUUID cursor_id = (mCursor >= 0 && mCursor < (S32)mRows.size()) ? mRows[mCursor].mID : LLUUID::null;

    if (mSearching)
    {
        readSearchRows();
    }
    else
    {
        mLoading = readRows(mFolderID, mLabels, mAllRows);
    }
    mRowIDs.clear();
    mFolderCount = 0;
    mItemCount = 0;
    for (SKInventoryRow& row : mAllRows)
    {
        (row.mIsFolder ? mFolderCount : mItemCount)++;
        mRowIDs.insert(row.mID);
        requestCreatorName(row);
    }
    ++mGeneration;

    sortAndGroup();
    setSelection(selection);
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        if (mRows[i].isEntry() && mRows[i].mID == cursor_id)
        {
            mCursor = (S32)i;
        }
    }
}

void SKInventoryFlatView::setSearchResults(const std::vector<std::pair<LLUUID, S32>>& hits)
{
    if (!mSearching)
    {
        mShowAll = false;
        mScrollbar->setDocPos(0);
    }
    mSearching = true;
    mSearchHits = hits;
    rebuild();
}

void SKInventoryFlatView::focusContent(bool select_first)
{
    setFocus(true);
    const S32 first = firstEntry();
    if (select_first && mSelectedCount == 0 && first >= 0)
    {
        selectRange(first, first, false);
        mCursor = first;
        mAnchor = first;
        scrollToRow(first);
        notifySelection();
    }
}

void SKInventoryFlatView::centerSelection()
{
    if (mCursor < 0 || mCursor >= (S32)mRows.size())
    {
        return;
    }
    const LLRect area = rowsArea();
    const LLRect row = rowRect(mCursor);
    scrollPixels((area.mTop + area.mBottom) / 2 - (row.mTop + row.mBottom) / 2);
}

void SKInventoryFlatView::clearSearch()
{
    if (!mSearching)
    {
        return;
    }
    mSearching = false;
    mSearchHits.clear();
    mShowAll = false;
    rebuild();
}

const std::string& SKInventoryFlatView::searchPath(const LLUUID& parent_id)
{
    auto found = mSearchPaths.find(parent_id);
    if (found != mSearchPaths.end())
    {
        return found->second;
    }
    std::vector<std::string> names;
    const LLViewerInventoryCategory* cat = gInventory.getCategory(parent_id);
    while (cat && cat->getUUID() != mFolderID)
    {
        names.push_back(cat->getName());
        cat = gInventory.getCategory(cat->getParentUUID());
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        path += (path.empty() ? "" : " \xE2\x80\xBA ") + *it;
    }
    return mSearchPaths.emplace(parent_id, path).first->second;
}

bool SKInventoryFlatView::readSearchRow(const std::pair<LLUUID, S32>& hit, SKInventoryRow& row)
{
    if (!readRow(hit.first, mLabels, row))
    {
        return false;
    }
    row.mSearchRank = hit.second;
    if (const LLInventoryObject* obj = gInventory.getObject(hit.first))
    {
        row.mFolderPath = searchPath(obj->getParentUUID());
    }
    return true;
}

void SKInventoryFlatView::readSearchRows()
{
    mAllRows.clear();
    mLoading = false;
    // Names and places may have changed since the paths were worked out.
    mSearchPaths.clear();
    mAllRows.reserve(mSearchHits.size());
    for (const auto& hit : mSearchHits)
    {
        SKInventoryRow row;
        if (readSearchRow(hit, row))
        {
            mAllRows.push_back(std::move(row));
        }
    }
}

void SKInventoryFlatView::appendSearchResults(const std::vector<std::pair<LLUUID, S32>>& hits)
{
    if (!mSearching)
    {
        setSearchResults(hits);
        return;
    }
    bool listed = false;
    for (const auto& hit : hits)
    {
        SKInventoryRow row;
        if (!readSearchRow(hit, row))
        {
            continue;
        }
        mSearchHits.push_back(hit);
        (row.mIsFolder ? mFolderCount : mItemCount)++;
        mRowIDs.insert(row.mID);
        requestCreatorName(row);
        // Shown at once while the first page has room; the paging row comes with the sort at the end.
        if (mShowAll || mEntryCount < PAGE_ROWS)
        {
            mRows.push_back(row);
            mSelected.push_back(false);
            ++mEntryCount;
            listed = true;
        }
        mAllRows.push_back(std::move(row));
    }
    ++mGeneration;
    if (listed)
    {
        layoutRows();
    }
}

void SKInventoryFlatView::sortAndGroup()
{
    // Headers from an earlier grouping go before sorting again.
    std::erase_if(mAllRows, [](const SKInventoryRow& row) { return !row.isEntry(); });
    sortRows(mAllRows, mSortKey, mSortAscending);
    if (mSearching)
    {
        // Description-only hits list after the name hits.
        std::stable_sort(mAllRows.begin(), mAllRows.end(),
                         [](const SKInventoryRow& a, const SKInventoryRow& b) { return a.mSearchRank < b.mSearchRank; });
    }
    groupRows(mAllRows, mGroup, mLabels);

    // 600 entries, then a row to show the rest; group headers keep their full counts.
    mRows.clear();
    mEntryCount = 0;
    const S32 total = mFolderCount + mItemCount;
    for (const SKInventoryRow& row : mAllRows)
    {
        if (row.isEntry())
        {
            if (!mShowAll && mEntryCount >= PAGE_ROWS)
            {
                break;
            }
            ++mEntryCount;
        }
        else if (!mShowAll && mEntryCount >= PAGE_ROWS)
        {
            break;
        }
        mRows.push_back(row);
    }
    if (!mShowAll && total > PAGE_ROWS)
    {
        SKInventoryRow more;
        more.mKind = SKInventoryRow::KIND_SHOW_ALL;
        LLStringUtil::format_map_t args;
        args["[TOTAL]"] = llformat("%d", total);
        args["[SHOWN]"] = llformat("%d", PAGE_ROWS);
        more.mName = mLabels.mShowAll;
        LLStringUtil::format(more.mName, args);
        mRows.push_back(more);
    }
    mSelected.assign(mRows.size(), false);
    mSelectedCount = 0;
    mCursor = -1;
    mAnchor = -1;
    layoutRows();
    findRenameRow();
}

void SKInventoryFlatView::findRenameRow()
{
    mRenameRow = -1;
    if (!mRenameEditor.get())
    {
        return;
    }
    for (S32 r = 0; r < (S32)mRows.size(); ++r)
    {
        if (mRows[r].isEntry() && mRows[r].mID == mRenameID)
        {
            mRenameRow = r;
            return;
        }
    }
    // The row went: the editor closes without saving.
    SKInventoryRename::cancel(mRenameEditor);
    mRenameEditor.markDead();
}

void SKInventoryFlatView::requestCreatorName(SKInventoryRow& row)
{
    if (row.mCreatorID.isNull())
    {
        return;
    }
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(row.mCreatorID, &av_name))
    {
        row.mCreatorName = av_name.getCompleteName();
    }
    else if (!mNameRequests.contains(row.mCreatorID))
    {
        mNameRequests[row.mCreatorID] = LLAvatarNameCache::get(row.mCreatorID,
            [this](const LLUUID&, const LLAvatarName&) { mNamesArrived = true; });
    }
}

void SKInventoryFlatView::applyArrivedNames()
{
    // Names are taken in batches, at most twice a second, so a burst of replies sorts once.
    if (!mNamesArrived || mNameTimer.getElapsedTimeF32() < 0.5f)
    {
        return;
    }
    mNamesArrived = false;
    mNameTimer.reset();
    bool changed = false;
    for (std::vector<SKInventoryRow>* rows : { &mAllRows, &mRows })
    {
        for (SKInventoryRow& row : *rows)
        {
            LLAvatarName av_name;
            if (row.mCreatorName.empty() && row.mCreatorID.notNull() && LLAvatarNameCache::get(row.mCreatorID, &av_name))
            {
                row.mCreatorName = av_name.getCompleteName();
                changed = true;
                auto request = mNameRequests.find(row.mCreatorID);
                if (request != mNameRequests.end())
                {
                    request->second.disconnect();
                    mNameRequests.erase(request);
                }
            }
        }
    }
    if (changed && (mSortKey == COL_CREATOR || mGroup == GROUP_CREATOR))
    {
        const uuid_vec_t selection = getSelection();
        sortAndGroup();
        setSelection(selection);
    }
}

void SKInventoryFlatView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    if (mScrollbar)
    {
        layoutRows();
    }
}

void SKInventoryFlatView::draw()
{
    if (mDirty)
    {
        rebuild();
    }
    applyArrivedNames();
    if (mSlowClickArmed && mSlowClickTimer.getElapsedTimeF32() >= SLOW_CLICK_SECONDS)
    {
        mSlowClickArmed = false;
        if (mSlowClickRow >= 0 && mSlowClickRow < (S32)mRows.size() && mSelectedCount == 1 && mSelected[mSlowClickRow])
        {
            startRename(mRows[mSlowClickRow].mID);
        }
    }
    // An open editor follows its row through scrolls, resizes and rebuilds.
    if (LLView* editor = mRenameEditor.get())
    {
        if (mRenameRow >= 0 && mRenameRow < (S32)mRows.size())
        {
            bool multiline = false;
            const LLRect rect = nameRect(mRenameRow, multiline);
            if (rect != editor->getRect())
            {
                editor->setShape(rect);
            }
        }
    }
    drawContent();
    drawDropTarget();
    drawLasso();
    LLUICtrl::draw();
}

S32 SKInventoryFlatView::nextEntry(S32 row, S32 step) const
{
    for (S32 i = row + step; i >= 0 && i < (S32)mRows.size(); i += step)
    {
        if (mRows[i].isEntry())
        {
            return i;
        }
    }
    return -1;
}

void SKInventoryFlatView::notifySelection()
{
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
    if (mSelectionCallback)
    {
        mSelectionCallback();
    }
}

void SKInventoryFlatView::selectRange(S32 from, S32 to, bool add)
{
    if (!add)
    {
        mSelected.assign(mRows.size(), false);
    }
    for (S32 i = llmax(0, llmin(from, to)); i <= llmax(from, to) && i < (S32)mRows.size(); ++i)
    {
        mSelected[i] = mRows[i].isEntry();
    }
}

void SKInventoryFlatView::moveCursor(S32 row, MASK mask)
{
    if (row < 0 || row >= (S32)mRows.size() || !mRows[row].isEntry())
    {
        return;
    }
    if ((mask & MASK_SHIFT) && mAnchor >= 0)
    {
        selectRange(mAnchor, row, false);
    }
    else
    {
        selectRange(row, row, false);
        mAnchor = row;
    }
    mCursor = row;
    scrollToRow(row);
    notifySelection();
}

void SKInventoryFlatView::startRename(const LLUUID& id)
{
    endRename();
    mSlowClickArmed = false;
    for (S32 r = 0; r < (S32)mRows.size(); ++r)
    {
        if (mRows[r].isEntry() && mRows[r].mID == id)
        {
            scrollToRow(r);
            bool multiline = false;
            const LLRect rect = nameRect(r, multiline);
            mRenameEditor = SKInventoryRename::start(this, id, rect, multiline, nullptr);
            mRenameID = id;
            mRenameRow = r;
            return;
        }
    }
}

void SKInventoryFlatView::endRename()
{
    SKInventoryRename::finish(mRenameEditor);
    mRenameEditor.markDead();
    mRenameRow = -1;
}

void SKInventoryFlatView::showMenuFromKeyboard()
{
    if (!mMenuCallback)
    {
        return;
    }
    S32 x = 20;
    S32 y = getRect().getHeight() - 20;
    if (mCursor >= 0 && mCursor < (S32)mRows.size() && mSelected[mCursor])
    {
        scrollToRow(mCursor);
        bool multiline = false;
        const LLRect rect = nameRect(mCursor, multiline);
        x = rect.mLeft + 20;
        y = rect.mBottom;
    }
    mMenuCallback(x, y, getSelection());
}

bool SKInventoryFlatView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mSlowClickArmed = false;
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    setFocus(true);
    if (handleHeaderMouseDown(x, y, mask))
    {
        return true;
    }
    if (!rowsArea().pointInRect(x, y))
    {
        return true;
    }
    const S32 row = rowAt(x, y, nullptr);
    if (row >= 0 && mRows[row].mKind == SKInventoryRow::KIND_SHOW_ALL)
    {
        mShowAll = true;
        const uuid_vec_t selection = getSelection();
        sortAndGroup();
        setSelection(selection);
        return true;
    }

    mPressX = x;
    mPressY = y;
    mPressDown = contentDown(y);
    mPressRow = row;
    mPressMask = mask;
    mPressCollapse = false;
    mPressToggle = false;
    mLassoOn = false;
    gFocusMgr.setMouseCapture(this);
    if (row >= 0 && mRows[row].isEntry() && isDragHandle(x, y, row))
    {
        make_ui_sound("UISndClick");
        mPress = PRESS_ITEM;
        if (mSelected[row] && (mask & MASK_CONTROL))
        {
            mPressToggle = true;
        }
        else if (mSelected[row] && !(mask & (MASK_CONTROL | MASK_SHIFT)))
        {
            // The selection stays whole so it can travel; a click without a drag selects the row alone.
            mPressCollapse = mSelectedCount > 1;
            bool on_name = false;
            rowAt(x, y, &on_name);
            if (mSelectedCount == 1 && on_name)
            {
                // A second, slow click on the only selected item's name renames it.
                mSlowClickRow = row;
                mSlowClickArmed = true;
                mSlowClickTimer.reset();
            }
        }
        else
        {
            clickRow(row, mask);
        }
        return true;
    }
    if (row >= 0 && mRows[row].isEntry())
    {
        make_ui_sound("UISndClick");
    }
    // Anywhere else a press draws a selection rectangle once it moves, or is a click when it doesn't.
    mPress = PRESS_LASSO;
    return true;
}

void SKInventoryFlatView::clickRow(S32 row, MASK mask)
{
    if (mask & MASK_CONTROL)
    {
        mSelected[row] = !mSelected[row];
        mAnchor = row;
        mCursor = row;
        notifySelection();
    }
    else
    {
        moveCursor(row, mask);
    }
}

bool SKInventoryFlatView::handleHover(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture() || mPress == PRESS_NONE)
    {
        return LLUICtrl::handleHover(x, y, mask);
    }
    const S32 dx = x - mPressX;
    const S32 dy = y - mPressY;
    if (mPress == PRESS_ITEM)
    {
        if (dx * dx + dy * dy >= SKInventoryExplorerDrag::DRAG_THRESHOLD * SKInventoryExplorerDrag::DRAG_THRESHOLD)
        {
            const uuid_vec_t ids = getSelection();
            mPress = PRESS_NONE;
            mSlowClickArmed = false;
            gFocusMgr.setMouseCapture(nullptr);
            if (SKInventoryExplorerDrag::begin(ids))
            {
                return LLToolDragAndDrop::getInstance()->handleHover(x, y, mask);
            }
        }
        return true;
    }
    if (!mLassoOn && llabs(dx) + llabs(dy) >= SKInventoryExplorerDrag::LASSO_THRESHOLD)
    {
        startLasso();
    }
    if (mLassoOn)
    {
        updateLasso(x, y);
        // Near the top or bottom edge, or past it, the content scrolls under the rectangle.
        const LLRect area = rowsArea();
        const S32 band = SKInventoryExplorerDrag::SCROLL_BAND;
        S32 step = 0;
        if (y > area.mTop - band)
        {
            step = -llmin(30, y - (area.mTop - band));
        }
        else if (y < area.mBottom + band)
        {
            step = llmin(30, area.mBottom + band - y);
        }
        if (step != 0 && mLassoTick.getElapsedTimeF32() >= 0.03f)
        {
            mLassoTick.reset();
            scrollPixels(step);
            updateLasso(x, y);
        }
    }
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

void SKInventoryFlatView::startLasso()
{
    mLassoOn = true;
    mSlowClickArmed = false;
    endRename();
    mLassoRestore = mSelected;
    if (mPressMask & (MASK_CONTROL | MASK_SHIFT))
    {
        mLassoBase = mSelected;
    }
    else
    {
        mLassoBase.assign(mRows.size(), false);
        mSelected.assign(mRows.size(), false);
        mSelectedCount = 0;
    }
    mLassoSwept.clear();
    mLassoTick.reset();
}

void SKInventoryFlatView::updateLasso(S32 x, S32 y)
{
    mLassoX = x;
    mLassoY = y;
    const S32 down = contentDown(y);
    std::vector<S32> rows;
    rowsInBand(llmin(mPressDown, down), llmax(mPressDown, down), llmin(mPressX, x), llmax(mPressX, x), rows);
    // Only the rows entering or leaving the rectangle change, so a sweep costs what it covers.
    for (S32 r : mLassoSwept)
    {
        if (r < (S32)mSelected.size() && mSelected[r] != mLassoBase[r])
        {
            mSelected[r] = mLassoBase[r];
            mSelectedCount += mSelected[r] ? 1 : -1;
        }
    }
    mLassoSwept.clear();
    for (S32 r : rows)
    {
        if (r >= 0 && r < (S32)mRows.size() && r < (S32)mLassoBase.size() && mRows[r].isEntry())
        {
            mLassoSwept.push_back(r);
            if (mSelected[r] == mLassoBase[r])
            {
                mSelected[r] = !mLassoBase[r];
                mSelectedCount += mSelected[r] ? 1 : -1;
            }
        }
    }
    if (mSelectionCallback)
    {
        mSelectionCallback();
    }
}

void SKInventoryFlatView::endPress(bool cancel)
{
    const EPress press = mPress;
    mPress = PRESS_NONE;
    if (press == PRESS_LASSO && mLassoOn)
    {
        mLassoOn = false;
        if (cancel && mLassoRestore.size() == mRows.size())
        {
            mSelected = mLassoRestore;
        }
        else if (!mLassoSwept.empty())
        {
            mCursor = mAnchor = mLassoSwept.back();
        }
        notifySelection();
    }
    mLassoOn = false;
    mLassoSwept.clear();
}

bool SKInventoryFlatView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return LLUICtrl::handleMouseUp(x, y, mask);
    }
    const EPress press = mPress;
    const bool lasso = mLassoOn;
    endPress(false);
    gFocusMgr.setMouseCapture(nullptr);
    if (press == PRESS_ITEM && mPressToggle && mPressRow >= 0 && mPressRow < (S32)mRows.size())
    {
        clickRow(mPressRow, mPressMask);
    }
    else if (press == PRESS_ITEM && mPressCollapse && mPressRow >= 0 && mPressRow < (S32)mRows.size())
    {
        moveCursor(mPressRow, MASK_NONE);
    }
    else if (press == PRESS_LASSO && !lasso)
    {
        if (mPressRow >= 0 && mPressRow < (S32)mRows.size() && mRows[mPressRow].isEntry())
        {
            clickRow(mPressRow, mPressMask);
        }
        else if (!(mPressMask & (MASK_CONTROL | MASK_SHIFT)))
        {
            // A click on empty space clears the selection.
            mSelected.assign(mRows.size(), false);
            mCursor = -1;
            mAnchor = -1;
            notifySelection();
        }
    }
    return true;
}

void SKInventoryFlatView::onMouseCaptureLost()
{
    endPress(false);
}

void SKInventoryFlatView::drawLasso()
{
    if (!mLassoOn)
    {
        return;
    }
    static LLUIColor accent = LLUIColorTable::instance().getColor("EmphasisColor");
    LLColor4 fill = accent.get();
    fill.mV[VALPHA] = 0.15f;
    const S32 top = contentToLocal(mPressDown);
    LLRect rect(llmin(mPressX, mLassoX), llmax(top, mLassoY), llmax(mPressX, mLassoX), llmin(top, mLassoY));
    rect.intersectWith(rowsArea());
    if (rect.isEmpty())
    {
        return;
    }
    gl_rect_2d(rect, fill, true);
    gl_rect_2d(rect, accent.get(), false);
}

void SKInventoryFlatView::drawDropTarget()
{
    if (!mDropHover.isActive())
    {
        return;
    }
    LLLocalClipRect clip(rowsArea());
    if (mDropRow >= 0 && mDropRow < (S32)mRows.size())
    {
        SKInventoryExplorerDrag::drawTargetMark(rowRect(mDropRow), mDropVerdict);
    }
    else if (mDropVerdict.mOK)
    {
        // The open folder's own space takes the outline alone.
        static LLUIColor accent = LLUIColorTable::instance().getColor("EmphasisColor");
        const LLRect area = rowsArea();
        gl_rect_2d(area.mLeft + 1, area.mTop - 1, area.mRight - 1, area.mBottom + 1, accent.get(), false);
        gl_rect_2d(area.mLeft + 2, area.mTop - 2, area.mRight - 2, area.mBottom + 2, accent.get(), false);
    }
}

bool SKInventoryFlatView::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                            EAcceptance* accept, std::string& tooltip_msg)
{
    if (childrenHandleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg))
    {
        return true;
    }
    const S32 row = rowAt(x, y, nullptr);
    const bool on_folder = row >= 0 && mRows[row].isEntry() && mRows[row].mIsFolder;
    const LLUUID dest = on_folder ? mRows[row].mTargetID : mFolderID;
    if (!on_folder && mSearching)
    {
        // Results are not a folder: a folder row among them still takes the drop.
        SKInventoryExplorerDrag::refuse("drag_into_search", accept, tooltip_msg);
    }
    else
    {
        SKInventoryExplorerDrag::dropOnFolder(dest, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }
    mDropRow = on_folder ? row : -1;
    mDropVerdict = SKInventoryExplorerDrag::verdict();
    if (!on_folder && mDropVerdict.mQuiet)
    {
        tooltip_msg.clear(); // no label over the open folder's own space
    }
    if (drop || LLToolDragAndDrop::getInstance()->getCargoIndex() != 0)
    {
        return true;
    }
    mDropHover.touch();

    // Once a frame: spring a folder held under the pointer, and scroll near the top and bottom.
    S32 screen_x = 0;
    S32 screen_y = 0;
    localPointToScreen(x, y, &screen_x, &screen_y);
    const LLUUID spring_id = on_folder && !mDropVerdict.mOntoCargo ? mRows[row].mID : LLUUID::null;
    if (mDropHover.spring(spring_id, screen_x, screen_y, SKInventoryExplorerDrag::SPRING_SECONDS) && mOpenCallback)
    {
        mOpenCallback(spring_id);
        return true;
    }
    const LLRect area = rowsArea();
    S32 step = 0;
    if (y > area.mTop - SKInventoryExplorerDrag::SCROLL_BAND)
    {
        step = -SKInventoryExplorerDrag::SCROLL_STEP;
    }
    else if (y < area.mBottom + SKInventoryExplorerDrag::SCROLL_BAND)
    {
        step = SKInventoryExplorerDrag::SCROLL_STEP;
    }
    if (step != 0 && mDropHover.tick(SKInventoryExplorerDrag::TICK_SECONDS))
    {
        const S32 before = contentDown(y);
        scrollPixels(step);
        if (contentDown(y) != before)
        {
            mDropHover.scrolled(screen_x, screen_y);
        }
    }
    return true;
}

void SKInventoryFlatView::undo()
{
    SKInventoryExplorerDrag::undo();
}

bool SKInventoryFlatView::canUndo() const
{
    return SKInventoryExplorerDrag::canUndo();
}

bool SKInventoryFlatView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    mSlowClickArmed = false;
    if (childrenHandleDoubleClick(x, y, mask))
    {
        return true;
    }
    const S32 row = rowAt(x, y, nullptr);
    if (row >= 0 && mRows[row].isEntry() && mOpenCallback)
    {
        mOpenCallback(mRows[row].mID);
    }
    return true;
}

bool SKInventoryFlatView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    mSlowClickArmed = false;
    if (childrenHandleRightMouseDown(x, y, mask))
    {
        return true;
    }
    setFocus(true);
    if (handleHeaderRightMouseDown(x, y, mask))
    {
        return true;
    }
    const S32 row = rowAt(x, y, nullptr);
    if (row < 0 || !mRows[row].isEntry())
    {
        // Empty space clears the selection and gives the open folder's menu.
        mSelected.assign(mRows.size(), false);
        mCursor = -1;
        notifySelection();
    }
    else if (!mSelected[row])
    {
        moveCursor(row, MASK_NONE);
    }
    if (row >= 0 && mRows[row].isEntry())
    {
        make_ui_sound("UISndClick");
    }
    if (mMenuCallback)
    {
        mMenuCallback(x, y, getSelection());
    }
    return true;
}

bool SKInventoryFlatView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    return mScrollbar->handleScrollWheel(0, 0, delta);
}

bool SKInventoryFlatView::handleKeyHere(KEY key, MASK mask)
{
    mSlowClickArmed = false;
    if (mask & MASK_ALT)
    {
        return false;
    }
    switch (key)
    {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_PAGE_UP:
        case KEY_PAGE_DOWN:
        case KEY_HOME:
        case KEY_END:
        {
            S32 row = -1;
            if ((mask & MASK_CONTROL) && (key == KEY_UP || key == KEY_HOME))
            {
                row = firstEntry();
            }
            else if ((mask & MASK_CONTROL) && (key == KEY_DOWN || key == KEY_END))
            {
                row = lastEntry();
            }
            else if (key == KEY_HOME)
            {
                row = firstEntry();
            }
            else if (key == KEY_END)
            {
                row = lastEntry();
            }
            else
            {
                row = rowFromKey(key, mask);
            }
            if (row >= 0)
            {
                moveCursor(row, mask & MASK_SHIFT);
            }
            return true;
        }
        case KEY_RETURN:
            if (mask == MASK_NONE && mCursor >= 0 && mSelected[mCursor] && mOpenCallback)
            {
                mOpenCallback(mRows[mCursor].mID);
                return true;
            }
            break;
        case KEY_BACKSPACE:
            if (mask == MASK_NONE && mUpCallback)
            {
                mUpCallback();
                return true;
            }
            break;
        case KEY_ESCAPE:
            if (mLassoOn)
            {
                endPress(true);
                gFocusMgr.setMouseCapture(nullptr);
                return true;
            }
            if (mEscapeCallback)
            {
                mEscapeCallback();
                return true;
            }
            if (mSelectedCount > 0)
            {
                deselect();
                return true;
            }
            break;
        case KEY_DELETE:
            if (canDoDelete())
            {
                doDelete();
            }
            return true;
        case KEY_F2:
        {
            const uuid_vec_t selection = getSelection();
            if (selection.size() == 1)
            {
                startRename(selection.front());
            }
            return true;
        }
        case KEY_F10:
            if (mask == MASK_SHIFT)
            {
                showMenuFromKeyboard();
                return true;
            }
            break;
        case KEY_CONTEXT_MENU:
            showMenuFromKeyboard();
            return true;
        case 'A':
            if (mask == MASK_CONTROL)
            {
                selectAll();
                return true;
            }
            break;
        case 'N':
            if (mask == (MASK_CONTROL | MASK_SHIFT))
            {
                SKInventoryExplorerActions::create(mFolderID, "category");
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void SKInventoryFlatView::onFocusReceived()
{
    gEditMenuHandler = this;
    LLUICtrl::onFocusReceived();
}

void SKInventoryFlatView::onFocusLost()
{
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
    mSlowClickArmed = false;
    LLUICtrl::onFocusLost();
}

void SKInventoryFlatView::cut()
{
    SKInventoryExplorerActions::cut(getSelection());
}

bool SKInventoryFlatView::canCut() const
{
    return SKInventoryExplorerActions::canCut(getSelection());
}

void SKInventoryFlatView::copy()
{
    SKInventoryExplorerActions::copy(getSelection());
}

bool SKInventoryFlatView::canCopy() const
{
    return SKInventoryExplorerActions::canCopy(getSelection());
}

void SKInventoryFlatView::paste()
{
    // Into a single selected folder, else into the open one.
    const uuid_vec_t selection = getSelection();
    const LLUUID folder = selection.size() == 1 ? SKInventoryExplorerActions::folderOf(selection.front()) : LLUUID::null;
    SKInventoryExplorerActions::paste(folder.notNull() ? folder : mFolderID);
}

bool SKInventoryFlatView::canPaste() const
{
    const uuid_vec_t selection = getSelection();
    const LLUUID folder = selection.size() == 1 ? SKInventoryExplorerActions::folderOf(selection.front()) : LLUUID::null;
    return SKInventoryExplorerActions::canPaste(folder.notNull() ? folder : mFolderID);
}

void SKInventoryFlatView::doDelete()
{
    SKInventoryExplorerActions::deleteObjects(getSelection());
}

bool SKInventoryFlatView::canDoDelete() const
{
    return SKInventoryExplorerActions::canDelete(getSelection(), mFolderID);
}

void SKInventoryFlatView::selectAll()
{
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        mSelected[i] = mRows[i].isEntry();
    }
    notifySelection();
}

void SKInventoryFlatView::deselect()
{
    mSelected.assign(mRows.size(), false);
    mCursor = -1;
    mAnchor = -1;
    notifySelection();
}
