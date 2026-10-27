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
#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llaudioengine.h"
#include "llclipboard.h"
#include "llenvironment.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llfriendcard.h"
#include "llgesturemgr.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorygallery.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllandmarkactions.h"
#include "llmarketplacefunctions.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llpreviewtexture.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "llviewerattachmenu.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "rlvactions.h"
#include "rlvlocks.h"

#include <fmt/format.h>

// The gallery menu's outfit action, defined in llinventorygallerymenu.cpp.
void modify_outfit(bool append, const LLUUID& cat_id, LLInventoryModel* model);

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
    if (!gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID()))
    {
        return false; // the Library is read-only
    }
    if (gInventory.getCategory(id))
    {
        return get_is_category_renameable(&gInventory, id);
    }
    // The checks LLItemBridge::isItemRenameable makes.
    const LLViewerInventoryItem* item = gInventory.getItem(id);
    if (!item || item->getInventoryType() == LLInventoryType::IT_CALLINGCARD || !item->isFinished())
    {
        return false;
    }
    const LLUUID inbox_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX);
    if (inbox_id.notNull() && gInventory.isObjectDescendentOf(id, inbox_id))
    {
        return false;
    }
    if (RlvActions::isRlvEnabled() && !RlvFolderLocks::instance().canRenameItem(id))
    {
        return false;
    }
    return item->getPermissions().allowModifyBy(gAgentID);
}

bool applyName(const LLUUID& id, const std::string& name)
{
    if (name.empty() || !canRename(id))
    {
        return false;
    }
    LLSD updates;
    updates["name"] = name;
    if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
    {
        if (cat->getName() == name)
        {
            return false;
        }
        update_inventory_category(id, updates, nullptr);
        return true;
    }
    LLViewerInventoryItem* item = gInventory.getItem(id);
    if (!item || item->getName() == name)
    {
        return false;
    }
    update_inventory_item(id, updates, nullptr);
    return true;
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


bool canCopy(const uuid_vec_t& ids)
{
    if (ids.empty())
    {
        return false;
    }
    for (const LLUUID& id : ids)
    {
        if (!LLInventoryGallery::isItemCopyable(id))
        {
            return false;
        }
    }
    return true;
}

void copy(const uuid_vec_t& ids)
{
    LLClipboard::instance().reset();
    for (const LLUUID& id : ids)
    {
        LLClipboard::instance().addToClipboard(id);
    }
}

bool canCut(const uuid_vec_t& ids)
{
    if (ids.empty())
    {
        return false;
    }
    for (const LLUUID& id : ids)
    {
        const bool removable = gInventory.getCategory(id) ? get_is_category_and_children_removable(&gInventory, id, true)
                                                          : get_is_item_removable(&gInventory, id, true);
        if (!removable)
        {
            return false;
        }
    }
    return true;
}

void cut(const uuid_vec_t& ids)
{
    LLClipboard::instance().reset();
    LLClipboard::instance().setCutMode(true);
    for (const LLUUID& id : ids)
    {
        LLClipboard::instance().addToClipboard(id);
    }
}

bool canPaste(const LLUUID& dest_id)
{
    if (!LLClipboard::instance().hasContents() || !gInventory.getCategory(dest_id)
        || !gInventory.isObjectDescendentOf(dest_id, gInventory.getRootFolderID()))
    {
        return false;
    }
    if (dest_id == gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT))
    {
        return false;
    }
    if (LLClipboard::instance().isCutMode())
    {
        return true;
    }
    uuid_vec_t objects;
    LLClipboard::instance().pasteFromClipboard(objects);
    for (const LLUUID& id : objects)
    {
        if (!LLInventoryGallery::isItemCopyable(id))
        {
            return false;
        }
    }
    return true;
}

