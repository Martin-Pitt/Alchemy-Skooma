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
#include "llmaniptranslate.h"
#include "llrender.h"
#include "llregionhandle.h"
#include "llfocusmgr.h"
#include "llselectmgr.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "lltoolselect.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "sknavmesh.h"
#include "sknavmarkobjects.h"
#include "sknavoverlay.h"
#include "sknavrooms.h"

#include <utility>

namespace
{
    using namespace sknav;

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

// The extra a stair draws over its line: the flight as treads between
    // rails with portal bars at the ends, and a ray and bar for each way up
    // beyond the line's own end. The prims carry the line itself; this is
    // what the line alone does not say.
    void drawStairExtras(SKNavOverlayGeom& geom, const Mark& mark, const LLColor4& color)
    {
        const std::vector<LLVector3d> path = mark.path();
        if (path.size() < 2)
        {
            return;
        }
        std::vector<LLVector3> run;
        run.reserve(path.size());
        for (const LLVector3d& v : path)
        {
            run.push_back(agentOf(v));
        }
        skDrawStairGlyph(geom, run, 1.2f, color, 0.06f);
        const LLVector3 foot = run.front();
        for (const LLVector3d& exit : mark.mExits)
        {
            const LLVector3 at = agentOf(exit);
            geom.edge(foot, at, color);
            skDrawStairBar(geom, at, at - foot, 1.2f, color);
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

    std::pair<U32, U32> portalPairOf(U32 a, U32 b)
    {
        return a <= b ? std::make_pair(a, b) : std::make_pair(b, a);
    }

    // The live portal a crossed mark names: same mesh, the two spaces it
    // joined either way round, its centre where the mark said, within a
    // little slack for a pass that has re-cut since. The mesh comes back
    // too, for the origin the portal's ends are relative to.
    struct ResolvedPortal
    {
        const SKNavWorldView::Mesh* mMesh = nullptr;
        const Portal* mPortal = nullptr;
    };

    bool resolvePortal(const Mark& mark, ResolvedPortal& out)
    {
        const SKNavWorldViewPtr view = SKNavMesh::instance().view();
        if (!view)
        {
            return false;
        }
        const std::pair<U32, U32> pair = mark.portalPair();
        F64 best = 0.0;
        for (const SKNavWorldView::Mesh& mesh : view->meshes())
        {
            if (mesh.mId != mark.mSpaceMesh || !mesh.mRooms || !mesh.mProduct)
            {
                continue;
            }
            const std::vector<U32>& ids = mesh.mProduct->mSpaceIds;
            for (const Portal& portal : mesh.mRooms->mPortals)
            {
                if (portal.mSpaces[0] >= ids.size() || portal.mSpaces[1] >= ids.size())
                {
                    continue;
                }
                if (portalPairOf(ids[portal.mSpaces[0]], ids[portal.mSpaces[1]]) != pair)
                {
                    continue;
                }
                const LLVector3d centre = mesh.mOrigin + LLVector3d((portal.mA + portal.mB) * 0.5f);
                const F64 d = dist_vec_squared(centre, mark.mPortalCentre);
                // Two cuts can join the same two spaces; the mark names the
                // one it sat on, so the nearest wins, within a little slack
                // for a pass that has re-cut since.
                if (d > 6.25 || (out.mPortal && d >= best))
                {
                    continue;
                }
                best = d;
                out.mMesh = &mesh;
                out.mPortal = &portal;
            }
        }
        return out.mPortal != nullptr;
    }

    // A crossed portal: an X across the cut, sized to it, its ends ticked.
    // A cut the pass has done away with strikes at the click instead, so
    // the mark stays findable until it is removed.
    void drawPortal(SKNavOverlayGeom& geom, const Mark& mark, const LLColor4& color)
    {
        if (mark.mVerts.empty())
        {
            return;
        }
        ResolvedPortal resolved;
        const bool found = resolvePortal(mark, resolved);
        const LLVector3 a = found ? gAgent.getPosAgentFromGlobal(resolved.mMesh->mOrigin + LLVector3d(resolved.mPortal->mA))
                                  : agentOf(mark.mVerts.front());
        const LLVector3 b = found ? gAgent.getPosAgentFromGlobal(resolved.mMesh->mOrigin + LLVector3d(resolved.mPortal->mB)) : a;
        const LLVector3 centre = (a + b) * 0.5f;
        const F32 size = llclamp(0.5f * dist_vec(a, b), 0.35f, 2.5f);
        const F32 k = 0.70710678f * size;
        LLVector3 run(b - a);
        if (run.magVecSquared() > 1e-8f)
        {
            // The X sits at forty-five degrees to the run, so it reads as a
            // strike whatever way the cut faces.
            run.normalize();
            const LLVector3 across(-run.mV[VY], run.mV[VX], 0.f);
            const LLVector3 arm = (run + across) * k;
            const LLVector3 arm2 = (run - across) * k;
            geom.edge(centre - arm, centre + arm, color);
            geom.edge(centre - arm2, centre + arm2, color);
        }
        else
        {
            geom.edge(centre - LLVector3(k, k, 0.f), centre + LLVector3(k, k, 0.f), color);
            geom.edge(centre - LLVector3(k, -k, 0.f), centre + LLVector3(k, -k, 0.f), color);
        }
        if (found)
        {
            cross(geom, a, 0.18f, color);
            cross(geom, b, 0.18f, color);
        }
        else
        {
            cross(geom, a, 0.3f, color);
        }
    }
}

LLColor4 skNavMarkColor(sknav::MarkType type)
{
    switch (type)
    {
        case sknav::MarkType::MISSED_OPENING: return LLColor4(0.2f, 1.f, 0.3f, 1.f);
        case sknav::MarkType::WRONG: return LLColor4(1.f, 0.2f, 0.2f, 1.f);
        case sknav::MarkType::DOOR: return LLColor4(0.8f, 0.4f, 0.2f, 1.f);
        case sknav::MarkType::WINDOW: return LLColor4(0.4f, 1.f, 1.f, 1.f);
        case sknav::MarkType::STAIR: return LLColor4(0.3f, 0.7f, 1.f, 1.f);
        case sknav::MarkType::RAMP: return LLColor4(0.3f, 0.5f, 0.9f, 1.f);
        case sknav::MarkType::LADDER: return LLColor4(1.f, 1.f, 0.3f, 1.f);
        case sknav::MarkType::ELEVATOR: return LLColor4(0.7f, 0.5f, 1.f, 1.f);
        case sknav::MarkType::LEDGE: return LLColor4(1.f, 0.45f, 0.45f, 1.f);
        case sknav::MarkType::INDOOR: return LLColor4(0.9f, 0.8f, 0.5f, 1.f);
        case sknav::MarkType::SHELTERED: return LLColor4(0.6f, 0.6f, 1.f, 1.f);
        case sknav::MarkType::OUTDOOR: return LLColor4(0.5f, 0.9f, 0.5f, 1.f);
        case sknav::MarkType::STREET: return LLColor4(0.75f, 0.75f, 0.75f, 1.f);
        case sknav::MarkType::BRIDGE: return LLColor4(0.8f, 0.6f, 0.4f, 1.f);
        case sknav::MarkType::BALCONY: return LLColor4(1.f, 0.8f, 0.6f, 1.f);
        case sknav::MarkType::COURTYARD: return LLColor4(0.6f, 0.9f, 0.7f, 1.f);
        case sknav::MarkType::WATER_BODY: return LLColor4(0.2f, 0.5f, 1.f, 1.f);
        case sknav::MarkType::WALKABLE: return LLColor4(0.1f, 0.9f, 0.6f, 1.f);
        case sknav::MarkType::NOT_WALKABLE: return LLColor4(0.9f, 0.1f, 0.5f, 1.f);
        case sknav::MarkType::MERGE: return LLColor4(1.f, 0.6f, 0.1f, 1.f);
        case sknav::MarkType::SPLIT: return LLColor4(1.f, 0.3f, 0.9f, 1.f);
        case sknav::MarkType::CROSS: return LLColor4(1.f, 0.15f, 0.1f, 1.f);
        default: return LLColor4(0.9f, 0.9f, 0.9f, 1.f);
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
    mHoverPortal = HoverPortal();
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
        mHoverPortal = HoverPortal();
        touch();
    }
}

void SKNavAnnotator::setHover(bool valid, const LLVector3d& at)
{
    mHasHover = valid;
    mHover = at;
    mHoverPortal = HoverPortal();
}

void SKNavAnnotator::hoverPortal(S32 x, S32 y)
{
    mHoverPortal = HoverPortal();
    if (!mArmed || markTypeKind(mNewType) != MarkKind::PORTAL)
    {
        return;
    }
    // Only what the spaces layer is drawing can be struck: nothing shown,
    // nothing to pick.
    static LLCachedControl<bool> show_spaces(gSavedSettings, "SKNavOverlaySpaces", false);
    if (!show_spaces)
    {
        return;
    }
    const SKNavWorldViewPtr view = SKNavMesh::instance().view();
    if (!view)
    {
        return;
    }
    // The same reach the overlay draws the portals within.
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera->getOrigin());
    const F64 reach = range;
    // A portal is under the cursor when its drawn run passes within a few
    // pixels of it. Ends behind the camera would fold back onto the view,
    // so a run both of whose ends are behind is skipped.
    static constexpr S32 REACH_PX = 12;
    F64 best = F64(REACH_PX) * REACH_PX;
    auto inFront = [camera](const LLVector3& agent) { return (agent - camera->getOrigin()) * camera->getAtAxis() > 0.f; };
    for (const SKNavWorldView::Mesh& mesh : view->meshes())
    {
        if (!mesh.mRooms || !mesh.mProduct)
        {
            continue;
        }
        const std::vector<U32>& ids = mesh.mProduct->mSpaceIds;
        for (const Portal& portal : mesh.mRooms->mPortals)
        {
            const LLVector3d a = mesh.mOrigin + LLVector3d(portal.mA) + LLVector3d(0.0, 0.0, 0.15);
            const LLVector3d b = mesh.mOrigin + LLVector3d(portal.mB) + LLVector3d(0.0, 0.0, 0.15);
            if (dist_vec_squared2D((a + b) * 0.5, camera_global) > reach * reach)
            {
                continue;
            }
            const LLVector3 agent_a = gAgent.getPosAgentFromGlobal(a);
            const LLVector3 agent_b = gAgent.getPosAgentFromGlobal(b);
            if (!inFront(agent_a) || !inFront(agent_b))
            {
                continue;
            }
            LLCoordGL pa, pb;
            camera->projectPosAgentToScreen(agent_a, pa, false);
            camera->projectPosAgentToScreen(agent_b, pb, false);
            // The closest point of the drawn run to the cursor, in real
            // numbers: whole-pixel arithmetic would collapse the projection
            // onto an end, and only the ends would answer.
            const F64 dx = F64(pb.mX) - pa.mX, dy = F64(pb.mY) - pa.mY;
            const F64 len2 = dx * dx + dy * dy;
            F64 t = len2 > 0.0 ? ((F64(x) - pa.mX) * dx + (F64(y) - pa.mY) * dy) / len2 : 0.0;
            t = llclamp(t, 0.0, 1.0);
            const F64 ex = pa.mX + t * dx - x, ey = pa.mY + t * dy - y;
            const F64 d = ex * ex + ey * ey;
            if (d < best)
            {
                best = d;
                mHoverPortal.mMesh = mesh.mId;
                mHoverPortal.mSpaces[0] = portal.mSpaces[0] < ids.size() ? ids[portal.mSpaces[0]] : 0;
                mHoverPortal.mSpaces[1] = portal.mSpaces[1] < ids.size() ? ids[portal.mSpaces[1]] : 0;
                mHoverPortal.mCentre = mesh.mOrigin + LLVector3d((portal.mA + portal.mB) * 0.5f);
                mHoverPortal.mA = a;
                mHoverPortal.mB = b;
                mHoverPortal.mKind = portalKindName(portal.mKind);
                mHoverPortal.mWidth = portal.mWidth;
            }
        }
    }
    mHoverPortal.mValid = mHoverPortal.mSpaces[0] && mHoverPortal.mSpaces[1] && best < F64(REACH_PX) * REACH_PX;
}

bool SKNavAnnotator::portalCrossed(const LLUUID& mesh, U32 space_a, U32 space_b) const
{
    const std::pair<U32, U32> pair = portalPairOf(space_a, space_b);
    for (const Mark& mark : mMarks.marks())
    {
        if (mark.mKind == MarkKind::PORTAL && mark.mSpaceMesh == mesh && mark.portalPair() == pair)
        {
            return true;
        }
    }
    return false;
}

void SKNavAnnotator::clickPortal()
{
    if (!mArmed || !mHoverPortal.mValid)
    {
        return;
    }
    // The mark's vertex sits on the cut itself: a click through open air
    // never met a surface to land on.
    togglePortal(mHoverPortal, mHoverPortal.mCentre);
}

void SKNavAnnotator::togglePortal(const HoverPortal& portal, const LLVector3d& at)
{
    const std::pair<U32, U32> pair = portalPairOf(portal.mSpaces[0], portal.mSpaces[1]);
    for (const Mark& mark : mMarks.marks())
    {
        if (mark.mKind == MarkKind::PORTAL && mark.mSpaceMesh == portal.mMesh && mark.portalPair() == pair
            && dist_vec_squared(mark.mPortalCentre, portal.mCentre) < 6.25)
        {
            // Struck already: clicking it again lets the cut back.
            remove(mark.mId);
            return;
        }
    }
    Mark mark;
    mark.mKind = MarkKind::PORTAL;
    mark.mType = MarkType::CROSS;
    mark.mVerts.push_back(at);
    mark.mSpaceMesh = portal.mMesh;
    mark.mSpaceIds[0] = portal.mSpaces[0];
    mark.mSpaceIds[1] = portal.mSpaces[1];
    mark.mPortalCentre = portal.mCentre;
    // The cut says what it is while the note says nothing of its own: its
    // kind and width, and the two spaces it joined.
    if (mark.mText.empty())
    {
        mark.mText = llformat("%s, %.2f m, spaces %u-%u", portal.mKind, portal.mWidth, portal.mSpaces[0], portal.mSpaces[1]);
    }
    commit(std::move(mark));
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
        SKNavMarkObjects::instance().despawnMark(id);
        touch();
    }
}

