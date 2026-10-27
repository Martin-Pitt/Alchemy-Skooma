/**
 * @file skinventoryrenameeditor.h
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

#ifndef SK_SKINVENTORYRENAMEEDITOR_H
#define SK_SKINVENTORYRENAMEEDITOR_H

#include "llhandle.h"
#include "llrect.h"
#include "lluuid.h"

#include <functional>
#include <string>

class LLView;

// One editor at a time, over the name it renames; Enter or a click away keeps, Esc reverts.
namespace SKInventoryRename
{
    typedef std::function<void(bool kept)> done_callback_t;

    // Opens the editor over rect, in parent's coordinates; a tile's editor wraps its lines.
    LLHandle<LLView> start(LLView* parent, const LLUUID& id, const LLRect& rect, bool multiline, const done_callback_t& done);

    // Ends an open editor, keeping what was typed.
    void finish(const LLHandle<LLView>& editor);

    // What is kept of typed text: whitespace collapsed and trimmed; empty keeps the old name.
    std::string cleanName(const std::string& text);

    // The initial selection's end: before a last dot-part of one to five characters (".lsl").
    S32 selectionEnd(const std::string& name);
}

#endif // SK_SKINVENTORYRENAMEEDITOR_H