void paste(const LLUUID& dest_id)
{
    // As LLInventoryGallery::paste(dest, ...) does it.
    if (!canPaste(dest_id))
    {
        return;
    }
    const LLUUID& marketplacelistings_id = gInventory.getMarketplaceListingsUUID();
    if (marketplacelistings_id.notNull() && gInventory.isObjectDescendentOf(dest_id, marketplacelistings_id))
    {
        return;
    }
    const bool is_cut_mode = LLClipboard::instance().isCutMode();
    uuid_vec_t objects;
    LLClipboard::instance().pasteFromClipboard(objects);
    for (const LLUUID& id : objects)
    {
        if (marketplacelistings_id.notNull() && gInventory.isObjectDescendentOf(id, marketplacelistings_id)
            && (LLMarketplaceData::instance().isInActiveFolder(id) || LLMarketplaceData::instance().isListedAndActive(id)))
        {
            return;
        }
        if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
        {
            if (is_cut_mode)
            {
                gInventory.changeCategoryParent(cat, dest_id, false);
            }
            else
            {
                copy_inventory_category(&gInventory, cat, dest_id);
            }
        }
        else if (LLViewerInventoryItem* item = gInventory.getItem(id))
        {
            if (is_cut_mode)
            {
                gInventory.changeItemParent(item, dest_id, false);
            }
            else if (item->getIsLinkType())
            {
                link_inventory_object(dest_id, id, nullptr);
            }
            else
            {
                copy_inventory_item(gAgent.getID(), item->getPermissions().getOwner(), item->getUUID(), dest_id, std::string(), nullptr);
            }
        }
    }
    LLClipboard::instance().setCutMode(false);
}

void toggleFavorite(const uuid_vec_t& ids)
{
    if (ids.empty())
    {
        return;
    }
    const bool favorite = !get_is_favorite(ids.front());
    for (const LLUUID& id : ids)
    {
        set_favorite(id, favorite);
    }
}

void create(const LLUUID& dest_id, const std::string& kind)
{
    menu_create_inventory_item(nullptr, dest_id, LLSD(kind));
}

void bridgeAction(LLInventoryPanel* panel, const LLUUID& id, const std::string& action)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!panel || !obj)
    {
        return;
    }
    const LLViewerInventoryItem* item = gInventory.getItem(id);
    std::unique_ptr<LLInvFVBridge> bridge(LLInvFVBridge::createBridge(obj->getType(), obj->getActualType(),
                                                                       item ? item->getInventoryType() : LLInventoryType::IT_CATEGORY,
                                                                       panel, nullptr, nullptr, id, item ? item->getFlags() : 0));
    if (bridge)
    {
        bridge->performAction(&gInventory, action);
    }
}

} // namespace SKInventoryExplorerActions

//
// The menu's icon row: Cut, Copy, Paste, Favourite, Delete.
//

class SKMenuIconRow final : public LLMenuItemGL
{
public:
    struct Icon
    {
        std::string mAction;
        std::string mToolTip;
        LLPointer<LLUIImage> mImage;
        bool mEnabled = true;
    };

    struct Params : public LLInitParam::Block<Params, LLMenuItemGL::Params>
    {
    };

    static constexpr S32 CELL = 28;
    static constexpr S32 ROW_HEIGHT = 26;

    std::vector<Icon> mIcons;
    std::function<void(const std::string&)> mAction;
    S32 mHover = -1;

    S32 iconAt(S32 x) const
    {
        const S32 index = x / CELL;
        return (index >= 0 && index < (S32)mIcons.size()) ? index : -1;
    }

    U32 getNominalHeight() const override { return ROW_HEIGHT; }
    U32 getNominalWidth() const override { return (U32)(CELL * mIcons.size()); }

    void draw() override
    {
        static LLUIColor hover_color = LLUIColorTable::instance().getColor("MenuItemHighlightBgColor");
        for (S32 i = 0; i < (S32)mIcons.size(); ++i)
        {
            const Icon& icon = mIcons[i];
            const S32 left = i * CELL + 2;
            if (i == mHover && icon.mEnabled)
            {
                gl_rect_2d(left, getRect().getHeight() - 2, left + CELL - 4, 2, hover_color.get());
            }
            if (icon.mImage.notNull())
            {
                const S32 size = 16;
                icon.mImage->draw(left + (CELL - 4 - size) / 2, (getRect().getHeight() - size) / 2, size, size,
                                  icon.mEnabled ? LLColor4::white : LLColor4(1.f, 1.f, 1.f, 0.3f));
            }
        }
    }

    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        mHover = iconAt(x);
        return LLMenuItemGL::handleHover(x, y, mask);
    }

    void onMouseLeave(S32 x, S32 y, MASK mask) override
    {
        mHover = -1;
        LLMenuItemGL::onMouseLeave(x, y, mask);
    }

    bool handleMouseDown(S32 x, S32 y, MASK mask) override { return true; }

    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        const S32 index = iconAt(x);
        if (index >= 0 && mIcons[index].mEnabled)
        {
            const std::string action = mIcons[index].mAction;
            auto callback = mAction;
            LLMenuGL::sMenuContainer->hideMenus();
            if (callback)
            {
                callback(action);
            }
        }
        return true;
    }

    bool handleToolTip(S32 x, S32 y, MASK mask) override
    {
        const S32 index = iconAt(x);
        if (index >= 0)
        {
            LLToolTipMgr::instance().show(mIcons[index].mToolTip);
            return true;
        }
        return false;
    }

