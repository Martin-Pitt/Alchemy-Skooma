/**
 * @file skoverlay.h
 * @brief Registered drawers for in-world (3D), world-anchored (2.5D) and screen (2D) overlays.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef SK_OVERLAY_H
#define SK_OVERLAY_H

#include "llcoord.h"
#include "v3math.h"

#include <functional>
#include <string>

// Where a drawer runs. Each layer is one call site in the frame; a drawer sees the
// GL state that site has set up and must leave it as it found it.
enum class SKOverlayLayer
{
    // 3D. After the scene and the debug displays, before post-processing. Camera
    // matrices are the world's; depth buffer is the scene's. Not run for probe
    // captures or impostors.
    World,
    // 2D, drawn before the UI. Ortho pixel space, y up, UI-scaled. For content
    // anchored to a world point (2.5D) via projectToScreen: labels, brackets, arrows.
    ScreenUnderUI,
    // 2D, drawn over the UI. For cursor-attached and ghost content that must beat floaters.
    ScreenOverUI,
};

class SKOverlays
{
public:
    using draw_fn_t = std::function<void()>;
    using id_t      = U32;

    // Register a drawer; the name only serves logging and the debug listing.
    static id_t add(SKOverlayLayer layer, const std::string& name, draw_fn_t draw);
    static void remove(id_t id);
    static bool any(SKOverlayLayer layer);

    // Called from the pipeline and the UI pass. Not for drawers.
    static void draw(SKOverlayLayer layer);

    // World point to the coordinates the screen layers draw in. False when behind
    // the camera. Off-screen points are returned unclamped so a drawer can decide.
    static bool projectToScreen(const LLVector3& pos_agent, S32& x, S32& y);
};

// Owns a registration for the lifetime of the holder (member of a floater, typically).
class SKOverlayHandle
{
public:
    SKOverlayHandle() = default;
    SKOverlayHandle(SKOverlayLayer layer, const std::string& name, SKOverlays::draw_fn_t draw)
        : mId(SKOverlays::add(layer, name, std::move(draw))) {}
    ~SKOverlayHandle() { reset(); }
    SKOverlayHandle(const SKOverlayHandle&) = delete;
    SKOverlayHandle& operator=(const SKOverlayHandle&) = delete;
    SKOverlayHandle(SKOverlayHandle&& o) noexcept : mId(o.mId) { o.mId = 0; }
    SKOverlayHandle& operator=(SKOverlayHandle&& o) noexcept
    {
        if (this != &o) { reset(); mId = o.mId; o.mId = 0; }
        return *this;
    }

    void reset() { if (mId) { SKOverlays::remove(mId); mId = 0; } }
    explicit operator bool() const { return mId != 0; }

private:
    SKOverlays::id_t mId{ 0 };
};

#endif // SK_OVERLAY_H
