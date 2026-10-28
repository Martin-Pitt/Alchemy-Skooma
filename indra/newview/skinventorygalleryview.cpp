/**
 * @file skinventorygalleryview.cpp
 * @brief Inventory Explorer's Gallery: a stage for the selected item beside a strip of the folder's contents
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

#include "skinventorygalleryview.h"

#include "llavatarnamecache.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llviewerinventory.h"
#include "llviewertexture.h"
#include "skinventoryiconsview.h"

static LLDefaultChildRegistry::Register<SKInventoryGalleryView> r_sk_inventory_gallery("sk_inventory_gallery");

namespace
{
    constexpr S32 GAP = 6;
    constexpr S32 CHIP_HEIGHT = 18;
    constexpr S32 CHIP_MARGIN = 8;
    constexpr S32 ICON_SIZE = 96;
    constexpr S32 PLAYER_MAX_WIDTH = 520;
    constexpr F32 SETTLE_SECONDS = 0.25f;

    // One 18 px chip style for every stage label; returns its rect.
    LLRect draw_chip(S32 left, S32 bottom, const std::string& text, const std::string& dim, S32 max_width)
    {
        static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
        static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const S32 text_w = font->getWidth(text);
        const S32 dim_w = dim.empty() ? 0 : font->getWidth(dim);
        const S32 width = llmin(max_width, text_w + dim_w + 12);
        const LLRect rect(left, bottom + CHIP_HEIGHT, left + width, bottom);
        gl_rect_2d(rect, LLColor4(0.f, 0.f, 0.f, 0.6f), true);
        const S32 room = width - 12;
        F32 right = 0.f;
        font->renderUTF8(text, 0, (F32)(left + 6), (F32)(bottom + CHIP_HEIGHT / 2), text_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, room, &right, true);
        if (!dim.empty() && text_w < room)
        {
            font->renderUTF8(dim, 0, right, (F32)(bottom + CHIP_HEIGHT / 2), dim_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER,
                             LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, room - text_w, nullptr, true);
        }
        return rect;
    }

    S32 chip_width(const std::string& text)
    {
        return LLFontGL::getFontSansSerifSmall()->getWidth(text) + 12;
    }
}

SKInventoryGalleryView::SKInventoryGalleryView(const Params& p)
:   LLUICtrl(p),
    mStrip(nullptr),
    mFetching(false),
    mHovered(false)
{
    mPlayer.setZoomable(true);
}

bool SKInventoryGalleryView::postBuild()
{
    mStrip = getChild<SKInventoryIconsView>("gallery_strip");
    mStrip->setTileSize(STRIP_TILE);
    mStrip->setBadges(false);
    return true;
}

void SKInventoryGalleryView::onVisibilityChange(bool new_visibility)
{
    if (!new_visibility)
    {
        mPlayer.stop();
        SKInventoryDetails::hideAspectMenu();
    }
    LLUICtrl::onVisibilityChange(new_visibility);
}

LLRect SKInventoryGalleryView::stageRect() const
{
    return LLRect(0, getRect().getHeight(), llmax(0, getRect().getWidth() - STRIP_WIDTH - GAP), 0);
}

bool SKInventoryGalleryView::overStage(S32 x, S32 y) const
{
    return stageRect().pointInRect(x, y);
}

void SKInventoryGalleryView::setItem(const LLUUID& item_id)
{
    if (item_id == mItemID)
    {
        return;
    }
    mItemID = item_id;
    mSettle.reset();
    mFetching = false;
    SKInventoryDetails::hideAspectMenu();
}

void SKInventoryGalleryView::draw()
{
    static LLUIColor deep_color = LLUIColorTable::instance().getColor("DkGray");

    // The strip keeps its scroll per folder.
    if (mStrip && mStrip->getFolder() != mStripFolder)
    {
        auto it = mStripScrolls.find(mStrip->getFolder());
        if (it != mStripScrolls.end())
        {
            mStrip->setScroll(it->second);
        }
        mStripFolder = mStrip->getFolder();
    }
    if (mStrip && mStripFolder.notNull())
    {
        mStripScrolls[mStripFolder] = mStrip->getScroll();
    }

    if (!mFetching && mItemID.notNull() && mSettle.getElapsedTimeF32() >= SETTLE_SECONDS)
    {
        mFetching = true;
    }
    const LLRect stage = stageRect();
    gl_rect_2d(stage, deep_color.get(), true);
    gl_rect_2d(stage, LLColor4::black, false);
    mAspectRect = LLRect();

    LLViewerInventoryItem* item = mItemID.notNull() ? gInventory.getItem(mItemID) : nullptr;
    LLViewerInventoryItem* target = item;
    if (item && item->getIsLinkType() && item->getLinkedItem())
    {
        target = item->getLinkedItem();
    }
    if (target)
    {
        LLLocalClipRect clip(stage);
        const LLUUID thumbnail = item->getThumbnailUUID().notNull() ? item->getThumbnailUUID() : target->getThumbnailUUID();
        // A texture shows itself; anything else with a thumbnail shows that, else its player.
        const bool texture = target->getType() == LLAssetType::AT_TEXTURE;
        mPlayer.setItem(mItemID, texture ? LLUUID::null : thumbnail);
        mPlayer.setFetching(mFetching);
        const LLRect inner(stage.mLeft + 8, stage.mTop - 8, stage.mRight - 8, stage.mBottom + CHIP_HEIGHT + 2 * CHIP_MARGIN);
        switch (mPlayer.getKind())
        {
            case SKInventoryPlayer::KIND_IMAGE:
                mPlayer.draw(stage, mHovered);
                break;
            case SKInventoryPlayer::KIND_NONE:
            {
                mPlayer.hide();
                SKInventoryRow row;
                if (SKInventoryFlatView::readRow(mItemID, SKInventoryDetails::labels(), row) && row.mIcon.notNull())
                {
                    row.mIcon->draw(stage.getCenterX() - ICON_SIZE / 2, stage.getCenterY() - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE);
                }
                break;
            }
            default:
            {
                const S32 width = llmin(PLAYER_MAX_WIDTH, inner.getWidth());
                const S32 height = llmin(inner.getHeight(), llmax(mPlayer.height(width), mPlayer.getKind() == SKInventoryPlayer::KIND_MATERIAL || mPlayer.getKind() == SKInventoryPlayer::KIND_LANDMARK ? llmin(width, inner.getHeight()) : 0));
                LLRect rect;
                rect.setCenterAndSize(inner.getCenterX(), inner.getCenterY(), width, height);
                mPlayer.draw(rect, mHovered);
                break;
            }
        }

        // Bottom left: the name and a dimmed "by Creator", no wider than 55% of the stage.
        std::string by;
        LLAvatarName av_name;
        if (target->getCreatorUUID().notNull() && LLAvatarNameCache::get(target->getCreatorUUID(), &av_name))
        {
            LLStringUtil::format_map_t args;
            args["[NAME]"] = av_name.getCompleteName();
            by = SKInventoryDetails::str("gallery_by");
            LLStringUtil::format(by, args);
        }
        draw_chip(stage.mLeft + CHIP_MARGIN, stage.mBottom + CHIP_MARGIN, item->getName(), by, (S32)(stage.getWidth() * 0.55f));

        // Bottom right: the aspect button, then the resolution (textures and photos).
        if (texture)
        {
            std::string resolution;
            LLViewerFetchedTexture* image = mPlayer.getTexture();
            if (image && image->getFullWidth() > 0)
            {
                LLStringUtil::format_map_t args;
                args["[W]"] = llformat("%d", image->getFullWidth());
                args["[H]"] = llformat("%d", image->getFullHeight());
                resolution = SKInventoryDetails::str(image->getDiscardLevel() == 0 ? "gallery_resolution_full" : "gallery_resolution");
                LLStringUtil::format(resolution, args);
            }
            S32 w = 1;
            S32 h = 1;
            SKInventoryDetails::aspectOf(target->getDescription(), w, h);
            const std::string aspect = llformat("%d:%d \xE2\x96\xBE", w, h);
            S32 right = stage.mRight - CHIP_MARGIN;
            if (!resolution.empty())
            {
                const S32 rw = chip_width(resolution);
                draw_chip(right - rw, stage.mBottom + CHIP_MARGIN, resolution, std::string(), rw);
                right -= rw + 4;
            }
            const S32 aw = chip_width(aspect);
            mAspectRect = draw_chip(right - aw, stage.mBottom + CHIP_MARGIN, aspect, std::string(), aw);
        }
    }
    else
    {
        mPlayer.hide();
    }
    LLUICtrl::draw();
}

bool SKInventoryGalleryView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    if (!overStage(x, y))
    {
        return LLUICtrl::handleMouseDown(x, y, mask);
    }
    // A press on the stage neither drags nor drag-selects.
    if (mAspectRect.pointInRect(x, y))
    {
        make_ui_sound("UISndClick");
        LLViewerInventoryItem* item = gInventory.getItem(mItemID);
        const LLUUID id = item && item->getIsLinkType() && item->getLinkedItem() ? item->getLinkedItem()->getUUID() : mItemID;
        if (SKInventoryDetails::canSetAspect(id))
        {
            SKInventoryDetails::showAspectMenu(this, mAspectRect, id, true);
        }
        return true;
    }
    mPlayer.handleMouseDown(x, y);
    return true;
}

bool SKInventoryGalleryView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (overStage(x, y))
    {
        mPlayer.handleDoubleClick(x, y);
        return true;
    }
    return LLUICtrl::handleDoubleClick(x, y, mask);
}

bool SKInventoryGalleryView::handleHover(S32 x, S32 y, MASK mask)
{
    mHovered = overStage(x, y);
    if (mHovered)
    {
        mPlayer.handleHover(x, y);
        if (mAspectRect.pointInRect(x, y))
        {
            return true;
        }
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SKInventoryGalleryView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = false;
    mPlayer.mouseLeave();
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKInventoryGalleryView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    if (overStage(x, y))
    {
        mPlayer.handleScrollWheel(x, y, delta.mClicks);
        return true;
    }
    return LLUICtrl::handleScrollWheel(x, y, delta);
}

bool SKInventoryGalleryView::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mAspectRect.pointInRect(x, y))
    {
        std::string why;
        LLToolTipMgr::instance().show(SKInventoryDetails::canSetAspect(mItemID, &why) ? SKInventoryDetails::str("tip_aspect") : why);
        return true;
    }
    std::string tip;
    if (overStage(x, y) && mPlayer.getToolTip(x, y, tip))
    {
        LLToolTipMgr::instance().show(tip);
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}