protected:
    SKMenuIconRow(const Params& p) : LLMenuItemGL(p) {}
    friend class LLUICtrlFactory;
};

//
// SKInventoryExplorerContextMenu
//

static void register_options(const SKExplorerOptions& options, LLUICtrl::CommitCallbackRegistry::ScopedRegistrar& registrar,
                             LLUICtrl::EnableCallbackRegistry::ScopedRegistrar& enable_registrar)
{
    registrar.add("SKInvExplorer.Option", [options](LLUICtrl*, const LLSD& param)
                  {
                      if (options.mSet)
                      {
                          options.mSet(param.asString());
                      }
                  });
    enable_registrar.add("SKInvExplorer.OptionCheck", [options](LLUICtrl*, const LLSD& param)
                         { return options.mCheck && options.mCheck(param.asString()); });
}

// static
void SKInventoryExplorerContextMenu::showOptionsMenu(const std::string& filename, const SKExplorerOptions& options, LLView* spawn, S32 x, S32 y)
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    register_options(options, registrar, enable_registrar);
    LLContextMenu* menu = createFromFile(filename);
    if (menu)
    {
        menu->show(x, y);
        LLMenuGL::showPopup(spawn, menu, x, y);
    }
}

LLContextMenu* SKInventoryExplorerContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("SKInvExplorer.Do", [this](LLUICtrl*, const LLSD& param) { doToSelected(param.asString()); });
    registrar.add("SKInvExplorer.Create", [this](LLUICtrl*, const LLSD& param)
                  { SKInventoryExplorerActions::create(mUUIDs.front(), param.asString()); });
    registrar.add("SKInvExplorer.Upload", [this](LLUICtrl*, const LLSD& param)
                  { LLInventoryAction::fileUploadLocation(mUUIDs.front(), param.asString()); });
    registrar.add("SKInvExplorer.Attach", [this](LLUICtrl*, const LLSD& param)
                  { LLViewerAttachMenu::attachObjects(mUUIDs, param.asString()); });
    register_options(mOptions, registrar, enable_registrar);

    LLContextMenu* menu = createFromFile("menu_sk_inventory_explorer.xml");
    if (menu)
    {
        updateMenuItemsVisibility(menu);
        addIconRow(menu);
    }
    return menu;
}

void SKInventoryExplorerContextMenu::addIconRow(LLContextMenu* menu)
{
    const bool many = mUUIDs.size() > 1;
    SKMenuIconRow::Params p;
    p.name("icon_row");
    p.label("");
    SKMenuIconRow* row = LLUICtrlFactory::create<SKMenuIconRow>(p);
    auto icon = [](const std::string& action, const std::string& tip, const std::string& image, bool enabled)
    {
        SKMenuIconRow::Icon i;
        i.mAction = action;
        i.mToolTip = tip;
        i.mImage = LLUI::getUIImage(image);
        i.mEnabled = enabled;
        return i;
    };
    const LLUUID& first = mUUIDs.front();
    const bool single_folder = !many && SKInventoryExplorerActions::folderOf(first).notNull();
    if (mRootMode)
    {
        // Over empty space only Paste, into the open folder.
        row->mIcons.push_back(icon("paste", "Paste  (Ctrl+V)", "Icon_Paste", SKInventoryExplorerActions::canPaste(first)));
    }
    else
    {
        const bool favorite = get_is_favorite(first);
        row->mIcons.push_back(icon("cut", "Cut  (Ctrl+X)", "Clipboard_Cut", SKInventoryExplorerActions::canCut(mUUIDs)));
        row->mIcons.push_back(icon("copy", "Copy  (Ctrl+C)", "Icon_Copy", SKInventoryExplorerActions::canCopy(mUUIDs)));
        row->mIcons.push_back(icon("paste", "Paste  (Ctrl+V)", "Icon_Paste",
                                   single_folder && SKInventoryExplorerActions::canPaste(SKInventoryExplorerActions::folderOf(first))));
        row->mIcons.push_back(icon("favorite", favorite ? "Remove from Favorites" : "Add to Favorites",
                                   favorite ? "Inv_Favorite_Star_Full" : "Inv_Favorite_Star_Content",
                                   gInventory.isObjectDescendentOf(first, gInventory.getRootFolderID())));
        row->mIcons.push_back(icon("delete", "Delete  (Del)", "TrashItem_Off",
                                   SKInventoryExplorerActions::canDelete(mUUIDs, mListedFolderID)));
    }
    row->mAction = [this](const std::string& action) { doToSelected(action); };
    menu->insert(0, row);
}

