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
#include "lltoolcomp.h"
#include "sknavannotate.h"
#include "v4color.h"

#include <map>
#include <ostream>
#include <string>

class LLHUDText;
class SKNavMarkObjects;

/// The colour a mark of this type draws in; its body prims wear it too.
LLColor4 skNavMarkColor(sknav::MarkType type);

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
    /// Whether a mark is being placed and not yet kept: a line with a
    /// click already on it.
    bool placingPending() const { return !mPending.mVerts.empty(); }
    /// Disarm when another tool has taken over from ours.
    void checkTool();
    U32 selected() const { return mSelected; }
    /// Something about the marks changed; a list shown of them is stale.
    void touch() { ++mRevision; }
    U32 revision() const { return mRevision; }

    /// The object selection holds a mark's body: the annotation follows it,
    /// and an emptied selection deselects with it.
    void syncSelection();
    /// Whether the object selection holds any mark's body.
    bool selectionHasProxy() const;

    /// Arm the next surface click to add a way up to the selected stair.
    void armExitPick();
    bool exitPickPending() const { return mExitPick; }
    void cancelExitPick();
    void addExit(const LLVector3d& at);

    /// Wind the selected stair as a helix, or straighten it again.
    void setSpiral(bool on);
    /// How many turns the selected spiral's climb takes.
    void setSpiralTurns(F64 turns);
    /// Which way the selected spiral winds.
    void setSpiralHand(S8 hand);

    /// The portal the cursor is over, while armed to cross out: the nearest
    /// drawn one to the cursor, by the spaces overlay's own reach. False in
    /// it when there is none.
    void hoverPortal(S32 x, S32 y);
    /// Strike or restore the portal under the cursor, whether or not the
    /// world behind it picked: cuts sit in open air as often as not.
    void clickPortal();
    /// Whether a portal mark crosses the cut between these two spaces of a
    /// mesh, either way round. What the spaces overlay asks while it draws.
    bool portalCrossed(const LLUUID& mesh, U32 space_a, U32 space_b, const LLVector3d& centre) const;

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
        const char* mKind = "";     ///< door, stair or opening
        F32 mWidth = 0.f;
    };

    void commit(sknav::Mark mark);
    void togglePortal(const HoverPortal& portal, const LLVector3d& at);
    void syncLabels();
    void syncTool();

    sknav::MarkSet mMarks;
    /// Per mesh, the spaces product its crosses were last held against.
    std::map<LLUUID, const void*> mCrossesChecked;
    void staleCrosses();
    sknav::Mark mPending;
    bool mArmed = false;
    sknav::MarkType mNewType = sknav::MarkType::MISSED_OPENING;
    U32 mSelected = 0;
    bool mFollowProxySelection = false;    ///< the selection the objects made chose this mark
    bool mExitPick = false;                ///< the next surface click adds an exit
    bool mExitPickArmed = false;           ///< placing was on its own when the exit pick armed the tool
    U32 mRevision = 0;
    bool mHasHover = false;
    LLVector3d mHover;
    HoverPortal mHoverPortal;
    std::map<U32, LLPointer<LLHUDText>> mLabels;
    U32 mLabelRevision = 0;
};

/// Places marks where the cursor meets a surface, as the build tools do,
/// and works the marks' bodies like objects: a click selects one, its
/// gizmo drags a point or the whole mark, a click away deselects.
class SKToolNavAnnotate : public LLToolComposite, public LLSingleton<SKToolNavAnnotate>
{
    LLSINGLETON(SKToolNavAnnotate);
    ~SKToolNavAnnotate() override;

public:
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    void handleSelect() override;
    void handleDeselect() override;
    LLTool* getOverrideTool(MASK mask) override;
    void render() override;
    bool clipMouseWhenDown() override { return false; }

    static void pickCallback(const LLPickInfo& pick_info);

private:
    bool pick(S32 x, S32 y, LLVector3d& out) const;
    LLTool* mPlace = nullptr;   ///< the inert child the placing runs under
};

#endif // SK_NAVANNOTATOR_H
