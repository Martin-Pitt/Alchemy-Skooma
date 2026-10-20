/**
 * @file sknavannotate.cpp
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

#include "linden_common.h"

#include "sknavannotate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sknav
{
    const char* markKindName(MarkKind kind)
    {
        switch (kind)
        {
            case MarkKind::POINT: return "point";
            case MarkKind::LINE: return "line";
            case MarkKind::PORTAL: return "portal";
            default: return "unknown";
        }
    }

    namespace
    {
        struct TypeInfo
        {
            MarkType mType;
            const char* mName;
            MarkGroup mGroup;
            MarkKind mKind;
            char mKey;
        };

        // In MarkType order. A wrong portal and a merge are points: a click
        // near the portal, or where the two spaces meet. Letters are mnemonic
        // where they can be, and none of W, A, S, D, E, C, F, Q or Z, which
        // move the avatar and the camera.
        const TypeInfo TYPES[] = {
            { MarkType::MISSED_OPENING, "missed_opening", MarkGroup::OPENING, MarkKind::LINE, '1' },
            { MarkType::WRONG, "wrong", MarkGroup::OPENING, MarkKind::POINT, '2' },
            { MarkType::DOOR, "door", MarkGroup::OPENING, MarkKind::LINE, '3' },
            { MarkType::WINDOW, "window", MarkGroup::OPENING, MarkKind::LINE, '4' },
            { MarkType::STAIR, "stair", MarkGroup::VERTICAL, MarkKind::LINE, '5' },
            { MarkType::RAMP, "ramp", MarkGroup::VERTICAL, MarkKind::LINE, '6' },
            { MarkType::LADDER, "ladder", MarkGroup::VERTICAL, MarkKind::LINE, '7' },
            { MarkType::ELEVATOR, "elevator", MarkGroup::VERTICAL, MarkKind::LINE, '8' },
            { MarkType::LEDGE, "ledge", MarkGroup::VERTICAL, MarkKind::LINE, '9' },
            { MarkType::INDOOR, "indoor", MarkGroup::AREA, MarkKind::POINT, 'I' },
            { MarkType::SHELTERED, "sheltered", MarkGroup::AREA, MarkKind::POINT, 'H' },
            { MarkType::OUTDOOR, "outdoor", MarkGroup::AREA, MarkKind::POINT, 'O' },
            { MarkType::STREET, "street", MarkGroup::AREA, MarkKind::LINE, 'T' },
            { MarkType::BRIDGE, "bridge", MarkGroup::AREA, MarkKind::LINE, 'B' },
            { MarkType::BALCONY, "balcony", MarkGroup::AREA, MarkKind::POINT, 'L' },
            { MarkType::COURTYARD, "courtyard", MarkGroup::AREA, MarkKind::POINT, 'Y' },
            { MarkType::WATER_BODY, "water", MarkGroup::AREA, MarkKind::POINT, 'P' },
            { MarkType::WALKABLE, "walkable", MarkGroup::JUDGEMENT, MarkKind::POINT, 'G' },
            { MarkType::NOT_WALKABLE, "not_walkable", MarkGroup::JUDGEMENT, MarkKind::POINT, 'X' },
            { MarkType::MERGE, "merge", MarkGroup::JUDGEMENT, MarkKind::POINT, 'M' },
            { MarkType::SPLIT, "split", MarkGroup::JUDGEMENT, MarkKind::LINE, 'V' },
            { MarkType::CROSS, "crossed", MarkGroup::JUDGEMENT, MarkKind::PORTAL, 'K' },
            { MarkType::NOTE, "note", MarkGroup::JUDGEMENT, MarkKind::POINT, 'N' },
        };
        static_assert(sizeof(TYPES) / sizeof(TYPES[0]) == static_cast<size_t>(MarkType::COUNT), "a row per type");

        const TypeInfo* info(MarkType type)
        {
            const size_t i = static_cast<size_t>(type);
            return i < static_cast<size_t>(MarkType::COUNT) ? &TYPES[i] : nullptr;
        }
    }

    const char* markTypeName(MarkType type)
    {
        const TypeInfo* t = info(type);
        return t ? t->mName : "unknown";
    }

    MarkGroup markTypeGroup(MarkType type)
    {
        const TypeInfo* t = info(type);
        return t ? t->mGroup : MarkGroup::JUDGEMENT;
    }

    MarkKind markTypeKind(MarkType type)
    {
        const TypeInfo* t = info(type);
        return t ? t->mKind : MarkKind::POINT;
    }

    char markTypeKey(MarkType type)
    {
        const TypeInfo* t = info(type);
        return t ? t->mKey : 0;
    }

    bool markTypeForKey(char key, MarkType& out)
    {
        for (const TypeInfo& t : TYPES)
        {
            if (t.mKey == key)
            {
                out = t.mType;
                return true;
            }
        }
        return false;
    }

    LLVector3d Mark::helixAt(F64 fraction) const
    {
        const LLVector3d& foot = mVerts[0];
        const LLVector3d& top = mVerts[1];
        // The helix starts at the foot's angle about the axis and winds the
        // turns the mark names, handed, the rise spread evenly along it.
        const F64 angle = std::atan2(foot.mdV[VY] - mSpiralCentre.mdV[VY], foot.mdV[VX] - mSpiralCentre.mdV[VX]);
        const F64 theta = angle + mSpiralHand * fraction * mSpiralTurns * 2.0 * F_PI;
        return LLVector3d(mSpiralCentre.mdV[VX] + mSpiralRadius * std::cos(theta),
                          mSpiralCentre.mdV[VY] + mSpiralRadius * std::sin(theta),
                          foot.mdV[VZ] + fraction * (top.mdV[VZ] - foot.mdV[VZ]));
    }

    std::vector<LLVector3d> Mark::path() const
    {
        if (!mSpiral || mVerts.size() < 2 || mSpiralRadius <= 0.0)
        {
            return mVerts;
        }
        // One vertex every SPIRAL_PITCH_RAD of winding, then the stored top:
        // a mark read back from its params stays honest about where it ends
        // even if the top no longer sits on the circle exactly.
        std::vector<LLVector3d> out;
        const F64 sweep = std::fabs(mSpiralTurns) * 2.0 * F_PI;
        const size_t steps = std::max<size_t>(1, (size_t)std::ceil(sweep / SPIRAL_PITCH_RAD));
        out.reserve(steps + 1);
        for (size_t i = 0; i < steps; ++i)
        {
            out.push_back(helixAt((F64)i / (F64)steps));
        }
        out.push_back(mVerts[1]);
        return out;
    }

    void Mark::extent(LLVector3d& min, LLVector3d& max) const
    {
        std::vector<LLVector3d> verts = path();
        verts.insert(verts.end(), mExits.begin(), mExits.end());
        if (verts.empty())
        {
            min.clear();
            max.clear();
            return;
        }
        min = max = verts.front();
        for (const LLVector3d& v : verts)
        {
            for (S32 axis = 0; axis < 3; ++axis)
            {
                min.mdV[axis] = std::min(min.mdV[axis], v.mdV[axis]);
                max.mdV[axis] = std::max(max.mdV[axis], v.mdV[axis]);
            }
        }
    }

    std::pair<U32, U32> Mark::portalPair() const
    {
        return std::minmax(mSpaceIds[0], mSpaceIds[1]);
    }

    bool Mark::strikes(const LLUUID& mesh, U32 space_a, U32 space_b, const LLVector3d& centre) const
    {
        return mKind == MarkKind::PORTAL && !mStale && mSpaceMesh == mesh && portalPair() == std::minmax(space_a, space_b) &&
               dist_vec_squared(centre, mPortalCentre) <= CROSS_REACH_M * CROSS_REACH_M;
    }

    U32 MarkSet::staleCrosses(const LLUUID& mesh, const std::vector<Cut>& cuts)
    {
        U32 n = 0;
        for (Mark& m : mMarks)
        {
            if (m.mKind != MarkKind::PORTAL || m.mStale || m.mSpaceMesh != mesh)
            {
                continue;
            }
            const bool live = std::any_of(cuts.begin(), cuts.end(), [&](const Cut& q) { return m.strikes(mesh, q.mSpaces[0], q.mSpaces[1], q.mCentre); });
            if (!live)
            {
                m.mStale = true;
                ++n;
            }
        }
        return n;
    }

    bool Mark::complete() const
    {
        switch (mKind)
        {
            case MarkKind::POINT: return mVerts.size() == 1;
            case MarkKind::LINE: return mVerts.size() >= 2;
            case MarkKind::PORTAL: return mVerts.size() == 1 && mSpaceIds[0] && mSpaceIds[1];
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

    void writeMarks(std::ostream& out, const std::vector<Mark>& marks, const MarkLocator& locate)
    {
        // Globals need ten digits for centimetres, whatever the caller left set.
        const std::streamsize precision = out.precision(std::max<std::streamsize>(out.precision(), 10));
        out << "\"marks\":[";
        auto writeVertex = [&out, &locate](const LLVector3d& v) {
            out << "{\"global\":[" << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
            MarkPlace place;
            if (locate && locate(v, place))
            {
                out << ",\"region\":\"" << jsonEscape(place.mRegion) << "\",\"regionGrid\":[" << place.mGridX << ',' << place.mGridY
                    << "],\"local\":[" << place.mLocal.mV[VX] << ',' << place.mLocal.mV[VY] << ',' << place.mLocal.mV[VZ] << ']';
            }
            out << '}';
        };
        for (size_t m = 0; m < marks.size(); ++m)
        {
            const Mark& mark = marks[m];
            out << (m ? "," : "") << "{\"id\":" << mark.mId << ",\"kind\":\"" << markKindName(mark.mKind) << "\",\"type\":\""
                << markTypeName(mark.mType) << "\",\"text\":\"" << jsonEscape(mark.mText) << "\",\"created\":" << mark.mCreated;
            if (mark.mKind == MarkKind::PORTAL)
            {
                // Which cut this strikes: the mesh, the two spaces it joined
                // and where the cut sat, so a reader of the same export can
                // find it among the portals it wrote.
                out << ",\"mesh\":\"" << mark.mSpaceMesh.asString() << "\",\"spaces\":[" << mark.mSpaceIds[0] << ','
                    << mark.mSpaceIds[1] << "],\"centre\":[" << mark.mPortalCentre.mdV[VX] << ','
                    << mark.mPortalCentre.mdV[VY] << ',' << mark.mPortalCentre.mdV[VZ] << ']';
                if (mark.mStale)
                {
                    out << ",\"stale\":true";
                }
            }
            // A spiral stair's winding, named beside the sampled polyline so
            // a reader keeps the true shape when the pitch it was sampled at
            // is too coarse for it.
            if (mark.mSpiral && mark.mVerts.size() >= 2)
            {
                out << ",\"spiral\":{\"centre\":[" << mark.mSpiralCentre.mdV[VX] << ',' << mark.mSpiralCentre.mdV[VY] << ','
                    << mark.mSpiralCentre.mdV[VZ] << "],\"radius\":" << mark.mSpiralRadius << ",\"turns\":" << mark.mSpiralTurns
                    << ",\"hand\":" << (S32)mark.mSpiralHand << '}';
            }
            out << ",\"vertices\":[";
            const std::vector<LLVector3d> path = mark.path();
            for (size_t i = 0; i < path.size(); ++i)
            {
                out << (i ? "," : "");
                writeVertex(path[i]);
            }
            out << ']';
            // The ways up beyond the line's own end, written like vertices.
            if (!mark.mExits.empty())
            {
                out << ",\"exits\":[";
                for (size_t i = 0; i < mark.mExits.size(); ++i)
                {
                    out << (i ? "," : "");
                    writeVertex(mark.mExits[i]);
                }
                out << ']';
            }
            out << '}';
        }
        out << ']';
        out.precision(precision);
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
