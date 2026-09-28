/**
 * @file sknavannotator.h
 * @brief In-world navmesh annotations: the session's marks, the tool that places them, and their drawing.
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
#include <ostream>
#include <string>

class LLHUDText;

/// The marks of this session and the one being placed. Main thread only.
class SKNavAnnotator : public LLSingleton<SKNavAnnotator>
{
    LLSINGLETON(SKNavAnnotator);
    ~SKNavAnnotator() override;

public:
    /// Arm the placing tool with the current type, or put it away. Armed,
    /// the annotation tool is in front of the current one.
    void arm(bool on);
    bool armed() const { return mArmed; }

    /// A type button or its key. With a mark selected, a type of the same
    /// shape retypes it. Otherwise the type sticks for the marks placed next
    /// and arms the tool with its shape; picked again, it puts the tool away.
    void pickType(sknav::MarkType type);
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
    void select(U32 id);
    /// Disarm when another tool has taken over from ours.
    void checkTool();
    U32 selected() const { return mSelected; }
    /// Something about the marks changed; a list shown of them is stale.
    void touch() { ++mRevision; }
    U32 revision() const { return mRevision; }

    /// The portal the cursor is over, while armed to cross out: the nearest
    /// drawn one to the cursor, by the spaces overlay's own reach. False in
    /// it when there is none.
    void hoverPortal(S32 x, S32 y);
    /// Whether a portal mark crosses the cut between these two spaces of a
    /// mesh, either way round. What the spaces overlay asks while it draws.
    bool portalCrossed(const LLUUID& mesh, U32 space_a, U32 space_b) const;

    /// Draw the marks, the one being placed and the hover point, in the
    /// world pass after the navmesh overlay.
    void render();

    /// The marks as an export's "marks" member, each vertex with its region.
    void writeMarks(std::ostream& out) const;

private:
    struct HoverPortal
    {
        bool mValid = false;
        LLUUID mMesh;
        U32 mSpaces[2] = { 0, 0 };  ///< the two spaces it joins, by stable id
        LLVector3d mCentre;         ///< the cut's centre, global, at the sill
        LLVector3d mA;
        LLVector3d mB;              ///< the cut's ends, global, lifted for drawing
    };

    void commit(sknav::Mark mark);
    void togglePortal(const HoverPortal& portal, const LLVector3d& at);
    void syncLabels();
    void syncTool();

    sknav::MarkSet mMarks;
    sknav::Mark mPending;
    bool mArmed = false;
    sknav::MarkType mNewType = sknav::MarkType::MISSED_OPENING;
    U32 mSelected = 0;
    U32 mRevision = 0;
    bool mHasHover = false;
    LLVector3d mHover;
    HoverPortal mHoverPortal;
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
