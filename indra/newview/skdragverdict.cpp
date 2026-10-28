/**
 * @file skdragverdict.cpp
 * @brief The viewer's half of drag feedback: the cargo's ghost, the world's verdicts and the chip's text.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
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

#include "skdragverdict.h"

#include "alnotecardworld.h"
#include "alselectionoutline.h"
#include "llavatariconctrl.h"
#include "llavatarnamecache.h"
#include "llcommandmanager.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "lltooldraganddrop.h"
#include "lltrans.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <cstdlib>

namespace
{
    using ESource = LLToolDragAndDrop::ESource;

    constexpr size_t MAX_GHOST_ICONS = 3;

    struct World
    {
        bool mValid = false;
        EDragAction mAction = EDragAction::None;
        std::string mTarget;
        std::vector<std::string> mHints;
        LLUUID mObjectID;
        U32 mTEMask = 0;
        bool mWholeLinkset = false;
        EDragMark mMark = EDragMark::None;
    };

    World sWorld;
    std::vector<std::string> sHints;
    // Whether the last pass reached the world, so a pick landing late does not mark it under a floater.
    bool sInWorld = false;

    // The width over height a texture's description names in a "(w:h)" tail, or 0.
    F32 aspect_of(const std::string& desc)
    {
        const size_t open = desc.rfind('(');
        if (open == std::string::npos || desc.empty() || desc.back() != ')')
        {
            return 0.f;
        }
        const std::string inner = desc.substr(open + 1, desc.size() - open - 2);
        const size_t colon = inner.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= inner.size())
        {
            return 0.f;
        }
        for (char c : inner)
        {
            if (c != ':' && (c < '0' || c > '9'))
            {
                return 0.f;
            }
        }
        const S32 w = atoi(inner.substr(0, colon).c_str());
        const S32 h = atoi(inner.substr(colon + 1).c_str());
        return (w > 0 && h > 0) ? (F32)w / (F32)h : 0.f;
    }

    LLInventoryObject* find_cargo(const LLUUID& id, EDragAndDropType type, S32 source, const LLUUID& source_id)
    {
        switch (source)
        {
        case LLToolDragAndDrop::SOURCE_AGENT:
        case LLToolDragAndDrop::SOURCE_LIBRARY:
            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                return item;
            }
            return gInventory.getCategory(id);
        case LLToolDragAndDrop::SOURCE_WORLD:
            if (LLViewerObject* object = gObjectList.findObject(source_id))
            {
                return object->getInventoryObject(id);
            }
            return nullptr;
        case LLToolDragAndDrop::SOURCE_NOTECARD:
            return ALNotecardDrag::dragged(id);
        default:
            return nullptr;
        }
    }

    LLPointer<LLUIImage> category_icon(const LLInventoryCategory* cat)
    {
        return LLUI::getUIImage(std::string(LLViewerFolderType::lookupIconName(cat->getPreferredType(), false)));
    }

    LLPointer<LLUIImage> item_icon(const LLInventoryItem* item)
    {
        return LLInventoryIcon::getIcon(item->getType(), item->getInventoryType(), item->getFlags(), false);
    }

    // The ghost's icon for one inventory object, a link showing its original's with a badge; and its picture.
    void add_inventory_icon(LLInventoryObject* obj, bool single)
    {
        LLPointer<LLUIImage> badge;
        LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(obj);
        LLInventoryCategory* cat = dynamic_cast<LLInventoryCategory*>(obj);
        if (item && item->getIsLinkType())
        {
            if (item->getIsBrokenLink())
            {
                LLDragFeedback::addCargoIcon(LLInventoryIcon::getIcon(LLInventoryType::ICONNAME_INVALID));
                return;
            }
            badge = LLUI::getUIImage("Inv_Link");
            if (LLViewerInventoryCategory* linked_cat = item->getLinkedCategory())
            {
                item = nullptr;
                cat = linked_cat;
            }
            else
            {
                item = item->getLinkedItem();
            }
        }

        if (cat)
        {
            LLDragFeedback::addCargoIcon(category_icon(cat), badge);
        }
        else if (item)
        {
            LLDragFeedback::addCargoIcon(item_icon(item), badge);
        }
        else
        {
            return;
        }

        if (!single)
        {
            return;
        }
        // A single item with a picture shows it: a texture's own image, else the thumbnail.
        LLUUID picture = obj->getThumbnailUUID();
        F32 aspect = 0.f;
        if (item && item->getType() == LLAssetType::AT_TEXTURE && item->getAssetUUID().notNull())
        {
            picture = item->getAssetUUID();
            aspect = aspect_of(item->getDescription());
        }
        else if (picture.isNull() && cat)
        {
            picture = cat->getThumbnailUUID();
        }
        if (picture.notNull())
        {
            LLPointer<LLTexture> texture = LLViewerTextureManager::getFetchedTexture(picture, FTT_DEFAULT, true,
                                                                                     LLGLTexture::BOOST_NONE,
                                                                                     LLViewerTexture::LOD_TEXTURE);
            LLDragFeedback::setThumbnail(texture, aspect);
        }
    }

    std::string verdict_text(EDragAction action, const std::string& target)
    {
        const bool named = !target.empty();
        const char* key = nullptr;
        switch (action)
        {
        case EDragAction::Move:             key = named ? "DragVerdictMove" : "DragVerdictMoveHere"; break;
        case EDragAction::Copy:             key = named ? "DragVerdictCopy" : "DragVerdictCopyHere"; break;
        case EDragAction::Link:             key = named ? "DragVerdictLink" : "DragVerdictLinkHere"; break;
        case EDragAction::Wear:             key = "DragVerdictWear"; break;
        case EDragAction::WearAdd:          key = "DragVerdictWearAdd"; break;
        case EDragAction::Attach:           key = "DragVerdictAttach"; break;
        case EDragAction::AttachAdd:        key = "DragVerdictAttachAdd"; break;
        case EDragAction::Activate:         key = "DragVerdictActivate"; break;
        case EDragAction::Give:             key = named ? "DragVerdictGive" : "DragVerdictGiveHere"; break;
        case EDragAction::Rez:              key = "DragVerdictRez"; break;
        case EDragAction::Contents:         key = "DragVerdictContents"; break;
        case EDragAction::ContentsInactive: key = "DragVerdictContentsInactive"; break;
        case EDragAction::Apply:            key = "DragVerdictApply"; break;
        case EDragAction::ApplyAll:         key = "DragVerdictApplyAll"; break;
        case EDragAction::MakeOutfit:       key = "DragVerdictMakeOutfit"; break;
        case EDragAction::Reorder:          key = "DragVerdictReorder"; break;
        case EDragAction::Set:              key = "DragVerdictSet"; break;
        default:                            return std::string();
        }
        LLStringUtil::format_map_t args;
        args["[TARGET]"] = target;
        return LLTrans::getString(key, args);
    }

    void add_hint(const char* key)
    {
        sWorld.mHints.push_back(LLTrans::getString(key));
    }
}

namespace SKDragVerdict
{
    std::string avatarName(const LLUUID& id)
    {
        LLAvatarName av_name;
        if (!LLAvatarNameCache::get(id, &av_name))
        {
            return std::string();
        }
        if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
        {
            return RlvStrings::getAnonym(av_name);
        }
        return av_name.getCompleteName();
    }

    void beginDrag(const std::vector<EDragAndDropType>& types, const uuid_vec_t& ids, S32 source,
                   const LLUUID& source_id)
    {
        clearWorld();
        LLDragFeedback::begin(ids);
        const bool single = ids.size() == 1;
        for (size_t i = 0; i < ids.size() && LLDragFeedback::iconCount() < MAX_GHOST_ICONS; ++i)
        {
            const EDragAndDropType type = i < types.size() ? types[i] : DAD_NONE;
            if (source == LLToolDragAndDrop::SOURCE_PEOPLE || type == DAD_PERSON)
            {
                LLDragFeedback::addCargoIcon(LLUI::getUIImage("Generic_Person"));
                if (single && RlvActions::canShowName(RlvActions::SNC_DEFAULT, ids[i]))
                {
                    if (LLUUID* icon_id = LLAvatarIconIDCache::getInstance()->get(ids[i]))
                    {
                        LLPointer<LLTexture> texture = LLViewerTextureManager::getFetchedTexture(
                            *icon_id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
                        LLDragFeedback::setThumbnail(texture, 1.f);
                    }
                }
                continue;
            }
            if (type == DAD_WIDGET)
            {
                if (LLCommand* command = LLCommandManager::instance().getCommand(LLCommandId(ids[i])))
                {
                    LLDragFeedback::addCargoIcon(LLUI::getUIImage(command->icon()));
                }
                continue;
            }
            if (LLInventoryObject* obj = find_cargo(ids[i], type, source, source_id))
            {
                add_inventory_icon(obj, single);
            }
        }
    }

    void clearWorld()
    {
        sWorld = World();
    }

    void pickWorld(EDragAndDropType cargo, S32 target, LLViewerObject* object, S32 face, MASK mask, EAcceptance accept)
    {
        sWorld = World();
        sWorld.mValid = true;
        const bool ctrl = mask & MASK_CONTROL;
        const bool shift = mask & MASK_SHIFT;
        EDragAction action = EDragAction::None;
        bool whole = false;

        switch (target)
        {
        case WORLD_SELF:
            whole = true;
            switch (cargo)
            {
            case DAD_OBJECT:
                action = ctrl ? EDragAction::AttachAdd : EDragAction::Attach;
                if (!ctrl) add_hint("DragHintAttachAdd");
                break;
            case DAD_CLOTHING:
                action = ctrl ? EDragAction::WearAdd : EDragAction::Wear;
                if (!ctrl) add_hint("DragHintWearAdd");
                break;
            case DAD_BODYPART:
                action = EDragAction::Wear;
                break;
            case DAD_CATEGORY:
                action = shift ? EDragAction::WearAdd : EDragAction::Wear;
                if (!shift) add_hint("DragHintWearAddShift");
                break;
            case DAD_GESTURE:
                action = EDragAction::Activate;
                break;
            default:
                break;
            }
            break;
        case WORLD_AVATAR:
            whole = true;
            action = EDragAction::Give;
            if (object)
            {
                sWorld.mTarget = avatarName(object->getID());
            }
            break;
        case WORLD_OBJECT:
            sWorld.mTarget = LLTrans::getString("DragTargetObject");
            switch (cargo)
            {
            case DAD_TEXTURE:
            case DAD_MATERIAL:
                if (ctrl)
                {
                    action = EDragAction::Contents;
                    whole = true;
                }
                else
                {
                    action = shift ? EDragAction::ApplyAll : EDragAction::Apply;
                    add_hint("DragHintContents");
                    if (!shift) add_hint("DragHintAllFaces");
                    if (cargo == DAD_TEXTURE && !(mask & MASK_ALT)) add_hint("DragHintNoPBR");
                }
                break;
            case DAD_MESH:
                action = ctrl ? EDragAction::Contents : EDragAction::Apply;
                whole = true;
                if (!ctrl) add_hint("DragHintContents");
                break;
            case DAD_SCRIPT:
                action = ctrl ? EDragAction::ContentsInactive : EDragAction::Contents;
                whole = true;
                if (!ctrl) add_hint("DragHintInactive");
                break;
            case DAD_OBJECT:
                action = ctrl ? EDragAction::Contents : EDragAction::Rez;
                whole = true;
                if (!ctrl) add_hint("DragHintContents");
                break;
            case DAD_CATEGORY:
                action = EDragAction::Contents;
                whole = true;
                if (!ctrl) add_hint("DragHintContentsNeeded");
                break;
            case DAD_NONE:
            case DAD_LINK:
            case DAD_WIDGET:
            case DAD_PERSON:
            case DAD_ROOT_CATEGORY:
            case DAD_GLTF:
            case DAD_GLTF_BIN:
                break;
            default:
                action = EDragAction::Contents;
                whole = true;
                break;
            }
            break;
        case WORLD_LAND:
            if (cargo == DAD_OBJECT)
            {
                action = EDragAction::Rez;
                sWorld.mTarget = LLTrans::getString("DragTargetLand");
            }
            break;
        default:
            break;
        }

        sWorld.mAction = action;
        // A refused folder into contents is refused for want of Control: its hint says so; nothing else hints.
        const bool accepted = accept >= ACCEPT_YES_COPY_SINGLE;
        if (!accepted && !(target == WORLD_OBJECT && cargo == DAD_CATEGORY && !ctrl))
        {
            sWorld.mHints.clear();
        }

        if (object && action != EDragAction::None && target != WORLD_LAND)
        {
            sWorld.mObjectID = object->getID();
            sWorld.mWholeLinkset = whole;
            sWorld.mTEMask = (whole || face < 0 || action == EDragAction::ApplyAll) ? 0xFFFFFFFF : (1u << (face & 31));
            sWorld.mMark = accepted ? EDragMark::Accept : EDragMark::Quiet;
        }
    }

    void beginPass()
    {
        LLDragFeedback::beginPass();
        sInWorld = false;
    }

    void reportWorld()
    {
        sInWorld = true;
        if (!sWorld.mValid)
        {
            return;
        }
        if (sWorld.mAction != EDragAction::None)
        {
            LLDragFeedback::report(sWorld.mAction, sWorld.mTarget);
        }
        sHints = sWorld.mHints;
    }

    void addWorldMark(ALSelectionOutline& outline)
    {
        if (!LLDragFeedback::shown() || !sInWorld || sWorld.mMark != EDragMark::Accept || sWorld.mObjectID.isNull())
        {
            return;
        }
        LLViewerObject* object = gObjectList.findObject(sWorld.mObjectID);
        if (!object || object->isDead())
        {
            return;
        }
        static const LLColor4 accept_line(0.3f, 0.82f, 1.f, 1.f);
        const LLColor4 colour = LLUIColorTable::instance().getColor("DragAcceptOutlineColor", accept_line).get();
        const U32 parts = ALSelectionOutline::PART_CONTOUR;
        if (sWorld.mWholeLinkset && !object->isAvatar())
        {
            LLViewerObject* root = object->getRootEdit();
            root = root ? root : object;
            outline.add(root, 0xFFFFFFFF, colour, false, ALSelectionOutline::PRIORITY_CONTEXT, parts);
            for (LLViewerObject* child : root->getChildren())
            {
                if (child && !child->isAvatar())
                {
                    outline.add(child, 0xFFFFFFFF, colour, false, ALSelectionOutline::PRIORITY_CONTEXT, parts);
                }
            }
            return;
        }
        if (object->isAvatar())
        {
            // An avatar is outlined by what it wears.
            for (LLViewerObject* child : object->getChildren())
            {
                if (child && !child->isAvatar() && !child->isHUDAttachment())
                {
                    outline.add(child, 0xFFFFFFFF, colour, false, ALSelectionOutline::PRIORITY_CONTEXT, parts);
                    for (LLViewerObject* grandchild : child->getChildren())
                    {
                        outline.add(grandchild, 0xFFFFFFFF, colour, false, ALSelectionOutline::PRIORITY_CONTEXT, parts);
                    }
                }
            }
            return;
        }
        outline.add(object, sWorld.mTEMask, colour, false, ALSelectionOutline::PRIORITY_CONTEXT, parts);
    }

    void compose(EAcceptance accept, const std::string& tooltip_msg, S32 source, U32 cargo_count)
    {
        std::vector<std::string> hints;
        hints.swap(sHints);
        if (!LLDragFeedback::shown())
        {
            return;
        }
        const bool accepted = accept >= ACCEPT_YES_COPY_SINGLE && tooltip_msg.empty();
        if (!accepted)
        {
            // A refusal with no message is quiet: nothing is said, and RLVa's never are.
            std::string text = tooltip_msg.empty() ? LLDragFeedback::refusal() : tooltip_msg;
            const bool quiet = LLDragFeedback::refusedQuietly() || text.empty();
            // Only a refusal for want of a modifier keeps its hint.
            LLDragFeedback::setChip(text, !quiet, hints);
            return;
        }

        EDragAction action = LLDragFeedback::reportedAction();
        std::string target = LLDragFeedback::reportedTarget();
        if (action == EDragAction::None)
        {
            // Nothing said what the drop does: a copy where the acceptance or the source says so.
            const bool copies = accept == ACCEPT_YES_COPY_SINGLE || accept == ACCEPT_YES_COPY_MULTI ||
                                source == LLToolDragAndDrop::SOURCE_LIBRARY || source == LLToolDragAndDrop::SOURCE_WORLD ||
                                source == LLToolDragAndDrop::SOURCE_NOTECARD;
            if (copies)
            {
                action = EDragAction::Copy;
            }
            else
            {
                LLDragFeedback::setChip(LLTrans::getString("DragVerdictDropHere"), false, hints);
                return;
            }
        }
        LLDragFeedback::setChip(verdict_text(action, target), false, hints);
    }

    ECursorType cursorFor(EAcceptance accept, ECursorType cursor, U32 cargo_count)
    {
        if (accept != ACCEPT_YES_MULTI && accept != ACCEPT_YES_SINGLE)
        {
            return cursor;
        }
        if (cursor != UI_CURSOR_ARROWDRAG && cursor != UI_CURSOR_ARROWDRAGMULTI)
        {
            return cursor;
        }
        const EDragAction action = LLDragFeedback::reportedAction();
        if (action == EDragAction::Copy || action == EDragAction::Link)
        {
            return cargo_count > 1 ? UI_CURSOR_ARROWCOPYMULTI : UI_CURSOR_ARROWCOPY;
        }
        return cursor;
    }
}
