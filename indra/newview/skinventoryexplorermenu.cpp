/**
 * @file skinventoryexplorermenu.cpp
 * @brief Inventory Explorer's action layer: the content views' context menu and the actions it shares
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

#include "skinventoryexplorermenu.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"

namespace SKInventoryExplorerActions
{

LLUUID folderOf(const LLUUID& id)
{
    if (gInventory.getCategory(id))
    {
        return id;
    }
    const LLViewerInventoryItem* item = gInventory.getItem(id);
    if (item && item->getIsLinkType())
    {
        const LLUUID& target = item->getLinkedUUID();
        if (gInventory.getCategory(target))
        {
            return target;
        }
    }
    return LLUUID::null;
}

void openItem(const LLUUID& id)
{
    LLViewerInventoryItem* item = gInventory.getItem(id);
    if (item)
    {
        LLInvFVBridgeAction::doAction(item->getType(), id, &gInventory);
    }
}

bool canRename(const LLUUID& id)
{
    if (gInventory.getCategory(id))
    {
        return get_is_category_renameable(&gInventory, id);
    }
    const LLViewerInventoryItem* item = gInventory.getItem(id);
    return item && gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())
        && (item->getIsLinkType() || item->getPermissions().allowModifyBy(gAgentID));
}

static void on_rename(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        return;
    }
    std::string new_name = response["new_name"].asString();
    LLStringUtil::trim(new_name);
    if (new_name.empty())
    {
        return;
    }

    const LLUUID id = notification["payload"]["id"].asUUID();
    LLSD updates;
    updates["name"] = new_name;
    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
    if (cat)
    {
        if (cat->getName() != new_name)
        {
            update_inventory_category(id, updates, nullptr);
        }
        return;
    }
    LLViewerInventoryItem* item = gInventory.getItem(id);
    if (item && item->getName() != new_name)
    {
        update_inventory_item(id, updates, nullptr);
    }
}

void rename(const LLUUID& id)
{
    LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj || !canRename(id))
    {
        return;
    }
    LLSD args;
    args["NAME"] = obj->getName();
    LLSD payload;
    payload["id"] = id;
    LLNotificationsUtil::add("RenameItem", args, payload, on_rename);
}

bool canDelete(const uuid_vec_t& ids, const LLUUID& listed_folder_id)
{
    if (ids.empty())
    {
        return false;
    }
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (listed_folder_id == trash_id || gInventory.isObjectDescendentOf(listed_folder_id, trash_id))
    {
        return false;
    }
    for (const LLUUID& id : ids)
    {
        if (gInventory.getCategory(id) ? !get_is_category_removable(&gInventory, id) : !get_is_item_removable(&gInventory, id, true))
        {
            return false;
        }
    }
    return true;
}

static void on_delete(const LLSD& notification, const LLSD& response, const uuid_vec_t& ids)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        return;
    }

    const bool has_worn = notification["payload"]["has_worn"].asBoolean();
    uuid_vec_t worn;
    uuid_vec_t item_deletion_list;
    uuid_vec_t cat_deletion_list;
    for (const LLUUID& id : ids)
    {
        if (gInventory.getCategory(id))
        {
            bool cat_has_worn = false;
            if (has_worn)
            {
                LLInventoryModel::cat_array_t cats;
                LLInventoryModel::item_array_t items;
                gInventory.collectDescendents(id, cats, items, false);
                for (LLViewerInventoryItem* item : items)
                {
                    if (!item->getIsLinkType() && get_is_item_worn(item))
                    {
                        worn.push_back(item->getUUID());
                        cat_has_worn = true;
                    }
                }
            }
            if (cat_has_worn)
            {
                cat_deletion_list.push_back(id);
            }
            else
            {
                gInventory.removeCategory(id);
            }
        }
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (item)
        {
            // A link is only removed; its target stays worn.
            if (has_worn && !item->getIsLinkType() && get_is_item_worn(item))
            {
                worn.push_back(id);
                item_deletion_list.push_back(id);
            }
            else
            {
                gInventory.removeItem(id);
            }
        }
    }

    if (!worn.empty())
    {
        // Moved to Trash once detached, as LLInventoryAction::onItemsRemovalConfirmation does.
        LLAppearanceMgr::instance().removeItemsFromAvatar(worn,
                                                          [item_deletion_list, cat_deletion_list]()
                                                          {
                                                              for (const LLUUID& id : item_deletion_list)
                                                              {
                                                                  gInventory.removeItem(id);
                                                              }
                                                              for (const LLUUID& id : cat_deletion_list)
                                                              {
                                                                  gInventory.removeCategory(id);
                                                              }
                                                          });
    }
}

static bool is_required_bodypart(const LLViewerInventoryItem* item)
{
    const LLWearableType::EType type = item->getWearableType();
    return type == LLWearableType::WT_SHAPE || type == LLWearableType::WT_SKIN || type == LLWearableType::WT_HAIR
        || type == LLWearableType::WT_EYES;
}

void deleteObjects(const uuid_vec_t& ids)
{
    bool has_worn = false;
    bool needs_replacement = false;
    for (const LLUUID& id : ids)
    {
        if (gInventory.getCategory(id))
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(id, cats, items, false);
            for (LLViewerInventoryItem* item : items)
            {
                if (!item->getIsLinkType() && get_is_item_worn(item))
                {
                    has_worn = true;
                    needs_replacement |= is_required_bodypart(item);
                }
            }
        }
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (item && !item->getIsLinkType() && get_is_item_worn(item))
        {
            has_worn = true;
            needs_replacement |= is_required_bodypart(item);
        }
    }

    if (needs_replacement)
    {
        LLNotificationsUtil::add("CantDeleteRequiredClothing");
    }
    else if (has_worn)
    {
        LLSD payload;
        payload["has_worn"] = true;
        LLNotificationsUtil::add("DeleteWornItems", LLSD(), payload,
                                 [ids](const LLSD& n, const LLSD& r) { on_delete(n, r, ids); });
    }
    else
    {
        if (!LLInventoryAction::sDeleteConfirmationDisplayed)
        {
            // Ask at least once a session, as the Inventory floater does.
            LLNotifications::instance().setIgnored("DeleteItems", false);
            LLInventoryAction::sDeleteConfirmationDisplayed = true;
        }
        LLSD args;
        args["QUESTION"] = LLTrans::getString("DeleteItem");
        LLNotificationsUtil::add("DeleteItems", args, LLSD(),
                                 [ids](const LLSD& n, const LLSD& r) { on_delete(n, r, ids); });
    }
}

} // namespace SKInventoryExplorerActions

LLContextMenu* SKInventoryExplorerContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("SKInvExplorer.DoToSelected", [this](LLUICtrl*, const LLSD& param) { doToSelected(param); });
    enable_registrar.add("SKInvExplorer.Enable", [this](LLUICtrl*, const LLSD& param) { return enableItem(param); });

    LLContextMenu* menu = createFromFile("menu_sk_inventory_explorer.xml");
    if (menu)
    {
        updateVisibility(menu);
    }
    return menu;
}

void SKInventoryExplorerContextMenu::updateVisibility(LLContextMenu* menu)
{
    const bool single_folder = mUUIDs.size() == 1 && SKInventoryExplorerActions::folderOf(mUUIDs.front()).notNull();
    menu->setItemVisible("open_in_new_tab", single_folder);
    menu->setItemVisible("rename", !mRootMode);
    menu->setItemVisible("delete", !mRootMode);
    menu->setItemVisible("edit_separator", !mRootMode);
}

bool SKInventoryExplorerContextMenu::enableItem(const LLSD& userdata)
{
    const std::string action = userdata.asString();
    if (mUUIDs.empty())
    {
        return false;
    }
    if (action == "open")
    {
        return mUUIDs.size() == 1;
    }
    if (action == "open_in_new_tab")
    {
        return mUUIDs.size() == 1;
    }
    if (action == "rename")
    {
        return mUUIDs.size() == 1 && SKInventoryExplorerActions::canRename(mUUIDs.front());
    }
    if (action == "delete")
    {
        return SKInventoryExplorerActions::canDelete(mUUIDs, mListedFolderID);
    }
    return false;
}

void SKInventoryExplorerContextMenu::doToSelected(const LLSD& userdata)
{
    const std::string action = userdata.asString();
    if (mUUIDs.empty())
    {
        return;
    }
    const LLUUID id = mUUIDs.front();
    if (action == "open" || action == "open_in_new_tab")
    {
        const LLUUID folder_id = SKInventoryExplorerActions::folderOf(id);
        if (folder_id.notNull())
        {
            if (mOpenCallback)
            {
                mOpenCallback(folder_id, action == "open_in_new_tab");
            }
        }
        else
        {
            SKInventoryExplorerActions::openItem(id);
        }
    }
    else if (action == "rename")
    {
        SKInventoryExplorerActions::rename(id);
    }
    else if (action == "delete")
    {
        if (SKInventoryExplorerActions::canDelete(mUUIDs, mListedFolderID))
        {
            SKInventoryExplorerActions::deleteObjects(mUUIDs);
        }
    }
}
