/**
 * @file skinventoryiconsview.cpp
 * @brief Inventory Explorer's Icons view: a grid of tiles drawn from the flat model
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

#include "skinventoryiconsview.h"

#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

static LLDefaultChildRegistry::Register<SKInventoryIconsView> r_sk_inventory_icons("sk_inventory_icons");

namespace
{
    constexpr S32 GAP = 4;
    constexpr S32 PAD = 6;          // inside a tile, above the art and below the name
    constexpr S32 NAME_GAP = 3;
    constexpr S32 HEADER_HEIGHT = 24;
    constexpr S32 WHEEL_PIXELS = 40;
    constexpr F32 ART_FRACTION = 0.62f;
    constexpr F32 TYPE_ICON_FRACTION = 0.48f;
    constexpr S32 BADGE_SIZE = 14;
    constexpr S32 WORN_DOT = 7;
    constexpr S32 RENAME_MIN_WIDTH = 120;
}

SKInventoryIconsView::SKInventoryIconsView(const Params& p)
:   SKInventoryFlatView(p),
    mTileSize(DEFAULT_TILE),
    mPerLine(1)
{
}

SKInventoryIconsView::~SKInventoryIconsView()
{
    mVisibleImages.clear();
    releaseHiddenTextures();
}

void SKInventoryIconsView::onVisibilityChange(bool new_visibility)
{
    // A hidden view keeps no references to its tiles' textures.
    if (!new_visibility)
    {
        mVisibleImages.clear();
        releaseHiddenTextures();
    }
    SKInventoryFlatView::onVisibilityChange(new_visibility);
}

void SKInventoryIconsView::setTileSize(S32 size)
{
    size = llclamp(size, MIN_TILE, MAX_TILE);
    if (size != mTileSize)
    {
        // The tile under the view's top stays there.
        const S32 line = lineAt(getRect().getHeight());
        const S32 row = line >= 0 ? mLines[line].mFirst : -1;
        mTileSize = size;
        layoutRows();
        if (row >= 0 && row < (S32)mRowLine.size())
        {
            mScrollbar->setDocPos(mLines[mRowLine[row]].mTop);
        }
    }
}

S32 SKInventoryIconsView::viewWidth() const
{
    return getRect().getWidth(); // the overlay scroll bar takes no gutter
}

S32 SKInventoryIconsView::tileHeight() const
{
    const S32 line = (S32)LLFontGL::getFontSansSerifSmall()->getLineHeight();
    return PAD + (S32)(mTileSize * ART_FRACTION) + NAME_GAP + 2 * line + PAD;
}

void SKInventoryIconsView::layoutRows()
{
    const S32 bar = mScrollbar->getRect().getWidth();
    mScrollbar->setRect(LLRect(getRect().getWidth() - bar, getRect().getHeight(), getRect().getWidth(), 0));
    const S32 width = getRect().getWidth();
    mPerLine = llmax(1, (width - GAP) / (mTileSize + GAP));

    mLines.clear();
    mRowLine.assign(mRows.size(), 0);
    const S32 tile_height = tileHeight();
    S32 top = GAP;
    for (S32 r = 0; r < (S32)mRows.size();)
    {
        Line line;
        line.mTop = top;
        line.mFirst = r;
        if (!mRows[r].isEntry())
        {
            // Headers and the paging row span the width.
            line.mTiles = false;
            line.mCount = 1;
            line.mHeight = HEADER_HEIGHT;
        }
        else
        {
            while (r + line.mCount < (S32)mRows.size() && line.mCount < mPerLine && mRows[r + line.mCount].isEntry())
            {
                ++line.mCount;
            }
            line.mHeight = tile_height + GAP;
        }
        for (S32 i = 0; i < line.mCount; ++i)
        {
            mRowLine[r + i] = (S32)mLines.size();
        }
        r += line.mCount;
        top += line.mHeight;
        mLines.push_back(line);
    }

    // Names are cut to two lines here, so drawing measures nothing.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const F32 name_width = (F32)(mTileSize - 2 * GAP);
    mNameLines.assign(mRows.size(), {});
    for (size_t r = 0; r < mRows.size(); ++r)
    {
        const std::string& name = mRows[r].mName;
        if (!mRows[r].isEntry())
        {
            continue;
        }
        const S32 first = font->maxDrawableBytes(name, name_width, S32_MAX, LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
        mNameLines[r].first = name.substr(0, first);
        std::string rest = name.substr(first);
        LLStringUtil::trimHead(rest);
        mNameLines[r].second = rest;
    }

    const S32 total = top;
    mScrollbar->setDocSize(total);
    mScrollbar->setPageSize(getRect().getHeight());
    mScrollbar->setVisible(total > getRect().getHeight());
    mScrollbar->setDocPos(mScrollbar->getDocPos());
}

S32 SKInventoryIconsView::lineAt(S32 y) const
{
    const S32 down = getRect().getHeight() - y + mScrollbar->getDocPos();
    auto it = std::upper_bound(mLines.begin(), mLines.end(), down, [](S32 value, const Line& line) { return value < line.mTop; });
    if (it == mLines.begin())
    {
        return -1;
    }
    const S32 index = (S32)(it - mLines.begin()) - 1;
    return down < mLines[index].mTop + mLines[index].mHeight ? index : -1;
}

LLRect SKInventoryIconsView::tileRect(S32 row) const
{
    const Line& line = mLines[mRowLine[row]];
    const S32 top = getRect().getHeight() - (line.mTop - mScrollbar->getDocPos());
    if (!line.mTiles)
    {
        return LLRect(GAP, top, viewWidth() - GAP, top - line.mHeight);
    }
    const S32 left = GAP + (row - line.mFirst) * (mTileSize + GAP);
    return LLRect(left, top, left + mTileSize, top - tileHeight());
}

S32 SKInventoryIconsView::rowAt(S32 x, S32 y, bool* on_name) const
{
    const S32 index = lineAt(y);
    if (index < 0)
    {
        return -1;
    }
    const Line& line = mLines[index];
    S32 row = line.mFirst;
    if (line.mTiles)
    {
        const S32 col = (x - GAP) / (mTileSize + GAP);
        if (x < GAP || col >= line.mCount)
        {
            return -1;
        }
        row = line.mFirst + col;
        if (!tileRect(row).pointInRect(x, y))
        {
            return -1;
        }
    }
    if (on_name)
    {
        bool multiline = false;
        *on_name = line.mTiles && nameRect(row, multiline).pointInRect(x, y);
    }
    return row;
}

void SKInventoryIconsView::scrollToRow(S32 row)
{
    if (row < 0 || row >= (S32)mRowLine.size())
    {
        return;
    }
    const Line& line = mLines[mRowLine[row]];
    const S32 view = getRect().getHeight();
    const S32 pos = mScrollbar->getDocPos();
    if (line.mTop < pos)
    {
        mScrollbar->setDocPos(line.mTop - GAP);
    }
    else if (line.mTop + line.mHeight > pos + view)
    {
        mScrollbar->setDocPos(line.mTop + line.mHeight - view);
    }
}

S32 SKInventoryIconsView::tileLineFrom(S32 line, S32 step) const
{
    for (S32 i = line + step; i >= 0 && i < (S32)mLines.size(); i += step)
    {
        if (mLines[i].mTiles)
        {
            return i;
        }
    }
    return -1;
}

S32 SKInventoryIconsView::rowFromKey(KEY key, MASK mask) const
{
    if (mCursor < 0 || mCursor >= (S32)mRowLine.size())
    {
        return firstEntry();
    }
    const S32 line = mRowLine[mCursor];
    const S32 col = mCursor - mLines[line].mFirst;
    auto in_line = [&](S32 target)
    {
        return target < 0 ? -1 : mLines[target].mFirst + llmin(col, mLines[target].mCount - 1);
    };
    switch (key)
    {
        case KEY_LEFT:
        {
            const S32 row = nextEntry(mCursor, -1);
            return row >= 0 ? row : mCursor;
        }
        case KEY_RIGHT:
        {
            const S32 row = nextEntry(mCursor, 1);
            return row >= 0 ? row : mCursor;
        }
        case KEY_UP:
        {
            const S32 row = in_line(tileLineFrom(line, -1));
            return row >= 0 ? row : mCursor;
        }
        case KEY_DOWN:
        {
            const S32 row = in_line(tileLineFrom(line, 1));
            return row >= 0 ? row : lastEntry();
        }
        case KEY_PAGE_UP:
        case KEY_PAGE_DOWN:
        {
            // Whole lines that fit, the same column.
            const S32 lines = llmax(1, getRect().getHeight() / llmax(1, tileHeight() + GAP));
            const S32 step = key == KEY_PAGE_UP ? -1 : 1;
            S32 target = line;
            for (S32 i = 0; i < lines; ++i)
            {
                const S32 next = tileLineFrom(target, step);
                if (next < 0)
                {
                    break;
                }
                target = next;
            }
            return in_line(target);
        }
        default:
            return -1;
    }
}

LLRect SKInventoryIconsView::nameRect(S32 row, bool& multiline) const
{
    multiline = true;
    const LLRect tile = tileRect(row);
    const S32 line = (S32)LLFontGL::getFontSansSerifSmall()->getLineHeight();
    const S32 top = tile.mTop - PAD - (S32)(mTileSize * ART_FRACTION) - NAME_GAP + 2;
    const S32 width = llmax(RENAME_MIN_WIDTH, tile.getWidth());
    const S32 left = llclamp(tile.getCenterX() - width / 2, 0, llmax(0, viewWidth() - width));
    return LLRect(left, top, left + width, top - 2 * line - 6);
}

bool SKInventoryIconsView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    mScrollbar->setDocPos(mScrollbar->getDocPos() + delta.mClicks * WHEEL_PIXELS);
    return true;
}

bool SKInventoryIconsView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(x, y, nullptr);
    if (row >= 0 && mRows[row].isEntry())
    {
        LLToolTipMgr::instance().show(mRows[row].mName);
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

void SKInventoryIconsView::releaseHiddenTextures()
{
    for (auto it = mTextures.begin(); it != mTextures.end();)
    {
        if (std::find(mVisibleImages.begin(), mVisibleImages.end(), it->first) == mVisibleImages.end())
        {
            it = mTextures.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SKInventoryIconsView::drawTile(const SKInventoryRow& row, const LLRect& rect, bool selected, bool focused)
{
    static LLUIColor sel_focus = LLUIColorTable::instance().getColor("EmphasisColor_35");
    static LLUIColor sel_blur = LLUIColorTable::instance().getColor("LtGray_35");
    static LLUIColor border_color = LLUIColorTable::instance().getColor("EmphasisColor");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor badge_bg = LLUIColorTable::instance().getColor("DkGray");
    static LLPointer<LLUIImage> link_badge = LLUI::getUIImage("Inv_Link");

    // Selection is a fill alone, never an outline.
    if (selected)
    {
        gl_rect_2d(rect, focused ? sel_focus.get() : sel_blur.get(), true);
    }

    const S32 art = (S32)(mTileSize * ART_FRACTION);
    const LLRect art_rect(rect.getCenterX() - art / 2, rect.mTop - PAD, rect.getCenterX() - art / 2 + art, rect.mTop - PAD - art);
    bool badge_type = false;
    if (row.mImageID.notNull())
    {
        auto it = mTextures.find(row.mImageID);
        if (it == mTextures.end())
        {
            // No boost: a thumbnail boost scales decoded images down to the drawn size, and any of these textures may be
            // shared with the world or a preview.
            LLViewerFetchedTexture* texture = LLViewerTextureManager::getFetchedTexture(row.mImageID, FTT_DEFAULT, MIPMAP_YES,
                                                                                        LLGLTexture::BOOST_NONE);
            it = mTextures.emplace(row.mImageID, texture).first;
        }
        mVisibleImages.push_back(row.mImageID);
        LLViewerFetchedTexture* texture = it->second;
        // The pixels the tile really covers, HiDPI included, asked for each draw as a world face asks.
        const S32 pixels = llceil(art * LLUI::getScaleFactor().mV[VX]);
        texture->setKnownDrawSize(pixels, pixels);
        texture->addTextureStats((F32)(pixels * pixels));
        // The picture at its own aspect, letterboxed in the square.
        S32 w = art;
        S32 h = art;
        if (row.mAspect > 1.f)
        {
            h = llmax(1, (S32)(art / row.mAspect));
        }
        else if (row.mAspect < 1.f)
        {
            w = llmax(1, (S32)(art * row.mAspect));
        }
        gl_draw_scaled_image(art_rect.getCenterX() - w / 2, art_rect.getCenterY() - h / 2, w, h, texture);
        badge_type = !row.mImageIsAsset && row.mAssetType != LLAssetType::AT_TEXTURE && row.mAssetType != LLAssetType::AT_MATERIAL;
    }
    else if (row.mIcon.notNull())
    {
        const S32 icon = (S32)(art * TYPE_ICON_FRACTION);
        row.mIcon->draw(art_rect.getCenterX() - icon / 2, art_rect.getCenterY() - icon / 2, icon, icon);
    }

    // A thumbnail on anything but a texture keeps its type as a corner badge; links show theirs opposite.
    if (badge_type && row.mIcon.notNull())
    {
        gl_rect_2d(art_rect.mLeft - 2, art_rect.mBottom + BADGE_SIZE + 2, art_rect.mLeft + BADGE_SIZE + 2, art_rect.mBottom - 2,
                   badge_bg.get(), true);
        row.mIcon->draw(art_rect.mLeft, art_rect.mBottom, BADGE_SIZE, BADGE_SIZE);
    }
    if (row.mIsLink && link_badge.notNull())
    {
        link_badge->draw(art_rect.mRight - BADGE_SIZE + 3, art_rect.mBottom - 3, BADGE_SIZE, BADGE_SIZE);
    }
    if (row.mIsWorn)
    {
        // No art for the worn dot yet, so a filled circle stands in.
        gGL.color4fv(border_color.get().mV);
        gl_circle_2d((F32)(art_rect.mLeft + WORN_DOT / 2), (F32)(art_rect.mTop - WORN_DOT / 2), WORN_DOT / 2.f, 12, true);
    }

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLColor4 color = row.mIsLink ? dim_color.get() : text_color.get();
    const U8 style = row.mIsLink ? LLFontGL::ITALIC : LLFontGL::NORMAL;
    const S32 line = (S32)font->getLineHeight();
    S32 y = art_rect.mBottom - NAME_GAP;
    const size_t index = &row - mRows.data();
    const auto& lines = mNameLines[index];
    font->renderUTF8(lines.first, 0, (F32)rect.getCenterX(), (F32)y, color, LLFontGL::HCENTER, LLFontGL::TOP, style, LLFontGL::NO_SHADOW,
                     S32_MAX, mTileSize - 2 * GAP, nullptr, true);
    if (!lines.second.empty())
    {
        font->renderUTF8(lines.second, 0, (F32)rect.getCenterX(), (F32)(y - line), color, LLFontGL::HCENTER, LLFontGL::TOP, style,
                         LLFontGL::NO_SHADOW, S32_MAX, mTileSize - 2 * GAP, nullptr, true);
    }
}

void SKInventoryIconsView::drawContent()
{
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor bright_color = LLUIColorTable::instance().getColor("White");
    static LLUIColor rule_color = LLUIColorTable::instance().getColor("White_10");
    static LLUIColor link_color = LLUIColorTable::instance().getColor("EmphasisColor");

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 width = viewWidth();
    const S32 height = getRect().getHeight();
    mVisibleImages.clear();
    if (mRows.empty())
    {
        const std::string& message = mLoading ? mLabels.mLoading : mLabels.mEmpty;
        font->renderUTF8(message, 0, width / 2, height - 40, dim_color.get(), LLFontGL::HCENTER, LLFontGL::VCENTER);
        releaseHiddenTextures();
        return;
    }

    const bool focused = hasFocus();
    LLLocalClipRect clip(LLRect(0, height, width, 0));
    // Only the lines in view are drawn.
    const S32 first = llmax(0, lineAt(height));
    const S32 pos = mScrollbar->getDocPos();
    for (S32 l = first; l < (S32)mLines.size() && mLines[l].mTop - pos < height; ++l)
    {
        const Line& line = mLines[l];
        const S32 top = height - (line.mTop - pos);
        if (!line.mTiles)
        {
            const SKInventoryRow& row = mRows[line.mFirst];
            const S32 middle = top - line.mHeight / 2;
            if (row.mKind == SKInventoryRow::KIND_HEADER)
            {
                font->renderUTF8(row.mName, 0, (F32)GAP, (F32)middle, bright_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER,
                                 LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX, width - 60, nullptr, true);
                const S32 name_width = llmin((S32)LLFontGL::getFontSansSerifSmallBold()->getWidthF32(row.mName), width - 60);
                font->renderUTF8(llformat("%d", row.mGroupCount), 0, 2 * GAP + name_width, middle, dim_color.get(),
                                 LLFontGL::LEFT, LLFontGL::VCENTER);
                gl_line_2d(GAP, top - line.mHeight + 2, width - GAP, top - line.mHeight + 2, rule_color.get());
            }
            else
            {
                font->renderUTF8(row.mName, 0, width / 2, middle, link_color.get(), LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
            continue;
        }
        for (S32 r = line.mFirst; r < line.mFirst + line.mCount; ++r)
        {
            if (focused && r == mCursor && !mSelected[r])
            {
                // The keyboard cursor on an unselected tile: a faint fill.
                static LLUIColor cursor_color = LLUIColorTable::instance().getColor("White_10");
                gl_rect_2d(tileRect(r), cursor_color.get(), true);
            }
            drawTile(mRows[r], tileRect(r), mSelected[r], focused);
        }
    }
    releaseHiddenTextures();
}

S32 SKInventoryIconsView::contentDown(S32 y) const
{
    return getRect().getHeight() - y + mScrollbar->getDocPos();
}

S32 SKInventoryIconsView::contentToLocal(S32 down) const
{
    return getRect().getHeight() - (down - mScrollbar->getDocPos());
}

void SKInventoryIconsView::rowsInBand(S32 top, S32 bottom, S32 left, S32 right, std::vector<S32>& rows) const
{
    // Lines are in order down the grid, so the band's first line is found by search.
    auto it = std::upper_bound(mLines.begin(), mLines.end(), top, [](S32 value, const Line& line) { return value < line.mTop; });
    if (it != mLines.begin())
    {
        --it;
    }
    for (; it != mLines.end() && it->mTop <= bottom; ++it)
    {
        if (!it->mTiles || it->mTop + tileHeight() < top)
        {
            continue;
        }
        for (S32 i = 0; i < it->mCount; ++i)
        {
            const S32 tile_left = GAP + i * (mTileSize + GAP);
            if (tile_left <= right && tile_left + mTileSize >= left)
            {
                rows.push_back(it->mFirst + i);
            }
        }
    }
}

LLRect SKInventoryIconsView::rowRect(S32 row) const
{
    return tileRect(row);
}

void SKInventoryIconsView::scrollPixels(S32 pixels)
{
    mScrollbar->setDocPos(mScrollbar->getDocPos() + pixels);
}
