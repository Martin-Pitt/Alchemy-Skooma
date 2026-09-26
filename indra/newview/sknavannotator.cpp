/**
 * @file sknavannotator.cpp
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

#include "llviewerprecompiledheaders.h"

#include "sknavannotator.h"

#include "llagent.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "llrender.h"
#include "llregionhandle.h"
#include "llfocusmgr.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "sknavmesh.h"
#include "sknavoverlay.h"

#include <iomanip>

namespace
{
    using namespace sknav;

    LLColor4 typeColor(MarkType type)
    {
        switch (type)
        {
            case MarkType::MISSED_OPENING: return LLColor4(0.2f, 1.f, 0.3f, 1.f);
            case MarkType::WRONG: return LLColor4(1.f, 0.2f, 0.2f, 1.f);
            case MarkType::MERGE: return LLColor4(1.f, 0.6f, 0.1f, 1.f);
            case MarkType::SPLIT: return LLColor4(1.f, 0.3f, 0.9f, 1.f);
            case MarkType::STAIR: return LLColor4(0.3f, 0.7f, 1.f, 1.f);
            case MarkType::WINDOW: return LLColor4(0.4f, 1.f, 1.f, 1.f);
            case MarkType::INDOOR: return LLColor4(0.9f, 0.8f, 0.5f, 1.f);
            case MarkType::OUTDOOR: return LLColor4(0.5f, 0.9f, 0.5f, 1.f);
            case MarkType::SHELTERED: return LLColor4(0.6f, 0.6f, 1.f, 1.f);
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

    /// A box's twelve edges as ribbons, so a wire box reads as boldly as a line.
    void wireBox(SKNavOverlayGeom& geom, const LLVector3& min, const LLVector3& max, const LLColor4& color)
    {
        static const S32 EDGES[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                          { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        LLVector3 corner[8];
        for (S32 n = 0; n < 8; ++n)
        {
            corner[n].set((n & 1) ? max.mV[VX] : min.mV[VX], (n & 2) ? max.mV[VY] : min.mV[VY], (n & 4) ? max.mV[VZ] : min.mV[VZ]);
        }
        for (const auto& e : EDGES)
        {
            geom.edge(corner[e[0]], corner[e[1]], color);
        }
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
            case MarkKind::BOX:
            {
                LLVector3d lo, hi;
                mark.extent(lo, hi);
                wireBox(geom, agentOf(lo), agentOf(hi), color);
                break;
            }
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

    void writeVec(std::ostream& file, const LLVector3d& v)
    {
        file << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
    }

    void writeVec(std::ostream& file, const LLVector3& v)
    {
        file << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
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

void SKNavAnnotator::setMode(Mode mode)
{
    if (mode == mMode)
    {
        return;
    }
    mMode = mode;
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
    if (mMode != Mode::NONE && !ours)
    {
        tools->setTransientTool(SKToolNavAnnotate::getInstance());
    }
    else if (mMode == Mode::NONE && ours)
    {
        tools->clearTransientTool();
    }
}

void SKNavAnnotator::checkTool()
{
    if (mMode != Mode::NONE && LLToolMgr::getInstance()->getCurrentTool() != SKToolNavAnnotate::getInstance())
    {
        mMode = Mode::NONE;
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
    if (mMode == Mode::NONE)
    {
        return;
    }
    if (mPending.mVerts.empty())
    {
        mPending = Mark();
        mPending.mKind = mMode == Mode::POINT ? MarkKind::POINT : (mMode == Mode::LINE ? MarkKind::LINE : MarkKind::BOX);
        mPending.mType = mNewType;
        mPending.mHeight = gSavedSettings.getF32("SKNavAnnotateBoxHeight");
    }
    mPending.mVerts.push_back(at);
    if (mPending.mKind != MarkKind::LINE && mPending.complete())
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
    mSelected = mMarks.add(std::move(mark));
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
        // Over the top of the mark, so a box's label clears its roof.
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
        if (mHasHover && (preview.mKind == MarkKind::LINE || preview.mKind == MarkKind::BOX))
        {
            preview.mVerts.push_back(mHover);
        }
        drawMark(bold, preview, typeColor(preview.mType));
    }
    if (mHasHover && mMode != Mode::NONE)
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

std::string SKNavAnnotator::exportCapture()
{
    if (mMarks.empty())
    {
        return "no marks to export";
    }
    const F64 margin = gSavedSettings.getF32("SKNavAnnotateMargin");
    LLVector3d min, max;
    mMarks.bounds(margin, min, max);

    const LLDate now = LLDate::now();
    const std::string stamp = now.toHTTPDateString("%Y%m%dT%H%M%SZ");
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sknav-capture-" + stamp + ".json");
    const std::string png = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sknav-capture-" + stamp + ".png");
    // The snapshot renders a frame of its own, so the hover cross is gone by then.
    mHasHover = false;

    // The world as it looks now, marks included, without the floaters over it.
    const bool shot = gViewerWindow->saveSnapshot(png, gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw(),
                                                  false, false, false, false, LLSnapshotModel::SNAPSHOT_TYPE_COLOR,
                                                  LLSnapshotModel::SNAPSHOT_FORMAT_PNG);

    llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return llformat("could not write %s", path.c_str());
    }
    file << std::setprecision(10);

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    file << "{\"format\":\"sknav-capture\",\"version\":1,\"exported\":\"" << now.asString() << "\",\"grid\":\""
         << jsonEscape(LLGridManager::getInstance()->getGridId()) << "\",\"screenshot\":\""
         << (shot ? jsonEscape(gDirUtilp->getBaseFileName(png)) : std::string()) << "\",\"agent\":";
    writeVec(file, gAgent.getPositionGlobal());
    file << ",\"camera\":{\"position\":";
    writeVec(file, gAgent.getPosGlobalFromAgent(camera->getOrigin()));
    file << ",\"at\":";
    writeVec(file, camera->getAtAxis());
    file << ",\"up\":";
    writeVec(file, camera->getUpAxis());
    file << ",\"fovY\":" << camera->getView() << ",\"aspect\":" << camera->getAspect() << '}';

    file << ",\"marks\":[";
    bool first_mark = true;
    for (const Mark& mark : mMarks.marks())
    {
        file << (first_mark ? "" : ",") << "{\"id\":" << mark.mId << ",\"kind\":\"" << markKindName(mark.mKind) << "\",\"type\":\""
             << markTypeName(mark.mType) << "\",\"text\":\"" << jsonEscape(mark.mText) << "\",\"created\":" << mark.mCreated;
        if (mark.mKind == MarkKind::BOX)
        {
            file << ",\"height\":" << mark.mHeight;
        }
        file << ",\"vertices\":[";
        for (size_t i = 0; i < mark.mVerts.size(); ++i)
        {
            const LLVector3d& v = mark.mVerts[i];
            file << (i ? "," : "") << "{\"global\":";
            writeVec(file, v);
            if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(v))
            {
                U32 x = 0, y = 0;
                from_region_handle(region->getHandle(), &x, &y);
                file << ",\"region\":\"" << jsonEscape(region->getName()) << "\",\"regionGrid\":[" << x / 256 << ',' << y / 256
                     << "],\"local\":";
                writeVec(file, region->getPosRegionFromGlobal(v));
            }
            file << '}';
        }
        file << "]}";
        first_mark = false;
    }
    file << ']';

    file << ",\"bounds\":{\"margin\":" << margin << ",\"min\":";
    writeVec(file, min);
    file << ",\"max\":";
    writeVec(file, max);
    file << '}';

    const std::string counts = SKNavMesh::instance().writeArea(file, min, max);
    file << "}\n";

    const std::streamoff bytes = file.tellp();
    file.close();

    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << LL_ENDL;
    return llformat("%zu marks, %s (%.1f MB)%s -> %s", mMarks.marks().size(), counts.c_str(),
                    static_cast<F64>(bytes) / (1024.0 * 1024.0), shot ? " + png" : ", no screenshot", path.c_str());
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
    if (annotator.mode() != SKNavAnnotator::Mode::LINE)
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
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        // A mark half placed goes first; with none, the tool itself does.
        if (!annotator.cancel())
        {
            annotator.setMode(SKNavAnnotator::Mode::NONE);
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
