/**
 * @file skmeshhavokshape.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skmeshhavokshape.h"

#include "llframetimer.h"
#include "llmeshrepository.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "skmeshhavok.h"

namespace
{
    constexpr F64 PENDING_TIMEOUT = 120.0;
    constexpr F64 TRANSIENT_RETRY = 30.0;
    constexpr F64 ABSENT_RETRY = 600.0;
}

SKPhysicsHavokResult skBuildPhysicsHavokShape(const U8* mesh, size_t mesh_size, const U8* havok, size_t havok_size,
                                              std::shared_ptr<const SKPhysicsHavokShape>& out, std::string& error)
{
    out.reset();
    const SKHavokLimits limits;
    SKHavokError failure;
    SKMeshHavok block;
    if (!skParseMeshHavok(havok, havok_size, block, limits, failure))
    {
        error = failure.mWhat + " at byte " + std::to_string(failure.mOffset);
        return SKPhysicsHavokResult::ABSENT;
    }

    auto shape = std::make_shared<SKPhysicsHavokShape>();
    if (block.mHasMopp)
    {
        LLSD mesh_section;
        SKPhysicsMesh physics_mesh;
        std::vector<SKMoppTriangleRef> refs;
        if (!mesh || !skInflateLLSD(mesh, mesh_size, mesh_section, limits, failure)
            || !skParsePhysicsMesh(mesh_section, physics_mesh, limits, failure)
            || !skMapMoppLeaves(block.mMopp, physics_mesh.getTriangleCounts(), refs, failure))
        {
            error = mesh ? failure.mWhat : "MOPP without physics_mesh";
            return SKPhysicsHavokResult::ABSENT;
        }

        U64 total = 0;
        for (const SKPhysicsMeshSubmesh& submesh : physics_mesh.mSubmeshes)
        {
            total += submesh.getTriangleCount();
        }
        std::vector<bool> drawn((size_t)total, false);
        shape->mTriangles.reserve(llmin(refs.size(), (size_t)total) * 3);
        for (const SKMoppTriangleRef& ref : refs)
        {
            if (drawn[(size_t)ref.mGlobal])
            {
                continue;
            }
            drawn[(size_t)ref.mGlobal] = true;
            const SKPhysicsMeshSubmesh& submesh = physics_mesh.mSubmeshes[ref.mSubmesh];
            for (U32 v = 0; v < 3; ++v)
            {
                shape->mTriangles.push_back(submesh.mPositions[submesh.mIndices[ref.mTriangle * 3 + v]]);
            }
        }
        shape->mHasMopp = true;
    }
    out = shape;
    return SKPhysicsHavokResult::OK;
}

bool skRenderPhysicsHavok(const LLUUID& mesh_id, const LLColor4& color)
{
    const std::shared_ptr<const SKPhysicsHavokShape> shape = gMeshRepo.getPhysicsHavok(mesh_id);
    if (!shape || !shape->mHasMopp || shape->mTriangles.empty())
    {
        return false;
    }
    gGL.getTextureSlot(0)->unbind();
    gGL.diffuseColor4fv(color.mV);
    LLVertexBuffer::drawArrays(LLRender::TRIANGLES, shape->mTriangles);
    return true;
}

std::shared_ptr<const SKPhysicsHavokShape> SKPhysicsHavokCache::get(const LLUUID& mesh_id)
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    auto found = mEntries.find(mesh_id);
    if (found == mEntries.end())
    {
        Entry& entry = mEntries[mesh_id];
        mUse.push_front(mesh_id);
        entry.mUse = mUse.begin();
        entry.mRetryAt = now + PENDING_TIMEOUT;
        mRequests.push_back(mesh_id);
        evict();
        return nullptr;
    }

    Entry& entry = found->second;
    touch(entry);
    if (!entry.mShape && now >= entry.mRetryAt)
    {
        entry.mRetryAt = now + PENDING_TIMEOUT;
        mRequests.push_back(mesh_id);
    }
    return entry.mShape;
}

void SKPhysicsHavokCache::takeRequests(std::vector<LLUUID>& out)
{
    out.clear();
    out.swap(mRequests);
}

void SKPhysicsHavokCache::deliver(const SKPhysicsHavokDelivery& delivery)
{
    auto found = mEntries.find(delivery.mMeshID);
    if (found == mEntries.end())
    {
        // Evicted while in flight; nobody has asked since.
        return;
    }
    Entry& entry = found->second;
    mBytes -= entry.mBytes;
    entry.mShape = delivery.mResult == SKPhysicsHavokResult::OK ? delivery.mShape : nullptr;
    entry.mBytes = entry.mShape ? entry.mShape->getMemoryBytes() : 0;
    mBytes += entry.mBytes;
    const F64 now = LLFrameTimer::getTotalSeconds();
    entry.mRetryAt = now + (delivery.mResult == SKPhysicsHavokResult::TRANSIENT ? TRANSIENT_RETRY : ABSENT_RETRY);
    evict();
}

void SKPhysicsHavokCache::clear()
{
    mEntries.clear();
    mUse.clear();
    mRequests.clear();
    mBytes = 0;
}

void SKPhysicsHavokCache::touch(Entry& entry)
{
    mUse.splice(mUse.begin(), mUse, entry.mUse);
}

void SKPhysicsHavokCache::evict()
{
    while (mUse.size() > 1 && (mEntries.size() > MAX_ENTRIES || mBytes > MAX_BYTES))
    {
        auto victim = mEntries.find(mUse.back());
        mBytes -= victim->second.mBytes;
        mEntries.erase(victim);
        mUse.pop_back();
    }
}
