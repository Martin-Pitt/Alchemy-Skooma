/**
 * @file skobjectfaces.h
 * @brief What a viewer object's faces show: how each blends, and how much of it can be seen through.
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


#ifndef SK_OBJECTFACES_H
#define SK_OBJECTFACES_H

#include "stdtypes.h"

class LLTextureEntry;
class LLViewerObject;
class LLViewerTexture;

/// How the renderer will blend a face: pbr-blend, pbr-mask, pbr-opaque,
/// none, blend, mask, emissive or default.
const char* skFaceAlphaMode(const LLTextureEntry* te);

/// The PBR base colour's alpha, which multiplies the tint on a PBR face; 1 without one.
F32 skFacePbrAlpha(const LLTextureEntry* te);

/// The image a face draws: a PBR face its material's base colour, any other its own texture.
const LLViewerTexture* skFaceImage(const LLViewerObject* object, U8 face);

/// Whether one face can be seen through: set to blend whatever its tint, faint under blend, or the stock
/// transparent texture.
bool skFaceSeeThrough(const LLViewerObject* object, U8 face);

/// SL's link number: the root is 1 and its non-avatar children count from 2.
S32 skLinkNumber(const LLViewerObject* object);

/// The share of a prim's faces that can be seen through: a pane of glass is
/// solid to the sim and told apart from a door only here.
F32 skSeeThroughShare(const LLViewerObject* object);

#endif // SK_OBJECTFACES_H
