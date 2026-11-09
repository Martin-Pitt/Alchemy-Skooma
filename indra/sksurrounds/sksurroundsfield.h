/**
 * @file sksurroundsfield.h
 * @brief The distant regions' layout: a grid-aligned restricted quadtree of far nodes around
 * the live regions and their ring of stand-ins, and the edge levels their seams share.
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

#ifndef SK_SURROUNDSFIELD_H
#define SK_SURROUNDSFIELD_H

#include "stdtypes.h"

#include <span>
#include <vector>

namespace SKSurrounds
{
    constexpr S32 REGION_METRES       = 256;
    constexpr S32 TOP_LEVEL           = 5; // 32 regions, 8 km: map level 6
    constexpr S32 LIVE_PATCH_METRES   = 16;
    constexpr S32 FAR_PATCHES_PER_EDGE = 8; // a far node's patch edge is its own edge / 8
    constexpr F64 DEFAULT_REACH       = 8192.0;
    constexpr F64 DEFAULT_SPLIT       = 2.0; // split while distance to the box < this x node size
    constexpr F32 MAX_EDGE_LEVEL      = 64.f;
    constexpr S8  LEVEL_SPECIAL       = -1;  // live and ring cells, as the balance counts them
    constexpr S8  NO_NEIGHBOUR        = 127;

    enum class Band : U8
    {
        Live,
        Ring,
        Far,
    };

    enum Side : U8
    {
        SIDE_WEST,
        SIDE_EAST,
        SIDE_SOUTH,
        SIDE_NORTH,
        SIDE_COUNT
    };

    struct Cell
    {
        S32 mX = 0;
        S32 mY = 0;
    };

    // A node of 2^level x 2^level regions at a grid multiple of 2^level (level -1: one live or
    // ring cell). mNeighbour holds, per side, the finest level across it (NO_NEIGHBOUR outside).
    struct Node
    {
        S32  mX = 0;
        S32  mY = 0;
        S8   mLevel = 0;
        Band mBand  = Band::Far;
        S8   mNeighbour[SIDE_COUNT] = { NO_NEIGHBOUR, NO_NEIGHBOUR, NO_NEIGHBOUR, NO_NEIGHBOUR };

        S32 sizeCells() const { return 1 << (mLevel < 0 ? 0 : mLevel); }
        S32 sizeMetres() const { return REGION_METRES * sizeCells(); }
        S32 patchMetres() const { return mBand == Band::Far ? sizeMetres() / FAR_PATCHES_PER_EDGE : LIVE_PATCH_METRES; }
        bool fractionalOdd() const { return mBand != Band::Far; }
    };

    struct SelectParams
    {
        F64 mReach      = DEFAULT_REACH;
        F64 mSplit      = DEFAULT_SPLIT;
        S32 mNearCells  = 1; // SKRenderSurroundsNearCells: the ring's width around the live cells
    };

    // Selects the nodes for a camera in global metres; keeps its buffers between calls, so a
    // frame's selection allocates nothing once they have grown. Nodes come live, ring, then far,
    // each sorted by (level, x, y).
    class Selection
    {
    public:
        void select(const F64 camera[3], std::span<const Cell> live, const SelectParams& params = {});

        const std::vector<Node>& nodes() const { return mNodes; }

    private:
        void visit(S32 level, S32 x, S32 y);
        bool containsSpecial(S32 level, S32 x, S32 y) const;
        bool inReach(S32 level, S32 x, S32 y) const;
        S8& levelAt(S32 x, S32 y) { return mLevels[(y - mGridY) * mGridW + (x - mGridX)]; }
        S8  levelAt(S32 x, S32 y) const;
        void paint();
        S8  sideLevel(const Node& node, Side side) const;

        F64                 mCamera[3] = {};
        SelectParams        mParams;
        S32                 mGridX = 0, mGridY = 0, mGridW = 0, mGridH = 0;
        std::vector<S8>     mLevels;  // per grid cell: leaf level, LEVEL_SPECIAL, or NO_NEIGHBOUR
        std::vector<U8>     mSpecial; // per grid cell: 1 live, 2 ring
        std::vector<Node>   mFar;
        std::vector<Node>   mNext;
        std::vector<Node>   mNodes;
    };

    // Rye's edgeLevel in float32, operation for operation: the edge's length times the density
    // over the distance from its midpoint to the origin (at least 1), clamped to [1, 64].
    F32 edgeLevel(const F32 a[3], const F32 b[3], const F32 origin[3], F32 density);

    // terrain_tess_origin in a tile's frame (global metres of its south-west corner). Live
    // regions subtract in float from the float agent-frame camera, as LLDrawPoolTerrain does;
    // stand-ins and far nodes cast the double offset.
    void liveOrigin(const F64 camera[3], const F64 agentOrigin[2], const F64 frame[2], F32 out[3]);
    void farOrigin(const F64 camera[3], const F64 frame[2], F32 out[3]);

    // A size-change edge: the parent edge's level, worked once by the coarse side, gives the fine
    // half m points (odd where the fine side is fractional_odd) and the coarse side 2m.
    S32 fineHalfLevel(F32 parentLevel, bool fineFractionalOdd);
    inline S32 coarseEdgeLevel(S32 fineHalf) { return 2 * fineHalf; }
    S32 ceilOdd(F64 level);

    // The parameter on the parent edge of the fine side's point k of m (first or second half),
    // as the coarse side holds it, so both sides evaluate the same vertex bit for bit.
    F32 parentParameter(S32 k, S32 m, bool firstHalf);

    // Map tiles: a node of level j reads map level j + 1 (a live or ring cell, level 1); the tile
    // starts at the node, and UV is local / width, south-up.
    struct MapTile
    {
        S32 mLevel   = 1;
        S32 mX       = 0;
        S32 mY       = 0;
        F32 mMetres  = REGION_METRES;
    };
    MapTile mapTile(const Node& node);
    void    mapUV(const MapTile& tile, F32 localX, F32 localY, F32 uv[2]);
}

#endif