void SKNavAnnotator::clear()
{
    mMarks.clear();
    mPending = Mark();
    mSelected = 0;
    mFollowProxySelection = false;
    mExitPick = false;
    mExitPickArmed = false;
    SKNavMarkObjects::instance().despawnAll();
    touch();
}

bool SKNavAnnotator::selectionHasProxy() const
{
    if (!SKNavMarkObjects::instanceExists())
    {
        return false;
    }
    SKNavMarkObjects& bodies = SKNavMarkObjects::instance();
    for (LLObjectSelection::iterator iter = LLSelectMgr::getInstance()->getSelection()->begin();
         iter != LLSelectMgr::getInstance()->getSelection()->end(); ++iter)
    {
        LLViewerObject* object = (*iter)->getObject();
        if (object && bodies.isMarkProxy(object))
        {
            return true;
        }
    }
    return false;
}

void SKNavAnnotator::syncSelection()
{
    if (!SKNavMarkObjects::instanceExists())
    {
        return;
    }
    SKNavMarkObjects& bodies = SKNavMarkObjects::instance();
    U32 from_objects = 0;
    for (LLObjectSelection::iterator iter = LLSelectMgr::getInstance()->getSelection()->begin();
         iter != LLSelectMgr::getInstance()->getSelection()->end(); ++iter)
    {
        LLViewerObject* object = (*iter)->getObject();
        if (object && bodies.isMarkProxy(object))
        {
            from_objects = bodies.markIdOf(object);
            break;
        }
    }
    if (from_objects)
    {
        mFollowProxySelection = true;
        if (mSelected != from_objects)
        {
            mSelected = from_objects;
            touch();
        }
    }
    else if (mFollowProxySelection && mSelected)
    {
        mSelected = 0;
        mFollowProxySelection = false;
        touch();
    }
}

