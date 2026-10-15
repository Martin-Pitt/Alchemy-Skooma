/**
 * @file skmeshhavokshape.h
 * @brief The simulator's collision triangles of a mesh, from its physics_havok section, and their cache
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


#ifndef SK_SKMESHHAVOKSHAPE_H
#define SK_SKMESHHAVOKSHAPE_H

#include "lluuid.h"
#include "v3math.h"

#include <boost/unordered_map.hpp>

#include <list>
#include <memory>
#include <string>
#include <vector>

class LLColor4;

// What the physics display draws for one mesh.
class SKPhysicsHavokShape
{
public:
    bool mHasMopp = false;
    // Three positions per triangle with a MOPP leaf, in the mesh frame.
    std::vector<LLVector3> mTriangles;

    size_t getMemoryBytes() const { return sizeof(*this) + mTriangles.capacity() * sizeof(LLVector3); }
};

enum class SKPhysicsHavokResult
{
    OK,
    ABSENT,    // no section, 404 or unreadable: will not change on retry soon
    TRANSIENT, // network trouble or out of memory: worth asking again shortly
    NEED_MESH, // the block has a MOPP and physics_mesh is not cached yet
};

struct SKPhysicsHavokDelivery
{
    LLUUID mMeshID;
    std::shared_ptr<const SKPhysicsHavokShape> mShape;
    SKPhysicsHavokResult mResult = SKPhysicsHavokResult::ABSENT;
};

// Builds the shape from the compressed physics_havok section and, for a MOPP, the compressed physics_mesh
// (null when it is not to hand). havok_valid says whether the section itself parsed. Never throws.
SKPhysicsHavokResult skBuildPhysicsHavokShape(const U8* mesh, size_t mesh_size, const U8* havok, size_t havok_size,
                                              std::shared_ptr<const SKPhysicsHavokShape>& out, bool& havok_valid,
                                              std::string& error);

// Main thread: draws the simulator's triangles of a mesh with a MOPP under the current matrix.
// False while they are absent or the mesh has no MOPP, for the caller to draw as before.
bool skRenderPhysicsHavok(const LLUUID& mesh_id, const LLColor4& color);

// Main thread only, like the rest of LLMeshRepository's maps: shapes by mesh.
// A mesh being fetched is never evicted and never asked for twice; only finished entries count
// against the bound and are dropped least recently used first, never one asked for in the last frame.
// Failures are asked again after a delay.
class SKPhysicsHavokCache
{
public:
    // Null until the shape has arrived; the first ask queues a request.
    std::shared_ptr<const SKPhysicsHavokShape> get(const LLUUID& mesh_id);
    void takeRequests(std::vector<LLUUID>& out);
    // Meshes whose physics_mesh the physics-shape fetch should bring into the cache.
    void takeMeshRequests(std::vector<LLUUID>& out);
    void deliver(const SKPhysicsHavokDelivery& delivery);
    void clear();

    static constexpr size_t MAX_ENTRIES = 2048;
    static constexpr size_t MAX_PENDING = 1024;
    static constexpr size_t MAX_BYTES = 64 * 1024 * 1024;

private:
    enum class EState
    {
        PENDING,
        READY,
        FAILED,
    };

    struct Entry
    {
        EState mState = EState::PENDING;
        std::shared_ptr<const SKPhysicsHavokShape> mShape;
        F64 mRetryAt = 0.0;
        U32 mMeshWaits = 0;
        U32 mLastFrame = 0;
        size_t mBytes = 0;
        std::list<LLUUID>::iterator mUse; // valid unless pending
    };

    void request(const LLUUID& mesh_id, Entry& entry);
    void finish(const LLUUID& mesh_id, Entry& entry);
    void evict();

    boost::unordered_map<LLUUID, Entry> mEntries;
    std::list<LLUUID> mUse; // finished entries, most recent first
    std::vector<LLUUID> mRequests;
    std::vector<LLUUID> mMeshRequests;
    size_t mPending = 0;
    size_t mBytes = 0;
};

#endif // SK_SKMESHHAVOKSHAPE_H
