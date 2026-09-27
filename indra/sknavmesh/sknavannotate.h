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

#include "stdtypes.h"
#include "v3dmath.h"
#include "v3math.h"

#include <functional>
#include <ostream>

#include <string>
#include <vector>

namespace sknav
{
    enum class MarkKind : U8
    {
        POINT = 0,
        LINE,
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
        std::string mText;
        F64 mCreated = 0.0;     ///< seconds since the epoch

        /// The box around the mark's vertices.
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
