/**
 * @file skinventorypathbar.cpp
 * @brief Inventory Explorer's path bar: a crumb per folder down to the open one, each with a menu of its folders
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

#include "skinventorypathbar.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llinventorymodel.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llviewerwindow.h"
#include "skinventoryexplorerpopup.h"

static LLDefaultChildRegistry::Register<SKInventoryPathBar> r_sk_path_bar("sk_inventory_path_bar");

namespace
{
    constexpr S32 CRUMB_PAD = 6;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 ARROW_WIDTH = 12;
    constexpr S32 MIN_CRUMB = 30;
    constexpr F32 CURRENT_SHARE = 0.6f;
    constexpr S32 MENU_MIN_WIDTH = 160;
    constexpr S32 MENU_MAX_WIDTH = 360;
    constexpr F32 MENU_MAX_HEIGHT = 0.6f;

    // Subfolders, system folders first, then by name; folder links are not folders here.
    std::vector<LLViewerInventoryCategory*> subfolders(const LLUUID& folder_id)
    {
        std::vector<LLViewerInventoryCategory*> folders;
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, cats, items);
        if (cats)
        {
            folders.assign(cats->begin(), cats->end());
        }
        std::sort(folders.begin(), folders.end(), [](LLViewerInventoryCategory* a, LLViewerInventoryCategory* b)
                  {
                      const bool sa = LLFolderType::lookupIsProtectedType(a->getPreferredType());
                      const bool sb = LLFolderType::lookupIsProtectedType(b->getPreferredType());
                      if (sa != sb)
                      {
                          return sa;
                      }
                      return LLStringUtil::compareDict(a->getName(), b->getName()) < 0;
                  });
        return folders;
    }
}

SKInventoryPathBar::SKInventoryPathBar(const Params& p)
:   LLUICtrl(p),
    mHover(-1),
    mHoverArrow(false),
    mMenu(nullptr),
    mMenuCrumb(-1),
    mMenuClosedFrame(0),
    mMenuClosedCrumb(-1),
    mPressCrumb(-1),
    mPressX(0),
    mPressY(0)
{
}

SKInventoryPathBar::~SKInventoryPathBar()
{
    if (mMenu)
    {
        SKExplorerPopupList::destroy(mMenuHandle);
    }
}

void SKInventoryPathBar::setStrings(const std::string& arrow_tooltip, const std::string& no_subfolders)
{
    mArrowToolTip = arrow_tooltip;
    mNoSubfolders = no_subfolders;
}

void SKInventoryPathBar::setFolder(const LLUUID& folder_id)
{
    if (mMenu && folder_id != mFolderID)
    {
        mMenu->hide();
    }
    mFolderID = folder_id;
    readCrumbs();
}

void SKInventoryPathBar::refresh()
{
    readCrumbs();
}

void SKInventoryPathBar::hideMenu()
{
    if (mMenu)
    {
        mMenu->hide();
    }
}

void SKInventoryPathBar::readCrumbs()
{
    mCrumbs.clear();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLViewerInventoryCategory* cat = gInventory.getCategory(mFolderID);
    while (cat)
    {
        Crumb crumb;
        crumb.mID = cat->getUUID();
        crumb.mName = cat->getName();
        mCrumbs.insert(mCrumbs.begin(), crumb);
        cat = cat->getParentUUID().notNull() ? gInventory.getCategory(cat->getParentUUID()) : nullptr;
    }
    for (size_t i = 0; i < mCrumbs.size(); ++i)
    {
        Crumb& crumb = mCrumbs[i];
        if (i == 0)
        {
            const LLViewerInventoryCategory* root = gInventory.getCategory(crumb.mID);
            crumb.mIcon = LLUI::getUIImage(LLViewerFolderType::lookupIconName(root ? root->getPreferredType() : LLFolderType::FT_NONE, false));
        }
        const LLViewerInventoryCategory* folder = gInventory.getCategory(crumb.mID);
        // A folder not yet loaded keeps its arrow; its menu says when it has nothing.
        const bool loaded = folder && folder->getVersion() != LLViewerInventoryCategory::VERSION_UNKNOWN
                            && gInventory.isCategoryComplete(crumb.mID);
        crumb.mArrow = !loaded || !subfolders(crumb.mID).empty();
        crumb.mNatural = font->getWidth(crumb.mName) + 2 * CRUMB_PAD + (crumb.mIcon.notNull() ? ICON_SIZE + 4 : 0);
    }
    mHover = -1;
    if (!mMenu || !mMenu->isOpen() || mMenuCrumb >= (S32)mCrumbs.size())
    {
        mMenuCrumb = -1;
    }
    layout();
}

void SKInventoryPathBar::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    layout();
}

void SKInventoryPathBar::layout()
{
    const S32 count = (S32)mCrumbs.size();
    if (count == 0)
    {
        return;
    }
    S32 arrows = 0;
    S32 natural = 0;
    for (const Crumb& crumb : mCrumbs)
    {
        arrows += crumb.mArrow ? ARROW_WIDTH : 0;
        natural += crumb.mNatural;
    }
    const S32 room = llmax(0, getRect().getWidth() - arrows);
    std::vector<S32> widths(count);
    if (natural <= room)
    {
        for (S32 i = 0; i < count; ++i)
        {
            widths[i] = mCrumbs[i].mNatural;
        }
    }
    else
    {
        // The open folder keeps most of the room, up to 60%; the others share the rest, each at least 30.
        const S32 last = count - 1;
        S32 current = llmin(mCrumbs[last].mNatural, (S32)(room * CURRENT_SHARE));
        S32 rest = room - current;
        std::vector<S32> order;
        for (S32 i = 0; i < last; ++i)
        {
            order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [this](S32 a, S32 b) { return mCrumbs[a].mNatural < mCrumbs[b].mNatural; });
        S32 left = (S32)order.size();
        for (S32 i : order)
        {
            const S32 share = left > 0 ? rest / left : rest;
            widths[i] = llmin(mCrumbs[i].mNatural, llmax(MIN_CRUMB, share));
            rest -= widths[i];
            --left;
        }
        // What the others leave, or take past their minimum, goes to or comes from the open folder.
        current = llclamp(current + rest, llmin(MIN_CRUMB, mCrumbs[last].mNatural), mCrumbs[last].mNatural);
        widths[last] = current;
    }

    const S32 height = getRect().getHeight();
    S32 x = 0;
    for (S32 i = 0; i < count; ++i)
    {
        Crumb& crumb = mCrumbs[i];
        crumb.mRect = LLRect(x, height, x + widths[i], 0);
        x += widths[i];
        crumb.mArrowRect = crumb.mArrow ? LLRect(x, height, x + ARROW_WIDTH, 0) : LLRect();
        x += crumb.mArrow ? ARROW_WIDTH : 0;
    }
}

S32 SKInventoryPathBar::crumbAt(S32 x, S32 y, bool* on_arrow) const
{
    for (S32 i = 0; i < (S32)mCrumbs.size(); ++i)
    {
        if (mCrumbs[i].mRect.pointInRect(x, y))
        {
            if (on_arrow)
            {
                *on_arrow = false;
            }
            return i;
        }
        if (mCrumbs[i].mArrow && mCrumbs[i].mArrowRect.pointInRect(x, y))
        {
            if (on_arrow)
            {
                *on_arrow = true;
            }
            return i;
        }
    }
    return -1;
}

void SKInventoryPathBar::draw()
{
    static LLUIColor hover_bg = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor text = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLPointer<LLUIImage> arrow_right = LLUI::getUIImage("Arrow_Small_Right");
    static LLPointer<LLUIImage> arrow_down = LLUI::getUIImage("Arrow_Small_Down");

    // Every row of the floater is clipped to its width: the bar never widens it.
    LLLocalClipRect clip(getLocalRect());
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 height = getRect().getHeight();
    for (S32 i = 0; i < (S32)mCrumbs.size(); ++i)
    {
        const Crumb& crumb = mCrumbs[i];
        if (i == mHover && !mHoverArrow)
        {
            gl_rect_2d(crumb.mRect, hover_bg.get(), true);
        }
        S32 x = crumb.mRect.mLeft + CRUMB_PAD;
        if (crumb.mIcon.notNull())
        {
            crumb.mIcon->draw(x, (height - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
            x += ICON_SIZE + 4;
        }
        font->renderUTF8(crumb.mName, 0, (F32)x, (F32)(height / 2), text.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                         LLFontGL::NO_SHADOW, S32_MAX, llmax(0, crumb.mRect.mRight - CRUMB_PAD - x), nullptr, LLFontGL::ELLIPSIS_MIDDLE);
        if (crumb.mArrow)
        {
            const bool open = i == mMenuCrumb && mMenu && mMenu->isOpen();
            if (open || (i == mHover && mHoverArrow))
            {
                gl_rect_2d(crumb.mArrowRect, hover_bg.get(), true);
            }
            LLUIImage* image = open ? arrow_down.get() : arrow_right.get();
            if (image)
            {
                const S32 w = llmin(image->getWidth(), ARROW_WIDTH);
                const S32 h = llmin(image->getHeight(), height);
                image->draw(crumb.mArrowRect.mLeft + (ARROW_WIDTH - w) / 2, (height - h) / 2, w, h, text.get());
            }
        }
    }
    SKInventoryExplorerDrag::drawMark(this);
    LLUICtrl::draw();
}

void SKInventoryPathBar::toggleMenu(S32 index)
{
    // The press that closed this crumb's menu, through the popup layer, does not open it again.
    if (mMenuClosedCrumb == index && mMenuClosedFrame == LLFrameTimer::getFrameCount())
    {
        return;
    }
    if (mMenu && mMenu->isOpen())
    {
        const bool same = mMenuCrumb == index;
        mMenu->hide();
        if (same)
        {
            return;
        }
    }
    if (!mMenu)
    {
        mMenu = SKExplorerPopupList::create();
        mMenuHandle = mMenu->getHandle();
        mMenu->setCloseCallback([this]()
                                {
                                    mMenuClosedCrumb = mMenuCrumb;
                                    mMenuClosedFrame = LLFrameTimer::getFrameCount();
                                    mMenuCrumb = -1;
                                });
        mMenu->setPickCallback([this](S32 row)
                               {
                                   const LLUUID id = mMenu->getRows()[row].mValue.asUUID();
                                   mMenu->hide();
                                   if (mNavigateCallback && id.notNull())
                                   {
                                       mNavigateCallback(id);
                                   }
                               });
    }

    const Crumb& crumb = mCrumbs[index];
    const LLUUID on_path = index + 1 < (S32)mCrumbs.size() ? mCrumbs[index + 1].mID : LLUUID::null;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    std::vector<SKPopupRow> rows;
    S32 width = MENU_MIN_WIDTH;
    for (LLViewerInventoryCategory* cat : subfolders(crumb.mID))
    {
        SKPopupRow row;
        row.mIcon = LLUI::getUIImage(LLViewerFolderType::lookupIconName(cat->getPreferredType(), false));
        row.mLabel = cat->getName();
        row.mBold = cat->getUUID() == on_path;
        row.mValue = cat->getUUID();
        width = llmax(width, font->getWidth(row.mLabel) + 48);
        rows.push_back(row);
    }
    if (rows.empty())
    {
        SKPopupRow row;
        row.mLabel = mNoSubfolders;
        row.mEnabled = false;
        rows.push_back(row);
    }
    mMenu->setRows(rows);
    mMenuCrumb = index;
    const S32 max_height = (S32)(gViewerWindow->getWorldViewRectScaled().getHeight() * MENU_MAX_HEIGHT);
    // 8 px left of the arrow and 3 px under it.
    mMenu->showBelow(this, crumb.mArrowRect, llmin(width, MENU_MAX_WIDTH), max_height, -8, 3);
}

bool SKInventoryPathBar::handleMouseDown(S32 x, S32 y, MASK mask)
{
    bool on_arrow = false;
    const S32 index = crumbAt(x, y, &on_arrow);
    if (index < 0)
    {
        return LLUICtrl::handleMouseDown(x, y, mask);
    }
    if (on_arrow)
    {
        toggleMenu(index);
        return true;
    }
    // A crumb navigates on release, or drags its folder once the pointer moves.
    mPressCrumb = index;
    mPressX = x;
    mPressY = y;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SKInventoryPathBar::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mPressCrumb >= 0 && mPressCrumb < (S32)mCrumbs.size())
    {
        if (std::abs(x - mPressX) + std::abs(y - mPressY) >= SKInventoryExplorerDrag::DRAG_THRESHOLD)
        {
            const LLUUID id = mCrumbs[mPressCrumb].mID;
            mPressCrumb = -1;
            gFocusMgr.setMouseCapture(nullptr);
            // The inventory's root is not something to carry.
            if (gInventory.getCategory(id) && gInventory.getCategory(id)->getParentUUID().notNull())
            {
                SKInventoryExplorerDrag::begin({ id });
            }
        }
        return true;
    }
    bool on_arrow = false;
    mHover = crumbAt(x, y, &on_arrow);
    mHoverArrow = on_arrow;
    return LLUICtrl::handleHover(x, y, mask);
}

bool SKInventoryPathBar::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        const S32 index = mPressCrumb;
        mPressCrumb = -1;
        gFocusMgr.setMouseCapture(nullptr);
        bool on_arrow = false;
        if (index >= 0 && crumbAt(x, y, &on_arrow) == index && !on_arrow && mNavigateCallback)
        {
            make_ui_sound("UISndClickRelease");
            mNavigateCallback(mCrumbs[index].mID);
        }
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

void SKInventoryPathBar::onMouseCaptureLost()
{
    mPressCrumb = -1;
    LLUICtrl::onMouseCaptureLost();
}

void SKInventoryPathBar::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKInventoryPathBar::handleToolTip(S32 x, S32 y, MASK mask)
{
    bool on_arrow = false;
    const S32 index = crumbAt(x, y, &on_arrow);
    if (index < 0)
    {
        return LLUICtrl::handleToolTip(x, y, mask);
    }
    if (on_arrow)
    {
        LLStringUtil::format_map_t args;
        args["[FOLDER]"] = mCrumbs[index].mName;
        std::string tip = mArrowToolTip;
        LLStringUtil::format(tip, args);
        LLToolTipMgr::instance().show(tip);
    }
    else
    {
        LLToolTipMgr::instance().show(mCrumbs[index].mName);
    }
    return true;
}

bool SKInventoryPathBar::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                           EAcceptance* accept, std::string& tooltip_msg)
{
    const S32 index = crumbAt(x, y, nullptr);
    if (index < 0)
    {
        return false;
    }
    // A crumb takes the drop into its folder; it never springs open.
    SKInventoryExplorerDrag::dropOnFolder(mCrumbs[index].mID, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    SKInventoryExplorerDrag::mark(this, mCrumbs[index].mRect);
    return true;
}
