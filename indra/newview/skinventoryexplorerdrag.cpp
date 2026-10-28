/**
 * @file skinventoryexplorerdrag.cpp
 * @brief Inventory Explorer's drag and drop: starting drags, drop verdicts, spring-loading, autoscroll and undo
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

#include "skinventoryexplorerdrag.h"

#include "llaisapi.h"
#include "llfriendcard.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "llviewerassettype.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llwindow.h"

// The gallery's drop functions, which carry every stock rule (llinventorygallery.cpp).
bool dragCategoryIntoFolder(LLUUID dest_id, LLInventoryCategory* inv_cat, bool drop, std::string& tooltip_msg, bool is_link);
bool dragItemIntoFolder(LLUUID folder_id, LLInventoryItem* inv_item, bool drop, std::string& tooltip_msg, bool user_confirm);

namespace SKInventoryExplorerDrag
{
namespace
{
    struct Move
    {
        LLUUID mID;
        LLUUID mFrom;
        LLUUID mTo;
    };

    struct UndoEntry
    {
        std::vector<Move> mMoves;
        uuid_vec_t mFavorites;
        bool mAdded = false;    // favourites added, else removed
    };

    std::map<std::string, std::string> sStrings;
    std::function<void(const std::string&)> sStatus;
    std::vector<UndoEntry> sUndo;
    Verdict sVerdict;

    // The verdict gathered over one pass of the cargo.
    struct Pass
    {
        LLUUID mDest;
        bool mFavorites = false;
        bool mLink = false;
        bool mAllOK = true;
        bool mAllHere = true;
        bool mAllFavorite = true;
        bool mAnyNotFolder = false;
        bool mOntoCargo = false;
        bool mIntoItself = false;
        bool mSystem = false;
        std::string mStockMessage;
        S32 mDone = 0;      // objects moved, linked or favourited by a drop
        bool mUndoable = true;
    };
    Pass sPass;

    std::string string(const std::string& key, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
    {
        auto it = sStrings.find(key);
        std::string text = it != sStrings.end() ? it->second : key;
        LLStringUtil::format(text, args);
        return text;
    }

    std::string folderName(const LLUUID& id)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        return cat ? cat->getName() : std::string();
    }

    void status(const std::string& text)
    {
        if (sStatus)
        {
            sStatus(text);
        }
    }

    bool inLibrary(const LLUUID& id)
    {
        return id == gInventory.getLibraryRootFolderID() || gInventory.isObjectDescendentOf(id, gInventory.getLibraryRootFolderID());
    }

    bool inPlainInventory(const LLUUID& id)
    {
        const LLUUID& root = gInventory.getRootFolderID();
        if (id != root && !gInventory.isObjectDescendentOf(id, root))
        {
            return false;
        }
        const LLUUID& listings = gInventory.getMarketplaceListingsUUID();
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        for (const LLUUID& outside : { listings, trash, cof })
        {
            if (outside.notNull() && (id == outside || gInventory.isObjectDescendentOf(id, outside)))
            {
                return false;
            }
        }
        return true;
    }

    // A pass starts at the first cargo object, or when the target or Alt changes mid-pass.
    void startPass(const LLUUID& dest, bool favorites, bool link)
    {
        if (LLToolDragAndDrop::getInstance()->getCargoIndex() != 0
            && sPass.mDest == dest && sPass.mFavorites == favorites && sPass.mLink == link)
        {
            return;
        }
        sPass = Pass();
        sPass.mDest = dest;
        sPass.mFavorites = favorites;
        sPass.mLink = link;
    }

    void finishVerdict(std::string& tooltip_msg)
    {
        LLStringUtil::format_map_t args;
        args["[FOLDER]"] = folderName(sPass.mDest);
        sVerdict = Verdict();
        sVerdict.mOntoCargo = sPass.mOntoCargo;
        if (sPass.mFavorites)
        {
            if (sPass.mAnyNotFolder)
            {
                tooltip_msg = string("drag_only_folders");
            }
            else if (sPass.mAllFavorite)
            {
                tooltip_msg = string("drag_already_favorites");
                sVerdict.mQuiet = true;
            }
            else if (!AISAPI::isAvailable())
            {
                tooltip_msg.clear();
            }
            else
            {
                tooltip_msg = string("drag_add_favorites");
                sVerdict.mOK = true;
            }
            return;
        }
        if (sPass.mAllOK)
        {
            sVerdict.mOK = true;
            const bool library = LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_LIBRARY;
            tooltip_msg = string(sPass.mLink ? "drag_link" : library ? "drag_library_copies" : "drag_move", args);
            return;
        }
        if (sPass.mAllHere && !sPass.mLink)
        {
            tooltip_msg = string("drag_already_here");
            sVerdict.mQuiet = true;
        }
        else if (!sPass.mStockMessage.empty())
        {
            tooltip_msg = sPass.mStockMessage;
        }
        else if (inLibrary(sPass.mDest))
        {
            tooltip_msg = string("drag_library_readonly");
        }
        else if (sPass.mIntoItself)
        {
            tooltip_msg = string("drag_into_itself");
        }
        else if (sPass.mSystem)
        {
            tooltip_msg = string("drag_system_folders");
        }
        else
        {
            tooltip_msg.clear();
        }
    }

    // The object a cargo pointer stands for, or null for cargo that is not inventory.
    LLInventoryObject* cargoObject(void* cargo_data)
    {
        const LLToolDragAndDrop::ESource source = LLToolDragAndDrop::getInstance()->getSource();
        if (!cargo_data || source == LLToolDragAndDrop::SOURCE_PEOPLE || source == LLToolDragAndDrop::SOURCE_VIEWER)
        {
            return nullptr;
        }
        return (LLInventoryObject*)cargo_data;
    }

    bool linkInto(const LLUUID& dest_id, LLInventoryObject* object, bool drop)
    {
        static LLCachedControl<bool> inventory_linking(gSavedSettings, "InventoryLinking", true);
        if (!inventory_linking || LLToolDragAndDrop::getInstance()->getSource() != LLToolDragAndDrop::SOURCE_AGENT
            || !inPlainInventory(dest_id) || dest_id == object->getUUID())
        {
            return false;
        }
        if (drop)
        {
            const LLUUID target = object->getIsLinkType() ? object->getLinkedUUID() : object->getUUID();
            link_inventory_object(dest_id, target, LLPointer<LLInventoryCallback>(nullptr));
        }
        return true;
    }

    void reportDrop()
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", sPass.mDone);
        args["[FOLDER]"] = folderName(sPass.mDest);
        if (sPass.mFavorites)
        {
            status(string("status_favorites_added", args));
        }
        else if (sPass.mLink)
        {
            status(string(sPass.mDone == 1 ? "status_linked_one" : "status_linked", args));
        }
        else if (sPass.mUndoable && !sUndo.empty() && !sUndo.back().mMoves.empty())
        {
            status(string(sPass.mDone == 1 ? "status_moved_one" : "status_moved", args));
        }
    }
}

bool begin(const uuid_vec_t& ids)
{
    std::vector<EDragAndDropType> types;
    uuid_vec_t cargo;
    bool library = false;
    bool agent = false;
    for (const LLUUID& id : ids)
    {
        if (const LLViewerInventoryItem* item = gInventory.getItem(id))
        {
            types.push_back(LLViewerAssetType::lookupDragAndDropType(item->getType()));
        }
        else if (const LLViewerInventoryCategory* cat = gInventory.getCategory(id))
        {
            if (!inLibrary(id) && LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
            {
                continue;   // system folders stay, as the gallery leaves them
            }
            types.push_back(LLViewerAssetType::lookupDragAndDropType(cat->getType()));
        }
        else
        {
            continue;
        }
        (inLibrary(id) ? library : agent) = true;
        cargo.push_back(id);
    }
    if (cargo.empty() || (library && agent))
    {
        return false;
    }
    LLToolDragAndDrop::getInstance()->beginMultiDrag(types, cargo,
                                                     library ? LLToolDragAndDrop::SOURCE_LIBRARY : LLToolDragAndDrop::SOURCE_AGENT);
    return true;
}

bool isDragging()
{
    return LLToolDragAndDrop::getInstance()->hasMouseCapture();
}

bool dropOnFolder(const LLUUID& dest_id, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                  EAcceptance* accept, std::string& tooltip_msg)
{
    const bool link = (mask & MASK_ALT) != 0;
    startPass(dest_id, false, link);
    *accept = ACCEPT_NO;
    LLInventoryObject* object = cargoObject(cargo_data);
    if (!object)
    {
        sPass.mAllOK = false;
        sPass.mAllHere = false;
        finishVerdict(tooltip_msg);
        return false;
    }

    const LLUUID id = object->getUUID();
    const LLUUID from = object->getParentUUID();
    LLViewerInventoryCategory* cargo_cat = gInventory.getCategory(id);
    if (cargo_cat && (dest_id == id || gInventory.isObjectDescendentOf(dest_id, id)))
    {
        sPass.mOntoCargo |= dest_id == id;
        sPass.mIntoItself = true;
    }
    if (cargo_cat && LLFolderType::lookupIsProtectedType(cargo_cat->getPreferredType()))
    {
        sPass.mSystem = true;
    }
    if (from != dest_id)
    {
        sPass.mAllHere = false;
    }

    bool accepted = false;
    std::string message;
    if (link)
    {
        accepted = !sPass.mIntoItself && linkInto(dest_id, object, drop);
    }
    else if (from == dest_id)
    {
        accepted = false;
    }
    else
    {
        switch (cargo_type)
        {
            case DAD_TEXTURE:
            case DAD_SOUND:
            case DAD_CALLINGCARD:
            case DAD_LANDMARK:
            case DAD_SCRIPT:
            case DAD_CLOTHING:
            case DAD_OBJECT:
            case DAD_NOTECARD:
            case DAD_BODYPART:
            case DAD_ANIMATION:
            case DAD_GESTURE:
            case DAD_MESH:
            case DAD_SETTINGS:
                accepted = dragItemIntoFolder(dest_id, (LLInventoryItem*)object, drop, message, true);
                break;
            case DAD_LINK:
            {
                LLInventoryItem* item = (LLInventoryItem*)object;
                LLInventoryCategory* linked = LLAssetType::AT_LINK_FOLDER == item->getActualType()
                                            ? gInventory.getCategory(item->getLinkedUUID()) : nullptr;
                accepted = linked ? dragCategoryIntoFolder(dest_id, linked, drop, message, true)
                                  : dragItemIntoFolder(dest_id, item, drop, message, true);
                break;
            }
            case DAD_CATEGORY:
                accepted = !LLFriendCardsManager::instance().isAnyFriendCategory(dest_id)
                        && dragCategoryIntoFolder(dest_id, (LLInventoryCategory*)object, drop, message, false);
                break;
            default:
                break;
        }
    }
    if (!accepted)
    {
        sPass.mAllOK = false;
        if (sPass.mStockMessage.empty())
        {
            sPass.mStockMessage = message;
        }
    }
    finishVerdict(tooltip_msg);
    if (!accepted)
    {
        return false;
    }
    *accept = ACCEPT_YES_MULTI;

    if (drop)
    {
        ++sPass.mDone;
        // Only a plain move inside My Inventory that landed at once can be undone.
        const LLInventoryObject* moved = gInventory.getObject(id);
        const bool undoable = !link && LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_AGENT
                            && moved && moved->getParentUUID() == dest_id && inPlainInventory(from) && inPlainInventory(dest_id);
        if (LLToolDragAndDrop::getInstance()->getCargoIndex() == 0)
        {
            sUndo.emplace_back();
        }
        if (undoable && !sUndo.empty())
        {
            sUndo.back().mMoves.push_back({ id, from, dest_id });
        }
        sPass.mUndoable &= undoable;
        reportDrop();
    }
    return true;
}

bool dropOnFavorites(bool drop, EDragAndDropType cargo_type, void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    startPass(LLUUID::null, true, false);
    *accept = ACCEPT_NO;
    LLInventoryObject* object = cargoObject(cargo_data);
    const LLUUID folder_id = object ? (object->getIsLinkType() ? object->getLinkedUUID() : object->getUUID()) : LLUUID::null;
    if (!object || !gInventory.getCategory(folder_id) || inLibrary(folder_id)
        || LLToolDragAndDrop::getInstance()->getSource() != LLToolDragAndDrop::SOURCE_AGENT)
    {
        sPass.mAnyNotFolder = true;
    }
    else if (!get_is_favorite(folder_id))
    {
        sPass.mAllFavorite = false;
    }
    finishVerdict(tooltip_msg);
    // A folder already in Favorites is taken along with the rest, and left as it is.
    if (sPass.mAnyNotFolder || !AISAPI::isAvailable())
    {
        return false;
    }
    *accept = ACCEPT_YES_MULTI;
    if (drop && LLToolDragAndDrop::getInstance()->getCargoIndex() == 0)
    {
        UndoEntry entry;
        entry.mAdded = true;
        sUndo.push_back(entry);
    }
    if (drop && !get_is_favorite(folder_id))
    {
        set_favorite(folder_id, true);
        if (!sUndo.empty())
        {
            sUndo.back().mFavorites.push_back(folder_id);
        }
        ++sPass.mDone;
        reportDrop();
    }
    return true;
}

const Verdict& verdict()
{
    return sVerdict;
}

void setStrings(const std::map<std::string, std::string>& strings)
{
    sStrings = strings;
}

void setStatusCallback(const std::function<void(const std::string&)>& cb)
{
    sStatus = cb;
}

void recordFavorites(const uuid_vec_t& ids, bool added)
{
    if (ids.empty())
    {
        return;
    }
    UndoEntry entry;
    entry.mFavorites = ids;
    entry.mAdded = added;
    sUndo.push_back(entry);
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", (S32)ids.size());
    status(string(added ? "status_favorites_added" : "status_favorites_removed", args));
}

bool canUndo()
{
    std::erase_if(sUndo, [](const UndoEntry& entry) { return entry.mMoves.empty() && entry.mFavorites.empty(); });
    return !sUndo.empty();
}

void undo()
{
    if (!canUndo())
    {
        return;
    }
    const UndoEntry entry = sUndo.back();
    // Every object must still be where the action left it, and every old parent still in place.
    for (const Move& move : entry.mMoves)
    {
        const LLInventoryObject* object = gInventory.getObject(move.mID);
        if (!object || object->getParentUUID() != move.mTo || !gInventory.getCategory(move.mFrom) || !inPlainInventory(move.mFrom))
        {
            LLStringUtil::format_map_t args;
            args["[NAME]"] = object ? object->getName() : folderName(move.mFrom);
            status(string("status_cant_undo", args));
            sUndo.pop_back();
            return;
        }
    }
    sUndo.pop_back();
    for (const Move& move : entry.mMoves)
    {
        if (LLViewerInventoryCategory* cat = gInventory.getCategory(move.mID))
        {
            gInventory.changeCategoryParent(cat, move.mFrom, false);
        }
        else if (LLViewerInventoryItem* item = gInventory.getItem(move.mID))
        {
            gInventory.changeItemParent(item, move.mFrom, false);
        }
    }
    for (const LLUUID& id : entry.mFavorites)
    {
        set_favorite(id, !entry.mAdded);
    }
    status(string("status_undone"));
}

void drawTargetMark(const LLRect& rect, const Verdict& verdict)
{
    static LLUIColor accent = LLUIColorTable::instance().getColor("EmphasisColor");
    static LLUIColor fill = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static const LLColor4 refusal(0.71f, 0.33f, 0.33f, 1.f);
    if (verdict.mOntoCargo || (!verdict.mOK && verdict.mQuiet))
    {
        return;
    }
    if (verdict.mOK)
    {
        gl_rect_2d(rect, fill.get(), true);
        gl_rect_2d(rect, accent.get(), false);
        gl_rect_2d(rect.mLeft + 1, rect.mTop - 1, rect.mRight - 1, rect.mBottom + 1, accent.get(), false);
        return;
    }
    // A dashed outline, 4 px on and 3 off.
    constexpr S32 ON = 4;
    constexpr S32 STEP = 7;
    for (S32 x = rect.mLeft; x < rect.mRight; x += STEP)
    {
        const S32 end = llmin(x + ON, rect.mRight);
        gl_line_2d(x, rect.mTop - 1, end, rect.mTop - 1, refusal);
        gl_line_2d(x, rect.mBottom, end, rect.mBottom, refusal);
    }
    for (S32 y = rect.mBottom; y < rect.mTop; y += STEP)
    {
        const S32 end = llmin(y + ON, rect.mTop);
        gl_line_2d(rect.mLeft, y, rect.mLeft, end, refusal);
        gl_line_2d(rect.mRight - 1, y, rect.mRight - 1, end, refusal);
    }
}
}

SKDragHover::SKDragHover()
:   mStill(false),
    mStillX(0),
    mStillY(0),
    mFrame(0)
{
}

bool SKDragHover::spring(const LLUUID& target, S32 screen_x, S32 screen_y, F32 delay)
{
    if (mStill && (llabs(screen_x - mStillX) > 3 || llabs(screen_y - mStillY) > 3))
    {
        mStill = false;
        mHeld.reset();
    }
    if (target != mTarget)
    {
        mTarget = target;
        mHeld.reset();
        mLastCheck.reset();
        return false;
    }
    if (target.isNull() || mStill || mLastCheck.getElapsedTimeF32() < SKInventoryExplorerDrag::TICK_SECONDS)
    {
        return false;
    }
    mLastCheck.reset();
    if (mHeld.getElapsedTimeF32() < delay)
    {
        return false;
    }
    mTarget.setNull();
    return true;
}

void SKDragHover::scrolled(S32 screen_x, S32 screen_y)
{
    mStill = true;
    mStillX = screen_x;
    mStillY = screen_y;
    mHeld.reset();
}

bool SKDragHover::tick(F32 period)
{
    if (mTick.getElapsedTimeF32() < period)
    {
        return false;
    }
    mTick.reset();
    return true;
}

void SKDragHover::touch()
{
    mFrame = LLFrameTimer::getFrameCount();
}

bool SKDragHover::isActive() const
{
    return mFrame + 1 >= LLFrameTimer::getFrameCount() && SKInventoryExplorerDrag::isDragging();
}

void SKDragHover::reset()
{
    mTarget.setNull();
    mStill = false;
    mFrame = 0;
}
