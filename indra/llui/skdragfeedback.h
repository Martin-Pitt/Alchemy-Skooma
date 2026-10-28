/**
 * @file skdragfeedback.h
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

#pragma once

#include "llpointer.h"
#include "llrect.h"
#include "lluuid.h"

#include <string>
#include <string_view>
#include <vector>

class LLTexture;
class LLUIImage;
class LLView;

// What a drop would do, as a target reports it (LLDragFeedback::report).
enum class EDragAction : U8
{
    None,
    Move,
    Copy,
    Link,
    Wear,
    WearAdd,
    Attach,
    AttachAdd,
    Activate,
    Give,
    Rez,
    Contents,
    ContentsInactive,
    Apply,
    ApplyAll,
    MakeOutfit,
    Reorder,
    Set,
    Count
};

// How a target under the pointer is marked: accepted, refused with a reason, or refused quietly (no mark).
enum class EDragMark : U8
{
    None,
    Accept,
    Refuse,
    Quiet
};

// The feedback of the one drag the viewer has at a time. The drag tool fills the cargo when the drag begins and
// clears the verdict and the marks before each pass over the targets (beginPass); targets that know what a drop
// would do say so during their pass (report, refuse) and mark the rect they light (mark); the tool then sets the
// chip's text, and the window draws the ghost and the chip over everything (draw). Static: the UI has one thread
// and one drag.
class LLDragFeedback
{
public:
    // The drag, from the tool.
    static void begin(const std::vector<LLUUID>& cargo_ids);
    static void end();
    static bool dragging();
    // Whether the new look is drawn: a drag is on and SkoomaDragFeedbackClassic is off.
    static bool shown();
    static bool classic();

    // The ghost: up to three icons, the first on top, each with an optional badge (a link's arrow).
    static void addCargoIcon(const LLPointer<LLUIImage>& icon, const LLPointer<LLUIImage>& badge = nullptr);
    static void setCargoCount(U32 count);
    // A single item's picture, drawn larger in place of the icons; aspect is width over height, or 0 to take the
    // texture's own once it has one. The texture is only told the size it is drawn at, never held at a priority.
    static void setThumbnail(const LLPointer<LLTexture>& texture, F32 aspect);
    static bool isCargo(const LLUUID& id);
    static size_t iconCount();

    // One pass over the targets.
    static void beginPass();
    static void report(EDragAction action, std::string_view target_name = {});
    static void refuse(std::string_view message, bool quiet = false);
    static EDragAction reportedAction();
    static const std::string& reportedTarget();
    static const std::string& refusal();
    static bool refusedQuietly();

    // The chip, set by the tool after each pass: its line, whether it refuses, and the modifier hints under it.
    static void setChip(const std::string& text, bool refused, const std::vector<std::string>& hints);
    static const std::string& chipText();

    // Marks: a target marks the rect it would light, in its own coordinates, during the hover pass; it is good for
    // that frame only. The first mark of a pass wins. The widget draws it itself, under its text (markFor).
    static void mark(const LLView* view, const LLRect& local_rect, EDragMark kind);
    static EDragMark markFor(const LLView* view, LLRect* local_rect = nullptr);
    // The mark a folder-like target takes for its verdict: accepted; refused with a message; else quiet.
    static EDragMark markKind(bool accepted, const std::string& tooltip_msg);
    static void drawTargetMark(const LLRect& rect, EDragMark kind);

    // The overlay, from the window after every other view, at the pointer in UI coordinates.
    static void draw(S32 x, S32 y, const LLRect& window_rect);
};
