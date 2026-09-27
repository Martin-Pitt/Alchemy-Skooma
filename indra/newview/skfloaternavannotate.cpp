/**
 * @file skfloaternavannotate.cpp
 * @brief Navmesh annotations: the type buttons that arm the placing tool, and the session's marks.
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

#include "skfloaternavannotate.h"

#include "llbutton.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llscrolllistctrl.h"
#include "lluictrlfactory.h"

using sknav::Mark;
using sknav::MarkGroup;
using sknav::MarkKind;
using sknav::MarkType;

namespace
{
    struct TypeLabel
    {
        const char* mLabel;
        const char* mTip;
    };

    // In MarkType order; the key comes from the type, so the two cannot drift.
    const TypeLabel TYPE_LABELS[] = {
        { "Opening", "A missed opening: a way through the navmesh did not find" },
        { "Portal", "A wrong portal: a way through where there is none" },
        { "Door", "A door" },
        { "Window", "A window" },
        { "Stair", "A stair" },
        { "Ramp", "A ramp" },
        { "Ladder", "A ladder" },
        { "Elevator", "An elevator" },
        { "Ledge", "A ledge or a drop" },
        { "Indoor", "Indoors" },
        { "Sheltered", "Sheltered: covered but open to the outside" },
        { "Outdoor", "Outdoors" },
        { "Street", "A street: a line along it, or a box over it" },
        { "Bridge", "A bridge" },
        { "Balcony", "A balcony" },
        { "Courtyard", "A courtyard" },
        { "Water", "Water: a pool or prim water" },
        { "Walkable", "Floor the navmesh missed" },
        { "Not walk", "Floor the navmesh has that should not be walkable" },
        { "Merge", "Spaces that should be one" },
        { "Split", "A space that should be two" },
        { "Note", "Anything else; say what in the note" },
    };
    static_assert(sizeof(TYPE_LABELS) / sizeof(TYPE_LABELS[0]) == static_cast<size_t>(MarkType::COUNT), "a label per type");

    constexpr S32 GRID_COLUMNS = 4;
    constexpr S32 GRID_GAP = 5;
    constexpr S32 ROW_HEIGHT = 22;
    constexpr S32 ROW_GAP = 3;
    constexpr S32 GROUP_GAP = 8;
}

SKFloaterNavAnnotate::SKFloaterNavAnnotate(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterNavAnnotate::postBuild()
{
    getChild<LLUICtrl>("mark_text")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onText(); });
    mList = getChild<LLScrollListCtrl>("marks_list");
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
    getChild<LLUICtrl>("delete_button")->setCommitCallback([](LLUICtrl*, const LLSD&) {
        SKNavAnnotator& annotator = SKNavAnnotator::instance();
        annotator.remove(annotator.selected());
    });
    getChild<LLUICtrl>("clear_button")->setCommitCallback([](LLUICtrl*, const LLSD&) { SKNavAnnotator::instance().clear(); });
    buildTypeGrid();
    return true;
}

void SKFloaterNavAnnotate::buildTypeGrid()
{
    // A button per type, one click to pick, each group starting a row of its own.
    LLPanel* grid = getChild<LLPanel>("type_grid");
    const S32 width = (grid->getRect().getWidth() - (GRID_COLUMNS - 1) * GRID_GAP) / GRID_COLUMNS;
    S32 top = grid->getRect().getHeight();
    S32 column = 0;
    MarkGroup group = MarkGroup::OPENING;
    for (S32 t = 0; t < static_cast<S32>(MarkType::COUNT); ++t)
    {
        const MarkType type = static_cast<MarkType>(t);
        if (t > 0 && (column == GRID_COLUMNS || sknav::markTypeGroup(type) != group))
        {
            top -= ROW_HEIGHT + (sknav::markTypeGroup(type) != group ? GROUP_GAP : ROW_GAP);
            column = 0;
        }
        group = sknav::markTypeGroup(type);
        LLButton::Params p;
        p.name = sknav::markTypeName(type);
        p.label = llformat("%c  %s", sknav::markTypeKey(type), TYPE_LABELS[t].mLabel);
        p.tool_tip = llformat("%s. A %s; key %c", TYPE_LABELS[t].mTip, sknav::markKindName(sknav::markTypeKind(type)),
                              sknav::markTypeKey(type));
        p.rect = LLRect(column * (width + GRID_GAP), top, column * (width + GRID_GAP) + width, top - ROW_HEIGHT);
        p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
        p.is_toggle = true;
        p.commit_callback.function = [type](LLUICtrl*, const LLSD&) { SKNavAnnotator::instance().pickType(type); };
        LLButton* button = LLUICtrlFactory::create<LLButton>(p);
        grid->addChild(button);
        mTypeButtons[t] = button;
        ++column;
    }
}

void SKFloaterNavAnnotate::onClose(bool app_quitting)
{
    if (!app_quitting)
    {
        SKNavAnnotator::instance().arm(false);
    }
    LLFloater::onClose(app_quitting);
}

void SKFloaterNavAnnotate::onSelect()
{
    LLScrollListItem* item = mList->getFirstSelected();
    SKNavAnnotator::instance().select(item ? item->getValue().asInteger() : 0);
    refreshSelection();
}

void SKFloaterNavAnnotate::onText()
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    if (Mark* mark = annotator.find(annotator.selected()))
    {
        mark->mText = getChild<LLUICtrl>("mark_text")->getValue().asString();
        annotator.touch();
    }
}

void SKFloaterNavAnnotate::refreshList()
{
    const SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();
    for (const Mark& mark : annotator.marks().marks())
    {
        LLSD row;
        row["value"] = static_cast<S32>(mark.mId);
        row["columns"][0]["column"] = "id";
        row["columns"][0]["value"] = static_cast<S32>(mark.mId);
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = sknav::markKindName(mark.mKind);
        row["columns"][2]["column"] = "type";
        row["columns"][2]["value"] = sknav::markTypeName(mark.mType);
        row["columns"][3]["column"] = "text";
        row["columns"][3]["value"] = mark.mText;
        mList->addElement(row);
    }
    mList->selectByValue(static_cast<S32>(annotator.selected()));
    mList->setScrollPos(scroll);
    refreshSelection();
}

void SKFloaterNavAnnotate::refreshSelection()
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const Mark* mark = annotator.find(annotator.selected());
    LLLineEditor* text = getChild<LLLineEditor>("mark_text");
    if (!text->hasFocus())
    {
        text->setValue(mark ? mark->mText : std::string());
    }
    text->setEnabled(mark != nullptr);
    // The selected mark's type when there is one, else the one the next mark gets.
    const MarkType shown = mark ? mark->mType : annotator.newType();
    for (S32 t = 0; t < static_cast<S32>(MarkType::COUNT); ++t)
    {
        if (mTypeButtons[t])
        {
            mTypeButtons[t]->setToggleState(static_cast<MarkType>(t) == shown);
        }
    }
    getChild<LLUICtrl>("delete_button")->setEnabled(mark != nullptr);
}

void SKFloaterNavAnnotate::draw()
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    annotator.checkTool();
    if (annotator.revision() != mRevision)
    {
        mRevision = annotator.revision();
        refreshList();
    }
    std::string status = getString("status_idle");
    if (annotator.armed())
    {
        const MarkType type = annotator.newType();
        status = getString(sknav::markTypeKind(type) == MarkKind::LINE ? "status_line" : "status_point");
        LLStringUtil::replaceString(status, "[TYPE]", TYPE_LABELS[static_cast<S32>(type)].mLabel);
    }
    getChild<LLUICtrl>("tool_status")->setValue(status);
    getChild<LLUICtrl>("clear_button")->setEnabled(!annotator.marks().empty());
    LLFloater::draw();
}
