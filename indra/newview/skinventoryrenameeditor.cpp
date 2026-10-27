/**
 * @file skinventoryrenameeditor.cpp
 * @brief Inventory Explorer's inline rename: an editor over a row's or a tile's name
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

#include "skinventoryrenameeditor.h"

#include "llcallbacklist.h"
#include "lldbstrings.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "lltexteditor.h"
#include "lltextvalidate.h"
#include "llui.h"
#include "skinventoryexplorermenu.h"

namespace
{
    // What both editors share: the object renamed, and an end that runs once.
    struct RenameState
    {
        LLUUID mID;
        SKInventoryRename::done_callback_t mDone;
        bool mEnded = false;
    };

    void end_rename(LLView* editor, RenameState& state, const std::string& text, bool keep, bool refocus)
    {
        if (state.mEnded)
        {
            return;
        }
        state.mEnded = true;
        LLUI::getInstance()->removePopup(editor);
        bool kept = false;
        if (keep)
        {
            const std::string name = SKInventoryRename::cleanName(text);
            if (!name.empty())
            {
                kept = SKInventoryExplorerActions::applyName(state.mID, name);
            }
        }
        LLView* parent = editor->getParent();
        editor->setVisible(false);
        if (refocus && parent)
        {
            parent->setFocus(true);
        }
        // The editor goes once the event that ended it is done with it.
        LLHandle<LLView> handle = editor->getHandle();
        doOnIdleOneTime([handle]() { delete handle.get(); });
        if (state.mDone)
        {
            state.mDone(kept);
        }
    }
}

class SKRenameLineEditor final : public LLLineEditor
{
public:
    struct Params : public LLInitParam::Block<Params, LLLineEditor::Params>
    {
    };

    RenameState mState;

    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (key == KEY_RETURN && mask == MASK_NONE)
        {
            end_rename(this, mState, getText(), true, true);
            return true;
        }
        if (key == KEY_ESCAPE && mask == MASK_NONE)
        {
            end_rename(this, mState, getText(), false, true);
            return true;
        }
        return LLLineEditor::handleKeyHere(key, mask);
    }

    void onFocusLost() override
    {
        LLLineEditor::onFocusLost();
        end_rename(this, mState, getText(), true, false);
    }

    void onTopLost() override { end_rename(this, mState, getText(), true, false); }

protected:
    SKRenameLineEditor(const Params& p) : LLLineEditor(p) {}
    friend class LLUICtrlFactory;
};

class SKRenameTextEditor final : public LLTextEditor
{
public:
    struct Params : public LLInitParam::Block<Params, LLTextEditor::Params>
    {
    };

    RenameState mState;

    void selectTo(S32 end)
    {
        mSelectionStart = 0;
        mSelectionEnd = end;
        setCursorPos(end);
    }

    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (key == KEY_RETURN && mask == MASK_NONE)
        {
            end_rename(this, mState, getText(), true, true);
            return true;
        }
        if (key == KEY_ESCAPE && mask == MASK_NONE)
        {
            end_rename(this, mState, getText(), false, true);
            return true;
        }
        return LLTextEditor::handleKeyHere(key, mask);
    }

    void onFocusLost() override
    {
        LLTextEditor::onFocusLost();
        end_rename(this, mState, getText(), true, false);
    }

    void onTopLost() override { end_rename(this, mState, getText(), true, false); }

protected:
    SKRenameTextEditor(const Params& p) : LLTextEditor(p) {}
    friend class LLUICtrlFactory;
};

namespace SKInventoryRename
{

std::string cleanName(const std::string& text)
{
    std::string name;
    bool space = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            space = !name.empty();
            continue;
        }
        if (space)
        {
            name += ' ';
            space = false;
        }
        name += c;
    }
    LLInventoryObject::correctInventoryName(name);
    return name;
}

S32 selectionEnd(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0)
    {
        const size_t ext = name.size() - dot - 1;
        if (ext >= 1 && ext <= 5 && name.find(' ', dot) == std::string::npos)
        {
            return (S32)dot;
        }
    }
    return (S32)name.size();
}

LLHandle<LLView> start(LLView* parent, const LLUUID& id, const LLRect& rect, bool multiline, const done_callback_t& done)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!parent || !obj || !SKInventoryExplorerActions::canRename(id))
    {
        return LLHandle<LLView>();
    }
    const std::string& name = obj->getName();
    LLView* view = nullptr;
    if (multiline)
    {
        SKRenameTextEditor::Params p;
        static_cast<LLTextEditor::Params&>(p).overwriteFrom(LLUICtrlFactory::getDefaultParams<LLTextEditor>());
        p.name("sk_rename");
        p.rect(rect);
        p.font(LLFontGL::getFontSansSerifSmall());
        p.wrap(true);
        p.max_text_length(DB_INV_ITEM_NAME_STR_LEN);
        p.commit_on_focus_lost(false);
        p.border_visible(true);
        p.follows.flags(FOLLOWS_NONE);
        SKRenameTextEditor* editor = LLUICtrlFactory::create<SKRenameTextEditor>(p);
        editor->mState.mID = id;
        editor->mState.mDone = done;
        editor->setText(name);
        parent->addChild(editor);
        editor->setFocus(true);
        editor->selectTo(selectionEnd(name));
        view = editor;
    }
    else
    {
        SKRenameLineEditor::Params p;
        static_cast<LLLineEditor::Params&>(p).overwriteFrom(LLUICtrlFactory::getDefaultParams<LLLineEditor>());
        p.name("sk_rename");
        p.rect(rect);
        p.font(LLFontGL::getFontSansSerifSmall());
        p.max_length.bytes(DB_INV_ITEM_NAME_STR_LEN);
        p.prevalidator(&LLTextValidate::validateASCIIPrintableNoPipe);
        p.commit_on_focus_lost(false);
        p.revert_on_esc(false);
        p.follows.flags(FOLLOWS_NONE);
        SKRenameLineEditor* editor = LLUICtrlFactory::create<SKRenameLineEditor>(p);
        editor->mState.mID = id;
        editor->mState.mDone = done;
        editor->setText(name);
        parent->addChild(editor);
        editor->setFocus(true);
        // A long name shows from its start, with the extension left out of the selection.
        editor->setCursor(0);
        editor->setSelection(0, selectionEnd(name));
        view = editor;
    }
    LLUI::getInstance()->addPopup(view);
    return view->getHandle();
}

void finish(const LLHandle<LLView>& editor)
{
    LLView* view = editor.get();
    if (!view || !view->getVisible())
    {
        return;
    }
    if (SKRenameLineEditor* line = dynamic_cast<SKRenameLineEditor*>(view))
    {
        end_rename(line, line->mState, line->getText(), true, false);
    }
    else if (SKRenameTextEditor* text = dynamic_cast<SKRenameTextEditor*>(view))
    {
        end_rename(text, text->mState, text->getText(), true, false);
    }
}

} // namespace SKInventoryRename