void SKInventoryExplorerContextMenu::addAttachmentPoints(LLContextMenu* menu)
{
    // As LLObjectBridge::buildContextMenu fills them, each point attaching through LLViewerAttachMenu.
    LLMenuGL* attach_menu = menu->findChildMenuByName("Attach To", true);
    LLMenuGL* attach_hud_menu = menu->findChildMenuByName("Attach To HUD", true);
    if (!attach_menu || !attach_hud_menu || !isAgentAvatarValid())
    {
        return;
    }
    for (const auto& [index, attachment] : gAgentAvatarp->mAttachmentPoints)
    {
        LLMenuItemCallGL::Params p;
        const std::string& point_name = attachment->getName();
        std::string translated;
        const std::string label = LLTrans::findString(translated, point_name) ? translated : point_name;
        p.name = attachment->getIsHUDAttachment() ? label : fmt::format(FMT_STRING("{} ({})"), label, index);
        LLSD cbparams;
        cbparams["index"] = index;
        cbparams["label"] = p.name;
        p.on_click.function_name = "SKInvExplorer.Attach";
        p.on_click.parameter = LLSD(point_name);
        p.on_enable.function_name = "Attachment.Label";
        p.on_enable.parameter = cbparams;
        LLUICtrlFactory::create<LLMenuItemCallGL>(p, attachment->getIsHUDAttachment() ? attach_hud_menu : attach_menu);
    }
}

static bool folder_has_wearables(const LLUUID& id)
{
    LLFindWearables is_wearable;
    LLIsType is_object(LLAssetType::AT_OBJECT);
    LLIsType is_gesture(LLAssetType::AT_GESTURE);
    for (LLInventoryCollectFunctor* functor : std::initializer_list<LLInventoryCollectFunctor*>{ &is_wearable, &is_object, &is_gesture })
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendentsIf(id, cats, items, LLInventoryModel::EXCLUDE_TRASH, *functor);
        if (!items.empty())
        {
            return true;
        }
    }
    return false;
}

