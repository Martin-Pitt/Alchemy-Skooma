/**
 * @file sknavannotate.cpp
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

#include "linden_common.h"

#include "sknavannotate.h"

#include <algorithm>
#include <cstdio>

namespace sknav
{
    const char* markKindName(MarkKind kind)
    {
        switch (kind)
        {
            case MarkKind::POINT: return "point";
            case MarkKind::LINE: return "line";
            case MarkKind::BOX: return "box";
            default: return "unknown";
        }
    }

    const char* markTypeName(MarkType type)
    {
        switch (type)
        {
            case MarkType::MISSED_OPENING: return "missed_opening";
            case MarkType::WRONG: return "wrong";
            case MarkType::MERGE: return "merge";
            case MarkType::SPLIT: return "split";
            case MarkType::STAIR: return "stair";
            case MarkType::WINDOW: return "window";
            case MarkType::INDOOR: return "indoor";
            case MarkType::OUTDOOR: return "outdoor";
            case MarkType::SHELTERED: return "sheltered";
            case MarkType::NOTE: return "note";
            default: return "unknown";
        }
    }

    void Mark::extent(LLVector3d& min, LLVector3d& max) const
    {
        if (mVerts.empty())
        {
            min.clear();
            max.clear();
            return;
        }
        min = max = mVerts.front();
        for (const LLVector3d& v : mVerts)
        {
            for (S32 axis = 0; axis < 3; ++axis)
            {
                min.mdV[axis] = std::min(min.mdV[axis], v.mdV[axis]);
                max.mdV[axis] = std::max(max.mdV[axis], v.mdV[axis]);
            }
        }
        if (mKind == MarkKind::BOX)
        {
            max.mdV[VZ] = min.mdV[VZ] + std::max(mHeight, 0.f);
        }
    }

    bool Mark::complete() const
    {
        switch (mKind)
        {
            case MarkKind::POINT: return mVerts.size() == 1;
            case MarkKind::LINE: return mVerts.size() >= 2;
            case MarkKind::BOX: return mVerts.size() == 2;
            default: return false;
        }
    }

    U32 MarkSet::add(Mark mark)
    {
        mark.mId = mNextId++;
        mMarks.push_back(std::move(mark));
        return mMarks.back().mId;
    }

    bool MarkSet::remove(U32 id)
    {
        const auto it = std::find_if(mMarks.begin(), mMarks.end(), [id](const Mark& m) { return m.mId == id; });
        if (it == mMarks.end())
        {
            return false;
        }
        mMarks.erase(it);
        return true;
    }

    Mark* MarkSet::find(U32 id)
    {
        const auto it = std::find_if(mMarks.begin(), mMarks.end(), [id](const Mark& m) { return m.mId == id; });
        return it == mMarks.end() ? nullptr : &*it;
    }

    const Mark* MarkSet::find(U32 id) const
    {
        return const_cast<MarkSet*>(this)->find(id);
    }

    bool MarkSet::bounds(F64 margin, LLVector3d& min, LLVector3d& max) const
    {
        bool any = false;
        for (const Mark& m : mMarks)
        {
            if (m.mVerts.empty())
            {
                continue;
            }
            LLVector3d lo, hi;
            m.extent(lo, hi);
            if (!any)
            {
                min = lo;
                max = hi;
                any = true;
                continue;
            }
            for (S32 axis = 0; axis < 3; ++axis)
            {
                min.mdV[axis] = std::min(min.mdV[axis], lo.mdV[axis]);
                max.mdV[axis] = std::max(max.mdV[axis], hi.mdV[axis]);
            }
        }
        if (any)
        {
            const LLVector3d grow(margin, margin, margin);
            min -= grow;
            max += grow;
        }
        return any;
    }

    std::string jsonEscape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
            }
        }
        return out;
    }
}
