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
#include "llfolderview.h"
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
    constexpr size_t UNDO_LIMIT = 50;

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
        std::function<void()> mUndo;    // any other action's own undo

        bool empty() const { return mMoves.empty() && mFavorites.empty() && !mUndo; }
    };

    // A refusal's weight: the most telling reason wins when the cargo is refused for several.
    enum EReason
    {
        REASON_NONE,
        REASON_LINK,
        REASON_STOCK,
        REASON_SYSTEM,
        REASON_LIBRARY,
        REASON_INTO_ITSELF,
    };

    std::map<std::string, std::string> sStrings;
    const void* sOwner = nullptr;
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
        EReason mReasonRank = REASON_NONE;
        std::string mReason;
        S32 mDone = 0;          // objects moved, linked or favourited by a drop
        bool mUndoable = true;  // every accepted object of the drop was recorded
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

    bool within(const LLUUID& id, const LLUUID& folder_id)
    {
        return folder_id.notNull() && (id == folder_id || gInventory.isObjectDescendentOf(id, folder_id));
    }

    bool inLibrary(const LLUUID& id)
    {
        return within(id, gInventory.getLibraryRootFolderID());
    }

    bool inPlainInventory(const LLUUID& id)
    {
        if (!within(id, gInventory.getRootFolderID()))
        {
            return false;
        }
        for (const LLUUID& outside : { gInventory.getMarketplaceListingsUUID(),
                                       gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH),
                                       gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT) })
        {
            if (within(id, outside))
            {
                return false;
            }
        }
        return true;
    }

    void refuse(EReason rank, const std::string& reason)
    {
        if (rank > sPass.mReasonRank && !reason.empty())
        {
            sPass.mReasonRank = rank;
            sPass.mReason = reason;
        }
    }

    // A new history entry, dropping empty ones and the oldest past the limit.
    void pushUndo(const UndoEntry& entry)
    {
        std::erase_if(sUndo, [](const UndoEntry& e) { return e.empty(); });
        sUndo.push_back(entry);
        if (sUndo.size() > UNDO_LIMIT)
        {
            sUndo.erase(sUndo.begin(), sUndo.begin() + (sUndo.size() - UNDO_LIMIT));
        }
    }

    // A pass starts at the first cargo object, or when the target or Alt changes mid-pass.
    void startPass(const LLUUID& dest, bool favorites, bool link, bool drop)
    {
        const bool first = LLToolDragAndDrop::getInstance()->getCargoIndex() == 0;
        if (!first && sPass.mDest == dest && sPass.mFavorites == favorites && sPass.mLink == link)
        {
            return;
        }
        sPass = Pass();
        sPass.mDest = dest;
        sPass.mFavorites = favorites;
        sPass.mLink = link;
        // A drop opens its own history entry before anything is accepted, so nothing lands in the last one.
        if (drop && first && !link)
        {
            UndoEntry entry;
            entry.mAdded = favorites;
            pushUndo(entry);
        }
    }

    // The tool's tooltip carries refusals alone; an accept is reported to the drag feedback, which words the chip.
    void finishVerdict(std::string& tooltip_msg)
    {
        sVerdict = Verdict();
        sVerdict.mOntoCargo = sPass.mOntoCargo;
        tooltip_msg.clear();
        if (sPass.mFavorites)
        {
            if (sPass.mAnyNotFolder)
            {
                tooltip_msg = string("drag_only_folders");
            }
            else if (sPass.mAllFavorite)
            {
                sVerdict.mQuiet = true;
            }
            else if (!AISAPI::isAvailable())
            {
                tooltip_msg = string("drag_favorites_unavailable");
            }
            else
            {
                sVerdict.mOK = true;
                LLDragFeedback::report(EDragAction::Favorite, {}, string("drag_add_favorites"));
            }
        }
        else if (sPass.mAllOK)
        {
            sVerdict.mOK = true;
            // The stock rules' own report (a link into an outfit, a wear) stands; a plain move, copy or link takes
            // the Explorer's words, and a note only where the stock rules gave none.
            const bool library = LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_LIBRARY;
            const EDragAction ours = sPass.mLink ? EDragAction::Link : library ? EDragAction::Copy : EDragAction::Move;
            const EDragAction reported = LLDragFeedback::reportedAction();
            if (reported == EDragAction::None || (reported == ours && LLDragFeedback::reportedText().empty()))
            {
                LLStringUtil::format_map_t args;
                args["[FOLDER]"] = folderName(sPass.mDest);
                const std::string text = ours == EDragAction::Link ? string("drag_link", args)
                                       : ours == EDragAction::Move ? string("drag_move", args) : std::string();
                const std::string note = !LLDragFeedback::reportedNote().empty() ? LLDragFeedback::reportedNote()
                                       : library ? string("drag_library_copies") : std::string();
                LLDragFeedback::report(ours, folderName(sPass.mDest), text, note);
            }
        }
        else if (sPass.mAllHere)
        {
            sVerdict.mQuiet = true;
        }
        else
        {
            tooltip_msg = sPass.mReason;
        }
        if (!sVerdict.mOK)
        {
            // A quiet refusal says nothing; a reason overrides a quiet one a stock rule gave for an earlier object.
            LLDragFeedback::refuse(tooltip_msg, sVerdict.mQuiet || tooltip_msg.empty());
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

    // Alt: a link, where the stock folder rules would take one.
    bool linkInto(const LLUUID& dest_id, LLInventoryObject* object, EDragAndDropType cargo_type, bool drop)
    {
        static LLCachedControl<bool> inventory_linking(gSavedSettings, "InventoryLinking", true);
        LLStringUtil::format_map_t args;
        args["[FOLDER]"] = folderName(dest_id);
        if (!inventory_linking)
        {
            refuse(REASON_LINK, string("drag_links_off"));
            return false;
        }
        if (LLToolDragAndDrop::getInstance()->getSource() != LLToolDragAndDrop::SOURCE_AGENT)
        {
            refuse(REASON_LINK, string("drag_link_own_only"));
            return false;
        }
        if (inLibrary(dest_id))
        {
            refuse(REASON_LIBRARY, string("drag_library_readonly"));
            return false;
        }
        const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        const LLViewerInventoryCategory* dest = gInventory.getCategory(dest_id);
        const bool into_outfit = dest_id == cof || (dest && dest->getPreferredType() == LLFolderType::FT_OUTFIT);

        // A folder links only where the stock rules link folders: the Current Outfit and outfits.
        LLInventoryCategory* folder = gInventory.getCategory(object->getUUID());
        if (!folder && object->getIsLinkType() && object->getActualType() == LLAssetType::AT_LINK_FOLDER)
        {
            folder = gInventory.getCategory(object->getLinkedUUID());
        }
        if (folder)
        {
            if (!into_outfit)
            {
                refuse(REASON_LINK, string("drag_folder_link", args));
                return false;
            }
            std::string message;
            const bool ok = dragCategoryIntoFolder(dest_id, folder, drop, message, true);
            if (!ok)
            {
                refuse(REASON_STOCK, message.empty() ? string("drag_no_link_here", args) : message);
            }
            return ok;
        }
        if (into_outfit)
        {
            // The stock drop into an outfit or the Current Outfit already makes links.
            std::string message;
            const bool ok = dragItemIntoFolder(dest_id, (LLInventoryItem*)object, drop, message, true);
            if (!ok)
            {
                refuse(REASON_STOCK, message.empty() ? string("drag_no_link_here", args) : message);
            }
            return ok;
        }
        const LLUUID calling_cards = gInventory.findCategoryUUIDForType(LLFolderType::FT_CALLINGCARD);
        const LLUUID landmarks = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
        const bool landmark_ok = !within(dest_id, landmarks) || object->getType() == LLAssetType::AT_LANDMARK;
        if (!inPlainInventory(dest_id) || within(dest_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_LOST_AND_FOUND))
            || within(dest_id, calling_cards) || !landmark_ok)
        {
            refuse(REASON_LINK, string("drag_no_link_here", args));
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
        const bool one = sPass.mDone == 1;
        if (sPass.mFavorites)
        {
            status(string("status_favorites_added", args));
        }
        else if (sPass.mLink)
        {
            status(string(one ? "status_linked_one" : "status_linked", args));
        }
        else if (LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_LIBRARY)
        {
            status(string(one ? "status_copied_one" : "status_copied", args));
        }
        else if (sPass.mUndoable)
        {
            status(string(one ? "status_moved_one" : "status_moved", args));
        }
        else
        {
            status(string(one ? "status_moved_no_undo_one" : "status_moved_no_undo", args));
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
    startPass(dest_id, false, link, drop);
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
    bool into_itself = false;
    if (cargo_cat && within(dest_id, id))
    {
        sPass.mOntoCargo |= dest_id == id;
        into_itself = true;
        refuse(REASON_INTO_ITSELF, string("drag_into_itself"));
    }
    if (from != dest_id)
    {
        sPass.mAllHere = false;
    }

    bool accepted = false;
    std::string message;
    if (into_itself || from == dest_id)
    {
        accepted = false;
    }
    else if (link)
    {
        accepted = linkInto(dest_id, object, cargo_type, drop);
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
        if (!accepted)
        {
            if (inLibrary(dest_id))
            {
                refuse(REASON_LIBRARY, string("drag_library_readonly"));
            }
            else if (cargo_cat && LLFolderType::lookupIsProtectedType(cargo_cat->getPreferredType()))
            {
                refuse(REASON_SYSTEM, string("drag_system_folders"));
            }
            refuse(REASON_STOCK, message);
        }
    }
    if (!accepted)
    {
        sPass.mAllOK = false;
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
        if (!link && !sUndo.empty())
        {
            // Only plain moves inside My Inventory that landed at once are undone, and a drop is undone whole or not at all.
            const LLInventoryObject* moved = gInventory.getObject(id);
            const bool undoable = LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_AGENT
                                && moved && moved->getParentUUID() == dest_id && inPlainInventory(from) && inPlainInventory(dest_id);
            if (undoable && sPass.mUndoable)
            {
                sUndo.back().mMoves.push_back({ id, from, dest_id });
            }
            else
            {
                sPass.mUndoable = false;
                sUndo.back().mMoves.clear();
            }
        }
        reportDrop();
    }
    return true;
}

bool dropOnFavorites(bool drop, EDragAndDropType cargo_type, void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    startPass(LLUUID::null, true, false, drop);
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

void setStatusCallback(const void* owner, const std::function<void(const std::string&)>& cb)
{
    sOwner = owner;
    sStatus = cb;
}

void releaseOwner(const void* owner)
{
    // The history belongs to the floater that made it; another instance's is left alone.
    if (sOwner == owner)
    {
        sOwner = nullptr;
        sStatus = nullptr;
        sUndo.clear();
    }
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
    pushUndo(entry);
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", (S32)ids.size());
    status(string(added ? "status_favorites_added" : "status_favorites_removed", args));
}

void recordAction(const std::function<void()>& undo_fn, const std::string& text)
{
    UndoEntry entry;
    entry.mUndo = undo_fn;
    pushUndo(entry);
    status(text);
}

bool refuse(const std::string& key, EAcceptance* accept, std::string& tooltip_msg)
{
    sVerdict = Verdict();
    *accept = ACCEPT_NO;
    tooltip_msg = string(key);
    LLDragFeedback::refuse(tooltip_msg);
    return true;
}

bool canUndo()
{
    for (const UndoEntry& entry : sUndo)
    {
        if (!entry.empty())
        {
            return true;
        }
    }
    return false;
}

void undo()
{
    while (!sUndo.empty() && sUndo.back().empty())
    {
        sUndo.pop_back();
    }
    if (sUndo.empty())
    {
        return;
    }
    const UndoEntry entry = sUndo.back();
    sUndo.pop_back();
    // Every object must still be where the action left it, every old parent still in place and not inside what moves.
    auto refuse_undo = [](const std::string& name)
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        status(string("status_cant_undo", args));
    };
    for (const Move& move : entry.mMoves)
    {
        const LLInventoryObject* object = gInventory.getObject(move.mID);
        if (!object || object->getParentUUID() != move.mTo)
        {
            refuse_undo(object ? object->getName() : folderName(move.mTo));
            return;
        }
        if (!gInventory.getCategory(move.mFrom) || !inPlainInventory(move.mFrom) || gInventory.isObjectDescendentOf(move.mFrom, move.mID))
        {
            refuse_undo(folderName(move.mFrom).empty() ? object->getName() : folderName(move.mFrom));
            return;
        }
    }
    for (const LLUUID& id : entry.mFavorites)
    {
        if (!gInventory.getCategory(id))
        {
            refuse_undo(folderName(id));
            return;
        }
    }
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
    if (entry.mUndo)
    {
        entry.mUndo();
    }
    status(string("status_undone"));
}

F32 springSeconds()
{
    return LLFolderView::sAutoOpenTime;
}

EDragMark markKind()
{
    if (sVerdict.mOntoCargo)
    {
        return EDragMark::Quiet;
    }
    return sVerdict.mOK ? EDragMark::Accept : sVerdict.mQuiet ? EDragMark::Quiet : EDragMark::Refuse;
}

void mark(const LLView* view, const LLRect& local_rect)
{
    LLDragFeedback::mark(view, local_rect, markKind());
}

void drawMark(const LLView* view, bool fill)
{
    LLRect rect;
    const EDragMark kind = LLDragFeedback::markFor(view, &rect);
    LLDragFeedback::drawTargetMark(rect, kind, fill);
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
