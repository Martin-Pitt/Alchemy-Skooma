/**
 * @file sknavoverlay.h
 * @brief Gathered debug geometry for the navmesh overlays, drawn in one batch per primitive type.
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

#ifndef SK_NAVOVERLAY_H
#define SK_NAVOVERLAY_H

#include "v3math.h"
#include "v4color.h"

#include <vector>

/// Debug geometry in agent coordinates, gathered before anything is drawn.
///
/// The overlays used to set a colour and draw a line, over and over. That was
/// wrong twice: `gDebugProgram` takes its colour as a uniform, and `LLRender`
/// only flushes a line batch once its buffer fills, so a run of lines came out
/// in whichever colour happened to be current at the flush — which moved from
/// frame to frame, and the overlay flickered. Colour here is per vertex under
/// `gUIProgram`, and the whole overlay is two draws: fills, then lines over
/// them. Gathering also lets the x-ray pass re-draw the same geometry without
/// walking the navmesh a second time.
class SKNavOverlayGeom
{
public:
    void line(const LLVector3& a, const LLVector3& b, const LLColor4& color);
    /// An edge of the walkable surface itself, drawn wider than the rest so
    /// the shape of the mesh reads before its annotations do.
    void edge(const LLVector3& a, const LLVector3& b, const LLColor4& color);
    void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color);
    /// The twelve edges of an axis-aligned box.
    void box(const LLVector3& min, const LLVector3& max, const LLColor4& color);

    bool empty() const { return mTriVerts.empty() && mLineVerts.empty() && mEdgeVerts.empty(); }
    void clear();

    /// Draw what was gathered: fills, then edges, then the thinner lines over
    /// them. `shade` darkens the colours (the x-ray pass, seen through
    /// geometry) and `alpha` scales them; `width` is the edge width in pixels,
    /// and everything else is drawn a pixel thinner.
    void draw(F32 width, F32 shade = 1.f, F32 alpha = 1.f) const;

    /// Bind `gUIProgram` against a white texture with alpha blending, which is
    /// what `draw()` expects. Colour is per vertex, so one bind covers the lot.
    static void bindShader();
    static void unbindShader();

private:
    std::vector<LLVector3> mTriVerts;
    std::vector<LLColor4>  mTriColors;
    std::vector<LLVector3> mLineVerts;
    std::vector<LLColor4>  mLineColors;
    std::vector<LLVector3> mEdgeVerts;
    std::vector<LLColor4>  mEdgeColors;
};

#endif // SK_NAVOVERLAY_H
