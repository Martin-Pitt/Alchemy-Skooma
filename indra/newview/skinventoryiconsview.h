/**
 * @file skinventoryiconsview.h
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

#ifndef SK_SKINVENTORYICONSVIEW_H
#define SK_SKINVENTORYICONSVIEW_H

#include "skinventoryflatview.h"

class LLViewerFetchedTexture;

class SKInventoryIconsView : public SKInventoryFlatView
{
public:
    AL_VIEW_TYPE(SKInventoryIconsView, SKInventoryFlatView);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    static constexpr S32 MIN_TILE = 64;
    static constexpr S32 MAX_TILE = 180;
    static constexpr S32 DEFAULT_TILE = 96;

    ~SKInventoryIconsView();

    void setTileSize(S32 size);
    S32 getTileSize() const { return mTileSize; }

    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onVisibilityChange(bool new_visibility) override;

protected:
    SKInventoryIconsView(const Params& p);
    friend class LLUICtrlFactory;

    // One line of the grid: a run of tiles, or a header or the paging row across the whole width.
    struct Line
    {
        S32 mTop = 0;       // pixels down from the grid's top
        S32 mHeight = 0;
        S32 mFirst = 0;     // the first row on the line
        S32 mCount = 0;
        bool mTiles = true;
    };

    void layoutRows() override;
    void drawContent() override;
    S32 rowAt(S32 x, S32 y, bool* on_name) const override;
    void scrollToRow(S32 row) override;
    S32 rowFromKey(KEY key, MASK mask) const override;
    LLRect nameRect(S32 row, bool& multiline) const override;

    S32 viewWidth() const;
    S32 tileHeight() const;
    S32 lineAt(S32 y) const;
    LLRect tileRect(S32 row) const;
    S32 tileLineFrom(S32 line, S32 step) const;
    void drawTile(const SKInventoryRow& row, const LLRect& rect, bool selected, bool focused);
    void releaseHiddenTextures();

    S32 mTileSize;
    S32 mPerLine;
    std::vector<Line> mLines;
    std::vector<S32> mRowLine;      // each row's line
    std::vector<std::pair<std::string, std::string>> mNameLines;   // each row's name cut to two lines

    // Referenced only for tiles in view; each asks for the pixels it draws, with no boost.
    std::map<LLUUID, LLPointer<LLViewerFetchedTexture>> mTextures;
    std::vector<LLUUID> mVisibleImages;
};

#endif // SK_SKINVENTORYICONSVIEW_H