void SKNavAnnotator::select(U32 id)
{
    if (mSelected == id && !mFollowProxySelection && (id == 0 || !selectionHasProxy()))
    {
        return;
    }
    mSelected = id;
    mFollowProxySelection = false;
    mExitPick = false;
    // The body follows the list: a mark picked there selects its object,
    // so the highlight and the gizmos come with it.
    LLViewerObject* root = id && SKNavMarkObjects::instanceExists() ? SKNavMarkObjects::instance().rootProxy(id) : nullptr;
    if (root)
    {
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(root, false, true);
    }
    else if (id == 0)
    {
        LLSelectMgr::getInstance()->deselectAll();
    }
    touch();
}

void SKNavAnnotator::armExitPick()
{
    Mark* mark = mMarks.find(mSelected);
    if (!mark || mark->mType != MarkType::STAIR || !mark->complete())
    {
        return;
    }
    // The click has to reach our tool for the exit to land, so a floater
    // used without placing first brings the tool up, and puts it away when
    // the exit is picked or let go.
    mExitPickArmed = mArmed;
    if (!mArmed)
    {
        arm(true);
    }
    mExitPick = true;
    touch();
}

void SKNavAnnotator::cancelExitPick()
{
    if (mExitPick)
    {
        mExitPick = false;
        if (!mExitPickArmed)
        {
            arm(false);
        }
        touch();
    }
}

