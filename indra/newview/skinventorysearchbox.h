/**
 * @file skinventorysearchbox.h
 * @brief Inventory Explorer's search box and its options dropdown: filters, their values, recent searches
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

#ifndef SK_SKINVENTORYSEARCHBOX_H
#define SK_SKINVENTORYSEARCHBOX_H

#include "llframetimer.h"
#include "lllineeditor.h"
#include "skinventoryexplorersearch.h"

#include <functional>
#include <map>

class SKExplorerPopupList;
struct SKPopupRow;

// A line editor that offers its keys, presses and focus to the dropdown before its own handling.
class SKInventorySearchBox : public LLLineEditor
{
public:
    AL_VIEW_TYPE(SKInventorySearchBox, LLLineEditor);

    struct Params : public LLInitParam::Block<Params, LLLineEditor::Params>
    {
    };

    typedef std::function<bool(KEY key, MASK mask)> key_hook_t;
    typedef std::function<void()> void_callback_t;

    void setKeyHook(const key_hook_t& hook) { mKeyHook = hook; }
    void setClickCallback(const void_callback_t& cb) { mClickCallback = cb; }
    void setFocusCallback(const void_callback_t& cb) { mFocusCallback = cb; }

    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    void onFocusReceived() override;
    void draw() override;

protected:
    SKInventorySearchBox(const Params& p);
    friend class LLUICtrlFactory;

    key_hook_t mKeyHook;
    void_callback_t mClickCallback;
    void_callback_t mFocusCallback;
};

// The options under the box: Add to Favorites, the filter keys with hints and examples, a key's values (the most
// common under the folder for brand and creator), and recent searches. It edits the box's text itself.
class SKSearchDropdown
{
public:
    typedef std::function<void()> void_callback_t;
    typedef std::function<bool()> bool_callback_t;
    typedef std::function<LLUUID()> folder_callback_t;

    explicit SKSearchDropdown(SKInventorySearchBox* box);
    ~SKSearchDropdown();

    // The strings, keyed as the floater's "search_*" strings without the prefix.
    void setStrings(const std::map<std::string, std::string>& strings) { mStrings = strings; }
    // The query typed: the floater runs the search.
    void setEditCallback(const void_callback_t& cb) { mEditCallback = cb; }
    void setSavedCallbacks(const bool_callback_t& is_saved, const void_callback_t& toggle)
    {
        mIsSaved = is_saved;
        mToggleSaved = toggle;
    }
    // The folder searched, and "everywhere" or "in X" for the favourite line.
    void setFolderCallback(const folder_callback_t& cb) { mFolderCallback = cb; }
    // Down with the dropdown closed enters the results; a second Esc returns to the content.
    void setLeaveCallbacks(const void_callback_t& enter_results, const void_callback_t& escape)
    {
        mEnterResults = enter_results;
        mEscape = escape;
    }

    // Lists the options for the text and caret, open while the box has focus.
    void refresh();
    void hide();
    bool isOpen() const;
    // A typed change: refreshes and restarts the 1.5 s settle that keeps the search in Recent.
    void onTyped();
    // Once a frame: closes when the box lost focus, keeps a settled search, follows the value counts.
    void update();
    bool handleKey(KEY key, MASK mask);

    // Recent searches, newest first, per account.
    static std::vector<std::string> recent();
    static void remember(const std::string& text);
    static void forget(const std::string& text);

private:
    struct Option
    {
        enum EKind
        {
            OPT_TITLE,
            OPT_DIVIDER,
            OPT_FAVORITE,
            OPT_KEY,
            OPT_VALUE,
            OPT_RECENT,
        };
        EKind mKind = OPT_TITLE;
        std::string mInsert;    // a key or key:value to put in place of the term
        std::string mWhole;     // a recent search
    };

    std::string string(const std::string& key) const;
    // The term the caret is in: where it starts, and its text up to the caret.
    void caretTerm(size_t& start, size_t& end, std::string& text) const;
    void pick(S32 index);
    void addValues(const std::string& key, const std::string& want, std::vector<SKPopupRow>& rows);
    void startTally(const std::string& key);
    void show(std::vector<SKPopupRow>& rows);

    SKInventorySearchBox* mBox;
    SKExplorerPopupList* mPopup;
    std::vector<Option> mOptions;
    std::map<std::string, std::string> mStrings;
    void_callback_t mEditCallback;
    bool_callback_t mIsSaved;
    void_callback_t mToggleSaved;
    folder_callback_t mFolderCallback;
    void_callback_t mEnterResults;
    void_callback_t mEscape;

    LLFrameTimer mSettle;
    bool mSettling;

    // brand: and creator: count their values under the folder a slice at a time, once per opening of the key.
    SKInventoryWalk mTallyWalk;
    std::string mTallyKey;
    LLUUID mTallyFolder;
    std::map<std::string, std::pair<std::string, S32>> mTally;   // lower case to the first spelling met and its count
    std::map<LLUUID, S32> mCreatorTally;
    LLFrameTimer mTallyShown;
};

#endif // SK_SKINVENTORYSEARCHBOX_H
