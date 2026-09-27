/**
 * @file sknavannotator.cpp
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

#include "llviewerprecompiledheaders.h"

#include "sknavannotator.h"

#include "llagent.h"
#include "lldate.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "llrender.h"
#include "llregionhandle.h"
#include "llfocusmgr.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "sknavoverlay.h"

namespace
{
    using namespace sknav;

    LLColor4 typeColor(MarkType type)
    {
        switch (type)
        {
            case MarkType::MISSED_OPENING: return LLColor4(0.2f, 1.f, 0.3f, 1.f);
            case MarkType::WRONG: return LLColor4(1.f, 0.2f, 0.2f, 1.f);
            case MarkType::DOOR: return LLColor4(0.8f, 0.4f, 0.2f, 1.f);
            case MarkType::WINDOW: return LLColor4(0.4f, 1.f, 1.f, 1.f);
            case MarkType::STAIR: return LLColor4(0.3f, 0.7f, 1.f, 1.f);
            case MarkType::RAMP: return LLColor4(0.3f, 0.5f, 0.9f, 1.f);
            case MarkType::LADDER: return LLColor4(1.f, 1.f, 0.3f, 1.f);
            case MarkType::ELEVATOR: return LLColor4(0.7f, 0.5f, 1.f, 1.f);
            case MarkType::LEDGE: return LLColor4(1.f, 0.45f, 0.45f, 1.f);
            case MarkType::INDOOR: return LLColor4(0.9f, 0.8f, 0.5f, 1.f);
            case MarkType::SHELTERED: return LLColor4(0.6f, 0.6f, 1.f, 1.f);
            case MarkType::OUTDOOR: return LLColor4(0.5f, 0.9f, 0.5f, 1.f);
            case MarkType::STREET: return LLColor4(0.75f, 0.75f, 0.75f, 1.f);
            case MarkType::BRIDGE: return LLColor4(0.8f, 0.6f, 0.4f, 1.f);
            case MarkType::BALCONY: return LLColor4(1.f, 0.8f, 0.6f, 1.f);
            case MarkType::COURTYARD: return LLColor4(0.6f, 0.9f, 0.7f, 1.f);
            case MarkType::WATER_BODY: return LLColor4(0.2f, 0.5f, 1.f, 1.f);
            case MarkType::WALKABLE: return LLColor4(0.1f, 0.9f, 0.6f, 1.f);
            case MarkType::NOT_WALKABLE: return LLColor4(0.9f, 0.1f, 0.5f, 1.f);
            case MarkType::MERGE: return LLColor4(1.f, 0.6f, 0.1f, 1.f);
            case MarkType::SPLIT: return LLColor4(1.f, 0.3f, 0.9f, 1.f);
            default: return LLColor4(0.9f, 0.9f, 0.9f, 1.f);
        }
    }

    LLVector3 agentOf(const LLVector3d& global)
    {
        return gAgent.getPosAgentFromGlobal(global);
    }

    void cross(SKNavOverlayGeom& geom, const LLVector3& at, F32 size, const LLColor4& color)
    {
        geom.edge(at - LLVector3(size, 0.f, 0.f), at + LLVector3(size, 0.f, 0.f), color);
        geom.edge(at - LLVector3(0.f, size, 0.f), at + LLVector3(0.f, size, 0.f), color);
        geom.edge(at - LLVector3(0.f, 0.f, size), at + LLVector3(0.f, 0.f, size), color);
    }

    void drawMark(SKNavOverlayGeom& geom, const Mark& mark, const LLColor4& color)
    {
        if (mark.mVerts.empty())
        {
            return;
        }
        switch (mark.mKind)
        {
            case MarkKind::POINT:
                cross(geom, agentOf(mark.mVerts.front()), 0.3f, color);
                break;
            case MarkKind::LINE:
                for (size_t i = 0; i + 1 < mark.mVerts.size(); ++i)
                {
                    geom.edge(agentOf(mark.mVerts[i]), agentOf(mark.mVerts[i + 1]), color);
                }
                for (const LLVector3d& v : mark.mVerts)
                {
                    cross(geom, agentOf(v), 0.1f, color);
                }
                break;
            default:
                break;
        }
    }

    std::string labelOf(const Mark& mark)
    {
        std::string label = llformat("#%u %s", mark.mId, markTypeName(mark.mType));
        if (!mark.mText.empty())
        {
            label += ": " + mark.mText;
        }
        return label;
    }
}

SKNavAnnotator::SKNavAnnotator() = default;

SKNavAnnotator::~SKNavAnnotator()
{
    for (auto& label : mLabels)
    {
        label.second->markDead();
    }
}

void SKNavAnnotator::arm(bool on)
{
    if (on == mArmed)
    {
        return;
    }
    mArmed = on;
    cancel();
    mHasHover = false;
    touch();
    syncTool();
}

void SKNavAnnotator::syncTool()
{
    // Other tools replace a transient one without a word, so ask rather than remember.
    LLToolMgr* tools = LLToolMgr::getInstance();
    const bool ours = tools->getCurrentTool() == SKToolNavAnnotate::getInstance();
    if (mArmed && !ours)
    {
        tools->setTransientTool(SKToolNavAnnotate::getInstance());
    }
    else if (!mArmed && ours)
    {
        tools->clearTransientTool();
    }
}

void SKNavAnnotator::checkTool()
{
    if (mArmed && LLToolMgr::getInstance()->getCurrentTool() != SKToolNavAnnotate::getInstance())
    {
        mArmed = false;
        cancel();
        mHasHover = false;
        touch();
    }
}

void SKNavAnnotator::setHover(bool valid, const LLVector3d& at)
{
    mHasHover = valid;
    mHover = at;
}

void SKNavAnnotator::click(const LLVector3d& at)
{
    if (!mArmed)
    {
        return;
    }
    if (mPending.mVerts.empty())
    {
        mPending = Mark();
        mPending.mKind = markTypeKind(mNewType);
        mPending.mType = mNewType;
        // A type picked now is for the mark being placed, not one picked earlier.
        mSelected = 0;
        touch();
    }
    mPending.mVerts.push_back(at);
    if (mPending.mKind == MarkKind::POINT && mPending.complete())
    {
        commit(std::move(mPending));
        mPending = Mark();
    }
}

void SKNavAnnotator::finish()
{
    if (mPending.mKind == MarkKind::LINE && mPending.complete())
    {
        commit(std::move(mPending));
    }
    mPending = Mark();
}

bool SKNavAnnotator::cancel()
{
    const bool had = !mPending.mVerts.empty();
    mPending = Mark();
    return had;
}

void SKNavAnnotator::commit(Mark mark)
{
    mark.mCreated = LLDate::now().secondsSinceEpoch();
    mMarks.add(std::move(mark));
    touch();
}

void SKNavAnnotator::remove(U32 id)
{
    if (mMarks.remove(id))
    {
        if (mSelected == id)
        {
            mSelected = 0;
        }
        touch();
    }
}

void SKNavAnnotator::select(U32 id)
{
    if (id != mSelected)
    {
        mSelected = id;
        touch();
    }
}

void SKNavAnnotator::clear()
{
    mMarks.clear();
    mPending = Mark();
    mSelected = 0;
    touch();
}

void SKNavAnnotator::syncLabels()
{
    if (mLabelRevision == mRevision && mLabels.size() == mMarks.marks().size())
    {
        return;
    }
    mLabelRevision = mRevision;
    for (auto it = mLabels.begin(); it != mLabels.end();)
    {
        if (!mMarks.find(it->first))
        {
            it->second->markDead();
            it = mLabels.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const Mark& mark : mMarks.marks())
    {
        LLPointer<LLHUDText>& label = mLabels[mark.mId];
        if (label.isNull())
        {
            label = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            label->setDoFade(false);
            label->setZCompare(false);
        }
        LLVector3d lo, hi;
        mark.extent(lo, hi);
        // Over the top of the mark, so the label clears it.
        label->setPositionGlobal(LLVector3d((lo.mdV[VX] + hi.mdV[VX]) * 0.5, (lo.mdV[VY] + hi.mdV[VY]) * 0.5, hi.mdV[VZ] + 0.4));
        LLColor4 color = typeColor(mark.mType);
        if (mark.mId == mSelected)
        {
            color = LLColor4::white;
        }
        label->setColor(color);
        label->setString(labelOf(mark));
    }
}

void SKNavAnnotator::render()
{
    syncLabels();
    if (mMarks.empty() && mPending.mVerts.empty() && !mHasHover)
    {
        return;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    SKNavOverlayGeom geom;
    geom.setEdgeWidth(4.f, camera->getOrigin(), camera->getView(), camera->getViewHeightInPixels());
    for (const Mark& mark : mMarks.marks())
    {
        if (mark.mId != mSelected)
        {
            drawMark(geom, mark, typeColor(mark.mType));
        }
    }

    // The selected mark and the one being placed, wider and in white over the rest.
    SKNavOverlayGeom bold;
    bold.setEdgeWidth(7.f, camera->getOrigin(), camera->getView(), camera->getViewHeightInPixels());
    if (const Mark* selected = mMarks.find(mSelected))
    {
        drawMark(bold, *selected, LLColor4::white);
    }
    if (!mPending.mVerts.empty())
    {
        Mark preview = mPending;
        if (mHasHover && preview.mKind == MarkKind::LINE)
        {
            preview.mVerts.push_back(mHover);
        }
        drawMark(bold, preview, typeColor(preview.mType));
    }
    if (mHasHover && mArmed)
    {
        cross(bold, agentOf(mHover), 0.2f, LLColor4::yellow);
    }

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            geom.draw();
            bold.draw();
        }
        {
            // Behind geometry, fainter, so a mark inside a wall is still found.
            LLGLDepthTest behind(GL_TRUE, GL_FALSE, GL_GREATER);
            geom.draw(0.6f, 0.4f);
            bold.draw(0.6f, 0.4f);
        }
    }
    SKNavOverlayGeom::unbindShader();
}

void SKNavAnnotator::pickType(MarkType type)
{
    const MarkKind kind = markTypeKind(type);
    Mark* selected = mMarks.find(mSelected);
    if (selected && selected->mKind == kind && selected->mType != type)
    {
        selected->mType = type;
        mNewType = type;
        touch();
        return;
    }
    // The armed type again puts the tool away, unless a mark is half placed.
    if (!selected && mArmed && type == mNewType && mPending.mVerts.empty())
    {
        arm(false);
        return;
    }
    mSelected = 0;
    mNewType = type;
    // A half-placed mark keeps its vertices when the shape still fits.
    if (mPending.mKind != kind)
    {
        mPending = Mark();
    }
    mPending.mType = type;
    touch();
    arm(true);
}

void SKNavAnnotator::writeMarks(std::ostream& out) const
{
    sknav::writeMarks(out, mMarks.marks(), [](const LLVector3d& v, MarkPlace& place) {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(v);
        if (!region)
        {
            return false;
        }
        U32 x = 0, y = 0;
        from_region_handle(region->getHandle(), &x, &y);
        place.mRegion = region->getName();
        place.mGridX = x / 256;
        place.mGridY = y / 256;
        place.mLocal = region->getPosRegionFromGlobal(v);
        return true;
    });
}

SKToolNavAnnotate::SKToolNavAnnotate()
    : LLTool(std::string("NavAnnotate"))
{
}

bool SKToolNavAnnotate::pick(S32 x, S32 y, LLVector3d& out) const
{
    const LLPickInfo info = gViewerWindow->pickImmediate(x, y, false, false, false, true);
    if (info.mPickType == LLPickInfo::PICK_OBJECT || info.mPickType == LLPickInfo::PICK_LAND || info.mPickType == LLPickInfo::PICK_FLORA)
    {
        out = info.mPosGlobal;
        return !out.isExactlyZero();
    }
    return false;
}

bool SKToolNavAnnotate::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // A transient tool hides the Alt override, so hand alt-cam on; it captures the drag.
    if (mask & MASK_ALT)
    {
        return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
    }
    // Off the button just pressed, so Enter and Esc reach the tool.
    gFocusMgr.setKeyboardFocus(nullptr);
    LLVector3d at;
    if (pick(x, y, at))
    {
        SKNavAnnotator::instance().click(at);
    }
    return true;
}

bool SKToolNavAnnotate::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return true;
}

bool SKToolNavAnnotate::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    if (markTypeKind(annotator.newType()) != MarkKind::LINE)
    {
        return handleMouseDown(x, y, mask);
    }
    annotator.finish();
    return true;
}

bool SKToolNavAnnotate::handleHover(S32 x, S32 y, MASK mask)
{
    if (mask & MASK_ALT)
    {
        SKNavAnnotator::instance().setHover(false, LLVector3d());
        return LLToolCamera::getInstance()->handleHover(x, y, mask);
    }
    LLVector3d at;
    const bool hit = pick(x, y, at);
    SKNavAnnotator::instance().setHover(hit, at);
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool SKToolNavAnnotate::handleKey(KEY key, MASK mask)
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    if (key == KEY_RETURN && mask == MASK_NONE)
    {
        annotator.finish();
        return true;
    }
    MarkType type;
    if (mask == MASK_NONE && key < 0x80 && markTypeForKey(static_cast<char>(key), type))
    {
        annotator.pickType(type);
        return true;
    }
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        // A mark half placed goes first; with none, the tool itself does.
        if (!annotator.cancel())
        {
            annotator.arm(false);
        }
        return true;
    }
    return LLTool::handleKey(key, mask);
}

void SKToolNavAnnotate::handleSelect()
{
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
}

void SKToolNavAnnotate::handleDeselect()
{
    SKNavAnnotator::instance().setHover(false, LLVector3d());
}
