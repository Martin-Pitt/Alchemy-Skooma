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
    TRANSIENT, // network trouble: worth asking again shortly
};

struct SKPhysicsHavokDelivery
{
    LLUUID mMeshID;
    std::shared_ptr<const SKPhysicsHavokShape> mShape;
    SKPhysicsHavokResult mResult = SKPhysicsHavokResult::ABSENT;
};

// Builds the shape from the compressed physics_mesh and physics_havok sections; physics_mesh may be absent.
SKPhysicsHavokResult skBuildPhysicsHavokShape(const U8* mesh, size_t mesh_size, const U8* havok, size_t havok_size,
                                              std::shared_ptr<const SKPhysicsHavokShape>& out, std::string& error);

// Main thread only: shapes by mesh, least recently used dropped first, failures retried after a delay.
class SKPhysicsHavokCache
{
public:
    // Null until the shape has arrived; the first ask queues a request.
    std::shared_ptr<const SKPhysicsHavokShape> get(const LLUUID& mesh_id);
    void takeRequests(std::vector<LLUUID>& out);
    void deliver(const SKPhysicsHavokDelivery& delivery);
    void clear();

    static constexpr size_t MAX_ENTRIES = 2048;
    static constexpr size_t MAX_BYTES = 64 * 1024 * 1024;

private:
    struct Entry
    {
        std::shared_ptr<const SKPhysicsHavokShape> mShape;
        F64 mRetryAt = 0.0; // for an entry with no shape: when to ask again
        size_t mBytes = 0;
        std::list<LLUUID>::iterator mUse;
    };

    void touch(Entry& entry);
    void evict();

    boost::unordered_map<LLUUID, Entry> mEntries;
    std::list<LLUUID> mUse; // most recent first
    std::vector<LLUUID> mRequests;
    size_t mBytes = 0;
};

#endif // SK_SKMESHHAVOKSHAPE_H
