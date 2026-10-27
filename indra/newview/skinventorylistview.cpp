/**
 * @file skinventorylistview.cpp
 * @brief Inventory Explorer's List view: one widget drawing a folder's rows from a flat model
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

#include "skinventorylistview.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llfontgl.h"
#include "llinventoryfunctions.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "llui.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "llwearabletype.h"
#include "skinventoryexplorermenu.h"

#include <algorithm>

static LLDefaultChildRegistry::Register<SKInventoryListView> r_sk_inventory_list("sk_inventory_list");

namespace
{
    constexpr S32 ROW_HEIGHT = 20;
    constexpr S32 HEADER_HEIGHT = 20;
    constexpr S32 CELL_PAD = 4;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 DEFAULT_WIDTHS[SKInventoryListView::COL_COUNT] = { 220, 110, 130, 120, 50, 100, 220 };
    const char* const COLUMN_KEYS[SKInventoryListView::COL_COUNT] = { "name", "type", "creator", "acquired", "perms", "worn", "description" };

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
}

const char* SKInventoryListView::columnKey(EColumn col)
{
    return COLUMN_KEYS[llclamp((S32)col, 0, (S32)COL_COUNT - 1)];
}

SKInventoryListView::EColumn SKInventoryListView::columnFromKey(const std::string& key)
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

SKInventoryListView::SKInventoryListView(const Params& p)
:   LLUICtrl(p),
    mFolderCount(0),
    mCursor(-1),
    mAnchor(-1),
    mSortKey(COL_ACQUIRED),
    mSortAscending(false),
    mHeaderClicks(0),
    mDirty(false),
    mLoading(false),
    mNamesArrived(false),
    mGeneration(0),
    mSelectedCount(0),
    mScrollbar(nullptr)
{
    std::copy(std::begin(DEFAULT_WIDTHS), std::end(DEFAULT_WIDTHS), mColumnWidths);

    const S32 bar = LLUI::getInstance()->mSettingGroups["config"]->getS32("UIScrollbarSize");
    LLRect bar_rect(getRect().getWidth() - bar, getRect().getHeight() - HEADER_HEIGHT, getRect().getWidth(), 0);
    LLScrollbar::Params sbparams;
    sbparams.name("list_scrollbar");
    sbparams.rect(bar_rect);
    sbparams.orientation(LLScrollbar::VERTICAL);
    sbparams.doc_size(0);
    sbparams.doc_pos(0);
    sbparams.page_size(1);
    sbparams.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
    sbparams.visible(false);
    mScrollbar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
    addChild(mScrollbar);
}

SKInventoryListView::~SKInventoryListView()
{
    for (auto& request : mNameRequests)
    {
        request.second.disconnect();
    }
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
}

void SKInventoryListView::setFolder(const LLUUID& folder_id, const uuid_vec_t& selection, S32 scroll)
{
    mFolderID = folder_id;
    mCursor = -1;
    mAnchor = -1;
    rebuild();
    setSelection(selection);
    mScrollbar->setDocPos(scroll);
}

void SKInventoryListView::setSort(EColumn key, bool ascending)
{
    const uuid_vec_t selection = getSelection();
    mSortKey = key;
    mSortAscending = ascending;
    sortRows();
    setSelection(selection);
}

uuid_vec_t SKInventoryListView::getSelection() const
{
    uuid_vec_t ids;
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        if (mSelected[i])
        {
            ids.push_back(mRows[i].mID);
        }
    }
    return ids;
}

void SKInventoryListView::setSelection(const uuid_vec_t& ids)
{
    mSelected.assign(mRows.size(), false);
    mCursor = -1;
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        if (std::find(ids.begin(), ids.end(), mRows[i].mID) != ids.end())
        {
            mSelected[i] = true;
            mCursor = (S32)i;
        }
    }
    mAnchor = mCursor;
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
}

S32 SKInventoryListView::getScroll() const
{
    return mScrollbar->getDocPos();
}

bool SKInventoryListView::readRows(const LLUUID& folder_id, const SKInventoryListLabels& labels, std::vector<SKInventoryRow>& rows,
                                   bool fetch)
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
            SKInventoryRow row;
            row.mID = cat->getUUID();
            row.mTargetID = row.mID;
            row.mIsFolder = true;
            row.mIsSystem = LLFolderType::lookupIsProtectedType(cat->getPreferredType());
            row.mName = cat->getName();
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
            rows.push_back(row);
        }
    }

    if (items)
    {
        for (LLViewerInventoryItem* item : *items)
        {
            SKInventoryRow row;
            row.mID = item->getUUID();
            row.mIsLink = item->getIsLinkType();
            row.mTargetID = row.mIsLink ? item->getLinkedUUID() : row.mID;
            row.mName = item->getName();

            LLViewerInventoryCategory* linked_cat = row.mIsLink ? gInventory.getCategory(row.mTargetID) : nullptr;
            if (linked_cat)
            {
                // A folder link lists as the folder it opens.
                row.mIsFolder = true;
                row.mType = labels.mFolder;
                row.mIcon = LLUI::getUIImage(LLViewerFolderType::lookupIconName(linked_cat->getPreferredType(), false));
                rows.push_back(row);
                continue;
            }

            const LLViewerInventoryItem* target = row.mIsLink ? gInventory.getItem(row.mTargetID) : item;
            const LLViewerInventoryItem* info = target ? target : item;
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
            rows.push_back(row);
        }
    }
    return loading;
}

void SKInventoryListView::rebuild()
{
    mDirty = false;
    const uuid_vec_t selection = getSelection();
    const LLUUID cursor_id = (mCursor >= 0 && mCursor < (S32)mRows.size()) ? mRows[mCursor].mID : LLUUID::null;

    mFolderCount = 0;
    mLoading = readRows(mFolderID, mLabels, mRows);

    mRowIDs.clear();
    for (SKInventoryRow& row : mRows)
    {
        mFolderCount += row.mIsFolder ? 1 : 0;
        mRowIDs.insert(row.mID);
        requestCreatorName(row);
    }
    ++mGeneration;

    sortRows();
    setSelection(selection);
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        if (mRows[i].mID == cursor_id)
        {
            mCursor = (S32)i;
        }
    }
    updateScrollbar();
}

void SKInventoryListView::requestCreatorName(SKInventoryRow& row)
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

void SKInventoryListView::applyArrivedNames()
{
    // Names are taken in batches, at most twice a second, so a burst of replies sorts once.
    if (!mNamesArrived || mNameTimer.getElapsedTimeF32() < 0.5f)
    {
        return;
    }
    mNamesArrived = false;
    mNameTimer.reset();
    for (SKInventoryRow& row : mRows)
    {
        LLAvatarName av_name;
        if (row.mCreatorName.empty() && row.mCreatorID.notNull() && LLAvatarNameCache::get(row.mCreatorID, &av_name))
        {
            row.mCreatorName = av_name.getCompleteName();
            auto request = mNameRequests.find(row.mCreatorID);
            if (request != mNameRequests.end())
            {
                request->second.disconnect();
                mNameRequests.erase(request);
            }
        }
    }
    if (mSortKey == COL_CREATOR)
    {
        setSort(mSortKey, mSortAscending);
    }
}

void SKInventoryListView::sortRows()
{
    sortRows(mRows, mSortKey, mSortAscending);
    mSelected.assign(mRows.size(), false);
    mSelectedCount = 0;
}

void SKInventoryListView::sortRows(std::vector<SKInventoryRow>& rows, EColumn key, bool ascending)
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

S32 SKInventoryListView::rowsPerPage() const
{
    return llmax(1, (getRect().getHeight() - HEADER_HEIGHT) / ROW_HEIGHT);
}

S32 SKInventoryListView::firstVisibleRow() const
{
    return mScrollbar->getDocPos();
}

void SKInventoryListView::updateScrollbar()
{
    mScrollbar->setDocSize((S32)mRows.size());
    mScrollbar->setPageSize(rowsPerPage());
    mScrollbar->setVisible((S32)mRows.size() > rowsPerPage());
    mScrollbar->setDocPos(mScrollbar->getDocPos());
}

void SKInventoryListView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    if (mScrollbar)
    {
        updateScrollbar();
    }
}

void SKInventoryListView::scrollToRow(S32 row)
{
    const S32 first = firstVisibleRow();
    const S32 page = rowsPerPage();
    if (row < first)
    {
        mScrollbar->setDocPos(row);
    }
    else if (row >= first + page)
    {
        mScrollbar->setDocPos(row - page + 1);
    }
}

S32 SKInventoryListView::rowAt(S32 y) const
{
    const S32 below_header = getRect().getHeight() - HEADER_HEIGHT - y;
    if (below_header < 0)
    {
        return -2; // the header
    }
    const S32 row = firstVisibleRow() + below_header / ROW_HEIGHT;
    return row < (S32)mRows.size() ? row : -1;
}

S32 SKInventoryListView::columnAt(S32 x) const
{
    S32 left = 0;
    for (S32 i = 0; i < COL_COUNT; ++i)
    {
        if (x >= left && x < left + mColumnWidths[i])
        {
            return i;
        }
        left += mColumnWidths[i];
    }
    return -1;
}

std::string SKInventoryListView::cellText(const SKInventoryRow& row, EColumn col) const
{
    switch (col)
    {
        case COL_NAME:        return row.mName;
        case COL_TYPE:        return row.mType;
        case COL_CREATOR:     return row.mCreatorName;
        case COL_ACQUIRED:    return row.mAcquiredLabel;
        case COL_PERMS:       return row.mPerms;
        case COL_WORN:        return row.mWornOn;
        case COL_DESCRIPTION: return row.mDescription;
        default:              return std::string();
    }
}

void SKInventoryListView::draw()
{
    if (mDirty)
    {
        rebuild();
    }
    applyArrivedNames();

    static LLUIColor sel_focus = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static LLUIColor sel_blur = LLUIColorTable::instance().getColor("LtGray_35");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor header_bg = LLUIColorTable::instance().getColor("DkGray2");
    static LLUIColor rule_color = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor worn_color = LLUIColorTable::instance().getColor("White");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 width = getRect().getWidth() - (mScrollbar->getVisible() ? mScrollbar->getRect().getWidth() : 0);
    const S32 height = getRect().getHeight();

    // Header, with the sort direction on the sorted column.
    gl_rect_2d(0, height, width, height - HEADER_HEIGHT, header_bg.get());
    S32 left = 0;
    for (S32 i = 0; i < COL_COUNT && left < width; ++i)
    {
        std::string label = i < (S32)mLabels.mColumns.size() ? mLabels.mColumns[i] : std::string(COLUMN_KEYS[i]);
        if (i == mSortKey)
        {
            label += mSortAscending ? " \xE2\x96\xB4" : " \xE2\x96\xBE";
        }
        const S32 cell = llmin(mColumnWidths[i], width - left);
        font->renderUTF8(label, 0, (F32)(left + CELL_PAD), (F32)(height - HEADER_HEIGHT / 2), text_color.get(),
                         LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX,
                         cell - 2 * CELL_PAD, nullptr, true);
        left += mColumnWidths[i];
    }
    gl_line_2d(0, height - HEADER_HEIGHT, width, height - HEADER_HEIGHT, rule_color.get());

    if (mRows.empty())
    {
        const std::string& message = mLoading ? mLabels.mLoading : mLabels.mEmpty;
        font->renderUTF8(message, 0, width / 2, height - HEADER_HEIGHT - 2 * ROW_HEIGHT, dim_color.get(),
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    // Only the rows in view are drawn.
    const bool focused = hasFocus();
    const S32 first = firstVisibleRow();
    const S32 last = llmin((S32)mRows.size(), first + rowsPerPage() + 1);
    LLLocalClipRect clip(LLRect(0, height - HEADER_HEIGHT, width, 0));
    for (S32 r = first; r < last; ++r)
    {
        const SKInventoryRow& row = mRows[r];
        const S32 top = height - HEADER_HEIGHT - (r - first) * ROW_HEIGHT;
        const S32 middle = top - ROW_HEIGHT / 2;
        if (mSelected[r])
        {
            gl_rect_2d(0, top, width, top - ROW_HEIGHT, focused ? sel_focus.get() : sel_blur.get());
        }

        left = 0;
        for (S32 i = 0; i < COL_COUNT && left < width; ++i)
        {
            S32 x = left + CELL_PAD;
            const S32 cell = llmin(mColumnWidths[i], width - left);
            if (i == COL_NAME && row.mIcon.notNull())
            {
                row.mIcon->draw(x, middle - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE);
                x += ICON_SIZE + CELL_PAD;
            }
            const std::string text = cellText(row, (EColumn)i);
            if (!text.empty())
            {
                LLColor4 color = text_color.get();
                U8 style = LLFontGL::NORMAL;
                if (i == COL_NAME && row.mIsLink)
                {
                    color = dim_color.get();
                    style = LLFontGL::ITALIC;
                }
                else if (i == COL_WORN)
                {
                    color = worn_color.get();
                    style = LLFontGL::BOLD;
                }
                else if (i == COL_DESCRIPTION || i == COL_TYPE)
                {
                    color = dim_color.get();
                }
                font->renderUTF8(text, 0, (F32)x, (F32)middle, color, LLFontGL::LEFT, LLFontGL::VCENTER, style,
                                 LLFontGL::NO_SHADOW, S32_MAX, llmax(0, left + cell - CELL_PAD - x), nullptr, true);
            }
            left += mColumnWidths[i];
        }
    }

    LLUICtrl::draw();
}

void SKInventoryListView::notifySelection()
{
    mSelectedCount = (S32)std::count(mSelected.begin(), mSelected.end(), true);
    if (mSelectionCallback)
    {
        mSelectionCallback();
    }
}

void SKInventoryListView::selectRange(S32 from, S32 to, bool add)
{
    if (!add)
    {
        mSelected.assign(mRows.size(), false);
    }
    for (S32 i = llmin(from, to); i <= llmax(from, to); ++i)
    {
        if (i >= 0 && i < (S32)mRows.size())
        {
            mSelected[i] = true;
        }
    }
}

void SKInventoryListView::moveCursor(S32 row, MASK mask)
{
    if (mRows.empty())
    {
        return;
    }
    row = llclamp(row, 0, (S32)mRows.size() - 1);
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

bool SKInventoryListView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const S32 row = rowAt(y);
    if (row == -2)
    {
        // A header click sorts by that column; again flips it.
        const S32 col = columnAt(x);
        if (col >= 0)
        {
            // Each header cycles: its first direction, flipped, then back to the default sort.
            if (col != mSortKey)
            {
                mHeaderClicks = 1;
                setSort((EColumn)col, col != COL_ACQUIRED);
            }
            else if (mHeaderClicks < 2)
            {
                mHeaderClicks = 2;
                setSort(mSortKey, !mSortAscending);
            }
            else
            {
                mHeaderClicks = 0;
                setSort(COL_ACQUIRED, false);
            }
            if (mSortCallback)
            {
                mSortCallback();
            }
        }
        return true;
    }
    if (row < 0)
    {
        mSelected.assign(mRows.size(), false);
        mCursor = -1;
        mAnchor = -1;
        notifySelection();
        return true;
    }

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
    return true;
}

bool SKInventoryListView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row >= 0 && mOpenCallback)
    {
        mOpenCallback(mRows[row].mID);
    }
    return true;
}

bool SKInventoryListView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const S32 row = rowAt(y);
    if (row == -2)
    {
        return true;
    }
    if (row < 0)
    {
        mSelected.assign(mRows.size(), false);
        mCursor = -1;
        notifySelection();
    }
    else if (!mSelected[row])
    {
        moveCursor(row, MASK_NONE);
    }
    if (mMenuCallback)
    {
        mMenuCallback(x, y, getSelection());
    }
    return true;
}

bool SKInventoryListView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    return mScrollbar->handleScrollWheel(0, 0, delta);
}

bool SKInventoryListView::handleKeyHere(KEY key, MASK mask)
{
    if (mask & MASK_ALT)
    {
        return false;
    }
    const S32 page = rowsPerPage();
    const S32 last = (S32)mRows.size() - 1;
    switch (key)
    {
        case KEY_UP:
            // Ctrl jumps to the first row, as Home does.
            moveCursor((mask & MASK_CONTROL) || mCursor < 0 ? 0 : mCursor - 1, mask);
            return true;
        case KEY_DOWN:
            moveCursor((mask & MASK_CONTROL) ? last : (mCursor < 0 ? 0 : mCursor + 1), mask);
            return true;
        case KEY_HOME:
            moveCursor(0, mask);
            return true;
        case KEY_END:
            moveCursor(last, mask);
            return true;
        case KEY_PAGE_UP:
            moveCursor(mCursor < 0 ? 0 : mCursor - page, mask);
            return true;
        case KEY_PAGE_DOWN:
            moveCursor(mCursor < 0 ? 0 : mCursor + page, mask);
            return true;
        case KEY_LEFT:
        case KEY_RIGHT:
            return true; // the List has no sideways movement
        case KEY_RETURN:
            if (mask == MASK_NONE && mCursor >= 0 && mCursor <= last && mOpenCallback)
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
                SKInventoryExplorerActions::rename(selection.front());
            }
            return true;
        }
        case 'A':
            if (mask == MASK_CONTROL)
            {
                selectAll();
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

bool SKInventoryListView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    const S32 col = columnAt(x);
    if (row >= 0 && col >= 0)
    {
        const std::string text = cellText(mRows[row], (EColumn)col);
        if (!text.empty())
        {
            LLToolTipMgr::instance().show(text);
            return true;
        }
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

void SKInventoryListView::onFocusReceived()
{
    gEditMenuHandler = this;
    LLUICtrl::onFocusReceived();
}

void SKInventoryListView::onFocusLost()
{
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
    LLUICtrl::onFocusLost();
}

void SKInventoryListView::doDelete()
{
    SKInventoryExplorerActions::deleteObjects(getSelection());
}

bool SKInventoryListView::canDoDelete() const
{
    return SKInventoryExplorerActions::canDelete(getSelection(), mFolderID);
}

void SKInventoryListView::selectAll()
{
    mSelected.assign(mRows.size(), true);
    notifySelection();
}

void SKInventoryListView::deselect()
{
    mSelected.assign(mRows.size(), false);
    mCursor = -1;
    notifySelection();
}

bool SKInventoryListView::canDeselect() const
{
    return mSelectedCount > 0;
}
