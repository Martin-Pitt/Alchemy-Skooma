/**
 * @file llvolume_test.cpp
 * @brief Volume face generation: a sculpt's pinched degenerate triangles.
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

#include "../test/lltut.h"

#include "llvolume.h"

#include <vector>

namespace
{
    // A 64 x 64 RGB sculpt whose middle rows are all the same colour. The
    // mesh rows sampled from them land in the same place, which is how a
    // sculpt gets its cut-out pieces: the surface is pinched into threads.
    std::vector<U8> pinchedSculpt()
    {
        const S32 size = 64;
        std::vector<U8> data(size * size * 3);
        for (S32 y = 0; y < size; ++y)
        {
            const S32 row = (y >= 16 && y <= 48) ? 16 : y;
            for (S32 x = 0; x < size; ++x)
            {
                U8* p = &data[(y * size + x) * 3];
                p[0] = static_cast<U8>(x * 255 / (size - 1));
                p[1] = static_cast<U8>(row * 255 / (size - 1));
                p[2] = 128;
            }
        }
        return data;
    }

    bool hasDegenerate(const LLVolumeFace& face)
    {
        for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
        {
            const LLVector4a& a = face.mPositions[face.mIndices[i]];
            const LLVector4a& b = face.mPositions[face.mIndices[i + 1]];
            const LLVector4a& c = face.mPositions[face.mIndices[i + 2]];
            if (a.equals3(b) || b.equals3(c) || c.equals3(a))
            {
                return true;
            }
        }
        return false;
    }
}

namespace tut
{
    struct llvolume_data {};
    typedef test_group<llvolume_data> llvolume_group;
    typedef llvolume_group::object llvolume_object;
    tut::llvolume_group g("llvolume");

    // A line is not a surface, and every consumer of the index buffer -
    // rasterisers, octrees, tangent builders - treats a degenerate triangle
    // as one. The pinch that gives a sculpt its cut-out shape leaves a run of
    // them, and the generated face carries none, while the surface remains.
    template<> template<>
    void llvolume_object::test<1>()
    {
        LLVolumeParams params;
        params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE);
        params.setSculptID(LLUUID::generateNewID(), LL_SCULPT_TYPE_PLANE);
        LLPointer<LLVolume> volume = new LLVolume(params, 1.f);

        std::vector<U8> data = pinchedSculpt();
        volume->sculpt(64, 64, 3, data.data(), 0, false);

        ensure("faces generated", volume->getNumVolumeFaces() > 0);
        S32 indices = 0;
        bool dropped = false;
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& face = volume->getVolumeFace(f);
            ensure("no degenerate triangle", !hasDegenerate(face));
            // The pinch leaves fewer triangles than the full grid would.
            dropped = dropped || face.mNumIndices < (face.mNumS - 1) * (face.mNumT - 1) * 6;
            indices += face.mNumIndices;
        }
        ensure("the pinch was dropped", dropped);
        ensure("triangles remain", indices > 0);
    }
}
