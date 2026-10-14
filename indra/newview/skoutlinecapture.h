/**
 * @file skoutlinecapture.h
 * @brief Coverage glyphs of vehicles, rendered on the GPU into an atlas and refreshed as they change.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#ifndef SK_SKOUTLINECAPTURE_H
#define SK_SKOUTLINECAPTURE_H

#include "llsingleton.h"
#include "lluuid.h"
#include "skoutlinepolicy.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// One drawable layer of a glyph: a coverage image in the atlas and where it sits in its own frame.
struct SKOutlineLayer
{
    // What the consumer poses the layer by, live: the vehicle root, or (later) a crew attachment's root.
    enum class PoseSource : U8
    {
        ROOT,
        CREW_ATTACHMENT,
    };

    PoseSource pose_source = PoseSource::ROOT;
    LLUUID pose_id;        // the object whose live transform poses the layer
    U32 texture = 0;       // the atlas; R8 coverage, swizzled so every channel reads it
    F32 u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f; // the layer's rect in the atlas
    // The rect's extents in metres in the pose source's frame, plan view: x along its +X, y along its +Y.
    F32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    F32 cell = 0.f;        // metres per atlas texel
    bool coarsened = false; // the layer was too long for its slot at the resolution ceiling
};

struct SKOutlineGlyph
{
    LLUUID root;
    std::vector<SKOutlineLayer> layers; // the base layer (root linkset) first
    bool settled = false;  // every part at its top LOD and no change for SKOutlineCaptureSettleSeconds
    U32 version = 0;       // bumps on every render, so a consumer can tell a new image
};

// Renders a plan-view coverage glyph of each requested vehicle on the GPU, at first sight and again when it
// changes or moves, under a per-frame budget. No readback: consumers draw straight from the atlas.
//
// v1 renders the base layer only (the root linkset). Crew attachment layers come next: they are found from
// the avatars seated on the root and its children, each rendered in its own attachment frame as a further
// layer with PoseSource::CREW_ATTACHMENT (see gatherLayers). A signed distance field of each layer would be
// built by the resolve pass, after the coverage average.
class SKOutlineCapture : public LLSingleton<SKOutlineCapture>
{
    LLSINGLETON(SKOutlineCapture);
    ~SKOutlineCapture() override;

public:
    // The glyph of a vehicle root, asking for it if it is not known yet. Null until its first render.
    // The pointer is valid until the next frame.
    static const SKOutlineGlyph* glyphFor(const LLUUID& root);

    // Ask for a capture without reading it; pinned requests never expire.
    void request(const LLUUID& root, bool pinned = false);
    void forget(const LLUUID& root);
    void forgetAll();

    // Runs the queue; called once per frame after the world render, before the UI, never on snapshots.
    static void onFrame();

    // The atlas goes with the GL context; every capture renders again once it is back.
    static void cleanupGL();

    // For the debug view.
    struct Stats
    {
        LLUUID root;
        std::string name;
        S32 parts = 0;
        S32 layers = 0;
        U32 renders = 0;
        U32 draws = 0;          // draw calls of the last render
        F64 last_render = 0.0;  // seconds ago
        F64 cpu_ms = 0.0;       // CPU submit of the last render
        F64 gpu_ms = -1.0;      // GPU time of the last render, when the debug view measures it
        F32 distance = 0.f;
        S32 cols = 0, rows = 0;
        F32 cell = 0.f;
        bool settled = false;
        bool pinned = false;
        bool waiting = false;   // parts not loaded or rebuilding
        std::string last_reason;
    };
    void forEachCapture(const std::function<void(const Stats&)>& fn) const;
    U32 atlasTexture() const;
    S32 atlasSize() const { return mAtlas.atlasSize(); }
    S32 slotsUsed() const { return mAtlas.used(); }
    S32 slotsTotal() const { return mAtlas.capacity(); }
    F64 lastFrameMs() const { return mLastFrameMs; }
    S32 lastFrameRenders() const { return mLastFrameRenders; }

    // Writes the atlas to a PNG (a debug readback).
    bool dumpAtlas(const std::string& path);

    struct Entry;

private:
    void frame();
    void releaseGL();
    bool ensureGL();
    bool render(Entry& entry);

    std::unordered_map<LLUUID, std::unique_ptr<Entry>> mEntries;
    SKOutlinePolicy::SlotAtlas mAtlas;
    SKOutlinePolicy::FrameBudget mBudget;
    F64 mLastFrameMs = 0.0;
    S32 mLastFrameRenders = 0;
    bool mGLFailed = false;
};

#endif // SK_SKOUTLINECAPTURE_H
