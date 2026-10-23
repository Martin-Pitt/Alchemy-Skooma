/**
 * @file skoutlinepolicy.h
 * @brief The outline capture's pure decisions: the cell grid of a layer, the atlas slots, render order and budget.
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

#ifndef SK_SKOUTLINEPOLICY_H
#define SK_SKOUTLINEPOLICY_H

#include "stdtypes.h"

#include <string>
#include <vector>

namespace SKOutlinePolicy
{
    // Rendered subsamples per cell along each axis; the resolve averages them.
    constexpr S32 SUPERSAMPLE = 2;

    // Which way a layer is looked at, in its root's frame (+X forward, +Y left, +Z up).
    enum class View : U8
    {
        PLAN,  // down -Z: horizontal +X, vertical +Y
        SIDE,  // from the -Y side looking +Y: horizontal +X, vertical +Z
        FRONT, // from +X looking back -X: horizontal +Y, vertical +Z
    };

    // A view's axes as indices into the root frame, and the sign that turns the depth axis into distance along the view.
    struct ViewAxes
    {
        S32 h = 0;
        S32 v = 1;
        S32 d = 2;
        F32 depth_sign = -1.f;
    };

    ViewAxes viewAxes(View view);

    // A layer's cells in its view plane (metres): corners on the cell grid with one cell of padding.
    struct Grid
    {
        F32 cell = 0.f;
        S32 cols = 0;
        S32 rows = 0;
        F32 x0 = 0.f;
        F32 y0 = 0.f;
        F32 x1 = 0.f;
        F32 y1 = 0.f;
        bool coarsened = false; // the slot could not hold the cells the ceiling asked for

        bool valid() const { return cols > 0 && rows > 0 && cell > 0.f; }
    };

    struct GridLimits
    {
        S32 target_cells = 160;  // across the longer side
        F32 min_cell = 0.002f;   // metres
        F32 max_cell = 0.25f;    // metres; a long vehicle gets more cells, up to the slot
        S32 max_cells = 254;     // what one atlas slot holds across, padding included
    };

    // span/target cells on the longer side, held to [min_cell, max_cell] and then to what the slot holds.
    Grid gridFor(F32 min_x, F32 min_y, F32 max_x, F32 max_y, const GridLimits& limits);

    // A square atlas cut into equal square slots, one per layer.
    class SlotAtlas
    {
    public:
        SlotAtlas(S32 atlas_size, S32 slot_size);

        S32 capacity() const { return mPerRow * mPerRow; }
        S32 used() const { return mUsed; }
        S32 slotSize() const { return mSlotSize; }
        S32 atlasSize() const { return mAtlasSize; }

        // The lowest free slot, or -1 when full.
        S32 acquire();
        void release(S32 slot);
        void clear();
        bool inUse(S32 slot) const;

        // Lower-left texel of a slot.
        void origin(S32 slot, S32& x, S32& y) const;

    private:
        S32 mAtlasSize;
        S32 mSlotSize;
        S32 mPerRow;
        S32 mUsed = 0;
        std::vector<bool> mTaken;
    };

    // Why a capture wants a render now.
    enum class Reason : U8
    {
        NONE,
        FIRST,   // never rendered
        CHANGED, // its parts, their poses, visibility or LOD changed
        MOVING,  // moving, and its refresh interval has passed
        CONTEXT, // the atlas was lost with the GL context
    };

    // What a capture's own state asks for. Changes and first sight always ask; motion asks once per interval.
    Reason reasonFor(bool rendered, bool lost, bool changed, bool moving, F64 since_render, F64 moving_interval);

    struct Candidate
    {
        S32 index = 0;     // the caller's own handle
        Reason reason = Reason::NONE;
        F32 distance = 0.f;
        F64 waiting = 0.0; // seconds since it first asked
    };

    // First sight and lost captures first, then changed, then moving; nearest first within a reason, then the
    // longest waiting. Candidates with Reason::NONE are dropped.
    void order(std::vector<Candidate>& candidates);

    // CPU time spent on renders this frame. One render always fits; after that the running estimate must fit too.
    class FrameBudget
    {
    public:
        void begin(F64 budget_ms, S32 max_renders);
        bool fits() const;
        void spend(F64 ms);
        S32 renders() const { return mRenders; }
        F64 spent() const { return mSpent; }
        F64 estimate() const { return mEstimate; }

    private:
        F64 mBudget = 0.0;
        S32 mMaxRenders = 0;
        S32 mRenders = 0;
        F64 mSpent = 0.0;
        F64 mEstimate = 0.05; // ms; the brief's 0.03-0.1 ms per render until measured
    };

    // Every part loaded at its top LOD and nothing changed for settle_seconds.
    bool settled(bool all_top_lod, F64 since_change, F64 settle_seconds);

    // An axis-aligned box in metres.
    struct Box
    {
        F32 min[3] = { 0.f, 0.f, 0.f };
        F32 max[3] = { 0.f, 0.f, 0.f };
    };

    // A crew member's worn linkset, measured in the vehicle root's frame.
    struct CrewPiece
    {
        Box box;                       // the attachment's bounds
        Box root;                      // the vehicle root linkset's bounds
        F32 wearer[3] = { 0.f, 0.f, 0.f };
        bool hud = false;
        bool avatar_center = false;    // worn on the avatar centre point
        bool temp = false;             // a temporary attachment
        bool name_affinity = false;    // its name shares the vehicle's vendor tag or product word
    };

    struct CrewLimits
    {
        F32 personal_reach = 2.f;      // metres from the wearer that personal attachments stay within
        F32 root_share = 0.5f;         // plan span against the root's that makes a piece vehicle-sized
    };

    enum class CrewVerdict : U8
    {
        NOT_LAYER, // a HUD, or nothing makes it the vehicle's
        PERSONAL,  // stays within reach of its wearer
        LAYER,
    };

    // A worn piece is part of the vehicle when it reaches past its wearer and is vehicle-sized, sits on the avatar
    // centre, or carries a supporting sign (the vehicle's name, a temporary attachment).
    CrewVerdict crewVerdict(const CrewPiece& piece, const CrewLimits& limits = CrewLimits());

    // How far a box reaches from a point: its furthest corner.
    F32 reach(const Box& box, const F32 point[3]);

    // A near-zero-thickness plane with only alpha-blended or masked faces, such as a pilot's outline aid.
    bool isThinPlane(F32 sx, F32 sy, F32 sz, bool alpha_faces, F32 max_thickness = 0.02f, F32 min_ratio = 50.f);

    // A layer covering so few cells it cannot be told from noise.
    bool isSpeck(S32 covered_cells, S32 max_cells = 2);

    // A plan-view rectangle in the vehicle root's frame.
    struct Rect
    {
        F32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    };

    // The share of a's area inside b.
    F32 insideFraction(const Rect& a, const Rect& b);

    // A layer that is the smaller and lies at least `inside` within another contributes nothing.
    bool mergedInto(const Rect& a, S32 covered_a, const Rect& b, S32 covered_b, F32 inside = 0.9f);

    // Two object names share a vendor tag ("[Chaos]") or a distinctive product word ("Hydra").
    bool nameAffinity(const std::string& a, const std::string& b);
}

#endif // SK_SKOUTLINEPOLICY_H
