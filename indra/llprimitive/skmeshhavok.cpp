/**
 * @file skmeshhavok.cpp
 * @brief Reader for the physics_havok section of a mesh asset
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "skmeshhavok.h"

#include "llsdserialize.h"

#include <zlib.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace
{
    constexpr F64 ROOT_RANGE = 65536.0;
    constexpr S16 OPEN_LO = std::numeric_limits<S16>::min();
    constexpr S16 OPEN_HI = std::numeric_limits<S16>::max();
    constexpr F64 MAX_ABS_VALUE = 1.0e7; // mass properties and MOPP origin; meshes are metres across

    // Projection of a k-DOP direction: (cx*x + cy*y + cz*z + k) / n, a negative component reading 255 - c.
    struct Direction
    {
        S32 c[3];
        S32 k;
        S32 n;
    };

    const Direction DIRECTIONS[13] = {
        { { 1, 0, 0 }, 0, 1 },    { { 0, 1, 0 }, 0, 1 },     { { 0, 0, 1 }, 0, 1 },
        { { 0, 1, 1 }, 0, 2 },    { { 0, 1, -1 }, 255, 2 },  { { 1, 0, 1 }, 0, 2 },
        { { 1, 0, -1 }, 255, 2 }, { { 1, 1, 0 }, 0, 2 },     { { 1, -1, 0 }, 255, 2 },
        { { 1, 1, 1 }, 0, 3 },    { { 1, 1, -1 }, 255, 3 },  { { 1, -1, 1 }, 255, 3 },
        { { 1, -1, -1 }, 510, 3 },
    };

    const Direction& direction_of(U8 opcode)
    {
        if (opcode >= 0x10 && opcode <= 0x1C)
        {
            return DIRECTIONS[opcode - 0x10];
        }
        // 0x20.., 0x23.., 0x26.. are the x, y, z axes.
        return DIRECTIONS[(opcode - 0x20) % 3];
    }

    // Axis of an axis-aligned opcode, or -1.
    S32 axis_of(U8 opcode)
    {
        if (opcode >= 0x10 && opcode <= 0x12)
        {
            return opcode - 0x10;
        }
        if (opcode >= 0x20 && opcode <= 0x28)
        {
            return (opcode - 0x20) % 3;
        }
        return -1;
    }

    bool fail(SKHavokError& error, const char* what, U64 offset)
    {
        error.mWhat = what;
        error.mOffset = offset;
        return false;
    }

    struct WalkState
    {
        U32 mPos;
        U32 mKey;
        U32 mFrame;
        S32 mConstraint;
        U32 mDepth;
        F64 mMin[3]; // root byte coordinates
        F64 mMax[3];
    };

    void narrow_box(WalkState& state, const SKMoppFrame& frame, S32 axis, S32 lo, S32 hi)
    {
        if (lo != OPEN_LO)
        {
            state.mMin[axis] = llmax(state.mMin[axis], lo / frame.mFactor + frame.mOffset[axis]);
        }
        if (hi != OPEN_HI)
        {
            state.mMax[axis] = llmin(state.mMax[axis], hi / frame.mFactor + frame.mOffset[axis]);
        }
    }

    bool finite_in_range(F64 value)
    {
        return std::isfinite(value) && std::fabs(value) <= MAX_ABS_VALUE;
    }

    bool read_real_array(const LLSD& value, size_t count, F64* out)
    {
        if (!value.isArray() || value.size() != count)
        {
            return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            const LLSD& element = value[i];
            if (!element.isReal() && !element.isInteger())
            {
                return false;
            }
            out[i] = element.asReal();
        }
        return true;
    }

    void read_mass_props(const LLSD& value, SKHavokMassProps& out)
    {
        out = SKHavokMassProps();
        if (!value.isMap())
        {
            return;
        }
        F64 com[3];
        F64 inertia[9];
        const LLSD& mass = value["mass"];
        const LLSD& volume = value["volume"];
        if (!read_real_array(value["CoM"], 3, com) || !read_real_array(value["inertia"], 9, inertia)
            || !(mass.isReal() || mass.isInteger()) || !(volume.isReal() || volume.isInteger()))
        {
            return;
        }
        const F64 mass_value = mass.asReal();
        const F64 volume_value = volume.asReal();
        bool valid = finite_in_range(mass_value) && finite_in_range(volume_value) && volume_value >= 0.0;
        for (S32 i = 0; valid && i < 3; ++i)
        {
            valid = finite_in_range(com[i]);
        }
        for (S32 i = 0; valid && i < 9; ++i)
        {
            valid = finite_in_range(inertia[i]);
        }
        if (!valid)
        {
            return;
        }
        out.mCoM.set((F32)com[0], (F32)com[1], (F32)com[2]);
        for (S32 i = 0; i < 9; ++i)
        {
            out.mInertia[i] = (F32)inertia[i];
        }
        out.mMass = (F32)mass_value;
        out.mVolume = (F32)volume_value;
        out.mValid = true;
    }
}

U32 SKMoppTree::getHalfSpaces(const SKMoppConstraint& constraint, SKMoppHalfSpace out[2]) const
{
    if (constraint.mFrame >= mFrames.size() || !(mScale > 0.0))
    {
        return 0;
    }
    // proj = (c . q' + k) / n with q' = (q - offset) * factor and q = (p - origin) * scale / 65536.
    const Direction& dir = direction_of(constraint.mOpcode);
    const SKMoppFrame& frame = mFrames[constraint.mFrame];
    const F64 s = mScale / ROOT_RANGE;
    F64 a[3];
    F64 b = dir.k;
    for (S32 i = 0; i < 3; ++i)
    {
        a[i] = dir.c[i] * frame.mFactor * s / dir.n;
        b -= dir.c[i] * frame.mFactor * (s * mOrigin[i] + frame.mOffset[i]);
    }
    b /= dir.n;

    U32 count = 0;
    if (constraint.mHi != OPEN_HI)
    {
        SKMoppHalfSpace& h = out[count++];
        h.mNormal[0] = a[0];
        h.mNormal[1] = a[1];
        h.mNormal[2] = a[2];
        h.mDistance = constraint.mHi - b;
    }
    if (constraint.mLo != OPEN_LO)
    {
        SKMoppHalfSpace& h = out[count++];
        h.mNormal[0] = -a[0];
        h.mNormal[1] = -a[1];
        h.mNormal[2] = -a[2];
        h.mDistance = b - constraint.mLo;
    }
    for (U32 i = 0; i < count; ++i)
    {
        if (!std::isfinite(out[i].mNormal[0]) || !std::isfinite(out[i].mNormal[1]) || !std::isfinite(out[i].mNormal[2])
            || !std::isfinite(out[i].mDistance))
        {
            return 0;
        }
    }
    return count;
}

bool SKHavokWelding::getEdgeCode(U32 triangle, U32 edge, U8& code) const
{
    if (edge > 2)
    {
        return false;
    }
    if (triangle < mCodes.size())
    {
        code = (U8)((mCodes[triangle] >> (5 * edge)) & 31);
        return true;
    }
    if (mHasPartial && triangle == mCodes.size() && edge == 0)
    {
        code = mPartialByte & 31;
        return true;
    }
    return false;
}

bool SKHavokWelding::getFlag(U32 triangle, bool& flag) const
{
    if (triangle < mCodes.size())
    {
        flag = (mCodes[triangle] & 0x8000) != 0;
        return true;
    }
    return false;
}

std::vector<U32> SKPhysicsMesh::getTriangleCounts() const
{
    std::vector<U32> counts;
    counts.reserve(mSubmeshes.size());
    for (const SKPhysicsMeshSubmesh& submesh : mSubmeshes)
    {
        counts.push_back(submesh.getTriangleCount());
    }
    return counts;
}

static bool walk_mopp(const U8* code, size_t length, const F64 origin[3], F64 scale, SKMoppTree& out,
                      const SKHavokLimits& limits, SKHavokError& error)
{
    out.mFrames.clear();
    out.mConstraints.clear();
    out.mLeaves.clear();
    for (S32 i = 0; i < 3; ++i)
    {
        if (!finite_in_range(origin[i]))
        {
            return fail(error, "MoppInfo origin is not finite", 0);
        }
        out.mOrigin[i] = origin[i];
    }
    // The root box, 255 units of 65536 / scale, has to stay a representable size in the mesh frame.
    if (!std::isfinite(scale) || scale <= 0.0 || scale > 1.0e15 || ROOT_RANGE * 255.0 / scale > MAX_ABS_VALUE)
    {
        return fail(error, "MoppInfo scale is out of range", 0);
    }
    out.mScale = scale;
    if (!code || length == 0)
    {
        return fail(error, "MoppData is empty", 0);
    }
    if (length > limits.mMaxCodeBytes)
    {
        return fail(error, "MoppData is over the size limit", 0);
    }

    out.mFrames.emplace_back();
    std::vector<WalkState> stack;
    WalkState root = { 0, 0, 0, -1, 0, { 0.0, 0.0, 0.0 }, { 255.0, 255.0, 255.0 } };
    stack.push_back(root);

    // A well-formed tree reads each byte once; anything visiting more is a DAG or a loop.
    U64 budget = (U64)length * 2 + 16;
    auto walk_bytes = [&]()
    {
        return (U64)length + out.mConstraints.size() * sizeof(SKMoppConstraint) + out.mFrames.size() * sizeof(SKMoppFrame)
               + out.mLeaves.size() * sizeof(SKMoppLeaf) + stack.size() * sizeof(WalkState);
    };

    auto add_constraint = [&](WalkState& state, U8 opcode, U8 kind, S32 lo, S32 hi) -> bool
    {
        if (out.mConstraints.size() >= limits.mMaxConstraints)
        {
            return false;
        }
        SKMoppConstraint c;
        c.mOpcode = opcode;
        c.mKind = kind;
        c.mLo = (S16)lo;
        c.mHi = (S16)hi;
        c.mFrame = state.mFrame;
        c.mParent = state.mConstraint;
        out.mConstraints.push_back(c);
        state.mConstraint = (S32)out.mConstraints.size() - 1;
        const S32 axis = axis_of(opcode);
        if (axis >= 0)
        {
            narrow_box(state, out.mFrames[state.mFrame], axis, lo, hi);
        }
        return true;
    };

    while (!stack.empty())
    {
        WalkState state = stack.back();
        stack.pop_back();

        while (true)
        {
            const U64 pos = state.mPos;
            if (budget-- == 0)
            {
                return fail(error, "MoppData walk is over its step budget", pos);
            }
            if (walk_bytes() > limits.mMaxWalkBytes)
            {
                return fail(error, "MoppData walk is over its memory budget", pos);
            }
            if (pos >= length)
            {
                return fail(error, "MoppData ends inside the tree", pos);
            }
            const U8 op = code[pos];
            auto has = [&](U64 operands) { return pos + operands < length; };
            auto u8 = [&](U64 at) { return (U32)code[pos + at]; };
            auto be = [&](U64 at, U32 width)
            {
                U32 value = 0;
                for (U32 i = 0; i < width; ++i)
                {
                    value = (value << 8) | code[pos + at + i];
                }
                return value;
            };

            if (op >= 0x26 && op <= 0x28)
            {
                if (!has(2))
                {
                    return fail(error, "MoppData ends inside a bound", pos);
                }
                if (!add_constraint(state, op, SKMoppConstraint::BOUND, (S32)u8(1), (S32)u8(2)))
                {
                    return fail(error, "MOPP constraint budget exceeded", pos);
                }
                state.mPos += 3;
            }
            else if (op >= 0x01 && op <= 0x04)
            {
                if (!has(3))
                {
                    return fail(error, "MoppData ends inside a rescale", pos);
                }
                if (out.mFrames.size() >= limits.mMaxFrames)
                {
                    return fail(error, "MOPP frame budget exceeded", pos);
                }
                const SKMoppFrame parent = out.mFrames[state.mFrame];
                SKMoppFrame frame;
                for (S32 i = 0; i < 3; ++i)
                {
                    frame.mOffset[i] = parent.mOffset[i] + u8(1 + i) / parent.mFactor;
                }
                frame.mFactor = parent.mFactor * (F64)(1u << op);
                if (frame.mFactor > limits.mMaxFrameFactor)
                {
                    return fail(error, "MOPP rescale is out of range", pos);
                }
                out.mFrames.push_back(frame);
                state.mFrame = (U32)out.mFrames.size() - 1;
                state.mPos += 4;
            }
            else if (op == 0x09 || op == 0x0A || op == 0x0B)
            {
                const U32 width = op == 0x09 ? 1 : op == 0x0A ? 2 : 4;
                if (!has(width))
                {
                    return fail(error, "MoppData ends inside a key operand", pos);
                }
                const U32 value = be(1, width);
                state.mKey = op == 0x0B ? value : state.mKey + value;
                state.mPos += 1 + width;
            }
            else if (op == 0x05 || op == 0x06)
            {
                const U32 width = op == 0x05 ? 1 : 2;
                if (!has(width))
                {
                    return fail(error, "MoppData ends inside a jump", pos);
                }
                state.mPos += 1 + width + be(1, width);
            }
            else if ((op >= 0x10 && op <= 0x1C) || (op >= 0x20 && op <= 0x25))
            {
                U32 first;
                U32 second;
                S32 first_hi;
                S32 second_lo;
                if (op <= 0x1C)
                {
                    if (!has(3))
                    {
                        return fail(error, "MoppData ends inside a split", pos);
                    }
                    first_hi = (S32)u8(1);
                    second_lo = (S32)u8(2);
                    first = state.mPos + 4;
                    second = first + u8(3);
                }
                else if (op <= 0x22)
                {
                    if (!has(2))
                    {
                        return fail(error, "MoppData ends inside a split", pos);
                    }
                    // One value: measured over 4244 trees, the first child reaches a + 1, not a.
                    first_hi = (S32)u8(1) + 1;
                    second_lo = (S32)u8(1);
                    first = state.mPos + 3;
                    second = first + u8(2);
                }
                else
                {
                    if (!has(6))
                    {
                        return fail(error, "MoppData ends inside a split", pos);
                    }
                    first_hi = (S32)u8(1);
                    second_lo = (S32)u8(2);
                    first = state.mPos + 7 + be(3, 2);
                    second = state.mPos + 7 + be(5, 2);
                }
                WalkState second_state = state;
                second_state.mPos = second;
                second_state.mDepth = state.mDepth + 1;
                if (!add_constraint(second_state, op, SKMoppConstraint::SPLIT_SECOND, second_lo, OPEN_HI))
                {
                    return fail(error, "MOPP constraint budget exceeded", pos);
                }
                state.mPos = first;
                state.mDepth += 1;
                if (!add_constraint(state, op, SKMoppConstraint::SPLIT_FIRST, OPEN_LO, first_hi))
                {
                    return fail(error, "MOPP constraint budget exceeded", pos);
                }
                stack.push_back(second_state);
            }
            else if (op >= 0x30 && op <= 0x53)
            {
                U32 value = op - 0x30;
                if (op >= 0x50)
                {
                    const U32 width = op - 0x4F;
                    if (!has(width))
                    {
                        return fail(error, "MoppData ends inside a leaf", pos);
                    }
                    value = be(1, width);
                }
                if (out.mLeaves.size() >= limits.mMaxLeaves)
                {
                    return fail(error, "MOPP leaf budget exceeded", pos);
                }
                SKMoppLeaf leaf;
                leaf.mPosition = state.mPos;
                leaf.mKey = state.mKey + value;
                leaf.mDepth = state.mDepth;
                leaf.mConstraint = state.mConstraint;
                const F64 step = ROOT_RANGE / scale;
                for (S32 i = 0; i < 3; ++i)
                {
                    if (state.mMin[i] > state.mMax[i])
                    {
                        return fail(error, "MOPP leaf bounds are empty", pos);
                    }
                    leaf.mMin.mV[i] = (F32)(origin[i] + state.mMin[i] * step);
                    leaf.mMax.mV[i] = (F32)(origin[i] + state.mMax[i] * step);
                }
                out.mLeaves.push_back(leaf);
                break;
            }
            else
            {
                return fail(error, "unknown MOPP opcode", pos);
            }
        }
    }
    return true;
}

// Runs body, turning any exception into a failure: nothing may escape onto the thread that called.
template <typename BODY>
static bool guarded(SKHavokError& error, BODY&& body)
{
    try
    {
        return body();
    }
    catch (const std::bad_alloc&)
    {
        error.mOutOfMemory = true;
        return fail(error, "out of memory", error.mOffset);
    }
    catch (...)
    {
        return fail(error, "unexpected exception", error.mOffset);
    }
}

bool skWalkMopp(const U8* code, size_t length, const F64 origin[3], F64 scale, SKMoppTree& out,
                const SKHavokLimits& limits, SKHavokError& error)
{
    const bool ok = guarded(error, [&]() { return walk_mopp(code, length, origin, scale, out, limits, error); });
    if (!ok)
    {
        out.mFrames.clear();
        out.mConstraints.clear();
        out.mLeaves.clear();
    }
    return ok;
}

static bool inflate_llsd(const U8* data, size_t size, LLSD& out, const SKHavokLimits& limits, SKHavokError& error)
{
    out.clear();
    if (!data || size == 0)
    {
        return fail(error, "section is empty", 0);
    }
    if (size > limits.mMaxCompressedBytes || size > (size_t)std::numeric_limits<uInt>::max())
    {
        return fail(error, "compressed section is over the size limit", 0);
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK)
    {
        return fail(error, "inflate could not start", 0);
    }
    stream.next_in = const_cast<U8*>(data);
    stream.avail_in = (uInt)size;

    std::string inflated;
    constexpr size_t CHUNK = 64 * 1024;
    int result = Z_OK;
    try
    {
        while (result != Z_STREAM_END)
        {
            const size_t used = inflated.size();
            if (used >= limits.mMaxInflatedBytes)
            {
                inflateEnd(&stream);
                return fail(error, "inflated section is over the size limit", size - stream.avail_in);
            }
            const size_t room = llmin(CHUNK, limits.mMaxInflatedBytes - used);
            inflated.resize(used + room);
            stream.next_out = reinterpret_cast<Bytef*>(&inflated[used]);
            stream.avail_out = (uInt)room;
            result = inflate(&stream, Z_NO_FLUSH);
            inflated.resize(used + room - stream.avail_out);
            if (result != Z_OK && result != Z_STREAM_END)
            {
                inflateEnd(&stream);
                return fail(error, "section does not inflate", size - stream.avail_in);
            }
            if (result == Z_OK && stream.avail_in == 0 && stream.avail_out != 0)
            {
                inflateEnd(&stream);
                return fail(error, "compressed section is truncated", size);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        inflateEnd(&stream);
        error.mOutOfMemory = true;
        return fail(error, "out of memory inflating section", size - stream.avail_in);
    }
    inflateEnd(&stream);

    std::istringstream input(inflated);
    if (LLSDSerialize::fromBinary(out, input, (llssize)inflated.size(), 64) <= 0)
    {
        out.clear();
        return fail(error, "inflated section is not binary LLSD", (U64)input.tellg());
    }
    return true;
}

static bool parse_mesh_havok(const LLSD& section, SKMeshHavok& out, const SKHavokLimits& limits, SKHavokError& error)
{
    out = SKMeshHavok();
    if (!section.isMap())
    {
        return fail(error, "physics_havok is not a map", 0);
    }

    read_mass_props(section["HullMassProps"], out.mHullMassProps);
    read_mass_props(section["MeshDecompMassProps"], out.mMeshDecompMassProps);

    const LLSD& welding = section["WeldingData"];
    if (section.has("WeldingData"))
    {
        if (!welding.isBinary() || welding.asBinary().size() % 2)
        {
            return fail(error, "WeldingData is malformed", 0);
        }
        const LLSD::Binary& bytes = welding.asBinary();
        // 2 bytes a triangle, of which only the first half is filled.
        const U64 triangles = bytes.size() / 2;
        if (triangles > limits.mMaxTriangles)
        {
            return fail(error, "WeldingData is over the size limit", 0);
        }
        {
            out.mWelding.mTriangles = (U32)triangles;
            const size_t codes = (size_t)(triangles / 2);
            out.mWelding.mCodes.resize(codes);
            for (size_t i = 0; i < codes; ++i)
            {
                out.mWelding.mCodes[i] = (U16)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
            }
            if (triangles % 2)
            {
                out.mWelding.mHasPartial = true;
                out.mWelding.mPartialByte = bytes[triangles - 1];
            }
        }
    }

    if (section.has("MOPP"))
    {
        const LLSD& mopp = section["MOPP"];
        if (!mopp.isMap() || !mopp["MoppData"].isBinary() || !mopp["BuildType"].isInteger())
        {
            return fail(error, "MOPP is malformed", 0);
        }
        F64 info[4];
        if (!read_real_array(mopp["MoppInfo"], 4, info))
        {
            return fail(error, "MoppInfo is malformed", 0);
        }
        const LLSD::Binary& code = mopp["MoppData"].asBinary();
        out.mMopp.mBuildType = mopp["BuildType"].asInteger();
        if (!walk_mopp(code.data(), code.size(), info, info[3], out.mMopp, limits, error))
        {
            out.mMopp = SKMoppTree();
            return false;
        }
        out.mMopp.mCode = code;
        out.mHasMopp = true;
    }
    return true;
}

static bool parse_physics_mesh(const LLSD& section, SKPhysicsMesh& out, const SKHavokLimits& limits, SKHavokError& error)
{
    out = SKPhysicsMesh();
    if (!section.isArray())
    {
        return fail(error, "physics_mesh is not an array", 0);
    }
    if (section.size() > limits.mMaxSubmeshes)
    {
        return fail(error, "physics_mesh has too many submeshes", 0);
    }
    U64 vertices = 0;
    U64 triangles = 0;
    out.mSubmeshes.resize(section.size());
    for (size_t s = 0; s < section.size(); ++s)
    {
        const LLSD& entry = section[s];
        SKPhysicsMeshSubmesh& submesh = out.mSubmeshes[s];
        if (!entry.isMap())
        {
            return fail(error, "physics_mesh entry is not a map", s);
        }
        if (entry.has("NoGeometry"))
        {
            // Every empty entry in the corpus is exactly this.
            if (!entry["NoGeometry"].isBoolean() || !entry["NoGeometry"].asBoolean())
            {
                return fail(error, "physics_mesh NoGeometry is malformed", s);
            }
            continue;
        }
        const LLSD& position = entry["Position"];
        const LLSD& indices = entry["TriangleList"];
        const LLSD& domain = entry["PositionDomain"];
        F64 lo[3];
        F64 hi[3];
        if (!position.isBinary() || !indices.isBinary() || !domain.isMap() || !read_real_array(domain["Min"], 3, lo)
            || !read_real_array(domain["Max"], 3, hi))
        {
            return fail(error, "physics_mesh submesh is malformed", s);
        }
        for (S32 i = 0; i < 3; ++i)
        {
            if (!finite_in_range(lo[i]) || !finite_in_range(hi[i]))
            {
                return fail(error, "physics_mesh domain is not finite", s);
            }
        }
        const LLSD::Binary& pos = position.asBinary();
        const LLSD::Binary& idx = indices.asBinary();
        // An entry with geometry but no triangle would shift the leaf keys' submesh numbering.
        if (pos.size() % 6 || idx.size() % 6 || idx.empty())
        {
            return fail(error, "physics_mesh buffers are malformed", s);
        }
        const U64 vertex_count = pos.size() / 6;
        const U64 triangle_count = idx.size() / 6;
        vertices += vertex_count;
        triangles += triangle_count;
        if (vertices > limits.mMaxVertices || triangles > limits.mMaxTriangles)
        {
            return fail(error, "physics_mesh is over the size limit", s);
        }
        submesh.mPositions.resize((size_t)vertex_count);
        for (U64 v = 0; v < vertex_count; ++v)
        {
            for (S32 i = 0; i < 3; ++i)
            {
                const U32 q = pos[v * 6 + i * 2] | (pos[v * 6 + i * 2 + 1] << 8);
                submesh.mPositions[v].mV[i] = (F32)(lo[i] + (hi[i] - lo[i]) * q / 65535.0);
            }
        }
        submesh.mIndices.resize((size_t)triangle_count * 3);
        for (U64 i = 0; i < triangle_count * 3; ++i)
        {
            const U16 index = (U16)(idx[i * 2] | (idx[i * 2 + 1] << 8));
            if (index >= vertex_count)
            {
                return fail(error, "physics_mesh index is out of range", s);
            }
            submesh.mIndices[i] = index;
        }
    }
    return true;
}

static bool map_mopp_leaves(const SKMoppTree& tree, const std::vector<U32>& submesh_triangles,
                            std::vector<SKMoppTriangleRef>& out, SKHavokError& error)
{
    out.clear();
    std::vector<U32> nonempty;
    std::vector<U64> first;
    U64 total = 0;
    for (size_t s = 0; s < submesh_triangles.size(); ++s)
    {
        if (submesh_triangles[s] > 0)
        {
            nonempty.push_back((U32)s);
            first.push_back(total);
        }
        total += submesh_triangles[s];
    }
    // Each triangle has exactly one leaf (corpus 4244/4244); anything else draws a partial surface.
    if (tree.mLeaves.size() != total)
    {
        return fail(error, "MOPP leaves do not cover physics_mesh", 0);
    }
    std::vector<bool> seen((size_t)total, false);
    out.reserve(tree.mLeaves.size());
    for (const SKMoppLeaf& leaf : tree.mLeaves)
    {
        const U32 shape = SKMoppTree::keySubmesh(leaf.mKey);
        const U32 triangle = SKMoppTree::keyTriangle(leaf.mKey);
        if (shape >= nonempty.size() || triangle >= submesh_triangles[nonempty[shape]])
        {
            out.clear();
            return fail(error, "MOPP leaf names no physics_mesh triangle", leaf.mPosition);
        }
        SKMoppTriangleRef ref;
        ref.mSubmesh = nonempty[shape];
        ref.mTriangle = triangle;
        ref.mGlobal = first[shape] + triangle;
        if (seen[(size_t)ref.mGlobal])
        {
            out.clear();
            return fail(error, "MOPP names a triangle twice", leaf.mPosition);
        }
        seen[(size_t)ref.mGlobal] = true;
        out.push_back(ref);
    }
    return true;
}

bool skInflateLLSD(const U8* data, size_t size, LLSD& out, const SKHavokLimits& limits, SKHavokError& error)
{
    const bool ok = guarded(error, [&]() { return inflate_llsd(data, size, out, limits, error); });
    if (!ok)
    {
        out.clear();
    }
    return ok;
}

bool skParseMeshHavok(const LLSD& section, SKMeshHavok& out, const SKHavokLimits& limits, SKHavokError& error)
{
    const bool ok = guarded(error, [&]() { return parse_mesh_havok(section, out, limits, error); });
    if (!ok)
    {
        out = SKMeshHavok();
    }
    return ok;
}

bool skParseMeshHavok(const U8* compressed, size_t size, SKMeshHavok& out, const SKHavokLimits& limits,
                      SKHavokError& error)
{
    LLSD section;
    return skInflateLLSD(compressed, size, section, limits, error) && skParseMeshHavok(section, out, limits, error);
}

bool skParsePhysicsMesh(const LLSD& section, SKPhysicsMesh& out, const SKHavokLimits& limits, SKHavokError& error)
{
    const bool ok = guarded(error, [&]() { return parse_physics_mesh(section, out, limits, error); });
    if (!ok)
    {
        out = SKPhysicsMesh();
    }
    return ok;
}

bool skMapMoppLeaves(const SKMoppTree& tree, const std::vector<U32>& submesh_triangles,
                     std::vector<SKMoppTriangleRef>& out, SKHavokError& error)
{
    const bool ok = guarded(error, [&]() { return map_mopp_leaves(tree, submesh_triangles, out, error); });
    if (!ok)
    {
        out.clear();
    }
    return ok;
}
