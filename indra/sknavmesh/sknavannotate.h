/**
 * @file sknavannotate.h
 * @brief Marks placed in the world where the navmesh is wrong: points and lines with a type and a note.
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

#ifndef SK_NAVANNOTATE_H
#define SK_NAVANNOTATE_H

#include "lluuid.h"
#include "stdtypes.h"
#include "v3dmath.h"
#include "v3math.h"

#include <functional>
#include <ostream>
#include <utility>

#include <string>
#include <vector>

namespace sknav
{
    enum class MarkKind : U8
    {
        POINT = 0,
        LINE,
        PORTAL,     ///< a portal the spaces pass cut, named by its two spaces
        COUNT
    };

    enum class MarkType : U8
    {
        // Openings.
        MISSED_OPENING = 0,
        WRONG,          ///< a wrong portal
        DOOR,
        WINDOW,
        // Vertical movement.
        STAIR,
        RAMP,
        LADDER,
        ELEVATOR,
        LEDGE,          ///< a ledge or drop
        // Areas.
        INDOOR,
        SHELTERED,
        OUTDOOR,
        STREET,
        BRIDGE,
        BALCONY,
        COURTYARD,
        WATER_BODY,     ///< a pool or prim water
        // Judgements.
        WALKABLE,       ///< floor the navmesh missed
        NOT_WALKABLE,   ///< floor that should not be
        MERGE,          ///< where two spaces that should be one meet
        SPLIT,          ///< along where a space should be cut in two
        CROSS,          ///< a portal the spaces pass cut that should not be there
        NOTE,
        COUNT
    };

    enum class MarkGroup : U8
    {
        OPENING = 0,
        VERTICAL,
        AREA,
        JUDGEMENT
    };

    /// A spiral stair's winding, in turns of the helix: the range a ring
    /// prim's revolutions take, so the editing offers the same.
    constexpr F64 SPIRAL_MIN_TURNS = 0.25;
    constexpr F64 SPIRAL_MAX_TURNS = 4.0;
    /// The winding a helix is sampled at, radians of arc per vertex: a
    /// quarter turn reads as a polyline without burying an export in points.
    constexpr F64 SPIRAL_PITCH_RAD = 0.25;

    const char* markKindName(MarkKind kind);
    /// Stable and lower-case: the exports and their readers key on it.
    const char* markTypeName(MarkType type);
    MarkGroup markTypeGroup(MarkType type);
    /// The shape a type is placed as: openings and ways up are lines along
    /// them, areas and judgements a point in them.
    MarkKind markTypeKind(MarkType type);
    /// The key that picks the type while placing: a digit or an upper-case
    /// letter, never one the camera and movement keys use.
    char markTypeKey(MarkType type);
    bool markTypeForKey(char key, MarkType& out);

    /// One mark, in global coordinates. Session only: nothing persists it.
    struct Mark
    {
        U32 mId = 0;
        MarkKind mKind = MarkKind::POINT;
        MarkType mType = MarkType::NOTE;
        std::vector<LLVector3d> mVerts;
        // A stair's ways up beyond where its line ends: the top of a flight
        // serves more floors than one. Stair marks; the export lists them
        // beside the vertices.
        std::vector<LLVector3d> mExits;
        // A stair wound as a helix rather than run straight (stair marks):
        // the axis centre at the foot's height, the turn radius, how many
        // turns the climb takes, and which way it winds (+1 right-handed,
        // -1 left). The line keeps its two vertices, foot then top; the
        // helix between them is sampled where a polyline is asked for.
        bool mSpiral = false;
        LLVector3d mSpiralCentre;
        F64 mSpiralRadius = 0.0;
        F64 mSpiralTurns = 1.0;
        S8 mSpiralHand = 1;
        std::string mText;
        F64 mCreated = 0.0;     ///< seconds since the epoch
        // A crossed-out portal (MarkKind::PORTAL) names the cut it strikes:
        // the mesh whose spaces pass cut it, the stable ids of the two spaces
        // it joined, and the cut's centre. The vertex stays where the click
        // landed, for the label and the export.
        LLUUID mSpaceMesh;
        U32 mSpaceIds[2] = { 0, 0 };
        LLVector3d mPortalCentre;

        /// The two space ids of a portal mark, smaller first.
        std::pair<U32, U32> portalPair() const;

        /// The point a share `fraction` (0 the foot, 1 the top) of a spiral
        /// stair's climb sits at: one turn of the helix about the centre, the
        /// rise spread evenly along it. Meaningful only for a spiral.
        LLVector3d helixAt(F64 fraction) const;

        /// The polyline the mark stands for: its vertices, or for a spiral
        /// the helix sampled along the climb. What drawing, the extent and
        /// the export read.
        std::vector<LLVector3d> path() const;

        /// The box around the mark, its exits included.
        void extent(LLVector3d& min, LLVector3d& max) const;
        bool complete() const;
    };

    class MarkSet
    {
    public:
        /// Takes the mark with a fresh id and returns it.
        U32 add(Mark mark);
        bool remove(U32 id);
        void clear() { mMarks.clear(); }
        Mark* find(U32 id);
        const Mark* find(U32 id) const;
        const std::vector<Mark>& marks() const { return mMarks; }
        bool empty() const { return mMarks.empty(); }

    private:
        std::vector<Mark> mMarks;
        U32 mNextId = 1;
    };

    /// Where a vertex is on the grid: its region, the region's grid
    /// coordinates and the region-local position.
    struct MarkPlace
    {
        std::string mRegion;
        U32 mGridX = 0;
        U32 mGridY = 0;
        LLVector3 mLocal;
    };
    typedef std::function<bool(const LLVector3d& global, MarkPlace& out)> MarkLocator;

    /// The marks as an export's "marks" array, key included; a vertex the
    /// locator places gets its region too.
    void writeMarks(std::ostream& out, const std::vector<Mark>& marks, const MarkLocator& locate);

    /// Text as the body of a JSON string: quotes, backslashes and control
    /// characters escaped.
    std::string jsonEscape(const std::string& text);
}

#endif // SK_NAVANNOTATE_H
