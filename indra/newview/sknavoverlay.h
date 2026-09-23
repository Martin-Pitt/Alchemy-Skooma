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
    /// the shape of the mesh reads before its annotations do. Built as a
    /// camera-facing ribbon rather than a wide GL line: LLRender clamps line
    /// width to the driver's aliased range, which is a single pixel on most
    /// core profiles, so a wide line is not a thing that can be asked for.
    /// Needs setEdgeWidth() first; without it an edge is an ordinary line.
    void edge(const LLVector3& a, const LLVector3& b, const LLColor4& color);

    /// Width in pixels, and where it is measured from. The ribbon is widened
    /// with distance so that it stays the same width on screen.
    void setEdgeWidth(F32 pixels, const LLVector3& camera_agent, F32 fov_y, S32 viewport_height);
    /// Full strength out to `fade_from` of `centre` (agent coordinates), then
    /// falling away to nothing at `radius`. Without a radius nothing falls
    /// away, which is what the fade defaults to.
    void setFade(const LLVector3& centre, F32 radius, F32 fade_from);
    void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color);
    /// The twelve edges of an axis-aligned box.
    void box(const LLVector3& min, const LLVector3& max, const LLColor4& color);

    bool empty() const { return mTriVerts.empty() && mLineVerts.empty() && mEdgeVerts.empty(); }
    void clear();

    /// Draw what was gathered: fills, then edges, then the lines over them.
    /// `shade` darkens the colours (the x-ray pass, seen through geometry)
    /// and `alpha` scales them.
    void draw(F32 shade = 1.f, F32 alpha = 1.f) const;

    /// Bind `gUIProgram` against a white texture with alpha blending, which is
    /// what `draw()` expects. Colour is per vertex, so one bind covers the lot.
    static void bindShader();
    static void unbindShader();

private:
    /// Alpha for one vertex: the fade scales it by how far the vertex sits
    /// from the centre, so a line or a triangle crossing the band blends
    /// across itself rather than vanishing all at once.
    F32 faded(F32 alpha, const LLVector3& at) const;

    std::vector<LLVector3> mTriVerts;
    std::vector<LLColor4>  mTriColors;
    std::vector<LLVector3> mLineVerts;
    std::vector<LLColor4>  mLineColors;
    std::vector<LLVector3> mEdgeVerts;      ///< triangles when ribboned, lines when not
    std::vector<LLColor4>  mEdgeColors;
    LLVector3 mCamera;
    F32 mPixelScale = 0.f;                  ///< world units per pixel per metre of depth
    F32 mEdgeWidth = 1.f;
    LLVector3 mFadeCentre;
    F32 mFadeRadius = 0.f;                  ///< at this distance and beyond nothing draws
    F32 mFadeFrom  = 0.f;                   ///< full strength up to here, then the fall
};

#endif // SK_NAVOVERLAY_H