void SKInventoryExplorerContextMenu::updateMenuItemsVisibility(LLContextMenu* menu)
{
    const LLUUID& first = mUUIDs.front();
    const bool many = mUUIDs.size() > 1;
    const LLInventoryObject* obj = gInventory.getObject(first);
    if (!obj)
    {
        return;
    }

    menuentry_vec_t items;
    menuentry_vec_t disabled;

    // The title names a selection of several.
    if (many && !mRootMode)
    {
        items.push_back("title");
        LLMenuItemGL* title = menu->getChild<LLMenuItemGL>("title");
        title->setLabel(llformat("%d ITEMS", (S32)mUUIDs.size()));
        disabled.push_back("title");
    }

    const LLUUID root_id = gInventory.getRootFolderID();
    const bool is_agent_inventory = gInventory.isObjectDescendentOf(first, root_id);
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    const bool is_in_trash = gInventory.isObjectDescendentOf(first, trash_id);
    const bool is_link = obj->getIsLinkType();
    const LLUUID folder_id = SKInventoryExplorerActions::folderOf(first);
    const LLViewerInventoryCategory* cat = gInventory.getCategory(first);

    for (const char* option : { "options_separator", "View", "Group By", "Sort By" })
    {
        items.push_back(option);
    }

    if (mRootMode || (!many && folder_id.notNull()))
    {
        // A folder, or the open folder's own menu from empty space.
        const LLViewerInventoryCategory* folder = gInventory.getCategory(folder_id);
        const LLFolderType::EType type = folder ? folder->getPreferredType() : LLFolderType::FT_NONE;
        const bool is_system = LLFolderType::lookupIsProtectedType(type);
        if (!mRootMode)
        {
            items.push_back("open");
            items.push_back("open_in_new_tab");
            items.push_back("folder_separator");
        }
        if (folder_id == trash_id)
        {
            items.push_back("empty_trash");
        }
        if (type == LLFolderType::FT_LOST_AND_FOUND)
        {
            items.push_back("empty_lost_and_found");
        }
        const bool is_cof = type == LLFolderType::FT_CURRENT_OUTFIT;
        const bool is_in_friends = folder && LLFriendCardsManager::instance().isCategoryInFriendFolder(folder);
        if (is_agent_inventory && !is_in_trash && !is_cof && !is_in_friends && !is_link)
        {
            items.push_back("create_new");
            for (const char* entry : { "new_category", "new_separator", "new_lsl", "new_notecard", "new_gesture", "new_material", "New Clothes",
                                       "New Body Parts", "New Settings" })
            {
                items.push_back(entry);
            }
            for (const char* kind : { "shirt", "pants", "shoes", "socks", "jacket", "skirt", "gloves", "undershirt", "underpants", "alpha",
                                      "tattoo", "universal", "physics", "shape", "skin", "hair", "eyes", "sky", "water", "daycycle" })
            {
                items.push_back(std::string("new_") + kind);
            }
            if (!LLEnvironment::instance().isInventoryEnabled())
            {
                for (const char* kind : { "new_sky", "new_water", "new_daycycle" })
                {
                    disabled.push_back(kind);
                }
            }
            if (gAgent.getRegionCapability("UpdateMaterialAgentInventory").empty())
            {
                disabled.push_back("new_material");
            }
            items.push_back("upload_options");
            for (const char* entry : { "upload_texture", "upload_sound", "upload_animation", "upload_model", "upload_pbr_material",
                                       "upload_separator", "upload_bulk" })
            {
                items.push_back(entry);
            }
        }
        if (!mRootMode && is_agent_inventory && !is_in_trash && !is_system && folder_has_wearables(folder_id))
        {
            items.push_back("outfit_separator");
            items.push_back("replace_outfit");
            if (!LLAppearanceMgr::instance().getCanReplaceCOF(folder_id))
            {
                disabled.push_back("replace_outfit");
            }
            if (type != LLFolderType::FT_OUTFIT)
            {
                items.push_back("add_to_outfit");
                if (!LLAppearanceMgr::instance().getCanAddToCOF(folder_id))
                {
                    disabled.push_back("add_to_outfit");
                }
            }
            items.push_back("remove_from_outfit");
            if (!LLAppearanceMgr::getCanRemoveFromCOF(folder_id))
            {
                disabled.push_back("remove_from_outfit");
            }
        }
        if (!mRootMode)
        {
            items.push_back("common_separator");
            items.push_back("rename");
            if (!SKInventoryExplorerActions::canRename(first))
            {
                disabled.push_back("rename");
            }
            items.push_back("copy_uuid");
            items.push_back("copy_inventory_uuid");
            if (!is_link && cat)
            {
                items.push_back("thumbnail");
                if (!is_agent_inventory || is_in_trash)
                {
                    disabled.push_back("thumbnail");
                }
            }
        }
        hide_context_entries(*menu, items, disabled);
        return;
    }

    // Items, or several of anything.
    items.push_back("item_separator");
    LLViewerInventoryItem* item = gInventory.getItem(first);
    const LLViewerInventoryItem* target = item ? item->getLinkedItem() : nullptr;
    if (!target)
    {
        target = item;
    }
    if (!many && item)
    {
        const LLAssetType::EType type = target->getType();
        if (is_link)
        {
            items.push_back("find_original");
            items.push_back("open_original");
            if (LLAssetType::lookupIsLinkType(obj->getType()))
            {
                disabled.push_back("find_original");
                disabled.push_back("open_original");
            }
        }
        switch (type)
        {
            case LLAssetType::AT_TEXTURE:
                items.push_back("open_item");
                items.push_back("save_as");
                if (!target->checkPermissionsSet(PERM_ITEM_UNRESTRICTED))
                {
                    disabled.push_back("save_as");
                }
                break;
            case LLAssetType::AT_SOUND:
            case LLAssetType::AT_ANIMATION:
                items.push_back("play_inworld");
                items.push_back("play_locally");
                break;
            case LLAssetType::AT_LANDMARK:
                items.push_back("teleport");
                items.push_back("show_on_map");
                items.push_back("copy_slurl");
                items.push_back("about_landmark");
                break;
            case LLAssetType::AT_GESTURE:
                items.push_back("gesture_play");
                items.push_back(LLGestureMgr::instance().isGestureActive(target->getUUID()) ? "deactivate" : "activate");
                break;
            case LLAssetType::AT_SETTINGS:
                items.push_back("apply_local");
                items.push_back("apply_parcel");
                if (!LLEnvironment::instance().isInventoryEnabled())
                {
                    disabled.push_back("apply_parcel");
                }
                break;
            case LLAssetType::AT_NOTECARD:
            case LLAssetType::AT_LSL_TEXT:
            case LLAssetType::AT_MATERIAL:
                if (!is_link)
                {
                    items.push_back("open_item");
                }
                break;
            default:
                break;
        }
    }

    // Wearables and objects, every one of the selection.
    bool wearable = !mUUIDs.empty();
    bool all_objects = true;
    bool all_worn = true;
    for (const LLUUID& id : mUUIDs)
    {
        const LLViewerInventoryItem* it = gInventory.getItem(id);
        const LLViewerInventoryItem* linked = it ? it->getLinkedItem() : nullptr;
        if (!linked)
        {
            linked = it;
        }
        const LLAssetType::EType type = linked ? linked->getType() : LLAssetType::AT_NONE;
        if (type != LLAssetType::AT_OBJECT && type != LLAssetType::AT_CLOTHING && type != LLAssetType::AT_BODYPART)
        {
            wearable = false;
            break;
        }
        all_objects &= type == LLAssetType::AT_OBJECT;
        all_worn &= get_is_item_worn(linked->getUUID());
    }
    if (wearable && isAgentAvatarValid() && !is_in_trash)
    {
        if (all_worn)
        {
            items.push_back(all_objects ? "detach" : "take_off");
        }
        else
        {
            items.push_back("wear");
            items.push_back("wear_add");
            if (all_objects)
            {
                items.push_back("Attach To");
                items.push_back("Attach To HUD");
                addAttachmentPoints(menu);
                if (!gAgentAvatarp->canAttachMoreObjects())
                {
                    for (const char* entry : { "wear", "wear_add", "Attach To", "Attach To HUD" })
                    {
                        disabled.push_back(entry);
                    }
                }
            }
        }
        if (!many)
        {
            items.push_back("edit");
            if (!get_is_item_worn(target->getUUID()) || (all_objects && !get_is_item_editable(target->getUUID())))
            {
                disabled.push_back("edit");
            }
        }
    }

    items.push_back("common_separator");
    if (!many)
    {
        items.push_back("rename");
        if (!SKInventoryExplorerActions::canRename(first))
        {
            disabled.push_back("rename");
        }
        items.push_back("properties");
        if (!is_link)
        {
            items.push_back("thumbnail");
            if (!is_agent_inventory || is_in_trash)
            {
                disabled.push_back("thumbnail");
            }
        }
    }
    items.push_back("copy_uuid");
    items.push_back("copy_inventory_uuid");
    if (!many && item)
    {
        items.push_back("copy_asset_uuid");
        if (!LLAssetType::lookupIsAssetIDKnowable(obj->getType()) || !(item->getIsFullPerm() || gAgent.isGodlike()))
        {
            disabled.push_back("copy_asset_uuid");
            LLMenuItemGL* asset = menu->getChild<LLMenuItemGL>("copy_asset_uuid");
            asset->setLabel(asset->getLabel() + "  (Full perm only)");
        }
    }
    if (many && is_agent_inventory)
    {
        items.push_back("new_folder_from_selected");
        if (!is_only_items_selected(mUUIDs) && !is_only_cats_selected(mUUIDs))
        {
            disabled.push_back("new_folder_from_selected");
        }
    }
    hide_context_entries(*menu, items, disabled);
}

