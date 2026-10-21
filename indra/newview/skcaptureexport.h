/**
 * @file skcaptureexport.h
 * @brief Writes a linkset, an avatar's attachments or an avatar as triangles with per-face visibility, for outline work offline.
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

#ifndef SK_CAPTUREEXPORT_H
#define SK_CAPTUREEXPORT_H

#include "stdtypes.h"

#include <string>

class LLTextureEntry;
class LLViewerObject;

enum class SKCaptureKind
{
    /// A linkset and everyone seated on it with what they wear, in the root's frame.
    VEHICLE,
    /// An avatar's worn linksets, each in its own root's frame.
    ATTACHMENTS,
    /// An avatar's worn linksets in the avatar's body frame.
    AVATAR,
};

/// Writes sknav-capture-<kind>-<id>.json to the logs folder and returns a
/// line for the console with the counts and the path, or why nothing was written.
std::string skCaptureExport(SKCaptureKind kind);

namespace skcapture
{
    /// How the renderer will blend a face: pbr-blend, pbr-mask, pbr-opaque,
    /// none, blend, mask, emissive or default.
    const char* faceAlphaMode(const LLTextureEntry* te);

    /// The PBR base colour's alpha, which multiplies the tint on a PBR face; 1 without one.
    F32 facePbrAlpha(const LLTextureEntry* te);

    /// SL's link number: the root is 1 and its non-avatar children count from 2.
    S32 linkNumber(const LLViewerObject* object);

    /// The share of a prim's faces that can be seen through.
    F32 seeThroughShare(const LLViewerObject* object);
}

#endif // SK_CAPTUREEXPORT_H
