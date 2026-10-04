/**
 * @file skfloaternavannotate.h
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

#ifndef SK_FLOATERNAVANNOTATE_H
#define SK_FLOATERNAVANNOTATE_H

#include "llfloater.h"
#include "sknavannotator.h"

class LLButton;
class LLScrollListCtrl;

/// Where the rooms oracle is wrong, marked in the world. The tool stays in
/// front while a type is armed and goes when the floater closes.
class SKFloaterNavAnnotate final : public LLFloater
{
public:
    SKFloaterNavAnnotate(const LLSD& key);
    ~SKFloaterNavAnnotate() override = default;

    bool postBuild() override;
    void draw() override;
    void onClose(bool app_quitting) override;

private:
    void onSelect();
    void onText();
    void buildTypeGrid();
    void refreshList();
    void refreshSelection();

    LLScrollListCtrl* mList = nullptr;
    LLButton* mTypeButtons[static_cast<size_t>(sknav::MarkType::COUNT)] = {};
    U32 mRevision = U32_MAX;
};

#endif // SK_FLOATERNAVANNOTATE_H