void SKInventoryExplorerContextMenu::doToSelected(const std::string& action)
{
    if (mUUIDs.empty())
    {
        return;
    }
    const uuid_vec_t ids = mUUIDs;
    const LLUUID id = ids.front();
    const LLUUID folder_id = SKInventoryExplorerActions::folderOf(id);
    LLInventoryPanel* panel = mPanelGetter ? mPanelGetter() : nullptr;
    LLViewerInventoryItem* item = gInventory.getItem(id);

    if (action == "open" || action == "open_in_new_tab")
    {
        if (folder_id.notNull() && mOpenCallback)
        {
            mOpenCallback(folder_id, action == "open_in_new_tab");
        }
    }
    else if (action == "open_item" || action == "open_original")
    {
        SKInventoryExplorerActions::openItem(id);
    }
    else if (action == "find_original")
    {
        show_item_original(id);
    }
    else if (action == "empty_trash")
    {
        gInventory.emptyFolderType("ConfirmEmptyTrash", LLFolderType::FT_TRASH);
    }
    else if (action == "empty_lost_and_found")
    {
        gInventory.emptyFolderType("ConfirmEmptyLostAndFound", LLFolderType::FT_LOST_AND_FOUND);
    }
    else if (action == "replace_outfit")
    {
        modify_outfit(false, folder_id, &gInventory);
    }
    else if (action == "add_to_outfit")
    {
        modify_outfit(true, folder_id, &gInventory);
    }
    else if (action == "remove_from_outfit")
    {
        if (const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id))
        {
            LLAppearanceMgr::instance().takeOffOutfit(cat->getLinkedUUID());
        }
    }
    else if (action == "save_as")
    {
        // As the gallery menu saves a texture.
        if (LLPreviewTexture* preview = LLFloaterReg::getTypedInstance<LLPreviewTexture>("preview_texture", id))
        {
            preview->openToSave();
            preview->saveAs();
        }
    }
    else if (action == "play_inworld")
    {
        const LLViewerInventoryItem* target = item ? item->getLinkedItem() : nullptr;
        target = target ? target : item;
        if (target && target->getType() == LLAssetType::AT_SOUND)
        {
            SKInventoryExplorerActions::bridgeAction(panel, target->getUUID(), "sound_play");
        }
        else if (target)
        {
            SKInventoryExplorerActions::bridgeAction(panel, target->getUUID(), "playworld");
        }
    }
    else if (action == "play_locally")
    {
        const LLViewerInventoryItem* target = item ? item->getLinkedItem() : nullptr;
        target = target ? target : item;
        if (target && target->getType() == LLAssetType::AT_SOUND)
        {
            if (gAudiop)
            {
                gAudiop->triggerSound(target->getAssetUUID(), gAgentID, 1.0f, LLAudioEngine::AUDIO_TYPE_UI);
            }
        }
        else if (target)
        {
            SKInventoryExplorerActions::bridgeAction(panel, target->getUUID(), "playlocal");
        }
    }
    else if (action == "teleport" || action == "about_landmark")
    {
        SKInventoryExplorerActions::bridgeAction(panel, id, action == "teleport" ? "teleport" : "about");
    }
    else if (action == "show_on_map")
    {
        auto show_on_map_cb = [](LLLandmark* landmark)
        {
            LLVector3d pos;
            if (landmark->getGlobalPos(pos) && !pos.isExactlyZero())
            {
                if (LLFloaterWorldMap* worldmap = LLFloaterWorldMap::getInstance())
                {
                    worldmap->trackLocation(pos);
                    LLFloaterReg::showInstance("world_map", "center");
                }
            }
        };
        if (LLLandmark* landmark = LLLandmarkActions::getLandmark(id, show_on_map_cb))
        {
            show_on_map_cb(landmark);
        }
    }
    else if (action == "copy_slurl")
    {
        auto copy_slurl_cb = [](LLLandmark* landmark)
        {
            LLVector3d pos;
            landmark->getGlobalPos(pos);
            if (pos.isExactlyZero())
            {
                LLNotificationsUtil::add("LandmarkLocationUnknown");
                return;
            }
            LLLandmarkActions::getSLURLfromPosGlobal(pos,
                                                     [](const std::string& slurl)
                                                     {
                                                         gViewerWindow->getWindow()->copyTextToClipboard(slurl);
                                                         LLSD args;
                                                         args["SLURL"] = slurl;
                                                         LLNotificationsUtil::add("CopySLURL", args);
                                                     },
                                                     true);
        };
        if (LLLandmark* landmark = LLLandmarkActions::getLandmark(id, copy_slurl_cb))
        {
            copy_slurl_cb(landmark);
        }
    }
    else if (action == "gesture_play")
    {
        SKInventoryExplorerActions::bridgeAction(panel, id, "play");
    }
    else if (action == "activate" || action == "deactivate")
    {
        SKInventoryExplorerActions::bridgeAction(panel, id, action == "activate" ? "activate" : "deactivate");
    }
    else if (action == "apply_local" || action == "apply_parcel")
    {
        SKInventoryExplorerActions::bridgeAction(panel, id, action == "apply_local" ? "apply_settings_local" : "apply_settings_parcel");
    }
    else if (action == "wear")
    {
        for (const LLUUID& selected : ids)
        {
            LLAppearanceMgr::instance().wearItemOnAvatar(selected, true, true);
        }
    }
    else if (action == "wear_add")
    {
        for (const LLUUID& selected : ids)
        {
            LLAppearanceMgr::instance().wearItemOnAvatar(selected, true, false);
        }
    }
    else if (action == "take_off" || action == "detach")
    {
        for (const LLUUID& selected : ids)
        {
            LLAppearanceMgr::instance().removeItemFromAvatar(selected);
        }
    }
    else if (action == "edit")
    {
        SKInventoryExplorerActions::bridgeAction(panel, id, "edit");
    }
    else if (action == "rename")
    {
        if (mRenameCallback)
        {
            mRenameCallback(id);
        }
    }
    else if (action == "properties")
    {
        show_item_profile(id);
    }
    else if (action == "thumbnail")
    {
        LLSD data;
        for (const LLUUID& selected : ids)
        {
            data.append(selected);
        }
        LLFloaterReg::showInstance("change_item_thumbnail", data);
    }
    else if (action == "copy_inventory_uuid")
    {
        gViewerWindow->getWindow()->copyTextToClipboard(id.asString());
    }
    else if (action == "copy_asset_uuid")
    {
        if (item)
        {
            gViewerWindow->getWindow()->copyTextToClipboard(item->getProtectedAssetUUID().asString());
        }
    }
    else if (action == "new_folder_from_selected")
    {
        // As LLInventoryAction::doToSelected does it for a folder view's selection.
        const LLInventoryObject* first_obj = gInventory.getObject(id);
        if (!first_obj)
        {
            return;
        }
        for (const LLUUID& selected : ids)
        {
            const LLInventoryObject* obj = gInventory.getObject(selected);
            if (!obj || obj->getParentUUID() != first_obj->getParentUUID())
            {
                LLNotificationsUtil::add("SameFolderRequired");
                return;
            }
        }
        LLSD args;
        args["DESC"] = LLTrans::getString("New Folder");
        LLNotificationsUtil::add("CreateSubfolder", args, LLSD(),
                                 [ids](const LLSD& notification, const LLSD& response)
                                 {
                                     if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                                     {
                                         std::string name = response["message"].asString();
                                         LLInventoryObject::correctInventoryName(name);
                                         move_items_to_new_subfolder(ids, name.empty() ? LLTrans::getString("New Folder") : name);
                                     }
                                 });
    }
    else if (action == "cut")
    {
        SKInventoryExplorerActions::cut(ids);
    }
    else if (action == "copy")
    {
        SKInventoryExplorerActions::copy(ids);
    }
    else if (action == "paste")
    {
        SKInventoryExplorerActions::paste(mRootMode ? id : folder_id);
    }
    else if (action == "favorite")
    {
        SKInventoryExplorerActions::toggleFavorite(ids);
    }
    else if (action == "delete")
    {
        if (SKInventoryExplorerActions::canDelete(ids, mListedFolderID))
        {
            SKInventoryExplorerActions::deleteObjects(ids);
        }
    }
}
