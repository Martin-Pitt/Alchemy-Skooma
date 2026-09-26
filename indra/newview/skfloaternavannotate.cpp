/**
 * @file skfloaternavannotate.cpp
 * @brief Navmesh annotations: the placing tools, the session's marks and the capture export.
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
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "llviewercontrol.h"

using sknav::Mark;
using sknav::MarkKind;
using sknav::MarkType;

SKFloaterNavAnnotate::SKFloaterNavAnnotate(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterNavAnnotate::postBuild()
{
    using Mode = SKNavAnnotator::Mode;
    getChild<LLUICtrl>("tool_point")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTool(Mode::POINT); });
    getChild<LLUICtrl>("tool_line")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTool(Mode::LINE); });
    getChild<LLUICtrl>("tool_box")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTool(Mode::BOX); });
    getChild<LLUICtrl>("type_combo")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onType(); });
    getChild<LLUICtrl>("mark_text")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onText(); });
    getChild<LLUICtrl>("box_height")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHeight(); });
    mList = getChild<LLScrollListCtrl>("marks_list");
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
    getChild<LLUICtrl>("delete_button")->setCommitCallback([](LLUICtrl*, const LLSD&) {
        SKNavAnnotator& annotator = SKNavAnnotator::instance();
        annotator.remove(annotator.selected());
    });
    getChild<LLUICtrl>("clear_button")->setCommitCallback([](LLUICtrl*, const LLSD&) { SKNavAnnotator::instance().clear(); });
    getChild<LLUICtrl>("export_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        getChild<LLUICtrl>("export_status")->setValue(SKNavAnnotator::instance().exportCapture());
    });
    getChild<LLUICtrl>("type_combo")->setValue(static_cast<S32>(SKNavAnnotator::instance().newType()));
    getChild<LLUICtrl>("box_height")->setValue(gSavedSettings.getF32("SKNavAnnotateBoxHeight"));
    return true;
}

void SKFloaterNavAnnotate::onClose(bool app_quitting)
{
    if (!app_quitting)
    {
        SKNavAnnotator::instance().setMode(SKNavAnnotator::Mode::NONE);
    }
    LLFloater::onClose(app_quitting);
}

void SKFloaterNavAnnotate::onTool(SKNavAnnotator::Mode mode)
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    // A second press on the tool that is down puts it away.
    annotator.setMode(annotator.mode() == mode ? SKNavAnnotator::Mode::NONE : mode);
}

void SKFloaterNavAnnotate::onSelect()
{
    LLScrollListItem* item = mList->getFirstSelected();
    SKNavAnnotator::instance().select(item ? item->getValue().asInteger() : 0);
    refreshSelection();
}

void SKFloaterNavAnnotate::onType()
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const S32 value = llclamp(getChild<LLUICtrl>("type_combo")->getValue().asInteger(), 0, static_cast<S32>(MarkType::COUNT) - 1);
    const MarkType type = static_cast<MarkType>(value);
    annotator.setNewType(type);
    if (Mark* mark = annotator.find(annotator.selected()))
    {
        mark->mType = type;
        annotator.touch();
    }
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

void SKFloaterNavAnnotate::onHeight()
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const F32 height = static_cast<F32>(getChild<LLUICtrl>("box_height")->getValue().asReal());
    Mark* mark = annotator.find(annotator.selected());
    if (mark && mark->mKind == MarkKind::BOX)
    {
        mark->mHeight = height;
        annotator.touch();
    }
    else
    {
        // With no box selected, the height is the one the next box gets.
        gSavedSettings.setF32("SKNavAnnotateBoxHeight", height);
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
    if (mark)
    {
        getChild<LLUICtrl>("type_combo")->setValue(static_cast<S32>(mark->mType));
    }
    if (mark && mark->mKind == MarkKind::BOX)
    {
        getChild<LLUICtrl>("box_height")->setValue(mark->mHeight);
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
    const SKNavAnnotator::Mode mode = annotator.mode();
    getChild<LLButton>("tool_point")->setToggleState(mode == SKNavAnnotator::Mode::POINT);
    getChild<LLButton>("tool_line")->setToggleState(mode == SKNavAnnotator::Mode::LINE);
    getChild<LLButton>("tool_box")->setToggleState(mode == SKNavAnnotator::Mode::BOX);
    getChild<LLUICtrl>("clear_button")->setEnabled(!annotator.marks().empty());
    getChild<LLUICtrl>("export_button")->setEnabled(!annotator.marks().empty());
    LLFloater::draw();
}
