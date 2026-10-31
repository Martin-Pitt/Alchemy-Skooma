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
#include "skinventoryexplorerdrag.h"

#include <deque>
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

// Finds the panel the bridges are built over (the floater's hidden bridge panel), asked at action time.
static bridge_panel_getter_t sBridgePanelGetter;

void setBridgePanelGetter(const bridge_panel_getter_t& getter)
{
    sBridgePanelGetter = getter;
}

static LLInventoryPanel* bridge_panel()
{
    return sBridgePanelGetter ? sBridgePanelGetter() : nullptr;
}

// A folder's stock bridge, kept alive a while: its paste may ask first and answers on the bridge itself.
static LLFolderBridge* folder_bridge(const LLUUID& dest_id)
{
    static std::deque<std::unique_ptr<LLInvFVBridge>> kept;
    LLInventoryPanel* panel = bridge_panel();
    if (!panel || !gInventory.getCategory(dest_id))
    {
        return nullptr;
    }
    LLInvFVBridge* bridge = LLInvFVBridge::createBridge(LLAssetType::AT_CATEGORY, LLAssetType::AT_CATEGORY, LLInventoryType::IT_CATEGORY,
                                                       panel, nullptr, nullptr, dest_id);
    if (!bridge)
    {
        return nullptr;
    }
    kept.emplace_back(bridge);
    if (kept.size() > 8)
    {
        kept.pop_front();
    }
    return dynamic_cast<LLFolderBridge*>(bridge);
}

bool canPaste(const LLUUID& dest_id)
{
    LLFolderBridge* bridge = LLClipboard::instance().hasContents() ? folder_bridge(dest_id) : nullptr;
    return bridge && bridge->isClipboardPasteable();
}

void paste(const LLUUID& dest_id)
{
    // LLFolderBridge::pasteFromClipboard, with its RLVa, outfit, landmark and Marketplace rules.
    if (LLFolderBridge* bridge = folder_bridge(dest_id))
    {
        bridge->performAction(&gInventory, "paste");
    }
}

bool canPasteLink(const LLUUID& dest_id)
{
    static LLCachedControl<bool> inventory_linking(gSavedSettings, "InventoryLinking", true);
    LLFolderBridge* bridge = (inventory_linking && LLClipboard::instance().hasContents()) ? folder_bridge(dest_id) : nullptr;
    return bridge && bridge->isClipboardPasteableAsLink();
}

void pasteLink(const LLUUID& dest_id)
{
    if (LLFolderBridge* bridge = folder_bridge(dest_id))
    {
        bridge->performAction(&gInventory, "paste_link");
    }
}

EClipboardMark clipboardMark(const LLUUID& id)
{
    const LLClipboard& clipboard = LLClipboard::instance();
    if (!clipboard.hasContents() || id.isNull())
    {
        return MARK_NONE;
    }
    if (clipboard.isOnClipboard(id))
    {
        return clipboard.isCutMode() ? MARK_CUT : MARK_COPY;
    }
    // What a cut folder holds dims with it, as LLFolderViewItem::isFadeItem has it.
    if (clipboard.isCutMode())
    {
        const LLInventoryObject* object = gInventory.getObject(id);
        for (LLUUID parent_id = object ? object->getParentUUID() : LLUUID::null; parent_id.notNull();)
        {
            if (clipboard.isOnClipboard(parent_id))
            {
                return MARK_CUT;
            }
            const LLViewerInventoryCategory* parent = gInventory.getCategory(parent_id);
            parent_id = parent ? parent->getParentUUID() : LLUUID::null;
        }
    }
    return MARK_NONE;
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
    SKInventoryExplorerDrag::recordFavorites(ids, favorite);
}

// Creations asked for here: the folder each lands in, and when it was asked.
static std::vector<std::pair<LLUUID, F64>> s_pending_creations;
constexpr F64 CREATION_WAIT_SECONDS = 60.0;

void create(const LLUUID& dest_id, const std::string& kind, const std::function<void(const LLUUID&)>& created_cb)
{
    const LLUUID folder_id = folderOf(dest_id).notNull() ? folderOf(dest_id) : dest_id;
    // Folders arrive through AIS, which the stock "show new inventory" does not watch.
    if (kind != "category")
    {
        s_pending_creations.emplace_back(folder_id, LLTimer::getTotalSeconds());
    }
    menu_create_inventory_item(nullptr, folder_id, LLSD(kind), LLUUID::null, created_cb);
}

