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

// What the content views do to inventory, through the same model calls and
// notifications the Inventory floater's gallery uses.
namespace SKInventoryExplorerActions
{
    // The folder a folder link points to, the folder itself, or null for an item.
    LLUUID folderOf(const LLUUID& id);

    // Opens an item as its bridge would; folders are the floater's to navigate.
    void openItem(const LLUUID& id);

    bool canRename(const LLUUID& id);
    void rename(const LLUUID& id);

    bool canDelete(const uuid_vec_t& ids, const LLUUID& listed_folder_id);
    void deleteObjects(const uuid_vec_t& ids);
}

// Modelled on LLInventoryGalleryContextMenu; the floater supplies navigation.
class SKInventoryExplorerContextMenu : public LLListContextMenu
{
public:
    typedef std::function<void(const LLUUID& id, bool new_tab)> open_callback_t;

    SKInventoryExplorerContextMenu(const open_callback_t& open_cb)
    :   LLListContextMenu(),
        mOpenCallback(open_cb),
        mRootMode(false)
    {}

    // The folder listed, and whether the menu was opened over empty space (its id is then the folder's).
    void setListedFolder(const LLUUID& folder_id, bool root_mode) { mListedFolderID = folder_id; mRootMode = root_mode; }

protected:
    LLContextMenu* createMenu() override;

    void doToSelected(const LLSD& userdata);
    bool enableItem(const LLSD& userdata);
    void updateVisibility(LLContextMenu* menu);

    open_callback_t mOpenCallback;
    LLUUID mListedFolderID;
    bool mRootMode;
};

#endif // SK_SKINVENTORYEXPLORERMENU_H
