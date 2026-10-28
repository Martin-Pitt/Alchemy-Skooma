/**
 * @file skinventorysearchbox.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skinventorysearchbox.h"

#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llinventorymodel.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "skinventoryexplorerpopup.h"
#include "skinventoryflatview.h"

static LLDefaultChildRegistry::Register<SKInventorySearchBox> r_sk_search_box("sk_inventory_search_box");

namespace
{
    constexpr size_t RECENT_COUNT = 8;
    constexpr F32 SETTLE_SECONDS = 1.5f;
    constexpr S32 DROPDOWN_MIN_WIDTH = 360;
    constexpr S32 DROPDOWN_MAX_HEIGHT = 360;
    constexpr size_t TOP_VALUES = 12;
    constexpr F64 TALLY_SLICE = 0.002;
    const char* const RECENT_SETTING = "SkoomaInvExplorerRecentSearches";

    // The filter keys, in the dropdown's order, with their examples.
    const char* const KEYS[][2] = {
        { "type", "type:texture" }, { "brand", "brand:Ash" },     { "creator", "creator:You" }, { "perm", "perm:full" },
        { "is", "is:worn" },        { "after", "after:7d" },      { "before", "before:2025-01-01" } };
    const char* const TYPE_VALUES[] = { "folder", "object", "clothing", "bodypart", "texture", "sound", "animation",
                                        "gesture", "landmark", "notecard", "script", "material", "settings", "callingcard" };
    const char* const PERM_VALUES[] = { "full", "copy", "nocopy", "mod", "nomod", "trans", "notrans" };
    const char* const IS_VALUES[] = { "worn", "link", "favorite", "folder", "item" };

    std::string lower(std::string text)
    {
        LLStringUtil::toLower(text);
        return text;
    }

    std::string quoted(const std::string& value)
    {
        return value.find(' ') != std::string::npos ? "\"" + value + "\"" : value;
    }

    std::string iso_day(time_t t)
    {
        tm parts{};
#if LL_WINDOWS
        localtime_s(&parts, &t);
#else
        localtime_r(&t, &parts);
#endif
        return llformat("%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
    }
}

//
// SKInventorySearchBox
//

SKInventorySearchBox::SKInventorySearchBox(const Params& p)
:   LLLineEditor(p)
{
}

bool SKInventorySearchBox::handleKeyHere(KEY key, MASK mask)
{
    if (mKeyHook && mKeyHook(key, mask))
    {
        return true;
    }
    return LLLineEditor::handleKeyHere(key, mask);
}

bool SKInventorySearchBox::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const bool handled = LLLineEditor::handleMouseDown(x, y, mask);
    if (mClickCallback)
    {
        mClickCallback();
    }
    return handled;
}

void SKInventorySearchBox::onFocusReceived()
{
    LLLineEditor::onFocusReceived();
    if (mFocusCallback)
    {
        mFocusCallback();
    }
}

void SKInventorySearchBox::draw()
{
    LLLineEditor::draw();
    if (hasFocus())
    {
        static LLUIColor focus = LLUIColorTable::instance().getColor("EmphasisColor");
        gl_rect_2d(getLocalRect(), focus.get(), false);
    }
}

//
// SKSearchDropdown
//

SKSearchDropdown::SKSearchDropdown(SKInventorySearchBox* box)
:   mBox(box),
    mPopup(SKExplorerPopupList::create()),
    mSettling(false)
{
    mPopup->setPickCallback([this](S32 index) { pick(index); });
    mPopup->setRemoveCallback([this](S32 index)
                              {
                                  if (index >= 0 && index < (S32)mOptions.size())
                                  {
                                      forget(mOptions[index].mWhole);
                                      refresh();
                                  }
                              });
}

SKSearchDropdown::~SKSearchDropdown()
{
    mPopup->hide();
    delete mPopup;
}

std::string SKSearchDropdown::string(const std::string& key) const
{
    auto it = mStrings.find(key);
    return it != mStrings.end() ? it->second : key;
}

std::vector<std::string> SKSearchDropdown::recent()
{
    std::vector<std::string> list;
    const LLSD saved = gSavedPerAccountSettings.getLLSD(RECENT_SETTING);
    for (LLSD::array_const_iterator it = saved.beginArray(); it != saved.endArray(); ++it)
    {
        if (it->isString() && !it->asString().empty())
        {
            list.push_back(it->asString());
        }
    }
    return list;
}

static void save_recent(const std::vector<std::string>& list)
{
    LLSD saved = LLSD::emptyArray();
    for (const std::string& text : list)
    {
        saved.append(text);
    }
    gSavedPerAccountSettings.setLLSD(RECENT_SETTING, saved);
}

void SKSearchDropdown::remember(const std::string& text)
{
    std::string trimmed = text;
    LLStringUtil::trim(trimmed);
    if (trimmed.empty())
    {
        return;
    }
    std::vector<std::string> list = recent();
    std::erase(list, trimmed);
    list.insert(list.begin(), trimmed);
    if (list.size() > RECENT_COUNT)
    {
        list.resize(RECENT_COUNT);
    }
    save_recent(list);
}

void SKSearchDropdown::forget(const std::string& text)
{
    std::vector<std::string> list = recent();
    std::erase(list, text);
    save_recent(list);
}

bool SKSearchDropdown::isOpen() const
{
    return mPopup->isOpen();
}

void SKSearchDropdown::hide()
{
    mPopup->hide();
}

void SKSearchDropdown::caretTerm(size_t& start, size_t& end, std::string& text) const
{
    const std::string value = mBox->getText();
    end = llclamp((size_t)mBox->getCursor(), (size_t)0, value.size());
    const size_t space = end > 0 ? value.rfind(' ', end - 1) : std::string::npos;
    start = space == std::string::npos ? 0 : space + 1;
    text = value.substr(start, end - start);
}

void SKSearchDropdown::onTyped()
{
    mSettle.reset();
    mSettling = true;
    mPopup->setHighlight(-1);
    refresh();
}

void SKSearchDropdown::update()
{
    if (mSettling && mSettle.getElapsedTimeF32() >= SETTLE_SECONDS)
    {
        mSettling = false;
        remember(mBox->getText());
    }
    if (mPopup->isOpen() && !mBox->hasFocus())
    {
        mPopup->hide();
    }
    // The value counts fill in as the walk runs, shown at most four times a second.
    if (mTallyWalk.isStarted() && !mTallyWalk.isDone())
    {
        const bool creator = mTallyKey == "creator";
        mTallyWalk.step(TALLY_SLICE, [this, creator](const LLInventoryObject* obj)
                        {
                            const LLViewerInventoryItem* item = dynamic_cast<const LLViewerInventoryItem*>(obj);
                            if (creator)
                            {
                                if (item && !item->getIsLinkType() && item->getCreatorUUID().notNull())
                                {
                                    ++mCreatorTally[item->getCreatorUUID()];
                                }
                                return;
                            }
                            const std::string brand = SKInventoryFlatView::brandOf(obj->getName());
                            if (!brand.empty())
                            {
                                auto& entry = mTally[lower(brand)];
                                if (entry.first.empty())
                                {
                                    entry.first = brand;
                                }
                                ++entry.second;
                            }
                        });
        if (mPopup->isOpen() && (mTallyWalk.isDone() || mTallyShown.getElapsedTimeF32() > 0.25f))
        {
            mTallyShown.reset();
            refresh();
        }
    }
}

void SKSearchDropdown::startTally(const std::string& key)
{
    const LLUUID folder = mFolderCallback ? mFolderCallback() : LLUUID::null;
    if (key == mTallyKey && folder == mTallyFolder && mTallyWalk.isStarted())
    {
        return;
    }
    mTallyKey = key;
    mTallyFolder = folder;
    mTally.clear();
    mCreatorTally.clear();
    mTallyShown.reset();
    mTallyWalk.start(folder);
}

void SKSearchDropdown::addValues(const std::string& key, const std::string& want, std::vector<SKPopupRow>& rows)
{
    std::vector<std::pair<std::string, std::string>> values;   // the value, and its hint or count
    if (key == "type")
    {
        for (const char* value : TYPE_VALUES)
        {
            values.emplace_back(value, std::string());
        }
    }
    else if (key == "perm" || key == "is")
    {
        const bool perm = key == "perm";
        for (const char* value : perm ? std::vector<const char*>(std::begin(PERM_VALUES), std::end(PERM_VALUES))
                                      : std::vector<const char*>(std::begin(IS_VALUES), std::end(IS_VALUES)))
        {
            values.emplace_back(value, string((perm ? "perm_" : "is_") + std::string(value)));
        }
    }
    else if (key == "after" || key == "before")
    {
        for (const char* value : { "1d", "7d", "30d", "365d" })
        {
            values.emplace_back(value, string(std::string("date_") + value));
        }
        tm parts{};
        const time_t now = time(nullptr);
#if LL_WINDOWS
        localtime_s(&parts, &now);
#else
        localtime_r(&now, &parts);
#endif
        values.emplace_back(llformat("%04d-01-01", parts.tm_year + 1900), string("date_jan1"));
        values.emplace_back(iso_day(now), string("date_today"));
    }
    else if (key == "brand" || key == "creator")
    {
        startTally(key);
        std::vector<std::pair<std::string, S32>> counted;
        if (key == "brand")
        {
            for (const auto& [lowered, entry] : mTally)
            {
                counted.emplace_back(entry.first, entry.second);
            }
        }
        else
        {
            for (const auto& [id, count] : mCreatorTally)
            {
                LLAvatarName name;
                counted.emplace_back(LLAvatarNameCache::get(id, &name) ? name.getUserName() : id.asString(), count);
            }
        }
        std::stable_sort(counted.begin(), counted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [value, count] : counted)
        {
            values.emplace_back(value, llformat("%d", count));
        }
    }

    for (const auto& [value, hint] : values)
    {
        if (!want.empty() && lower(value).rfind(want, 0) != 0)
        {
            continue;
        }
        Option option;
        option.mKind = Option::OPT_VALUE;
        option.mInsert = key + ":" + quoted(value);
        mOptions.push_back(option);
        SKPopupRow row;
        row.mLabel = quoted(value);
        row.mHint = hint;
        rows.push_back(row);
        if ((key == "brand" || key == "creator") && rows.size() >= TOP_VALUES + 1)
        {
            break;
        }
    }
}

void SKSearchDropdown::refresh()
{
    if (!mBox->hasFocus())
    {
        mPopup->hide();
        return;
    }
    mOptions.clear();
    std::vector<SKPopupRow> rows;
    size_t start = 0;
    size_t end = 0;
    std::string term;
    caretTerm(start, end, term);

    const size_t colon = term.find(':');
    const std::string key = colon != std::string::npos ? lower(term.substr(0, colon)) : std::string();
    const bool in_key = colon != std::string::npos
                        && std::any_of(std::begin(KEYS), std::end(KEYS), [&key](const auto& k) { return key == k[0]; });
    if (in_key)
    {
        // Inside a key: its hint as the title, then its values.
        mOptions.push_back(Option());
        SKPopupRow title;
        title.mKind = SKPopupRow::ROW_TITLE;
        title.mLabel = string("key_" + key);
        rows.push_back(title);
        std::string want = lower(term.substr(colon + 1));
        std::erase(want, '"');
        addValues(key, want, rows);
        if (rows.size() == 1)
        {
            rows.clear();
            mOptions.clear();
        }
        show(rows);
        return;
    }

    std::string whole = mBox->getText();
    LLStringUtil::trim(whole);
    const std::string whole_lower = lower(whole);
    if (!whole.empty())
    {
        // The search as it stands, favourited for this folder.
        const bool saved = mIsSaved && mIsSaved();
        Option option;
        option.mKind = Option::OPT_FAVORITE;
        mOptions.push_back(option);
        SKPopupRow row;
        row.mIcon = LLUI::getUIImage(saved ? "Inv_Favorite_Star_Full" : "Inv_Favorite_Star_Content");
        row.mLabel = string(saved ? "remove_favorite" : "add_favorite");
        row.mBold = true;
        const LLUUID folder = mFolderCallback ? mFolderCallback() : LLUUID::null;
        if (folder == gInventory.getRootFolderID())
        {
            row.mHint = string("everywhere");
        }
        else
        {
            const LLViewerInventoryCategory* cat = gInventory.getCategory(folder);
            LLStringUtil::format_map_t args;
            args["[FOLDER]"] = cat ? cat->getName() : std::string();
            row.mHint = string("in_folder");
            LLStringUtil::format(row.mHint, args);
        }
        rows.push_back(row);
        mOptions.push_back(Option());
        SKPopupRow divider;
        divider.mKind = SKPopupRow::ROW_DIVIDER;
        rows.push_back(divider);
    }

    const std::string want = lower(term);
    std::vector<SKPopupRow> key_rows;
    std::vector<Option> key_options;
    for (const auto& [name, example] : KEYS)
    {
        const std::string insert = std::string(name) + ":";
        if (!want.empty() && insert.rfind(want, 0) != 0)
        {
            continue;
        }
        Option option;
        option.mKind = Option::OPT_KEY;
        option.mInsert = insert;
        key_options.push_back(option);
        SKPopupRow row;
        row.mLabel = insert;
        row.mBold = true;
        row.mHint = string(std::string("key_") + name);
        row.mExample = example;
        key_rows.push_back(row);
    }
    if (!key_rows.empty())
    {
        mOptions.push_back(Option());
        SKPopupRow title;
        title.mKind = SKPopupRow::ROW_TITLE;
        title.mLabel = string("title");
        rows.push_back(title);
        mOptions.insert(mOptions.end(), key_options.begin(), key_options.end());
        rows.insert(rows.end(), key_rows.begin(), key_rows.end());
    }

    // Recent searches containing the text and differing from it.
    bool recent_title = false;
    for (const std::string& past : recent())
    {
        const std::string past_lower = lower(past);
        if (!whole_lower.empty() && (past_lower.find(whole_lower) == std::string::npos || past_lower == whole_lower))
        {
            continue;
        }
        if (!recent_title)
        {
            recent_title = true;
            mOptions.push_back(Option());
            SKPopupRow divider;
            divider.mKind = SKPopupRow::ROW_DIVIDER;
            rows.push_back(divider);
            mOptions.push_back(Option());
            SKPopupRow title;
            title.mKind = SKPopupRow::ROW_TITLE;
            title.mLabel = string("recent");
            rows.push_back(title);
        }
        Option option;
        option.mKind = Option::OPT_RECENT;
        option.mWhole = past;
        mOptions.push_back(option);
        SKPopupRow row;
        row.mIcon = LLUI::getUIImage("Search_Icon");
        row.mLabel = past;
        row.mRemoveToolTip = string("remove_history");
        rows.push_back(row);
    }
    // A trailing divider has nothing under it.
    while (!rows.empty() && rows.back().mKind != SKPopupRow::ROW_OPTION)
    {
        rows.pop_back();
        mOptions.pop_back();
    }
    show(rows);
}

void SKSearchDropdown::show(std::vector<SKPopupRow>& rows)
{
    if (rows.empty() || !mBox->hasFocus())
    {
        mPopup->hide();
        return;
    }
    const S32 highlight = mPopup->isOpen() ? mPopup->getHighlight() : -1;
    mPopup->setRows(rows);
    mPopup->setHighlight(highlight);
    // 3 px under the box, at least 360 wide.
    const LLRect box = mBox->getLocalRect();
    mPopup->showBelow(mBox, box, llmax(box.getWidth(), DROPDOWN_MIN_WIDTH), DROPDOWN_MAX_HEIGHT, 0, 3);
}

void SKSearchDropdown::pick(S32 index)
{
    if (index < 0 || index >= (S32)mOptions.size())
    {
        return;
    }
    const Option option = mOptions[index];
    switch (option.mKind)
    {
        case Option::OPT_FAVORITE:
            if (mToggleSaved)
            {
                mToggleSaved();
            }
            refresh();
            return;
        case Option::OPT_RECENT:
        {
            // Picking a recent search replaces the query and moves it to the top.
            const std::string text = option.mWhole + " ";
            mBox->setText(text);
            mBox->setCursor((S32)text.size());
            remember(option.mWhole);
            mSettling = false;
            if (mEditCallback)
            {
                mEditCallback();
            }
            mPopup->setHighlight(-1);
            refresh();
            return;
        }
        case Option::OPT_KEY:
        case Option::OPT_VALUE:
        {
            size_t start = 0;
            size_t end = 0;
            std::string term;
            caretTerm(start, end, term);
            const std::string value = mBox->getText();
            // A key leaves the caret after its colon; a value adds a space and replaces the rest of the term.
            const std::string tail = option.mKind == Option::OPT_KEY ? std::string() : std::string(" ");
            std::string rest = value.substr(end);
            const size_t word_end = rest.find(' ');
            rest = word_end == std::string::npos ? std::string() : rest.substr(word_end);
            if (!tail.empty() && !rest.empty() && rest.front() == ' ')
            {
                rest = rest.substr(1);
            }
            const std::string text = value.substr(0, start) + option.mInsert + tail + rest;
            mBox->setText(text);
            mBox->setCursor((S32)(start + option.mInsert.size() + tail.size()));
            if (mEditCallback)
            {
                mEditCallback();
            }
            mSettle.reset();
            mSettling = true;
            mPopup->setHighlight(-1);
            refresh();
            if (option.mKind == Option::OPT_KEY)
            {
                // Picking a key highlights its first value.
                mPopup->setHighlight(mPopup->firstOption());
            }
            return;
        }
        default:
            return;
    }
}

bool SKSearchDropdown::handleKey(KEY key, MASK mask)
{
    if (mask != MASK_NONE)
    {
        return false;
    }
    if (!mPopup->isOpen())
    {
        switch (key)
        {
            case KEY_DOWN:
                if (mEnterResults)
                {
                    mEnterResults();
                }
                return true;
            case KEY_ESCAPE:
                if (mEscape)
                {
                    mEscape();
                }
                return true;
            case KEY_RETURN:
                remember(mBox->getText());
                mSettling = false;
                return true;
            default:
                return false;
        }
    }
    switch (key)
    {
        case KEY_DOWN:
            mPopup->moveHighlight(1);
            return true;
        case KEY_UP:
            mPopup->moveHighlight(-1);
            return true;
        case KEY_RETURN:
            if (mPopup->getHighlight() >= 0)
            {
                pick(mPopup->getHighlight());
            }
            else
            {
                remember(mBox->getText());
                mSettling = false;
                mPopup->hide();
            }
            return true;
        case KEY_TAB:
        {
            S32 index = mPopup->getHighlight();
            if (index < 0 || (mOptions[index].mKind != Option::OPT_KEY && mOptions[index].mKind != Option::OPT_VALUE))
            {
                index = -1;
                for (S32 i = 0; i < (S32)mOptions.size(); ++i)
                {
                    if (mOptions[i].mKind == Option::OPT_KEY || mOptions[i].mKind == Option::OPT_VALUE)
                    {
                        index = i;
                        break;
                    }
                }
            }
            if (index < 0)
            {
                return false;
            }
            pick(index);
            return true;
        }
        case KEY_ESCAPE:
            mPopup->hide();
            return true;
        default:
            return false;
    }
}
