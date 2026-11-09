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
    constexpr F64 TRANSIENT_RETRY = 30.0;
    constexpr F64 ABSENT_RETRY = 600.0;
    constexpr F64 MESH_RETRY = 5.0;
    constexpr U32 MAX_MESH_WAITS = 3;

    SKPhysicsHavokResult build_shape(const U8* mesh, size_t mesh_size, const U8* havok, size_t havok_size,
                                     std::shared_ptr<const SKPhysicsHavokShape>& out, bool& havok_valid,
                                     std::string& error)
    {
        const SKHavokLimits limits;
        SKHavokError failure;
        SKMeshHavok block;
        if (!skParseMeshHavok(havok, havok_size, block, limits, failure))
        {
            error = failure.mWhat + " at byte " + std::to_string(failure.mOffset);
            return failure.mOutOfMemory ? SKPhysicsHavokResult::TRANSIENT : SKPhysicsHavokResult::ABSENT;
        }
        havok_valid = true;

        auto shape = std::make_shared<SKPhysicsHavokShape>();
        if (block.mHasMopp)
        {
            if (!mesh)
            {
                return SKPhysicsHavokResult::NEED_MESH;
            }
            LLSD mesh_section;
            SKPhysicsMesh physics_mesh;
            std::vector<SKMoppTriangleRef> refs;
            if (!skInflateLLSD(mesh, mesh_size, mesh_section, limits, failure)
                || !skParsePhysicsMesh(mesh_section, physics_mesh, limits, failure)
                || !skMapMoppLeaves(block.mMopp, physics_mesh.getTriangleCounts(), refs, failure))
            {
                error = failure.mWhat;
                return failure.mOutOfMemory ? SKPhysicsHavokResult::TRANSIENT : SKPhysicsHavokResult::ABSENT;
            }

            // Mapping guarantees one leaf per triangle, so every triangle is drawn once.
            shape->mTriangles.reserve(refs.size() * 3);
            for (const SKMoppTriangleRef& ref : refs)
            {
                const SKPhysicsMeshSubmesh& submesh = physics_mesh.mSubmeshes[ref.mSubmesh];
                for (U32 v = 0; v < 3; ++v)
                {
                    shape->mTriangles.push_back(submesh.mPositions[submesh.mIndices[ref.mTriangle * 3 + v]]);
                }
            }

            // The simulator fits the physics mesh's bounds to the object's box, each axis on its own: the same block
            // comes back for an upload at any scale, and its MOPP origin is the physics mesh's lowest corner.
            LLVector3 lo(F32_MAX, F32_MAX, F32_MAX);
            LLVector3 hi(-F32_MAX, -F32_MAX, -F32_MAX);
            for (const SKPhysicsMeshSubmesh& submesh : physics_mesh.mSubmeshes)
            {
                for (const LLVector3& p : submesh.mPositions)
                {
                    update_min_max(lo, hi, p);
                }
            }
            for (LLVector3& p : shape->mTriangles)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    const F32 range = hi.mV[k] - lo.mV[k];
                    p.mV[k] = range > 1e-6f ? (p.mV[k] - lo.mV[k]) / range - 0.5f : 0.f;
                }
            }
            shape->mHasMopp = true;
        }
        out = shape;
        return SKPhysicsHavokResult::OK;
    }
}

