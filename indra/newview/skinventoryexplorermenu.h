/**
 * @file skinventoryexplorermenu.h
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

#ifndef SK_SKINVENTORYEXPLORERMENU_H
#define SK_SKINVENTORYEXPLORERMENU_H

#include "lllistcontextmenu.h"

#include <functional>
#include <map>

class LLContextMenu;
class LLFloater;
class LLInventoryPanel;

// What the content views do to inventory, through the same model calls, free functions
// and notifications the Inventory floater's gallery and bridges use.
namespace SKInventoryExplorerActions
{
    // The folder a folder link points to, the folder itself, or null for an item.
    LLUUID folderOf(const LLUUID& id);

    // Opens an item as its bridge would; folders are the floater's to navigate.
    void openItem(const LLUUID& id);

    // Where the bridges allow a rename: not system folders, the Library, no-modify items or RLVa-locked folders.
    bool canRename(const LLUUID& id);
    // Saves a new name, true when it differs from the old one.
    bool applyName(const LLUUID& id, const std::string& name);

    bool canDelete(const uuid_vec_t& ids, const LLUUID& listed_folder_id);
    void deleteObjects(const uuid_vec_t& ids);

    // The clipboard, as LLInventoryGallery's cut, copy and paste use it.
    bool canCopy(const uuid_vec_t& ids);
    void copy(const uuid_vec_t& ids);
    bool canCut(const uuid_vec_t& ids);
    void cut(const uuid_vec_t& ids);
    // Pastes go through the folder's stock bridge (LLFolderBridge::pasteFromClipboard and its rules).
    bool canPaste(const LLUUID& dest_id);
    void paste(const LLUUID& dest_id);
    bool canPasteLink(const LLUUID& dest_id);
    void pasteLink(const LLUUID& dest_id);
    // How a row shows it is on the clipboard: cut rows dim, as the stock inventory's do, and copied ones go bold.
    enum EClipboardMark { MARK_NONE, MARK_CUT, MARK_COPY };
    EClipboardMark clipboardMark(const LLUUID& id);

    // Every selected object takes the opposite of the first one's favourite state.
    void toggleFavorite(const uuid_vec_t& ids);

    // One of the Inventory floater's "New" kinds (category, lsl, notecard, shirt, sky, ...) in the folder.
    // The callback gets the new object's id; the Inventory floater is not opened for it.
    void create(const LLUUID& dest_id, const std::string& kind,
                const std::function<void(const LLUUID&)>& created_cb = std::function<void(const LLUUID&)>());
    // True once for an item a create() here asked for, so the stock "show new inventory" passes it by.
    bool claimCreated(const LLUUID& id);

    // Finds the live inventory panel the stock bridges are built over, null while there is none.
    typedef std::function<LLInventoryPanel*()> bridge_panel_getter_t;
    void setBridgePanelGetter(const bridge_panel_getter_t& getter);
    // Runs a bridge's own action for an object, the bridge built over that panel.
    void bridgeAction(const LLUUID& id, const std::string& action);
}

// The view, group, sort and column options, set and checked by key ("sort:name", "group:type", "dir:asc",
// "view:icons", "col:type"); every menu that lists them goes through one of these.
struct SKExplorerOptions
{
    std::function<void(const std::string& option)> mSet;
    std::function<bool(const std::string& option)> mCheck;
};

// Modelled on LLInventoryGalleryContextMenu; the floater supplies navigation, rename and the options.
class SKInventoryExplorerContextMenu : public LLListContextMenu
{
public:
    typedef std::function<void(const LLUUID& id, bool new_tab)> open_callback_t;
    typedef std::function<void(const LLUUID& id)> id_callback_t;
    SKInventoryExplorerContextMenu(const open_callback_t& open_cb, const id_callback_t& rename_cb, const SKExplorerOptions& options)
    :   LLListContextMenu(),
        mOpenCallback(open_cb),
        mRenameCallback(rename_cb),
        mOptions(options),
        mRootMode(false)
    {}

    // The labels and tooltips the menu sets itself, from the floater's XUI strings.
    void setStrings(const std::map<std::string, std::string>& strings) { mStrings = strings; }

    // The folder listed, and whether the menu was opened over empty space (its id is then the folder's).
    void setListedFolder(const LLUUID& folder_id, bool root_mode) { mListedFolderID = folder_id; mRootMode = root_mode; }
    // Set while the menu is over search results: "Show in Folder" is offered and runs this.
    void setShowInFolderCallback(const id_callback_t& cb) { mShowInFolderCallback = cb; }
    // Gets each object New made, to select and rename it.
    void setCreatedCallback(const id_callback_t& cb) { mCreatedCallback = cb; }

    // Remembers where the menu opened, so Properties and Image… open by the item.
    void show(LLView* spawning_view, const uuid_vec_t& uuids, S32 x, S32 y) override;

    // A menu of options alone (the List's header menu, the toolbar's Sort and Group), shown under spawn at x, y.
    static void showOptionsMenu(const std::string& filename, const SKExplorerOptions& options, LLView* spawn, S32 x, S32 y);
    // Deletes the options menu, whose callbacks reach the floater that opened it.
    static void closeOptionsMenu();

protected:
    LLContextMenu* createMenu() override;

    void doToSelected(const std::string& action);
    void updateMenuItemsVisibility(LLContextMenu* menu);
    void addIconRow(LLContextMenu* menu);
    // Next to the point the menu opened at if the screen has room there, else beside the Explorer, on screen.
    void placeNearSpawn(LLFloater* floater);
    void addAttachmentPoints(LLContextMenu* menu);
    std::string string(const std::string& key) const
    {
        auto it = mStrings.find(key);
        return it != mStrings.end() ? it->second : key;
    }

    open_callback_t mOpenCallback;
    id_callback_t mRenameCallback;
    id_callback_t mShowInFolderCallback;
    id_callback_t mCreatedCallback;
    LLHandle<LLView> mSpawnView;
    S32 mSpawnX = 0;
    S32 mSpawnY = 0;
    SKExplorerOptions mOptions;
    LLUUID mListedFolderID;
    bool mRootMode;
    std::map<std::string, std::string> mStrings;
    static LLHandle<LLContextMenu> sOptionsMenu;   // the one options menu open, deleted before the next
};

#endif // SK_SKINVENTORYEXPLORERMENU_H
