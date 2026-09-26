/**
 * @file sknavannotator.h
 * @brief In-world navmesh annotations: the session's marks, the tool that places them, their drawing and the capture export.
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

#ifndef SK_NAVANNOTATOR_H
#define SK_NAVANNOTATOR_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lltool.h"
#include "sknavannotate.h"

#include <map>
#include <string>

class LLHUDText;

/// The marks of this session and the one being placed. Main thread only.
class SKNavAnnotator : public LLSingleton<SKNavAnnotator>
{
    LLSINGLETON(SKNavAnnotator);
    ~SKNavAnnotator() override;

public:
    enum class Mode : U8
    {
        NONE = 0,
        POINT,
        LINE,
        BOX
    };

    /// Pick a placing tool, or none. Any tool puts the annotation tool in
    /// front of the current one; none gives the previous one back.
    void setMode(Mode mode);
    Mode mode() const { return mMode; }

    /// The type and note new marks are given.
    void setNewType(sknav::MarkType type) { mNewType = type; }
    sknav::MarkType newType() const { return mNewType; }

    /// A surface hit under the cursor, and a click on it.
    void setHover(bool valid, const LLVector3d& at);
    void click(const LLVector3d& at);
    /// Enter or a double-click: a line with two vertices or more is kept.
    void finish();
    /// Esc: the mark being placed goes. True when there was one.
    bool cancel();

    const sknav::MarkSet& marks() const { return mMarks; }
    sknav::Mark* find(U32 id) { return mMarks.find(id); }
    void remove(U32 id);
    void clear();
    void select(U32 id) { mSelected = id; }
    U32 selected() const { return mSelected; }
    /// Something about the marks changed; a list shown of them is stale.
    void touch() { ++mRevision; }
    U32 revision() const { return mRevision; }

    /// Draw the marks, the one being placed and the hover point, in the
    /// world pass after the navmesh overlay.
    void render();

    /// Write the capture bundle and its screenshot to the logs folder, and
    /// return a line for the console.
    std::string exportCapture();

private:
    void commit(sknav::Mark mark);
    void syncLabels();

    sknav::MarkSet mMarks;
    sknav::Mark mPending;
    Mode mMode = Mode::NONE;
    sknav::MarkType mNewType = sknav::MarkType::MISSED_OPENING;
    U32 mSelected = 0;
    U32 mRevision = 0;
    bool mHasHover = false;
    LLVector3d mHover;
    std::map<U32, LLPointer<LLHUDText>> mLabels;
    U32 mLabelRevision = 0;
};

/// Places marks where the cursor meets a surface, as the build tools do.
class SKToolNavAnnotate : public LLTool, public LLSingleton<SKToolNavAnnotate>
{
    LLSINGLETON(SKToolNavAnnotate);
    ~SKToolNavAnnotate() override = default;

public:
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    void handleSelect() override;
    void handleDeselect() override;
    bool clipMouseWhenDown() override { return false; }

private:
    bool pick(S32 x, S32 y, LLVector3d& out) const;
};

#endif // SK_NAVANNOTATOR_H