SKPhysicsHavokResult skBuildPhysicsHavokShape(const U8* mesh, size_t mesh_size, const U8* havok, size_t havok_size,
                                              std::shared_ptr<const SKPhysicsHavokShape>& out, bool& havok_valid,
                                              std::string& error)
{
    out.reset();
    havok_valid = false;
    try
    {
        return build_shape(mesh, mesh_size, havok, havok_size, out, havok_valid, error);
    }
    catch (const std::bad_alloc&)
    {
        out.reset();
        error = "out of memory";
        return SKPhysicsHavokResult::TRANSIENT;
    }
    catch (...)
    {
        out.reset();
        error = "unexpected exception";
        return SKPhysicsHavokResult::ABSENT;
    }
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
    auto found = mEntries.find(mesh_id);
    if (found == mEntries.end())
    {
        // Back-pressure: with this many in flight, ask again on a later frame.
        if (mPending < MAX_PENDING)
        {
            Entry& entry = mEntries[mesh_id];
            entry.mLastFrame = LLFrameTimer::getFrameCount();
            request(mesh_id, entry);
        }
        return nullptr;
    }

    Entry& entry = found->second;
    entry.mLastFrame = LLFrameTimer::getFrameCount();
    switch (entry.mState)
    {
    case EState::PENDING:
        return nullptr;
    case EState::READY:
        mUse.splice(mUse.begin(), mUse, entry.mUse);
        return entry.mShape;
    case EState::FAILED:
        mUse.splice(mUse.begin(), mUse, entry.mUse);
        if (LLFrameTimer::getTotalSeconds() >= entry.mRetryAt && mPending < MAX_PENDING)
        {
            mUse.erase(entry.mUse);
            request(mesh_id, entry);
        }
        return nullptr;
    }
    return nullptr;
}

void SKPhysicsHavokCache::request(const LLUUID& mesh_id, Entry& entry)
{
    entry.mState = EState::PENDING;
    entry.mShape.reset();
    ++mPending;
    mRequests.push_back(mesh_id);
}

void SKPhysicsHavokCache::takeRequests(std::vector<LLUUID>& out)
{
    out.clear();
    out.swap(mRequests);
}

void SKPhysicsHavokCache::takeMeshRequests(std::vector<LLUUID>& out)
{
    out.clear();
    out.swap(mMeshRequests);
}

void SKPhysicsHavokCache::deliver(const SKPhysicsHavokDelivery& delivery)
{
    auto found = mEntries.find(delivery.mMeshID);
    if (found == mEntries.end() || found->second.mState != EState::PENDING)
    {
        return;
    }
    Entry& entry = found->second;
    --mPending;

    const F64 now = LLFrameTimer::getTotalSeconds();
    if (delivery.mResult == SKPhysicsHavokResult::OK && delivery.mShape
        && delivery.mShape->getMemoryBytes() <= MAX_BYTES)
    {
        entry.mState = EState::READY;
        entry.mShape = delivery.mShape;
        entry.mBytes = entry.mShape->getMemoryBytes();
        entry.mMeshWaits = 0;
    }
    else
    {
        entry.mState = EState::FAILED;
        entry.mShape.reset();
        entry.mBytes = 0;
        F64 delay = ABSENT_RETRY;
        if (delivery.mResult == SKPhysicsHavokResult::TRANSIENT)
        {
            delay = TRANSIENT_RETRY;
        }
        else if (delivery.mResult == SKPhysicsHavokResult::NEED_MESH && entry.mMeshWaits < MAX_MESH_WAITS)
        {
            ++entry.mMeshWaits;
            delay = MESH_RETRY;
            mMeshRequests.push_back(delivery.mMeshID);
        }
        else
        {
            entry.mMeshWaits = 0;
        }
        entry.mRetryAt = now + delay;
    }
    finish(delivery.mMeshID, entry);
}

void SKPhysicsHavokCache::finish(const LLUUID& mesh_id, Entry& entry)
{
    mUse.push_front(mesh_id);
    entry.mUse = mUse.begin();
    mBytes += entry.mBytes;
    evict();
}

void SKPhysicsHavokCache::clear()
{
    mEntries.clear();
    mUse.clear();
    mRequests.clear();
    mMeshRequests.clear();
    mPending = 0;
    mBytes = 0;
}

void SKPhysicsHavokCache::evict()
{
    // Whatever was drawn this frame or the last stays, however many there are: evicting a mesh
    // still in view would only fetch it again.
    const U32 recent = LLFrameTimer::getFrameCount() - 1;
    while (mUse.size() > 1 && (mUse.size() > MAX_ENTRIES || mBytes > MAX_BYTES))
    {
        auto victim = mEntries.find(mUse.back());
        if ((S32)(victim->second.mLastFrame - recent) >= 0)
        {
            break;
        }
        mBytes -= victim->second.mBytes;
        mEntries.erase(victim);
        mUse.pop_back();
    }
}
