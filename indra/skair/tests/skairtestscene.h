/**
 * @file skairtestscene.h
 * @brief Records, land and tile inputs the air tests build their scenes from, and the synthetic fixture scenes.
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

#ifndef SK_AIRTESTSCENE_H
#define SK_AIRTESTSCENE_H

#include "../skairraster.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// Each test file that includes this keeps its own copy, in its own anonymous namespace.
namespace
{
    namespace airscene
    {
        using namespace skworld;

        /// A box record in mesh-local metres (origin at zero), axis aligned and solid.
        inline RecordPtr box(F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1)
        {
            auto r = std::make_shared<Record>();
            r->mShape = Shape::BOX;
            r->mCenter.set((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
            r->mHalf.set((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2);
            r->mAxes[0].set(1, 0, 0);
            r->mAxes[1].set(0, 1, 0);
            r->mAxes[2].set(0, 0, 1);
            r->mSolid = true;
            r->mMin.set(x0, y0, z0);
            r->mMax.set(x1, y1, z1);
            return r;
        }

        /// A record of triangles, as the census hands one over; a hull is solid.
        inline RecordPtr soup(const std::vector<LLVector3>& tris, Provenance provenance)
        {
            auto r = std::make_shared<Record>();
            r->mShape = Shape::TRIANGLES;
            r->mProvenance = provenance;
            r->mSolid = provenance == Provenance::HULL || provenance == Provenance::BASE_HULL;
            r->mTriangles = std::make_shared<const std::vector<LLVector3>>(tris);
            r->mMin.set(1e9, 1e9, 1e9);
            r->mMax.set(-1e9, -1e9, -1e9);
            for (const LLVector3& p : tris)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    r->mMin.mdV[k] = std::min(r->mMin.mdV[k], static_cast<F64>(p.mV[k]));
                    r->mMax.mdV[k] = std::max(r->mMax.mdV[k], static_cast<F64>(p.mV[k]));
                }
            }
            return r;
        }

        inline RecordPtr hull(const std::vector<LLVector3>& tris)
        {
            return soup(tris, Provenance::HULL);
        }

        /// A flat quad at z, wound up.
        inline std::vector<LLVector3> slab(F32 x0, F32 y0, F32 x1, F32 y1, F32 z)
        {
            return {
                LLVector3(x0, y0, z), LLVector3(x1, y0, z), LLVector3(x1, y1, z),
                LLVector3(x0, y0, z), LLVector3(x1, y1, z), LLVector3(x0, y1, z),
            };
        }

        /// A quad a, b, c, d into a soup.
        inline void quad(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
        {
            out.insert(out.end(), { a, b, c, a, c, d });
        }

        /// Flat land at z, reaching past the tile's padding on every side.
        inline TerrainWindow flat(F32 z, S32 n = 37, F32 ox = -2.f, F32 oy = -2.f)
        {
            TerrainWindow t;
            t.mOrigin.set(ox, oy, 0);
            t.mSpacing = 1.f;
            t.mWidth = n;
            t.mHeight = n;
            t.mZ.assign(static_cast<size_t>(n) * n, z);
            t.mWater = -10.f;
            return t;
        }

        /// A tile of a mesh at the origin, at a coarse cell to keep tests quick.
        inline TileInput tile(S32 tx, S32 ty, F32 cell = 0.25f)
        {
            TileInput in;
            in.mTileX = tx;
            in.mTileY = ty;
            in.mZMin = -1.f;
            in.mZMax = 12.f;
            in.mCell = cell;
            in.mAirPad = airPadCells(cell);
            return in;
        }

        /// The tile as the viewer builds it with its air: faces, run-off and face offsets.
        inline TileInput airTile(S32 tx, S32 ty, F32 cell = 0.25f)
        {
            TileInput in = tile(tx, ty, cell);
            in.mAvatar = { 2.f, 0.3f, 0.75f, 74.f };
            in.mAir = true;
            in.mWallFaces = true;
            in.mRunoff = true;
            return in;
        }

        /// The gap of the air over a floor at an SL point within tol of z, or null.
        inline const AirGap* gapAt(const TileAirResult& r, F32 x, F32 y, F32 z, F32 tol = 0.3f)
        {
            if (!r.mAir)
            {
                return nullptr;
            }
            const TileAir& air = *r.mAir;
            const S32 ix = static_cast<S32>(std::floor((x - air.mOrigin.mV[VX]) / air.mCell));
            const S32 iy = static_cast<S32>(std::floor((y - air.mOrigin.mV[VY]) / air.mCell));
            if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
            {
                return nullptr;
            }
            const S32 leaf = findLeafByKey(air, ix, iy);
            if (leaf < 0)
            {
                return nullptr;
            }
            AirColumn column;
            airColumn(air, leaf, column);
            for (U32 k = 0; k < column.mCount; ++k)
            {
                if (std::fabs((air.mOrigin.mV[VZ] + column.floorAt(air, k, ix, iy) * air.mUnit) - z) <= tol)
                {
                    return &air.mGaps[column.mGap[k]];
                }
            }
            return nullptr;
        }

        /// The air has a floor at the point, of any kind.
        inline bool airFloorAt(const TileAirResult& r, F32 x, F32 y, F32 z, F32 tol = 0.3f)
        {
            return gapAt(r, x, y, z, tol) != nullptr;
        }

        /// The air's slope stands on the floor at the point.
        inline bool topFloorAt(const TileAirResult& r, F32 x, F32 y, F32 z, F32 tol = 0.3f)
        {
            const AirGap* g = gapAt(r, x, y, z, tol);
            return g && (g->mFlags & AirGap::TOP);
        }

        /// AVATAR stands on the floor at the point.
        inline bool avatarFloorAt(const TileAirResult& r, F32 x, F32 y, F32 z, F32 tol = 0.3f)
        {
            const AirGap* g = gapAt(r, x, y, z, tol);
            return g && (g->mFlags & AirGap::AVATAR_FLOOR);
        }

        /// One synthetic scene of the fixture set.
        struct Scene
        {
            std::string mName;
            TileInput mInput;
        };

        /// The in-repo fixture scenes: each a tile at the viewer's cell, built with its air.
        inline std::vector<Scene> fixtureScenes()
        {
            std::vector<Scene> out;
            const auto add = [&](const char* name, const TileInput& in) { out.push_back({ name, in }); };

            {   // a straight stair of overlapping prims, 0.25 m risers on 0.4 m treads, onto a landing
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                for (S32 k = 0; k < 12; ++k)
                {
                    in.mRecords.push_back(box(12.f, 8.f + k * 0.4f, 0.f, 15.f, 20.f, (k + 1) * 0.25f));
                }
                add("stairs", in);
            }
            {   // a closed box: a roof, and no floor inside
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(box(8.f, 8.f, -0.5f, 24.f, 24.f, 3.f));
                add("closed box", in);
            }
            {   // a hollow box: a room with walls, a floor and a roof, and a doorway
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 0.3f));        // floor
                in.mRecords.push_back(box(8.f, 8.f, 3.3f, 24.f, 24.f, 3.6f));       // roof
                in.mRecords.push_back(box(8.f, 8.f, 0.3f, 8.3f, 24.f, 3.3f));       // walls
                in.mRecords.push_back(box(23.7f, 8.f, 0.3f, 24.f, 24.f, 3.3f));
                in.mRecords.push_back(box(8.3f, 23.7f, 0.3f, 23.7f, 24.f, 3.3f));
                in.mRecords.push_back(box(8.3f, 8.f, 0.3f, 15.f, 8.3f, 3.3f));      // the south wall either side of a doorway
                in.mRecords.push_back(box(16.2f, 8.f, 0.3f, 23.7f, 8.3f, 3.3f));
                add("hollow box", in);
            }
            {   // a tunnel through one closed surface
                std::vector<LLVector3> tris;
                const auto q = [&](F32 ax, F32 ay, F32 az, F32 bx, F32 by, F32 bz, F32 cx, F32 cy, F32 cz, F32 dx, F32 dy, F32 dz) {
                    quad(tris, LLVector3(ax, ay, az), LLVector3(bx, by, bz), LLVector3(cx, cy, cz), LLVector3(dx, dy, dz));
                };
                q(8, 8, 4, 24, 8, 4, 24, 24, 4, 8, 24, 4);
                q(8, 8, -0.5f, 8, 24, -0.5f, 24, 24, -0.5f, 24, 8, -0.5f);
                q(8, 8, -0.5f, 8, 24, -0.5f, 8, 24, 4, 8, 8, 4);
                q(24, 8, -0.5f, 24, 8, 4, 24, 24, 4, 24, 24, -0.5f);
                for (F32 y : { 8.f, 24.f })
                {
                    q(8, y, -0.5f, 14, y, -0.5f, 14, y, 4, 8, y, 4);
                    q(18, y, -0.5f, 24, y, -0.5f, 24, y, 4, 18, y, 4);
                    q(14, y, -0.5f, 18, y, -0.5f, 18, y, 0.5f, 14, y, 0.5f);
                    q(14, y, 3, 18, y, 3, 18, y, 4, 14, y, 4);
                }
                q(14, 8, 0.5f, 18, 8, 0.5f, 18, 24, 0.5f, 14, 24, 0.5f);
                q(14, 8, 3, 14, 24, 3, 18, 24, 3, 18, 8, 3);
                q(14, 8, 0.5f, 14, 24, 0.5f, 14, 24, 3, 14, 8, 3);
                q(18, 8, 0.5f, 18, 8, 3, 18, 24, 3, 18, 24, 0.5f);
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(hull(tris));
                add("tunnel", in);
            }
            {   // a framed door leaf AVATAR leaves out
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(box(4.f, 4.f, 0.f, 28.f, 28.f, 1.f));
                in.mRecords.push_back(box(15.8f, 11.f, 1.f, 16.2f, 12.f, 4.f));
                in.mRecords.push_back(box(15.8f, 14.f, 1.f, 16.2f, 15.f, 4.f));
                in.mRecords.push_back(box(15.8f, 12.f, 3.5f, 16.2f, 14.f, 4.f));
                auto door = std::make_shared<Record>(*box(15.9f, 12.f, 1.f, 16.1f, 14.f, 3.5f));
                door->mExclude = EXCLUDE_AVATAR;
                in.mRecords.push_back(door);
                add("door leaf", in);
            }
            {   // a crevice one column wide between two floors
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(soup(slab(4.f, 4.f, 12.f, 11.8f, 2.f), Provenance::EXACT));
                in.mRecords.push_back(soup(slab(4.f, 11.95f, 12.f, 20.f, 2.f), Provenance::EXACT));
                add("crevice", in);
            }
            {   // a prim buried in the land, and one sunk into it
                TileInput in = airTile(0, 0, CELL_M);
                in.mZMin = -4.f;
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(box(14.f, 14.f, -4.f, 18.f, 18.f, -2.5f));
                in.mRecords.push_back(box(20.f, 14.f, -3.5f, 24.f, 18.f, 2.5f));
                add("buried prim", in);
            }
            {   // an exclusion volume cut out of a platform
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mRecords.push_back(box(4.f, 4.f, 0.f, 28.f, 28.f, 1.f));
                auto cut = std::make_shared<Record>(*box(10.f, 10.f, 0.5f, 14.f, 14.f, 3.f));
                cut->mNavRole = NavRole::EXCLUSION_VOLUME;
                in.mCuts.push_back(cut);
                add("exclusion cut", in);
            }
            {   // land sloping down under the water
                TileInput in = airTile(0, 0, CELL_M);
                in.mZMin = -6.f;
                TerrainWindow t = flat(0.f);
                for (S32 j = 0; j < t.mHeight; ++j)
                {
                    for (S32 i = 0; i < t.mWidth; ++i)
                    {
                        t.mZ[static_cast<size_t>(j) * t.mWidth + i] = 2.f - 0.25f * i;
                    }
                }
                t.mWater = 0.f;
                in.mTerrain = t;
                add("under-water slope", in);
            }
            {   // a megaprim whose top meets the raster's height clamp
                TileInput in = airTile(0, 0, CELL_M);
                in.mTerrain = flat(0.f);
                in.mZMax = 1022.f;
                in.mRecords.push_back(box(6.f, 6.f, 0.f, 26.f, 26.f, 1020.f));
                add("megaprim", in);
            }
            return out;
        }
    }
}

#endif // SK_AIRTESTSCENE_H
