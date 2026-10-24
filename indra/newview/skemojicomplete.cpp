/**
 * @file skemojicomplete.cpp
 * @brief An inline list completing a typed :shortcode: to its emoji, for chat inputs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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

#include "llviewerprecompiledheaders.h"
#include "skemojicomplete.h"

#include "llemojihelper.h"
#include "llfontgl.h"
#include "llrender2dutils.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "lluiimage.h"
#include "llwindow.h"

namespace
{
constexpr S32 MAX_ROWS = 7;
constexpr S32 PAD = 4;       // between the rounded edge and the rows
constexpr S32 ROW_PAD = 3;   // above and below a row's text
constexpr S32 GLYPH_W = 26;  // the emoji's column
constexpr S32 GAP = 4;       // between the list and its editor
constexpr S32 MAX_W = 340;

LLFontGL* label_font() { return LLFontGL::getFontSansSerif(); }
LLFontGL* glyph_font() { return LLFontGL::getFontEmojiMedium(); }

S32 row_height()
{
    return llmax(label_font()->getLineHeight(), glyph_font()->getLineHeight()) + 2 * ROW_PAD;
}
}

SKEmojiComplete::SKEmojiComplete(LLTextEditor* editor)
    : LLView([]
        {
            LLView::Params p;
            p.name = "sk_emoji_complete";
            p.mouse_opaque = true;
            p.visible = false;
            p.follows.flags = FOLLOWS_NONE;
            return p;
        }())
    , mEditor(editor)
{
    setSoundFlags(LLView::SILENT);
}

// static
SKEmojiComplete* SKEmojiComplete::attach(LLTextEditor* editor)
{
    LLView* parent = editor ? editor->getParent() : nullptr;
    if (!parent)
    {
        return nullptr;
    }
    SKEmojiComplete* list = new SKEmojiComplete(editor);
    parent->addChild(list);
    editor->setShowEmojiShortcodeHelper(false);
    LLHandle<LLView> handle = list->getHandle();
    editor->setKeyInterceptor([handle](KEY key, MASK mask)
    {
        SKEmojiComplete* self = static_cast<SKEmojiComplete*>(handle.get());
        return self && self->handleEditorKey(key, mask);
    });
    return list;
}

void SKEmojiComplete::update()
{
    if (!mEditor->getVisible() || mEditor->getReadOnly())
    {
        mLastCursor = -1;
        close();
        return;
    }
    const std::string& text = mEditor->getText();
    const S32 cursor = mEditor->getCursorPosition();
    if (cursor == mLastCursor && text == mLastText)
    {
        if (isOpen())
        {
            layout(); // the editor grows as it wraps
        }
        return;
    }
    mLastText = text;
    mLastCursor = cursor;

    S32 pos = -1;
    if (!LLEmojiHelper::isCursorInEmojiCode(text, cursor, &pos))
    {
        mDismissedPos = -1;
        close();
        return;
    }
    if (pos == mDismissedPos)
    {
        close();
        return;
    }
    const std::string query = text.substr(pos, cursor - pos);

    // A whole code typed out, closing colon and all, becomes its emoji as the picker would have it.
    const std::string emoji = LLEmojiDictionary::instance().getEmojiFromShortCode(query);
    if (!emoji.empty())
    {
        mEditor->handleEmojiCommit(emoji);
        mLastCursor = -1;
        close();
        return;
    }

    if (query != mQuery)
    {
        LLEmojiDictionary::instance().findByShortCode(mResults, query);
        mQuery = query;
        mSelected = 0;
        mFirst = 0;
    }
    mCodePos = pos;
    if (mResults.empty())
    {
        close();
        return;
    }
    layout();
    setVisible(true);
}

void SKEmojiComplete::dismiss()
{
    mDismissedPos = mCodePos;
    close();
}

void SKEmojiComplete::close()
{
    setVisible(false);
    mQuery.clear();
    mResults.clear();
}

S32 SKEmojiComplete::visibleRows() const
{
    return llmin((S32)mResults.size(), MAX_ROWS);
}

void SKEmojiComplete::layout()
{
    const LLRect editor = mEditor->getRect();
    const S32 width = llclamp(editor.getWidth(), 160, MAX_W);
    const S32 height = visibleRows() * row_height() + 2 * PAD;
    LLRect rect;
    rect.setOriginAndSize(editor.mLeft, editor.mTop + GAP, width, height);
    if (rect != getRect())
    {
        setShape(rect);
    }
}

void SKEmojiComplete::select(S32 index)
{
    const S32 count = (S32)mResults.size();
    if (count == 0)
    {
        return;
    }
    mSelected = (index % count + count) % count;
    if (mSelected < mFirst)
    {
        mFirst = mSelected;
    }
    else if (mSelected >= mFirst + MAX_ROWS)
    {
        mFirst = mSelected - MAX_ROWS + 1;
    }
}

void SKEmojiComplete::commit(S32 index)
{
    if (index < 0 || index >= (S32)mResults.size())
    {
        return;
    }
    const std::string emoji = mResults[index].Character;
    close();
    mEditor->setFocus(true);
    mEditor->handleEmojiCommit(emoji);
    mLastCursor = -1;
}

bool SKEmojiComplete::handleEditorKey(KEY key, MASK mask)
{
    if (!isOpen() || mask != MASK_NONE)
    {
        return false;
    }
    switch (key)
    {
    case KEY_UP:
        select(mSelected - 1);
        return true;
    case KEY_DOWN:
        select(mSelected + 1);
        return true;
    case KEY_PAGE_UP:
        select(llmax(mSelected - MAX_ROWS, 0));
        return true;
    case KEY_PAGE_DOWN:
        select(llmin(mSelected + MAX_ROWS, (S32)mResults.size() - 1));
        return true;
    case KEY_RETURN:
    case KEY_TAB:
        commit(mSelected);
        return true;
    case KEY_ESCAPE:
        dismiss();
        return true;
    default:
        return false;
    }
}

S32 SKEmojiComplete::rowAt(S32 y) const
{
    const S32 from_top = getRect().getHeight() - PAD - y;
    if (from_top < 0)
    {
        return -1;
    }
    const S32 row = from_top / row_height();
    return row < visibleRows() ? mFirst + row : -1;
}

void SKEmojiComplete::draw()
{
    static LLUIImagePtr back = LLUI::getUIImage("Rounded_Square");
    static LLUIColor back_color = LLUIColorTable::instance().getColor("Black");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("White");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray");
    static LLUIColor match_color = LLUIColorTable::instance().getColor("EmphasisColor");

    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    back->draw(LLRect(0, h, w, 0), back_color.get() % 0.8f);

    const S32 row_h = row_height();
    const S32 rows = visibleRows();
    LLFontGL* label = label_font();
    LLFontGL* glyph = glyph_font();
    for (S32 r = 0; r < rows; ++r)
    {
        const S32 index = mFirst + r;
        const LLEmojiSearchResult& result = mResults[index];
        const S32 top = h - PAD - r * row_h;
        const S32 bottom = top - row_h;
        if (index == mSelected)
        {
            gl_rect_2d(PAD, top, w - PAD, bottom, text_color.get() % 0.14f, true);
        }
        glyph->renderUTF8(result.Character, 0, (F32)(PAD + 4), (F32)(bottom + ROW_PAD), LLColor4::white,
                          LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);

        // The code with the part the query matched picked out.
        const std::string& code = result.String;
        const size_t begin = llmin(result.Begin, code.size());
        const size_t end = llclamp(result.End, begin, code.size());
        const S32 room = w - PAD - (PAD + GLYPH_W);
        F32 x = (F32)(PAD + GLYPH_W);
        const F32 y = (F32)(bottom + ROW_PAD);
        const std::string parts[3] = { code.substr(0, begin), code.substr(begin, end - begin), code.substr(end) };
        for (S32 p = 0; p < 3; ++p)
        {
            if (parts[p].empty())
            {
                continue;
            }
            const S32 left = room - ll_round(x - (F32)(PAD + GLYPH_W));
            if (left <= 0)
            {
                break;
            }
            const LLColor4 color = p == 1 ? match_color.get() : (index == mSelected ? text_color.get() : dim_color.get());
            label->renderUTF8(parts[p], 0, x, y, color, LLFontGL::LEFT, LLFontGL::BOTTOM, p == 1 ? LLFontGL::BOLD : LLFontGL::NORMAL,
                              LLFontGL::NO_SHADOW, S32_MAX, left, nullptr, true);
            x += (F32)label->getWidth(parts[p]);
        }
    }

    // A thin thumb tells there is more to scroll to.
    const S32 count = (S32)mResults.size();
    if (count > rows && rows > 0)
    {
        const S32 track = h - 2 * PAD;
        const S32 thumb = llmax(track * rows / count, 8);
        const S32 thumb_top = h - PAD - (track - thumb) * mFirst / llmax(count - rows, 1);
        gl_rect_2d(w - PAD - 2, thumb_top, w - PAD, thumb_top - thumb, text_color.get() % 0.35f, true);
    }
}

bool SKEmojiComplete::handleHover(S32 x, S32 y, MASK mask)
{
    const S32 index = rowAt(y);
    if (index >= 0)
    {
        mSelected = index;
    }
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool SKEmojiComplete::handleMouseDown(S32 x, S32 y, MASK mask)
{
    commit(rowAt(y));
    return true;
}

bool SKEmojiComplete::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    const S32 count = (S32)mResults.size();
    mFirst = llclamp(mFirst + delta.mClicks, 0, llmax(count - MAX_ROWS, 0));
    mSelected = llclamp(mSelected, mFirst, mFirst + visibleRows() - 1);
    return true;
}
