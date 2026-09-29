/**
 * @file sknavmarkobjects.h
 * @brief The client-only bodies of navmesh annotation marks: prims a click selects and the gizmos drag.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SK_NAVMARKOBJECTS_H
#define SK_NAVMARKOBJECTS_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "sknavannotate.h"

#include <map>
#include <string>
#include <vector>

class LLViewerObject;
class LLViewerRegion;
class SKNavAnnotator;

/// The bodies of a session's marks. Each mark carries client-only prims - a
/// handle prim at every vertex and exit, a beam along every straight stretch,
/// a handle on a spiral's axis - so a mark selects, highlights and moves like
/// an object does, the build tools' gizmos included, and no sim ever hears of
/// it. The prims follow the marks, the marks follow the prims: a handle a
/// gizmo dragged lands back in its mark's vertices, a mark edited by hand
/// moves its handles. Main thread only.
class SKNavMarkObjects : public LLSingleton<SKNavMarkObjects>
{
    LLSINGLETON(SKNavMarkObjects);
    ~SKNavMarkObjects() override;

public:
    /// Bring the prims level with the marks: spawn what is new, despawn what
    /// is gone, a moved prim back into its mark, a changed mark out to its
    /// prims. Once a frame, from the annotator.
    void sync(SKNavAnnotator& annotator);

    /// Whether this prim is a body of one of the marks, and which mark.
    bool isMarkProxy(const LLViewerObject* object) const;
    U32 markIdOf(const LLViewerObject* object) const;
    /// A vertex or exit handle: selecting one selects that point alone,
    /// gizmo and all, rather than the whole mark.
    bool isVertexHandle(const LLViewerObject* object) const;

    /// The prim a mark is selected by whole.
    LLViewerObject* rootProxy(U32 mark_id) const;

    /// Name and description a picker shows for one prim of a mark. False
    /// when the prim is no body of a mark.
    bool proxyDisplay(const LLViewerObject* object, std::string& name, std::string& description) const;

    /// Take one mark's prims out of the world; the mark itself is the
    /// annotator's to drop.
    void despawnMark(U32 mark_id);

    /// Take every mark's body out of the world; the marks themselves are
    /// the annotator's to clear.
    void despawnAll();

private:
    enum ERole : U8
    {
        ROOT_HANDLE = 0,    ///< a vertex handle that roots the mark's linkset
        VERTEX_HANDLE,
        EXIT_HANDLE,
        CENTRE_HANDLE,      ///< a spiral's axis
        BEAM
    };

    struct Ref
    {
        U32 mMarkId = 0;
        ERole mRole = ROOT_HANDLE;
        S32 mIndex = 0;     ///< the vertex, exit or beam it stands for
    };

    struct MarkPrims
    {
        std::vector<LLPointer<LLViewerObject>> mHandles;    ///< one per vertex; the first roots the linkset
        std::vector<LLPointer<LLViewerObject>> mExits;
        LLPointer<LLViewerObject> mCentre;                  ///< a spiral's axis handle
        std::vector<LLPointer<LLViewerObject>> mBeams;      ///< one per straight stretch
    };

    void spawn(const sknav::Mark& mark, MarkPrims& prims);
    void despawnPrims(MarkPrims& prims);
    /// A prim the gizmo or a grab has moved, back into its mark. True when
    /// anything in the mark changed.
    bool derive(sknav::Mark& mark, const MarkPrims& prims);
    /// A mark the annotator has changed, out to its prims.
    void reassert(const sknav::Mark& mark, MarkPrims& prims);
    /// A straight stretch between two points, laid along its beam prim,
    /// parented to a mark's root.
    void placeBeam(LLViewerObject* beam, LLViewerObject* root, const LLVector3d& a, const LLVector3d& b);
    /// The shape the body takes for a mark: how many handles and beams.
    void wantFor(const sknav::Mark& mark, size_t& handles, size_t& beams, bool& centre) const;
    LLViewerObject* makeHandle(const sknav::Mark& mark, F32 size);
    LLViewerObject* makeBeam(const sknav::Mark& mark);

    std::map<U32, MarkPrims> mProxies;      ///< by mark id
    std::map<LLUUID, Ref> mByUUID;          ///< every prim of every mark
    U32 mSeenRevision = 0;                  ///< the annotator revision the prims were last level at
    LLViewerRegion* mRegion = nullptr;      ///< the region the bodies were rezzed against
};

#endif // SK_NAVMARKOBJECTS_H