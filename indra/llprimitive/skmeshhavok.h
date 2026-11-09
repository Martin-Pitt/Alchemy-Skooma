/**
 * @file skmeshhavok.h
 * @brief Reader for the physics_havok section of a mesh asset: mass properties, MOPP tree, welding codes
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

#ifndef SK_SKMESHHAVOK_H
#define SK_SKMESHHAVOK_H

#include "llsd.h"
#include "v3math.h"

#include <string>
#include <vector>

// Caps on untrusted input; going over any of them is a clean failure.
struct SKHavokLimits
{
    size_t mMaxCompressedBytes = 8 * 1024 * 1024;
    size_t mMaxInflatedBytes = 32 * 1024 * 1024;
    size_t mMaxCodeBytes = 8 * 1024 * 1024;
    U32 mMaxLeaves = 1u << 21;
    U32 mMaxConstraints = 1u << 23;
    U32 mMaxFrames = 1u << 20;
    size_t mMaxWalkBytes = 64 * 1024 * 1024; // everything a MOPP walk holds at once, the code's copy included
    F64 mMaxFrameFactor = 65536.0;           // composed rescales; the corpus reaches 128
    U32 mMaxSubmeshes = 4096;
    U64 mMaxVertices = 1u << 22;
    U64 mMaxTriangles = 1u << 22;
};

struct SKHavokError
{
    std::string mWhat;
    U64 mOffset = 0; // byte offset into the input that was being read
    bool mOutOfMemory = false; // the input may be fine; worth trying again later
};

// HullMassProps or MeshDecompMassProps: inertia is row-major, about the CoM, for unit mass.
struct SKHavokMassProps
{
    bool mValid = false;
    LLVector3 mCoM;
    F32 mInertia[9] = {};
    F32 mMass = 0.f;
    F32 mVolume = 0.f;
};

// Byte frame of a MOPP path: q' = (q - offset) * factor, offset in root units.
struct SKMoppFrame
{
    F64 mOffset[3] = { 0.0, 0.0, 0.0 };
    F64 mFactor = 1.0;
};

// One k-DOP constraint lo <= proj <= hi; constraints form a tree through mParent so a path is shared.
struct SKMoppConstraint
{
    enum EKind : U8
    {
        BOUND,
        SPLIT_FIRST,  // first child of a split: proj <= hi
        SPLIT_SECOND, // second child of a split: proj >= lo
    };

    U8 mOpcode = 0;
    U8 mKind = BOUND;
    S16 mLo = 0;
    S16 mHi = 0;
    U32 mFrame = 0;
    S32 mParent = -1;
};

// n . p <= d in the mesh frame.
struct SKMoppHalfSpace
{
    F64 mNormal[3] = { 0.0, 0.0, 0.0 };
    F64 mDistance = 0.0;
};

struct SKMoppLeaf
{
    U32 mPosition = 0;   // byte of the leaf opcode in MoppData
    U32 mKey = 0;        // submesh << 20 | triangle
    U32 mDepth = 0;      // splits above it
    S32 mConstraint = -1; // last constraint on its path
    LLVector3 mMin;      // axis bounds of the path, mesh frame
    LLVector3 mMax;
};

class SKMoppTree
{
public:
    S32 mBuildType = 0;
    F64 mOrigin[3] = { 0.0, 0.0, 0.0 };
    F64 mScale = 0.0;
    std::vector<U8> mCode;
    std::vector<SKMoppFrame> mFrames;
    std::vector<SKMoppConstraint> mConstraints;
    std::vector<SKMoppLeaf> mLeaves;

    // The constraint as half-spaces in the mesh frame; returns how many of out[] it filled (1 or 2).
    U32 getHalfSpaces(const SKMoppConstraint& constraint, SKMoppHalfSpace out[2]) const;

    static U32 keySubmesh(U32 key) { return key >> 20; }
    static U32 keyTriangle(U32 key) { return key & 0xFFFFF; }
};

// WeldingData: only its first N bytes carry codes, for triangles 0 .. N/2-1, plus part of triangle N/2 when N is odd.
class SKHavokWelding
{
public:
    U32 mTriangles = 0;
    std::vector<U16> mCodes;
    bool mHasPartial = false;
    U8 mPartialByte = 0;

    // Edge 0 is (v0,v1), 1 (v1,v2), 2 (v2,v0); false where the data carries no code.
    bool getEdgeCode(U32 triangle, U32 edge, U8& code) const;
    bool getFlag(U32 triangle, bool& flag) const;
};

struct SKMeshHavok
{
    SKHavokMassProps mHullMassProps;
    SKHavokMassProps mMeshDecompMassProps;
    SKHavokWelding mWelding;
    bool mHasMopp = false;
    SKMoppTree mMopp;
};

// physics_mesh in file order; a {NoGeometry: true} entry is kept, empty.
struct SKPhysicsMeshSubmesh
{
    std::vector<LLVector3> mPositions;
    std::vector<U16> mIndices;
    U32 getTriangleCount() const { return (U32)(mIndices.size() / 3); }
};

struct SKPhysicsMesh
{
    std::vector<SKPhysicsMeshSubmesh> mSubmeshes;
    std::vector<U32> getTriangleCounts() const;
};

struct SKMoppTriangleRef
{
    U32 mSubmesh = 0;  // file-order index into physics_mesh
    U32 mTriangle = 0; // within that submesh
    U64 mGlobal = 0;   // over all submeshes
};

bool skWalkMopp(const U8* code, size_t length, const F64 origin[3], F64 scale, SKMoppTree& out,
                const SKHavokLimits& limits, SKHavokError& error);
bool skInflateLLSD(const U8* data, size_t size, LLSD& out, const SKHavokLimits& limits, SKHavokError& error);
bool skParseMeshHavok(const LLSD& section, SKMeshHavok& out, const SKHavokLimits& limits, SKHavokError& error);
bool skParseMeshHavok(const U8* compressed, size_t size, SKMeshHavok& out, const SKHavokLimits& limits,
                      SKHavokError& error);
bool skParsePhysicsMesh(const LLSD& section, SKPhysicsMesh& out, const SKHavokLimits& limits, SKHavokError& error);

// Each leaf's triangle, keys indexing the non-empty submeshes; submesh_triangles is per physics_mesh entry.
// Fails unless every triangle has exactly one leaf.
bool skMapMoppLeaves(const SKMoppTree& tree, const std::vector<U32>& submesh_triangles,
                     std::vector<SKMoppTriangleRef>& out, SKHavokError& error);

#endif // SK_SKMESHHAVOK_H
