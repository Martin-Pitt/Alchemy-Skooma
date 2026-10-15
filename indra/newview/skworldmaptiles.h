/**
 * @file skworldmaptiles.h
 * @brief The 3D world map's tiles: the map server's images by level, fetched nearest first under a
 *        cap, and the per-region surface lookup a cached impostor can later answer instead.
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

#pragma once

#include "llpointer.h"
#include "llviewertexture.h"
#include "skworldmapmath.h"

#include <map>
#include <vector>

/// What covers one region of the ground instead of its map tile, such as a cached impostor of it.
/// Nothing provides one yet; the ground asks before it falls back to the map tile.
class SKWorldMapSurfaceSource
{
public:
    virtual ~SKWorldMapSurfaceSource() = default;

    /// The texture to draw over the region with this handle, or null to keep the map tile.
    virtual LLViewerTexture* surfaceFor(U64 region_handle) = 0;
};

/// The map server's object tiles for the 3D map. LLWorldMipmap serves the 2D map one level at a time
/// and loads every tile it is asked for at once; this keeps every level live together, starts at most
/// MAX_IN_FLIGHT fetches, the nearest first, and lets a tile still loading borrow a loaded ancestor.
/// The URLs and the fetch itself are LLWorldMipmap's.
class SKWorldMapTiles
{
public:
    static constexpr S32 MAX_IN_FLIGHT = 24;
    static constexpr S32 MAX_KEPT = 1536;           ///< tiles kept after they leave the view, least recently used dropped
    static constexpr F64 FETCH_TIMEOUT = 30.0;      ///< seconds before a fetch stops counting against the cap

    struct Draw
    {
        LLViewerTexture* mTexture = nullptr;
        F32 mUV[4] = { 0.f, 0.f, 1.f, 1.f };        ///< u0, v0, u1, v1; v grows north
    };

    /// The texture to draw for a selected tile: the tile itself once it has arrived, else its nearest
    /// loaded ancestor, else nothing. Records the tile as wanted for this frame.
    bool resolve(const skworldmap::SelectedTile& tile, F64 now, Draw& out);

    /// Starts this frame's fetches, nearest wanted tile first, and drops the oldest unused tiles.
    void update(F64 now);

    /// Lowers the priority of the tiles this map raised, once the map closes.
    void dropPriorities();

    void clear();

    S32 inFlight() const { return mInFlight; }
    size_t size() const { return mTiles.size(); }

private:
    struct Entry
    {
        LLPointer<LLViewerFetchedTexture> mImage;
        F64 mRequested = 0.0;
        F64 mLastUsed = 0.0;
        bool mMissing = false;      ///< open water or no region; kept for the session, as the texture list keeps the URL's missing texture
        bool mBoosted = false;      ///< raised by this map, so only this map lowers it
        S32 mPriorBoost = 0;        ///< the level before this map raised it, e.g. the 2D map's, put back on lowering

        void raise();
        void lower();
    };

    bool isReady(const Entry& e) const;
    Entry* find(const skworldmap::TileKey& key);

    std::map<skworldmap::TileKey, Entry> mTiles;
    std::vector<skworldmap::SelectedTile> mWanted;
    S32 mInFlight = 0;
};
