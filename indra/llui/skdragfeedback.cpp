/**
 * @file skdragfeedback.cpp
 * @brief What a drag-and-drop shows: a ghost of the cargo, a verdict chip and the marks of its targets.
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

#include "linden_common.h"

#include "skdragfeedback.h"

#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lltexture.h"
#include "llui.h"
#include "lluicolortable.h"
#include "lluiimage.h"
#include "llview.h"

#include <algorithm>

namespace
{
    constexpr size_t MAX_ICONS = 3;
    constexpr S32 ICON_SIZE = 16;
    constexpr S32 CELL_PAD = 4;
    constexpr S32 STACK_DX = 4;
    constexpr S32 STACK_DY = 3;
    constexpr S32 THUMB_SIZE = 64;
    constexpr S32 CURSOR_DX = 14;
    constexpr S32 CURSOR_DY = 10;
    constexpr S32 GAP = 6;
    constexpr S32 DASH = 4;
    constexpr S32 DASH_GAP = 3;

    struct CargoIcon
    {
        LLPointer<LLUIImage> mIcon;
        LLPointer<LLUIImage> mBadge;
    };

    struct Mark
    {
        const LLView* mView = nullptr;
        LLRect mRect;
        EDragMark mKind = EDragMark::None;
    };

    struct State
    {
        bool mDragging = false;
        std::vector<LLUUID> mCargoIDs;
        std::vector<CargoIcon> mIcons;
        U32 mCount = 0;
        LLPointer<LLTexture> mThumbnail;
        F32 mAspect = 0.f;

        EDragAction mAction = EDragAction::None;
        std::string mTarget;
        std::string mRefusal;
        bool mQuiet = false;
        bool mBlocked = false;

        std::string mChip;
        bool mChipRefused = false;
        std::vector<std::string> mHints;

        Mark mMark;
    };

    State& state()
    {
        static State s;
        return s;
    }

    LLColor4 colour(const char* name, const LLColor4& fallback)
    {
        return LLUIColorTable::instance().getColor(name, fallback).get();
    }

    void draw_dashed_rect(const LLRect& r, const LLColor4& c)
    {
        auto run = [&c](S32 x0, S32 y0, S32 x1, S32 y1)
        {
            const S32 len = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            const S32 dx = (x1 > x0) - (x1 < x0);
            const S32 dy = (y1 > y0) - (y1 < y0);
            for (S32 at = 0; at < len; at += DASH + DASH_GAP)
            {
                const S32 end = std::min(at + DASH, len);
                gl_line_2d(x0 + dx * at, y0 + dy * at, x0 + dx * end, y0 + dy * end, c);
            }
        };
        run(r.mLeft, r.mTop - 1, r.mRight, r.mTop - 1);
        run(r.mLeft, r.mBottom, r.mRight, r.mBottom);
        run(r.mLeft, r.mBottom, r.mLeft, r.mTop);
        run(r.mRight - 1, r.mBottom, r.mRight - 1, r.mTop);
    }
}

void LLDragFeedback::begin(const std::vector<LLUUID>& cargo_ids)
{
    State& s = state();
    s = State();
    s.mDragging = true;
    s.mCargoIDs = cargo_ids;
    s.mCount = (U32)cargo_ids.size();
}

void LLDragFeedback::end()
{
    state() = State();
}

bool LLDragFeedback::dragging()
{
    return state().mDragging;
}

bool LLDragFeedback::classic()
{
    static LLCachedControl<bool> classic(*LLUI::getInstance()->mSettingGroups["config"], "SkoomaDragFeedbackClassic", false);
    return classic;
}

bool LLDragFeedback::shown()
{
    return state().mDragging && !classic();
}

void LLDragFeedback::addCargoIcon(const LLPointer<LLUIImage>& icon, const LLPointer<LLUIImage>& badge)
{
    State& s = state();
    if (s.mIcons.size() < MAX_ICONS)
    {
        s.mIcons.push_back({ icon, badge });
    }
}

void LLDragFeedback::setCargoCount(U32 count)
{
    state().mCount = count;
}

void LLDragFeedback::setThumbnail(const LLPointer<LLTexture>& texture, F32 aspect)
{
    state().mThumbnail = texture;
    state().mAspect = aspect;
}

bool LLDragFeedback::isCargo(const LLUUID& id)
{
    const std::vector<LLUUID>& ids = state().mCargoIDs;
    return id.notNull() && std::find(ids.begin(), ids.end(), id) != ids.end();
}

size_t LLDragFeedback::iconCount()
{
    return state().mIcons.size();
}

void LLDragFeedback::beginPass()
{
    State& s = state();
    s.mAction = EDragAction::None;
    s.mTarget.clear();
    s.mRefusal.clear();
    s.mQuiet = false;
    s.mBlocked = false;
    s.mMark = Mark();
}

void LLDragFeedback::block()
{
    State& s = state();
    s.mBlocked = true;
    s.mRefusal.clear();
    s.mQuiet = true;
}

bool LLDragFeedback::blocked()
{
    return state().mBlocked;
}

void LLDragFeedback::report(EDragAction action, std::string_view target_name)
{
    State& s = state();
    s.mAction = action;
    s.mTarget = target_name;
}

void LLDragFeedback::refuse(std::string_view message, bool quiet)
{
    State& s = state();
    s.mRefusal = message;
    s.mQuiet = quiet;
}

EDragAction LLDragFeedback::reportedAction()
{
    return state().mAction;
}

const std::string& LLDragFeedback::reportedTarget()
{
    return state().mTarget;
}

const std::string& LLDragFeedback::refusal()
{
    return state().mRefusal;
}

bool LLDragFeedback::refusedQuietly()
{
    return state().mQuiet;
}

void LLDragFeedback::setChip(const std::string& text, bool refused, const std::vector<std::string>& hints)
{
    State& s = state();
    s.mChip = text;
    s.mChipRefused = refused;
    s.mHints = hints;
}

const std::string& LLDragFeedback::chipText()
{
    return state().mChip;
}

void LLDragFeedback::mark(const LLView* view, const LLRect& local_rect, EDragMark kind)
{
    State& s = state();
    if (!s.mDragging || !view)
    {
        return;
    }
    // The first view of a pass keeps the mark; each cargo item it refuses takes the accept away.
    if (s.mMark.mView && (s.mMark.mView != view || kind == EDragMark::Accept || s.mMark.mKind == EDragMark::Refuse))
    {
        return;
    }
    s.mMark = { view, local_rect, kind };
}

EDragMark LLDragFeedback::markFor(const LLView* view, LLRect* local_rect)
{
    const State& s = state();
    if (!s.mDragging || !view || s.mMark.mView != view)
    {
        return EDragMark::None;
    }
    if (local_rect)
    {
        *local_rect = s.mMark.mRect;
    }
    return s.mMark.mKind;
}

void LLDragFeedback::unmark()
{
    state().mMark = Mark();
}

EDragMark LLDragFeedback::markKind(bool accepted, const std::string& tooltip_msg)
{
    if (accepted)
    {
        return EDragMark::Accept;
    }
    const State& s = state();
    if (s.mQuiet)
    {
        return EDragMark::Quiet;
    }
    return (!tooltip_msg.empty() || !s.mRefusal.empty()) ? EDragMark::Refuse : EDragMark::Quiet;
}

void LLDragFeedback::drawTargetMark(const LLRect& rect, EDragMark kind, bool fill)
{
    if (kind != EDragMark::Accept && kind != EDragMark::Refuse)
    {
        return;
    }
    gGL.getTextureSlot(0)->unbind();
    if (kind == EDragMark::Accept)
    {
        static const LLColor4 accept_fill(0.3f, 0.82f, 1.f, 0.25f);
        static const LLColor4 accept_line(0.3f, 0.82f, 1.f, 1.f);
        if (fill)
        {
            gl_rect_2d(rect, colour("DragAcceptFillColor", accept_fill), true);
        }
        const LLColor4 line = colour("DragAcceptOutlineColor", accept_line);
        gl_rect_2d(rect, line, false);
        gl_rect_2d(rect.mLeft + 1, rect.mTop - 1, rect.mRight - 1, rect.mBottom + 1, line, false);
    }
    else
    {
        static const LLColor4 refuse_line(0.71f, 0.33f, 0.33f, 1.f);
        draw_dashed_rect(rect, colour("DragRefuseOutlineColor", refuse_line));
    }
}

void LLDragFeedback::draw(S32 x, S32 y, const LLRect& window_rect)
{
    State& s = state();
    if (!shown() || !window_rect.pointInRect(x, y))
    {
        return;
    }

    static const LLColor4 black(0.f, 0.f, 0.f, 1.f);
    const LLColor4 cell_bg = colour("DragGhostBgColor", LLColor4(0.165f, 0.165f, 0.165f, 1.f));
    const LLColor4 cell_line = colour("DragGhostBorderColor", black);
    const LLColor4 badge_bg = colour("DragGhostBadgeColor", LLColor4(0.3f, 0.82f, 1.f, 1.f));
    const LLColor4 chip_bg = colour("DragChipBgColor", LLColor4(0.08f, 0.08f, 0.08f, 0.92f));
    const LLColor4 chip_text = colour("DragChipTextColor", LLColor4(0.8f, 0.8f, 0.8f, 1.f));
    const LLColor4 chip_refuse = colour("DragChipRefuseTextColor", LLColor4(1.f, 0.55f, 0.55f, 1.f));
    const LLColor4 chip_hint = colour("DragChipHintTextColor", LLColor4(0.62f, 0.64f, 0.64f, 1.f));
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    gGL.getTextureSlot(0)->unbind();
    const S32 left = x + CURSOR_DX;
    const S32 top = y - CURSOR_DY;
    S32 ghost_right = left;
    S32 ghost_bottom = top;

    // A single item with a picture: the picture at its aspect, the long side THUMB_SIZE.
    bool thumbed = false;
    if (s.mThumbnail.notNull() && s.mCount == 1)
    {
        F32 aspect = s.mAspect;
        if (aspect <= 0.f && s.mThumbnail->getWidth() > 0 && s.mThumbnail->getHeight() > 0)
        {
            aspect = (F32)s.mThumbnail->getWidth() / (F32)s.mThumbnail->getHeight();
        }
        if (aspect > 0.f)
        {
            const S32 w = aspect >= 1.f ? THUMB_SIZE : llmax(1, ll_round(THUMB_SIZE * aspect));
            const S32 h = aspect >= 1.f ? llmax(1, ll_round(THUMB_SIZE / aspect)) : THUMB_SIZE;
            const LLRect cell(left, top, left + w + 2, top - h - 2);
            gl_rect_2d(cell, cell_line, true);
            s.mThumbnail->setKnownDrawSize(w, h);
            gl_draw_scaled_image(cell.mLeft + 1, cell.mBottom + 1, w, h, s.mThumbnail.get());
            gGL.getTextureSlot(0)->unbind();
            ghost_right = cell.mRight;
            ghost_bottom = cell.mBottom;
            thumbed = true;
        }
    }

    if (!thumbed && !s.mIcons.empty())
    {
        const S32 cell = ICON_SIZE + 2 * CELL_PAD + 2;
        // The last icon first, so the first lies on top.
        for (S32 i = (S32)s.mIcons.size() - 1; i >= 0; --i)
        {
            const S32 cl = left + i * STACK_DX;
            const S32 ct = top - i * STACK_DY;
            const LLRect r(cl, ct, cl + cell, ct - cell);
            gl_rect_2d(r, cell_line, true);
            gl_rect_2d(r.mLeft + 1, r.mTop - 1, r.mRight - 1, r.mBottom + 1, cell_bg, true);
            const CargoIcon& icon = s.mIcons[i];
            if (icon.mIcon.notNull())
            {
                icon.mIcon->draw(r.mLeft + 1 + CELL_PAD, r.mBottom + 1 + CELL_PAD, ICON_SIZE, ICON_SIZE);
            }
            if (icon.mBadge.notNull())
            {
                icon.mBadge->draw(r.mLeft + 1 + CELL_PAD, r.mBottom + 1 + CELL_PAD, ICON_SIZE / 2 + 2, ICON_SIZE / 2 + 2);
            }
            gGL.getTextureSlot(0)->unbind();
            ghost_right = std::max(ghost_right, r.mRight);
            ghost_bottom = std::min(ghost_bottom, r.mBottom);
        }
        if (s.mCount > 1)
        {
            const std::string count = std::to_string(s.mCount);
            const S32 bw = font->getWidth(count) + 10;
            const S32 bh = font->getLineHeight() + 2;
            const S32 cr = left + cell + 8;
            const LLRect b(cr - bw, top + 6, cr, top + 6 - bh);
            gl_rect_2d(b, badge_bg, true);
            font->renderUTF8(count, 0, b.getCenterX(), b.mBottom + 1, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM);
            gGL.getTextureSlot(0)->unbind();
            ghost_right = std::max(ghost_right, b.mRight);
        }
    }

    // The chip: the verdict, then any modifier hints, beside the ghost.
    std::vector<std::pair<std::string, LLColor4>> lines;
    if (!s.mChip.empty())
    {
        lines.emplace_back(s.mChip, s.mChipRefused ? chip_refuse : chip_text);
    }
    for (const std::string& hint : s.mHints)
    {
        lines.emplace_back(hint, chip_hint);
    }
    if (lines.empty())
    {
        return;
    }
    const S32 line_h = font->getLineHeight() + 2;
    S32 width = 0;
    for (const auto& line : lines)
    {
        width = std::max(width, font->getWidth(line.first));
    }
    const S32 chip_w = width + 12;
    const S32 chip_h = (S32)lines.size() * line_h + 4;
    S32 chip_l = (ghost_right > left ? ghost_right : left) + GAP;
    S32 chip_t = (top + ghost_bottom) / 2 + line_h / 2 + 2;
    if (ghost_bottom == top)
    {
        chip_t = top;
    }
    // Kept inside the window: flipped to the pointer's left, then clamped.
    if (chip_l + chip_w > window_rect.mRight)
    {
        chip_l = x - CURSOR_DX - chip_w;
    }
    chip_l = llclamp(chip_l, window_rect.mLeft, std::max(window_rect.mLeft, window_rect.mRight - chip_w));
    chip_t = llclamp(chip_t, window_rect.mBottom + chip_h, window_rect.mTop);

    const LLRect chip(chip_l, chip_t, chip_l + chip_w, chip_t - chip_h);
    gl_rect_2d(chip, cell_line, true);
    gl_rect_2d(chip.mLeft + 1, chip.mTop - 1, chip.mRight - 1, chip.mBottom + 1, chip_bg, true);
    S32 line_top = chip.mTop - 2;
    for (const auto& line : lines)
    {
        font->renderUTF8(line.first, 0, chip.mLeft + 6, line_top, line.second, LLFontGL::LEFT, LLFontGL::TOP);
        line_top -= line_h;
    }
    gGL.getTextureSlot(0)->unbind();
}
