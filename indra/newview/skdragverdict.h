/**
 * @file skdragverdict.h
 * @brief The viewer's half of drag feedback: the cargo's ghost, the world's verdicts and the chip's text.
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

#pragma once

#include "llui.h"
#include "lluuid.h"
#include "llwindow.h"
#include "skdragfeedback.h"

#include <string>
#include <vector>

class ALSelectionOutline;
class LLViewerObject;

namespace SKDragVerdict
{
    // Where a world drop lands, as LLToolDragAndDrop's EDropTarget numbers it.
    enum EWorldTarget : S32
    {
        WORLD_NONE = 0,
        WORLD_SELF = 1,
        WORLD_AVATAR = 2,
        WORLD_OBJECT = 3,
        WORLD_LAND = 4,
    };

    // Fills the ghost from the cargo as a drag begins. `source` is LLToolDragAndDrop::ESource.
    void beginDrag(const std::vector<EDragAndDropType>& types, const uuid_vec_t& ids, S32 source,
                   const LLUUID& source_id);

    // The world's verdict for the last pick: the action the drop takes for the first cargo item with the live mask,
    // the target's name, and the hints of the modifiers that would change it. Kept until the next pick, and reported
    // again on every pass that reaches the world (reportWorld), since the pick lands after the pass that asked it.
    void pickWorld(EDragAndDropType cargo, S32 target, LLViewerObject* object, S32 face, MASK mask, EAcceptance accept);
    void reportWorld();
    void clearWorld();

    // The world mark for the frame: the face, the faces or the object, accepted or refused (ALSelectionOutline).
    void addWorldMark(ALSelectionOutline& outline);

    // The chip's text after a pass, from what was reported, the tooltip message and the acceptance.
    void compose(EAcceptance accept, const std::string& tooltip_msg, S32 source, U32 cargo_count);

    // The cursor for an accepting verdict that copies or links where the acceptance says move.
    ECursorType cursorFor(EAcceptance accept, ECursorType cursor, U32 cargo_count);

    // The name of an avatar for a verdict, as RLVa lets it be shown.
    std::string avatarName(const LLUUID& id);
}
