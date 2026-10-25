/**
 * @file skcensusrecord.cpp
 * @brief A census record's geometry and what keys it.
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

#include "skcensusrecord.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using namespace skworld;

    const S32 CYLINDER_SIDES = 16;

    const S32 SPHERE_RINGS = 6;
    const S32 SPHERE_SEGMENTS = 12;
    void tri(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c)
    {
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }

    // a, b, c, d counter-clockwise seen from outside.
    void quad(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
    {
        tri(out, a, b, c);
        tri(out, a, c, d);
    }

    LLVector3 at(const Record& r, F32 u, F32 v, F32 w)
    {
        return r.mCenter + r.mAxes[0] * (u * r.mHalf[0]) + r.mAxes[1] * (v * r.mHalf[1]) + r.mAxes[2] * (w * r.mHalf[2]);
    }

    void emitBox(const Record& r, std::vector<LLVector3>& out)
    {
        const LLVector3 c000 = at(r, -1, -1, -1), c100 = at(r, 1, -1, -1), c110 = at(r, 1, 1, -1), c010 = at(r, -1, 1, -1);
        const LLVector3 c001 = at(r, -1, -1, 1), c101 = at(r, 1, -1, 1), c111 = at(r, 1, 1, 1), c011 = at(r, -1, 1, 1);
        quad(out, c001, c101, c111, c011); // +w (top when upright)
        quad(out, c000, c010, c110, c100); // -w
        quad(out, c000, c100, c101, c001); // -v
        quad(out, c010, c011, c111, c110); // +v
        quad(out, c000, c001, c011, c010); // -u
        quad(out, c100, c110, c111, c101); // +u
    }

    void emitCylinder(const Record& r, std::vector<LLVector3>& out)
    {
        const LLVector3 top = at(r, 0, 0, 1), bottom = at(r, 0, 0, -1);
        for (S32 i = 0; i < CYLINDER_SIDES; ++i)
        {
            const F32 a0 = F_TWO_PI * i / CYLINDER_SIDES, a1 = F_TWO_PI * (i + 1) / CYLINDER_SIDES;
            const F32 u0 = std::cos(a0), v0 = std::sin(a0), u1 = std::cos(a1), v1 = std::sin(a1);
            const LLVector3 b0 = at(r, u0, v0, -1), b1 = at(r, u1, v1, -1), t0 = at(r, u0, v0, 1), t1 = at(r, u1, v1, 1);
            quad(out, b0, b1, t1, t0);      // side, outward
            tri(out, top, t0, t1);          // cap, up
            tri(out, bottom, b1, b0);       // base, down
        }
    }

    void emitSphere(const Record& r, std::vector<LLVector3>& out)
    {
        for (S32 ring = 0; ring < SPHERE_RINGS; ++ring)
        {
            const F32 p0 = F_PI * ring / SPHERE_RINGS - F_PI_BY_TWO, p1 = F_PI * (ring + 1) / SPHERE_RINGS - F_PI_BY_TWO;
            for (S32 seg = 0; seg < SPHERE_SEGMENTS; ++seg)
            {
                const F32 t0 = F_TWO_PI * seg / SPHERE_SEGMENTS, t1 = F_TWO_PI * (seg + 1) / SPHERE_SEGMENTS;
                const LLVector3 a = at(r, std::cos(p0) * std::cos(t0), std::cos(p0) * std::sin(t0), std::sin(p0));
                const LLVector3 b = at(r, std::cos(p0) * std::cos(t1), std::cos(p0) * std::sin(t1), std::sin(p0));
                const LLVector3 c = at(r, std::cos(p1) * std::cos(t1), std::cos(p1) * std::sin(t1), std::sin(p1));
                const LLVector3 d = at(r, std::cos(p1) * std::cos(t0), std::cos(p1) * std::sin(t0), std::sin(p1));
                quad(out, a, b, c, d);
            }
        }
    }

    U64 quant(U64 h, F64 v, F64 step)
    {
        return mix(h, static_cast<U64>(std::llround(v / step)));
    }

    U64 quantVec(U64 h, const LLVector3& v, F64 step)
    {
        for (S32 i = 0; i < 3; ++i)
        {
            h = quant(h, v.mV[i], step);
        }
        return h;
    }

    U64 quantVecD(U64 h, const LLVector3d& v, F64 step)
    {
        for (S32 i = 0; i < 3; ++i)
        {
            h = quant(h, v.mdV[i], step);
        }
        return h;
    }

    U64 mixId(U64 h, const LLUUID& id)
    {
        for (U8 b : id.mData)
        {
            h = mix(h, b);
        }
        return h;
    }
}

namespace skworld
{
    U64 finalise(U64 h)
    {
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        return h ^ (h >> 31);
    }

    U64 recordSignature(const std::vector<RecordPtr>& records)
    {
        // Order free: a grid hands the same records back in whatever order the census scan produced them.
        U64 h = 0;
        for (const RecordPtr& r : records)
        {
            h += mix(SEED, r->mSignature);
        }
        return h;
    }

    U64 recordContentKey(const Record& record)
    {
        U64 h = SEED;
        U32 tail;
        std::memcpy(&tail, record.mPart.mData + 12, sizeof(tail));
        // A placeholder from the region's object cache is named by its local
        // id, which the sim reassigns; its box is what it is.
        if (tail != PLACEHOLDER_TAG)
        {
            h = mixId(h, record.mPart);
        }
        h = mix(h, static_cast<U64>(record.mRole));
        h = mix(h, static_cast<U64>(record.mProvenance));
        h = mix(h, static_cast<U64>(record.mNavRole));
        h = mix(h, static_cast<U64>(record.mShape));
        h = mix(h, record.mDynamic);
        h = mix(h, record.mSolid);
        h = mix(h, static_cast<U64>(record.mFeature));
        h = mix(h, record.mExclude);
        h = quantVecD(h, record.mOrigin, KEY_POSITION_M);
        h = quantVec(h, record.mCenter, KEY_POSITION_M);
        for (const LLVector3& axis : record.mAxes)
        {
            h = quantVec(h, axis, KEY_UNIT);
        }
        h = quantVec(h, record.mHalf, KEY_POSITION_M);
        h = quantVecD(h, record.mMin, KEY_POSITION_M);
        h = quantVecD(h, record.mMax, KEY_POSITION_M);
        h = mix(h, record.mConvexTris.size());
        for (S32 n : record.mConvexTris)
        {
            h = mix(h, static_cast<U64>(n));
        }
        if (record.mTriangles)
        {
            h = mix(h, record.mTriangles->size());
            for (const LLVector3& v : *record.mTriangles)
            {
                h = quantVec(h, v, KEY_POSITION_M);
            }
        }
        return h;
    }

    void emitRecord(const Record& record, const LLVector3d& mesh_origin, std::vector<LLVector3>& out)
    {
        // F64 until the shift is applied, then F32 in mesh space.
        const LLVector3d shift64 = record.mOrigin - mesh_origin;
        const LLVector3 shift(static_cast<F32>(shift64.mdV[VX]), static_cast<F32>(shift64.mdV[VY]), static_cast<F32>(shift64.mdV[VZ]));
        // Analytic shapes tessellate into scratch kept per thread, not a vector per call.
        thread_local std::vector<LLVector3> local;
        local.clear();
        const std::vector<LLVector3>* src = &local;
        switch (record.mShape)
        {
            case Shape::TRIANGLES:
                if (record.mTriangles)
                {
                    src = record.mTriangles.get();
                }
                break;
            case Shape::BOX: emitBox(record, local); break;
            case Shape::CYLINDER: emitCylinder(record, local); break;
            case Shape::SPHERE: emitSphere(record, local); break;
        }
        // Grown geometrically: a caller appending record after record into one
        // vector would otherwise reallocate it at every record.
        const size_t first = out.size(), count = src->size();
        if (out.capacity() < first + count)
        {
            out.reserve(std::max(first + count, out.capacity() * 2));
        }
        out.resize(first + count);
        LLVector3* dst = out.data() + first;
        const LLVector3* from = src->data();
        for (size_t i = 0; i < count; ++i)
        {
            dst[i] = from[i] + shift;
        }
    }
}
