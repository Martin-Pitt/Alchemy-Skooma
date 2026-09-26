/**
 * @file sknavannotate.h
 * @brief Marks placed in the world where the navmesh is wrong: points, lines and boxes with a type and a note.
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

#include <string>
#include <vector>

namespace sknav
{
    enum class MarkKind : U8
    {
        POINT = 0,
        LINE,
        BOX,        ///< two opposite floor corners, then a height
        COUNT
    };

    enum class MarkType : U8
    {
        MISSED_OPENING = 0,
        WRONG,
        MERGE,
        SPLIT,
        STAIR,
        WINDOW,
        INDOOR,
        OUTDOOR,
        SHELTERED,
        NOTE,
        COUNT
    };

    const char* markKindName(MarkKind kind);
    const char* markTypeName(MarkType type);

    /// One mark, in global coordinates. Session only: nothing persists it.
    struct Mark
    {
        U32 mId = 0;
        MarkKind mKind = MarkKind::POINT;
        MarkType mType = MarkType::NOTE;
        std::vector<LLVector3d> mVerts;
        F32 mHeight = 3.f;      ///< a box's, up from the lower of its two corners
        std::string mText;
        F64 mCreated = 0.0;     ///< seconds since the epoch

        /// The mark's extent: its vertices, and for a box its height too.
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

        /// The AABB of every mark grown by the margin on each side. False
        /// when there are no marks.
        bool bounds(F64 margin, LLVector3d& min, LLVector3d& max) const;

    private:
        std::vector<Mark> mMarks;
        U32 mNextId = 1;
    };

    /// Text as the body of a JSON string: quotes, backslashes and control
    /// characters escaped.
    std::string jsonEscape(const std::string& text);
}

#endif // SK_NAVANNOTATE_H
