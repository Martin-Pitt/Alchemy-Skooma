/**
 * @file skcachedobject.h
 * @brief What a region's object cache holds for a volume, read without making an object of it.
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

#ifndef SK_CACHEDOBJECT_H
#define SK_CACHEDOBJECT_H

#include "llquaternion.h"
#include "lluuid.h"
#include "llvolume.h"

class LLVOCacheEntry;

/// A cached ObjectUpdateCompressed body, taken apart as LLViewerObject and
/// LLVOVolume take it apart when they make an object of it: the same values
/// a revived object holds, so whatever is worked from them comes out alike.
struct SKCachedObject
{
    LLUUID mId;
    U32 mLocalId = 0;
    U32 mCRC = 0;
    U32 mParentId = 0;          ///< local id of the linkset root, 0 for a root
    U8 mPCode = 0;
    U8 mMaterial = 0;
    LLVector3 mScale;
    LLVector3 mPos;             ///< region frame for a root, the root's frame for a child
    LLQuaternion mRot;          ///< normalised as an update normalises it
    LLVolumeParams mParams;     ///< with the sculpt or mesh id from the sculpt parameter
    bool mSculpted = false;
    bool mMesh = false;
    bool mFlexible = false;
};

/// False when the entry holds no body, or one that is not a volume or does not read to its end of the volume
/// parameters. Reads through a packer of its own, so the entry's packer is left where its other readers expect it.
bool skDecodeCachedObject(const LLVOCacheEntry& entry, SKCachedObject& out);

#endif // SK_CACHEDOBJECT_H
