/**
 * @file skroadbenchgeometry.h
 * @brief Spike: a road part's geometry rebuilt from what the road survey recorded of it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef SK_ROADBENCHGEOMETRY_H
#define SK_ROADBENCHGEOMETRY_H

#include "llsd.h"
#include "skcensustypes.h"
#include "v2math.h"
#include "v3math.h"

#include <string>
#include <vector>

// The survey keeps references, not triangles: a mesh part's asset, frame and physics shape type, a prim's volume
// params and frame. This rebuilds both what is drawn (each face with its UVs, at the top LOD) and what the census
// would have taken as its physics shape, in region coordinates, from the viewer's mesh cache and LLVolume.
namespace skroadbench
{
    struct RebuiltFace
    {
        std::vector<LLVector3> positions;   // region-local
        std::vector<LLVector2> uvs;
        std::vector<U32> indices;
    };

    struct Rebuilt
    {
        std::vector<RebuiltFace> faces;     // by face index, as the part's TEs
        std::vector<LLVector3> physics;     // three a triangle, region-local, as the census's record holds them
        std::vector<S32> convex_tris;       // physics as separate hulls: the triangles of each, in order
        skworld::Provenance provenance = skworld::Provenance::EXACT;
        bool solid = true;
        std::string why;                    // what is missing, where something is
    };

    // The viewer's asset cache: SK_ROADBENCH_CACHE, else %LOCALAPPDATA%\AlchemyNext\cache.
    std::string defaultCacheDir();

    // From a survey "part" record with "pos", "rot", "scale" and "mesh" or "vp". False where neither faces nor a
    // physics shape could be made (why says so); a mesh missing from the cache is the usual reason.
    bool rebuildPart(const LLSD& record, const std::string& cache_dir, Rebuilt& out);

    // A convex hull's triangles, outward wound; empty for fewer than four points off one plane.
    void convexHull(const std::vector<LLVector3>& points, std::vector<LLVector3>& out);
}

#endif // SK_ROADBENCHGEOMETRY_H