void SKNavAnnotator::addExit(const LLVector3d& at)
{
    mExitPick = false;
    Mark* mark = mMarks.find(mSelected);
    if (!mark || mark->mType != MarkType::STAIR || !mark->complete())
    {
        if (!mExitPickArmed)
        {
            arm(false);
        }
        return;
    }
    if (!mExitPickArmed)
    {
        arm(false);
    }
    mark->mExits.push_back(at);
    touch();
}

void SKNavAnnotator::setSpiral(bool on)
{
    Mark* mark = mMarks.find(mSelected);
    if (!mark || mark->mType != MarkType::STAIR || !mark->complete() || mark->mSpiral == on)
    {
        return;
    }
    mark->mSpiral = on;
    if (on)
    {
        // A helix about the run: the axis a metre to the side of the foot,
        // a turn of it from foot to top, right-handed, until edited.
        const LLVector3d& foot = mark->mVerts[0];
        const LLVector3d& top = mark->mVerts[1];
        LLVector3d run(top - foot);
        run.mdV[VZ] = 0.0;
        const F64 len = run.magVec();
        const LLVector3d across = len > 1e-6 ? LLVector3d(-run.mdV[VY], run.mdV[VX], 0.0) * (1.0 / len) : LLVector3d(1.0, 0.0, 0.0);
        mark->mSpiralCentre = foot + across * 1.0;
        mark->mSpiralRadius = 1.0;
        mark->mSpiralTurns = 1.0;
        mark->mSpiralHand = 1;
        // The top winds to where a turn of the helix ends, at the height
        // it already had.
        const LLVector3d wound = mark->helixAt(1.0);
        mark->mVerts[1].set(wound.mdV[VX], wound.mdV[VY], top.mdV[VZ]);
    }
    touch();
}