bool claimCreated(const LLUUID& id)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    const F64 now = LLTimer::getTotalSeconds();
    std::erase_if(s_pending_creations, [now](const auto& pending) { return now - pending.second > CREATION_WAIT_SECONDS; });
    if (!obj)
    {
        return false;
    }
    for (auto it = s_pending_creations.begin(); it != s_pending_creations.end(); ++it)
    {
        if (it->first == obj->getParentUUID())
        {
            s_pending_creations.erase(it);
            return true;
        }
    }
    return false;
}

void bridgeAction(const LLUUID& id, const std::string& action)
{
    LLInventoryPanel* panel = bridge_panel();
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

LLHandle<LLContextMenu> SKInventoryExplorerContextMenu::sOptionsMenu;

// static
void SKInventoryExplorerContextMenu::closeOptionsMenu()
{
    if (LLContextMenu* menu = sOptionsMenu.get())
    {
        menu->die();
    }
    sOptionsMenu.markDead();
}

// static
void SKInventoryExplorerContextMenu::showOptionsMenu(const std::string& filename, const SKExplorerOptions& options, LLView* spawn, S32 x, S32 y)
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    register_options(options, registrar, enable_registrar);
    // One at a time, as LLListContextMenu::show keeps its menu.
    closeOptionsMenu();
    LLContextMenu* menu = createFromFile(filename);
    if (menu)
    {
        sOptionsMenu = menu->getHandle();
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
                  { SKInventoryExplorerActions::create(mUUIDs.front(), param.asString(), mCreatedCallback); });
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

void SKInventoryExplorerContextMenu::show(LLView* spawning_view, const uuid_vec_t& uuids, S32 x, S32 y)
{
    mSpawnView = spawning_view ? spawning_view->getHandle() : LLHandle<LLView>();
    mSpawnX = x;
    mSpawnY = y;
    LLListContextMenu::show(spawning_view, uuids, x, y);
}

void SKInventoryExplorerContextMenu::placeNearSpawn(LLFloater* floater)
{
    LLView* spawn = mSpawnView.get();
    if (!floater || !spawn || !gFloaterView || floater->getHost() || floater->getParent() != gFloaterView)
    {
        return;
    }
    constexpr S32 GAP = 16;
    const LLRect screen = gFloaterView->getSnapRect();
    const S32 width = floater->getRect().getWidth();
    const S32 height = floater->getRect().getHeight();
    S32 x = 0;
    S32 y = 0;
    spawn->localPointToOtherView(mSpawnX, mSpawnY, &x, &y, gFloaterView);
    // Beside the item where the screen has room, else beside the Explorer on the side with room.
    S32 left = x + GAP;
    if (left + width > screen.mRight)
    {
        left = x - GAP - width;
    }
    if (left < screen.mLeft)
    {
        LLRect explorer = spawn->calcScreenRect();
        if (LLFloater* owner = spawn->getParentByType<LLFloater>())
        {
            explorer = owner->getRect();
        }
        left = (screen.mRight - explorer.mRight >= width + 4) ? explorer.mRight + 4 : explorer.mLeft - 4 - width;
    }
    LLRect rect(left, y + height / 2, left + width, y + height / 2 - height);
    rect.translate(0, llmin(0, screen.mTop - rect.mTop));
    rect.translate(0, llmax(0, screen.mBottom - rect.mBottom));
    rect.translate(llmax(0, screen.mLeft - rect.mLeft), 0);
    rect.translate(llmin(0, screen.mRight - rect.mRight), 0);
    floater->setOrigin(rect.mLeft, rect.mBottom);
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
        row->mIcons.push_back(icon("paste", string("tip_paste"), "Clipboard_Paste", SKInventoryExplorerActions::canPaste(first)));
    }
    else
    {
        const bool favorite = get_is_favorite(first);
        row->mIcons.push_back(icon("cut", string("tip_cut"), "Clipboard_Cut", SKInventoryExplorerActions::canCut(mUUIDs)));
        row->mIcons.push_back(icon("copy", string("tip_copy"), "Clipboard_Copy", SKInventoryExplorerActions::canCopy(mUUIDs)));
        row->mIcons.push_back(icon("paste", string("tip_paste"), "Clipboard_Paste",
                                   single_folder && SKInventoryExplorerActions::canPaste(SKInventoryExplorerActions::folderOf(first))));
        row->mIcons.push_back(icon("favorite", string(favorite ? "tip_unfavorite" : "tip_favorite"),
                                   favorite ? "Inv_Favorite_Star_Full" : "Inv_Favorite_Star_Content",
                                   gInventory.isObjectDescendentOf(first, gInventory.getRootFolderID())));
        row->mIcons.push_back(icon("delete", string("tip_delete"), "TrashItem_Off",
                                   SKInventoryExplorerActions::canDelete(mUUIDs, mListedFolderID)));
    }
    // A row with nothing to press (the empty space's Paste with an empty clipboard) is left out.
    if (std::none_of(row->mIcons.begin(), row->mIcons.end(), [](const SKMenuIconRow::Icon& i) { return i.mEnabled; }))
    {
        delete row;
        return;
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

// Whether anything under the folder can be worn, an object or a gesture; one walk that stops at the first.
static bool folder_has_wearables(const LLUUID& id)
{
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    std::vector<LLUUID> pending{ id };
    while (!pending.empty())
    {
        const LLUUID folder = pending.back();
        pending.pop_back();
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (items)
        {
            for (const LLViewerInventoryItem* item : *items)
            {
                const LLAssetType::EType type = item->getType();
                if (type == LLAssetType::AT_OBJECT || type == LLAssetType::AT_GESTURE || type == LLAssetType::AT_CLOTHING
                    || type == LLAssetType::AT_BODYPART)
                {
                    return true;
                }
            }
        }
        if (cats)
        {
            for (const LLViewerInventoryCategory* cat : *cats)
            {
                if (cat->getUUID() != trash_id)
                {
                    pending.push_back(cat->getUUID());
                }
            }
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
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)mUUIDs.size());
        std::string label = string("menu_title");
        LLStringUtil::format(label, args);
        title->setLabel(label);
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
        if (SKInventoryExplorerActions::canPasteLink(folder_id))
        {
            items.push_back("paste_link");
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
            if (mShowInFolderCallback)
            {
                items.push_back("show_in_folder");
            }
            items.push_back("rename");
            if (!SKInventoryExplorerActions::canRename(first))
            {
                disabled.push_back("rename");
            }
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
        if (mShowInFolderCallback)
        {
            items.push_back("show_in_folder");
        }
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
    if (!many && item && LLAssetType::lookupIsAssetIDKnowable(obj->getType()) && (item->getIsFullPerm() || gAgent.isGodlike()))
    {
        items.push_back("copy_asset_uuid");
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
    LLViewerInventoryItem* item = gInventory.getItem(id);

    if (action == "open" || action == "open_in_new_tab")
    {
        if (folder_id.notNull() && mOpenCallback)
        {
            mOpenCallback(folder_id, action == "open_in_new_tab");
        }
    }
    else if (action == "show_in_folder")
    {
        if (mShowInFolderCallback)
        {
            mShowInFolderCallback(id);
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
            SKInventoryExplorerActions::bridgeAction(target->getUUID(), "sound_play");
        }
        else if (target)
        {
            SKInventoryExplorerActions::bridgeAction(target->getUUID(), "playworld");
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
            SKInventoryExplorerActions::bridgeAction(target->getUUID(), "playlocal");
        }
    }
    else if (action == "teleport" || action == "about_landmark")
    {
        SKInventoryExplorerActions::bridgeAction(id, action == "teleport" ? "teleport" : "about");
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
        SKInventoryExplorerActions::bridgeAction(id, "play");
    }
    else if (action == "activate" || action == "deactivate")
    {
        SKInventoryExplorerActions::bridgeAction(id, action == "activate" ? "activate" : "deactivate");
    }
    else if (action == "apply_local" || action == "apply_parcel")
    {
        SKInventoryExplorerActions::bridgeAction(id, action == "apply_local" ? "apply_settings_local" : "apply_settings_parcel");
    }
    else if (action == "wear" || action == "wear_add")
    {
        // Batched, as LLInventoryAction::doToSelected wears a selection.
        wear_multiple(ids, action == "wear");
    }
    else if (action == "take_off" || action == "detach")
    {
        LLAppearanceMgr::instance().removeItemsFromAvatar(ids);
    }
    else if (action == "paste_link")
    {
        SKInventoryExplorerActions::pasteLink(mRootMode ? id : folder_id);
    }
    else if (action == "edit")
    {
        SKInventoryExplorerActions::bridgeAction(id, "edit");
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
        // As show_item_profile, then placed by the item.
        placeNearSpawn(LLFloaterReg::showInstance("item_properties", LLSD().with("id", gInventory.getLinkedItemID(id))));
    }
    else if (action == "thumbnail")
    {
        LLSD data;
        for (const LLUUID& selected : ids)
        {
            data.append(selected);
        }
        placeNearSpawn(LLFloaterReg::showInstance("change_item_thumbnail", data));
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
