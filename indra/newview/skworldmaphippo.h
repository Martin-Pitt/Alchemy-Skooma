/**
 * @file skworldmaphippo.h
 * @brief What carries the 3D world map's disc: a mesh asset hung beneath it, seen only past the rim.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_WORLDMAPHIPPO_H
#define SK_WORLDMAPHIPPO_H

#include "llpointer.h"
#include "lluuid.h"
#include "skworldmapmath.h"

#include <memory>

class LLFetchedGLTFMaterial;
class LLVertexBuffer;
class LLViewerFetchedTexture;

/// The mesh in SKWorldMapHippoMesh, its paw plane against the disc's underside and its length a little past
/// the disc's diameter, lit by ambient light alone. Nothing is fetched or drawn while the disc hides it.
class SKWorldMapHippo
{
public:
    ~SKWorldMapHippo();

    /// Draws it under the current projection, vertices relative to `eye`, depth-tested against what the caller
    /// has already written: the disc's depth hides all of it the rim covers.
    void draw(const LLVector3d& eye, const skworldmap::WorldDisc& disc);

    void release();

private:
    enum class State { Idle, Fetching, Ready, Failed };

    void fetch(const LLUUID& mesh_id);
    bool build(const std::string& asset);

    State mState = State::Idle;
    LLUUID mMeshId;
    F64 mRetryAt = 0.0;
    LLPointer<LLVertexBuffer> mBuffer;
    U32 mIndexCount = 0;
    LLPointer<LLViewerFetchedTexture> mTexture;
    LLPointer<LLFetchedGLTFMaterial> mMaterial;  ///< its base colour, once loaded, takes the texture's place
    std::shared_ptr<SKWorldMapHippo*> mSelf;    ///< the fetch coroutine's way back, cleared when this goes
};

#endif // SK_WORLDMAPHIPPO_H