void SKNavAnnotator::setSpiralTurns(F64 turns)
{
    Mark* mark = mMarks.find(mSelected);
    if (!mark || !mark->mSpiral || mark->mVerts.size() < 2)
    {
        return;
    }
    mark->mSpiralTurns = llclamp(turns, sknav::SPIRAL_MIN_TURNS, sknav::SPIRAL_MAX_TURNS);
    const LLVector3d wound = mark->helixAt(1.0);
    mark->mVerts[1].set(wound.mdV[VX], wound.mdV[VY], mark->mVerts[1].mdV[VZ]);
    touch();
}

void SKNavAnnotator::setSpiralHand(S8 hand)
{
    Mark* mark = mMarks.find(mSelected);
    if (!mark || !mark->mSpiral || mark->mVerts.size() < 2)
    {
        return;
    }
    mark->mSpiralHand = hand >= 0 ? 1 : -1;
    const LLVector3d wound = mark->helixAt(1.0);
    mark->mVerts[1].set(wound.mdV[VX], wound.mdV[VY], mark->mVerts[1].mdV[VZ]);
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
        LLColor4 color = skNavMarkColor(mark.mType);
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
    // The bodies follow the marks and the marks the bodies, once a frame,
    // and the annotation follows what the objects picked.
    SKNavMarkObjects::instance().sync(*this);
    syncSelection();
    syncLabels();
    if (mMarks.empty() && mPending.mVerts.empty() && !mHasHover)
    {
        return;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    // Three gathers: the ghost every mark leaves under geometry, what
    // stands over the world besides the bodies, and the selected mark and
    // the one being placed, wider, over everything.
    SKNavOverlayGeom ghost, front, bold;
    ghost.setEdgeWidth(4.f, camera->getOrigin(), camera->getView(), camera->getViewHeightInPixels());
    front.setEdgeWidth(4.f, camera->getOrigin(), camera->getView(), camera->getViewHeightInPixels());
    bold.setEdgeWidth(7.f, camera->getOrigin(), camera->getView(), camera->getViewHeightInPixels());
    for (const Mark& mark : mMarks.marks())
    {
        const bool selected = mark.mId == mSelected;
        const LLColor4 color = skNavMarkColor(mark.mType);
        // A mark is findable through a wall: its own shape, faint, behind.
        if (mark.mKind == MarkKind::PORTAL)
        {
            drawPortal(ghost, mark, color);
        }
        else
        {
            drawMark(ghost, mark, color);
        }
        // Over the world the bodies carry the shape; what they cannot say
        // draws here - a portal's strike and a stair's flight.
        if (mark.mKind == MarkKind::PORTAL)
        {
            drawPortal(selected ? bold : front, mark, selected ? LLColor4::white : color);
        }
        else if (mark.mType == MarkType::STAIR)
        {
            drawStairExtras(selected ? bold : front, mark, selected ? LLColor4::white : color);
        }
        else if (selected)
        {
            drawMark(bold, mark, LLColor4::white);
        }
    }

    if (!mPending.mVerts.empty())
    {
        Mark preview = mPending;
        if (mHasHover && preview.mKind == MarkKind::LINE)
        {
            preview.mVerts.push_back(mHover);
        }
        drawMark(bold, preview, skNavMarkColor(preview.mType));
        if (preview.mType == MarkType::STAIR)
        {
            drawStairExtras(bold, preview, skNavMarkColor(preview.mType));
        }
    }
    if (mHasHover && mArmed)
    {
        cross(bold, agentOf(mHover), 0.2f, LLColor4::yellow);
    }
    if (mHoverPortal.mValid)
    {
        // The portal the cursor is over, over everything, ready to strike.
        bold.edge(agentOf(mHoverPortal.mA), agentOf(mHoverPortal.mB), LLColor4::white);
    }

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            front.draw();
            bold.draw();
        }
        {
            // Behind geometry, fainter, so a mark inside a wall is still found.
            LLGLDepthTest behind(GL_TRUE, GL_FALSE, GL_GREATER);
            ghost.draw(0.6f, 0.4f);
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
    // The selection was the last mark's; the type is for the next one.
    if (mPending.mVerts.empty())
    {
        LLSelectMgr::getInstance()->deselectAll();
        mFollowProxySelection = false;
    }
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
    : LLToolComposite(std::string("NavAnnotate"))
{
    // The gizmo the selected mark's points move with, and the inert child
    // the placing runs under.
    mManip = new LLManipTranslate(this);
    mPlace = new LLTool(std::string("NavAnnotatePlace"), this);
    mCur = mDefault = mPlace;
}

SKToolNavAnnotate::~SKToolNavAnnotate()
{
    delete mManip;
    mManip = nullptr;
    delete mPlace;
    mPlace = nullptr;
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
    mMouseDown = true;
    gViewerWindow->pickAsync(x, y, mask, pickCallback, /*pick_transparent*/ false, /*pick_rigged*/ false,
                             /*pick_unselectable*/ true, /*pick_reflection_probes*/ false);
    return true;
}

bool SKToolNavAnnotate::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mMouseDown = false;
    return LLToolComposite::handleMouseUp(x, y, mask);
}

void SKToolNavAnnotate::pickCallback(const LLPickInfo& pick_info)
{
    SKToolNavAnnotate* self = SKToolNavAnnotate::getInstance();
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    const bool held = self->mMouseDown;
    self->mMouseDown = false;
    const S32 x = pick_info.mMousePt.mX, y = pick_info.mMousePt.mY;
    if (annotator.armed() && markTypeKind(annotator.newType()) == MarkKind::PORTAL)
    {
        // The hovered cut toggles whether or not the world behind it picked.
        annotator.clickPortal();
        return;
    }
    LLViewerObject* hit = pick_info.getObject();
    const bool mark_hit = hit && SKNavMarkObjects::instanceExists() && SKNavMarkObjects::instance().isMarkProxy(hit);
    if (mark_hit)
    {
        annotator.cancelExitPick();
        // A mark's body answers like an object does: the handle clicked
        // selects that point alone, a beam the whole mark.
        LLToolSelect::handleObjectSelection(pick_info, SKNavMarkObjects::instance().isVertexHandle(hit), false);
        if (held)
        {
            // The button is still down: with the gizmo under it, the drag
            // begins here, as a build click does.
            self->mManip->highlightManipulators(x, y);
            if (self->mManip->getHighlightedPart() != LLManip::LL_NO_PART && self->mManip->canAffectSelection())
            {
                self->setCurrentTool(self->mManip);
                self->mManip->handleMouseDownOnPart(x, y, pick_info.mKeyMask);
            }
        }
        return;
    }
    if (hit && annotator.exitPickPending())
    {
        annotator.addExit(pick_info.mPosGlobal);
        return;
    }
    if (hit && !annotator.placingPending() && annotator.selectionHasProxy())
    {
        // A click on the world with a mark selected deselects it, the way a
        // click away does; the next click places.
        LLSelectMgr::getInstance()->deselectAll();
        return;
    }
    if (hit)
    {
        annotator.click(pick_info.mPosGlobal);
        return;
    }
    // The click went past the world entirely.
    LLSelectMgr::getInstance()->deselectAll();
    annotator.cancelExitPick();
}

bool SKToolNavAnnotate::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    if (markTypeKind(annotator.newType()) == MarkKind::PORTAL)
    {
        // A crossed portal toggles once per click; the double click that
        // followed the first press does nothing.
        return true;
    }
    if (markTypeKind(annotator.newType()) != MarkKind::LINE)
    {
        return handleMouseDown(x, y, mask);
    }
    annotator.finish();
    return true;
}

