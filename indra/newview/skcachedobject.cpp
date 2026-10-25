/**
 * @file skcachedobject.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skcachedobject.h"

#include "lldatapacker.h"
#include "llpartdata.h"
#include "llprimitive.h"
#include "llvocache.h"
#include "llvolumemessage.h"

#include <vector>

namespace
{
    // As LLViewerObject reads an extra parameter block.
    constexpr S32 MAX_PARAM_SIZE = 1024;
}

bool skDecodeCachedObject(const LLVOCacheEntry& entry, SKCachedObject& out)
{
    const LLDataPackerBinaryBuffer* cached = entry.getDP();
    if (!cached || !cached->getBuffer())
    {
        return false;
    }
    // The walk of LLViewerObject::processUpdateMessage (OUT_FULL_CACHED) and
    // LLVOVolume::processUpdateMessage, field for field, through a packer of
    // our own over the same bytes.
    LLDataPackerBinaryBuffer dp(const_cast<U8*>(cached->getBuffer()), cached->getBufferSize());
    bool ok = dp.unpackUUID(out.mId, "ID");
    ok = ok && dp.unpackU32(out.mLocalId, "LocalID");
    ok = ok && dp.unpackU8(out.mPCode, "PCode");
    if (!ok || out.mPCode != LL_PCODE_VOLUME)
    {
        return false;
    }
    U8 state = 0;
    ok = dp.unpackU8(state, "State");
    ok = ok && dp.unpackU32(out.mCRC, "CRC");
    ok = ok && dp.unpackU8(out.mMaterial, "Material");
    U8 click_action = 0;
    ok = ok && dp.unpackU8(click_action, "ClickAction");
    ok = ok && dp.unpackVector3(out.mScale, "Scale");
    ok = ok && dp.unpackVector3(out.mPos, "Pos");
    LLVector3 packed_rot;
    ok = ok && dp.unpackVector3(packed_rot, "Rot");
    out.mRot.unpackFromVector3(packed_rot);
    out.mRot.normQuat();

    U32 value = 0;
    ok = ok && dp.unpackU32(value, "SpecialCode");
    LLUUID owner;
    ok = ok && dp.unpackUUID(owner, "Owner");
    if (ok && (value & 0x80))
    {
        LLVector3 omega;
        ok = dp.unpackVector3(omega, "Omega");
    }
    out.mParentId = 0;
    if (ok && (value & 0x20))
    {
        ok = dp.unpackU32(out.mParentId, "ParentID");
    }
    if (ok && (value & 0x2))
    {
        U8 tree = 0;
        ok = dp.unpackU8(tree, "TreeData");
    }
    else if (ok && (value & 0x1))
    {
        U32 size = 0;
        ok = dp.unpackU32(size, "ScratchPadSize");
        if (ok)
        {
            std::vector<U8> scratch(size);
            S32 read = 0;
            ok = dp.unpackBinaryData(scratch.data(), (S32)size, read, "PartData");
        }
    }
    if (ok && (value & 0x4))
    {
        std::string text;
        U8 color[4];
        ok = dp.unpackString(text, "Text") && dp.unpackBinaryDataFixed(color, 4, "Color");
    }
    if (ok && (value & 0x200))
    {
        std::string media_url;
        ok = dp.unpackString(media_url, "MediaURL");
    }
    if (ok && (value & 0x8))
    {
        LLPartSysData particles;
        ok = particles.unpackLegacy(dp);
    }

    LLUUID sculpt_id;
    U8 sculpt_type = 0;
    U8 num_parameters = 0;
    ok = ok && dp.unpackU8(num_parameters, "num_params");
    U8 param_block[MAX_PARAM_SIZE];
    for (U8 param = 0; ok && param < num_parameters; ++param)
    {
        U16 param_type = 0;
        S32 param_size = 0;
        ok = dp.unpackU16(param_type, "param_type")
            && dp.unpackBinaryData(param_block, MAX_PARAM_SIZE, param_size, "param_data");
        if (ok && param_type == LLNetworkData::PARAMS_SCULPT)
        {
            LLSculptParams sculpt;
            LLDataPackerBinaryBuffer dp2(param_block, param_size);
            sculpt.unpack(dp2);
            out.mSculpted = true;
            sculpt_id = sculpt.getSculptTexture();
            sculpt_type = sculpt.getSculptType();
        }
    }
    if (ok && (value & 0x10))
    {
        LLUUID sound;
        F32 gain = 0.f, radius = 0.f;
        U8 flags = 0;
        ok = dp.unpackUUID(sound, "SoundUUID") && dp.unpackF32(gain, "SoundGain") && dp.unpackU8(flags, "SoundFlags")
            && dp.unpackF32(radius, "SoundRadius");
    }
    if (ok && (value & 0x100))
    {
        std::string name_values;
        ok = dp.unpackString(name_values, "NV");
    }

    // LLVOVolume's part.
    ok = ok && LLVolumeMessage::unpackVolumeParams(&out.mParams, dp);
    if (!ok)
    {
        return false;
    }
    out.mParams.setSculptID(sculpt_id, sculpt_type);
    out.mMesh = out.mSculpted && (sculpt_type & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH;
    // setVolume keeps the flexible parameter in use exactly while the path is flexible.
    out.mFlexible = out.mParams.getPathParams().getCurveType() == LL_PCODE_PATH_FLEXIBLE;
    return true;
}