bool SKToolNavAnnotate::handleHover(S32 x, S32 y, MASK mask)
{
    SKNavAnnotator& annotator = SKNavAnnotator::instance();
    if (mask & MASK_ALT)
    {
        SKNavAnnotator::instance().setHover(false, LLVector3d());
        return LLToolCamera::getInstance()->handleHover(x, y, mask);
    }
    if (mManip->hasMouseCapture())
    {
        return mManip->handleHover(x, y, mask);
    }
    // A selected mark's gizmo pre-highlights under the cursor, the way a
    // build tool's does.
    if (annotator.selectionHasProxy())
    {
        mManip->highlightManipulators(x, y);
        if (mManip->getHighlightedPart() != LLManip::LL_NO_PART)
        {
            annotator.setHover(false, LLVector3d());
            setCurrentTool(mManip);
            return mManip->handleHover(x, y, mask);
        }
        setCurrentTool(mPlace);
        gViewerWindow->setCursor(UI_CURSOR_ARROW);
        return true;
    }
    setCurrentTool(mPlace);
    LLVector3d at;
    const bool hit = pick(x, y, at);
    annotator.setHover(hit, at);
    annotator.hoverPortal(x, y);
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
        // A mark half placed goes first; with none, an exit pick does;
        // with neither, the selection does; with nothing selected, the
        // tool does.
        if (annotator.cancel())
        {
            return true;
        }
        if (annotator.exitPickPending())
        {
            annotator.cancelExitPick();
            return true;
        }
        if (annotator.selectionHasProxy())
        {
            LLSelectMgr::getInstance()->deselectAll();
            return true;
        }
        annotator.arm(false);
        return true;
    }
    return LLToolComposite::handleKey(key, mask);
}

LLTool* SKToolNavAnnotate::getOverrideTool(MASK mask)
{
    // Ctrl and Ctrl+Shift work the other gizmos over the same selection,
    // as they do over an object's.
    if (mask == MASK_CONTROL)
    {
        return LLToolCompRotate::getInstance();
    }
    if (mask == (MASK_CONTROL | MASK_SHIFT))
    {
        return LLToolCompScale::getInstance();
    }
    return LLToolComposite::getOverrideTool(mask);
}

void SKToolNavAnnotate::handleSelect()
{
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
}

void SKToolNavAnnotate::handleDeselect()
{
    SKNavAnnotator::instance().setHover(false, LLVector3d());
}
